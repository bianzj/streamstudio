#include "scene_sensor.h"
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
double spectral(double wavelength,double temperature) {
    return 1.191043934e8/(std::pow(wavelength,5)*std::expm1(14388.29104/(wavelength*temperature)));
}
double radiance(const SceneSensorOptions& o,double temperature) {
    if(!o.bandIntegrated)return spectral(o.wavelength,temperature);
    double sum=0;for(int i=0;i<8;i++)sum+=spectral(8+double(i)*6/7,temperature)*(i==0||i==7?.5:1);
    return sum*6/7;
}
struct Push {std::array<int32_t,4> image;std::array<float,4> exposure,calibration,optics;};
static_assert(sizeof(Push)==64);
}

struct SceneSensor::Impl {
    SceneSensorOptions options;
    VkDevice device{};VkQueue queue{};VkBuffer raw{};VkDeviceMemory memory{};
    VkDescriptorSetLayout setLayout{};VkDescriptorPool descriptorPool{};VkDescriptorSet set{};
    VkPipelineLayout layout{};VkShaderModule module{};VkPipeline pipeline{};
    VkCommandPool commandPool{};VkCommandBuffer cmd{};VkFence fence{};
    void* mapped=nullptr;std::vector<float> cache;double low=0,high=0;bool initialized=false;
    ~Impl() {
        if(!device)return;vkDeviceWaitIdle(device);
        if(mapped)vkUnmapMemory(device,memory);
        if(fence)vkDestroyFence(device,fence,nullptr);
        if(commandPool)vkDestroyCommandPool(device,commandPool,nullptr);
        if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
        if(module)vkDestroyShaderModule(device,module,nullptr);
        if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
        if(descriptorPool)vkDestroyDescriptorPool(device,descriptorPool,nullptr);
        if(setLayout)vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
        if(raw)vkDestroyBuffer(device,raw,nullptr);
        if(memory)vkFreeMemory(device,memory,nullptr);
    }
};

