// ==========================================================================
// 文件：voxelrtio.h
// 功能：VoxelRT（体元辐射）模式的运行时数据容器 VoxelrtIO，集中保存场景尺寸、气象、几何、
//   运行设置、Vulkan 句柄、描述符与管线对象，在 Voxelrt 及其子模块之间共享。
// 主要模块：
//   1. Allocator 类型选择（DMA / VMA / 专用分配，由编译宏 ALLOC_DMA、ALLOC_VMA 决定）
//   2. class VoxelrtIO：
//      - 场景与体元网格尺寸（sceneSize_XYZ、voxelSize_XZY、stepsize_*）
//      - 气象 Meteo、传感器 SensorMatrix、光源 LightSet、观测角 angles、波长 waves
//      - 运行设置（波段/视角/航点数、采样数、碰撞阶数、周期边界、天空盒等开关）
//      - 各子 IO（mesh / instance / virtual / voxel / surf / defined）与 GPU 缓冲
//      - Vulkan 设备、队列、同步对象、描述符集与计算管线
// 数据流：voxelrt.cpp 的 uploadSetting 等函数填充本对象 → buffer/descriptor/pipeline 创建
//   GPU 对象 → command.cpp 使用它们派发计算着色器。
// 约定：voxelSize_XZY 的分量顺序为（x、竖直、y），其 .y 为竖直层数（见 voxelrt.cpp updateSetting，待核对）；
//   m_pipelineLayout_* / m_pipelines_rt/et/eb / m_pipeline_aero/bio 以及 SBT 相关成员沿用
//   其他模式，VoxelRT 只使用 m_pipelines 与 m_pipelineLayout。
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_VOXELRTIO_H
#define FIELD_VOXELRTIO_H

#include <vulkan/vulkan.hpp>
#include <nvvk/resourceallocator_vk.hpp>
#include <nvvk/context_vk.hpp>
#include <nvvk/commands_vk.hpp>
#include <nvvk/debug_util_vk.hpp>
#include <nvvk/descriptorsets_vk.hpp>
#include <nvvk/sbtwrapper_vk.hpp>

#include "src/base/accelstruct.h"
#include "src/base/meshio.h"
#include "src/base/instanceio.h"
#include "src/base/virtualio.h"
#include "src/base/fileio.h"
#include "src/base/surfio.h"
#include "src/base/voxelio.h"
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





// 模块：VoxelRT 运行时状态（纯数据 + 构造时创建各子 IO 对象），无业务逻辑。
class VoxelrtIO {
public:
VoxelrtIO(){
    m_meshio = std::make_shared<MeshIO>();
    m_instanceio = std::make_shared<InstanceIO>();
    m_virtualio = std::make_shared<VirtualIO>();
    m_surfio = std::make_shared<SurfIO>();
    m_pAccelStruct = std::make_shared<AccelStruct>();  // method and resource, have to make
    m_defined = std::make_shared<DefinedIO>();
    m_voxelio = std::make_shared<VoxelIO>();

};


    // 以下分组：工程路径、场景尺寸、气象、几何、设置、子 IO、Vulkan 初始化、
    // 描述符/管线、SBT。
    // system info
    std::string projectDir;
    std::string definedDir;

    // Scene infor
    glm::vec3 sceneSize_XYZ;  // (5,5,0) with height = 0
    glm::ivec3 voxelSize_XZY;  // sceneSize_XYZ / stepsize; (5,5,0) with height = 0
    glm::vec3 sceneOrigin_XYZ;
    glm::vec3 voxelOrigin_XZY;
    // 体元边长（m）：stepsize_surface 为水平/体元边长，stepsize_height 为竖直步长，
    // stepsize_atmosphere 为大气层步长（用途待核对）；体元化要求前两者相等（见 scene.cpp）。
    float stepsize_surface;
    float stepsize_height;
    float stepsize_atmosphere;
/*    glm::vec3 sMin;
    glm::vec3 sMax;*/
    int m_year = 2019;
    float m_timezone = 8.0f;
    int n_modelmesh = 0;
    int n_instance = 0;
    int n_voxel = 0;
    int n_buffer = 0;
    int n_surface = 0;

    bool isUAVTrave = false; // for the scene or for the UAV
    std::vector<glm::vec3> uavposes;
    std::vector<float> uavViewAzimuths;
    //Meteo Info

