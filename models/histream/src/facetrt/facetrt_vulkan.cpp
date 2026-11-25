// ==========================================================================
// 文件：facetrt_vulkan.cpp
// 功能：FacetRT / FacetEB 共用的 Vulkan 光栅辐射度后端 FacetrtVulkan 的实现（声明见 facetrt_vulkan.h）。
//   用光栅化 A-buffer 估计面元各侧之间的可见性（视角因子的光栅估计），再在 GPU 上做
//   双面反射/透射的 Jacobi 辐射度迭代；还提供直射日照比例、光谱波段求解与 GPU 能量平衡。
// 主要模块：
//   1. 匿名命名空间工具：哈希空槽键、周期瓦片偏移表、向量运算、校验层回调
//   2. 设备选择 rankDevices、Vulkan 上下文/命令池/缓冲/着色器加载等底层封装
//   3. 几何上传 setGeometry 与正交投影 makeProjection（含周期边界邻接瓦片）
//   4. 可见性图 buildVisibilityGraph：光栅 → 解析 → 回读哈希表 → CSR → 下边界（等效地面）
//   5. 日照可见比例 computeSunlitFraction（4×4 亚像素抖动）
//   6. 辐射度求解 solve / solveAccelerated 与入射辐照度 incidentIrradiance
//   7. GPU 能量平衡（energy.comp）：beginEnergyNode / evaluateEnergy / updateEnergyTemperature
//   8. 光谱波段求解（spectral.comp）：beginSpectral / solveSpectralBand / readSpectralOutput
//   9. 资源释放 destroy*
// 数据流：Vertex 三角形列表 + 方向集 → A-buffer → 边哈希（无向边 = 射线计数）→ CSR 边表
//   → 与 SurfaceOptics 一起在 GPU 上迭代求解各 side 的辐射度（计数/分母即交换权重）。
// 与其他文件：着色器位于 models/histream/shader（facetrt/ 与 faceteb/），按名称加载 *.spv；
//   调用方：facetrtcore.cpp（GPU 可见性/求解/能量/光谱内核）、appsetting.cpp（设备枚举）；
// 约定：每个面元有两个 oriented side：side = 2*facet（几何法向侧）、2*facet+1（反面）；
//   可见性方向指向虚拟相机；辐射度/辐照度单位由调用方决定，本文件不做单位换算；
//   所有 Vulkan 调用均同步提交并等待（endCommands），错误一律抛异常。
// ==========================================================================

#include "facetrt_vulkan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

