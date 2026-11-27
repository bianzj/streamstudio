// ==========================================================================
// 文件：voxelebio.h
// 功能：VoxelEB 模式（体元能量平衡）的运行数据总容器 VoxelebIO
// 主要模块：
//   1. 系统与场景信息：目录、体元尺度/原点、步长、年/时区、初始土壤温度
//   2. 气象与几何信息：meteos、传感器/光源、角度、波长、无人机位姿、大气/气溶胶条件
//   3. 运行设置：时间节点、影像、过程开关、能量迭代与 Monin–Obukhov 稳定度诊断、船舶水线、
//      短波直散分离（diffuseFractionMode）、流体（fluid*）参数
//   4. GPU 缓冲：波长、大气、传感器、光源、气象、气溶胶、流体速度/标量/LBM 缓冲
//   5. Vulkan 句柄：设备、队列、围栏、描述符、各管线与布局、SBT 等
// 数据流：voxeleb.cpp 读入工程填充本类 → buffer/descriptor/pipeline/command 创建并使用 GPU 资源
// 与其他文件的关系：由 voxeleb.h/voxeleb.cpp 使用；子 IO 结构（meshio、voxelio 等）在 base/*.h
// 约定：长度 m、温度 K、辐照度 W m-2；voxelSize_XZY 的分量顺序为 X、Z、Y（待核对）；
//   浮点缺省 NaN 表示“未配置”（如 initialSoilTemperature）
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_VOXELEBIO_H
#define FIELD_VOXELEBIO_H

#include <vulkan/vulkan.hpp>
#include <limits>
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

// 类：VoxelEB 的数据容器（含 CPU 数据、GPU 缓冲句柄与 Vulkan 对象），各成员由 Voxeleb 各接口填充。
class VoxelebIO {
public:
VoxelebIO(){
    m_meshio = std::make_shared<MeshIO>();
    m_instanceio = std::make_shared<InstanceIO>();
    m_virtualio = std::make_shared<VirtualIO>();
    m_surfio = std::make_shared<SurfIO>();
    m_pAccelStruct = std::make_shared<AccelStruct>();  // method and resource, have to make
    m_defined = std::make_shared<DefinedIO>();
    m_voxelio = std::make_shared<VoxelIO>();
};

    // 模块 1：系统与场景信息。
    // system info
    std::string projectDir;
    std::string definedDir;
    TerrainEnvironment terrainEnvironment;
    glm::vec2 terrainAirHeightRange{0.0f, 0.0f};

    // Scene infor
    glm::vec3 sceneSize_XYZ;  // (5,5,0) with height = 0
    glm::ivec3 voxelSize_XZY;  // sceneSize_XYZ / stepsize; (5,5,0) with height = 0
    glm::vec3 sceneOrigin_XYZ;
    glm::vec3 voxelOrigin_XZY;
    float stepsize_surface;
    float stepsize_height;
    float stepsize_atmosphere;
    // glm::vec3 sMin;
    // glm::vec3 sMax;
    int m_year = 2019;
    float m_timezone = 8.0f;
    // Configured initial soil temperature Tsold [K]; NaN keeps the air temperature.
    float initialSoilTemperature = std::numeric_limits<float>::quiet_NaN();
    int n_modelmesh = 0;
    int n_instance = 0;
    int n_voxel = 0;
    int n_buffer = 0;
    int n_surface = 0;

    // 模块 2：气象信息（逐时间节点的气象序列 meteos 与当前节点 meteo）。
    //Meteo Info
    std::vector<Meteo> meteos;
    Meteo meteo;
    int startTimeNode;
    int endTimeNode;
    float lat;
    float lon;
   // MeteoMeta meta;

    // 模块 3：观测几何与大气条件（传感器、光源、角度、波长、UAV 位姿等）。
    //Geometry info
    SensorMatrix sensor;   // original sensor become the angular sensor and spectral wavesets
    LightSet     light;
    std::vector<Angle> angles;
    std::vector<float> waves;
    bool isUAVTrave{false};
    std::vector<glm::vec3> uavposes;
    std::vector<float> uavViewAzimuths;
    int n_pos{0};
    int k_pos{0};
    std::vector<AtomCond> atomconds;
    std::vector<AeroCond>  aeroconds;

    // 模块 4：运行设置与诊断量；n_pipeline 为管线个数（待核对）。
    // setting infor
    // Voxel transport options resolved from the project and environment (src/base/voxelflags.h).
    int voxelFlags = 0;
    int n_pipeline = 15;
    int n_wave;
    int n_angle;
    int n_node;
    int k_angle;
    int k_node=0;

