// ==========================================================================
// 文件：vkcheck.h
// 功能：Vulkan 返回值检查辅助函数 vkCheck，把非 VK_SUCCESS 转成 C++ 异常。
// 主要模块：
//   1. vkCheck(result, operation)：失败时抛 std::runtime_error，附操作名和 VkResult 数值
// 关系：被 raytracing/voxeleb/voxelrt 的 command.cpp 以及 raytracing/pipeline.cpp 引用。
// 注意：目的是让设备丢失（如 Windows TDR）使整次运行失败，而不是静默输出全零图像。
// ==========================================================================

#pragma once

#include <vulkan/vulkan_core.h>

#include <stdexcept>
#include <string>

// 作用：检查 Vulkan 调用返回值，非 VK_SUCCESS 时抛出异常。
// 输入：result 为 Vulkan 返回码，operation 为用于报错的操作名；无返回值。
// Device loss (e.g. a Windows TDR) must fail the run instead of producing zero images.
inline void vkCheck(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: VkResult " + std::to_string(static_cast<int>(result)));
}