namespace facetvk {
namespace {

// 匿名命名空间：本文件内部使用的常量与小工具（不对外导出）。
// kEmptyEdgeKey：边哈希表中“空槽”的哨兵键（64 位全 1）。
constexpr uint64_t kEmptyEdgeKey = std::numeric_limits<uint64_t>::max();
// Highest per-pixel fragment count the visibility shaders can resolve
// (MAX_FRAGMENTS in visibility_resolve.comp / direct_resolve.comp).
constexpr uint32_t kMaximumRasterLayers = 256;

// Destroys a shader module on scope exit, including pipeline-creation failures.
struct ShaderModuleGuard {
    VkDevice device;
    VkShaderModule module;
    ~ShaderModuleGuard() { if (module != VK_NULL_HANDLE) vkDestroyShaderModule(device, module, nullptr); }
};

// 可见性管线共用的 push constant 可见阶段（顶点/片元/计算），对应 VisibilityPush。
constexpr VkShaderStageFlags kVisibilityPushStages = VK_SHADER_STAGE_VERTEX_BIT |
                                                     VK_SHADER_STAGE_FRAGMENT_BIT |
                                                     VK_SHADER_STAGE_COMPUTE_BIT;
// 作用：Vulkan 校验层回调，仅输出 WARNING 及以上级别的消息到 stderr；始终返回 VK_FALSE（不中断调用）。
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

// 作用：向量归一化；长度不大于 1e-8 时抛 invalid_argument（可见性方向不允许为零向量）。
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

// 作用：周期边界邻接瓦片的 (X, Z) 整数偏移表。index=0 为本体 {0,0}；
// index=1..20 依次为一圈 4 邻 + 4 角 + 外圈 12 个；超出 20 时钳到最后一项。
// 与 Config::periodicNeighborCount、Config::periodicSizeX/Z 配合使用（偏移乘以域尺寸）。
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

// 模块：GPU 枚举与排序。
// 筛选条件：非 CPU 类型设备；支持 shaderInt64、shaderBufferInt64Atomics、
// fragmentStoresAndAtomics；存在同时具备 graphics+compute 的队列族。
// 评分：独显 1000、其他 100，再加 maxImageDimension2D/1024；按分数稳定降序，
// 与 Config::gpuIndex 的下标一致。
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

// 模块：可见性图的下边界（“等效地面”）处理。
// 作用：把 firstGroundFacet 及之后的面元视为地面：其背朝下的一侧（由面元法向 y 分量判定）
// 置为失效（分母=0、天空计数=0、地面计数=kInactiveSide），另一侧（朝上）按面积权重
// （总和归一为 1）构成“等效地面”，供向下逃逸的视线取其平均辐射度。
// 输入：顶点（每面元 3 顶点，法向相同）、三类逐侧计数（长度均为 2×面元数）；
// 输出：GroundBoundary，并填充 diagnostics 的 ground* / skyFraction 诊断量。
// 诊断量只统计非地面的物体侧（side < first*2），地面的朝下侧不计入任何分母。
GroundBoundary applyGroundBoundary(const std::vector<Vertex>& vertices, uint32_t firstGroundFacet,
                                   std::vector<uint32_t>& denominators, std::vector<uint32_t>& skyCounts,
                                   std::vector<uint32_t>& groundCounts, GraphDiagnostics& diagnostics)
{
    const uint32_t facets = static_cast<uint32_t>(vertices.size() / 3U);
    if (denominators.size() != size_t(facets) * 2U || skyCounts.size() != denominators.size() ||
        groundCounts.size() != denominators.size())
        throw std::invalid_argument("Ground boundary requires one count per oriented side");
    const uint32_t first = std::min(firstGroundFacet, facets);
    const auto area = [&](uint32_t facet) {
        const auto& a = vertices[size_t(facet) * 3U].position;
        const auto& b = vertices[size_t(facet) * 3U + 1U].position;
        const auto& c = vertices[size_t(facet) * 3U + 2U].position;
        const std::array<float, 3> n = cross3({b[0] - a[0], b[1] - a[1], b[2] - a[2]},
                                              {c[0] - a[0], c[1] - a[1], c[2] - a[2]});
        return 0.5 * std::sqrt(double(n[0]) * n[0] + double(n[1]) * n[1] + double(n[2]) * n[2]);
    };
    // 步骤 1：地面面元（索引 >= first）：朝下一侧失效；朝上一侧按面积权重加入等效地面。
    GroundBoundary ground;
    double total = 0.0;
    std::vector<double> weights;
    for (uint32_t facet = first; facet < facets; ++facet) {
        // The side whose normal points down faces the soil: it receives
        // neither sky nor ground and exchanges with nothing.
        const uint32_t down = facet * 2U + (vertices[size_t(facet) * 3U].normal[1] >= 0.0f ? 1U : 0U);
        denominators[down] = 0U;
        skyCounts[down] = 0U;
        groundCounts[down] = kInactiveSide;
        const double weight = area(facet);
        if (weight > 0.0) {
            ground.sides.push_back(down ^ 1U);
            weights.push_back(weight);
            total += weight;
        }
    }
    for (double weight : weights) ground.weights.push_back(static_cast<float>(weight / total));
    diagnostics.groundFacets = facets - first;
    diagnostics.inactiveGroundSides = facets - first;
    // 步骤 2：统计物体侧（非地面）的向下逃逸/天空比例；按分母（可见性采样数）加权，
    // 另外给出按面元面积加权的逐侧平均与最大逐侧比例。
    double denominator = 0.0, escaped = 0.0, sky = 0.0, areaSum = 0.0, areaEscape = 0.0;
    uint32_t escapeSides = 0U;
    float maximum = 0.0f;
    for (uint32_t side = 0; side < first * 2U; ++side) {
        if (denominators[side] == 0U) continue;
        const double fraction = double(groundCounts[side]) / denominators[side];
        denominator += denominators[side];
        escaped += groundCounts[side];
        sky += skyCounts[side];
        escapeSides += groundCounts[side] != 0U ? 1U : 0U;
        maximum = std::max(maximum, static_cast<float>(fraction));
        const double weight = area(side / 2U);
        areaSum += weight;
        areaEscape += weight * fraction;
    }
    // 步骤 3：单独统计朝上地面侧的逃逸比例，然后写入诊断结构（分母为 0 时一律置 0）。
    double groundDenominator = 0.0, groundEscaped = 0.0;
    for (uint32_t side : ground.sides) {
        groundDenominator += denominators[side];
        groundEscaped += groundCounts[side];
    }
    diagnostics.groundEscapeSides = escapeSides;
    diagnostics.groundEscapeFraction = denominator > 0.0 ? float(escaped / denominator) : 0.0f;
    diagnostics.groundEscapeAreaFraction = areaSum > 0.0 ? float(areaEscape / areaSum) : 0.0f;
    diagnostics.groundEscapeMaxFraction = maximum;
    diagnostics.groundEscapeGroundFraction =
        groundDenominator > 0.0 ? float(groundEscaped / groundDenominator) : 0.0f;
    diagnostics.skyFraction = denominator > 0.0 ? float(sky / denominator) : 0.0f;
    return ground;
}

// 作用：等效地面辐射度 = Σ weight_i × radiosity[side_i]（double 累加）；
// 没有地面面元时退化为天空辐射度 skyRadiosity。
float groundBoundaryRadiosity(const GroundBoundary& ground, const std::vector<float>& radiosity, float skyRadiosity)
{
    if (ground.sides.empty()) return skyRadiosity;
    double value = 0.0;
    for (size_t i = 0; i < ground.sides.size(); ++i)
        value += double(ground.weights[i]) * radiosity.at(ground.sides[i]);
    return static_cast<float>(value);
}

FacetrtVulkan::~FacetrtVulkan()
{
    destroy();
}

// 作用：检查 VkResult，非 VK_SUCCESS 时抛 runtime_error（含操作名与错误码）。
void FacetrtVulkan::require(VkResult result, const char* operation) const
{
    if (result != VK_SUCCESS) {
        std::ostringstream stream;
        stream << operation << " failed with VkResult " << static_cast<int>(result);
        throw std::runtime_error(stream.str());
    }
}

// 作用：初始化后端：先 destroy() 释放旧资源，校验栅格尺寸与每像素最大片元数（1..256，
// 与着色器 MAX_FRAGMENTS 一致），保存配置与着色器目录（SPIR-V 所在），
// 再创建 Vulkan 上下文与命令池。几何和管线在 setGeometry / buildVisibilityGraph 中创建。
void FacetrtVulkan::initialize(const Config& config, const std::string& shaderDirectory)
{
    destroy();
    if (config.rasterWidth == 0 || config.rasterHeight == 0) {
        throw std::invalid_argument("Raster dimensions must be positive");
    }
    if (config.maxFragmentsPerPixel == 0 || config.maxFragmentsPerPixel > 256) {
        throw std::invalid_argument("maxFragmentsPerPixel must be in [1, 256]");
    }
    m_config = config;
    m_shaderDirectory = shaderDirectory;
    createContext();
    createCommandPool();
}

// 作用：创建 Vulkan 1.2 实例（可选校验层）、选择物理设备、创建逻辑设备与单个队列。
// 设备特性：shaderInt64 + shaderBufferInt64Atomics（64 位边键原子操作）、
// fragmentStoresAndAtomics（片元着色器写 A-buffer）；shaderFloat64 有则启用，
// 结果记入 m_shaderFloat64；有双精度时才创建 incident 管线，且在 energy.comp.spv 存在时创建能量资源。
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
    // 注意：gpuIndex 超出候选数量时钳到最后一块可用 GPU（见上方 DEFERRED 说明）。
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

// 作用：创建命令池（transient + 可单独复位），所有一次性命令缓冲都由 beginCommands() 从中分配。
void FacetrtVulkan::createCommandPool()
{
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = m_queueFamily;
    require(vkCreateCommandPool(m_device, &info, nullptr, &m_commandPool),
            "vkCreateCommandPool");
}

// 作用：在 typeBits 允许的内存类型中，返回第一个包含全部 properties 标志的类型下标；找不到则抛错。
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

// 作用：创建 VkBuffer 并分配、绑定独立的 VkDeviceMemory（每个缓冲一次 vkAllocateMemory）。
// 说明：大小最小取 4 字节（避免零长度缓冲）；失败时先释放已创建的句柄再重新抛出。
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

// 模块：同步命令提交。beginCommands() 分配并开始一个 ONE_TIME_SUBMIT 命令缓冲；
// endCommands() 结束、提交、用 fence 等待完成并释放（每次提交都阻塞到 GPU 完成）。
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

// 说明：stage 变量记录最近一步的名称，失败时用于错误信息；
// 等待 fence 失败（pending）时命令缓冲可能仍在 GPU 上运行，因此不释放它。
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

// 作用：同步的缓冲到缓冲拷贝（从 offset 0 开始拷 size 字节）。
void FacetrtVulkan::copyBuffer(VkBuffer source, VkBuffer destination, VkDeviceSize size) const
{
    VkCommandBuffer commandBuffer = beginCommands();
    const VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(commandBuffer, source, destination, 1, &copy);
    endCommands(commandBuffer);
}

// 作用：主机 → 设备上传：写入临时的 host-visible staging 缓冲，再拷贝到设备端缓冲；
// size 超过目标缓冲大小时抛 out_of_range。
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

// 作用：设备 → 主机回读：先拷到 host-visible staging 缓冲，再映射并 memcpy 到 data。
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

// 作用：释放缓冲及其显存，并把 Buffer 结构清零；对空句柄安全，可重复调用。
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

// 作用：从 m_shaderDirectory 读取 SPIR-V（.spv）文件并创建 VkShaderModule；
// 文件大小必须为正且是 4 字节的整数倍。调用者负责销毁（常用 ShaderModuleGuard）。
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

// 作用：返回不小于 value 的最小 2 的幂（value<=1 时为 1）；超过 2^31 时回绕为 0，
// 调用者通过 capacity < 请求值 检测该溢出。
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

// 作用：边哈希表容量上限 = 不超过 maxStorageBufferRange / 8 字节（每槽一个 uint64 键）的最大 2 的幂；
// 设备上限小于 65536 个槽时视为不可用并抛错。
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

// 作用：确定边哈希表容量（槽数，2 的幂）。
// Config::edgeHashCapacity 非 0 时取其向上取 2 的幂，超过设备上限则抛 overflow_error；
// 为 0 时自动取 max(65536, 64×oriented side 数) 向上取 2 的幂，并以设备上限封顶。
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

// 作用：A-buffer（片元缓冲）允许占用的显存预算（字节）。
// 环境变量 HISTREAM_FACET_RASTER_BUDGET_MB（单位 MB，须为正数）优先；
// 否则取最大 device-local 堆的 1/4。
uint64_t FacetrtVulkan::rasterLayerBudgetBytes() const
{
    if (const char* requested = std::getenv("HISTREAM_FACET_RASTER_BUDGET_MB")) {
        char* end = nullptr;
        const double megabytes = std::strtod(requested, &end);
        if (end != requested && megabytes > 0.0) {
            return static_cast<uint64_t>(megabytes * 1024.0 * 1024.0);
        }
    }
    // Keep the fragment buffer inside a quarter of the largest device-local
    // heap so a denser A-buffer cannot evict the geometry and solve buffers.
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memory);
    VkDeviceSize deviceLocal = 0;
    for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
        if ((memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0U) {
            deviceLocal = std::max(deviceLocal, memory.memoryHeaps[heap].size);
        }
    }
    return deviceLocal / 4U;
}

// 作用：A-buffer 每像素层数不足时扩容（只修改 m_config.maxFragmentsPerPixel，
// 由调用者随后重建可见性资源）。新层数 = max(2×当前, 上次溢出实测所需层数向上取 2 的幂)，
// 封顶 kMaximumRasterLayers(256)；每片元占 8 字节，若超过 maxStorageBufferRange
// 或显存预算则返回 false（调用者随后抛 RasterLayerOverflow）。
bool FacetrtVulkan::growRasterLayers()
{
    const uint32_t current = std::max(1U, m_config.maxFragmentsPerPixel);
    if (current >= kMaximumRasterLayers) {
        return false;
    }
    // Jump straight to the deepest per-pixel list observed in the overflowing
    // pass (rounded to a power of two) rather than doubling: each attempt
    // rebuilds the whole visibility graph (~1 s per step on dense scenes).
    uint32_t next = std::min(kMaximumRasterLayers, current * 2U);
    if (m_requiredRasterLayers > next) {
        next = std::min(kMaximumRasterLayers, nextPowerOfTwo(m_requiredRasterLayers));
    }
    const uint64_t fragments = uint64_t(m_config.rasterWidth) *
                               uint64_t(m_config.rasterHeight) * uint64_t(next);
    if (fragments > std::numeric_limits<uint64_t>::max() / 8ULL) {
        return false;
    }
    const uint64_t bytes = fragments * 8ULL;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
    if (bytes > uint64_t(properties.limits.maxStorageBufferRange)) {
        return false;
    }
    const uint64_t budget = rasterLayerBudgetBytes();
    if (budget != 0ULL && bytes > budget) {
        return false;
    }
    m_config.maxFragmentsPerPixel = next;
    std::cout << "facetrt: A-buffer overflow at " << current
              << " layers; reserving " << next << " layers per pixel ("
              << (bytes >> 20U) << " MiB fragment buffer)" << std::endl;
    return true;
}

// 作用：校验几何输入：必须是非空三角形列表（顶点数为 3 的倍数）、坐标有限、
// 法向为单位向量（长度允许 0.99~1.01）。
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

// 作用：上传几何并（重新）创建可见性相关资源。
// 步骤：1) 校验并等待设备空闲，释放旧的 solve/可见性资源；2) 记录面元数与包围盒；
// 3) 选择边哈希容量；4) 创建可见性缓冲、上传顶点、创建描述符与光栅/解析管线。
// 副作用：会使已有的可见性图失效，需重新调用 buildVisibilityGraph。
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

// 作用：为一个可见性方向构造正交投影矩阵（列主序，视图×投影合一）。
// direction 指向虚拟相机，右/上轴由参考轴叉乘得到（|view.z|<0.9 用 z 轴，否则用 y 轴）。
// x/y 窗口半径取中心块（原始几何）包围球，使每个方向的像素面积一致，
// 即各方向的可见性计数权重相同；深度范围覆盖周期副本的包围球，映射到 [0,1]，
// 离相机越远深度越大。
std::array<float, 16> FacetrtVulkan::makeProjection(const Direction& direction) const
{
    const std::array<float, 3> view = normalize3({direction.x, direction.y, direction.z});
    const std::array<float, 3> reference = std::abs(view[2]) < 0.9f
                                               ? std::array<float, 3>{0.0f, 0.0f, 1.0f}
                                               : std::array<float, 3>{0.0f, 1.0f, 0.0f};
    const std::array<float, 3> right = normalize3(cross3(reference, view));
    const std::array<float, 3> up = cross3(view, right);
    // 步骤 1：把周期邻接瓦片（最多 20 个）的包围盒并入，得到用于深度范围的扩展包围盒。
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
    // 步骤 2：只用中心块的包围球确定 x/y 窗口（原作者说明见下）。
    // Only central-block fragments are receivers, and every occluder of a
    // central facet projects onto that facet's pixel, so the x/y window only
    // has to cover the central block. Using the central bounding sphere (the
    // same radius for every direction keeps the pixel area, hence the
    // per-direction weight of the visibility counts, constant) instead of the
    // sphere around all periodic replicas raises the effective resolution by
    // the ratio of the two radii (3-5x linear for 4-20 neighbour tiles).
    // Depth still spans the replicas so every occluder stays in [0, 1].
    const std::array<float, 3> centralCenter{
        (m_boundsMin[0] + m_boundsMax[0]) * 0.5f,
        (m_boundsMin[1] + m_boundsMax[1]) * 0.5f,
        (m_boundsMin[2] + m_boundsMax[2]) * 0.5f};
    const std::array<float, 3> centralDiagonal{
        m_boundsMax[0] - m_boundsMin[0],
        m_boundsMax[1] - m_boundsMin[1],
        m_boundsMax[2] - m_boundsMin[2]};
    const float windowRadius = std::max(
        0.5f * std::sqrt(dot3(centralDiagonal, centralDiagonal)) * 1.001f, 1.0e-4f);

    // 步骤 3：填充矩阵：x/y 为相对中心块中心的右/上投影，z 为沿视线方向的深度（0.5 居中）。
    std::array<float, 16> matrix{};
    matrix[0] = right[0] / windowRadius;
    matrix[1] = up[0] / windowRadius;
    matrix[2] = -view[0] / (2.0f * radius);
    matrix[4] = right[1] / windowRadius;
    matrix[5] = up[1] / windowRadius;
    matrix[6] = -view[1] / (2.0f * radius);
    matrix[8] = right[2] / windowRadius;
    matrix[9] = up[2] / windowRadius;
    matrix[10] = -view[2] / (2.0f * radius);
    matrix[12] = -dot3(centralCenter, right) / windowRadius;
    matrix[13] = -dot3(centralCenter, up) / windowRadius;
    matrix[14] = 0.5f + dot3(center, view) / (2.0f * radius);
    matrix[15] = 1.0f;
    return matrix;
}

// 模块：可见性阶段的设备端缓冲（全部 device-local 的 storage buffer）。
// m_fragments：A-buffer，像素数 × maxFragmentsPerPixel × 8 字节；m_pixelCounts：每像素片元数；
// m_edgeKeys/m_edgeCounts：边哈希表（uint64 键 = 一对相邻 oriented side，计数 uint32）；
// m_denominator/m_skyCounts/m_groundCounts：逐 side 的可见性采样数、天空与向下逃逸计数；
// m_visibilityStats：溢出统计（4 个 uint32；statistics[1] 为哈希溢出次数）；m_directTotal/m_directVisible：太阳直射可见性计数。
// 会先检查各缓冲是否超过 maxStorageBufferRange，超过则抛错。
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
    m_groundCounts = createBuffer(VkDeviceSize(surfaceCount()) * sizeof(uint32_t),
                                  deviceStorage, deviceLocal);
    m_visibilityStats = createBuffer(4U * sizeof(uint32_t), deviceStorage, deviceLocal);
    m_directTotal = createBuffer(VkDeviceSize(surfaceCount()) * sizeof(uint32_t),
                                 deviceStorage, deviceLocal);
    m_directVisible = createBuffer(VkDeviceSize(surfaceCount()) * sizeof(uint32_t),
                                   deviceStorage, deviceLocal);
}