    glm::ivec2 imageSize;
    int n_sample;
    int maxDepth;
    bool isTemperature;
    bool isDisplay;
    bool isImage;
    bool isProcess;
    bool isRadiationProcess;
    bool isEnergyProcess;
    bool isAlbedo;
    bool heterogeneousVoxel{false};
    int periodicNeighborCount{0};
    bool skyboxEnabled{false};
    int vegetationTemperatureMethod{0};
    int shortwaveScatteringOrders{1};
    // Final count is the number of conditional sunlit/shaded surface states
    // above the existing energy-residual threshold, not a voxel count.
    int energyIterations{0};
    uint32_t energyFinalUnconvergedStateCount{0};
    bool energyFinalized{false};
    // Monin-Obukhov stability iteration (control.stabilityCorrection) and
    // its per-node diagnostics: scene 1/L [m-1], ground-column mean u*
    // [m/s] and z-d [m], scene sensible heat per ground area [W/m2],
    // number of aerodynamic evaluations and the last |delta(z/L)|.
    bool stabilityCorrection{false};
    bool stabilityAvailable{false};
    double inverseObukhov{0.0};
    double frictionVelocity{0.0};
    double stabilityHeight{0.0};
    double stabilitySensibleHeat{0.0};
    int stabilityAeroUpdates{0};
    double stabilityChange{0.0};
    // Ship waterline contact (Ship material waterHeatTransferCoefficient > 0):
    // scene mean water temperature [K] of the last budget pass (0 = not yet
    // set), water voxel count of that pass, and the switch.
    bool shipWaterline{false};
    double shipWaterK{0.0};
    int shipWaterVoxels{0};
    std::shared_ptr<nvvk::Buffer> m_pScatteringStats;
    // Solar-visibility diagnostics of the current node (zero on skipped
    // night nodes) and the beam-normal irradiance that turns its
    // dimensionless transmittance residuals into W/m2 bounds.
    SolarVisibilityStats solarStats{};
    double solarBeamIrradiance{0.0};
    // light.diffuseFractionMode (shortwave_split.h): 0 fixed, 1 erbs. The
    // per-node split rescales the VNIR atomconds from the fraction-1 shapes
    // and updates LightSet.direct/diffuse; fixed mode never touches them.
    int diffuseFractionMode{0};
    float fixedDirectFraction{0.8f};
    float nodeDirectFraction{0.8f};
    std::vector<std::array<float, 2>> meteoMeasuredSplit;
    std::vector<AtomCond> unitShortwaveShapes;
    FluidXml fluid;
    glm::ivec3 fluidGridSize{1, 1, 1};
    uint64_t fluidCellCount{1};
    int fluidSubsteps{1};
    FluidParameters fluidParameters{};
    urban::Canyon urbanCanyon{};
    double urbanConvectiveVelocity{0.0}; // canyon convective velocity w* of the last pass, m s-1

    // 模块 5：子 IO 结构、推送常量设置与 GPU 缓冲句柄。
    std::shared_ptr<MeshIO> m_meshio;
    std::shared_ptr<InstanceIO> m_instanceio;
    std::shared_ptr<VirtualIO> m_virtualio;
    std::shared_ptr<VoxelIO> m_voxelio;
    std::shared_ptr<SurfIO> m_surfio;
    std::shared_ptr<DefinedIO> m_defined;

    VoxelLstSetting setting;
    // The general DMA allocator uses 32-bit suballocation sizes.
    std::unique_ptr<nvvk::ResourceAllocatorDedicated> m_radianceAllocator;

    std::shared_ptr<nvvk::Buffer> m_pBufferWave;
    std::shared_ptr<nvvk::Buffer> m_pBufferAtomcond;
    std::shared_ptr<nvvk::Buffer> m_pBufferSensor;
    std::shared_ptr<nvvk::Buffer> m_pBufferLight;
    std::shared_ptr<nvvk::Buffer> m_pMeteoBuffer;   // Meteo
    std::shared_ptr<AccelStruct>  m_pAccelStruct;
    std::shared_ptr<nvvk::Buffer> m_pBufferAero;
    std::shared_ptr<nvvk::Buffer> m_pFluidVelocityA;
    std::shared_ptr<nvvk::Buffer> m_pFluidVelocityB;
    std::shared_ptr<nvvk::Buffer> m_pFluidScalarA;
    std::shared_ptr<nvvk::Buffer> m_pFluidScalarB;
    std::shared_ptr<nvvk::Buffer> m_pFluidLbmA;
    std::shared_ptr<nvvk::Buffer> m_pFluidLbmB;
    std::shared_ptr<nvvk::Buffer> m_pFluidDensity;
    std::shared_ptr<nvvk::Buffer> m_pFluidMeta;
    std::shared_ptr<nvvk::Buffer> m_pFluidParameters;

    // 模块 6：Vulkan 设备/队列/同步对象。
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

    // 模块 7：create 阶段创建的命令池、描述符、管线布局、管线与光追 SBT 对象。
    // create
    nvvk::CommandPool   m_genCmdBuf;
    // VkCommandPool m_cmdPool{ VK_NULL_HANDLE };
    VkPipelineCache m_pipelineCache{ VK_NULL_HANDLE };
    nvvk::DescriptorSetBindings m_bindings;
    VkDescriptorPool m_descPool{ VK_NULL_HANDLE };
    VkDescriptorSet m_descSet{ VK_NULL_HANDLE };
    VkDescriptorSetLayout m_descSetLayout{ VK_NULL_HANDLE };

    VkPipelineLayout m_pipelineLayout_rt{ VK_NULL_HANDLE };
    VkPipelineLayout m_pipelineLayout_et{ VK_NULL_HANDLE };
    VkPipelineLayout m_pipelineLayout_eb{ VK_NULL_HANDLE };
    VkPipelineLayout m_pipelineLayout_aero{ VK_NULL_HANDLE };
    VkPipelineLayout m_pipelineLayout_bio{ VK_NULL_HANDLE };
    std::vector<VkPipeline> m_pipelines_rt;
    std::vector<VkPipeline> m_pipelines_et;
    std::vector<VkPipeline> m_pipelines_eb;

    std::map<VoxelEBStage, VkPipeline> m_pipelines;
    VkPipelineLayout m_pipelineLayout { VK_NULL_HANDLE };

    VkPipeline m_pipeline_aero;
    VkPipeline m_pipeline_bio;

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
