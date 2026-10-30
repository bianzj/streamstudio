#include "facetrt_vulkan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

namespace facetvk {
namespace {

constexpr uint64_t kEmptyEdgeKey = std::numeric_limits<uint64_t>::max();

// Destroys a shader module on scope exit, including pipeline-creation failures.
struct ShaderModuleGuard {
    VkDevice device;
    VkShaderModule module;
    ~ShaderModuleGuard() { if (module != VK_NULL_HANDLE) vkDestroyShaderModule(device, module, nullptr); }
};

constexpr VkShaderStageFlags kVisibilityPushStages = VK_SHADER_STAGE_VERTEX_BIT |
                                                     VK_SHADER_STAGE_FRAGMENT_BIT |
                                                     VK_SHADER_STAGE_COMPUTE_BIT;
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                               VkDebugUtilsMessageTypeFlagsEXT,
                                               const VkDebugUtilsMessengerCallbackDataEXT* data,
                                               void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        // Validation output is intentionally concise; fatal API errors throw separately.
        std::fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

bool hasLayer(const char* name)
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(), [name](const VkLayerProperties& layer) {
        return std::strcmp(layer.layerName, name) == 0;
    });
}

float dot3(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> cross3(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}

std::array<float, 3> normalize3(std::array<float, 3> value)
{
    const float length = std::sqrt(dot3(value, value));
    if (!(length > 1.0e-8f)) {
        throw std::invalid_argument("A visibility direction has zero length");
    }
    for (float& component : value) {
        component /= length;
    }
    return value;
}

std::array<int, 2> periodicTileOffset(uint32_t index)
{
    static constexpr std::array<std::array<int, 2>, 20> offsets{{
        {{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}},
        {{-1, -1}}, {{-1, 1}}, {{1, -1}}, {{1, 1}},
        {{-2, 0}}, {{2, 0}}, {{0, -2}}, {{0, 2}},
        {{-2, -1}}, {{-2, 1}}, {{2, -1}}, {{2, 1}},
        {{-1, -2}}, {{1, -2}}, {{-1, 2}}, {{1, 2}}
    }};
    return index == 0U ? std::array<int, 2>{0, 0}
                       : offsets[std::min<uint32_t>(index - 1U, 19U)];
}

} // namespace

std::vector<DeviceCandidate> rankDevices(VkInstance instance)
{
    uint32_t deviceCount = 0;
    if (vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) != VK_SUCCESS)
        throw std::runtime_error("vkEnumeratePhysicalDevices failed");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()) != VK_SUCCESS)
        throw std::runtime_error("vkEnumeratePhysicalDevices failed");

    std::vector<DeviceCandidate> candidates;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        // The Facet CPU backend is the native raster path, not a CPU Vulkan driver.
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            continue;
        }
        VkPhysicalDeviceShaderAtomicInt64Features atomic64{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &atomic64;
        vkGetPhysicalDeviceFeatures2(device, &features);
        if (!features.features.shaderInt64 || !atomic64.shaderBufferInt64Atomics ||
            !features.features.fragmentStoresAndAtomics) {
            continue;
        }

        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueCount, queues.data());
        for (uint32_t family = 0; family < queueCount; ++family) {
            const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((queues[family].queueFlags & required) != required) {
                continue;
            }
            uint32_t score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000U : 100U;
            score += properties.limits.maxImageDimension2D / 1024U;
            candidates.push_back({device, family, score});
            break;
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const DeviceCandidate& a, const DeviceCandidate& b) { return a.score > b.score; });
    return candidates;
}

FacetrtVulkan::~FacetrtVulkan()
{
    destroy();
}

void FacetrtVulkan::require(VkResult result, const char* operation) const
{
    if (result != VK_SUCCESS) {
        std::ostringstream stream;
        stream << operation << " failed with VkResult " << static_cast<int>(result);
        throw std::runtime_error(stream.str());
    }
}

void FacetrtVulkan::initialize(const Config& config, const std::string& shaderDirectory)
{
    destroy();
    if (config.rasterWidth == 0 || config.rasterHeight == 0) {
        throw std::invalid_argument("Raster dimensions must be positive");
    }
    if (config.maxFragmentsPerPixel == 0 || config.maxFragmentsPerPixel > 64) {
        throw std::invalid_argument("maxFragmentsPerPixel must be in [1, 64]");
    }
    m_config = config;
    m_shaderDirectory = shaderDirectory;
    createContext();
    createCommandPool();
}

void FacetrtVulkan::createContext()
{
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    const bool useValidation = m_config.enableValidation && hasLayer(validationLayer);

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "facet-vulkan-radiosity";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "radiosity";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    std::vector<const char*> layers;
    std::vector<const char*> extensions;
    if (useValidation) {
        layers.push_back(validationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    require(vkCreateInstance(&instanceInfo, nullptr, &m_instance), "vkCreateInstance");

    if (useValidation) {
        VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugInfo.pfnUserCallback = debugCallback;
        const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger != nullptr) {
            require(createMessenger(m_instance, &debugInfo, nullptr, &m_debugMessenger),
                    "vkCreateDebugUtilsMessengerEXT");
        }
    }

    const std::vector<DeviceCandidate> candidates = rankDevices(m_instance);
    if (candidates.empty()) {
        throw std::runtime_error(
            "The GPU must provide a graphics+compute queue, shaderBufferInt64Atomics and fragmentStoresAndAtomics");
    }
    // DEFERRED[低] 错误处理: gpuIndex 越界时被静默钳到最后一块 GPU，用户配置错误不会被发现（可能落到集显上）；建议：越界时抛错或至少打印警告与实际选中的设备名。
    const size_t selected = std::min<size_t>(m_config.gpuIndex, candidates.size() - 1U);
    m_physicalDevice = candidates[selected].device;
    m_queueFamily = candidates[selected].queueFamily;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = m_queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceShaderAtomicInt64Features atomic64{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES};
    atomic64.shaderBufferInt64Atomics = VK_TRUE;
    VkPhysicalDeviceFeatures features{};
    features.shaderInt64 = VK_TRUE;
    features.fragmentStoresAndAtomics = VK_TRUE;
    VkPhysicalDeviceFeatures available{};
    vkGetPhysicalDeviceFeatures(m_physicalDevice, &available);
    m_shaderFloat64 = available.shaderFloat64 == VK_TRUE;
    features.shaderFloat64 = available.shaderFloat64;

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &atomic64;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.pEnabledFeatures = &features;
    require(vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device), "vkCreateDevice");
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
}

void FacetrtVulkan::createCommandPool()
{
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = m_queueFamily;
    require(vkCreateCommandPool(m_device, &info, nullptr, &m_commandPool),
            "vkCreateCommandPool");
}

uint32_t FacetrtVulkan::findMemoryType(uint32_t typeBits,
                                         VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
    for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
        if ((typeBits & (1U << index)) != 0U &&
            (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties) {
            return index;
        }
    }
    throw std::runtime_error("No compatible Vulkan memory type was found");
}

FacetrtVulkan::Buffer FacetrtVulkan::createBuffer(VkDeviceSize size,
                                                       VkBufferUsageFlags usage,
                                                       VkMemoryPropertyFlags properties) const
{
    Buffer result{};
    result.size = std::max<VkDeviceSize>(size, 4);
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = result.size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    require(vkCreateBuffer(m_device, &bufferInfo, nullptr, &result.buffer), "vkCreateBuffer");

    try {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, result.buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, properties);
        require(vkAllocateMemory(m_device, &allocation, nullptr, &result.memory), "vkAllocateMemory");
        require(vkBindBufferMemory(m_device, result.buffer, result.memory, 0), "vkBindBufferMemory");
    } catch (...) {
        destroyBuffer(result);
        throw;
    }
    return result;
}

VkCommandBuffer FacetrtVulkan::beginCommands() const
{
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = m_commandPool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    require(vkAllocateCommandBuffers(m_device, &allocation, &commandBuffer),
            "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkResult begun = vkBeginCommandBuffer(commandBuffer, &begin);
    if (begun != VK_SUCCESS) vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
    require(begun, "vkBeginCommandBuffer");
    return commandBuffer;
}

void FacetrtVulkan::endCommands(VkCommandBuffer commandBuffer) const
{
    VkResult result = vkEndCommandBuffer(commandBuffer);
    const char* stage = "vkEndCommandBuffer";
    VkFence fence{VK_NULL_HANDLE};
    bool pending = false;
    if (result == VK_SUCCESS) {
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(m_device, &fenceInfo, nullptr, &fence);
        stage = "vkCreateFence";
    }
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        result = vkQueueSubmit(m_queue, 1, &submit, fence);
        stage = "Vulkan command fence";
        if (result == VK_SUCCESS) {
            result = vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX);
            pending = result != VK_SUCCESS;
        }
    }
    if (fence != VK_NULL_HANDLE) vkDestroyFence(m_device, fence, nullptr);
    // A buffer still pending after a failed wait must not be freed.
    if (!pending) vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
    require(result, stage);
}

void FacetrtVulkan::copyBuffer(VkBuffer source, VkBuffer destination, VkDeviceSize size) const
{
    VkCommandBuffer commandBuffer = beginCommands();
    const VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(commandBuffer, source, destination, 1, &copy);
    endCommands(commandBuffer);
}

void FacetrtVulkan::uploadBuffer(Buffer& destination,
                                    const void* data,
                                    VkDeviceSize size) const
{
    if (size > destination.size) {
        throw std::out_of_range("Upload exceeds Vulkan buffer size");
    }
    // DEFERRED[低] 性能: 每次上传都 vkAllocateMemory 新 staging + 新建 fence 并同步等待；updateEnergyInputs 等在 EB 迭代中被反复调用，分配次数受 maxMemoryAllocationCount(常为 4096) 约束且开销大；建议：复用一个持久映射的 staging 环形缓冲。
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    try {
        void* mapped = nullptr;
        require(vkMapMemory(m_device, staging.memory, 0, size, 0, &mapped), "vkMapMemory");
        std::memcpy(mapped, data, static_cast<size_t>(size));
        vkUnmapMemory(m_device, staging.memory);
        copyBuffer(staging.buffer, destination.buffer, size);
    } catch (...) {
        destroyBuffer(staging);
        throw;
    }
    destroyBuffer(staging);
}

