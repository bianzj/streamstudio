// ==========================================================================
// 文件：outputsize.h
// 功能：输出影像缓冲大小的合法性检查（仅一个内联函数 checkedOutputImageElements）。
// 主要步骤：
//   1. 检查 width/height/bands 均为正数，否则抛 std::runtime_error
//   2. 用 64 位计算元素个数 width*height*bands（避免 int 溢出）
//   3. 与 GPU 的 maxStorageBufferRange 比较，超限则抛异常并提示降低分辨率或波段数
// 数据流：输入 VkPhysicalDevice 与影像尺寸 → 输出 size_t 元素个数（float 个数，不是字节数）。
// 与其他文件：被 raytracing / voxelrt / voxeleb 各自的 buffer.cpp 在创建输出影像缓冲前调用。
// 约定：每个元素按 sizeof(float) 字节计；输出影像存放在单个存储缓冲（storage buffer）中。
// ==========================================================================

#pragma once

#include <vulkan/vulkan_core.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

// Element count of a width x height x bands float image stored in one GPU
// storage buffer, rejecting sizes the device cannot address.
// 作用：返回 width x height x bands 的 float 元素个数，并保证整块缓冲不超过设备单个存储缓冲上限。
// 输入：物理设备、影像宽高（像素）、波段数；输出：元素个数；非法尺寸或超限时抛 std::runtime_error。
inline size_t checkedOutputImageElements(VkPhysicalDevice physicalDevice, int width, int height, int bands)
{
    if (width <= 0 || height <= 0 || bands <= 0)
        throw std::runtime_error("Invalid output image size: " + std::to_string(width) + " x "
            + std::to_string(height) + " x " + std::to_string(bands) + " bands");
    const uint64_t elements = uint64_t(width) * uint64_t(height) * uint64_t(bands);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    const uint64_t bytes = elements * sizeof(float);
    if (bytes > properties.limits.maxStorageBufferRange)
        throw std::runtime_error("Output image " + std::to_string(width) + " x " + std::to_string(height)
            + " x " + std::to_string(bands) + " bands needs " + std::to_string(bytes)
            + " bytes, exceeding GPU maxStorageBufferRange " + std::to_string(properties.limits.maxStorageBufferRange)
            + "; reduce resolution or band count");
    return static_cast<size_t>(elements);
}
