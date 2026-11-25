// ==========================================================================
// 文件：appsetting.cpp
// 功能：HiStream 的 Vulkan 环境初始化：隐藏 GLFW 辅助窗口、创建 Vulkan 实例/设备/队列/表面，
//       选择计算后端（GPU / CPU lavapipe / Auto），并能以 JSON 报告可用设备。
// 主要模块/步骤：
//   1. parseComputeBackend / registerLavapipe：解析后端名；在 Vulkan 加载前通过环境变量注册 Mesa lavapipe
//   2. 匿名命名空间工具：jsonString 转义、deviceTypeName、deviceReportJson（设备清单 JSON）
//   3. AppSetting::describeDevices / init / destroy：探测设备、完整初始化、释放资源
//   4. initWindow：GLFW 初始化并创建不可见窗口（仅为获得 Vulkan 表面）
//   5. initVulkan：挑选物理设备、启用扩展/特性、创建 nvvk::Context 与队列
//   6. initSurface：为窗口创建 VkSurfaceKHR（见下文）
// 数据流：main.cpp 解析命令行 → AppSetting::init(gpuIndex, backend) → m_context 供各模式引擎使用。
// 与其他文件：声明在 appsetting.h；设备候选来自 facetrt/facetrt_vulkan.h（Facet 模式单独枚举）。
// 约定：gpuIndex 为在“满足所需特性的 GPU 列表”中的下标；voxelIndex/facetIndex = -1 表示该后端不支持。
// ==========================================================================

#include "appsetting.h"
#include "../facetrt/facetrt_vulkan.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

// 作用：把后端字符串（不区分大小写）解析为 ComputeBackend；未识别的值一律按 Gpu。
ComputeBackend parseComputeBackend(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (value == "cpu") return ComputeBackend::Cpu;
	if (value == "auto") return ComputeBackend::Auto;
	return ComputeBackend::Gpu;
}