void FacetrtVulkan::downloadBuffer(const Buffer& source,
                                      void* data,
                                      VkDeviceSize size) const
{
    if (size > source.size) {
        throw std::out_of_range("Download exceeds Vulkan buffer size");
    }
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    try {
        copyBuffer(source.buffer, staging.buffer, size);
        void* mapped = nullptr;
        require(vkMapMemory(m_device, staging.memory, 0, size, 0, &mapped), "vkMapMemory");
        std::memcpy(data, mapped, static_cast<size_t>(size));
        vkUnmapMemory(m_device, staging.memory);
    } catch (...) {
        destroyBuffer(staging);
        throw;
    }
    destroyBuffer(staging);
}

void FacetrtVulkan::destroyBuffer(Buffer& buffer) const
{
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, buffer.memory, nullptr);
    }
    buffer = {};
}

VkShaderModule FacetrtVulkan::loadShader(const std::string& fileName) const
{
    const std::string path = m_shaderDirectory + "/" + fileName;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Cannot open SPIR-V shader: " + path);
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || (size % 4) != 0) {
        throw std::runtime_error("Invalid SPIR-V shader size: " + path);
    }
    file.seekg(0);
    std::vector<uint32_t> words(static_cast<size_t>(size) / sizeof(uint32_t));
    // DEFERRED[低] 错误处理: file.read 的结果未检查，读取不完整时把半截 SPIR-V 交给驱动；建议：检查 file.gcount()==size，并校验首字 0x07230203 魔数。
    file.read(reinterpret_cast<char*>(words.data()), size);

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = static_cast<size_t>(size);
    info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    require(vkCreateShaderModule(m_device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

uint32_t FacetrtVulkan::nextPowerOfTwo(uint32_t value) const
{
    if (value <= 1U) {
        return 1U;
    }
    --value;
    value |= value >> 1U;
    value |= value >> 2U;
    value |= value >> 4U;
    value |= value >> 8U;
    value |= value >> 16U;
    return value + 1U;
}

uint32_t FacetrtVulkan::maximumHashCapacity() const
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
    const uint64_t descriptorEntries =
        uint64_t(properties.limits.maxStorageBufferRange) / sizeof(uint64_t);
    if (descriptorEntries < 65536ULL) {
        throw std::runtime_error("GPU maxStorageBufferRange is too small for FacetRT");
    }
    uint32_t maximumCapacity = 1U;
    while (maximumCapacity <= (std::numeric_limits<uint32_t>::max() >> 1U) &&
           uint64_t(maximumCapacity << 1U) <= descriptorEntries) {
        maximumCapacity <<= 1U;
    }
    return maximumCapacity;
}

uint32_t FacetrtVulkan::chooseHashCapacity() const
{
    const uint32_t maximumCapacity = maximumHashCapacity();
    if (m_config.edgeHashCapacity != 0U) {
        const uint32_t capacity = nextPowerOfTwo(m_config.edgeHashCapacity);
        if (capacity < m_config.edgeHashCapacity || capacity > maximumCapacity) {
            throw std::overflow_error(
                "edgeHashCapacity exceeds GPU maxStorageBufferRange");
        }
        return capacity;
    }
    const uint64_t requested = std::max<uint64_t>(65536ULL,
                                                   uint64_t(surfaceCount()) * 64ULL);
    if (requested >= uint64_t(maximumCapacity)) {
        return maximumCapacity;
    }
    return nextPowerOfTwo(static_cast<uint32_t>(requested));
}

void FacetrtVulkan::validateGeometry(const std::vector<Vertex>& vertices) const
{
    if (vertices.empty() || (vertices.size() % 3U) != 0U) {
        throw std::invalid_argument("Geometry must be a non-empty triangle list");
    }
    for (const Vertex& vertex : vertices) {
        for (float coordinate : vertex.position) {
            if (!std::isfinite(coordinate)) {
                throw std::invalid_argument("Geometry contains a non-finite position");
            }
        }
        const float normalLength = std::sqrt(vertex.normal[0] * vertex.normal[0] +
                                             vertex.normal[1] * vertex.normal[1] +
                                             vertex.normal[2] * vertex.normal[2]);
        if (!(normalLength > 0.99f && normalLength < 1.01f)) {
            throw std::invalid_argument("Facet normals must be normalized");
        }
    }
}

void FacetrtVulkan::setGeometry(const std::vector<Vertex>& vertices)
{
    if (!initialized()) {
        throw std::logic_error("initialize() must be called before setGeometry()");
    }
    validateGeometry(vertices);
    require(vkDeviceWaitIdle(m_device), "vkDeviceWaitIdle");
    destroySolveResources();
    destroyVisibilityResources();

    m_facetCount = static_cast<uint32_t>(vertices.size() / 3U);
    m_geometry = vertices;
    m_boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    m_boundsMax = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest()};
    for (const Vertex& vertex : vertices) {
        for (size_t axis = 0; axis < 3; ++axis) {
            m_boundsMin[axis] = std::min(m_boundsMin[axis], vertex.position[axis]);
            m_boundsMax[axis] = std::max(m_boundsMax[axis], vertex.position[axis]);
        }
    }
    m_hashCapacity = chooseHashCapacity();
    createVisibilityResources();
    uploadBuffer(m_vertices, vertices.data(), vertices.size() * sizeof(Vertex));
    createVisibilityDescriptors();
    createVisibilityPipelines();
}

std::array<float, 16> FacetrtVulkan::makeProjection(const Direction& direction) const
{
    const std::array<float, 3> view = normalize3({direction.x, direction.y, direction.z});
    const std::array<float, 3> reference = std::abs(view[2]) < 0.9f
                                               ? std::array<float, 3>{0.0f, 0.0f, 1.0f}
                                               : std::array<float, 3>{0.0f, 1.0f, 0.0f};
    const std::array<float, 3> right = normalize3(cross3(reference, view));
    const std::array<float, 3> up = cross3(view, right);
    std::array<float, 3> extendedMin = m_boundsMin;
    std::array<float, 3> extendedMax = m_boundsMax;
    const uint32_t neighborCount = std::min(m_config.periodicNeighborCount, 20U);
    for (uint32_t tileIndex = 1U; tileIndex <= neighborCount; ++tileIndex) {
        const auto tile = periodicTileOffset(tileIndex);
        const float offsetX = static_cast<float>(tile[0]) * m_config.periodicSizeX;
        const float offsetZ = static_cast<float>(tile[1]) * m_config.periodicSizeZ;
        extendedMin[0] = std::min(extendedMin[0], m_boundsMin[0] + offsetX);
        extendedMax[0] = std::max(extendedMax[0], m_boundsMax[0] + offsetX);
        extendedMin[2] = std::min(extendedMin[2], m_boundsMin[2] + offsetZ);
        extendedMax[2] = std::max(extendedMax[2], m_boundsMax[2] + offsetZ);
    }
    const std::array<float, 3> center{
        (extendedMin[0] + extendedMax[0]) * 0.5f,
        (extendedMin[1] + extendedMax[1]) * 0.5f,
        (extendedMin[2] + extendedMax[2]) * 0.5f};
    const std::array<float, 3> diagonal{
        extendedMax[0] - extendedMin[0],
        extendedMax[1] - extendedMin[1],
        extendedMax[2] - extendedMin[2]};
    const float radius = std::max(0.5f * std::sqrt(dot3(diagonal, diagonal)) * 1.001f,
                                  1.0e-4f);

    std::array<float, 16> matrix{};
    matrix[0] = right[0] / radius;
    matrix[1] = up[0] / radius;
    matrix[2] = -view[0] / (2.0f * radius);
    matrix[4] = right[1] / radius;
    matrix[5] = up[1] / radius;
    matrix[6] = -view[1] / (2.0f * radius);
    matrix[8] = right[2] / radius;
    matrix[9] = up[2] / radius;
    matrix[10] = -view[2] / (2.0f * radius);
    matrix[12] = -dot3(center, right) / radius;
    matrix[13] = -dot3(center, up) / radius;
    matrix[14] = 0.5f + dot3(center, view) / (2.0f * radius);
    matrix[15] = 1.0f;
    return matrix;
}

