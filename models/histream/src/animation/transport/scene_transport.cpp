#include "scene_transport.h"
#include "animation/render/compute_pass.h"
#include "animation/scene/fire_sources.h"
#include "base/appsetting.h"
#include "base/vkcheck.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
constexpr int NX=64,NY=56,NZ=56,COUNT=NX*NY*NZ;
constexpr double DT=1.0/60;
struct Cell {std::array<float,4> flow,matter;};
static_assert(sizeof(Cell)==32);
struct Params {
    std::array<int32_t,4> grid;
    std::array<float,4> domain,dynamics,settings;
    std::array<int32_t,4> style;
    std::array<float,4> sourcePoint,flightStart,flightSettings;
};
static_assert(sizeof(Params)==128);
}

struct SceneTransport::Snapshot {
    std::vector<Cell> cells;Params params{};int step=0;
};
struct SceneTransport::Impl {
    VkDevice device=VK_NULL_HANDLE;
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    VkQueue queue=VK_NULL_HANDLE;
    VkBuffer cells[2]{},staging{};
    VkDeviceMemory memory[3]{};
    VkDescriptorSetLayout setLayout{};
    VkDescriptorPool descriptorPool{};
    VkDescriptorSet sets[2]{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkShaderModule module{};
    VkCommandPool commandPool{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    void* mapped=nullptr;
    int active=0,step=0;
    Params params{};
    uint32_t pushBytes=80;bool mesh=false;
    std::unique_ptr<animation::HostBuffer> emptyFire;
    static constexpr VkDeviceSize bytes=VkDeviceSize(COUNT)*sizeof(Cell);
    ~Impl() {
        if(!device) return;
        vkDeviceWaitIdle(device);
        if(mapped) vkUnmapMemory(device,memory[2]);
        if(fence) vkDestroyFence(device,fence,nullptr);
        if(commandPool) vkDestroyCommandPool(device,commandPool,nullptr);
        if(pipeline) vkDestroyPipeline(device,pipeline,nullptr);
        if(module) vkDestroyShaderModule(device,module,nullptr);
        if(layout) vkDestroyPipelineLayout(device,layout,nullptr);
        if(descriptorPool) vkDestroyDescriptorPool(device,descriptorPool,nullptr);
        if(setLayout) vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
        for(auto b:cells) if(b) vkDestroyBuffer(device,b,nullptr);
        if(staging) vkDestroyBuffer(device,staging,nullptr);
        for(auto mem:memory) if(mem) vkFreeMemory(device,mem,nullptr);
    }
    void allocate(VkBuffer& buffer,VkDeviceMemory& mem,VkMemoryPropertyFlags flags,VkMemoryPropertyFlags preferred=0) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size=bytes;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(device,&bi,nullptr,&buffer),"transport buffer");
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer,&req);
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(physical,&props);
        uint32_t type=UINT32_MAX;
        // Prefer a type with the extra (e.g. HOST_CACHED) flags; fall back to the required flags only.
        for(auto wanted:{flags|preferred,flags})if(type==UINT32_MAX)
            for(uint32_t i=0;i<props.memoryTypeCount;i++)
                if((req.memoryTypeBits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&wanted)==wanted){type=i;break;}
        if(type==UINT32_MAX) throw std::runtime_error("No transport memory type");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        vkCheck(vkAllocateMemory(device,&ai,nullptr,&mem),"transport allocate");
        vkCheck(vkBindBufferMemory(device,buffer,mem,0),"transport bind");
    }
    void begin() {
        vkCheck(vkResetCommandBuffer(cmd,0),"transport reset");
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};info.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(vkBeginCommandBuffer(cmd,&info),"transport begin");
        // Includes reads by the renderer and transfer readback in earlier submissions.
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    }
    void finish() {
        vkCheck(vkEndCommandBuffer(cmd),"transport end");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
        vkCheck(vkQueueSubmit(queue,1,&submit,fence),"transport submit");
        vkCheck(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"transport wait");
        vkCheck(vkResetFences(device,1,&fence),"transport reset fence");
    }
    void dispatch(int stage) {
        params.grid[3]=stage;
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&sets[active],0,nullptr);
        vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,pushBytes,&params);
        vkCmdDispatch(cmd,(NX+3)/4,(NY+3)/4,(NZ+3)/4);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        active=1-active;
    }
};