namespace {
// LP_MAX_THREADS of the bundled Mesa lavapipe build.
// （说明：随附 Mesa lavapipe 的线程上限，用于设备报告中提示 CPU 并行度上限。）
constexpr int kLavapipeMaxThreads = 32;

const char* backendName(ComputeBackend backend)
{
	return backend == ComputeBackend::Cpu ? "cpu" : backend == ComputeBackend::Auto ? "auto" : "gpu";
}

// The Vulkan loader reads these variables when the instance is created, so this
// must run before GLFW or nvvk touch Vulkan.
// 作用：按后端选择把 Mesa lavapipe（CPU Vulkan 驱动）的 ICD 清单注册给 Vulkan loader。
// 输入：backend；Gpu 时什么都不做。
// 说明：Cpu 只加载 lavapipe（避免坏掉的 GPU 驱动阻塞 CPU 运行）；Auto 追加 lavapipe，
//       GPU 与 CPU 设备并存。清单位于可执行文件同级的 lavapipe/lvp_icd.x86_64.json（仅 Windows 随包提供）。
// 副作用：设置进程环境变量，必须在 GLFW/nvvk 触碰 Vulkan 之前调用。
void registerLavapipe(ComputeBackend backend)
{
	if (backend == ComputeBackend::Gpu) return;
#ifdef _WIN32
	std::wstring buffer(32768, L'\0');
	buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
	const std::filesystem::path manifest =
		std::filesystem::path(buffer).parent_path() / "lavapipe" / "lvp_icd.x86_64.json";
	if (!std::filesystem::exists(manifest)) {
		if (backend == ComputeBackend::Cpu)
			throw std::runtime_error("CPU backend requires the Mesa lavapipe driver: " + manifest.string());
		std::cerr << "HiStream: Mesa lavapipe not found; auto backend can only use GPUs\n";
		return;
	}
	const std::wstring path = manifest.wstring();
	if (backend == ComputeBackend::Cpu) {
		// Load lavapipe only, so a missing or broken GPU driver cannot block CPU runs.
		SetEnvironmentVariableW(L"VK_DRIVER_FILES", path.c_str());
		SetEnvironmentVariableW(L"VK_ICD_FILENAMES", path.c_str());
	} else {
		SetEnvironmentVariableW(L"VK_ADD_DRIVER_FILES", path.c_str());
	}
#else
	if (backend == ComputeBackend::Cpu)
		throw std::runtime_error("CPU backend is only bundled for Windows; select Mesa lavapipe with VK_DRIVER_FILES");
#endif
}

// 作用：把 C 字符串转成带引号的 JSON 字符串（转义 " \ 与控制字符）。
std::string jsonString(const char* text)
{
	std::string result = "\"";
	for (const unsigned char c : std::string(text)) {
		if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
		else if (c < 0x20) { char escaped[8]; std::snprintf(escaped, sizeof escaped, "\\u%04x", c); result += escaped; }
		else result += static_cast<char>(c);
	}
	return result + "\"";
}

// 作用：Vulkan 物理设备类型 → 报告中使用的简短英文名。
const char* deviceTypeName(VkPhysicalDeviceType type)
{
	switch (type) {
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
	case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
	default: return "other";
	}
}

// voxelIndex/facetIndex are the "gpu" control values that select each device
// in the ray-tracing (VoxelRT/VoxelEB/Raytracing) and Facet backends; -1 = unsupported.
// 作用：生成可用设备的 JSON 报告（逻辑处理器数、lavapipe 线程上限、各 GPU 及其在 Voxel/Facet
//       后端中的下标、CPU lavapipe 名称）。
// 输入：全部物理设备；gpuDevices/cpuDevices 为 nvvk 筛出的设备下标；facetDevices 为 Facet 后端候选。
// 输出：JSON 文本，由前端/服务端据此填充设备选择列表。
std::string deviceReportJson(const std::vector<VkPhysicalDevice>& physicalDevices,
	const std::vector<uint32_t>& gpuDevices, const std::vector<uint32_t>& cpuDevices,
	const std::vector<facetvk::DeviceCandidate>& facetDevices)
{
	std::ostringstream json;
	json << "{\"logicalProcessors\":" << std::max(1U, std::thread::hardware_concurrency())
		<< ",\"lavapipeThreadLimit\":" << kLavapipeMaxThreads << ",\"gpus\":[";
	bool first = true;
	for (uint32_t index = 0; index < physicalDevices.size(); ++index) {
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(physicalDevices[index], &properties);
		if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
		const auto voxel = std::find(gpuDevices.begin(), gpuDevices.end(), index);
		const auto facet = std::find_if(facetDevices.begin(), facetDevices.end(),
			[&](const facetvk::DeviceCandidate& candidate) { return candidate.device == physicalDevices[index]; });
		json << (first ? "" : ",") << "{\"name\":" << jsonString(properties.deviceName)
			<< ",\"type\":\"" << deviceTypeName(properties.deviceType) << "\""
			<< ",\"voxelIndex\":" << (voxel == gpuDevices.end() ? -1 : static_cast<int>(voxel - gpuDevices.begin()))
			<< ",\"facetIndex\":" << (facet == facetDevices.end() ? -1 : static_cast<int>(facet - facetDevices.begin()))
			<< "}";
		first = false;
	}
	json << "],\"cpu\":{\"lavapipe\":";
	if (cpuDevices.empty()) {
		json << "null";
	} else {
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(physicalDevices[cpuDevices.front()], &properties);
		json << jsonString(properties.deviceName);
	}
	json << "}}";
	return json.str();
}
}

// 作用：不做完整初始化，只临时创建窗口与 Vulkan 实例来枚举设备并返回 JSON 报告。
// 说明：以 Auto 后端探测，这样 lavapipe 也会出现在报告里；探测后立即 destroy。
std::string AppSetting::describeDevices()
{
	std::string report;
	AppSetting probe;
	probe.m_deviceReport = &report;
	probe.computeBackend = ComputeBackend::Auto;
	registerLavapipe(ComputeBackend::Auto);
	probe.initWindow();
	probe.initVulkan();
	probe.destroy();
	return report;
}

