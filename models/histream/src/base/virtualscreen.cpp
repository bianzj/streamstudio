// ==========================================================================
// 文件：virtualscreen.cpp
// 功能：实现设备缓冲到主机可见缓冲的回读（VirtualScreen::bufferToBuffer）。
// 主要模块：
//   1. readbackBuffer 模板：容量检查 → 屏障 → 拷贝 → 屏障 → 提交并等待
//   2. 三个重载（RaytracingIO / VoxelebIO / VoxelrtIO）均转调 readbackBuffer
// 数据流：生产者（着色器/传输）写 bufferIn → 传输读屏障 → vkCmdCopyBuffer 到 bufferOut
//         → 主机读屏障 → 在生产者所在队列提交并等待完成。
// 关系：声明见 virtualscreen.h；调用方为各模式的 *.cpp 回读逻辑。
// 注意：栅栏只保证执行完成，不保证内存可见，因此必须显式加两道缓冲屏障。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#include "virtualscreen.h"

namespace {
// 模块：回读模板函数（匿名命名空间内）。
// 作用：在 modelio 的队列上录制并同步提交一次 bufferIn → bufferOut 的 size 字节拷贝。
// 返回：size 超过任一缓冲的内存需求时返回 false（不提交任何命令），否则 true。
// 副作用：阻塞到命令完成（submitAndWait）；偏移固定为 0。
// The producer and readback use the same queue. A fence completes execution,
// but the shader writes and the host read still need memory dependencies.
template <typename ModelIO>
bool readbackBuffer(ModelIO& modelio, const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut)
{
    // 步骤 1：用缓冲内存需求大小做越界保护，避免拷贝范围超出任一缓冲。
    VkMemoryRequirements sourceRequirements{};
    VkMemoryRequirements destinationRequirements{};
    vkGetBufferMemoryRequirements(modelio->m_device, bufferIn.buffer, &sourceRequirements);
    vkGetBufferMemoryRequirements(modelio->m_device, bufferOut.buffer, &destinationRequirements);
    if (size > sourceRequirements.size || size > destinationRequirements.size)
    {
        return false;
    }
    // 步骤 2：在生产者所用队列族上创建临时命令缓冲。
    nvvk::CommandPool genCmdBuf((vk::Device)modelio->m_device, modelio->m_queueIndex);
    vk::CommandBuffer cmdBuff = genCmdBuf.createCommandBuffer();

    // 步骤 3：源缓冲屏障，使着色器/传输写入对随后的传输读可见。
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

    // 步骤 4：整段从偏移 0 拷贝 size 字节。
    vk::BufferCopy copyRegion;
    copyRegion.setSrcOffset(0);
    copyRegion.setDstOffset(0);
    copyRegion.setSize(size);
    cmdBuff.copyBuffer(bufferIn.buffer, bufferOut.buffer, copyRegion);

    // 步骤 5：目标缓冲屏障，使传输写入对主机读可见。
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

// RayTracing 模式重载：转调 readbackBuffer。
bool VirtualScreen::bufferToBuffer(std::shared_ptr<RaytracingIO> & raytracingio,
                                   const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut)
{
    return readbackBuffer(raytracingio, bufferIn, size, bufferOut);
}

// VoxelEB 模式重载：转调 readbackBuffer。
bool VirtualScreen::bufferToBuffer(std::shared_ptr<VoxelebIO> & modelio,
                                   const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut)
{
    return readbackBuffer(modelio, bufferIn, size, bufferOut);
}

// VoxelRT 模式重载：转调 readbackBuffer。
bool VirtualScreen::bufferToBuffer(std::shared_ptr<VoxelrtIO> & modelio,
                                   const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut)
{
    return readbackBuffer(modelio, bufferIn, size, bufferOut);
}
