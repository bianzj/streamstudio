// ==========================================================================
// 文件：voxellimits.h
// 功能：体元模式（VoxelRT / VoxelEB）的规模上限检查。逐体元的 SSBO 经描述符绑定，单个缓冲不得超过
//       GPU 的 maxStorageBufferRange（NVIDIA 为 4 GiB-1）；1D 派发的工作组数不得超过
//       maxComputeWorkGroupCount[0]（规范下限 65535，即 64 体元/组时 4.19 M 体元）。
//       此前超限是静默的（着色器越界访问/结果截断，或丢弃图像环境项）；现在一律抛出带体元数与上限的明确错误。
// ==========================================================================
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace voxellimits {

inline std::string grouped(uint64_t value)
{
    std::string text = std::to_string(value);
    for (int i = static_cast<int>(text.size()) - 3; i > 0; i -= 3) text.insert(static_cast<size_t>(i), ",");
    return text;
}

// One per-voxel storage buffer: `voxels * bytesPerVoxel` bytes must fit maxStorageBufferRange.
inline void requireStorageRange(VkPhysicalDevice physicalDevice, const char* model, const char* bufferName,
                                uint64_t voxels, uint64_t bytesPerVoxel)
{
    if (!physicalDevice || bytesPerVoxel == 0) return;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    const uint64_t limit = properties.limits.maxStorageBufferRange;
    const uint64_t bytes = voxels * bytesPerVoxel;
    if (bytes <= limit) return;
    throw std::runtime_error(std::string(model) + ": " + grouped(voxels) + " voxels need " + grouped(bytes)
        + " bytes for the " + bufferName + " buffer (" + std::to_string(bytesPerVoxel) + " B per voxel), above this GPU's "
        + "maxStorageBufferRange of " + grouped(limit) + " bytes (at most " + grouped(limit / bytesPerVoxel)
        + " voxels with this buffer). Reduce the scene size or use a larger voxel edge.");
}

// 32-bit element index of a per-voxel table with `entriesPerVoxel` entries (shader index arithmetic is uint).
inline void requireIndex32(const char* model, const char* tableName, uint64_t voxels, uint64_t entriesPerVoxel)
{
    if (entriesPerVoxel == 0 || voxels * entriesPerVoxel <= UINT32_MAX) return;
    throw std::runtime_error(std::string(model) + ": " + grouped(voxels) + " voxels x " + std::to_string(entriesPerVoxel)
        + " entries exceed the 32-bit index range (4,294,967,295) of the " + tableName + " table (at most "
        + grouped(UINT32_MAX / entriesPerVoxel) + " voxels). Reduce the scene size or use a larger voxel edge.");
}

// 1D dispatch of ceil(voxels / groupSize) work groups.
inline void requireDispatch(VkPhysicalDevice physicalDevice, const char* model, uint64_t voxels, uint64_t groupSize)
{
    if (!physicalDevice) return;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    const uint64_t limit = properties.limits.maxComputeWorkGroupCount[0];
    const uint64_t groups = (voxels + groupSize - 1) / groupSize;
    if (groups <= limit) return;
    throw std::runtime_error(std::string(model) + ": " + grouped(voxels) + " voxels need " + grouped(groups)
        + " compute work groups of " + std::to_string(groupSize) + ", above this GPU's maxComputeWorkGroupCount[0] of "
        + grouped(limit) + " (at most " + grouped(limit * groupSize) + " voxels). Reduce the scene size.");
}

// One device-memory allocation (vkAllocateMemory) must not exceed maxMemoryAllocationSize (0xffe00000 = 4,292,870,144 B
// on the NVIDIA driver, a little below the 4 GiB storage-buffer range). A single mesh buffer above it used to crash the
// process (access violation in the allocator) with no message.
inline uint64_t maxMemoryAllocationSize(VkPhysicalDevice physicalDevice)
{
    VkPhysicalDeviceMaintenance3Properties maintenance3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &maintenance3;
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
    return maintenance3.maxMemoryAllocationSize;
}

// `what` names the buffer ("proxy vertex", "proxy vertex staging", "acceleration structure", ...); `elements` and
// `elementName` describe its content for the message (e.g. 40,000,000 vertices).
inline void requireAllocation(VkPhysicalDevice physicalDevice, const char* model, const char* what, uint64_t bytes,
                              uint64_t elements, const char* elementName, const char* hint)
{
    if (!physicalDevice) return;
    const uint64_t limit = maxMemoryAllocationSize(physicalDevice);
    if (bytes <= limit) return;
    throw std::runtime_error(std::string(model) + ": the " + what + " buffer for " + grouped(elements) + " " + elementName
        + " needs " + grouped(bytes) + " bytes, above this GPU's maxMemoryAllocationSize of " + grouped(limit)
        + " bytes (a single allocation cannot be larger). " + hint);
}

}  // namespace voxellimits
