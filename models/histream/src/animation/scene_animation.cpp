#include "scene_animation.h"
#include "transport/scene_transport.h"
#include "sensor/scene_sensor.h"
#include "render/mesh_renderer.h"
#include "render/mesh_sequence.h"
#include "scene/mesh_scene.h"
#include "base/appsetting.h"
#include "base/vkcheck.h"
#include <nlohmann/json.hpp>
#include <gdal_priv.h>
#include <cpl_vsi.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using Json = nlohmann::json;
struct Push {
    std::array<int32_t,4> image;
    std::array<float,4> time;
    std::array<float,4> camera;
    std::array<float,4> target;
    std::array<int32_t,4> tile;
    std::array<float,4> thermal;
    std::array<float,4> fieldOrigin;
    std::array<int32_t,4> fieldGrid;
};
static_assert(sizeof(Push)==128);

struct Gpu {
    AppSetting app;
    bool contextStarted=false;
    VkDevice device=VK_NULL_HANDLE;
    VkBuffer buffer=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool=VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkShaderModule module=VK_NULL_HANDLE;
    VkCommandPool commandPool=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    void* mapped=nullptr;
    ~Gpu() {
        if(device) {
            vkDeviceWaitIdle(device);
            if(mapped) vkUnmapMemory(device,memory);
            if(fence) vkDestroyFence(device,fence,nullptr);
            if(commandPool) vkDestroyCommandPool(device,commandPool,nullptr);
            if(pipeline) vkDestroyPipeline(device,pipeline,nullptr);
            if(module) vkDestroyShaderModule(device,module,nullptr);
            if(pipelineLayout) vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
            if(descriptorPool) vkDestroyDescriptorPool(device,descriptorPool,nullptr);
            if(setLayout) vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
            if(buffer) vkDestroyBuffer(device,buffer,nullptr);
            if(memory) vkFreeMemory(device,memory,nullptr);
        }
        if(contextStarted) app.destroy();
    }
};

void writeJson(const std::filesystem::path& path, const Json& data) {
    std::ofstream stream(path);
    stream << data.dump(2) << '\n';
    if(!stream) throw std::runtime_error("Cannot write JSON: "+animation::pathText(path));
}
void writePng(const std::filesystem::path& path, int width, int height, void* rgba) {
    auto* mem=GetGDALDriverManager()->GetDriverByName("MEM");
    auto* png=GetGDALDriverManager()->GetDriverByName("PNG");
    if(!mem || !png) throw std::runtime_error("PNG/MEM GDAL drivers are unavailable");
    auto close=[](GDALDataset* d){if(d) GDALClose(d);};
    std::unique_ptr<GDALDataset,decltype(close)> image(mem->Create("",width,height,4,GDT_Byte,nullptr),close);
    if(!image || image->RasterIO(GF_Write,0,0,width,height,rgba,width,height,GDT_Byte,4,nullptr,4,width*4,1,nullptr)!=CE_None)
        throw std::runtime_error("Cannot prepare GPU PNG output");
    const std::string virtualPath="/vsimem/histream-animation-"+std::to_string(reinterpret_cast<uintptr_t>(rgba))+".png";
    const char* options[]={"ZLEVEL=2",nullptr};
    {
        std::unique_ptr<GDALDataset,decltype(close)> encoded(png->CreateCopy(virtualPath.c_str(),image.get(),FALSE,const_cast<char**>(options),nullptr,nullptr),close);
        if(!encoded) {VSIUnlink(virtualPath.c_str());throw std::runtime_error("PNG encoding failed");}
    }
    vsi_l_offset size=0;
    auto* bytes=VSIGetMemFileBuffer(virtualPath.c_str(),&size,FALSE);
    std::ofstream output(path,std::ios::binary);
    if(bytes) output.write(reinterpret_cast<const char*>(bytes),static_cast<std::streamsize>(size));
    output.flush();
    VSIUnlink(virtualPath.c_str());
    if(!bytes || !output) throw std::runtime_error("Cannot write animation PNG");
}
float finiteValue(const Json& data, const char* name, float fallback, float lo, float hi) {
    const float value=data.value(name,fallback);
    if(!std::isfinite(value) || value<lo || value>hi)
        throw std::runtime_error(std::string(name)+" is outside the supported finite range");
    return value;
}
int integerValue(const Json& data, const char* name, int fallback, int lo, int hi) {
    if(data.contains(name) && !data.at(name).is_number_integer())
        throw std::runtime_error(std::string(name)+" must be an integer");
    if(!data.contains(name)) return fallback;
    if(data.at(name).is_number_unsigned()) {
        const auto value=data.at(name).get<uint64_t>();
        if(value>static_cast<uint64_t>(hi) || value<static_cast<uint64_t>(lo))
            throw std::runtime_error(std::string(name)+" is outside the supported range");
        return static_cast<int>(value);
    }
    const auto value=data.at(name).get<int64_t>();
    if(value<lo || value>hi) throw std::runtime_error(std::string(name)+" is outside the supported range");
    return static_cast<int>(value);
}
}