// 模块：可见性阶段的描述符。两个 8 绑定的 storage-buffer 集合：
//   集合 0（m_visibilityDescriptorSet，建图）：fragments, pixelCounts, edgeKeys, edgeCounts,
//     denominator, skyCounts, visibilityStats, groundCounts；
//   集合 1（m_directDescriptorSet，太阳可见性）：同上，但 denominator/skyCounts 换成
//     directTotal/directVisible。绑定编号必须与 visibility*.comp / direct_resolve.comp 一致。
void FacetrtVulkan::createVisibilityDescriptors()
{
    std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
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

    const std::array<Buffer*, 8> buffers{&m_fragments, &m_pixelCounts, &m_edgeKeys,
                                         &m_edgeCounts, &m_denominator, &m_skyCounts,
                                         &m_visibilityStats, &m_groundCounts};
    std::array<VkDescriptorBufferInfo, 8> bufferInfos{};
    std::array<VkWriteDescriptorSet, 8> writes{};
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

    const std::array<Buffer*, 8> directBuffers{&m_fragments, &m_pixelCounts, &m_edgeKeys,
                                               &m_edgeCounts, &m_directTotal,
                                               &m_directVisible, &m_visibilityStats,
                                               &m_groundCounts};
    std::array<VkDescriptorBufferInfo, 8> directInfos{};
    std::array<VkWriteDescriptorSet, 8> directWrites{};
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

// 模块：可见性管线。1) 光栅管线（visibility.vert/frag）：无颜色/深度附件，不剔除背面，
// 视口与裁剪为动态状态，片元着色器把每个片元写入 A-buffer；
// 2) 计算管线 visibility_resolve（逐像素深度排序并登记相邻 oriented side 的边）；
// 3) 计算管线 direct_resolve（太阳方向的直射可见性）。三者共用 VisibilityPush 的 push constant。
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

    // 渲染通道没有任何附件：光栅阶段只为产生片元，结果通过 storage buffer 输出。
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

// 模块：辐射度求解阶段的设备端缓冲。
// CSR 行偏移与边（CsrEdge{neighbor, count}）、逐 side 的光学参数 / 两份 ping-pong 辐射度 /
// 入射辐照度，以及等效地面的 side 索引、权重和均值（4 个 float，初始为 0）。
// 无边或无地面时分配最小占位缓冲，避免零长度。
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
    const size_t groundSides = m_ground.sides.size();
    m_groundSides = createBuffer(VkDeviceSize(std::max<size_t>(groundSides, 1U)) * sizeof(uint32_t), usage, memory);
    m_groundWeights = createBuffer(VkDeviceSize(std::max<size_t>(groundSides, 1U)) * sizeof(float), usage, memory);
    m_groundMean = createBuffer(4U * sizeof(float), usage, memory);
    if (groundSides != 0U) {
        uploadBuffer(m_groundSides, m_ground.sides.data(), groundSides * sizeof(uint32_t));
        uploadBuffer(m_groundWeights, m_ground.weights.data(), groundSides * sizeof(float));
    }
    const std::array<float, 4> noGround{};
    uploadBuffer(m_groundMean, noGround.data(), sizeof(noGround));

    uploadBuffer(m_rowOffsets, m_rowOffsetsCpu.data(),
                 m_rowOffsetsCpu.size() * sizeof(uint32_t));
    if (!m_edgesCpu.empty()) {
        uploadBuffer(m_edges, m_edgesCpu.data(), m_edgesCpu.size() * sizeof(CsrEdge));
    } else {
        const CsrEdge empty{};
        uploadBuffer(m_edges, &empty, sizeof(empty));
    }
}