void FacetrtVulkan::createVisibilityResources()
{
    const VkBufferUsageFlags deviceStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkMemoryPropertyFlags deviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const uint64_t pixelCount = uint64_t(m_config.rasterWidth) * m_config.rasterHeight;
    const uint64_t fragmentCount = pixelCount * m_config.maxFragmentsPerPixel;
    if (fragmentCount > std::numeric_limits<VkDeviceSize>::max() / 8ULL) {
        throw std::overflow_error("Visibility A-buffer size overflow");
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
    const auto requireStorageRange = [&](VkDeviceSize size, const char* name) {
        if (size > properties.limits.maxStorageBufferRange) {
            std::ostringstream stream;
            stream << name << " requires " << size
                   << " bytes, exceeding GPU maxStorageBufferRange "
                   << properties.limits.maxStorageBufferRange;
            throw std::runtime_error(stream.str());
        }
    };
    requireStorageRange(fragmentCount * 8ULL, "Visibility fragment buffer");
    requireStorageRange(VkDeviceSize(m_hashCapacity) * sizeof(uint64_t),
                        "Visibility edge-key buffer");
    requireStorageRange(VkDeviceSize(m_hashCapacity) * sizeof(uint32_t),
                        "Visibility edge-count buffer");

    m_vertices = createBuffer(VkDeviceSize(m_facetCount) * 3U * sizeof(Vertex),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              deviceLocal);
    m_fragments = createBuffer(fragmentCount * 8ULL, deviceStorage, deviceLocal);
    m_pixelCounts = createBuffer(pixelCount * sizeof(uint32_t), deviceStorage, deviceLocal);
    m_edgeKeys = createBuffer(VkDeviceSize(m_hashCapacity) * sizeof(uint64_t),
                              deviceStorage, deviceLocal);
    m_edgeCounts = createBuffer(VkDeviceSize(m_hashCapacity) * sizeof(uint32_t),
                                deviceStorage, deviceLocal);
    m_denominator = createBuffer(VkDeviceSize(surfaceCount()) * sizeof(uint32_t),
                                 deviceStorage, deviceLocal);
    m_skyCounts = createBuffer(VkDeviceSize(surfaceCount()) * sizeof(uint32_t),
                               deviceStorage, deviceLocal);
    m_visibilityStats = createBuffer(2U * sizeof(uint32_t), deviceStorage, deviceLocal);
    m_directTotal = createBuffer(VkDeviceSize(surfaceCount()) * sizeof(uint32_t),
                                 deviceStorage, deviceLocal);
    m_directVisible = createBuffer(VkDeviceSize(surfaceCount()) * sizeof(uint32_t),
                                   deviceStorage, deviceLocal);
}

void FacetrtVulkan::createVisibilityDescriptors()
{
    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    for (uint32_t index = 0; index < bindings.size(); ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    require(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr,
                                        &m_visibilitySetLayout),
            "vkCreateDescriptorSetLayout(visibility)");

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  static_cast<uint32_t>(bindings.size() * 2U)};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    require(vkCreateDescriptorPool(m_device, &poolInfo, nullptr,
                                   &m_visibilityDescriptorPool),
            "vkCreateDescriptorPool(visibility)");

    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    const std::array<VkDescriptorSetLayout, 2> layouts{m_visibilitySetLayout,
                                                       m_visibilitySetLayout};
    allocation.descriptorPool = m_visibilityDescriptorPool;
    allocation.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocation.pSetLayouts = layouts.data();
    std::array<VkDescriptorSet, 2> sets{};
    require(vkAllocateDescriptorSets(m_device, &allocation, sets.data()),
            "vkAllocateDescriptorSets(visibility)");
    m_visibilityDescriptorSet = sets[0];
    m_directDescriptorSet = sets[1];

    const std::array<Buffer*, 7> buffers{&m_fragments, &m_pixelCounts, &m_edgeKeys,
                                         &m_edgeCounts, &m_denominator, &m_skyCounts,
                                         &m_visibilityStats};
    std::array<VkDescriptorBufferInfo, 7> bufferInfos{};
    std::array<VkWriteDescriptorSet, 7> writes{};
    for (uint32_t index = 0; index < buffers.size(); ++index) {
        bufferInfos[index] = {buffers[index]->buffer, 0, buffers[index]->size};
        writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[index].dstSet = m_visibilityDescriptorSet;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index].pBufferInfo = &bufferInfos[index];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);

    const std::array<Buffer*, 7> directBuffers{&m_fragments, &m_pixelCounts, &m_edgeKeys,
                                               &m_edgeCounts, &m_directTotal,
                                               &m_directVisible, &m_visibilityStats};
    std::array<VkDescriptorBufferInfo, 7> directInfos{};
    std::array<VkWriteDescriptorSet, 7> directWrites{};
    for (uint32_t index = 0; index < directBuffers.size(); ++index) {
        directInfos[index] = {directBuffers[index]->buffer, 0, directBuffers[index]->size};
        directWrites[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        directWrites[index].dstSet = m_directDescriptorSet;
        directWrites[index].dstBinding = index;
        directWrites[index].descriptorCount = 1;
        directWrites[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        directWrites[index].pBufferInfo = &directInfos[index];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(directWrites.size()),
                           directWrites.data(), 0, nullptr);
}

void FacetrtVulkan::createVisibilityPipelines()
{
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(VisibilityPush);
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_visibilitySetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    require(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr,
                                   &m_visibilityPipelineLayout),
            "vkCreatePipelineLayout(visibility)");

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    require(vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass),
            "vkCreateRenderPass");

    VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebufferInfo.renderPass = m_renderPass;
    framebufferInfo.width = m_config.rasterWidth;
    framebufferInfo.height = m_config.rasterHeight;
    framebufferInfo.layers = 1;
    require(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffer),
            "vkCreateFramebuffer");

    VkShaderModule vertexModule = loadShader("visibility.vert.spv");
    const ShaderModuleGuard vertexGuard{m_device, vertexModule};
    VkShaderModule fragmentModule = loadShader("visibility.frag.spv");
    const ShaderModuleGuard fragmentGuard{m_device, fragmentModule};
    const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vertexModule, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModule, "main", nullptr}}};

    VkVertexInputBindingDescription vertexBinding{};
    vertexBinding.binding = 0;
    vertexBinding.stride = sizeof(Vertex);
    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 2> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)}}};
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &vertexBinding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,
                                                VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_visibilityPipelineLayout;
    pipelineInfo.renderPass = m_renderPass;
    pipelineInfo.subpass = 0;
    require(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                      nullptr, &m_rasterPipeline),
            "vkCreateGraphicsPipelines(visibility)");

    VkShaderModule resolveModule = loadShader("visibility_resolve.comp.spv");
    const ShaderModuleGuard resolveGuard{m_device, resolveModule};
    VkComputePipelineCreateInfo computeInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    computeInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    computeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    computeInfo.stage.module = resolveModule;
    computeInfo.stage.pName = "main";
    computeInfo.layout = m_visibilityPipelineLayout;
    require(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &computeInfo,
                                     nullptr, &m_resolvePipeline),
            "vkCreateComputePipelines(visibility resolve)");

    VkShaderModule directModule = loadShader("direct_resolve.comp.spv");
    const ShaderModuleGuard directGuard{m_device, directModule};
    computeInfo.stage.module = directModule;
    require(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &computeInfo,
                                     nullptr, &m_directResolvePipeline),
            "vkCreateComputePipelines(direct resolve)");
}

void FacetrtVulkan::createSolveResources(uint32_t directedEdgeCount)
{
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    m_rowOffsets = createBuffer(m_rowOffsetsCpu.size() * sizeof(uint32_t), usage, memory);
    m_edges = createBuffer(std::max<size_t>(directedEdgeCount, 1U) * sizeof(CsrEdge),
                           usage, memory);
    m_optics = createBuffer(VkDeviceSize(surfaceCount()) * sizeof(SurfaceOptics), usage, memory);
    m_radiosityA = createBuffer(VkDeviceSize(surfaceCount()) * sizeof(float), usage, memory);
    m_radiosityB = createBuffer(VkDeviceSize(surfaceCount()) * sizeof(float), usage, memory);
    m_residual = createBuffer(sizeof(uint32_t), usage, memory);
    m_incident = createBuffer(VkDeviceSize(surfaceCount()) * sizeof(float), usage, memory);

    uploadBuffer(m_rowOffsets, m_rowOffsetsCpu.data(),
                 m_rowOffsetsCpu.size() * sizeof(uint32_t));
    if (!m_edgesCpu.empty()) {
        uploadBuffer(m_edges, m_edgesCpu.data(), m_edgesCpu.size() * sizeof(CsrEdge));
    } else {
        const CsrEdge empty{};
        uploadBuffer(m_edges, &empty, sizeof(empty));
    }
}

void FacetrtVulkan::createSolveDescriptors()
{
    std::array<VkDescriptorSetLayoutBinding, 9> bindings{};
    for (uint32_t index = 0; index < bindings.size(); ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    require(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_solveSetLayout),
            "vkCreateDescriptorSetLayout(solve)");

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  static_cast<uint32_t>(bindings.size())};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    require(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_solveDescriptorPool),
            "vkCreateDescriptorPool(solve)");
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = m_solveDescriptorPool;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &m_solveSetLayout;
    require(vkAllocateDescriptorSets(m_device, &allocation, &m_solveDescriptorSet),
            "vkAllocateDescriptorSets(solve)");

    const std::array<Buffer*, 9> buffers{&m_rowOffsets, &m_edges, &m_denominator,
                                         &m_skyCounts, &m_optics, &m_radiosityA,
                                         &m_radiosityB, &m_residual, &m_incident};
    std::array<VkDescriptorBufferInfo, 9> infos{};
    std::array<VkWriteDescriptorSet, 9> writes{};
    for (uint32_t index = 0; index < buffers.size(); ++index) {
        infos[index] = {buffers[index]->buffer, 0, buffers[index]->size};
        writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[index].dstSet = m_solveDescriptorSet;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index].pBufferInfo = &infos[index];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
}

void FacetrtVulkan::createSolvePipeline()
{
    VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SolvePush)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_solveSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    require(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_solvePipelineLayout),
            "vkCreatePipelineLayout(solve)");

    VkShaderModule module = loadShader("radiosity.comp.spv");
    {
        const ShaderModuleGuard guard{m_device, module};
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.layout = m_solvePipelineLayout;
        require(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr,
                                         &m_solvePipeline),
                "vkCreateComputePipelines(radiosity)");
    }
    if (m_shaderFloat64) {
        {
            const VkShaderModule incidentModule = loadShader("incident.comp.spv");
            const ShaderModuleGuard guard{m_device, incidentModule};
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            info.stage.module = incidentModule;
            info.stage.pName = "main";
            info.layout = m_solvePipelineLayout;
            require(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr,
                                            &m_incidentPipeline), "vkCreateComputePipelines(incident)");
        }
        if (std::ifstream(m_shaderDirectory + "/energy.comp.spv").good()) createEnergyResources();
    }
}

GraphDiagnostics FacetrtVulkan::buildVisibilityGraph(
    const std::vector<Direction>& directions)
{
    for (;;) {
        try {
            return buildVisibilityGraphOnce(directions);
        } catch (const EdgeHashOverflow&) {
            // Distinct visible facet pairs exceeded the table: double it and rebuild.
            if (m_hashCapacity >= maximumHashCapacity()) throw;
            m_config.edgeHashCapacity = m_hashCapacity * 2U;
            const std::vector<Vertex> geometry = m_geometry;
            setGeometry(geometry);
        }
    }
}