int runSceneAnimation(const std::filesystem::path& input,
                      const std::filesystem::path& rawOutput,
                      const std::filesystem::path& shader,
                      const std::filesystem::path& executable) {
    auto output=std::filesystem::absolute(rawOutput).lexically_normal();
    while(output.filename().empty() && output.has_relative_path()) output=output.parent_path();
    std::ifstream source(input);
    if(!source) throw std::runtime_error("Cannot open scene animation input");
    Json config;
    source >> config;
    if(!config.is_object()) throw std::runtime_error("Scene animation config must be an object");
    const auto backend=config.value("backend",std::string("procedural"));
    if(backend=="meshSequence")return animation::runMeshSequence(input,rawOutput,shader.parent_path(),executable);
    if(backend=="mesh")return animation::runMeshScene(input,rawOutput,shader.parent_path()/"mesh/static.comp.spv",executable);
    if(backend!="procedural")throw std::runtime_error("backend must be procedural or mesh");
    const int width=integerValue(config,"width",1280,32,4096);
    const int height=integerValue(config,"height",720,32,4096);
    const int fps=integerValue(config,"fps",24,1,120);
    const int count=integerValue(config,"frameCount",288,1,10000);
    const int first=integerValue(config,"firstFrame",1,1,count);
    const int last=integerValue(config,"lastFrame",count,first,count);
    const int gpuIndex=integerValue(config,"gpuIndex",0,0,128);
    const int samples=integerValue(config,"samples",1,1,4);
    const float impact=finiteValue(config,"impactTime",3,0,3600);
    const std::string renderMode=config.value("renderMode",std::string("visible"));
    if(renderMode!="visible" && renderMode!="thermal")
        throw std::runtime_error("renderMode must be visible or thermal");
    const float wavelength=finiteValue(config,"thermalWavelengthMicrometers",10,3,14);
    const std::string bandMode=config.value("thermalBandMode",std::string("single"));
    const bool showMedia=config.value("showParticipatingMedia",true);
    if(bandMode!="single" && bandMode!="lwir8_14")throw std::runtime_error("thermalBandMode must be single or lwir8_14");
    const float minK=finiteValue(config,"thermalDisplayMinK",260,150,2000);
    const float maxK=finiteValue(config,"thermalDisplayMaxK",650,150,2000);
    if(maxK<=minK) throw std::runtime_error("thermalDisplayMaxK must exceed thermalDisplayMinK");
    const std::string sceneStyle=config.value("sceneStyle",std::string("simple"));
    if(sceneStyle!="simple" && sceneStyle!="detailed")throw std::runtime_error("sceneStyle must be simple or detailed");
    const bool detailed=sceneStyle=="detailed";
    const bool sensorEnabled=config.value("thermalSensor",false);
    if(sensorEnabled && renderMode!="thermal")throw std::runtime_error("thermalSensor requires thermal rendering");
    SceneSensorOptions sensorOptions;
    sensorOptions.width=width;sensorOptions.height=height;sensorOptions.wavelength=wavelength;
    sensorOptions.minK=minK;sensorOptions.maxK=maxK;sensorOptions.bandIntegrated=bandMode=="lwir8_14";
    sensorOptions.binning=integerValue(config,"sensorBinning",2,1,4);
    sensorOptions.blurSigma=finiteValue(config,"sensorBlurSigmaPixels",.7f,0,1.5f);
    sensorOptions.noiseKelvin=finiteValue(config,"sensorNoiseKelvin",.09f,0,2);
    sensorOptions.fixedPatternKelvin=finiteValue(config,"sensorFixedPatternKelvin",.05f,0,2);
    sensorOptions.responseSeconds=finiteValue(config,"sensorResponseSeconds",.35f,0,5);
    sensorOptions.agc=config.value("sensorAgc",true);
    std::vector<int> exportRadiance;
    if(config.contains("exportRadianceFrames")) {
        const auto& frames=config.at("exportRadianceFrames");
        if(!frames.is_array())throw std::runtime_error("exportRadianceFrames must be an array");
        for(const auto& value:frames) {
            if(!value.is_number_integer() || value<1 || value>last)throw std::runtime_error("exportRadianceFrames must lie in the executed frame range 1..lastFrame");
            exportRadiance.push_back(value.get<int>());
        }
    }
    if(!exportRadiance.empty() && !sensorEnabled)throw std::runtime_error("exportRadianceFrames requires thermalSensor");
    const std::string physicsMode=config.value("physicsMode",std::string("procedural"));
    if(physicsMode!="procedural" && physicsMode!="transport") throw std::runtime_error("physicsMode must be procedural or transport");
    if(physicsMode=="transport" && (renderMode!="thermal" || double(count-1)/fps>60))
        throw std::runtime_error("transport physicsMode requires thermal rendering and a timeline of at most 60 seconds");
    SceneTransportOptions transportOptions;
    transportOptions.impactTime=impact;
    transportOptions.detailed=detailed;
    transportOptions.windX=finiteValue(config,"transportWindX",.8f,-5,5);
    transportOptions.windY=finiteValue(config,"transportWindY",.15f,-5,5);
    const std::string scenario=config.value("transportScenario",std::string("heatedPlume"));
    if(scenario!="heatedPlume" && scenario!="ambient" && scenario!="passivePulse")throw std::runtime_error("Unknown transportScenario");
    transportOptions.scenario=scenario=="ambient"?1:scenario=="passivePulse"?2:0;
    // Validate before GPU setup, and never mix a new run with old frames.
    if(std::filesystem::exists(output)) throw std::runtime_error("Animation output already exists; use a new run directory");
    std::ifstream shaderFile(shader,std::ios::binary|std::ios::ate);
    if(!shaderFile) throw std::runtime_error("Missing animation SPIR-V: "+animation::pathText(shader));
    const auto bytes=shaderFile.tellg();
    if(bytes<=0 || bytes%4!=0) throw std::runtime_error("Invalid animation SPIR-V size");
    std::vector<uint32_t> code(static_cast<size_t>(bytes)/4);
    shaderFile.seekg(0);
    shaderFile.read(reinterpret_cast<char*>(code.data()),bytes);
    if(!shaderFile) throw std::runtime_error("Cannot read complete animation SPIR-V");

    Gpu gpu;
    gpu.contextStarted=true;
    gpu.app.init(gpuIndex,ComputeBackend::Gpu);
    gpu.device=gpu.app.m_context.m_device;
    const VkDevice device=gpu.device;
    const VkPhysicalDevice physical=gpu.app.m_context.m_physicalDevice;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical,&properties);
    if(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)
        throw std::runtime_error("Scene animation requires a hardware GPU");
    const VkDeviceSize size=VkDeviceSize(width)*height*4;
    if(size>properties.limits.maxStorageBufferRange)
        throw std::runtime_error("Animation image exceeds GPU storage buffer limit");
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size=size;
    bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(device,&bi,nullptr,&gpu.buffer),"animation buffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device,gpu.buffer,&req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    uint32_t memoryType=UINT32_MAX;
    constexpr VkMemoryPropertyFlags flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(uint32_t i=0;i<mp.memoryTypeCount;i++)
        if((req.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&flags)==flags) {memoryType=i;break;}
    if(memoryType==UINT32_MAX) throw std::runtime_error("No coherent GPU readback memory available");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize=req.size;
    ai.memoryTypeIndex=memoryType;
    vkCheck(vkAllocateMemory(device,&ai,nullptr,&gpu.memory),"animation memory");
    vkCheck(vkBindBufferMemory(device,gpu.buffer,gpu.memory,0),"animation bind");
    vkCheck(vkMapMemory(device,gpu.memory,0,size,0,&gpu.mapped),"animation map");
    VkDescriptorSetLayoutBinding bindings[3]{{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount=3;li.pBindings=bindings;
    vkCheck(vkCreateDescriptorSetLayout(device,&li,nullptr,&gpu.setLayout),"animation descriptor layout");
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets=1;dpi.poolSizeCount=1;dpi.pPoolSizes=&poolSize;
    vkCheck(vkCreateDescriptorPool(device,&dpi,nullptr,&gpu.descriptorPool),"animation descriptor pool");
    VkDescriptorSetAllocateInfo dsi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsi.descriptorPool=gpu.descriptorPool;dsi.descriptorSetCount=1;dsi.pSetLayouts=&gpu.setLayout;
    VkDescriptorSet set=VK_NULL_HANDLE;
    vkCheck(vkAllocateDescriptorSets(device,&dsi,&set),"animation descriptor set");
    VkDescriptorBufferInfo dbi{gpu.buffer,0,size};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet=set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&dbi;
    vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    // Valid unused binding for the default procedural path.
    VkDescriptorBufferInfo dummy{gpu.buffer,0,32};
    write.dstBinding=1;write.pBufferInfo=&dummy;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    write.dstBinding=2;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount=1;pli.pSetLayouts=&gpu.setLayout;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&range;
    vkCheck(vkCreatePipelineLayout(device,&pli,nullptr,&gpu.pipelineLayout),"animation pipeline layout");
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize=code.size()*4;smi.pCode=code.data();
    vkCheck(vkCreateShaderModule(device,&smi,nullptr,&gpu.module),"animation shader");
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.layout=gpu.pipelineLayout;
    pi.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pi.stage.module=gpu.module;pi.stage.pName="main";
    vkCheck(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&gpu.pipeline),"animation pipeline");
    const auto queue=gpu.app.m_queues.front();
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex=queue.familyIndex;cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCheck(vkCreateCommandPool(device,&cpi,nullptr,&gpu.commandPool),"animation command pool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool=gpu.commandPool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
    VkCommandBuffer cmd=VK_NULL_HANDLE;
    vkCheck(vkAllocateCommandBuffers(device,&cai,&cmd),"animation command buffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCheck(vkCreateFence(device,&fi,nullptr,&gpu.fence),"animation fence");
    if(!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    if(!std::filesystem::create_directory(output))
        throw std::runtime_error("Animation output already exists; use a new run directory");
    std::filesystem::create_directory(output/"frames");
    std::filesystem::create_directory(output/"provenance");
    // Snapshot the exact loaded SPIR-V, rather than hashing a mutable path later.
    {
        std::ofstream snapshot(output/"provenance/renderer.comp.spv",std::ios::binary);
        snapshot.write(reinterpret_cast<const char*>(code.data()),code.size()*4);
        snapshot.flush();
        if(!snapshot) throw std::runtime_error("Cannot write renderer provenance");
    }
    std::filesystem::copy_file(executable,output/"provenance/histream.exe");
    std::unique_ptr<SceneTransport> transport;
    if(physicsMode=="transport") transport=std::make_unique<SceneTransport>(gpu.app,shader.parent_path()/"transport.comp.spv",
        output/"provenance/transport.comp.spv",transportOptions);
    std::unique_ptr<SceneSensor> sensor;
    if(sensorEnabled) {
        sensor=std::make_unique<SceneSensor>(gpu.app,gpu.buffer,shader.parent_path()/"sensor.comp.spv",output/"provenance/sensor.comp.spv",sensorOptions);
        VkDescriptorBufferInfo raw{sensor->radianceBuffer(),0,sensor->radianceBytes()};
        write.dstBinding=2;write.pBufferInfo=&raw;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        if(!exportRadiance.empty())std::filesystem::create_directory(output/"radiance");
    }
    GDALAllRegister();
    writeJson(output/"input.json",config);
    Json manifest={{"kind","histream-gpu-scene-animation"},{"status","running"},
        {"backend","gpu"},{"device",properties.deviceName},{"gpuIndex",gpuIndex},
        {"width",width},{"height",height},{"fps",fps},{"timelineFrameCount",count},
        {"firstFrame",first},{"lastFrame",last},{"frameCount",last-first+1},
        {"impactTime",impact},{"physicsValidated",false},{"shader",animation::pathText(shader)},
        {"shaderSnapshot","provenance/renderer.comp.spv"},{"engineSnapshot","provenance/histream.exe"},
        {"scope","GPU procedural RGB visualization; prescribed motion, facade opening, fire and smoke. No blast, structural, combustion or spectral solver."}};
    writeJson(output/"run-manifest.json",manifest);
    std::ofstream times(output/"frames.csv");
    times<<"frame,presentation_time_seconds,png\n";
    manifest["renderMode"]=renderMode;
    manifest["radiometricallyValidated"]=false;
    manifest["thermalFieldIsPrescribed"]=renderMode=="thermal";
    manifest["physicsMode"]=physicsMode;
    manifest["sceneStyle"]=sceneStyle;
    if(detailed)manifest["visualAssets"]={{"materials","concrete, curtain glass, brick, industrial panels"},
        {"damage","staged preset planar cuts, edge chips and visual reinforcement"},
        {"renderOnlyAccessories","parked vehicles, curbs, bollards and thin reinforcement; not flow obstacles"},
        {"backgroundTemperature","prescribed spatial/material/room variations; not a diurnal energy-balance solution"}};
    manifest["sensorWarmupFrames"]=sensorEnabled?first-1:0;
    if(sensor){manifest["sensor"]=sensor->description();manifest["sensorShaderSnapshot"]="provenance/sensor.comp.spv";manifest["radianceExportFrames"]=exportRadiance;}
    manifest["geometry"]={{"contextBuildingCount",13},{"hollowTargetInterior",true},
        {"damageEvolution","prescribed local removal of facade, slabs and columns; no structural response solver"}};
    manifest["mediumRenderingEnabled"]=showMedia;
    if(transport) {
        manifest["thermalFieldIsPrescribed"]=false;
        manifest["transport"]=SceneTransport::description();
        manifest["transport"]["scenario"]=scenario;
        manifest["transport"]["windMetersPerSecond"]={transportOptions.windX,transportOptions.windY,0};
        manifest["transportShaderSnapshot"]="provenance/transport.comp.spv";
        manifest["sourceAndMaterialBaselinesArePrescribed"]=true;
    }
    if(renderMode=="thermal") {
        manifest["thermal"]={{"wavelengthMicrometers",wavelength},{"displayMinK",minK},{"displayMaxK",maxK},
            {"palette","white-hot"},{"displayStretch",sensorEnabled?(sensorOptions.agc?"temporally smoothed percentile AGC, gamma 0.92":"fixed radiance normalization, gamma 0.92"):"fixed logarithmic spectral radiance"},
            {"temperatureSource","prescribed procedural temperatures; no heat/combustion solver"}};
        manifest["scope"]="GPU single-wavelength thermal visualization with prescribed temperatures, greybody surfaces and simplified absorbing/emitting volumes; no calibrated sensor, atmosphere, blast or structural solver.";
        manifest["thermal"]["bandMode"]=bandMode;
        manifest["thermal"]["configuredDisplayBoundsUsed"]=!sensorEnabled||!sensorOptions.agc;
        manifest["thermal"]["radianceUnits"]=bandMode=="single"?"W m-2 sr-1 um-1":"W m-2 sr-1";
        if(bandMode=="lwir8_14") {
            manifest["thermal"]["bandMinMicrometers"]=8;
            manifest["thermal"]["bandMaxMicrometers"]=14;
            manifest["thermal"]["bandResponse"]="uniform response, 8-point trapezoidal integration; not a specific instrument SRF";
            manifest["scope"]="GPU 8-14 um thermal visualization with supplied temperature fields and simplified volume transfer; no calibrated sensor or blast solver.";
        }
        if(transport) {
            manifest["thermal"]["temperatureSource"]="GPU evolved air temperature and surface-temperature perturbation; supplied heat/tracer sources and prescribed material baselines";
            manifest["scope"]="GPU low-speed heat/tracer transport and surface-layer cooling coupled to thermal rendering; sources/baselines prescribed, no blast, combustion, structural or calibrated sensor solver.";
        }
    }
    writeJson(output/"run-manifest.json",manifest);
    Push push{{width,height,0,count},{0,impact,1.0f,float(samples)},{61,-90,53,1.35f},{-3,-2,13,0},
              {0,renderMode=="thermal"?1:0,transport?1:0,bandMode=="lwir8_14"?1:0},{wavelength,minK,maxK,16},{-32,-40,0,1},{64,56,56,showMedia?1:0}};
    push.fieldGrid[3]=(showMedia?1:0)|(detailed?2:0)|(sensorEnabled?4:0);
    if(detailed){push.camera={60,-90,47,1.45f};push.target={0,7,11,0};}
    manifest["camera"]={{"position",{push.camera[0],push.camera[1],push.camera[2]}},
        {"target",{push.target[0],push.target[1],push.target[2]}},{"focalScale",push.camera[3]}};
    writeJson(output/"run-manifest.json",manifest);
    std::ofstream transportLog;
    bool transportHeader=false;
    if(transport)transportLog.open(output/"transport.csv");
    std::ofstream sensorLog;bool sensorHeader=false;
    if(sensor)sensorLog.open(output/"sensor.csv");
    const auto begin=std::chrono::steady_clock::now();
    double dispatchSeconds=0, pngSeconds=0;
    for(int frame=sensorEnabled?1:first;frame<=last;frame++) {
        push.image[2]=frame-1;
        push.time[0]=float(frame-1)/fps;
        if(transport) {
            transport->advanceTo(double(frame-1)/fps);
            if(frame>=first) {
            const auto values=transport->statistics();
            if(!transportHeader) {
                transportLog<<"frame,presentation_time_s";for(const auto& item:values.items())transportLog<<','<<item.key();transportLog<<'\n';transportHeader=true;
            }
            transportLog<<frame<<','<<std::setprecision(10)<<double(frame-1)/fps;
            for(const auto& item:values.items())transportLog<<','<<item.value().get<double>();
            transportLog<<'\n';if(!transportLog)throw std::runtime_error("Cannot write transport diagnostics");
            }
            VkDescriptorBufferInfo field{transport->buffer(),0,transport->byteSize()};
            write.dstBinding=1;write.pBufferInfo=&field;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        }
        const auto frameBegin=std::chrono::steady_clock::now();
        // Bound each submission to a narrow strip to avoid a long Windows TDR dispatch.
        for(int row=0;row<height;row+=32) {
            push.tile[0]=row;
            vkCheck(vkResetCommandBuffer(cmd,0),"animation reset command");
            VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            cb.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkCheck(vkBeginCommandBuffer(cmd,&cb),"animation begin command");
            VkMemoryBarrier previous{VK_STRUCTURE_TYPE_MEMORY_BARRIER};previous.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;previous.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&previous,0,nullptr,0,nullptr);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,gpu.pipeline);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,gpu.pipelineLayout,0,1,&set,0,nullptr);
            vkCmdPushConstants(cmd,gpu.pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
            vkCmdDispatch(cmd,(width+7)/8,4,1);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            vkCheck(vkEndCommandBuffer(cmd),"animation end command");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
            vkCheck(vkQueueSubmit(queue.queue,1,&submit,gpu.fence),"animation submit");
            vkCheck(vkWaitForFences(device,1,&gpu.fence,VK_TRUE,UINT64_MAX),"animation GPU wait");
            vkCheck(vkResetFences(device,1,&gpu.fence),"animation reset fence");
        }
        if(sensor) {
            const auto values=sensor->process(frame,1.0/fps);
            if(frame>=first) {
                if(!sensorHeader){bool comma=false;for(const auto& item:values.items()){if(comma)sensorLog<<',';sensorLog<<item.key();comma=true;}sensorLog<<'\n';sensorHeader=true;}
                bool comma=false;for(const auto& item:values.items()){if(comma)sensorLog<<',';sensorLog<<std::setprecision(10)<<item.value().get<double>();comma=true;}sensorLog<<'\n';
                if(!sensorLog)throw std::runtime_error("Cannot write sensor diagnostics");
            }
            if(std::find(exportRadiance.begin(),exportRadiance.end(),frame)!=exportRadiance.end()) {
                std::ostringstream name;name<<"frame_"<<std::setw(4)<<std::setfill('0')<<frame<<".f32";
                sensor->exportRadiance(output/"radiance"/name.str());
            }
        }
        if(frame<first)continue;
        const auto gpuDone=std::chrono::steady_clock::now();
        std::ostringstream name;name<<"frame_"<<std::setw(4)<<std::setfill('0')<<frame<<".png";
        writePng(output/"frames"/name.str(),width,height,gpu.mapped);
        const auto pngDone=std::chrono::steady_clock::now();
        const double dispatchTime=std::chrono::duration<double>(gpuDone-frameBegin).count();
        const double pngTime=std::chrono::duration<double>(pngDone-gpuDone).count();
        dispatchSeconds+=dispatchTime;pngSeconds+=pngTime;
        times<<frame<<','<<std::setprecision(9)<<push.time[0]<<','<<name.str()<<'\n';
        if(!times) throw std::runtime_error("Cannot write animation frame receipt");
        std::cout<<"FRAME\t"<<frame<<'/'<<count<<"\tt="<<push.time[0]<<'\t'<<name.str()
                 <<"\tdispatch/readback="<<dispatchTime<<"\tpng="<<pngTime<<std::endl;
    }
    times.flush();
    if(transport){transportLog.flush();if(!transportLog)throw std::runtime_error("Cannot flush transport diagnostics");}
    if(sensor){sensorLog.flush();if(!sensorLog)throw std::runtime_error("Cannot flush sensor diagnostics");}
    if(!times) throw std::runtime_error("Cannot flush animation frame receipt");
    manifest["status"]="complete";
    manifest["wallSeconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    manifest["dispatchReadbackWallSeconds"]=dispatchSeconds;
    manifest["pngWriteWallSeconds"]=pngSeconds;
    writeJson(output/"run-manifest.json",manifest);
    return 0;
}
