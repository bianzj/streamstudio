#pragma once

#include <vulkan/vulkan_core.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

// Element count of a width x height x bands float image stored in one GPU
// storage buffer, rejecting sizes the device cannot address.
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