GraphDiagnostics FacetrtVulkan::buildVisibilityGraphOnce(
    const std::vector<Direction>& directions)
{
    if (m_rasterPipeline == VK_NULL_HANDLE || m_facetCount == 0U) {
        throw std::logic_error("setGeometry() must be called before buildVisibilityGraph()");
    }
    if (directions.empty()) {
        throw std::invalid_argument("At least one visibility direction is required");
    }
    destroySolveResources();

    VisibilityPush push{};
    push.raster[0] = m_config.rasterWidth;
    push.raster[1] = m_config.rasterHeight;
    push.raster[2] = m_config.maxFragmentsPerPixel;
    push.raster[3] = m_hashCapacity;
    push.periodic[0] = m_config.periodicSizeX;
    push.periodic[1] = m_config.periodicSizeZ;
    push.periodic[2] = static_cast<float>(m_config.periodicNeighborCount);

    VkCommandBuffer commandBuffer = beginCommands();
    vkCmdFillBuffer(commandBuffer, m_edgeKeys.buffer, 0, m_edgeKeys.size, 0xffffffffU);
    vkCmdFillBuffer(commandBuffer, m_edgeCounts.buffer, 0, m_edgeCounts.size, 0U);
    vkCmdFillBuffer(commandBuffer, m_denominator.buffer, 0, m_denominator.size, 0U);
    vkCmdFillBuffer(commandBuffer, m_skyCounts.buffer, 0, m_skyCounts.size, 0U);
    vkCmdFillBuffer(commandBuffer, m_visibilityStats.buffer, 0, m_visibilityStats.size, 0U);

    VkMemoryBarrier transferToShader{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    transferToShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    transferToShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &transferToShader, 0, nullptr, 0, nullptr);
    endCommands(commandBuffer);

    const VkDeviceSize vertexOffset = 0;
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(m_config.rasterWidth),
                              static_cast<float>(m_config.rasterHeight), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {m_config.rasterWidth, m_config.rasterHeight}};
    const VkRenderPassBeginInfo renderBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                             nullptr,
                                             m_renderPass,
                                             m_framebuffer,
                                             scissor,
                                             0,
                                             nullptr};

    // DEFERRED[低] 性能: 每个可见性方向单独 beginCommands/endCommands（每次新建 fence 并阻塞 vkWaitForFences），方向数大时 CPU-GPU 往返成为瓶颈；建议：把多个方向录制进同一命令缓冲（方向间已有 resolveToNext 屏障），批量提交。
    for (const Direction& inputDirection : directions) {
        commandBuffer = beginCommands();
        // Vulkan bindings do not survive command-buffer boundaries.
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertices.buffer, &vertexOffset);
        const std::array<float, 3> direction =
            normalize3({inputDirection.x, inputDirection.y, inputDirection.z});
        const std::array<float, 16> projection = makeProjection(inputDirection);
        std::copy(projection.begin(), projection.end(), push.viewProjection);
        push.viewDirection[0] = direction[0];
        push.viewDirection[1] = direction[1];
        push.viewDirection[2] = direction[2];
        push.viewDirection[3] = 0.0f;

        vkCmdFillBuffer(commandBuffer, m_pixelCounts.buffer, 0, m_pixelCounts.size, 0U);
        VkMemoryBarrier countClear{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        countClear.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        countClear.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             1, &countClear, 0, nullptr, 0, nullptr);

        vkCmdBeginRenderPass(commandBuffer, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_rasterPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_visibilityPipelineLayout, 0, 1,
                                &m_visibilityDescriptorSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_visibilityPipelineLayout,
                           kVisibilityPushStages,
                           0, sizeof(push), &push);
        vkCmdDraw(commandBuffer, m_facetCount * 3U,
                  std::min(m_config.periodicNeighborCount, 20U) + 1U,
                  0, 0);
        vkCmdEndRenderPass(commandBuffer);

        VkMemoryBarrier rasterToResolve{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        rasterToResolve.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        rasterToResolve.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             1, &rasterToResolve, 0, nullptr, 0, nullptr);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_resolvePipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_visibilityPipelineLayout, 0, 1,
                                &m_visibilityDescriptorSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_visibilityPipelineLayout,
                           kVisibilityPushStages, 0, sizeof(push), &push);
        const uint64_t pixels = uint64_t(m_config.rasterWidth) * m_config.rasterHeight;
        vkCmdDispatch(commandBuffer, static_cast<uint32_t>((pixels + 63ULL) / 64ULL), 1, 1);

        VkMemoryBarrier resolveToNext{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        resolveToNext.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        // DEFERRED[低] GPU同步: dstAccessMask 只有 TRANSFER_WRITE，缺 TRANSFER_READ；循环结束后 downloadBuffer 用 vkCmdCopyBuffer 读取 m_edgeKeys/m_edgeCounts/m_denominator 时，resolve 计算写入对 transfer 读不保证可见；建议：dstAccessMask 加 VK_ACCESS_TRANSFER_READ_BIT。
        resolveToNext.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
                                      VK_ACCESS_SHADER_READ_BIT |
                                      VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &resolveToNext, 0, nullptr, 0, nullptr);
        endCommands(commandBuffer);
    }

    // DEFERRED[中] 内存: 整张边哈希表（容量可达 maxStorageBufferRange/8，即最多 ~512M 项 → 4 GiB 键 + 2 GiB 计数）一次性回读到主机，另外 downloadBuffer 还要分配同样大小的 host-visible staging，容易耗尽内存；建议：在 GPU 上先压缩非空槽（prefix-sum/compaction）再回读，或分块回读。
    std::vector<uint64_t> edgeKeys(m_hashCapacity);
    std::vector<uint32_t> edgeCounts(m_hashCapacity);
    std::array<uint32_t, 2> statistics{};
    m_denominatorCpu.resize(surfaceCount());
    m_skyCountsCpu.resize(surfaceCount());
    downloadBuffer(m_edgeKeys, edgeKeys.data(), edgeKeys.size() * sizeof(uint64_t));
    downloadBuffer(m_edgeCounts, edgeCounts.data(), edgeCounts.size() * sizeof(uint32_t));
    downloadBuffer(m_denominator, m_denominatorCpu.data(),
                   m_denominatorCpu.size() * sizeof(uint32_t));
    downloadBuffer(m_skyCounts, m_skyCountsCpu.data(),
                   m_skyCountsCpu.size() * sizeof(uint32_t));
    downloadBuffer(m_visibilityStats, statistics.data(), sizeof(statistics));

    GraphDiagnostics diagnostics{};
    diagnostics.facetCount = m_facetCount;
    diagnostics.surfaceCount = surfaceCount();
    diagnostics.fragmentOverflow = statistics[0];
    diagnostics.hashOverflow = statistics[1];
    if (diagnostics.fragmentOverflow != 0U) {
        throw std::runtime_error(
            "Visibility A-buffer overflow; increase Config::maxFragmentsPerPixel");
    }
    if (diagnostics.hashOverflow != 0U) {
        throw EdgeHashOverflow();
    }

    std::vector<std::pair<uint64_t, uint32_t>> undirectedEdges;
    undirectedEdges.reserve(std::min<uint32_t>(m_hashCapacity / 16U, 1U << 20U));
    for (uint32_t slot = 0; slot < m_hashCapacity; ++slot) {
        if (edgeKeys[slot] == kEmptyEdgeKey || edgeCounts[slot] == 0U) {
            continue;
        }
        const uint32_t sideA = static_cast<uint32_t>(edgeKeys[slot] >> 32U);
        const uint32_t sideB = static_cast<uint32_t>(edgeKeys[slot]);
        if (sideA >= surfaceCount() || sideB >= surfaceCount()) {
            throw std::runtime_error("GPU visibility hash contains an invalid surface index");
        }
        undirectedEdges.emplace_back(edgeKeys[slot], edgeCounts[slot]);
    }

    m_rowOffsetsCpu.assign(surfaceCount() + 1U, 0U);
    for (const auto& entry : undirectedEdges) {
        const uint32_t sideA = static_cast<uint32_t>(entry.first >> 32U);
        const uint32_t sideB = static_cast<uint32_t>(entry.first);
        ++m_rowOffsetsCpu[sideA + 1U];
        ++m_rowOffsetsCpu[sideB + 1U];
    }
    std::partial_sum(m_rowOffsetsCpu.begin(), m_rowOffsetsCpu.end(),
                     m_rowOffsetsCpu.begin());
    m_edgesCpu.assign(m_rowOffsetsCpu.back(), {});
    std::vector<uint32_t> cursor = m_rowOffsetsCpu;
    for (const auto& entry : undirectedEdges) {
        const uint32_t sideA = static_cast<uint32_t>(entry.first >> 32U);
        const uint32_t sideB = static_cast<uint32_t>(entry.first);
        const uint32_t count = entry.second;
        m_edgesCpu[cursor[sideA]++] = {sideB, count};
        m_edgesCpu[cursor[sideB]++] = {sideA, count};
    }
    diagnostics.directedEdgeCount = static_cast<uint32_t>(m_edgesCpu.size());
    if (std::none_of(m_denominatorCpu.begin(), m_denominatorCpu.end(), [](uint32_t n){return n>0;}))
        throw std::runtime_error("Visibility raster has no surface samples; radiation results are invalid");

    float maxClosureError = 0.0f;
    for (uint32_t side = 0; side < surfaceCount(); ++side) {
        uint64_t visibleCount = m_skyCountsCpu[side];
        for (uint32_t index = m_rowOffsetsCpu[side];
             index < m_rowOffsetsCpu[side + 1U]; ++index) {
            visibleCount += m_edgesCpu[index].count;
        }
        // DEFERRED[中] 物理: 没有任何栅格样本的面（亚像素小面元）被直接跳过，不计入闭合误差；而 incidentIrradiance/incident.comp 中此类面直接取 skyRadiosity（视为全开阔天空），对冠层内部被遮挡的小面元会系统性高估入射辐射；建议：统计并报告 denominator==0 的面元数，对其用邻近面/抖动多采样估计，而不是默认全天空。
        if (m_denominatorCpu[side] == 0U) {
            continue;
        }
        const float closure = static_cast<float>(visibleCount) /
                              static_cast<float>(m_denominatorCpu[side]);
        maxClosureError = std::max(maxClosureError, std::abs(closure - 1.0f));
    }
    diagnostics.maxClosureError = maxClosureError;

    createSolveResources(diagnostics.directedEdgeCount);
    createSolveDescriptors();
    createSolvePipeline();
    return diagnostics;
}

