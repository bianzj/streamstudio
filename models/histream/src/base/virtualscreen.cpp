//
// Created by admin on 2024/1/24.
//

#include "virtualscreen.h"

namespace {
// The producer and readback use the same queue. A fence completes execution,
// but the shader writes and the host read still need memory dependencies.
template <typename ModelIO>
bool readbackBuffer(ModelIO& modelio, const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut)
{
    // FIXED[低] 越界/性能: 此前未校验 size 不超过 bufferIn/bufferOut 的实际大小，超出时 vkCmdCopyBuffer 越界（命令池复用需重构，本轮未改）；已修复：复制前用 vkGetBufferMemoryRequirements 取两缓冲容量，size 超出时返回 false，不再提交越界复制。
    VkMemoryRequirements sourceRequirements{};
    VkMemoryRequirements destinationRequirements{};
    vkGetBufferMemoryRequirements(modelio->m_device, bufferIn.buffer, &sourceRequirements);
    vkGetBufferMemoryRequirements(modelio->m_device, bufferOut.buffer, &destinationRequirements);
    if (size > sourceRequirements.size || size > destinationRequirements.size)
    {
        return false;
    }
    nvvk::CommandPool genCmdBuf((vk::Device)modelio->m_device, modelio->m_queueIndex);
    vk::CommandBuffer cmdBuff = genCmdBuf.createCommandBuffer();

    VkBufferMemoryBarrier sourceBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    sourceBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    sourceBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    sourceBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBarrier.buffer = bufferIn.buffer;
    sourceBarrier.offset = 0;
    sourceBarrier.size = size;
    // ALL_COMMANDS covers compute and ray-tracing producers without requiring
    // the ray-tracing stage bit to be valid on every pipeline.
    vkCmdPipelineBarrier(cmdBuff, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &sourceBarrier, 0, nullptr);

    vk::BufferCopy copyRegion;
    copyRegion.setSrcOffset(0);
    copyRegion.setDstOffset(0);
    copyRegion.setSize(size);
    cmdBuff.copyBuffer(bufferIn.buffer, bufferOut.buffer, copyRegion);

    VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.buffer = bufferOut.buffer;
    hostBarrier.offset = 0;
    hostBarrier.size = size;
    vkCmdPipelineBarrier(cmdBuff, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostBarrier, 0, nullptr);
    // Explicitly submit on the producer queue, including a nonzero queue index.
    genCmdBuf.submitAndWait(cmdBuff, modelio->m_queue);

    return true;
}
}

bool VirtualScreen::bufferToBuffer(std::shared_ptr<RaytracingIO> & raytracingio,
                                   const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut)
{
    return readbackBuffer(raytracingio, bufferIn, size, bufferOut);
}

bool VirtualScreen::bufferToBuffer(std::shared_ptr<VoxelebIO> & modelio,
                                   const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut)
{
    return readbackBuffer(modelio, bufferIn, size, bufferOut);
}

bool VirtualScreen::bufferToBuffer(std::shared_ptr<VoxelrtIO> & modelio,
                                   const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut)
{
    return readbackBuffer(modelio, bufferIn, size, bufferOut);
}
