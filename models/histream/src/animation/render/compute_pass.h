#pragma once
#include "base/appsetting.h"
#include <filesystem>
#include <memory>
#include <vector>

namespace animation {
class HostBuffer {
public:
    HostBuffer(AppSetting&,VkDeviceSize);
    ~HostBuffer();
    VkDescriptorBufferInfo info() const;
    void* data()const;
private:
    VkDevice device{};VkBuffer buffer{};VkDeviceMemory memory{};VkDeviceSize size{};void* mapped{};
};
class ComputePass {
public:
    ComputePass(AppSetting&,const std::filesystem::path&,const std::vector<VkDescriptorType>&,uint32_t pushBytes);
    ~ComputePass();
    void bind(uint32_t,VkDescriptorBufferInfo);
    void bind(uint32_t,VkAccelerationStructureKHR);
    void dispatch(const void* push,uint32_t x,uint32_t y,uint32_t z=1);
    // One command buffer per push block (same barriers as dispatch), all in a single vkQueueSubmit
    // with one VkSubmitInfo each and one fence wait at the end. Execution order equals sequential dispatch().
    void dispatchStrips(const std::vector<std::vector<uint8_t>>& pushes,uint32_t x,uint32_t y,uint32_t z=1);
    void snapshot(const std::filesystem::path&)const;
private:
    struct Impl;std::unique_ptr<Impl> m;
};
}
