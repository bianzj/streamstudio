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
constexpr int kLavapipeMaxThreads = 32;

const char* backendName(ComputeBackend backend)
{
	return backend == ComputeBackend::Cpu ? "cpu" : backend == ComputeBackend::Auto ? "auto" : "gpu";
}

// The Vulkan loader reads these variables when the instance is created, so this
// must run before GLFW or nvvk touch Vulkan.
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

int AppSetting::initWindow()
{
	// Setup GLFW window
	glfwSetErrorCallback(onErrorCallback);
	if (glfwInit() == GLFW_FALSE)
	{
		fprintf(stderr, "GLFW: initialization failed (no desktop session?)\n");
		return 0;
	}
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	// HiSTREAM runs as a batch engine behind the web UI. A Vulkan surface is
	// still required, but its helper window must never flash or steal focus.
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
	// FIXED[中] 错误处理: glfwInit/glfwCreateWindow 失败此前只返回 0 且 init() 忽略，随后以空窗口创建 surface；已修复：initWindow 失败时记录 GLFW 错误并释放窗口/GLFW 后返回 0，init() 检查返回值并抛异常。
	m_window = glfwCreateWindow(SAMPLE_WIDTH, SAMPLE_HEIGHT, PROJECT_NAME, nullptr, nullptr);
	if (!m_window)
	{
		fprintf(stderr, "GLFW: window creation failed; cannot create a Vulkan surface\n");
		glfwTerminate();
		return 0;
	}

	// Setup Vulkan
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

int AppSetting::initVulkan()
{
	// Vulkan required extensions
	if (m_ifDisplay) { assert(glfwVulkanSupported() == 1); }
	uint32_t count{ 0 };
	auto     reqExtensions = glfwGetRequiredInstanceExtensions(&count);

	// Requesting Vulkan extensions and layers
	m_contextInfo.setVersion(1, 3);                       // Using Vulkan 1.3
	for (uint32_t ext_id = 0; ext_id < count; ext_id++)  // Adding required extensions (surface, win32, linux, ..)
		m_contextInfo.addInstanceExtension(reqExtensions[ext_id]);
	m_contextInfo.addInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, true);  // Allow debug names
	m_contextInfo.addDeviceExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);            // Enabling ability to present rendering

	VkPhysicalDeviceShaderClockFeaturesKHR clockFeature{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR };
	m_contextInfo.addDeviceExtension(VK_KHR_SHADER_CLOCK_EXTENSION_NAME, false, &clockFeature);
	// #VKRay: Activate the ray tracing extension
	VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures = nvvk::make<VkPhysicalDeviceAccelerationStructureFeaturesKHR>();
	m_contextInfo.addDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, false, &asFeatures);
	VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeature{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
	m_contextInfo.addDeviceExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, false, &rtPipelineFeature);
	VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures = nvvk::make<VkPhysicalDeviceRayQueryFeaturesKHR>();
	m_contextInfo.addDeviceExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME, false, &rayQueryFeatures);
	m_contextInfo.addDeviceExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
	m_contextInfo.addDeviceExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
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

	// FIXED[低] Bug: 手工把 AS/RT 管线/BDA 特性结构串到 enabledAtomicsFeatures.pNext，与 addDeviceExtension 已注册的同 sType 结构重复（nvvk 构建链时会覆盖 pNext，此链被丢弃，若未覆盖则违反 VUID-VkDeviceCreateInfo-pNext）；已修复：删除重复的 AS/RT 管线/BDA 手工链，只保留随扩展注册的 shader-atomic-float 特性结构。
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


	m_contextInfo.verboseCompatibleDevices = false; // 关闭“兼容设备”列表打印
	m_contextInfo.verboseUsed              = false; // 关闭“当前使用的设备/扩展”打印
	m_contextInfo.verboseAvailable         = false; // 关闭“所有可用扩展”列表打印

	if (!m_context.initInstance(m_contextInfo))
		throw std::runtime_error("Vulkan instance creation failed");

	//assert(asFeatures.accelerationStructure == VK_TRUE && rayQueryFeatures.rayQuery == VK_TRUE);
	// inital device
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
	std::stable_partition(gpuDevices.begin(), gpuDevices.end(),
		[&](uint32_t index) { return deviceType(index) == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU; });
	if (m_deviceReport) {
		*m_deviceReport = deviceReportJson(physicalDevices, gpuDevices, cpuDevices,
			facetvk::rankDevices(m_context.m_instance));
		return 1;
	}
	const std::vector<uint32_t>& gpuCandidates = gpuDevices;
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
	m_context.ignoreDebugMessage(1303270965);  // Bogus "general layout" perf warning.

	//auto  qGCT1 = m_context.createQueue(m_contextInfo.defaultQueueGCT, "GCT1", 1.0f);
	m_queues.push_back({ m_context.m_queueGCT.queue, m_context.m_queueGCT.familyIndex, m_context.m_queueGCT.queueIndex });
	//m_queues.push_back({ qGCT1.queue, qGCT1.familyIndex, qGCT1.queueIndex });
	// FIXED[低] Bug: 在 lavapipe 上 requestedQueues 被缩减为仅 GCT 后，m_queueC/m_queueT 是空句柄却仍被放进 m_queues[1]/[2]；已修复：句柄为空时回退为已创建的 GCT 队列，避免后续向 VK_NULL_HANDLE 提交。
	const nvvk::Queue fallbackQueue = m_queues.front();
	nvvk::Queue queueC{ m_context.m_queueC.queue, m_context.m_queueC.familyIndex, m_context.m_queueC.queueIndex };
	nvvk::Queue queueT{ m_context.m_queueT.queue, m_context.m_queueT.familyIndex, m_context.m_queueT.queueIndex };
	if (queueC.queue == VK_NULL_HANDLE) queueC = fallbackQueue;
	if (queueT.queue == VK_NULL_HANDLE) queueT = fallbackQueue;
	m_queues.push_back(queueC);
	m_queues.push_back(queueT);

	// Initialize Vulkan function pointers
	vk::DynamicLoader dl;
	auto              vkGetInstanceProcAddr = dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);
	VULKAN_HPP_DEFAULT_DISPATCHER.init(static_cast<vk::Instance>(m_context.m_instance));
	VULKAN_HPP_DEFAULT_DISPATCHER.init(static_cast<vk::Device>(m_context.m_device));

	//VULKAN_HPP_DEFAULT_DISPATCHER.init(m_context.m_instance, vkGetInstanceProcAddr, m_context.m_device);

	return 1;
}

int AppSetting::initSurface()
{
	VkResult err = glfwCreateWindowSurface(m_context.m_instance, m_window, nullptr, &m_surface);

	// FIXED[中] 错误处理: Release 下 assert 被移除，surface 创建失败后仍以无效 m_surface 查询 present 支持；已修复：创建失败时抛异常，查询失败或不支持 present 时记录警告并返回 0（CPU 批处理后端可能不支持 present）。
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
