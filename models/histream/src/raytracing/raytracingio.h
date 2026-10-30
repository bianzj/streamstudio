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




// Must match NO_TEXTURE_OFFSET in offline.rgen.
constexpr uint32_t kNoTextureOffset = 0xFFFFFFFFu;

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
    std::string projectDir;
    std::string definedDir;



    // GPU buffer

   // std::vector<Angle>         m_pGeometry;


    std::shared_ptr<MeshIO> m_meshio;
    std::shared_ptr<InstanceIO> m_instanceio;
    std::shared_ptr<VirtualIO> m_virtualio;
    std::shared_ptr<DefinedIO> m_defined;


   int kangle = 0;
   //int n_wave = 0;


    // Scene info
    int n_modelmesh = 0;
    int n_instance = 0;
    glm::vec3 sceneSize{1.0f, 1.0f, 1.0f};
    glm::vec3 sceneOrigin{0.0f, 0.0f, 0.0f};
    glm::vec3 voxelSize;
    glm::vec3 voxelOrigin;
    float stepsize_surface;
/*    glm::vec3 sMin;
    glm::vec3 sMax;*/

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

    // Setting info
    std::string datestr="000000";
    std::string timestr="000000";
    bool istime;  // if true using timestr default "000000"



    int n_wave;
    int n_angle;
    glm::ivec2 imageSize;
    int n_sample;
    int maxDepth;
    int periodicNeighborCount{0};
    bool skyboxEnabled{false};
    int dispatchRowCount{0};
    bool isTemperature;
    bool isDisplay;
    bool isImage;
    bool isAlbedo;

    bool isOrth;


    RayRTSetting setting;
    std::shared_ptr<nvvk::Buffer> m_pBufferWave;
    std::shared_ptr<nvvk::Buffer> m_pBufferSensor;
    std::shared_ptr<nvvk::Buffer> m_pBufferLight;
    std::shared_ptr<AccelStruct>  m_pAccelStruct;

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