// 模块：求解阶段描述符（13 个 storage buffer，仅计算阶段可见）。绑定顺序：
//   0 rowOffsets, 1 edges, 2 denominator, 3 skyCounts, 4 optics, 5 radiosityA,
//   6 radiosityB, 7 residual, 8 incident, 9 groundCounts, 10 groundSides,
//   11 groundWeights, 12 groundMean（须与 radiosity/incident/energy 等着色器一致）。
void FacetrtVulkan::createSolveDescriptors()
{
    std::array<VkDescriptorSetLayoutBinding, 13> bindings{};
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

    const std::array<Buffer*, 13> buffers{&m_rowOffsets, &m_edges, &m_denominator,
                                          &m_skyCounts, &m_optics, &m_radiosityA,
                                          &m_radiosityB, &m_residual, &m_incident,
                                          &m_groundCounts, &m_groundSides, &m_groundWeights,
                                          &m_groundMean};
    std::array<VkDescriptorBufferInfo, 13> infos{};
    std::array<VkWriteDescriptorSet, 13> writes{};
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

// 模块：求解阶段的计算管线，共用 SolvePush 的 push constant 与求解描述符布局。
// radiosity.comp：Jacobi 迭代；ground_mean.comp：等效地面均值归约；
// incident.comp：入射辐照度，仅在设备支持 shaderFloat64 时创建；
// 此外 energy.comp.spv 存在时创建 GPU 能量平衡资源（同样要求 shaderFloat64）。
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
    {
        const VkShaderModule groundModule = loadShader("ground_mean.comp.spv");
        const ShaderModuleGuard guard{m_device, groundModule};
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = groundModule;
        info.stage.pName = "main";
        info.layout = m_solvePipelineLayout;
        require(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr,
                                         &m_groundPipeline),
                "vkCreateComputePipelines(ground_mean)");
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

// 作用：构建可见性图（对外入口），带自动恢复：
// 边哈希表溢出时把容量翻倍并重传几何后重建；A-buffer 溢出时通过 growRasterLayers() 加层后重建；
// 已达设备上限则重新抛出异常。同时保存方向列表，供之后太阳可见性溢出时重建同一张图。
GraphDiagnostics FacetrtVulkan::buildVisibilityGraph(
    const std::vector<Direction>& directions)
{
    m_visibilityDirections = directions;
    for (;;) {
        try {
            return buildVisibilityGraphOnce(directions);
        } catch (const EdgeHashOverflow&) {
            // Distinct visible facet pairs exceeded the table: double it and rebuild.
            if (m_hashCapacity >= maximumHashCapacity()) throw;
            m_config.edgeHashCapacity = m_hashCapacity * 2U;
            const std::vector<Vertex> geometry = m_geometry;
            setGeometry(geometry);
        } catch (const RasterLayerOverflow&) {
            // Dense canopies overflow the per-pixel fragment list long before
            // the hash table: reserve more layers and rebuild the resources.
            if (!growRasterLayers()) throw;
            const std::vector<Vertex> geometry = m_geometry;
            setGeometry(geometry);
        }
    }
}

// 作用：构建一次可见性图（不含溢出重试，重试见 buildVisibilityGraph）。
// 输入：一组等立体角权重的方向（指向虚拟相机）；输出：GraphDiagnostics，
// 并把 CSR 边表、逐 side 分母/天空/地面计数和等效地面保存在成员中，同时建好求解资源。
// 步骤：1) 准备 push constant；2) 清零计数与哈希表；3) 逐方向光栅 + 解析；
// 4) 回读哈希表/计数并检查溢出；5) 取出无向边；6) 展开为有向边并建 CSR；
// 7) 应用下边界并回传计数；8) 计算闭合误差诊断；9) 创建求解资源/描述符/管线。
// 溢出时抛 RasterLayerOverflow 或 EdgeHashOverflow（片元溢出优先）。
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

    // 步骤 1：push constant：光栅尺寸、每像素最大层数、哈希容量，以及周期域尺寸与邻接瓦片数。
    // viewProjection/viewDirection 在每个方向的循环里填写。
    VisibilityPush push{};
    push.raster[0] = m_config.rasterWidth;
    push.raster[1] = m_config.rasterHeight;
    push.raster[2] = m_config.maxFragmentsPerPixel;
    push.raster[3] = m_hashCapacity;
    push.periodic[0] = m_config.periodicSizeX;
    push.periodic[1] = m_config.periodicSizeZ;
    push.periodic[2] = static_cast<float>(m_config.periodicNeighborCount);

    VkCommandBuffer commandBuffer = beginCommands();
    // 步骤 2：清零累积缓冲；边键填 0xFFFFFFFF（即 kEmptyEdgeKey，表示空槽），
    // 各方向的计数在 GPU 上累加，直到全部方向处理完才回读。
    vkCmdFillBuffer(commandBuffer, m_edgeKeys.buffer, 0, m_edgeKeys.size, 0xffffffffU);
    vkCmdFillBuffer(commandBuffer, m_edgeCounts.buffer, 0, m_edgeCounts.size, 0U);
    vkCmdFillBuffer(commandBuffer, m_denominator.buffer, 0, m_denominator.size, 0U);
    vkCmdFillBuffer(commandBuffer, m_skyCounts.buffer, 0, m_skyCounts.size, 0U);
    vkCmdFillBuffer(commandBuffer, m_groundCounts.buffer, 0, m_groundCounts.size, 0U);
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

    // 步骤 3：逐方向：清 pixelCounts → 光栅化全部面元写入 A-buffer → 计算着色器解析并累加。
    // 每个方向一个命令缓冲，同步提交。
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
        // 每个方向前清零每像素片元计数 pixelCounts。
        VkMemoryBarrier countClear{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        countClear.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        countClear.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             1, &countClear, 0, nullptr, 0, nullptr);

        // 光栅：instance 数 = 周期邻接瓦片数 + 1（本体 + 邻接复制体，复制体只参与遮挡），
        // 邻接数最多取 20（与 periodicTileOffset 表一致）。
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

        // 解析：visibility_resolve 每线程一个像素（工作组 64 线程），排序片元并写边哈希/计数。
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
        resolveToNext.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                      VK_ACCESS_SHADER_READ_BIT |
                                      VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &resolveToNext, 0, nullptr, 0, nullptr);
        endCommands(commandBuffer);
    }

    // 步骤 4：回读哈希表、各 side 计数与统计量。statistics[0] = A-buffer 溢出，
    // [1] = 哈希表溢出，[2] = 溢出时观察到的最深片元列表长度（用于下次扩层）。
    // DEFERRED[中] 内存: 整张边哈希表（容量可达 maxStorageBufferRange/8，即最多 ~512M 项 → 4 GiB 键 + 2 GiB 计数）一次性回读到主机，另外 downloadBuffer 还要分配同样大小的 host-visible staging，容易耗尽内存；建议：在 GPU 上先压缩非空槽（prefix-sum/compaction）再回读，或分块回读。
    std::vector<uint64_t> edgeKeys(m_hashCapacity);
    std::vector<uint32_t> edgeCounts(m_hashCapacity);
    std::array<uint32_t, 4> statistics{};
    m_denominatorCpu.resize(surfaceCount());
    m_skyCountsCpu.resize(surfaceCount());
    downloadBuffer(m_edgeKeys, edgeKeys.data(), edgeKeys.size() * sizeof(uint64_t));
    downloadBuffer(m_edgeCounts, edgeCounts.data(), edgeCounts.size() * sizeof(uint32_t));
    downloadBuffer(m_denominator, m_denominatorCpu.data(),
                   m_denominatorCpu.size() * sizeof(uint32_t));
    downloadBuffer(m_skyCounts, m_skyCountsCpu.data(),
                   m_skyCountsCpu.size() * sizeof(uint32_t));
    m_groundCountsCpu.resize(surfaceCount());
    downloadBuffer(m_groundCounts, m_groundCountsCpu.data(),
                   m_groundCountsCpu.size() * sizeof(uint32_t));
    downloadBuffer(m_visibilityStats, statistics.data(), sizeof(statistics));

    GraphDiagnostics diagnostics{};
    diagnostics.facetCount = m_facetCount;
    diagnostics.surfaceCount = surfaceCount();
    diagnostics.fragmentOverflow = statistics[0];
    diagnostics.hashOverflow = statistics[1];
    if (diagnostics.fragmentOverflow != 0U) {
        m_requiredRasterLayers = std::max(m_requiredRasterLayers, statistics[2]);
        throw RasterLayerOverflow();
    }
    if (diagnostics.hashOverflow != 0U) {
        throw EdgeHashOverflow();
    }

    // 步骤 5：收集非空槽为无向边：键高 32 位为 sideA，低 32 位为 sideB，值为射线计数；
    // side 下标越界说明 GPU 结果损坏，直接报错。
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

    // 步骤 6：把无向边展开为双向的有向边并建 CSR：先统计每行度数并前缀和得到行偏移，
    // 再按键排序后填充，使浮点累加顺序可复现。
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
    // Hash-slot placement depends on concurrent insertion; canonical edge order
    // makes CSR floating-point accumulation reproducible between paired runs.
    std::sort(undirectedEdges.begin(),undirectedEdges.end(),
        [](const auto& a,const auto& b){return a.first<b.first;});
    std::vector<uint32_t> cursor = m_rowOffsetsCpu;
    for (const auto& entry : undirectedEdges) {
        const uint32_t sideA = static_cast<uint32_t>(entry.first >> 32U);
        const uint32_t sideB = static_cast<uint32_t>(entry.first);
        const uint32_t count = entry.second;
        m_edgesCpu[cursor[sideA]++] = {sideB, count};
        m_edgesCpu[cursor[sideB]++] = {sideA, count};
    }
    diagnostics.directedEdgeCount = static_cast<uint32_t>(m_edgesCpu.size());
    // 步骤 7：没有任何 side 有采样则无法求解（报错）；之后应用下边界，
    // 并把调整后的分母/天空/地面计数写回 GPU。
    if (std::none_of(m_denominatorCpu.begin(), m_denominatorCpu.end(), [](uint32_t n){return n>0;}))
        throw std::runtime_error("Visibility raster has no surface samples; radiation results are invalid");
    // Lower boundary: downward escapes see the equivalent ground and the
    // soil-facing ground sides leave the solve. The adjusted counts replace
    // the device copies the solve descriptors read.
    m_ground = applyGroundBoundary(m_geometry, m_firstGroundFacet, m_denominatorCpu,
                                   m_skyCountsCpu, m_groundCountsCpu, diagnostics);
    // Without any downward escape the per-sweep reduction is skipped: the
    // sweeps are then exactly those of a scene without a lower boundary.
    m_groundUsed = std::any_of(m_groundCountsCpu.begin(), m_groundCountsCpu.end(),
                               [](uint32_t n) { return n != 0U && n != kInactiveSide; });
    uploadBuffer(m_denominator, m_denominatorCpu.data(), m_denominatorCpu.size() * sizeof(uint32_t));
    uploadBuffer(m_skyCounts, m_skyCountsCpu.data(), m_skyCountsCpu.size() * sizeof(uint32_t));
    uploadBuffer(m_groundCounts, m_groundCountsCpu.data(), m_groundCountsCpu.size() * sizeof(uint32_t));

    // 步骤 8：闭合误差诊断：对每个有效 side，（天空计数 + 地面逃逸计数 + 全部邻边计数）
    // 应与分母相等，取 |比值 − 1| 的最大值写入 maxClosureError；无样本的 side 跳过。
    float maxClosureError = 0.0f;
    for (uint32_t side = 0; side < surfaceCount(); ++side) {
        if (m_groundCountsCpu[side] == kInactiveSide) continue;
        uint64_t visibleCount = uint64_t(m_skyCountsCpu[side]) + m_groundCountsCpu[side];
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

    // 步骤 9：用新的 CSR 数据创建求解缓冲、描述符与管线。
    createSolveResources(diagnostics.directedEdgeCount);
    createSolveDescriptors();
    createSolvePipeline();
    return diagnostics;
}