std::vector<float> FacetrtVulkan::computeSunlitFraction(const Direction& sunDirection)
{
    if (m_directResolvePipeline == VK_NULL_HANDLE || m_facetCount == 0U) {
        throw std::logic_error("setGeometry() must be called before computeSunlitFraction()");
    }
    const std::array<float, 3> direction =
        normalize3({sunDirection.x, sunDirection.y, sunDirection.z});
    const std::array<float, 16> projection = makeProjection(sunDirection);
    VisibilityPush push{};
    std::copy(projection.begin(), projection.end(), push.viewProjection);
    push.viewDirection[0] = direction[0];
    push.viewDirection[1] = direction[1];
    push.viewDirection[2] = direction[2];
    push.raster[0] = m_config.rasterWidth;
    push.raster[1] = m_config.rasterHeight;
    push.raster[2] = m_config.maxFragmentsPerPixel;
    push.raster[3] = m_hashCapacity;
    push.periodic[0] = m_config.periodicSizeX;
    push.periodic[1] = m_config.periodicSizeZ;
    push.periodic[2] = static_cast<float>(m_config.periodicNeighborCount);

    VkCommandBuffer commandBuffer = beginCommands();
    vkCmdFillBuffer(commandBuffer, m_directTotal.buffer, 0, m_directTotal.size, 0U);
    vkCmdFillBuffer(commandBuffer, m_directVisible.buffer, 0, m_directVisible.size, 0U);
    vkCmdFillBuffer(commandBuffer, m_visibilityStats.buffer, 0, m_visibilityStats.size, 0U);

    const VkDeviceSize vertexOffset = 0;
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(m_config.rasterWidth),
                              static_cast<float>(m_config.rasterHeight), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {m_config.rasterWidth, m_config.rasterHeight}};
    const uint64_t pixels = uint64_t(m_config.rasterWidth) * m_config.rasterHeight;
    // Facets smaller than a pixel are sampled with probability ~ their area,
    // and an unsampled facet has no sunlit estimate. Stratified sub-pixel
    // jitter multiplies the samples by kSunJitter^2.
    constexpr uint32_t kSunJitter = 4U;
    for (uint32_t jitter = 0; jitter < kSunJitter * kSunJitter; ++jitter) {
        const float offsetX = ((static_cast<float>(jitter % kSunJitter) + 0.5f) /
            static_cast<float>(kSunJitter) - 0.5f) * 2.0f / static_cast<float>(m_config.rasterWidth);
        const float offsetY = ((static_cast<float>(jitter / kSunJitter) + 0.5f) /
            static_cast<float>(kSunJitter) - 0.5f) * 2.0f / static_cast<float>(m_config.rasterHeight);
        push.viewProjection[12] = projection[12] + offsetX * projection[15];
        push.viewProjection[13] = projection[13] + offsetY * projection[15];

        VkMemoryBarrier clearBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        clearBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                     VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             1, &clearBarrier, 0, nullptr, 0, nullptr);
        vkCmdFillBuffer(commandBuffer, m_pixelCounts.buffer, 0, m_pixelCounts.size, 0U);
        VkMemoryBarrier fillBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fillBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &fillBarrier, 0, nullptr, 0, nullptr);

        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertices.buffer, &vertexOffset);
        VkRenderPassBeginInfo renderBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        renderBegin.renderPass = m_renderPass;
        renderBegin.framebuffer = m_framebuffer;
        renderBegin.renderArea = scissor;
        vkCmdBeginRenderPass(commandBuffer, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_rasterPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_visibilityPipelineLayout, 0, 1,
                                &m_directDescriptorSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_visibilityPipelineLayout,
                           kVisibilityPushStages,
                           0, sizeof(push), &push);
        vkCmdDraw(commandBuffer, m_facetCount * 3U,
                  std::min(m_config.periodicNeighborCount, 20U) + 1U,
                  0, 0);
        vkCmdEndRenderPass(commandBuffer);

        VkMemoryBarrier rasterToResolve{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        rasterToResolve.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        rasterToResolve.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             1, &rasterToResolve, 0, nullptr, 0, nullptr);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_directResolvePipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_visibilityPipelineLayout, 0, 1,
                                &m_directDescriptorSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_visibilityPipelineLayout,
                           kVisibilityPushStages, 0, sizeof(push), &push);
        vkCmdDispatch(commandBuffer, static_cast<uint32_t>((pixels + 63ULL) / 64ULL), 1, 1);
    }
    // DEFERRED[中] GPU同步: 最后一次 direct_resolve 计算写入 m_directTotal/m_directVisible 后没有任何 COMPUTE→TRANSFER_READ 屏障，随后 downloadBuffer 的 vkCmdCopyBuffer 读取属于跨提交但无内存依赖（fence 只保证 host 侧可见，不保证后续 transfer 读可见）；建议：endCommands 前补一个 computeBarrier(commandBuffer) 或 srcAccess=SHADER_WRITE/dstAccess=TRANSFER_READ 的屏障。
    endCommands(commandBuffer);

    std::vector<uint32_t> total(surfaceCount());
    std::vector<uint32_t> visible(surfaceCount());
    std::array<uint32_t, 2> statistics{};
    downloadBuffer(m_directTotal, total.data(), total.size() * sizeof(uint32_t));
    downloadBuffer(m_directVisible, visible.data(), visible.size() * sizeof(uint32_t));
    downloadBuffer(m_visibilityStats, statistics.data(), sizeof(statistics));
    if (statistics[0] != 0U) {
        throw std::runtime_error(
            "Direct-light A-buffer overflow; increase Config::maxFragmentsPerPixel");
    }

    // NaN marks a side with no sample; the caller estimates it from neighbours.
    std::vector<float> fraction(surfaceCount(), std::numeric_limits<float>::quiet_NaN());
    for (uint32_t side = 0; side < surfaceCount(); ++side) {
        if (total[side] != 0U) {
            fraction[side] = static_cast<float>(visible[side]) /
                             static_cast<float>(total[side]);
        }
    }
    return fraction;
}

void FacetrtVulkan::validateOptics(const std::vector<SurfaceOptics>& optics) const
{
    if (optics.size() != surfaceCount()) {
        throw std::invalid_argument("Optics array must contain exactly two sides per facet");
    }
    for (const SurfaceOptics& value : optics) {
        if (!std::isfinite(value.reflectance) || !std::isfinite(value.transmittance) ||
            !std::isfinite(value.emission) || !std::isfinite(value.directIrradiance) ||
            value.reflectance < 0.0f || value.transmittance < 0.0f ||
            value.emission < 0.0f || value.directIrradiance < 0.0f ||
            value.reflectance + value.transmittance > 1.0001f) {
            throw std::invalid_argument(
                "Optical values must be finite, non-negative, and reflectance+transmittance<=1");
        }
    }
}

uint32_t FacetrtVulkan::recordSolve(VkCommandBuffer commandBuffer, float skyRadiosity, uint32_t iterations,
                                    float relaxation)
{
    vkCmdFillBuffer(commandBuffer, m_radiosityA.buffer, 0, m_radiosityA.size, 0U);
    vkCmdFillBuffer(commandBuffer, m_radiosityB.buffer, 0, m_radiosityB.size, 0U);
    vkCmdFillBuffer(commandBuffer, m_residual.buffer, 0, m_residual.size, 0U);
    VkMemoryBarrier clearBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &clearBarrier, 0, nullptr, 0, nullptr);

    const uint32_t ping = recordIterations(commandBuffer, 0U, skyRadiosity, iterations, relaxation);
    recordIncident(commandBuffer, ping, skyRadiosity);
    return ping;
}

uint32_t FacetrtVulkan::recordIterations(VkCommandBuffer commandBuffer, uint32_t ping, float skyRadiosity,
                                        uint32_t iterations, float relaxation, bool indirect)
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_solvePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_solvePipelineLayout, 0, 1,
                            &m_solveDescriptorSet, 0, nullptr);
    for (uint32_t iteration = 0; iteration < iterations; ++iteration)
    {
        vkCmdFillBuffer(commandBuffer, m_residual.buffer, 0, sizeof(uint32_t), 0U);
        VkMemoryBarrier residualClear{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        residualClear.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        residualClear.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &residualClear, 0, nullptr, 0, nullptr);

        const SolvePush push{m_facetCount, ping, skyRadiosity, relaxation};
        vkCmdPushConstants(commandBuffer, m_solvePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        if (indirect) vkCmdDispatchIndirect(commandBuffer, m_spectralDispatch.buffer, 0);
        else vkCmdDispatch(commandBuffer, (m_facetCount + 63U) / 64U, 1, 1);

        VkMemoryBarrier iterationBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        iterationBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        iterationBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                             &iterationBarrier, 0, nullptr, 0, nullptr);
        ping ^= 1U;
    }
    return ping;
}

void FacetrtVulkan::computeBarrier(VkCommandBuffer commandBuffer) const
{
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
        VK_ACCESS_HOST_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void FacetrtVulkan::recordIncident(VkCommandBuffer commandBuffer, uint32_t ping, float skyRadiosity)
{
    if (m_incidentPipeline == VK_NULL_HANDLE)
        return;
    computeBarrier(commandBuffer);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_incidentPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_solvePipelineLayout, 0, 1,
                            &m_solveDescriptorSet, 0, nullptr);
    const SolvePush push{m_facetCount, ping, skyRadiosity, 1.0f};
    vkCmdPushConstants(commandBuffer, m_solvePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commandBuffer, (surfaceCount() + 63U) / 64U, 1, 1);
    computeBarrier(commandBuffer);
}

