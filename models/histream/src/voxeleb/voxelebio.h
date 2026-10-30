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

    //Meteo Info
    std::vector<Meteo> meteos;
    Meteo meteo;
    int startTimeNode;
    int endTimeNode;
    float lat;
    float lon;
   // MeteoMeta meta;

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

    // setting infor
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
    std::shared_ptr<nvvk::Buffer> m_pScatteringStats;
    FluidXml fluid;
    glm::ivec3 fluidGridSize{1, 1, 1};
    uint64_t fluidCellCount{1};
    int fluidSubsteps{1};
    FluidParameters fluidParameters{};

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