// 作用：计算每个 oriented side 的直射日照比例（对外入口）。
// A-buffer 溢出（低太阳高度角会使每像素片元更多）时扩层、重建几何与可见性图后重试。
std::vector<float> FacetrtVulkan::computeSunlitFraction(const Direction& sunDirection)
{
    for (;;) {
        try {
            return computeSunlitFractionOnce(sunDirection);
        } catch (const RasterLayerOverflow&) {
            // A grazing sun stacks more fragments per pixel than the 40-direction
            // hemisphere graph did. Reserve more layers, rebuild the graph the
            // solve resources depend on, and retry this direction.
            if (!growRasterLayers()) throw;
            const std::vector<Vertex> geometry = m_geometry;
            setGeometry(geometry);
            if (!m_visibilityDirections.empty()) {
                const std::vector<Direction> directions = m_visibilityDirections;
                buildVisibilityGraph(directions);
            }
        }
    }
}

// 作用：沿 sunDirection（指向太阳）做一次直射可见性光栅，返回长度 2×面元数的比例数组：
// visible/total，其中 total 为落在该 side 上的样本数，visible 为其中最近（未被遮挡）的样本数。
// 无样本的 side 返回 NaN，由调用者用邻近面估计。
// 为了让小于一个像素的面元也有样本，对栅格做 4×4 的亚像素抖动，共 16 次累加。
std::vector<float> FacetrtVulkan::computeSunlitFractionOnce(const Direction& sunDirection)
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
    // 步骤 1：清零直射计数与统计量，16 次抖动的结果累加在同一缓冲中。
    vkCmdFillBuffer(commandBuffer, m_directTotal.buffer, 0, m_directTotal.size, 0U);
    vkCmdFillBuffer(commandBuffer, m_directVisible.buffer, 0, m_directVisible.size, 0U);
    vkCmdFillBuffer(commandBuffer, m_visibilityStats.buffer, 0, m_visibilityStats.size, 0U);

    const VkDeviceSize vertexOffset = 0;
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(m_config.rasterWidth),
                              static_cast<float>(m_config.rasterHeight), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {m_config.rasterWidth, m_config.rasterHeight}};
    const uint64_t pixels = uint64_t(m_config.rasterWidth) * m_config.rasterHeight;
    // 步骤 2：亚像素抖动循环。每次把 viewProjection 平移半个像素范围内的偏移（NDC 单位），
    // 清 pixelCounts → 光栅（使用 m_directDescriptorSet）→ direct_resolve 累加 total/visible。
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
    VkMemoryBarrier readback{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    readback.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    readback.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,1,&readback,0,nullptr,0,nullptr);
    endCommands(commandBuffer);

    // 步骤 3：回读计数；片元溢出（statistics[0]）则记录所需层数并抛 RasterLayerOverflow。
    std::vector<uint32_t> total(surfaceCount());
    std::vector<uint32_t> visible(surfaceCount());
    std::array<uint32_t, 4> statistics{};
    downloadBuffer(m_directTotal, total.data(), total.size() * sizeof(uint32_t));
    downloadBuffer(m_directVisible, visible.data(), visible.size() * sizeof(uint32_t));
    downloadBuffer(m_visibilityStats, statistics.data(), sizeof(statistics));
    if (statistics[0] != 0U) {
        m_requiredRasterLayers = std::max(m_requiredRasterLayers, statistics[2]);
        throw RasterLayerOverflow();
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

// 作用：校验光学参数：长度必须是 2×面元数；值有限且非负；反射率+透射率 <= 1.0001（容差）；
// emission 允许为负，但 emission + reflectance×directIrradiance 不得为负（见内部说明）。
void FacetrtVulkan::validateOptics(const std::vector<SurfaceOptics>& optics) const
{
    if (optics.size() != surfaceCount()) {
        throw std::invalid_argument("Optics array must contain exactly two sides per facet");
    }
    for (const SurfaceOptics& value : optics) {
        // A Hapke background reflects the beam with its own albedo, carried as
        // the source (beam - diffuse albedo) * E, which may be negative; the
        // reflected beam reflectance * E + emission must not be.
        if (!std::isfinite(value.reflectance) || !std::isfinite(value.transmittance) ||
            !std::isfinite(value.emission) || !std::isfinite(value.directIrradiance) ||
            value.reflectance < 0.0f || value.transmittance < 0.0f ||
            (value.emission < 0.0f &&
             value.emission + value.reflectance * value.directIrradiance < -1.0e-6f *
                 std::max(1.0f, value.directIrradiance)) ||
            value.directIrradiance < 0.0f ||
            value.reflectance + value.transmittance > 1.0001f) {
            throw std::invalid_argument(
                "Optical values must be finite, non-negative, and reflectance+transmittance<=1");
        }
    }
}

// 作用：录制一次完整的辐射度求解：两份 ping-pong 辐射度与残差清零（初值为 0），
// 从 ping=0 做 iterations 次 Jacobi 迭代，再计算入射辐照度。
// 返回最终结果所在的 ping 缓冲下标（0 表示 radiosityA，1 表示 radiosityB，待核对）。
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

// 作用：录制 iterations 次 Jacobi 辐射度扫描（radiosity.comp，每线程处理一个面元的两侧）。
// 输入：ping = 当前读取的缓冲（0=radiosityA，1=radiosityB），每次扫描写另一份后翻转；
// 返回最后一次扫描所写缓冲的下标（即当前迭代值所在缓冲）。
// 每次扫描先清零 residual（存放本次最大变化量，float 位模式），有地面时先更新等效地面均值
// （保证与 Jacobi 迭代一致）；indirect=true 时工作组数取自 m_spectralDispatch（光谱加速模式）。
// 副作用：++m_solveGeneration，使 evaluateEnergy 不再沿用旧的热启动结果。
uint32_t FacetrtVulkan::recordIterations(VkCommandBuffer commandBuffer, uint32_t ping, float skyRadiosity,
                                        uint32_t iterations, float relaxation, bool indirect)
{
    ++m_solveGeneration;
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

        // The equivalent ground follows the iterate (Jacobi-consistent).
        if (m_groundUsed) recordGroundMean(commandBuffer, ping, skyRadiosity);
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

// 作用：计算/传输之间的全局内存屏障，也让结果对主机读取与间接调度可见。
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

// 作用：用 ground_mean.comp（单个工作组）把 ping 缓冲中地面朝上侧按面积权重求均值，
// 写入 m_groundMean（等效地面辐射度）；结束后重新绑定 radiosity 管线，供后续扫描使用。
void FacetrtVulkan::recordGroundMean(VkCommandBuffer commandBuffer, uint32_t ping, float skyRadiosity)
{
    SolvePush push{m_facetCount, ping, skyRadiosity, 1.0f};
    push.groundSideCount = static_cast<uint32_t>(m_ground.sides.size());
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_groundPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_solvePipelineLayout, 0, 1,
                            &m_solveDescriptorSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_solvePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commandBuffer, 1, 1, 1);
    VkMemoryBarrier meanBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    meanBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    meanBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                         &meanBarrier, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_solvePipeline);
}

