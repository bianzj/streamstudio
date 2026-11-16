#include "mesh_renderer.h"
#include "animation/scene/mesh_scene.h"
#include "animation/bridge/histream_mesh_adapter.h"
#include "animation/thermal/background_field.h"
#include "base/appsetting.h"
#include "base/vkcheck.h"
#include <gdal_priv.h>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace animation {
namespace {
using Json=nlohmann::json;
struct Pixel {glm::vec4 hit,normalRadiance;glm::uvec4 color;};
struct Push {glm::ivec4 image;glm::vec4 origin,forward,right,up,thermal,light,reserved;};
static_assert(sizeof(Pixel)==48 && sizeof(Push)==128);
struct Context {
    AppSetting app;
    bool started=false;
    ~Context(){if(started)app.destroy();}
};
struct Resources {
    VkDevice device{};VkBuffer output{};VkDeviceMemory memory{};void* mapped=nullptr;
    VkDescriptorSetLayout setLayout{};VkDescriptorPool descriptorPool{};VkDescriptorSet set{};
    VkPipelineLayout layout{};VkShaderModule shader{};VkPipeline pipeline{};
    VkCommandPool commandPool{};VkCommandBuffer cmd{};VkFence fence{};
    ~Resources(){
        if(!device)return;vkDeviceWaitIdle(device);
        if(mapped)vkUnmapMemory(device,memory);
        if(fence)vkDestroyFence(device,fence,nullptr);
        if(commandPool)vkDestroyCommandPool(device,commandPool,nullptr);
        if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
        if(shader)vkDestroyShaderModule(device,shader,nullptr);
        if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
        if(descriptorPool)vkDestroyDescriptorPool(device,descriptorPool,nullptr);
        if(setLayout)vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
        if(output)vkDestroyBuffer(device,output,nullptr);
        if(memory)vkFreeMemory(device,memory,nullptr);
    }
};
void writeJson(const std::filesystem::path& path,const Json& value){
    std::ofstream f(path);f<<value.dump(2)<<'\n';if(!f)throw std::runtime_error("Cannot write mesh run receipt");
}
void binary(const std::filesystem::path& path,const void* data,size_t bytes){
    std::ofstream f(path,std::ios::binary);f.write(static_cast<const char*>(data),bytes);f.flush();
    if(!f)throw std::runtime_error("Cannot write mesh output: "+pathText(path));
}
void png(const std::filesystem::path& path,int width,int height,const std::vector<uint32_t>& pixels){
    auto close=[](GDALDataset* image){if(image)GDALClose(image);};
    auto* mem=GetGDALDriverManager()->GetDriverByName("MEM");auto* driver=GetGDALDriverManager()->GetDriverByName("PNG");
    if(!mem||!driver)throw std::runtime_error("PNG/MEM drivers unavailable");
    std::unique_ptr<GDALDataset,decltype(close)> image(mem->Create("",width,height,4,GDT_Byte,nullptr),close);
    if(!image||image->RasterIO(GF_Write,0,0,width,height,const_cast<uint32_t*>(pixels.data()),width,height,GDT_Byte,4,nullptr,4,width*4,1,nullptr)!=CE_None)
        throw std::runtime_error("Cannot prepare mesh PNG");
    const char* options[]={"ZLEVEL=2",nullptr};
    std::unique_ptr<GDALDataset,decltype(close)> encoded(driver->CreateCopy(pathText(path).c_str(),image.get(),FALSE,const_cast<char**>(options),nullptr,nullptr),close);
    if(!encoded)throw std::runtime_error("Cannot write mesh PNG");
}
uint32_t rgba(float r,float g,float b){
    auto byte=[](float value){return uint32_t(std::round(std::clamp(value,0.f,1.f)*255));};
    return byte(r)|(byte(g)<<8)|(byte(b)<<16)|0xff000000u;
}
float radiance(float lambda,float t){return 1.191043934e8f/(std::pow(lambda,5.f)*std::expm1(14388.29104f/(lambda*t)));}
}