// 作用：完整初始化：注册驱动 → 创建隐藏窗口 → 创建 Vulkan 上下文 → 创建表面。
// 输入：gpuIndex（所选 GPU 在合格设备中的下标）、backend；失败时抛 std::runtime_error。
void AppSetting::init(int gpuIndex, ComputeBackend backend)
{
	gpuDeviceIndex = gpuIndex;
	computeBackend = backend;
	registerLavapipe(backend);
	if (initWindow() == 0)
		throw std::runtime_error("GLFW window initialization failed; a Vulkan surface cannot be created");
	initVulkan();
	initSurface();
}

// 作用：按创建的逆序释放表面、Vulkan 上下文与 GLFW 窗口。
// 说明：m_ifDisplay 为真时表面由显示路径管理，此处不销毁。
void AppSetting::destroy()
{
	if (!m_ifDisplay && m_surface != VK_NULL_HANDLE)
	{
		vkDestroySurfaceKHR(m_context.m_instance, m_surface, nullptr);
		m_surface = VK_NULL_HANDLE;
	}

	m_context.deinit();

	if (m_window) glfwDestroyWindow(m_window);
	m_window = nullptr;
	glfwTerminate();
}

// 作用：初始化 GLFW 并创建不可见窗口；返回 1 成功，0 失败（失败时已自行清理 GLFW）。
int AppSetting::initWindow()
{
	// Setup GLFW window
	glfwSetErrorCallback(onErrorCallback);
	if (glfwInit() == GLFW_FALSE)
	{
		fprintf(stderr, "GLFW: initialization failed (no desktop session?)\n");
		return 0;
	}
	// 步骤 1：不创建 OpenGL 上下文，只为 Vulkan 使用。
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	// HiSTREAM runs as a batch engine behind the web UI. A Vulkan surface is
	// still required, but its helper window must never flash or steal focus.
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
	m_window = glfwCreateWindow(SAMPLE_WIDTH, SAMPLE_HEIGHT, PROJECT_NAME, nullptr, nullptr);
	if (!m_window)
	{
		fprintf(stderr, "GLFW: window creation failed; cannot create a Vulkan surface\n");
		glfwTerminate();
		return 0;
	}

	// Setup Vulkan
	// 步骤 2：确认 loader 找到可用的 Vulkan 驱动，否则放弃。
	if (glfwVulkanSupported() == GLFW_FALSE)
	{
		printf("GLFW: Vulkan Not Supported\n");
		glfwDestroyWindow(m_window);
		m_window = nullptr;
		glfwTerminate();
		return 0;
	}

	return 1;
}