// 作用：回读最近一次求解/入射计算得到的等效地面辐射度；尚未创建缓冲时返回 0。
float FacetrtVulkan::groundRadiosity() const
{
    if (m_groundMean.buffer == VK_NULL_HANDLE) return 0.0f;
    float value = 0.0f;
    downloadBuffer(m_groundMean, &value, sizeof(value));
    return value;
}

// 作用：录制末态的等效地面均值，并在有 incident 管线时按 side 计算入射辐照度（写 m_incident）。
// 没有 incident 管线（设备无 shaderFloat64）时只更新地面均值，入射需由 CPU 的
// incidentIrradiance() 计算。
void FacetrtVulkan::recordIncident(VkCommandBuffer commandBuffer, uint32_t ping, float skyRadiosity)
{
    // Final equivalent ground of this solve (incident pass and diagnostics).
    computeBarrier(commandBuffer);
    recordGroundMean(commandBuffer, ping, skyRadiosity);
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

// 模块：GPU 能量平衡（energy.comp）的资源。布局与 faceteb/energy.comp 的头注释一致：
//   0 参数 P（每面元 32 float）、1 输入 I（每（状态,侧）12 float，状态 0=日照 1=阴影）、
//   2 温度 T（每（状态,侧）1 float）、3 通量 F（每（状态,侧）12 float）、
//   4 optics 与 5 incident（与求解阶段共用）、6 stats（7 个 uint32，主机可见，用于读回）。
// push constant 为 EnergyPush（48 字节）。
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
    m_energyStats = createBuffer(7 * sizeof(uint32_t), usage,
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

// 作用：释放能量平衡的管线、描述符与缓冲，并清空句柄。
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

// 作用：开始一个能量平衡节点：校验并上传面元参数、输入和初始温度，保存 push constant。
// 校验：GPU 能量可用；数组长度与面元数匹配（参数 32/面元，输入 12×2 个/side，
// 温度 2 个/side）；push.facets 等于面元数；method 只能是 0 或 1；dt、pressure、relaxation
// 必须为正；所有数值有限。
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
// 作用：仅重新上传输入 I（每（状态,侧）12 个 float，各分量含义见 energy.comp 头注释）。
void FacetrtVulkan::updateEnergyInputs(const std::vector<float> &inputs)
{
    if (inputs.size() != size_t(surfaceCount()) * 2 * 12)
        throw std::invalid_argument("GPU energy input dimensions");
    uploadBuffer(m_energyInputs, inputs.data(), inputs.size() * sizeof(float));
}
// 作用：录制一次 energy.comp 调度（每线程一个面元），phase 取值：
// 0 = 计算通量与残差统计，1 = 牛顿更新温度，2 = 由当前温度更新长波光学参数。
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
// 作用：读回 energy.comp 写入的 7 个统计量：stats[0..4] 为 EnergyStatistics::maxima
// （最大残差、最大残差/响应、最大气孔失配、最大叶残差、最大温度步长），stats[6] 为能量不闭合
// 面元数；stats[5] 非 0 表示出现非有限值或无效生理量，此时抛 runtime_error。
EnergyStatistics FacetrtVulkan::readEnergyStatistics()
{
    void *mapped = nullptr;
    require(vkMapMemory(m_device, m_energyStats.memory, 0, m_energyStats.size, 0, &mapped), "energy statistics map");
    std::array<uint32_t, 7> bits{};
    std::memcpy(bits.data(), mapped, sizeof(bits));
    vkUnmapMemory(m_device, m_energyStats.memory);
    if (bits[5])
        throw std::runtime_error("GPU energy calculation produced a nonfinite state or invalid physiology");
    EnergyStatistics values;
    std::memcpy(values.maxima.data(), bits.data(), sizeof(values.maxima));
    values.failingFacets = bits[6];
    return values;
}
// 作用：对当前温度做一次“长波辐射 + 能量通量”评估，返回残差统计（供外层耦合迭代判断收敛）。
// 步骤：清统计 → phase 2 更新长波 optics → 长波 Jacobi 求解（冷启动 64 次扫描；
// 沿用上次结果时 16 次）→ phase 0 计算通量与残差 → 读回统计。
EnergyStatistics FacetrtVulkan::evaluateEnergy(float skyRadiosity)
{
    VkCommandBuffer cmd = beginCommands();
    vkCmdFillBuffer(cmd, m_energyStats.buffer, 0, m_energyStats.size, 0);
    recordEnergy(cmd, 2);
    // 步骤：长波辐射求解。仅当自上次 evaluateEnergy 以来没有其他求解动过 ping-pong 缓冲
    // （m_solveGeneration 未变）才热启动，否则冷启动。
    // Longwave Jacobi. The first evaluation after any other solve starts cold
    // with 64 sweeps. Later coupling iterations start from the previous
    // longwave radiosity, which only differs through the temperature update;
    // the Jacobi error contracts by max(r+t) (~0.02-0.1 in the thermal
    // infrared) per sweep, so 16 warm sweeps leave a residual many orders
    // below the 2 W/m2 energy criterion while cutting the dominant per-
    // iteration GPU cost by 4x.
    constexpr uint32_t coldIterations = 64;
    constexpr uint32_t warmIterations = 16;
    const bool warm = m_energyWarmGeneration != 0 &&
        m_energyWarmGeneration == m_solveGeneration;
    if (warm) {
        m_energyRadiosityPing = recordIterations(
            cmd, m_energyRadiosityPing, skyRadiosity, warmIterations, 1.0f);
        recordIncident(cmd, m_energyRadiosityPing, skyRadiosity);
        m_energyIterations = warmIterations;
    } else {
        m_energyRadiosityPing = recordSolve(cmd, skyRadiosity, coldIterations, 1.0f);
        m_energyIterations = coldIterations;
    }
    m_energyWarmGeneration = m_solveGeneration;
    recordEnergy(cmd, 0);
    endCommands(cmd);
    return readEnergyStatistics();
}
// 作用：执行 phase 1 的牛顿温度更新，返回本步最大温度步长 stats[4]（K，用于判断收敛）。
float FacetrtVulkan::updateEnergyTemperature()
{
    VkCommandBuffer cmd = beginCommands();
    vkCmdFillBuffer(cmd, m_energyStats.buffer, 0, m_energyStats.size, 0);
    recordEnergy(cmd, 1);
    endCommands(cmd);
    return readEnergyStatistics().maxima[4];
}
// 作用：读回温度与通量；includeRadiation=true 时还读回 optics、末态辐射度（取最后一次
// evaluateEnergy 所在 ping 缓冲）、入射辐照度、最大变化量、迭代次数与等效地面辐射度。
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
        // Read the buffer that holds the final iterate of the last
        // evaluateEnergy solve (recordSolve's returned ping: 0 = A, 1 = B).
        downloadBuffer(m_energyRadiosityPing == 0U ? m_radiosityA : m_radiosityB,
                       result.radiation.radiosity.data(), surfaceCount() * sizeof(float));
        downloadBuffer(m_incident, result.radiation.incident.data(), surfaceCount() * sizeof(float));
        uint32_t delta{};
        downloadBuffer(m_residual, &delta, sizeof(delta));
        std::memcpy(&result.radiation.maxDelta, &delta, sizeof(delta));
        result.radiation.iterations = m_energyIterations;
        result.radiation.groundRadiosity = groundRadiosity();
    }
    return result;
}