void FacetrtVulkan::createEnergyResources()
{
    static_assert(sizeof(EnergyPush) == 48);
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const auto device = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkDeviceSize states = VkDeviceSize(surfaceCount()) * 2;
    m_energyParameters = createBuffer(VkDeviceSize(m_facetCount) * 32 * sizeof(float), usage, device);
    m_energyInputs = createBuffer(states * 12 * sizeof(float), usage, device);
    m_energyTemperatures = createBuffer(states * sizeof(float), usage, device);
    m_energyFluxes = createBuffer(states * 12 * sizeof(float), usage, device);
    m_energyStats = createBuffer(6 * sizeof(uint32_t), usage,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i)
    {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = static_cast<uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    require(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_energySetLayout), "energy descriptor layout");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    require(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_energyDescriptorPool), "energy pool");
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = m_energyDescriptorPool;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &m_energySetLayout;
    require(vkAllocateDescriptorSets(m_device, &allocation, &m_energyDescriptorSet), "energy descriptor set");
    const std::array<Buffer *, 7> buffers{&m_energyParameters, &m_energyInputs, &m_energyTemperatures, &m_energyFluxes,
                                          &m_optics,           &m_incident,     &m_energyStats};
    std::array<VkDescriptorBufferInfo, 7> infos{};
    std::array<VkWriteDescriptorSet, 7> writes{};
    for (uint32_t i = 0; i < buffers.size(); ++i)
    {
        infos[i] = {buffers[i]->buffer, 0, buffers[i]->size};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = m_energyDescriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, 7, writes.data(), 0, nullptr);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(EnergyPush)};
    VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayout.setLayoutCount = 1;
    pipelineLayout.pSetLayouts = &m_energySetLayout;
    pipelineLayout.pushConstantRangeCount = 1;
    pipelineLayout.pPushConstantRanges = &range;
    require(vkCreatePipelineLayout(m_device, &pipelineLayout, nullptr, &m_energyPipelineLayout),
            "energy pipeline layout");
    const VkShaderModule module = loadShader("energy.comp.spv");
    const ShaderModuleGuard guard{m_device, module};
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = module;
    pipeline.stage.pName = "main";
    pipeline.layout = m_energyPipelineLayout;
    require(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_energyPipeline),
            "energy pipeline");
}

void FacetrtVulkan::destroyEnergyResources()
{
    if (m_energyPipeline)
        vkDestroyPipeline(m_device, m_energyPipeline, nullptr);
    if (m_energyPipelineLayout)
        vkDestroyPipelineLayout(m_device, m_energyPipelineLayout, nullptr);
    if (m_energyDescriptorPool)
        vkDestroyDescriptorPool(m_device, m_energyDescriptorPool, nullptr);
    if (m_energySetLayout)
        vkDestroyDescriptorSetLayout(m_device, m_energySetLayout, nullptr);
    m_energyPipeline = VK_NULL_HANDLE;
    m_energyPipelineLayout = VK_NULL_HANDLE;
    m_energyDescriptorPool = VK_NULL_HANDLE;
    m_energySetLayout = VK_NULL_HANDLE;
    m_energyDescriptorSet = VK_NULL_HANDLE;
    destroyBuffer(m_energyParameters);
    destroyBuffer(m_energyInputs);
    destroyBuffer(m_energyTemperatures);
    destroyBuffer(m_energyFluxes);
    destroyBuffer(m_energyStats);
}

void FacetrtVulkan::beginEnergyNode(const EnergyPush &push, const std::vector<float> &parameters,
                                    const std::vector<float> &inputs, const std::vector<float> &temperatures)
{
    if (!supportsGpuEnergy() || parameters.size() != size_t(m_facetCount) * 32 ||
        inputs.size() != size_t(surfaceCount()) * 2 * 12 || temperatures.size() != size_t(surfaceCount()) * 2 ||
        push.facets != m_facetCount || push.objects > m_facetCount || push.method > 1 ||
        !std::isfinite(push.air + push.rho + push.pressure + push.vapor + push.dt + push.co2 + push.relaxation +
                       push.wind) ||
        push.dt <= 0 || push.pressure <= 0 || push.relaxation <= 0)
        throw std::invalid_argument("Invalid GPU energy node inputs");
    for (const auto *values : {&parameters, &inputs, &temperatures})
        for (float value : *values)
            if (!std::isfinite(value))
                throw std::invalid_argument("Nonfinite GPU energy inputs");
    m_energyPush = push;
    uploadBuffer(m_energyParameters, parameters.data(), parameters.size() * sizeof(float));
    updateEnergyInputs(inputs);
    uploadBuffer(m_energyTemperatures, temperatures.data(), temperatures.size() * sizeof(float));
}
void FacetrtVulkan::updateEnergyInputs(const std::vector<float> &inputs)
{
    if (inputs.size() != size_t(surfaceCount()) * 2 * 12)
        throw std::invalid_argument("GPU energy input dimensions");
    uploadBuffer(m_energyInputs, inputs.data(), inputs.size() * sizeof(float));
}
void FacetrtVulkan::recordEnergy(VkCommandBuffer commandBuffer, uint32_t phase)
{
    computeBarrier(commandBuffer);
    m_energyPush.phase = phase;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_energyPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_energyPipelineLayout, 0, 1,
                            &m_energyDescriptorSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_energyPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(EnergyPush),
                       &m_energyPush);
    vkCmdDispatch(commandBuffer, (m_facetCount + 63U) / 64U, 1, 1);
    computeBarrier(commandBuffer);
}
std::array<float, 5> FacetrtVulkan::readEnergyStatistics()
{
    void *mapped = nullptr;
    require(vkMapMemory(m_device, m_energyStats.memory, 0, m_energyStats.size, 0, &mapped), "energy statistics map");
    std::array<uint32_t, 6> bits{};
    std::memcpy(bits.data(), mapped, sizeof(bits));
    vkUnmapMemory(m_device, m_energyStats.memory);
    if (bits[5])
        throw std::runtime_error("GPU energy calculation produced a nonfinite state or invalid physiology");
    std::array<float, 5> values{};
    std::memcpy(values.data(), bits.data(), sizeof(values));
    return values;
}
std::array<float, 5> FacetrtVulkan::evaluateEnergy(float skyRadiosity)
{
    VkCommandBuffer cmd = beginCommands();
    vkCmdFillBuffer(cmd, m_energyStats.buffer, 0, m_energyStats.size, 0);
    recordEnergy(cmd, 2);
    // DEFERRED[中] 数值: 能量平衡每次评估都从零辐射度冷启动、固定 64 次 Jacobi 且不检查残差；对 r+t 接近 1 的密集冠层（近红外/长波多次散射）64 次可能未收敛，导致吸收辐射偏低且无任何告警；建议：用上一次辐射度热启动，并读取 m_residual 与容差比较，不收敛时告警或继续迭代。
    recordSolve(cmd, skyRadiosity, 64, 1.0f);
    recordEnergy(cmd, 0);
    endCommands(cmd);
    return readEnergyStatistics();
}
float FacetrtVulkan::updateEnergyTemperature()
{
    VkCommandBuffer cmd = beginCommands();
    vkCmdFillBuffer(cmd, m_energyStats.buffer, 0, m_energyStats.size, 0);
    recordEnergy(cmd, 1);
    endCommands(cmd);
    return readEnergyStatistics()[4];
}
EnergySnapshot FacetrtVulkan::readEnergyState(bool includeRadiation)
{
    EnergySnapshot result;
    result.temperatures.resize(size_t(surfaceCount()) * 2);
    result.fluxes.resize(size_t(surfaceCount()) * 2 * 12);
    downloadBuffer(m_energyTemperatures, result.temperatures.data(), result.temperatures.size() * sizeof(float));
    downloadBuffer(m_energyFluxes, result.fluxes.data(), result.fluxes.size() * sizeof(float));
    if (includeRadiation)
    {
        result.optics.resize(surfaceCount());
        result.radiation.radiosity.resize(surfaceCount());
        result.radiation.incident.resize(surfaceCount());
        downloadBuffer(m_optics, result.optics.data(), result.optics.size() * sizeof(SurfaceOptics));
        // DEFERRED[低] Bug: 固定读取 m_radiosityA 并把 iterations 写死为 64，只在最后一次求解恰好是 evaluateEnergy（64 次、偶数、ping=0）时正确；若之后调用过 solveAccelerated/solveSpectralBand（ping 可能为 1），读到的是上一步的旧辐射度且 maxDelta 也来自别的求解；建议：在成员中记录最后一次求解的 ping 与迭代数，按其选择缓冲。
        downloadBuffer(m_radiosityA, result.radiation.radiosity.data(), surfaceCount() * sizeof(float));
        downloadBuffer(m_incident, result.radiation.incident.data(), surfaceCount() * sizeof(float));
        uint32_t delta{};
        downloadBuffer(m_residual, &delta, sizeof(delta));
        std::memcpy(&result.radiation.maxDelta, &delta, sizeof(delta));
        result.radiation.iterations = 64;
    }
    return result;
}

SolveResult FacetrtVulkan::solve(const std::vector<SurfaceOptics> &optics, float skyRadiosity, uint32_t iterations,
                                 float relaxation)
{
    if (m_solvePipeline == VK_NULL_HANDLE)
    {
        throw std::logic_error("buildVisibilityGraph() must be called before solve()");
    }
    validateOptics(optics);
    if (!std::isfinite(skyRadiosity) || skyRadiosity < 0.0f) {
        throw std::invalid_argument("skyRadiosity must be finite and non-negative");
    }
    if (!(relaxation > 0.0f && relaxation <= 1.0f)) {
        throw std::invalid_argument("GPU Jacobi relaxation must be in (0,1]");
    }
    uploadBuffer(m_optics, optics.data(), optics.size() * sizeof(SurfaceOptics));

    VkCommandBuffer commandBuffer = beginCommands();
    const uint32_t ping = recordSolve(commandBuffer, skyRadiosity, iterations, relaxation);
    endCommands(commandBuffer);

    SolveResult result{};
    result.iterations = iterations;
    result.radiosity.resize(surfaceCount(), 0.0f);
    const Buffer& finalBuffer = ping == 0U ? m_radiosityA : m_radiosityB;
    downloadBuffer(finalBuffer, result.radiosity.data(),
                   result.radiosity.size() * sizeof(float));
    uint32_t deltaBits = 0U;
    downloadBuffer(m_residual, &deltaBits, sizeof(deltaBits));
    std::memcpy(&result.maxDelta, &deltaBits, sizeof(result.maxDelta));
    if (m_incidentPipeline != VK_NULL_HANDLE) {
        result.incident.resize(surfaceCount());
        downloadBuffer(m_incident, result.incident.data(), result.incident.size() * sizeof(float));
    }
    return result;
}