    std::vector<Meteo> meteos;
    Meteo meteo;
    float lat;
    float lon;
   // MeteoMeta meta;

    //Geometry info
    SensorMatrix sensor;   // original sensor become the angular sensor and spectral wavesets
    LightSet     light;
    std::vector<Angle> angles;
    std::vector<float> waves;


    // 运行设置：n_wave 波段数，n_angle 观测角数，n_pos UAV 航点数；
    // k_angle / k_pos / k_node 为当前循环序号（k_node = -1 表示无时间节点）。
    // setting infor
    int n_pipeline = 3;
    int n_wave;
    int n_angle;
    int n_pos;
    int n_node;
    int k_angle;
    int k_node=0;
    int k_pos;

    glm::ivec2 imageSize;
    int n_sample;
    int maxDepth;
    bool isTemperature;
    bool isDisplay;
    bool isImage;
    bool isRadiationProcess;
    bool isAlbedo;
    // 开关：heterogeneousVoxel 非均质体元；acceleratedRadiationSolver 加速求解器（多波段批处理）；
    // collisionOrders 体元间碰撞（散射）阶数；periodicNeighborCount 周期边界邻域数（0 = 关闭）；
    // skyboxEnabled 天空盒光照。
    bool heterogeneousVoxel{false};
    bool acceleratedRadiationSolver{false};
    int collisionOrders{2};
    int periodicNeighborCount{0};
    bool skyboxEnabled{false};


    std::shared_ptr<MeshIO> m_meshio;
    std::shared_ptr<InstanceIO> m_instanceio;
    std::shared_ptr<VirtualIO> m_virtualio;
    std::shared_ptr<VoxelIO> m_voxelio;
    std::shared_ptr<SurfIO> m_surfio;
    std::shared_ptr<DefinedIO> m_defined;

    // GPU 推送常量，由 Voxelrt::updateSetting 填写，command.cpp 每次派发前推送。
    VoxelRTSetting setting;

    std::shared_ptr<nvvk::Buffer> m_pBufferWave;
    std::shared_ptr<nvvk::Buffer> m_pBufferSensor;
    std::shared_ptr<nvvk::Buffer> m_pBufferLight;
    std::shared_ptr<AccelStruct>  m_pAccelStruct;


    //std::shared_ptr<

    // Vulkan 对象：设备、队列、栅栏/信号量。
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


    // create
    nvvk::CommandPool   m_genCmdBuf;
    // VkCommandPool m_cmdPool{ VK_NULL_HANDLE };
    VkPipelineCache m_pipelineCache{ VK_NULL_HANDLE };
    nvvk::DescriptorSetBindings m_bindings;
    VkDescriptorPool m_descPool{ VK_NULL_HANDLE };
    VkDescriptorSet m_descSet{ VK_NULL_HANDLE };
    VkDescriptorSetLayout m_descSetLayout{ VK_NULL_HANDLE };

    // 以下 *_rt/_et/_eb/_aero/_bio 管线句柄沿用其他模式，VoxelRT 并不使用（待核对）。
    VkPipelineLayout m_pipelineLayout_rt{ VK_NULL_HANDLE };
    VkPipelineLayout m_pipelineLayout_et{ VK_NULL_HANDLE };
    VkPipelineLayout m_pipelineLayout_eb{ VK_NULL_HANDLE };
    VkPipelineLayout m_pipelineLayout_aero{ VK_NULL_HANDLE };
    VkPipelineLayout m_pipelineLayout_bio{ VK_NULL_HANDLE };
    std::vector<VkPipeline> m_pipelines_rt;
    std::vector<VkPipeline> m_pipelines_et;
    std::vector<VkPipeline> m_pipelines_eb;

    // VoxelRT 实际使用的管线与布局：按阶段（gap / diffuse / out）索引，由 Pipeline::createPipeline 创建。
    std::map<VoxelRTStage, VkPipeline> m_pipelines;
    VkPipelineLayout m_pipelineLayout { VK_NULL_HANDLE };

    VkPipeline m_pipeline_aero;
    VkPipeline m_pipeline_bio;


    // 光追着色器绑定表（SBT）相关成员，同样沿用自光追模式；VoxelRT 计算管线不依赖。
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


#endif //FIELD_VOXELEBIO_H