// 作用：固定迭代次数的辐射度求解（Jacobi，初值为 0）。
// 输入：逐 side 的光学参数、天空辐射度（非负）、迭代数、松弛因子 relaxation ∈ (0,1]；
// 输出：辐射度、最后一次扫描的最大变化量 maxDelta、（有 incident 管线时）入射辐照度。
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
    result.groundRadiosity = groundRadiosity();
    return result;
}

// 作用：带收敛判据的辐射度求解：每批 batchIterations 次扫描后回读 maxDelta，
// 不超过 tolerance 或达到 maximumIterations 即停止；可给定初值 initialRadiosity 热启动。
// 步骤：1) 校验输入；2) 初始化 ping-pong 缓冲；3) 分批迭代；4) 计算入射并回读结果。
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
    // 步骤 2：初始化两份 ping-pong 缓冲：有初值则都写入初值，否则清零（光学参数已在上面上传）。
    ++m_solveGeneration; // overwrites the ping-pong buffers below
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
    // 步骤 3：分批迭代；每批结束同步一次并回读最大变化量，用于判断是否已收敛。
    while (result.iterations < maximumIterations) {
        const uint32_t count = std::min(
            batchIterations, maximumIterations - result.iterations);
        VkCommandBuffer commandBuffer = beginCommands();
        ping = recordIterations(commandBuffer, ping, skyRadiosity, count, relaxation);
        endCommands(commandBuffer);
        result.iterations += count;

        uint32_t deltaBits = 0U;
        downloadBuffer(m_residual, &deltaBits, sizeof(deltaBits));
        std::memcpy(&result.maxDelta, &deltaBits, sizeof(result.maxDelta));
        if (result.maxDelta <= tolerance) {
            break;
        }
    }

    // 步骤 4：计算入射辐照度与等效地面，回读最终辐射度与入射辐照度。
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
    result.groundRadiosity = groundRadiosity();
    return result;
}