SolveResult FacetrtVulkan::solveAccelerated(
    const std::vector<SurfaceOptics>& optics,
    float skyRadiosity,
    const std::vector<float>* initialRadiosity,
    uint32_t maximumIterations,
    uint32_t batchIterations,
    float tolerance,
    float relaxation)
{
    if (m_solvePipeline == VK_NULL_HANDLE) {
        throw std::logic_error(
            "buildVisibilityGraph() must be called before solveAccelerated()");
    }
    validateOptics(optics);
    if (!std::isfinite(skyRadiosity) || skyRadiosity < 0.0f) {
        throw std::invalid_argument("skyRadiosity must be finite and non-negative");
    }
    if (!(relaxation > 0.0f && relaxation <= 1.0f)) {
        throw std::invalid_argument("GPU Jacobi relaxation must be in (0,1]");
    }
    if (maximumIterations == 0U || batchIterations == 0U ||
        !std::isfinite(tolerance) || tolerance < 0.0f) {
        throw std::invalid_argument(
            "Accelerated solve iterations must be positive and tolerance non-negative");
    }
    if (initialRadiosity != nullptr) {
        if (initialRadiosity->size() != surfaceCount() ||
            std::any_of(initialRadiosity->begin(), initialRadiosity->end(),
                        [](float value) {
                            return !std::isfinite(value) || value < 0.0f;
                        })) {
            throw std::invalid_argument(
                "Initial radiosity must contain one finite non-negative value per surface");
        }
    }

    uploadBuffer(m_optics, optics.data(), optics.size() * sizeof(SurfaceOptics));
    if (initialRadiosity != nullptr) {
        const VkDeviceSize bytes =
            initialRadiosity->size() * sizeof(float);
        uploadBuffer(m_radiosityA, initialRadiosity->data(), bytes);
        uploadBuffer(m_radiosityB, initialRadiosity->data(), bytes);
    } else {
        VkCommandBuffer clearCommands = beginCommands();
        vkCmdFillBuffer(clearCommands, m_radiosityA.buffer, 0,
                        m_radiosityA.size, 0U);
        vkCmdFillBuffer(clearCommands, m_radiosityB.buffer, 0,
                        m_radiosityB.size, 0U);
        endCommands(clearCommands);
    }

    SolveResult result{};
    uint32_t ping = 0U;
    while (result.iterations < maximumIterations) {
        const uint32_t count = std::min(
            batchIterations, maximumIterations - result.iterations);
        VkCommandBuffer commandBuffer = beginCommands();
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_solvePipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_solvePipelineLayout, 0, 1,
                                &m_solveDescriptorSet, 0, nullptr);
        for (uint32_t iteration = 0; iteration < count; ++iteration) {
            vkCmdFillBuffer(commandBuffer, m_residual.buffer, 0,
                            sizeof(uint32_t), 0U);
            VkMemoryBarrier residualClear{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            residualClear.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            residualClear.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 1, &residualClear, 0, nullptr, 0, nullptr);

            const SolvePush push{
                m_facetCount, ping, skyRadiosity, relaxation};
            vkCmdPushConstants(commandBuffer, m_solvePipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push), &push);
            vkCmdDispatch(commandBuffer, (m_facetCount + 63U) / 64U, 1, 1);

            VkMemoryBarrier iterationBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            iterationBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            iterationBarrier.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(commandBuffer,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 1, &iterationBarrier,
                                 0, nullptr, 0, nullptr);
            ping ^= 1U;
        }
        endCommands(commandBuffer);
        result.iterations += count;

        uint32_t deltaBits = 0U;
        downloadBuffer(m_residual, &deltaBits, sizeof(deltaBits));
        std::memcpy(&result.maxDelta, &deltaBits, sizeof(result.maxDelta));
        if (result.maxDelta <= tolerance) {
            break;
        }
    }

    VkCommandBuffer incidentCommands = beginCommands();
    recordIncident(incidentCommands, ping, skyRadiosity);
    endCommands(incidentCommands);
    result.radiosity.resize(surfaceCount(), 0.0f);
    const Buffer& finalBuffer = ping == 0U ? m_radiosityA : m_radiosityB;
    downloadBuffer(finalBuffer, result.radiosity.data(),
                   result.radiosity.size() * sizeof(float));
    if (m_incidentPipeline != VK_NULL_HANDLE) {
        result.incident.resize(surfaceCount());
        downloadBuffer(m_incident, result.incident.data(), m_incident.size);
    }
    return result;
}

std::vector<float> FacetrtVulkan::incidentIrradiance(
    const SolveResult& solution,
    const std::vector<SurfaceOptics>& optics,
    float skyRadiosity) const
{
    if (solution.radiosity.size() != surfaceCount() ||
        optics.size() != surfaceCount() ||
        m_rowOffsetsCpu.size() != surfaceCount() + 1U ||
        m_denominatorCpu.size() != surfaceCount() ||
        m_skyCountsCpu.size() != surfaceCount()) {
        throw std::invalid_argument(
            "Incident irradiance requires a completed solve and visibility graph");
    }
    if (!std::isfinite(skyRadiosity) || skyRadiosity < 0.0f) {
        throw std::invalid_argument(
            "Sky radiosity must be finite and non-negative");
    }

    if (solution.incident.size() == surfaceCount()) return solution.incident;
    std::vector<float> irradiance(surfaceCount(), 0.0f);
    for (uint32_t side = 0; side < surfaceCount(); ++side) {
        double diffuse = skyRadiosity;
        const uint32_t denominator = m_denominatorCpu[side];
        if (denominator != 0U) {
            diffuse = static_cast<double>(m_skyCountsCpu[side]) *
                      static_cast<double>(skyRadiosity);
            for (uint32_t edgeIndex = m_rowOffsetsCpu[side];
                 edgeIndex < m_rowOffsetsCpu[side + 1U]; ++edgeIndex) {
                const CsrEdge& edge = m_edgesCpu[edgeIndex];
                diffuse += static_cast<double>(edge.count) *
                           static_cast<double>(
                               solution.radiosity[edge.neighbor]);
            }
            diffuse /= static_cast<double>(denominator);
        }
        irradiance[side] = std::max(
            0.0f, static_cast<float>(diffuse) +
                      optics[side].directIrradiance);
    }
    return irradiance;
}

void FacetrtVulkan::beginSpectral(const std::vector<float>& facets, const std::vector<float>& materialBands,
                                 uint32_t bands, uint32_t materials, const Direction& sun, float backgroundStrength,
                                 float beamIrradiance)
{
    if (m_solvePipeline == VK_NULL_HANDLE || facets.size() != size_t(m_facetCount) * 12U || bands == 0 ||
        materials == 0 || materialBands.size() != size_t(bands) * materials * 4U)
        throw std::invalid_argument("Invalid spectral facet/material layout");
    for (float v : facets) if (!std::isfinite(v)) throw std::invalid_argument("Non-finite spectral facet");
    for (float v : materialBands) if (!std::isfinite(v)) throw std::invalid_argument("Invalid material spectrum");
    destroySpectralResources();
    static_assert(sizeof(SpectralPush) == 56);
    m_spectralPush = {0, 0, m_facetCount, materials, sun.x, sun.y, sun.z, backgroundStrength,
                      beamIrradiance, 1, 0, 0, bands, 0};
    const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const auto device = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkDeviceSize outputBytes = (VkDeviceSize(bands) * 3U + 1U) * surfaceCount() * sizeof(float);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
    if (outputBytes > properties.limits.maxStorageBufferRange || outputBytes / 4 > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Spectral output exceeds the GPU storage-buffer limit");
    m_spectralFacets = createBuffer(facets.size() * sizeof(float), usage, device);
    m_spectralMaterials = createBuffer(materialBands.size() * sizeof(float), usage, device);
    m_spectralEmission = createBuffer(VkDeviceSize(surfaceCount()) * 2 * sizeof(float), usage, device);
    m_spectralPrevious = createBuffer(VkDeviceSize(surfaceCount()) * sizeof(float), usage, device);
    m_spectralOutput = createBuffer(outputBytes, usage, device);
    m_spectralConvergence = createBuffer(2U * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    m_spectralDispatch = createBuffer(8U * sizeof(uint32_t), usage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, device);
    uploadBuffer(m_spectralFacets, facets.data(), facets.size() * sizeof(float));
    uploadBuffer(m_spectralMaterials, materialBands.data(), materialBands.size() * sizeof(float));
    std::array<VkDescriptorSetLayoutBinding, 10> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = uint32_t(bindings.size()); setInfo.pBindings = bindings.data();
    require(vkCreateDescriptorSetLayout(m_device, &setInfo, nullptr, &m_spectralSetLayout), "spectral descriptor layout");
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, uint32_t(bindings.size())};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1; poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize;
    require(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_spectralPool), "spectral descriptor pool");
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = m_spectralPool; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &m_spectralSetLayout;
    require(vkAllocateDescriptorSets(m_device, &allocation, &m_spectralSet), "spectral descriptor set");
    const std::array<Buffer*, 10> buffers{&m_spectralFacets, &m_spectralMaterials, &m_optics, &m_spectralEmission,
                                         &m_radiosityA, &m_radiosityB, &m_spectralPrevious, &m_spectralOutput,
                                         &m_spectralDispatch, &m_residual};
    std::array<VkDescriptorBufferInfo, 10> infos{};
    std::array<VkWriteDescriptorSet, 10> writes{};
    for (uint32_t i = 0; i < buffers.size(); ++i) {
        infos[i] = {buffers[i]->buffer, 0, buffers[i]->size};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = m_spectralSet; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SpectralPush)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1; layout.pSetLayouts = &m_spectralSetLayout;
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
    require(vkCreatePipelineLayout(m_device, &layout, nullptr, &m_spectralLayout), "spectral pipeline layout");
    VkShaderModule module = loadShader("spectral.comp.spv");
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.layout = m_spectralLayout;
    pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = module; pipeline.stage.pName = "main";
    const VkResult created = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_spectralPipeline);
    vkDestroyShaderModule(m_device, module, nullptr);
    require(created, "spectral compute pipeline");
    auto cmd = beginCommands();
    vkCmdFillBuffer(cmd, m_spectralOutput.buffer, 0, m_spectralOutput.size, 0U);
    computeBarrier(cmd); endCommands(cmd);
}

