#include "histream_mesh_adapter.h"
#include "base/appsetting.h"
#include "base/vkcheck.h"
#include <nvvk/resourceallocator_vk.hpp>
#include <nvvk/raytraceKHR_vk.hpp>
#include <nvvk/commands_vk.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace animation {
struct HistreamMeshAdapter::Impl {
    VkDevice device{};
    nvvk::ResourceAllocatorDma allocator;
    nvvk::RaytracingBuilderKHR builder;
    nvvk::Buffer positions{},indices{},triangles{},materials{};
    size_t triangleBytes=0,materialBytes=0;
    bool initialized=false;
    ~Impl(){
        if(!initialized)return;
        vkDeviceWaitIdle(device);builder.destroy();
        for(auto* buffer:{&positions,&indices,&triangles,&materials})allocator.destroy(*buffer);
        allocator.deinit();
    }
};
HistreamMeshAdapter::HistreamMeshAdapter(AppSetting& app,const MeshScene& scene):m(std::make_unique<Impl>()){
    m->device=app.m_context.m_device;
    const auto queue=app.m_queues.front();
    m->allocator.init(m->device,app.m_context.m_physicalDevice);m->initialized=true;
    m->builder.setup(m->device,&m->allocator,queue.familyIndex);
    struct Position {float x,y,z;};static_assert(sizeof(Position)==12);
    std::vector<Position> vertices;std::vector<uint32_t> indices;
    vertices.reserve(scene.triangles.size()*3);indices.reserve(scene.triangles.size()*3);
    for(const auto& t:scene.triangles)for(auto v:{t.a,t.b,t.c}){indices.push_back(uint32_t(vertices.size()));vertices.push_back({v.x,v.y,v.z});}
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(app.m_context.m_physicalDevice,&properties);
    m->triangleBytes=scene.triangles.size()*sizeof(Triangle);m->materialBytes=scene.materials.values.size()*sizeof(Material);
    if(m->triangleBytes>properties.limits.maxStorageBufferRange||m->materialBytes>properties.limits.maxStorageBufferRange)
        throw std::runtime_error("Mesh storage exceeds GPU storage-buffer range");
    nvvk::CommandPool commands(m->device,queue.familyIndex,VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,queue.queue);
    const auto cmd=commands.createCommandBuffer();
    constexpr auto geometryUsage=VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    m->positions=m->allocator.createBuffer(cmd,vertices,geometryUsage);
    m->indices=m->allocator.createBuffer(cmd,indices,geometryUsage);
    m->triangles=m->allocator.createBuffer(cmd,scene.triangles,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    m->materials=m->allocator.createBuffer(cmd,scene.materials.values,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    commands.submitAndWait(cmd);vkCheck(vkQueueWaitIdle(queue.queue),"mesh upload completion");
    m->allocator.finalizeAndReleaseStaging();
    auto address=[&](VkBuffer buffer){VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};info.buffer=buffer;return vkGetBufferDeviceAddress(m->device,&info);};
    // Legacy AccelStruct::objectToVkGeometryKHR assumes a 44-byte VertexAttribute.
    // This adapter supplies a verified 12-byte position stream and explicit stride.
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;geometry.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& triangles=geometry.geometry.triangles;triangles.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;triangles.vertexStride=sizeof(Position);
    triangles.vertexData.deviceAddress=address(m->positions.buffer);triangles.maxVertex=uint32_t(vertices.size()-1);
    triangles.indexType=VK_INDEX_TYPE_UINT32;triangles.indexData.deviceAddress=address(m->indices.buffer);
    VkAccelerationStructureBuildRangeInfoKHR range{};range.primitiveCount=uint32_t(scene.triangles.size());
    nvvk::RaytracingBuilderKHR::BlasInput blas;blas.asGeometry.push_back(geometry);blas.asBuildOffsetInfo.push_back(range);
    m->builder.buildBlas({blas},VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
    VkAccelerationStructureInstanceKHR instance{};
    instance.transform.matrix[0][0]=instance.transform.matrix[1][1]=instance.transform.matrix[2][2]=1;
    instance.mask=0xff;instance.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference=m->builder.getBlasDeviceAddress(0);
    m->builder.buildTlas(std::vector<VkAccelerationStructureInstanceKHR>{instance});
}
HistreamMeshAdapter::~HistreamMeshAdapter()=default;
VkAccelerationStructureKHR HistreamMeshAdapter::tlas()const{return m->builder.getAccelerationStructure();}
VkDescriptorBufferInfo HistreamMeshAdapter::triangles()const{return {m->triangles.buffer,0,m->triangleBytes};}
VkDescriptorBufferInfo HistreamMeshAdapter::materials()const{return {m->materials.buffer,0,m->materialBytes};}

struct DynamicMeshAdapter::Impl {
    struct Position {float x,y,z;};
    VkDevice device{};VkQueue queue{};
    nvvk::ResourceAllocatorDma allocator;bool initialized=false;
    size_t count=0,positionBytes=0,triangleBytes=0;
    nvvk::Buffer positions{},indices{},triangles{},staging{},scratch{},instances{};
    nvvk::AccelKHR blas{},tlas{};void* mapped=nullptr;
    VkCommandPool pool{};VkCommandBuffer cmd{};VkFence fence{};
    VkAccelerationStructureGeometryKHR blasGeometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    VkAccelerationStructureGeometryKHR tlasGeometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    VkDeviceAddress scratchAddress=0;
    ~Impl(){
        if(!initialized)return;
        // Every submission was already waited on in build() and the fence reset afterwards; waiting on
        // the (unsignaled, never resubmitted) fence here would block forever.
        if(fence)vkDestroyFence(device,fence,nullptr);
        if(pool)vkDestroyCommandPool(device,pool,nullptr);
        if(mapped)allocator.unmap(staging);
        allocator.destroy(tlas);allocator.destroy(blas);
        for(auto* buffer:{&positions,&indices,&triangles,&staging,&scratch,&instances})allocator.destroy(*buffer);
        allocator.deinit();
    }
    VkDeviceAddress address(VkBuffer buffer)const{VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};info.buffer=buffer;return vkGetBufferDeviceAddress(device,&info);}
    VkAccelerationStructureBuildGeometryInfoKHR info(bool top)const{
        VkAccelerationStructureBuildGeometryInfoKHR b{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        b.type=top?VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR:VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        // Same flags/mode as HistreamMeshAdapter (nvvk buildBlas/buildTlas defaults): full build, no refit.
        b.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;b.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        b.geometryCount=1;b.pGeometries=top?&tlasGeometry:&blasGeometry;return b;
    }
    // Copies the staged positions/triangles, rebuilds BLAS then TLAS in place, waits on one fence.
    void build(){
        vkCheck(vkResetCommandBuffer(cmd,0),"dynamic mesh reset");VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;vkCheck(vkBeginCommandBuffer(cmd,&begin),"dynamic mesh begin");
        // Previous sample's shader reads of the triangle buffer / TLAS finished: the caller's dispatches wait on their fences.
        VkBufferCopy p{0,0,positionBytes},t{positionBytes,0,triangleBytes};
        vkCmdCopyBuffer(cmd,staging.buffer,positions.buffer,1,&p);vkCmdCopyBuffer(cmd,staging.buffer,triangles.buffer,1,&t);
        VkMemoryBarrier copied{VK_STRUCTURE_TYPE_MEMORY_BARRIER};copied.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;copied.dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR|VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&copied,0,nullptr,0,nullptr);
        auto bottom=info(false);bottom.dstAccelerationStructure=blas.accel;bottom.scratchData.deviceAddress=scratchAddress;
        VkAccelerationStructureBuildRangeInfoKHR blasRange{uint32_t(count),0,0,0};const auto* blasRanges=&blasRange;
        vkCmdBuildAccelerationStructuresKHR(cmd,1,&bottom,&blasRanges);
        VkMemoryBarrier built{VK_STRUCTURE_TYPE_MEMORY_BARRIER};built.srcAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;built.dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,0,1,&built,0,nullptr,0,nullptr);
        auto top=info(true);top.dstAccelerationStructure=tlas.accel;top.scratchData.deviceAddress=scratchAddress;
        VkAccelerationStructureBuildRangeInfoKHR tlasRange{1,0,0,0};const auto* tlasRanges=&tlasRange;
        vkCmdBuildAccelerationStructuresKHR(cmd,1,&top,&tlasRanges);
        VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;ready.dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
        vkCheck(vkEndCommandBuffer(cmd),"dynamic mesh end");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
        vkCheck(vkQueueSubmit(queue,1,&submit,fence),"dynamic mesh submit");vkCheck(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"dynamic mesh wait");vkCheck(vkResetFences(device,1,&fence),"dynamic mesh fence reset");
    }
    void stage(const MeshScene& scene){
        auto* out=static_cast<Position*>(mapped);size_t k=0;
        for(const auto& tri:scene.triangles)for(auto v:{tri.a,tri.b,tri.c})out[k++]={v.x,v.y,v.z};
        std::memcpy(static_cast<char*>(mapped)+positionBytes,scene.triangles.data(),triangleBytes);
    }
};
DynamicMeshAdapter::DynamicMeshAdapter(AppSetting& app,const MeshScene& scene):m(std::make_unique<Impl>()){
    if(scene.triangles.empty())throw std::runtime_error("Dynamic mesh needs at least one triangle");
    m->device=app.m_context.m_device;const auto queue=app.m_queues.front();m->queue=queue.queue;
    m->allocator.init(m->device,app.m_context.m_physicalDevice);m->initialized=true;
    m->count=scene.triangles.size();m->positionBytes=m->count*3*sizeof(Impl::Position);m->triangleBytes=m->count*sizeof(Triangle);
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(app.m_context.m_physicalDevice,&properties);
    if(m->triangleBytes>properties.limits.maxStorageBufferRange)throw std::runtime_error("Mesh storage exceeds GPU storage-buffer range");
    constexpr auto geometryUsage=VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    m->positions=m->allocator.createBuffer(m->positionBytes,geometryUsage);
    m->triangles=m->allocator.createBuffer(m->triangleBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    m->staging=m->allocator.createBuffer(m->positionBytes+m->triangleBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    m->mapped=m->allocator.map(m->staging);if(!m->mapped)throw std::runtime_error("Cannot map dynamic mesh staging");
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=queue.familyIndex;cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;vkCheck(vkCreateCommandPool(m->device,&cp,nullptr,&m->pool),"dynamic mesh pool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=m->pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;vkCheck(vkAllocateCommandBuffers(m->device,&ca,&m->cmd),"dynamic mesh command");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkCheck(vkCreateFence(m->device,&fi,nullptr,&m->fence),"dynamic mesh fence");
    // Index stream 0..3n-1 and the single identity instance never change for a fixed triangle count.
    {
        std::vector<uint32_t> indices(m->count*3);for(uint32_t i=0;i<indices.size();i++)indices[i]=i;
        nvvk::CommandPool commands(m->device,queue.familyIndex,VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,queue.queue);const auto upload=commands.createCommandBuffer();
        m->indices=m->allocator.createBuffer(upload,indices,geometryUsage);
        auto& tri=m->blasGeometry.geometry.triangles;m->blasGeometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;m->blasGeometry.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
        tri.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;tri.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;tri.vertexStride=sizeof(Impl::Position);
        tri.vertexData.deviceAddress=m->address(m->positions.buffer);tri.maxVertex=uint32_t(m->count*3-1);tri.indexType=VK_INDEX_TYPE_UINT32;tri.indexData.deviceAddress=m->address(m->indices.buffer);
        auto bottom=m->info(false);const uint32_t primitives=uint32_t(m->count);VkAccelerationStructureBuildSizesInfoKHR bs{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(m->device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&bottom,&primitives,&bs);
        VkAccelerationStructureCreateInfoKHR bc{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};bc.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;bc.size=bs.accelerationStructureSize;m->blas=m->allocator.createAcceleration(bc);
        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};ai.accelerationStructure=m->blas.accel;
        VkAccelerationStructureInstanceKHR instance{};instance.transform.matrix[0][0]=instance.transform.matrix[1][1]=instance.transform.matrix[2][2]=1;
        instance.mask=0xff;instance.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;instance.accelerationStructureReference=vkGetAccelerationStructureDeviceAddressKHR(m->device,&ai);
        m->instances=m->allocator.createBuffer(upload,std::vector<VkAccelerationStructureInstanceKHR>{instance},VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
        commands.submitAndWait(upload);m->allocator.finalizeAndReleaseStaging();
        m->tlasGeometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;m->tlasGeometry.geometry.instances.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;m->tlasGeometry.geometry.instances.data.deviceAddress=m->address(m->instances.buffer);
        auto top=m->info(true);const uint32_t one=1;VkAccelerationStructureBuildSizesInfoKHR ts{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(m->device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&top,&one,&ts);
        VkAccelerationStructureCreateInfoKHR tc{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};tc.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;tc.size=ts.accelerationStructureSize;m->tlas=m->allocator.createAcceleration(tc);
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p2.pNext=&asProps;vkGetPhysicalDeviceProperties2(app.m_context.m_physicalDevice,&p2);
        const VkDeviceSize align=std::max<VkDeviceSize>(asProps.minAccelerationStructureScratchOffsetAlignment,1);
        m->scratch=m->allocator.createBuffer(std::max(bs.buildScratchSize,ts.buildScratchSize)+align,VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        m->scratchAddress=(m->address(m->scratch.buffer)+align-1)/align*align;
    }
    m->stage(scene);m->build();
}
DynamicMeshAdapter::~DynamicMeshAdapter()=default;
size_t DynamicMeshAdapter::triangleCount()const{return m->count;}
bool DynamicMeshAdapter::update(const MeshScene& scene){if(scene.triangles.size()!=m->count)return false;m->stage(scene);m->build();return true;}
VkAccelerationStructureKHR DynamicMeshAdapter::tlas()const{return m->tlas.accel;}
VkDescriptorBufferInfo DynamicMeshAdapter::triangles()const{return {m->triangles.buffer,0,m->triangleBytes};}

struct InstancedMeshAdapter::Impl {
    struct Position {float x,y,z;};
    struct Layout {uint64_t key;uint32_t first,count;bool operator==(const Layout& o)const{return key==o.key&&first==o.first&&count==o.count;}};
    VkDevice device{};VkQueue queue{};
    nvvk::ResourceAllocatorDma allocator;bool initialized=false;
    size_t count=0,triangleBytes=0;std::vector<Layout> layout;std::vector<glm::dmat4> referenceInverse;
    nvvk::Buffer positions{},triangles{},staging{},scratch{},instances{};std::vector<nvvk::AccelKHR> blas;nvvk::AccelKHR tlas{};
    void* mapped=nullptr;void* instanceMapped=nullptr;
    VkCommandPool pool{};VkCommandBuffer cmd{};VkFence fence{};
    VkAccelerationStructureGeometryKHR tlasGeometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};VkDeviceAddress tlasScratch=0;
    ~Impl(){
        if(!initialized)return;
        // Every submission was waited on in build(); the fence is unsignaled afterwards (see DynamicMeshAdapter).
        if(fence)vkDestroyFence(device,fence,nullptr);
        if(pool)vkDestroyCommandPool(device,pool,nullptr);
        if(mapped)allocator.unmap(staging);
        if(instanceMapped)allocator.unmap(instances);
        allocator.destroy(tlas);for(auto& b:blas)allocator.destroy(b);
        for(auto* buffer:{&positions,&triangles,&staging,&scratch,&instances})allocator.destroy(*buffer);
        allocator.deinit();
    }
    VkDeviceAddress address(VkBuffer buffer)const{VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};info.buffer=buffer;return vkGetBufferDeviceAddress(device,&info);}
    void begin(){vkCheck(vkResetCommandBuffer(cmd,0),"instanced mesh reset");VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;vkCheck(vkBeginCommandBuffer(cmd,&b),"instanced mesh begin");}
    void submit(){
        vkCheck(vkEndCommandBuffer(cmd),"instanced mesh end");VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};s.commandBufferCount=1;s.pCommandBuffers=&cmd;
        vkCheck(vkQueueSubmit(queue,1,&s,fence),"instanced mesh submit");vkCheck(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"instanced mesh wait");vkCheck(vkResetFences(device,1,&fence),"instanced mesh fence reset");
    }
    static std::vector<Layout> describe(const std::vector<MeshPart>& parts){std::vector<Layout> l;l.reserve(parts.size());for(const auto& p:parts)l.push_back({p.key,p.first,p.count});return l;}
    void stage(const MeshScene& scene,const std::vector<MeshPart>& parts){
        std::memcpy(mapped,scene.triangles.data(),triangleBytes);
        auto* out=static_cast<VkAccelerationStructureInstanceKHR*>(instanceMapped);
        for(size_t i=0;i<parts.size();i++){
            const glm::dmat4 m=glm::dmat4(parts[i].transform)*referenceInverse[i];
            for(int r=0;r<3;r++)for(int c=0;c<4;c++)out[i].transform.matrix[r][c]=float(m[c][r]);
        }
    }
    // Copies the triangle records and rebuilds the TLAS over the persistent per-part BLAS.
    void build(){
        begin();
        VkBufferCopy t{0,0,triangleBytes};vkCmdCopyBuffer(cmd,staging.buffer,triangles.buffer,1,&t);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR|VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&host,0,nullptr,0,nullptr);
        VkAccelerationStructureBuildGeometryInfoKHR top{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};top.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        top.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;top.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;top.geometryCount=1;top.pGeometries=&tlasGeometry;
        top.dstAccelerationStructure=tlas.accel;top.scratchData.deviceAddress=tlasScratch;
        VkAccelerationStructureBuildRangeInfoKHR range{uint32_t(layout.size()),0,0,0};const auto* ranges=&range;
        vkCmdBuildAccelerationStructuresKHR(cmd,1,&top,&ranges);
        VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;ready.dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
        submit();
    }
};
InstancedMeshAdapter::InstancedMeshAdapter(AppSetting& app,const MeshScene& scene,const std::vector<MeshPart>& parts):m(std::make_unique<Impl>()){
    if(scene.triangles.empty()||parts.empty())throw std::runtime_error("Instanced mesh needs at least one part");
    {uint64_t next=0;for(const auto& p:parts){if(p.first!=next||p.count==0||p.first>0xFFFFFFu)throw std::runtime_error("Instanced mesh parts must cover the triangles contiguously (first < 2^24)");next+=p.count;}
        if(next!=scene.triangles.size())throw std::runtime_error("Instanced mesh parts do not cover all triangles");}
    m->device=app.m_context.m_device;const auto queue=app.m_queues.front();m->queue=queue.queue;
    m->allocator.init(m->device,app.m_context.m_physicalDevice);m->initialized=true;
    m->count=scene.triangles.size();m->triangleBytes=m->count*sizeof(Triangle);m->layout=Impl::describe(parts);
    for(const auto& p:parts)m->referenceInverse.push_back(glm::inverse(glm::dmat4(p.transform)));
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(app.m_context.m_physicalDevice,&properties);
    if(m->triangleBytes>properties.limits.maxStorageBufferRange)throw std::runtime_error("Mesh storage exceeds GPU storage-buffer range");
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p2.pNext=&asProps;vkGetPhysicalDeviceProperties2(app.m_context.m_physicalDevice,&p2);
    const VkDeviceSize align=std::max<VkDeviceSize>(asProps.minAccelerationStructureScratchOffsetAlignment,1);
    constexpr auto geometryUsage=VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    m->triangles=m->allocator.createBuffer(m->triangleBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    m->staging=m->allocator.createBuffer(m->triangleBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    m->mapped=m->allocator.map(m->staging);if(!m->mapped)throw std::runtime_error("Cannot map instanced mesh staging");
    m->instances=m->allocator.createBuffer(parts.size()*sizeof(VkAccelerationStructureInstanceKHR),geometryUsage,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    m->instanceMapped=m->allocator.map(m->instances);if(!m->instanceMapped)throw std::runtime_error("Cannot map instanced mesh instances");
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=queue.familyIndex;cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;vkCheck(vkCreateCommandPool(m->device,&cp,nullptr,&m->pool),"instanced mesh pool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=m->pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;vkCheck(vkAllocateCommandBuffers(m->device,&ca,&m->cmd),"instanced mesh command");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkCheck(vkCreateFence(m->device,&fi,nullptr,&m->fence),"instanced mesh fence");
    // Reference vertices (construction pose) for the per-part BLAS, non-indexed triangle lists.
    std::vector<Impl::Position> reference;reference.reserve(m->count*3);
    for(const auto& tri:scene.triangles)for(auto v:{tri.a,tri.b,tri.c})reference.push_back({v.x,v.y,v.z});
    {nvvk::CommandPool commands(m->device,queue.familyIndex,VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,queue.queue);const auto upload=commands.createCommandBuffer();
        m->positions=m->allocator.createBuffer(upload,reference,geometryUsage);commands.submitAndWait(upload);m->allocator.finalizeAndReleaseStaging();}
    const VkDeviceAddress vertexBase=m->address(m->positions.buffer);
    std::vector<VkAccelerationStructureGeometryKHR> geometries(parts.size(),VkAccelerationStructureGeometryKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR});
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos(parts.size(),VkAccelerationStructureBuildGeometryInfoKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR});
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(parts.size());std::vector<VkDeviceSize> scratchOffsets(parts.size());VkDeviceSize scratchTotal=0;
    for(size_t i=0;i<parts.size();i++){
        auto& g=geometries[i];g.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;g.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;auto& tri=g.geometry.triangles;
        tri.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;tri.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;tri.vertexStride=sizeof(Impl::Position);
        tri.vertexData.deviceAddress=vertexBase+VkDeviceAddress(parts[i].first)*3*sizeof(Impl::Position);tri.maxVertex=parts[i].count*3-1;tri.indexType=VK_INDEX_TYPE_NONE_KHR;
        auto& b=infos[i];b.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;b.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;b.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;b.geometryCount=1;b.pGeometries=&g;
        VkAccelerationStructureBuildSizesInfoKHR bs{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};const uint32_t primitives=parts[i].count;
        vkGetAccelerationStructureBuildSizesKHR(m->device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&b,&primitives,&bs);
        VkAccelerationStructureCreateInfoKHR bc{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};bc.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;bc.size=bs.accelerationStructureSize;
        m->blas.push_back(m->allocator.createAcceleration(bc));b.dstAccelerationStructure=m->blas.back().accel;
        scratchOffsets[i]=scratchTotal;scratchTotal+=(bs.buildScratchSize+align-1)/align*align;ranges[i]={parts[i].count,0,0,0};
    }
    // TLAS sizes and the shared scratch (per-part BLAS regions first, then the TLAS region).
    m->tlasGeometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;m->tlasGeometry.geometry.instances.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;m->tlasGeometry.geometry.instances.data.deviceAddress=m->address(m->instances.buffer);
    VkAccelerationStructureBuildGeometryInfoKHR top{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};top.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;top.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;top.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;top.geometryCount=1;top.pGeometries=&m->tlasGeometry;
    const uint32_t instanceCount=uint32_t(parts.size());VkAccelerationStructureBuildSizesInfoKHR ts{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(m->device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&top,&instanceCount,&ts);
    VkAccelerationStructureCreateInfoKHR tc{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};tc.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;tc.size=ts.accelerationStructureSize;m->tlas=m->allocator.createAcceleration(tc);
    const VkDeviceSize tlasOffset=scratchTotal;scratchTotal+=(ts.buildScratchSize+align-1)/align*align;
    m->scratch=m->allocator.createBuffer(scratchTotal+align,VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const VkDeviceAddress scratchBase=(m->address(m->scratch.buffer)+align-1)/align*align;m->tlasScratch=scratchBase+tlasOffset;
    // All part BLAS in one command (disjoint scratch regions), built once.
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePointers(parts.size());
    for(size_t i=0;i<parts.size();i++){infos[i].scratchData.deviceAddress=scratchBase+scratchOffsets[i];rangePointers[i]=&ranges[i];}
    m->begin();vkCmdBuildAccelerationStructuresKHR(m->cmd,uint32_t(infos.size()),infos.data(),rangePointers.data());
    VkMemoryBarrier built{VK_STRUCTURE_TYPE_MEMORY_BARRIER};built.srcAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;built.dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(m->cmd,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,0,1,&built,0,nullptr,0,nullptr);m->submit();
    auto* out=static_cast<VkAccelerationStructureInstanceKHR*>(m->instanceMapped);
    for(size_t i=0;i<parts.size();i++){
        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};ai.accelerationStructure=m->blas[i].accel;
        out[i]=VkAccelerationStructureInstanceKHR{};out[i].instanceCustomIndex=parts[i].first;out[i].mask=0xff;out[i].flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        out[i].accelerationStructureReference=vkGetAccelerationStructureDeviceAddressKHR(m->device,&ai);
    }
    m->stage(scene,parts);m->build();
}
InstancedMeshAdapter::~InstancedMeshAdapter()=default;
bool InstancedMeshAdapter::update(const MeshScene& scene,const std::vector<MeshPart>& parts){
    if(scene.triangles.size()!=m->count||!(Impl::describe(parts)==m->layout))return false;m->stage(scene,parts);m->build();return true;
}
VkAccelerationStructureKHR InstancedMeshAdapter::tlas()const{return m->tlas.accel;}
VkDescriptorBufferInfo InstancedMeshAdapter::triangles()const{return {m->triangles.buffer,0,m->triangleBytes};}
size_t InstancedMeshAdapter::partCount()const{return m->layout.size();}
}