int runMeshScene(const std::filesystem::path& input,const std::filesystem::path& rawOutput,
                 const std::filesystem::path& shaderPath,const std::filesystem::path& executable){
    auto output=std::filesystem::absolute(rawOutput).lexically_normal();
    while(output.filename().empty()&&output.has_relative_path())output=output.parent_path();
    if(std::filesystem::exists(output))throw std::runtime_error("Mesh output already exists; use a new run directory");
    MeshScene scene(input);
    std::ifstream spv(shaderPath,std::ios::binary|std::ios::ate);
    if(!spv)throw std::runtime_error("Missing mesh SPIR-V");
    const auto length=spv.tellg();if(length<=0||length%4)throw std::runtime_error("Invalid mesh SPIR-V");
    std::vector<uint32_t> code(size_t(length)/4);spv.seekg(0);spv.read(reinterpret_cast<char*>(code.data()),length);
    if(!spv)throw std::runtime_error("Cannot read mesh SPIR-V");
    Context context;context.started=true;context.app.init(scene.gpuIndex,ComputeBackend::Gpu);
    auto& app=context.app;Resources gpu;gpu.device=app.m_context.m_device;const auto device=gpu.device;const auto queue=app.m_queues.front();
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(app.m_context.m_physicalDevice,&props);
    if(props.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)throw std::runtime_error("Mesh rendering requires a hardware GPU");
    const VkDeviceSize bytes=VkDeviceSize(scene.width)*scene.height*sizeof(Pixel);
    if(bytes>props.limits.maxStorageBufferRange)throw std::runtime_error("Mesh image exceeds GPU storage-buffer range");
    HistreamMeshAdapter geometry(app,scene);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=bytes;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(device,&bi,nullptr,&gpu.output),"mesh output buffer");
    VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,gpu.output,&req);
    VkPhysicalDeviceMemoryProperties memoryProps{};vkGetPhysicalDeviceMemoryProperties(app.m_context.m_physicalDevice,&memoryProps);
    uint32_t type=UINT32_MAX;constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(uint32_t i=0;i<memoryProps.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i))&&(memoryProps.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
    if(type==UINT32_MAX)throw std::runtime_error("No coherent mesh readback memory");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
    vkCheck(vkAllocateMemory(device,&ai,nullptr,&gpu.memory),"mesh output allocation");
    vkCheck(vkBindBufferMemory(device,gpu.output,gpu.memory,0),"mesh output bind");
    vkCheck(vkMapMemory(device,gpu.memory,0,bytes,0,&gpu.mapped),"mesh output map");
    VkDescriptorSetLayoutBinding bindings[4]{};
    for(uint32_t i=0;i<4;i++)bindings[i]={i,i==0?VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=4;li.pBindings=bindings;
    vkCheck(vkCreateDescriptorSetLayout(device,&li,nullptr,&gpu.setLayout),"mesh descriptor layout");
    VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=2;pi.pPoolSizes=sizes;
    vkCheck(vkCreateDescriptorPool(device,&pi,nullptr,&gpu.descriptorPool),"mesh descriptor pool");
    VkDescriptorSetAllocateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};si.descriptorPool=gpu.descriptorPool;si.descriptorSetCount=1;si.pSetLayouts=&gpu.setLayout;
    vkCheck(vkAllocateDescriptorSets(device,&si,&gpu.set),"mesh descriptor set");
    const auto tlas=geometry.tlas();VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};as.accelerationStructureCount=1;as.pAccelerationStructures=&tlas;
    VkDescriptorBufferInfo info[3]{geometry.triangles(),geometry.materials(),{gpu.output,0,bytes}};
    for(uint32_t i=0;i<4;i++){
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=gpu.set;write.dstBinding=i;write.descriptorCount=1;write.descriptorType=bindings[i].descriptorType;
        if(i==0)write.pNext=&as;else write.pBufferInfo=&info[i-1];vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=1;pli.pSetLayouts=&gpu.setLayout;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&range;
    vkCheck(vkCreatePipelineLayout(device,&pli,nullptr,&gpu.layout),"mesh pipeline layout");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=code.size()*4;sm.pCode=code.data();
    vkCheck(vkCreateShaderModule(device,&sm,nullptr,&gpu.shader),"mesh shader module");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=gpu.layout;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=gpu.shader;ci.stage.pName="main";
    vkCheck(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,nullptr,&gpu.pipeline),"mesh compute pipeline");
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpi.queueFamilyIndex=queue.familyIndex;cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCheck(vkCreateCommandPool(device,&cpi,nullptr,&gpu.commandPool),"mesh command pool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=gpu.commandPool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
    vkCheck(vkAllocateCommandBuffers(device,&cai,&gpu.cmd),"mesh command buffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkCheck(vkCreateFence(device,&fi,nullptr,&gpu.fence),"mesh fence");
    std::filesystem::create_directories(output.parent_path());
    if(!std::filesystem::create_directory(output))throw std::runtime_error("Mesh output already exists");
    std::filesystem::create_directory(output/"provenance");
    std::filesystem::copy_file(input,output/"provenance/scene.json");
    if(sha256File(output/"provenance/scene.json")!=scene.receipt["sourceSha256"])
        throw std::runtime_error("Scene input changed during rendering setup");
    std::filesystem::copy_file(executable,output/"provenance/histream.exe");
    binary(output/"provenance/static.comp.spv",code.data(),code.size()*4);
    auto replay=scene.input;
    for(size_t i=0;i<scene.assets.size();i++){
        auto snapshot=output/"provenance"/("asset_"+std::to_string(i)+".obj");std::filesystem::copy_file(scene.assets[i],snapshot);
        if(sha256File(snapshot)!=scene.receipt["assets"][i]["sha256"])throw std::runtime_error("Asset changed during rendering setup");
        scene.receipt["assets"][i]["snapshot"]=pathText(snapshot.filename());
        replay["assets"][i]["path"]=pathText(snapshot.filename());
    }
    if(scene.background)replay["backgroundField"]=scene.background->snapshot(output/"provenance");
    writeJson(output/"provenance/replay.json",replay);
    auto& manifest=scene.receipt;manifest["status"]="running";manifest["device"]=props.deviceName;
    manifest["shaderSha256"]=sha256File(output/"provenance/static.comp.spv");manifest["executableSha256"]=sha256File(output/"provenance/histream.exe");
    manifest["thermal"]={{"wavelengthMicrometers",scene.wavelength},{"displayMinK",scene.minK},{"displayMaxK",scene.maxK},{"skyK",scene.skyK},
        {"radianceUnits","W m-2 sr-1 um-1"},{"display","fixed linear radiance white-hot"},{"reflection","greybody reflection from uniform supplied sky radiance"}};
    manifest["hitLayout"]={"distance_m","material_index","normal_x","normal_y","normal_z","temperature_K","instance_index","triangle_index"};
    manifest["hitConvention"]="centre ray; -1 distance/material/instance/triangle for sky; little-endian float32";
    manifest["radianceProducts"]={{"radiance.f32","antialiasing sample mean used for the displayed pixel"},
        {"centerRadiance.f32","pixel-centre ray; corresponds to hit temperature/material, without AA mixing"}};
    manifest["renderLimitations"]="Opaque geometry including glass; no textured surfaces, traced transmission, calibrated sensor, atmosphere, participating media or transient thermal/structural calculation";
    writeJson(output/"run-manifest.json",manifest);
    GDALAllRegister();
    const auto start=std::chrono::steady_clock::now();
    std::vector<Pixel> pixels(size_t(scene.width)*scene.height);
    std::vector<uint32_t> visible(pixels.size()),thermal(pixels.size()),normal(pixels.size()),material(pixels.size());
    std::vector<std::array<float,8>> hits(pixels.size());std::vector<float> radiances(pixels.size()),temperatures(pixels.size()),centerRadiances(pixels.size());
    for(const auto& camera:scene.cameras){
        Push push{};push.image={scene.width,scene.height,scene.samples,0};
        const auto forward=glm::normalize(camera.target-camera.position),right=glm::normalize(glm::cross(forward,camera.up)),up=glm::normalize(glm::cross(right,forward));
        push.origin={camera.position,std::tan(camera.fovY*glm::pi<float>()/360.f)};push.forward={forward,float(scene.width)/scene.height};push.right={right,0};push.up={up,0};
        push.thermal={scene.wavelength,scene.minK,scene.maxK,scene.skyK};push.light={scene.sunDirection,0};
        for(int row=0;row<scene.height;row+=32){
            push.image.w=row;vkCheck(vkResetCommandBuffer(gpu.cmd,0),"mesh reset command");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkCheck(vkBeginCommandBuffer(gpu.cmd,&begin),"mesh begin command");
            VkMemoryBarrier previous{VK_STRUCTURE_TYPE_MEMORY_BARRIER};previous.srcAccessMask=VK_ACCESS_HOST_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;previous.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(gpu.cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&previous,0,nullptr,0,nullptr);
            vkCmdBindPipeline(gpu.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,gpu.pipeline);vkCmdBindDescriptorSets(gpu.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,gpu.layout,0,1,&gpu.set,0,nullptr);
            vkCmdPushConstants(gpu.cmd,gpu.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);vkCmdDispatch(gpu.cmd,(scene.width+7)/8,4,1);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(gpu.cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            vkCheck(vkEndCommandBuffer(gpu.cmd),"mesh end command");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&gpu.cmd;
            vkCheck(vkQueueSubmit(queue.queue,1,&submit,gpu.fence),"mesh submit");vkCheck(vkWaitForFences(device,1,&gpu.fence,VK_TRUE,UINT64_MAX),"mesh GPU wait");
            vkCheck(vkResetFences(device,1,&gpu.fence),"mesh reset fence");
        }
        // Copy coherent device memory once, then analyse cached CPU values.
        std::copy_n(static_cast<const Pixel*>(gpu.mapped),pixels.size(),pixels.data());
        const auto low=radiance(scene.wavelength,scene.minK),high=radiance(scene.wavelength,scene.maxK);
        size_t foreground=0;
        for(size_t i=0;i<pixels.size();i++){
            const auto& p=pixels[i];for(int k=0;k<4;k++)if(!std::isfinite(p.hit[k])||!std::isfinite(p.normalRadiance[k]))throw std::runtime_error("Nonfinite GPU mesh result");
            visible[i]=p.color.x;radiances[i]=p.normalRadiance.w;temperatures[i]=p.hit.w;
            std::memcpy(&centerRadiances[i],&p.color.z,sizeof(float));
            if(!std::isfinite(centerRadiances[i])||centerRadiances[i]<=0)throw std::runtime_error("Invalid pixel-centre radiance");
            float intensity=(radiances[i]-low)/(high-low);thermal[i]=rgba(intensity,intensity,intensity);
            const auto n=glm::vec3(p.normalRadiance);normal[i]=p.hit.x<0?rgba(0,0,0):rgba(n.x*.5f+.5f,n.y*.5f+.5f,n.z*.5f+.5f);
            uint32_t id=p.hit.x<0?0:uint32_t(p.hit.y)+1;
            material[i]=id?rgba(float((id*71)%255)/255,float((id*137)%255)/255,float((id*193)%255)/255):rgba(0,0,0);
            hits[i]={p.hit.x,p.hit.y,n.x,n.y,n.z,p.hit.w,p.hit.z,p.hit.x<0?-1.f:float(p.color.y)};
            if(p.hit.x>=0)foreground++;
        }
        for(auto item:{std::make_pair("visible",&visible),std::make_pair("thermal",&thermal),std::make_pair("normal",&normal),std::make_pair("material",&material)})png(output/(camera.id+"_"+item.first+".png"),scene.width,scene.height,*item.second);
        binary(output/(camera.id+"_hits.f32"),hits.data(),hits.size()*sizeof(hits[0]));binary(output/(camera.id+"_radiance.f32"),radiances.data(),radiances.size()*sizeof(float));
        binary(output/(camera.id+"_temperatureK.f32"),temperatures.data(),temperatures.size()*sizeof(float));
        binary(output/(camera.id+"_centerRadiance.f32"),centerRadiances.data(),centerRadiances.size()*sizeof(float));
        manifest["views"][camera.id]={{"foregroundPixels",foreground},{"pixelCount",pixels.size()}};
        std::cout<<"MESH_VIEW\t"<<camera.id<<"\ttriangles="<<scene.triangles.size()<<"\thitPixels="<<foreground<<std::endl;
    }
    manifest["wallSeconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();manifest["status"]="complete";
    writeJson(output/"run-manifest.json",manifest);return 0;
}
}
