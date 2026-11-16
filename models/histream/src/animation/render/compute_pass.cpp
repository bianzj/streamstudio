#include "compute_pass.h"
#include "animation/scene/mesh_scene.h"
#include "base/vkcheck.h"
#include <fstream>
#include <map>
#include <stdexcept>

namespace animation {
HostBuffer::HostBuffer(AppSetting& app,VkDeviceSize bytes):device(app.m_context.m_device),size(bytes){
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(device,&bi,nullptr,&buffer),"sequence buffer");
    VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer,&req);VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(app.m_context.m_physicalDevice,&props);
    uint32_t type=UINT32_MAX;constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(uint32_t i=0;i<props.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
    if(type==UINT32_MAX)throw std::runtime_error("No coherent sequence buffer memory");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
    vkCheck(vkAllocateMemory(device,&ai,nullptr,&memory),"sequence memory");vkCheck(vkBindBufferMemory(device,buffer,memory,0),"sequence bind");vkCheck(vkMapMemory(device,memory,0,size,0,&mapped),"sequence map");
}
HostBuffer::~HostBuffer(){if(mapped)vkUnmapMemory(device,memory);if(buffer)vkDestroyBuffer(device,buffer,nullptr);if(memory)vkFreeMemory(device,memory,nullptr);}
VkDescriptorBufferInfo HostBuffer::info()const{return {buffer,0,size};}void* HostBuffer::data()const{return mapped;}
struct ComputePass::Impl {
    VkDevice device{};VkQueue queue{};VkDescriptorSetLayout setLayout{};VkDescriptorPool pool{};VkDescriptorSet set{};
    VkPipelineLayout layout{};VkPipeline pipeline{};VkShaderModule shader{};VkCommandPool commands{};VkCommandBuffer cmd{};VkFence fence{};
    uint32_t pushBytes=0;std::vector<uint32_t> code;std::vector<VkDescriptorType> types;std::vector<VkCommandBuffer> strips;
    // Records exactly the barrier/dispatch sequence of the original single dispatch.
    void record(VkCommandBuffer c,const void* push,uint32_t x,uint32_t y,uint32_t z){
        vkCheck(vkResetCommandBuffer(c,0),"sequence reset");VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;vkCheck(vkBeginCommandBuffer(c,&begin),"sequence begin");
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};before.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;before.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&before,0,nullptr,0,nullptr);
        vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);vkCmdPushConstants(c,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,pushBytes,push);vkCmdDispatch(c,x,y,z);
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};after.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,nullptr,0,nullptr);
        vkCheck(vkEndCommandBuffer(c),"sequence end");
    }
    ~Impl(){if(!device)return;vkDeviceWaitIdle(device);if(fence)vkDestroyFence(device,fence,nullptr);if(commands)vkDestroyCommandPool(device,commands,nullptr);if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);if(shader)vkDestroyShaderModule(device,shader,nullptr);if(layout)vkDestroyPipelineLayout(device,layout,nullptr);if(pool)vkDestroyDescriptorPool(device,pool,nullptr);if(setLayout)vkDestroyDescriptorSetLayout(device,setLayout,nullptr);}
};
ComputePass::ComputePass(AppSetting& app,const std::filesystem::path& path,const std::vector<VkDescriptorType>& types,uint32_t pushBytes):m(std::make_unique<Impl>()){
    m->device=app.m_context.m_device;m->queue=app.m_queues.front().queue;m->types=types;m->pushBytes=pushBytes;const auto d=m->device;
    std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Missing sequence shader: "+animation::pathText(path));auto length=f.tellg();if(length<=0||length%4)throw std::runtime_error("Invalid sequence shader");m->code.resize(size_t(length)/4);f.seekg(0);f.read(reinterpret_cast<char*>(m->code.data()),length);if(!f)throw std::runtime_error("Cannot read sequence shader");
    std::vector<VkDescriptorSetLayoutBinding> bindings;std::map<VkDescriptorType,uint32_t> counts;
    for(uint32_t i=0;i<types.size();i++){bindings.push_back({i,types[i],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});counts[types[i]]++;}
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=uint32_t(bindings.size());li.pBindings=bindings.data();vkCheck(vkCreateDescriptorSetLayout(d,&li,nullptr,&m->setLayout),"sequence descriptor layout");
    std::vector<VkDescriptorPoolSize> sizes;for(const auto& item:counts)sizes.push_back({item.first,item.second});
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=uint32_t(sizes.size());pi.pPoolSizes=sizes.data();vkCheck(vkCreateDescriptorPool(d,&pi,nullptr,&m->pool),"sequence descriptor pool");
    VkDescriptorSetAllocateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};si.descriptorPool=m->pool;si.descriptorSetCount=1;si.pSetLayouts=&m->setLayout;vkCheck(vkAllocateDescriptorSets(d,&si,&m->set),"sequence set");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,pushBytes};VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=1;pli.pSetLayouts=&m->setLayout;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&range;vkCheck(vkCreatePipelineLayout(d,&pli,nullptr,&m->layout),"sequence pipeline layout");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=m->code.size()*4;sm.pCode=m->code.data();vkCheck(vkCreateShaderModule(d,&sm,nullptr,&m->shader),"sequence shader");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=m->layout;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=m->shader;ci.stage.pName="main";vkCheck(vkCreateComputePipelines(d,VK_NULL_HANDLE,1,&ci,nullptr,&m->pipeline),"sequence pipeline");
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=app.m_queues.front().familyIndex;cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;vkCheck(vkCreateCommandPool(d,&cp,nullptr,&m->commands),"sequence command pool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=m->commands;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;vkCheck(vkAllocateCommandBuffers(d,&ca,&m->cmd),"sequence command");VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkCheck(vkCreateFence(d,&fi,nullptr,&m->fence),"sequence fence");
}
ComputePass::~ComputePass()=default;
void ComputePass::bind(uint32_t binding,VkDescriptorBufferInfo info){VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=m->set;w.dstBinding=binding;w.descriptorType=m->types.at(binding);w.descriptorCount=1;w.pBufferInfo=&info;vkUpdateDescriptorSets(m->device,1,&w,0,nullptr);}
void ComputePass::bind(uint32_t binding,VkAccelerationStructureKHR as){VkWriteDescriptorSetAccelerationStructureKHR a{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};a.accelerationStructureCount=1;a.pAccelerationStructures=&as;VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.pNext=&a;w.dstSet=m->set;w.dstBinding=binding;w.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;w.descriptorCount=1;vkUpdateDescriptorSets(m->device,1,&w,0,nullptr);}
void ComputePass::dispatch(const void* push,uint32_t x,uint32_t y,uint32_t z){
    m->record(m->cmd,push,x,y,z);VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&m->cmd;vkCheck(vkQueueSubmit(m->queue,1,&submit,m->fence),"sequence submit");vkCheck(vkWaitForFences(m->device,1,&m->fence,VK_TRUE,UINT64_MAX),"sequence wait");vkCheck(vkResetFences(m->device,1,&m->fence),"sequence reset fence");
}
void ComputePass::dispatchStrips(const std::vector<std::vector<uint8_t>>& pushes,uint32_t x,uint32_t y,uint32_t z){
    if(pushes.empty())return;
    for(const auto& p:pushes)if(p.size()<m->pushBytes)throw std::runtime_error("strip push block smaller than the push-constant range");
    if(m->strips.size()<pushes.size()){
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=m->commands;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=uint32_t(pushes.size()-m->strips.size());
        std::vector<VkCommandBuffer> extra(ca.commandBufferCount);vkCheck(vkAllocateCommandBuffers(m->device,&ca,extra.data()),"sequence strip commands");m->strips.insert(m->strips.end(),extra.begin(),extra.end());
    }
    std::vector<VkSubmitInfo> submits(pushes.size(),VkSubmitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO});
    for(size_t i=0;i<pushes.size();i++){m->record(m->strips[i],pushes[i].data(),x,y,z);submits[i].commandBufferCount=1;submits[i].pCommandBuffers=&m->strips[i];}
    vkCheck(vkQueueSubmit(m->queue,uint32_t(submits.size()),submits.data(),m->fence),"sequence strip submit");vkCheck(vkWaitForFences(m->device,1,&m->fence,VK_TRUE,UINT64_MAX),"sequence strip wait");vkCheck(vkResetFences(m->device,1,&m->fence),"sequence strip reset fence");
}
void ComputePass::snapshot(const std::filesystem::path& path)const{std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(m->code.data()),m->code.size()*4);if(!f)throw std::runtime_error("Cannot snapshot sequence shader");}
}