// 作用：CPU 版入射辐照度（GPU 已有结果 solution.incident 时直接返回）。
// 公式：E = max(0, [N_sky·L_sky + N_ground·L_ground + Σ_j count_j·B_j] / N + E_direct)，
// 其中 N 为该 side 的采样总数（denominator），B_j 为邻接 side 的辐射度。
// 分母为 0（无栅格样本）的 side 按全天空处理（diffuse = skyRadiosity，见 buildVisibilityGraphOnce 中的 DEFERRED 说明）；
// 失效的地面背侧只取直射项。
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
    const double groundValue = groundBoundaryRadiosity(m_ground, solution.radiosity, skyRadiosity);
    for (uint32_t side = 0; side < surfaceCount(); ++side) {
        if (m_groundCountsCpu[side] == kInactiveSide) {
            irradiance[side] = std::max(0.0f, optics[side].directIrradiance);
            continue;
        }
        double diffuse = skyRadiosity;
        const uint32_t denominator = m_denominatorCpu[side];
        if (denominator != 0U) {
            diffuse = static_cast<double>(m_skyCountsCpu[side]) *
                      static_cast<double>(skyRadiosity) +
                      static_cast<double>(m_groundCountsCpu[side]) * groundValue;
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

// 作用：开始一次光谱求解会话：上传面元记录与材料光谱，创建 spectral.comp 管线。
// 输入：facets = 每面元 12 个 float（其后可接 Hapke 查表）；materialBands = 每（波段,材料）
// 4 个 float（反射率、透射率、日照侧热发射、阴影侧热发射）；sun = 太阳方向；
// backgroundStrength = Hapke 背景强度；beamIrradiance = 光束法向辐照度。
// 输出缓冲 m_spectralOutput 大小为 (3×bands+1)×side 数 个 float：
//   [band*n+s] 辐射度；[bands*n+2*(band*n+s)+{0,1}] 亮/阴状态；[3*bands*n+s] 漫射模式的直射分量。
// 步骤：1) 校验输入；2) 保存 push 默认值；3) 创建缓冲并上传；4) 创建 10 绑定的描述符；
// 5) 创建管线；6) 清零输出。布局须与 spectral.comp 的头注释一致。
void FacetrtVulkan::beginSpectral(const std::vector<float>& facets, const std::vector<float>& materialBands,
                                 uint32_t bands, uint32_t materials, const Direction& sun, float backgroundStrength,
                                 float beamIrradiance)
{
    // 步骤 1：校验：需已建好求解管线；面元记录不少于 12×面元数；材料数组大小 = 波段×材料×4；全部有限。
    // Facet records (12 floats each), optionally followed by the Hapke tables.
    if (m_solvePipeline == VK_NULL_HANDLE || facets.size() < size_t(m_facetCount) * 12U || bands == 0 ||
        materials == 0 || materialBands.size() != size_t(bands) * materials * 4U)
        throw std::invalid_argument("Invalid spectral facet/material layout");
    for (float v : facets) if (!std::isfinite(v)) throw std::invalid_argument("Non-finite spectral facet");
    for (float v : materialBands) if (!std::isfinite(v)) throw std::invalid_argument("Invalid material spectrum");
    // 步骤 2：释放旧会话并初始化 push（phase=0、scale=1、thermal=0）。
    destroySpectralResources();
    static_assert(sizeof(SpectralPush) == 56);
    m_spectralPush = {0, 0, m_facetCount, materials, sun.x, sun.y, sun.z, backgroundStrength,
                      beamIrradiance, 1, 0, 0, bands, 0};
    const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const auto device = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    // 步骤 3：创建缓冲。输出缓冲不得超过 maxStorageBufferRange，且元素下标须能放进 uint32。
    // m_spectralConvergence 为主机可见（迭代数、最大变化量）；m_spectralDispatch 是间接调度参数。
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
    // 步骤 4：描述符绑定：0 面元，1 材料，2 optics（与求解共用），3 发射，4/5 辐射度 A/B，
    // 6 上次物理辐射度（热启动），7 输出，8 间接调度参数，9 residual（与求解共用）。
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
    // 步骤 5：创建 spectral.comp 管线（着色器模块创建后立即销毁）。
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
    // 步骤 6：输出缓冲清零。
    vkCmdFillBuffer(cmd, m_spectralOutput.buffer, 0, m_spectralOutput.size, 0U);
    computeBarrier(cmd); endCommands(cmd);
}

// 作用：录制一次 spectral.comp 调度（每线程一个 side）；phase 见 solveSpectralBand。
void FacetrtVulkan::recordSpectral(VkCommandBuffer commandBuffer, const SpectralPush& push)
{
    computeBarrier(commandBuffer);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spectralPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spectralLayout, 0, 1, &m_spectralSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_spectralLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commandBuffer, (surfaceCount() + 63U) / 64U, 1, 1);
    computeBarrier(commandBuffer);
}

// 作用：求解一个光谱波段并把结果写入 m_spectralOutput（用 readSpectralOutput 读回）。
// 参数：thermal=热红外波段；sky=天空辐射度；scale=输出缩放；accelerated=用收敛判据的
// 间接调度（否则固定 64 次扫描）；warmStart=用上次结果作初值（仅加速模式）；
// diffuse=只做漫射求解（仅 band 0）；beam>=0 覆盖该波段的光束辐照度。
// spectral.comp 的 phase：0 生成 optics/发射，4 漫射模式清直射项，5/6 加速模式的间接调度
// 初始化/更新，2 输出辐射度与亮阴状态，3 漫射输出。
// 步骤：1) 设置 push；2) phase 0/4；3) 设初值；4) 迭代；5) 入射 + 输出；6) 读回收敛信息。
SolveResult FacetrtVulkan::solveSpectralBand(uint32_t band, bool thermal, float sky, float scale,
                                           bool accelerated, bool warmStart, bool diffuse, float beam)
{
    if (m_spectralPipeline == VK_NULL_HANDLE || band >= m_spectralPush.bands || !std::isfinite(sky) || sky < 0 ||
        !std::isfinite(scale) || scale <= 0 || (diffuse && band != 0) || !std::isfinite(beam))
        throw std::invalid_argument("Invalid GPU spectral band solve");
    auto push = m_spectralPush;
    // 步骤 1：以 beginSpectral 的 push 为基础设置波段、热红外标志、光束与缩放。
    // Per-band beam-normal irradiance (beam < 0: the value from beginSpectral).
    // Thermal bands pass their 2.5-6 um solar beam in radiance units (0 above
    // 6 um), so the shader applies it exactly like the optical beam.
    const float bandBeam = beam >= 0.0f ? beam : (thermal ? 0.0f : m_spectralPush.beam);
    push.band = band; push.thermal = thermal; push.beam = bandBeam; push.scale = scale;
    push.phase = diffuse ? 4U : 0U;
    // 步骤 2：录制 phase 0（漫射模式为 4）生成该波段的光学参数与发射。
    auto cmd = beginCommands();
    recordSpectral(cmd, push);
    // 步骤 3：初值：加速 + 热启动时用上次物理辐射度作为 A/B 初值，否则清零。
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
    // 步骤 4：迭代。加速模式：phase 5 初始化间接调度参数，随后 8 批（每批 8 次扫描 + phase 6
    // 更新收敛状态），把迭代数与最大变化量从 m_spectralDispatch 拷到 convergence 缓冲
    // （具体收敛逻辑见 spectral.comp，待核对）；否则固定 64 次扫描并拷贝 residual。
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
    // 步骤 5：计算入射辐照度与等效地面，再用 phase 2（漫射为 3）把辐射度整理进输出缓冲；
    // 提交后读回迭代数与最大变化量（非加速模式固定记为 64 次）。
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
    result.groundRadiosity = groundRadiosity();
    return result;
}

// 作用：读回整个光谱输出缓冲（布局见 beginSpectral）。
std::vector<float> FacetrtVulkan::readSpectralOutput()
{
    if (m_spectralPipeline == VK_NULL_HANDLE) throw std::logic_error("No spectral output");
    std::vector<float> output(size_t(m_spectralOutput.size / sizeof(float)));
    downloadBuffer(m_spectralOutput, output.data(), m_spectralOutput.size);
    return output;
}

// 作用：审计用读回：optics、发射、上次物理辐射度，以及指定波段的输出与亮/阴状态切片
// （偏移 b = band×n，状态起点 k = bands×n + 2b）。
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

// 作用：释放光谱会话的管线、描述符与缓冲。
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

// 作用：审计用：保留的参考求解器改写 m_optics 后，重新上传指定的 optics。
void FacetrtVulkan::restoreSpectralOptics(const std::vector<SurfaceOptics>& optics)
{
    validateOptics(optics);
    uploadBuffer(m_optics, optics.data(), optics.size() * sizeof(SurfaceOptics));
}

// 作用：释放求解阶段的全部资源（光谱、能量、incident、地面、radiosity 管线、CSR 与
// 辐射度缓冲）并清空 CPU 侧 CSR。可重复调用；设备未创建时直接返回。
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
    if (m_groundPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_groundPipeline, nullptr);
    m_groundPipeline = VK_NULL_HANDLE;
    destroyBuffer(m_groundSides);
    destroyBuffer(m_groundWeights);
    destroyBuffer(m_groundMean);
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

// 作用：释放可见性阶段的管线、渲染通道、描述符与缓冲，并清空 CPU 侧计数副本。
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
    destroyBuffer(m_groundCounts);
    destroyBuffer(m_visibilityStats);
    destroyBuffer(m_directTotal);
    destroyBuffer(m_directVisible);
    m_denominatorCpu.clear();
    m_skyCountsCpu.clear();
    m_groundCountsCpu.clear();
}

// 作用：完整销毁：等待设备空闲 → 求解资源 → 可见性资源 → 命令池 → 设备 → 校验 messenger →
// 实例，并把句柄与计数复位，使对象可再次 initialize。
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
