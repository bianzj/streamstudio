// ==========================================================================
// 文件：raytracingio.h
// 功能：RayTracing 模式的“数据总线”类 RaytracingIO：把场景/观测/设置数据与全部 Vulkan 句柄
//       集中在一个对象里，供 Raytracing 及其子模块（buffer、descriptor、pipeline、command）
//       通过 shared_ptr 共享读写（成员均为 public，没有逻辑）。
// 主要内容：
//   1. 通用 IO 容器：m_meshio、m_instanceio、m_virtualio、m_defined、m_pAccelStruct
//   2. 场景/观测几何：场景尺寸与原点、sensor/light、角度与波长列表、UAV 航点
//   3. 设置：日期时间、波段/角度数、图像尺寸、采样数、最大深度、周期边界、天空盒、输出开关
//   4. GPU 缓冲：波长、传感器、光源、物理纹理偏移与因子
//   5. Vulkan 对象：设备/队列/命令池/描述符/管线布局/管线
//   6. SBT 相关：SBTWrapper 或手工 SBT 的缓冲与各区域
// 数据流：Raytracing::upload/uploadSetting 填充 2、3 → buffer.cpp 建 4 → descriptor 与 pipeline
//         建 5、6 → Command::run 使用全部。着色器读取的推送常量为 RayRTSetting（structs_cg.h）。
// 约定：kNoTextureOffset 须与 offline.rgen 中的 NO_TEXTURE_OFFSET 一致；
//       Allocator 类型由编译宏 ALLOC_DMA / ALLOC_VMA 选择，缺省为 Dedicated。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_RAYTRACINGIO_H
#define FIELD_RAYTRACINGIO_H

#include <vulkan/vulkan.hpp>
#include <nvvk/resourceallocator_vk.hpp>
#include <nvvk/context_vk.hpp>
#include <nvvk/commands_vk.hpp>
#include <nvvk/debug_util_vk.hpp>
#include <nvvk/descriptorsets_vk.hpp>
#include <nvvk/sbtwrapper_vk.hpp>
//#include <nvvk/raytraceKHR_vk.hpp>

#include "src/base/accelstruct.h"
#include "src/base/meshio.h"
#include "src/base/instanceio.h"
#include "src/base/virtualio.h"
#include "src/base/fileio.h"
#include "src/base/structs.h"
#include "src/base/structs_cg.h"
#include "src/base/queue.h"
#include "src/base/appsetting.h"
#include "src/base/defined.h"



// #define ALLOC_DMA  <--- This is in the CMakeLists.txt
#include "nvvk/resourceallocator_vk.hpp"
#if defined(ALLOC_DMA)
#include <nvvk/memallocator_dma_vk.hpp>
typedef nvvk::ResourceAllocatorDma Allocator;
#elif defined(ALLOC_VMA)
#include <nvvk/memallocator_vma_vk.hpp>
typedef nvvk::ResourceAllocatorVma Allocator;
#else
typedef nvvk::ResourceAllocatorDedicated Allocator;
#endif




// 常量：物理纹理偏移的“无纹理”标记（uint32 最大值）。
// Must match NO_TEXTURE_OFFSET in offline.rgen.
constexpr uint32_t kNoTextureOffset = 0xFFFFFFFFu;

// 类：RaytracingIO——RayTracing 模式的共享状态。构造时创建网格/实例/虚拟屏幕/defined/
// 加速结构等子 IO 对象；其余成员由 Raytracing 的各步骤填充（见文件头的数据流）。
class RaytracingIO {


public:
    RaytracingIO(){
        m_meshio = std::make_shared<MeshIO>();
        m_instanceio = std::make_shared<InstanceIO>();
        m_virtualio = std::make_shared<VirtualIO>();
        m_defined = std::make_shared<DefinedIO>();
        m_pAccelStruct = std::make_shared<AccelStruct>();  // method and resource, have to make
    };



//    void setup(AppSetting &appsetting,const nvvk::Queue& queue, int m_queueIndex, std::shared_ptr<Allocator> &pAlloc );
//    bool createCPUBuffer();
//    bool createGPUBuffer();

 //   bool input(std::shared_ptr<FileIO> &fileio);
//    void updateSensor(SensorMatrix senso);
//    void updateLight(LightSet light);
//    void destroy();
    //
    // projectDir：工程输出目录（TIFF、albedo.txt 写在此处）；
    // definedDir：histream 资源根目录，着色器按 definedDir + "/shader/raytracing/" 查找。
    std::string projectDir;
    std::string definedDir;



    // GPU buffer

   // std::vector<Angle>         m_pGeometry;


    // 通用 IO 容器：网格（含材质链接/光谱参数）、实例、虚拟屏幕（输出存储缓冲）、defined 配置。
    std::shared_ptr<MeshIO> m_meshio;
    std::shared_ptr<InstanceIO> m_instanceio;
    std::shared_ptr<VirtualIO> m_virtualio;
    std::shared_ptr<DefinedIO> m_defined;


   // kangle：当前正在处理的观测角序号（run 的循环变量写入）。
   int kangle = 0;
   //int n_wave = 0;


    // Scene info
    int n_modelmesh = 0;
    int n_instance = 0;
    glm::vec3 sceneSize{1.0f, 1.0f, 1.0f};
    glm::vec3 sceneOrigin{0.0f, 0.0f, 0.0f};
    // voxelSize / voxelOrigin：场景水平范围与原点，由 base/scene.cpp 建场景时写入，
    // Geometry::orthcorrect 用它确定正射校正的地面范围（单位 m）。
    glm::vec3 voxelSize;
    glm::vec3 voxelOrigin;
    float stepsize_surface;
/*    glm::vec3 sMin;
    glm::vec3 sMax;*/