SceneTransport::SceneTransport(AppSetting& app,const std::filesystem::path& shader,
        const std::filesystem::path& snapshot,const SceneTransportOptions& options):m(std::make_unique<Impl>()) {
    m->mesh=options.meshMask!=VK_NULL_HANDLE;m->pushBytes=m->mesh?128:80;const bool impactSource=m->mesh&&options.impactSource!=VK_NULL_HANDLE;
    m->device=app.m_context.m_device;m->physical=app.m_context.m_physicalDevice;m->queue=app.m_queues.front().queue;
    for(int i=0;i<2;i++)m->allocate(m->cells[i],m->memory[i],VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    // Host-cached readback memory makes the per-frame statistics copy a cached read instead of an uncached bus read.
    m->allocate(m->staging,m->memory[2],VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    vkCheck(vkMapMemory(m->device,m->memory[2],0,Impl::bytes,0,&m->mapped),"transport map");
    const uint32_t bindingCount=impactSource?5u:m->mesh?4u:2u;
    VkDescriptorSetLayoutBinding bindings[5]{};
    for(uint32_t i=0;i<bindingCount;i++)bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=bindingCount;li.pBindings=bindings;
    vkCheck(vkCreateDescriptorSetLayout(m->device,&li,nullptr,&m->setLayout),"transport descriptor layout");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,bindingCount*2};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=2;pi.poolSizeCount=1;pi.pPoolSizes=&ps;
    vkCheck(vkCreateDescriptorPool(m->device,&pi,nullptr,&m->descriptorPool),"transport descriptor pool");
    VkDescriptorSetLayout layouts[2]{m->setLayout,m->setLayout};
    VkDescriptorSetAllocateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};si.descriptorPool=m->descriptorPool;si.descriptorSetCount=2;si.pSetLayouts=layouts;
    vkCheck(vkAllocateDescriptorSets(m->device,&si,m->sets),"transport descriptor sets");
    for(int set=0;set<2;set++)for(int binding=0;binding<2;binding++) {
        VkDescriptorBufferInfo bi{m->cells[binding==0?set:1-set],0,Impl::bytes};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=m->sets[set];write.dstBinding=binding;
        write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&bi;
        vkUpdateDescriptorSets(m->device,1,&write,0,nullptr);
    }
    if(m->mesh)setObstacleMask(options.meshMask);
    if(m->mesh){
        VkBuffer fire=options.fireSources;VkDeviceSize bytes=sizeof(animation::FireBlock);
        if(!fire){m->emptyFire=std::make_unique<animation::HostBuffer>(app,16);std::memset(m->emptyFire->data(),0,16);fire=m->emptyFire->info().buffer;bytes=16;}
        VkDescriptorBufferInfo bi{fire,0,bytes};
        for(auto set:m->sets){VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=3;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&bi;vkUpdateDescriptorSets(m->device,1,&write,0,nullptr);}
    }
    if(impactSource){
        VkDescriptorBufferInfo bi{options.impactSource,0,VK_WHOLE_SIZE};
        for(auto set:m->sets){VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=4;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&bi;vkUpdateDescriptorSets(m->device,1,&write,0,nullptr);}
    }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,m->pushBytes};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=1;pli.pSetLayouts=&m->setLayout;
    pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&range;
    vkCheck(vkCreatePipelineLayout(m->device,&pli,nullptr,&m->layout),"transport pipeline layout");
    std::ifstream input(shader,std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("Missing transport shader");
    const auto length=input.tellg();
    if(length<=0||length%4!=0)throw std::runtime_error("Invalid transport shader size");
    std::vector<uint32_t> code(size_t(length)/4);input.seekg(0);input.read(reinterpret_cast<char*>(code.data()),length);
    if(!input)throw std::runtime_error("Cannot read transport shader");
    std::ofstream copy(snapshot,std::ios::binary);copy.write(reinterpret_cast<char*>(code.data()),length);copy.flush();
    if(!copy)throw std::runtime_error("Cannot write transport shader snapshot");
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smi.codeSize=code.size()*4;smi.pCode=code.data();
    vkCheck(vkCreateShaderModule(m->device,&smi,nullptr,&m->module),"transport shader module");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=m->layout;
    ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=m->module;ci.stage.pName="main";
    vkCheck(vkCreateComputePipelines(m->device,VK_NULL_HANDLE,1,&ci,nullptr,&m->pipeline),"transport pipeline");
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpi.queueFamilyIndex=app.m_queues.front().familyIndex;cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCheck(vkCreateCommandPool(m->device,&cpi,nullptr,&m->commandPool),"transport command pool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=m->commandPool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
    vkCheck(vkAllocateCommandBuffers(m->device,&cai,&m->cmd),"transport command buffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkCheck(vkCreateFence(m->device,&fi,nullptr,&m->fence),"transport fence");
    m->params={{NX,NY,NZ,0},{options.origin[0],options.origin[1],options.origin[2],1},{0,float(DT),options.impactTime,options.windX},
        {options.windY,float(options.scenario),0,float(std::ceil(double(options.impactTime)/DT-1e-6))},
        {options.detailed?1:0,0,0,0},{options.contact[0],options.contact[1],options.contact[2],options.outwardAzimuth},
        {options.flightStart[0],options.flightStart[1],options.flightStart[2],options.flightSmoke?1.f:0.f},{2.7f,8.f,100.f,.3f}};
    // Mesh-only unused style words carry the explicit trajectory endpoint,
    // retaining the128byte push interface and the unchanged80byte legacy one.
    if(m->mesh&&options.explicitFlightEnd){m->params.flightStart[3]=options.flightSmoke?2.f:0.f;std::memcpy(m->params.style.data()+1,options.flightEnd.data(),3*sizeof(float));}
    m->begin();m->dispatch(0);m->dispatch(2);
    for(int k=0;k<28;k++)m->dispatch(3);
    m->dispatch(4);m->finish();
}
SceneTransport::~SceneTransport()=default;
void SceneTransport::setObstacleMask(VkBuffer mask){
    if(!m->mesh||!mask)throw std::runtime_error("Invalid mesh transport mask");
    VkDescriptorBufferInfo info{mask,0,VkDeviceSize(COUNT)*sizeof(uint32_t)};
    for(auto set:m->sets){VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=2;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&info;vkUpdateDescriptorSets(m->device,1,&w,0,nullptr);}
}
void SceneTransport::advanceTo(double time) {
    const int target=int(std::floor(time/DT+1e-4));
    if(target<m->step)throw std::runtime_error("Transport time cannot move backwards");
    while(m->step<target) {
        m->begin();
        const int end=std::min(target,m->step+4);
        while(m->step<end) {
            m->params.dynamics[0]=float((m->step+1)*DT);
            m->params.settings[2]=float(m->step+1);
            m->dispatch(1);m->dispatch(2);
            for(int k=0;k<28;k++)m->dispatch(3);
            m->dispatch(4);
            m->step++;
        }
        m->finish();
    }
}
VkBuffer SceneTransport::buffer()const{return m->cells[m->active];}
VkDeviceSize SceneTransport::byteSize()const{return Impl::bytes;}
nlohmann::json SceneTransport::description() {
    return {{"solver","incompressible-projection"},{"grid",{NX,NY,NZ}},{"originMeters",{-32,-40,0}},
        {"cellSizeMeters",1},{"fixedStepSeconds",DT},{"pressureIterations",28},
        {"velocityLayout","staggered positive-face MAC fluxes; compatible backward divergence/forward pressure gradient"},
        {"pressureBoundary","closed ground/solid Neumann; exterior p=0 with corrected boundary face flux"},
        {"obstacleVoxelization","conservative cell bounding sphere SDF test; thin members can be thickened at 1 m resolution"},
        {"advection","semi-Lagrangian, not strictly mass conservative"},
        {"thermalDiffusivityM2PerSecond",.12},{"tracerDiffusivityM2PerSecond",.08},{"eddyViscosityM2PerSecond",.05},
        {"surfaceLayerThicknessMeters",.003},{"surfaceConductivityWPerMK",1.4},
        {"surfaceDensityKgPerM3",2300},{"surfaceHeatCapacityJPerKgK",880},
        {"surfaceConvectionWPerM2K",12},{"sources","supplied heat and normalized tracer; no impact-energy calculation"},
        {"validation","numerical fixtures only; no independent physical validation"}};
}
std::shared_ptr<const SceneTransport::Snapshot> SceneTransport::capture() {
    m->begin();
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};before.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;before.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(m->cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&before,0,nullptr,0,nullptr);
    VkBufferCopy copy{0,0,Impl::bytes};vkCmdCopyBuffer(m->cmd,buffer(),m->staging,1,&copy);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(m->cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,nullptr,0,nullptr);
    m->finish();
    // Vulkan host-visible memory can be uncached/write-combined. Scan a normal
    // CPU copy rather than repeatedly reading individual words over the bus.
    auto s=std::make_shared<Snapshot>();s->cells.resize(COUNT);s->params=m->params;s->step=m->step;
    std::memcpy(s->cells.data(),m->mapped,size_t(Impl::bytes));
    return s;
}
nlohmann::json SceneTransport::statistics(){return summarize(*capture());}
int SceneTransport::step()const{return m->step;}
std::vector<uint8_t> SceneTransport::saveState(){
    auto s=capture();std::vector<uint8_t> out(sizeof(int32_t)+size_t(Impl::bytes));const int32_t step=m->step;
    std::memcpy(out.data(),&step,sizeof step);std::memcpy(out.data()+sizeof step,s->cells.data(),size_t(Impl::bytes));return out;
}
void SceneTransport::loadState(const std::vector<uint8_t>& state){
    if(state.size()!=sizeof(int32_t)+size_t(Impl::bytes))throw std::runtime_error("Transport checkpoint size mismatch");
    int32_t step=0;std::memcpy(&step,state.data(),sizeof step);if(step<0)throw std::runtime_error("Invalid transport checkpoint step");
    std::memcpy(m->mapped,state.data()+sizeof step,size_t(Impl::bytes));
    m->begin();
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(m->cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&host,0,nullptr,0,nullptr);
    VkBufferCopy copy{0,0,Impl::bytes};vkCmdCopyBuffer(m->cmd,m->staging,buffer(),1,&copy);
    VkMemoryBarrier done{VK_STRUCTURE_TYPE_MEMORY_BARRIER};done.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;done.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(m->cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&done,0,nullptr,0,nullptr);
    m->finish();m->step=step;
    // Per-step push values are rewritten from the step counter in advanceTo(); keep them consistent for statistics.
    m->params.dynamics[0]=float(step*DT);m->params.settings[2]=float(step);
}
nlohmann::json SceneTransport::summarize(const Snapshot& snapshot) {
    const auto* cells=snapshot.cells.data();const auto& params=snapshot.params;
    double mass=0,x=0,y=0,z=0,r2=0,maxSpeed=0,minT=1e9,maxT=0,wallMax=0,minSmoke=1e9;
    double divBefore=0,divAfter=0,pressureResidual=0,heat=0;int air=0,invalid=0;
    auto velocity=[&](int ix,int iy,int iz,int axis)->double {
        const double wind=axis==0?params.dynamics[3]:axis==1?params.settings[0]:0;
        if(ix<0||ix>=NX||iy<0||iy>=NY||iz<0||iz>=NZ) {
            if(axis==2&&iz==-1)return 0;
            int p[3]{ix,iy,iz};if(p[axis]==-1) {
                p[axis]++;
                if(p[0]>=0&&p[0]<NX&&p[1]>=0&&p[1]<NY&&p[2]>=0&&p[2]<NZ) {
                    const auto& n=cells[(p[2]*NY+p[1])*NX+p[0]];
                    return n.matter[1]>.5?0:wind-n.matter[2];
                }
            }
            return wind;
        }
        const auto& c=cells[(iz*NY+iy)*NX+ix];if(c.matter[1]>.5)return 0;
        int p[3]{ix,iy,iz};p[axis]++;
        if(p[0]<NX&&p[1]<NY&&p[2]<NZ && cells[(p[2]*NY+p[1])*NX+p[0]].matter[1]>.5)return 0;
        return c.flow[axis];
    };
    for(int iz=0;iz<NZ;iz++)for(int iy=0;iy<NY;iy++)for(int ix=0;ix<NX;ix++) {
        const auto& c=cells[(iz*NY+iy)*NX+ix];
        bool finite=true;for(float v:c.flow)finite=finite&&std::isfinite(v);for(float v:c.matter)finite=finite&&std::isfinite(v);
        if(!finite){invalid++;continue;}
        if(c.matter[1]>.5){wallMax=std::max(wallMax,double(c.flow[3]));continue;}
        air++;double smoke=c.matter[0],px=params.domain[0]+ix+.5,py=params.domain[1]+iy+.5,pz=params.domain[2]+iz+.5;
        minSmoke=std::min(minSmoke,smoke);mass+=smoke;x+=smoke*px;y+=smoke*py;z+=smoke*pz;r2+=smoke*(px*px+py*py+pz*pz);
        minT=std::min(minT,double(c.flow[3]));maxT=std::max(maxT,double(c.flow[3]));heat+=(c.flow[3]-300)*1206;
        double u=.5*(velocity(ix,iy,iz,0)+velocity(ix-1,iy,iz,0));
        double v=.5*(velocity(ix,iy,iz,1)+velocity(ix,iy-1,iz,1));
        double w=.5*(velocity(ix,iy,iz,2)+velocity(ix,iy,iz-1,2));
        double speed=std::sqrt(u*u+v*v+w*w);maxSpeed=std::max(maxSpeed,speed);
        const double div=velocity(ix,iy,iz,0)-velocity(ix-1,iy,iz,0)+velocity(ix,iy,iz,1)-velocity(ix,iy-1,iz,1)+velocity(ix,iy,iz,2)-velocity(ix,iy,iz-1,2);
        double lap=-6*c.matter[2];
        const int offsets[6][3]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        for(const auto& offset:offsets) {
            int x2=ix+offset[0],y2=iy+offset[1],z2=iz+offset[2];
            if(z2<0)lap+=c.matter[2];
            else if(x2>=0&&x2<NX&&y2>=0&&y2<NY&&z2<NZ) {
                const auto& n=cells[(z2*NY+y2)*NX+x2];lap+=n.matter[1]>.5?c.matter[2]:n.matter[2];
            }
        }
        pressureResidual+=(lap-c.matter[3])*(lap-c.matter[3]);
        divBefore+=c.matter[3]*c.matter[3];divAfter+=div*div;
    }
    if(invalid)throw std::runtime_error("Non-finite transport cells");
    double cx=mass>0?x/mass:0,cy=mass>0?y/mass:0,cz=mass>0?z/mass:0;
    return {{"simulation_time_s",snapshot.step*DT},{"smoke_integral",mass},{"smoke_min",minSmoke},
        {"smoke_centroid_x_m",cx},{"smoke_centroid_y_m",cy},{"smoke_centroid_z_m",cz},
        {"smoke_variance_m2",mass>0?std::max(0.0,r2/mass-cx*cx-cy*cy-cz*cz):0},
        {"air_min_K",minT},{"air_max_K",maxT},{"surface_max_K",wallMax},{"air_excess_heat_J",heat},
        {"max_speed_m_s",maxSpeed},{"divergence_before_rms",std::sqrt(divBefore/std::max(air,1))},
        {"divergence_after_rms",std::sqrt(divAfter/std::max(air,1))},
        {"pressure_residual_rms",std::sqrt(pressureResidual/std::max(air,1))},{"invalid_cells",invalid},
        {"column_probe_solid",cells[(10*NY+45)*NX+32].matter[1]},
        {"slab_probe_solid",cells[(4*NY+40)*NX+25].matter[1]},
        {"interior_probe_solid",cells[(6*NY+40)*NX+27].matter[1]}};
}