// 作用：枚举/选择 Vulkan 设备并创建 m_context（见下方各步骤）。
int AppSetting::initVulkan()
{
	// Vulkan required extensions
	if (m_ifDisplay) { assert(glfwVulkanSupported() == 1); }
	uint32_t count{ 0 };
	auto     reqExtensions = glfwGetRequiredInstanceExtensions(&count);

	// Requesting Vulkan extensions and layers
	// 步骤 1：声明所需的实例/设备扩展与特性（Vulkan 1.3；光追加速结构、ray query、原子操作等）。
	//       第二个参数 true 表示“可选”：缺失时设备仍可用；false 表示必需，缺失则设备被判为不兼容。
	m_contextInfo.setVersion(1, 3);                       // Using Vulkan 1.3
	for (uint32_t ext_id = 0; ext_id < count; ext_id++)  // Adding required extensions (surface, win32, linux, ..)
		m_contextInfo.addInstanceExtension(reqExtensions[ext_id]);
	m_contextInfo.addInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, true);  // Allow debug names
	m_contextInfo.addDeviceExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);            // Enabling ability to present rendering

	VkPhysicalDeviceShaderClockFeaturesKHR clockFeature{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR };
	m_contextInfo.addDeviceExtension(VK_KHR_SHADER_CLOCK_EXTENSION_NAME, false, &clockFeature);
	// #VKRay: Activate the ray tracing extension
	// 光线追踪三件套：加速结构 + 光追管线 + ray query（体元模式的 compute 着色器用 ray query 追踪）。
	VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures = nvvk::make<VkPhysicalDeviceAccelerationStructureFeaturesKHR>();
	m_contextInfo.addDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, false, &asFeatures);
	VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeature{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
	m_contextInfo.addDeviceExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, false, &rtPipelineFeature);
	VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures = nvvk::make<VkPhysicalDeviceRayQueryFeaturesKHR>();
	m_contextInfo.addDeviceExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME, false, &rayQueryFeatures);
	m_contextInfo.addDeviceExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
	m_contextInfo.addDeviceExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
	// 64 位整型原子为可选扩展；以下额外请求一个队列用于并行加载/构建。
	m_contextInfo.addDeviceExtension(VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME, true);
	// Extra queues for parallel load/build
	m_contextInfo.addRequestedQueue(m_contextInfo.defaultQueueGCT, 1, 1.0f);  // Loading scene - mipmap generation

	m_contextInfo.addDeviceExtension(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
	m_contextInfo.addDeviceExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
	m_contextInfo.addDeviceExtension(VK_KHR_MAINTENANCE3_EXTENSION_NAME);
	VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR physicalDevicePipelineExecutableProprtiesFeaturesKHR = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, nullptr, VK_TRUE };
	m_contextInfo.addDeviceExtension(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME, true,
		&physicalDevicePipelineExecutableProprtiesFeaturesKHR);

	m_contextInfo.addDeviceExtension("VK_KHR_spirv_1_4");
	m_contextInfo.addDeviceExtension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
	m_contextInfo.addInstanceExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

	VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT m_fragmentShaderInterlockFeatures{
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,  // sType
	nullptr,                                                                   // pNext
	VK_TRUE,                                                                   // fragmentShaderSampleInterlock
	VK_TRUE,                                                                   // fragmentShaderPixelInterlock
	VK_FALSE };
	m_contextInfo.addDeviceExtension(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, true, &m_fragmentShaderInterlockFeatures);

	// VkPhysicalDeviceScalarBlockLayoutFeatures scalarLayout{
	// 	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES
	// };
	// scalarLayout.scalarBlockLayout = VK_TRUE;
	// m_contextInfo.addDeviceExtension(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME, true, &scalarLayout);

	// 启用 float32 缓冲原子加，着色器里的辐射量累加依赖它（可选扩展，缺失时需另有回退路径，待核对）。
	VkPhysicalDeviceShaderAtomicFloatFeaturesEXT enabledAtomicsFeatures{};
	enabledAtomicsFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
	enabledAtomicsFeatures.pNext = nullptr;
	enabledAtomicsFeatures.shaderBufferFloat32AtomicAdd = VK_TRUE;
	m_contextInfo.addDeviceExtension(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME, true, &enabledAtomicsFeatures);

	// DEFERRED[中] 性能: ENABLE_GPU_PRINTF 无条件定义，且 m_contextInfo 以 ContextCreateInfo(true) 开启校验层；发布版若部署了校验层，debugPrintf 插桩会使光追/计算着色器显著变慢并占用额外描述符槽。理由：本文件没有现成的校验层/debugPrintf 开关可跟随，而开关默认值属本轮禁止改动范围，需先确认构建开关方案。
	#define ENABLE_GPU_PRINTF //   Enabling printf in shaders
	// #extension GL_EXT_debug_printf
	// debugPrintfEXT("");