    //Geometry info
    // 观测/光源几何：每个观测角一个 Angle（sza/saa/vza/vaa，度），waves 为波长列表（nm 或 μm）。
    // UAV 巡航模式：uavposes 为航点位置（x、y、z=高度），uavViewAzimuths 为各航点航向，
    // n_pos 为航点数，k_pos 为当前航点。
    SensorMatrix sensor;   // original sensor become the angular sensor and spectral wavesets
    LightSet     light;
    std::vector<Angle> angles;
    std::vector<float> waves;
    bool isUAVTrave{false};
    std::vector<glm::vec3> uavposes;
    std::vector<float> uavViewAzimuths;
    int n_pos{0};
    int k_pos{0};

    // Setting info
    // 设置信息：datestr / timestr 缺省 "000000"（istime 为真时使用 timestr 缺省值，见下方原注释）；
    // 以下由 Raytracing::uploadSetting 从 XML 填充。
    std::string datestr="000000";
    std::string timestr="000000";
    bool istime;  // if true using timestr default "000000"



    int n_wave;
    int n_angle;
    glm::ivec2 imageSize;
    int n_sample;
    int maxDepth;
    // periodicNeighborCount：周期边界的邻域遍历次数（0 表示不启用）；
    // skyboxEnabled：未命中的光线是否取天空盒；dispatchRowCount：Command::run 每次提交的图像行数（0 表示整幅）。
    int periodicNeighborCount{0};
    bool skyboxEnabled{false};
    int dispatchRowCount{0};
    // RNG seed (-> RayRTSetting::frame) and sampler (0 legacy PCG, 1 Owen-scrambled Sobol).
    int rtSeed{0};
    int rtSampler{kRaytracingDefaultSampler};
    // Variance-adaptive sampling (pilot pass + refinement pass, offline.rgen AdaptivePass).
    bool rtAdaptive{false};
    int adaptiveTile{1};   // HISTREAM_RT_ADAPTIVE_TILE (1 = per-pixel allocation; larger = coherent tiles)
    float adaptiveScoutFraction{0.25f}; // HISTREAM_RT_ADAPTIVE_SCOUT: scout samples as a fraction of the budget
    bool isTemperature;
    bool isDisplay;
    bool isImage;
    bool isAlbedo;

    bool isOrth;


    // setting：推送常量副本（updateSetting 写入）；下面三个缓冲分别存放波长、传感器、光源。
    RayRTSetting setting;
    std::shared_ptr<nvvk::Buffer> m_pBufferWave;
    std::shared_ptr<nvvk::Buffer> m_pBufferSensor;
    std::shared_ptr<nvvk::Buffer> m_pBufferLight;
    std::shared_ptr<AccelStruct>  m_pAccelStruct;

    // 物理纹理数据（由 buildPhysicalTextureFactors 生成，上传为两个 GPU 缓冲）：
    // Physical texture: per-instance offset into textureFactors (kNoTextureOffset = none);
    // factors are stored per (instance, triangle) and scale shortwave reflectance.
    std::vector<uint32_t> instanceTextureOffsets;
    std::vector<float> textureFactors;
    std::shared_ptr<nvvk::Buffer> m_pBufferTextureOffset;
    std::shared_ptr<nvvk::Buffer> m_pBufferTextureFactor;



    // initialization
    VkDevice                       m_device;
    VkPhysicalDevice               m_physicalDevice;
    VkInstance                     m_instance;
    std::shared_ptr<Allocator>     m_pAlloc;
    nvvk::DebugUtil                m_debug;
    std::vector<nvvk::Queue>       m_queues;
    VkQueue                        m_queue;
    uint32_t                       m_queueIndex{ 0 };
    VkFence m_fence;
    int m_currentSemaphore;
    std::vector<VkSemaphore> m_semaphores;
    int semaphoresNum{ 0 };


    nvvk::CommandPool   m_genCmdBuf;
    VkPipelineCache m_pipelineCache{ VK_NULL_HANDLE };
    nvvk::DescriptorSetBindings m_bindings;
    VkDescriptorPool m_descPool{ VK_NULL_HANDLE };
    VkDescriptorSet m_descSet{ VK_NULL_HANDLE };
    VkDescriptorSetLayout m_descSetLayout{ VK_NULL_HANDLE };
    VkPipelineLayout m_pipelineLayout{ VK_NULL_HANDLE };
    VkPipeline m_pipeline{ VK_NULL_HANDLE };



    // SBT：缺省使用 nvvk::SBTWrapper；useSBTWrapper=false 时用 Pipeline::createShaderBindingTable
    // 手工构建（m_rtSBTBuffer + 各区域）。missCount=2（普通 miss + 阴影 miss），hitCount=1。
    // useDeferred 为真时用 deferred operation 并行编译管线（缺省关闭）。
    bool useSBTWrapper{ true };
    bool useDeferred{ false };
    nvvk::SBTWrapper m_sbtWrapper;
    uint32_t missCount{ 2 };
    uint32_t hitCount{ 1 };
    nvvk::Buffer m_rtSBTBuffer;
    VkStridedDeviceAddressRegionKHR m_rgenRegion{};
    VkStridedDeviceAddressRegionKHR m_missRegion{};
    VkStridedDeviceAddressRegionKHR m_hitRegion{};
    VkStridedDeviceAddressRegionKHR m_callRegion{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_rtProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };

};


#endif //FIELD_RAYTRACINGIO_H
