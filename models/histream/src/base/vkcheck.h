#pragma once

#include <vulkan/vulkan_core.h>

#include <stdexcept>
#include <string>

// Device loss (e.g. a Windows TDR) must fail the run instead of producing zero images.
inline void vkCheck(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: VkResult " + std::to_string(static_cast<int>(result)));
}