#ifdef ENABLE_GPU_PRINTF
	m_contextInfo.addDeviceExtension(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
	std::vector<VkValidationFeatureEnableEXT>  enables{ VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT };
	std::vector<VkValidationFeatureDisableEXT> disables{};
	VkValidationFeaturesEXT                    features{ VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT };
	features.enabledValidationFeatureCount = static_cast<uint32_t>(enables.size());
	features.pEnabledValidationFeatures = enables.data();
	features.disabledValidationFeatureCount = static_cast<uint32_t>(disables.size());
	features.pDisabledValidationFeatures = disables.data();
	m_contextInfo.instanceCreateInfoExt = &features;
#endif  // ENABLE_GPU_PRINTF

	// Creating Vulkan base application
	//nvvk::Context m_context{};
	//m_context.init(m_contextInfo);


	// 步骤 2：关闭 nvvk 的设备/扩展清单打印，避免污染引擎的标准输出（供上层解析）。
	m_contextInfo.verboseCompatibleDevices = false; // 关闭“兼容设备”列表打印
	m_contextInfo.verboseUsed              = false; // 关闭“当前使用的设备/扩展”打印
	m_contextInfo.verboseAvailable         = false; // 关闭“所有可用扩展”列表打印

	// 步骤 3：创建 Vulkan 实例（此时 lavapipe 的环境变量已生效，CPU 设备可被枚举）。
	if (!m_context.initInstance(m_contextInfo))
		throw std::runtime_error("Vulkan instance creation failed");

	//assert(asFeatures.accelerationStructure == VK_TRUE && rayQueryFeatures.rayQuery == VK_TRUE);
	// inital device
	// 步骤 4：筛选满足必需扩展的设备，按类型分为 GPU 与 CPU(lavapipe) 两组。
	const auto compatibleDevices = m_context.getCompatibleDevices(m_contextInfo);  // Find all compatible devices
	const auto physicalDevices = m_context.getPhysicalDevices();
	const auto deviceType = [&](uint32_t index) {
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(physicalDevices[index], &properties);
		return properties.deviceType;
	};
	std::vector<uint32_t> gpuDevices, cpuDevices;
	for (const uint32_t index : compatibleDevices)
		(deviceType(index) == VK_PHYSICAL_DEVICE_TYPE_CPU ? cpuDevices : gpuDevices).push_back(index);
	// GPU index order: discrete GPUs first, then Vulkan enumeration order.
	std::sort(gpuDevices.begin(), gpuDevices.end());
	// GPU 下标顺序：独立显卡优先，其余保持 Vulkan 枚举顺序；gpuIndex 即指该顺序中的位置。
	std::stable_partition(gpuDevices.begin(), gpuDevices.end(),
		[&](uint32_t index) { return deviceType(index) == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU; });
	// describeDevices 的探测路径：只生成设备报告并返回，不创建逻辑设备。
	if (m_deviceReport) {
		*m_deviceReport = deviceReportJson(physicalDevices, gpuDevices, cpuDevices,
			facetvk::rankDevices(m_context.m_instance));
		return 1;
	}
	const std::vector<uint32_t>& gpuCandidates = gpuDevices;
	// 步骤 5：按后端决定物理设备。Cpu 用 lavapipe；Auto 在没有合格 GPU 时回退到 CPU；
	//       否则按 gpuDeviceIndex 取 GPU（越界抛错并给出有效范围）。
	uint32_t selectedDevice = 0;
	if (computeBackend == ComputeBackend::Cpu
		|| (computeBackend == ComputeBackend::Auto && gpuCandidates.empty() && !cpuDevices.empty())) {
		if (cpuDevices.empty())
			throw std::runtime_error("Mesa lavapipe CPU Vulkan device is unavailable or lacks the required features");
		if (computeBackend == ComputeBackend::Auto)
			std::cerr << "HiStream: no compatible Vulkan GPU; falling back to the CPU backend\n";
		selectedDevice = cpuDevices.front();
	} else {
		if (gpuCandidates.empty())
			throw std::runtime_error("No Vulkan GPU supports the required ray tracing features");
		if (gpuDeviceIndex < 0 || static_cast<size_t>(gpuDeviceIndex) >= gpuCandidates.size())
			throw std::runtime_error("GPU index " + std::to_string(gpuDeviceIndex) + " is out of range; "
				+ std::to_string(gpuCandidates.size()) + " compatible GPU(s) found (valid: 0-"
				+ std::to_string(gpuCandidates.size() - 1) + ")");
		selectedDevice = gpuCandidates[static_cast<size_t>(gpuDeviceIndex)];
	}
	{
		// 步骤 6：若设备的总队列数不足以满足额外请求的队列（lavapipe 只有 1 个），降级为只请求 GCT 队列。
		// Mesa lavapipe exposes a single queue, too few for the extra queues
		// requested above; the models only submit to the GCT queue.
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[selectedDevice], &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[selectedDevice], &familyCount, families.data());
		uint32_t available = 0, requested = 0;
		for (const auto& family : families) available += family.queueCount;
		for (const auto& queue : m_contextInfo.requestedQueues) requested += queue.count;
		if (available < requested)
			m_contextInfo.requestedQueues = { { m_contextInfo.defaultQueueGCT, 1, m_contextInfo.defaultPriorityGCT } };
	}
	// 步骤 7：创建逻辑设备，并打印实际使用的后端/设备（CPU 时附线程数，受 lavapipe 上限截断）。
	if (!m_context.initDevice(selectedDevice, m_contextInfo))
		throw std::runtime_error("Vulkan device creation failed");
	{
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(m_context.m_physicalDevice, &properties);
		const bool cpu = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
		std::cout << "HiStream compute backend (" << backendName(computeBackend) << "): "
			<< properties.deviceName << (cpu ? " [CPU]" : " [GPU]");
		if (const char* threads = std::getenv("LP_NUM_THREADS"); cpu && threads) {
			const int requested = std::atoi(threads);
			std::cout << ", " << (std::min)(requested, kLavapipeMaxThreads) << " threads";
			if (requested > kLavapipeMaxThreads)
				std::cout << " (requested " << requested << ", lavapipe max " << kLavapipeMaxThreads << ")";
		}
		std::cout << std::endl;
	}
	// （屏蔽校验层中编号 1303270965 的“general layout”性能警告，原作者认为是误报。）
	m_context.ignoreDebugMessage(1303270965);  // Bogus "general layout" perf warning.

	//auto  qGCT1 = m_context.createQueue(m_contextInfo.defaultQueueGCT, "GCT1", 1.0f);
	// 步骤 8：整理队列：m_queues[0]=GCT，[1]=计算队列，[2]=传输队列；
	//       设备没有独立的 C/T 队列时回退到 GCT 队列（各模块按下标取用）。
	m_queues.push_back({ m_context.m_queueGCT.queue, m_context.m_queueGCT.familyIndex, m_context.m_queueGCT.queueIndex });
	//m_queues.push_back({ qGCT1.queue, qGCT1.familyIndex, qGCT1.queueIndex });
	const nvvk::Queue fallbackQueue = m_queues.front();
	nvvk::Queue queueC{ m_context.m_queueC.queue, m_context.m_queueC.familyIndex, m_context.m_queueC.queueIndex };
	nvvk::Queue queueT{ m_context.m_queueT.queue, m_context.m_queueT.familyIndex, m_context.m_queueT.queueIndex };
	if (queueC.queue == VK_NULL_HANDLE) queueC = fallbackQueue;
	if (queueT.queue == VK_NULL_HANDLE) queueT = fallbackQueue;
	m_queues.push_back(queueC);
	m_queues.push_back(queueT);

	// Initialize Vulkan function pointers
	// 步骤 9：初始化 Vulkan-Hpp 默认分发器（instance 与 device 级函数指针）。
	vk::DynamicLoader dl;
	auto              vkGetInstanceProcAddr = dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);
	VULKAN_HPP_DEFAULT_DISPATCHER.init(static_cast<vk::Instance>(m_context.m_instance));
	VULKAN_HPP_DEFAULT_DISPATCHER.init(static_cast<vk::Device>(m_context.m_device));

	//VULKAN_HPP_DEFAULT_DISPATCHER.init(m_context.m_instance, vkGetInstanceProcAddr, m_context.m_device);

	return 1;
}

// 作用：为（不可见的）GLFW 窗口创建 VkSurfaceKHR，并检查 GCT 队列族是否支持呈现。
// 返回：1 支持呈现；0 不支持（批处理无需呈现，仅打印提示）；创建失败抛异常。
int AppSetting::initSurface()
{
	VkResult err = glfwCreateWindowSurface(m_context.m_instance, m_window, nullptr, &m_surface);

	if (err != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to create a GLFW Vulkan surface, VkResult = "
			+ std::to_string(static_cast<int>(err)));
	}
	VkBool32 supportsPresent = VK_FALSE;
	const VkResult supportResult = vkGetPhysicalDeviceSurfaceSupportKHR(
		m_context.m_physicalDevice, m_context.m_queueGCT.familyIndex, m_surface, &supportsPresent);
	if (supportResult != VK_SUCCESS || supportsPresent != VK_TRUE)
	{
		std::cerr << "HiStream: queue family does not support presenting to the window surface; "
			"continuing without present support (batch mode)\n";
		return 0;
	}
	return 1;
}