void FacetrtVulkan::recordSpectral(VkCommandBuffer commandBuffer, const SpectralPush& push)
{
    computeBarrier(commandBuffer);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spectralPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spectralLayout, 0, 1, &m_spectralSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_spectralLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commandBuffer, (surfaceCount() + 63U) / 64U, 1, 1);
    computeBarrier(commandBuffer);
}

SolveResult FacetrtVulkan::solveSpectralBand(uint32_t band, bool thermal, float sky, float scale,
                                           bool accelerated, bool warmStart, bool diffuse)
{
    if (m_spectralPipeline == VK_NULL_HANDLE || band >= m_spectralPush.bands || !std::isfinite(sky) || sky < 0 ||
        !std::isfinite(scale) || scale <= 0 || (diffuse && band != 0))
        throw std::invalid_argument("Invalid GPU spectral band solve");
    auto push = m_spectralPush;
    push.band = band; push.thermal = thermal; push.beam = thermal ? 0.0f : m_spectralPush.beam; push.scale = scale;
    push.phase = diffuse ? 4U : 0U;
    auto cmd = beginCommands();
    recordSpectral(cmd, push);
    if (warmStart && accelerated) {
        const VkBufferCopy copy{0, 0, m_spectralPrevious.size};
        vkCmdCopyBuffer(cmd, m_spectralPrevious.buffer, m_radiosityA.buffer, 1, &copy);
        vkCmdCopyBuffer(cmd, m_spectralPrevious.buffer, m_radiosityB.buffer, 1, &copy);
    } else {
        vkCmdFillBuffer(cmd, m_radiosityA.buffer, 0, m_radiosityA.size, 0U);
        vkCmdFillBuffer(cmd, m_radiosityB.buffer, 0, m_radiosityB.size, 0U);
    }
    computeBarrier(cmd);
    uint32_t ping = 0U;
    if (accelerated) {
        push.phase = 5U;
        recordSpectral(cmd, push);
        for (uint32_t batch = 0; batch < 8U; ++batch) {
            ping = recordIterations(cmd, ping, sky, 8U, 1.0f, true);
            push.phase = 6U;
            recordSpectral(cmd, push);
        }
        const VkBufferCopy copy{3U * sizeof(uint32_t), 0, 2U * sizeof(uint32_t)};
        vkCmdCopyBuffer(cmd, m_spectralDispatch.buffer, m_spectralConvergence.buffer, 1, &copy);
    } else {
        ping = recordIterations(cmd, 0U, sky, 64U, 1.0f);
        computeBarrier(cmd);
        const VkBufferCopy copy{0, sizeof(uint32_t), sizeof(uint32_t)};
        vkCmdCopyBuffer(cmd, m_residual.buffer, m_spectralConvergence.buffer, 1, &copy);
    }
    recordIncident(cmd, ping, sky);
    push.ping = ping; push.phase = diffuse ? 3U : 2U;
    recordSpectral(cmd, push); endCommands(cmd);
    SolveResult result{};
    void* mapped = nullptr;
    require(vkMapMemory(m_device, m_spectralConvergence.memory, 0, 2U * sizeof(uint32_t), 0, &mapped), "spectral convergence map");
    result.iterations = 64U;
    if (accelerated) std::memcpy(&result.iterations, mapped, sizeof(uint32_t));
    std::memcpy(&result.maxDelta, static_cast<const char*>(mapped) + sizeof(uint32_t), sizeof(float));
    vkUnmapMemory(m_device, m_spectralConvergence.memory);
    return result;
}

std::vector<float> FacetrtVulkan::readSpectralOutput()
{
    if (m_spectralPipeline == VK_NULL_HANDLE) throw std::logic_error("No spectral output");
    std::vector<float> output(size_t(m_spectralOutput.size / sizeof(float)));
    downloadBuffer(m_spectralOutput, output.data(), m_spectralOutput.size);
    return output;
}

SpectralAudit FacetrtVulkan::readSpectralAudit(uint32_t band)
{
    SpectralAudit audit;
    audit.optics.resize(surfaceCount()); audit.emission.resize(surfaceCount()); audit.raw.resize(surfaceCount());
    downloadBuffer(m_optics, audit.optics.data(), m_optics.size);
    downloadBuffer(m_spectralEmission, audit.emission.data(), m_spectralEmission.size);
    downloadBuffer(m_spectralPrevious, audit.raw.data(), m_spectralPrevious.size);
    const auto output = readSpectralOutput();
    const size_t n = surfaceCount(), b = size_t(band) * n, k = size_t(m_spectralPush.bands) * n + 2U * b;
    audit.output.assign(output.begin() + b, output.begin() + b + n);
    audit.states.assign(output.begin() + k, output.begin() + k + 2U * n);
    return audit;
}

void FacetrtVulkan::destroySpectralResources()
{
    if (m_spectralPipeline) vkDestroyPipeline(m_device, m_spectralPipeline, nullptr);
    if (m_spectralLayout) vkDestroyPipelineLayout(m_device, m_spectralLayout, nullptr);
    if (m_spectralPool) vkDestroyDescriptorPool(m_device, m_spectralPool, nullptr);
    if (m_spectralSetLayout) vkDestroyDescriptorSetLayout(m_device, m_spectralSetLayout, nullptr);
    m_spectralPipeline = VK_NULL_HANDLE; m_spectralLayout = VK_NULL_HANDLE;
    m_spectralPool = VK_NULL_HANDLE; m_spectralSetLayout = VK_NULL_HANDLE; m_spectralSet = VK_NULL_HANDLE;
    destroyBuffer(m_spectralFacets); destroyBuffer(m_spectralMaterials); destroyBuffer(m_spectralEmission);
    destroyBuffer(m_spectralPrevious); destroyBuffer(m_spectralOutput); destroyBuffer(m_spectralConvergence);
    destroyBuffer(m_spectralDispatch);
}

void FacetrtVulkan::restoreSpectralOptics(const std::vector<SurfaceOptics>& optics)
{
    validateOptics(optics);
    uploadBuffer(m_optics, optics.data(), optics.size() * sizeof(SurfaceOptics));
}

void FacetrtVulkan::destroySolveResources()
{
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    destroySpectralResources();
    destroyEnergyResources();
    if (m_incidentPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_incidentPipeline, nullptr);
    m_incidentPipeline = VK_NULL_HANDLE;
    destroyBuffer(m_incident);
    if (m_solvePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_solvePipeline, nullptr);
    }
    if (m_solvePipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_solvePipelineLayout, nullptr);
    }
    if (m_solveDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_solveDescriptorPool, nullptr);
    }
    if (m_solveSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_solveSetLayout, nullptr);
    }
    m_solvePipeline = VK_NULL_HANDLE;
    m_solvePipelineLayout = VK_NULL_HANDLE;
    m_solveDescriptorPool = VK_NULL_HANDLE;
    m_solveDescriptorSet = VK_NULL_HANDLE;
    m_solveSetLayout = VK_NULL_HANDLE;
    destroyBuffer(m_rowOffsets);
    destroyBuffer(m_edges);
    destroyBuffer(m_optics);
    destroyBuffer(m_radiosityA);
    destroyBuffer(m_radiosityB);
    destroyBuffer(m_residual);
    m_rowOffsetsCpu.clear();
    m_edgesCpu.clear();
}

void FacetrtVulkan::destroyVisibilityResources()
{
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    if (m_resolvePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_resolvePipeline, nullptr);
    }
    if (m_directResolvePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_directResolvePipeline, nullptr);
    }
    if (m_rasterPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_rasterPipeline, nullptr);
    }
    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
    }
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
    }
    if (m_visibilityPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_visibilityPipelineLayout, nullptr);
    }
    if (m_visibilityDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_visibilityDescriptorPool, nullptr);
    }
    if (m_visibilitySetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_visibilitySetLayout, nullptr);
    }
    m_resolvePipeline = VK_NULL_HANDLE;
    m_directResolvePipeline = VK_NULL_HANDLE;
    m_rasterPipeline = VK_NULL_HANDLE;
    m_framebuffer = VK_NULL_HANDLE;
    m_renderPass = VK_NULL_HANDLE;
    m_visibilityPipelineLayout = VK_NULL_HANDLE;
    m_visibilityDescriptorPool = VK_NULL_HANDLE;
    m_visibilityDescriptorSet = VK_NULL_HANDLE;
    m_directDescriptorSet = VK_NULL_HANDLE;
    m_visibilitySetLayout = VK_NULL_HANDLE;
    destroyBuffer(m_vertices);
    destroyBuffer(m_fragments);
    destroyBuffer(m_pixelCounts);
    destroyBuffer(m_edgeKeys);
    destroyBuffer(m_edgeCounts);
    destroyBuffer(m_denominator);
    destroyBuffer(m_skyCounts);
    destroyBuffer(m_visibilityStats);
    destroyBuffer(m_directTotal);
    destroyBuffer(m_directVisible);
    m_denominatorCpu.clear();
    m_skyCountsCpu.clear();
}

void FacetrtVulkan::destroy()
{
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        destroySolveResources();
        destroyVisibilityResources();
        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        }
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_debugMessenger != VK_NULL_HANDLE && m_instance != VK_NULL_HANDLE) {
        const auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger != nullptr) {
            destroyMessenger(m_instance, m_debugMessenger, nullptr);
        }
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
    }
    m_instance = VK_NULL_HANDLE;
    m_debugMessenger = VK_NULL_HANDLE;
    m_physicalDevice = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_queue = VK_NULL_HANDLE;
    m_commandPool = VK_NULL_HANDLE;
    m_queueFamily = 0;
    m_facetCount = 0;
    m_hashCapacity = 0;
}

} // namespace facetvk