SceneSensor::SceneSensor(AppSetting& app,VkBuffer output,const std::filesystem::path& shader,
    const std::filesystem::path& snapshot,const SceneSensorOptions& options):m(std::make_unique<Impl>()) {
    m->options=options;m->device=app.m_context.m_device;m->queue=app.m_queues.front().queue;
    const VkDevice device=m->device;const auto size=VkDeviceSize(options.width)*options.height*sizeof(float);
    m->cache.resize(size/sizeof(float));
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(device,&bi,nullptr,&m->raw),"sensor radiance buffer");
    VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,m->raw,&req);
    VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(app.m_context.m_physicalDevice,&props);
    uint32_t type=UINT32_MAX;
    constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(uint32_t i=0;i<props.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
    if(type==UINT32_MAX)throw std::runtime_error("No coherent sensor readback memory");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
    vkCheck(vkAllocateMemory(device,&ai,nullptr,&m->memory),"sensor memory");
    vkCheck(vkBindBufferMemory(device,m->raw,m->memory,0),"sensor bind");
    vkCheck(vkMapMemory(device,m->memory,0,size,0,&m->mapped),"sensor map");
    VkDescriptorSetLayoutBinding bindings[2]{{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=2;li.pBindings=bindings;
    vkCheck(vkCreateDescriptorSetLayout(device,&li,nullptr,&m->setLayout),"sensor descriptor layout");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=1;pi.pPoolSizes=&ps;
    vkCheck(vkCreateDescriptorPool(device,&pi,nullptr,&m->descriptorPool),"sensor descriptor pool");
    VkDescriptorSetAllocateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};si.descriptorPool=m->descriptorPool;si.descriptorSetCount=1;si.pSetLayouts=&m->setLayout;
    vkCheck(vkAllocateDescriptorSets(device,&si,&m->set),"sensor descriptor set");
    for(int i=0;i<2;i++) {
        VkDescriptorBufferInfo info{i==0?m->raw:output,0,size};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=m->set;write.dstBinding=i;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.descriptorCount=1;write.pBufferInfo=&info;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=1;pli.pSetLayouts=&m->setLayout;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&range;
    vkCheck(vkCreatePipelineLayout(device,&pli,nullptr,&m->layout),"sensor pipeline layout");
    std::ifstream input(shader,std::ios::binary|std::ios::ate);if(!input)throw std::runtime_error("Missing sensor SPIR-V");
    const auto length=input.tellg();if(length<=0||length%4)throw std::runtime_error("Invalid sensor SPIR-V");
    std::vector<uint32_t> code(size_t(length)/4);input.seekg(0);input.read(reinterpret_cast<char*>(code.data()),length);
    if(!input)throw std::runtime_error("Cannot read sensor SPIR-V");
    std::ofstream copy(snapshot,std::ios::binary);copy.write(reinterpret_cast<char*>(code.data()),length);copy.flush();if(!copy)throw std::runtime_error("Cannot snapshot sensor SPIR-V");
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smi.codeSize=code.size()*4;smi.pCode=code.data();
    vkCheck(vkCreateShaderModule(device,&smi,nullptr,&m->module),"sensor shader");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=m->layout;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=m->module;ci.stage.pName="main";
    vkCheck(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,nullptr,&m->pipeline),"sensor pipeline");
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpi.queueFamilyIndex=app.m_queues.front().familyIndex;cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCheck(vkCreateCommandPool(device,&cpi,nullptr,&m->commandPool),"sensor command pool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=m->commandPool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
    vkCheck(vkAllocateCommandBuffers(device,&cai,&m->cmd),"sensor command buffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkCheck(vkCreateFence(device,&fi,nullptr,&m->fence),"sensor fence");
}
SceneSensor::~SceneSensor()=default;
std::vector<uint8_t> SceneSensor::saveState()const{
    std::vector<uint8_t> out(2*sizeof(double)+1);std::memcpy(out.data(),&m->low,sizeof(double));std::memcpy(out.data()+sizeof(double),&m->high,sizeof(double));out.back()=m->initialized?1:0;return out;
}
void SceneSensor::loadState(const std::vector<uint8_t>& s){
    if(s.size()!=2*sizeof(double)+1)throw std::runtime_error("Sensor checkpoint size mismatch");
    std::memcpy(&m->low,s.data(),sizeof(double));std::memcpy(&m->high,s.data()+sizeof(double),sizeof(double));m->initialized=s.back()!=0;
}
VkBuffer SceneSensor::radianceBuffer()const{return m->raw;}
VkDeviceSize SceneSensor::radianceBytes()const{return VkDeviceSize(m->cache.size())*sizeof(float);}
nlohmann::json SceneSensor::process(int frame,double interval) {
    const auto& o=m->options;
    std::memcpy(m->cache.data(),m->mapped,size_t(radianceBytes()));
    float minimum=std::numeric_limits<float>::max(),maximum=0;
    std::vector<float> sample;sample.reserve(24000);
    const size_t stride=std::max(size_t(1),m->cache.size()/20000);
    const double histogramFloor=radiance(o,275);
    for(size_t i=0;i<m->cache.size();i++) {
        float value=m->cache[i];if(!std::isfinite(value)||value<0)throw std::runtime_error("Invalid linear thermal radiance");
        minimum=std::min(minimum,value);maximum=std::max(maximum,value);
        if(i%stride==0 && value>=histogramFloor)sample.push_back(value);
    }
    if(sample.size()<32)for(size_t i=0;i<m->cache.size();i+=stride)sample.push_back(m->cache[i]);
    auto percentile=[&](double quantile) {
        const size_t index=std::min(sample.size()-1,size_t(quantile*(sample.size()-1)));
        std::nth_element(sample.begin(),sample.begin()+index,sample.end());return double(sample[index]);
    };
    double targetLow=o.agc?percentile(.02):radiance(o,o.minK);
    double targetHigh=o.agc?percentile(.993):radiance(o,o.maxK);
    const double referenceDerivative=radiance(o,300.5)-radiance(o,299.5);
    if(o.agc)targetHigh=std::max(targetHigh,targetLow+o.agcMinimumSpanKelvin*referenceDerivative);
    if(o.agc&&o.logarithmicDisplay)targetHigh=std::max(targetHigh,double(maximum));
    if(!m->initialized){m->low=targetLow;m->high=targetHigh;m->initialized=true;}
    else {
        const double alpha=o.responseSeconds>0?1-std::exp(-interval/o.responseSeconds):1;
        m->low+=(targetLow-m->low)*alpha;m->high+=(targetHigh-m->high)*alpha;
    }
    size_t clipped=0;for(float v:m->cache)clipped+=v>m->high;
    Push push{{o.width,o.height,frame,o.binning},{float(m->low),float(m->high),o.blurSigma,float(referenceDerivative*o.noiseKelvin)},
        {float(referenceDerivative*o.fixedPatternKelvin),.92f,4095,17},{o.logarithmicDisplay?1.f:0.f,float(10*referenceDerivative),0,0}};
    vkCheck(vkResetCommandBuffer(m->cmd,0),"sensor reset");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(m->cmd,&begin),"sensor begin");
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};before.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;before.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(m->cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&before,0,nullptr,0,nullptr);
    vkCmdBindPipeline(m->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,m->pipeline);
    vkCmdBindDescriptorSets(m->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,m->layout,0,1,&m->set,0,nullptr);
    vkCmdPushConstants(m->cmd,m->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
    vkCmdDispatch(m->cmd,(o.width+7)/8,(o.height+7)/8,1);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};after.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(m->cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,nullptr,0,nullptr);
    vkCheck(vkEndCommandBuffer(m->cmd),"sensor end");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&m->cmd;
    vkCheck(vkQueueSubmit(m->queue,1,&submit,m->fence),"sensor submit");
    vkCheck(vkWaitForFences(m->device,1,&m->fence,VK_TRUE,UINT64_MAX),"sensor wait");
    vkCheck(vkResetFences(m->device,1,&m->fence),"sensor reset fence");
    return {{"frame",frame},{"radiance_min",minimum},{"radiance_max",maximum},{"target_low",targetLow},{"target_high",targetHigh},
        {"exposure_low",m->low},{"exposure_high",m->high},{"display_clipped_fraction",double(clipped)/m->cache.size()}};
}
void SceneSensor::exportRadiance(const std::filesystem::path& path)const {
    std::ofstream output(path,std::ios::binary);output.write(reinterpret_cast<const char*>(m->cache.data()),radianceBytes());output.flush();
    if(!output)throw std::runtime_error("Cannot export float radiance");
}
nlohmann::json SceneSensor::description()const {
    const auto& o=m->options;
    return {{"enabled",true},{"processingDomain","linear radiance before display mapping"},
        {"detectorPixels",{(o.width+o.binning-1)/o.binning,(o.height+o.binning-1)/o.binning}},
        {"binning",o.binning},{"gaussianSigmaDetectorPixels",o.blurSigma},
        {"blurKernelDetectorPixels",5},{"blurModel","truncated normalized Gaussian"},
        {"noiseEquivalentKelvinAt300K",o.noiseKelvin},{"fixedPatternEquivalentKelvinAt300K",o.fixedPatternKelvin},
        {"temporalAgc",o.agc},{"agcResponseSeconds",o.responseSeconds},{"agcPercentiles",{2,99.3}},
        {"agcMinimumSpanKelvin",o.agcMinimumSpanKelvin},{"logarithmicDisplay",o.logarithmicDisplay},
        {"agcHistogramFloorReferenceKelvin",275},{"agcColdSceneFallback","all samples when fewer than 32 exceed the floor"},
        {"displayQuantizationBits",12},{"radianceExportFormat","little-endian float32 row-major"},
        {"limitations","generic configured PSF/noise/display chain; no calibrated camera, atmosphere or measured NEDT"}};
}
