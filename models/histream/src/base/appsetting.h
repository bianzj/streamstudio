// ==========================================================================
// 文件：appsetting.h
// 功能：声明 AppSetting —— Vulkan 实例/设备/表面/队列的统一初始化入口，以及计算后端选择。
// 主要模块：
//   1. onErrorCallback：GLFW 错误回调（写 stderr）
//   2. ComputeBackend / parseComputeBackend：Gpu、Cpu（Mesa lavapipe）、Auto 三种后端
//   3. AppSetting：init/destroy/describeDevices 与成员 m_context、m_queues
// 数据流：main.cpp / Engine 调用 AppSetting::init → m_context（nvvk::Context）与 m_queues 交给各模式引擎使用。
// 与其他文件：实现见 appsetting.cpp；队列类型来自 queue.h。
// 约定：m_queues 顺序为 [GCT, Compute, Transfer]（缺独立队列时回退为 GCT 队列）。
// ==========================================================================

// Inital the information about Vulkan(extensions), device and surface window.
#pragma once

#ifdef USE_NEW_NVPRO_CORE
//#include "nvvkhl/appbase_vk.hpp"
//#include "nvvkhl/appbase_vkpp.hpp"
#else
//#include "appbase_vk.hpp"
//#include "appbase_vkpp.hpp"
#include <nvvk/appwindowprofiler_vk.hpp>
#endif

#include <vulkan/vulkan.hpp>
#include "nvvk/context_vk.hpp"
#include "nvh/gltfscene.hpp"
#include "nvvk/debug_util_vk.hpp"
#include "nvvk/profiler_vk.hpp"
#include "nvvk/raytraceKHR_vk.hpp"
#include "nvvk/raypicker_vk.hpp"
#include "nvvk/structs_vk.hpp"

#include "queue.h"
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
/// <summary>
///
/// </summary>
//



// GLFW Callback functions
// GLFW 错误回调：把错误码与描述写到 stderr。
static void onErrorCallback(int error, const char* description)
{
	fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// 计算后端：Gpu 仅用 GPU；Cpu 仅用 lavapipe；Auto 优先 GPU，无合格 GPU 时回退 CPU。
enum class ComputeBackend { Gpu, Cpu, Auto };

// 把 "gpu"/"cpu"/"auto"（不区分大小写）解析为枚举，其余按 Gpu。
ComputeBackend parseComputeBackend(std::string value);

// 类：AppSetting，封装窗口（仅用于获得表面）、Vulkan 上下文与队列的创建和销毁。
class AppSetting
{
public:

	// gpuIndex selects among the Vulkan devices with the required features.
	// Cpu/Auto run the same shaders on the bundled Mesa lavapipe CPU Vulkan driver.
	void init(int gpuIndex = 0, ComputeBackend backend = ComputeBackend::Gpu);
	void destroy();
	// JSON list of usable GPUs (with their per-backend indices) and the CPU driver.
	static std::string describeDevices();

	nvvk::Context   m_context{};
	std::vector<nvvk::Queue> m_queues{};

	// 是否在显示模式下运行（表面生命周期由显示路径管理）；批处理时为 false。
	// SAMPLE_WIDTH/HEIGHT 是辅助窗口的像素尺寸，与计算分辨率无关。
	bool m_ifDisplay{ false };
	int SAMPLE_WIDTH = 960;
	int SAMPLE_HEIGHT = 720;
private:
	// 私有步骤，按 initWindow → initVulkan → initSurface 的顺序由 init 调用。
	int initWindow();
	int initVulkan();
	int initSurface();
	nvvk::ContextCreateInfo m_contextInfo = nvvk::ContextCreateInfo(true);
	int             gpuDeviceIndex{ 0 };
	ComputeBackend  computeBackend{ ComputeBackend::Gpu };
	std::string*    m_deviceReport{ nullptr };
	GLFWwindow*     m_window{ nullptr };
	VkSurfaceKHR    m_surface{};
};
