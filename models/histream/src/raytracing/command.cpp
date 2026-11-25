// ==========================================================================
// 文件：command.cpp（raytracing）
// 功能：实现 RayTracing 模式的命令阶段：创建同步对象，按行分块提交 vkCmdTraceRaysKHR，等待并回收命令缓冲。
// 主要模块：
//   1. Command::create：创建 fence 与 2 个信号量
//   2. Command::run：按 GPU 实测耗时自适应调整每次提交的行数，直到覆盖整幅图像
//   3. Command::submit：记录命令缓冲（含全局内存屏障）并提交，用 fence 同步
//   4. Command::recordCommandBuffer：绑定管线/描述符集、推送 RayRTSetting、追踪射线
//   5. Command::waitFence / destroy：等待完成并回收；销毁同步对象
// 数据流：raytracingio->setting（推送常量，含 imageOffsetY）与 dispatchRowCount 决定每块追踪的行范围；
//   结果写入 virtualio 的输出存储缓冲，由 raytracing.cpp 读回。
// 注意：分块是为了避免单次提交过长触发 Windows 的 GPU 超时恢复（TDR，约 2 s）。
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#include "command.h"
#include "base/vkcheck.h"

#include <algorithm>
#include <chrono>


Command::Command()
{

}


// 作用：创建 fence 与 2 个信号量。信号量只为旧的信号量同步方案保留（提交代码已注释），当前仅用 fence。
// 返回值恒为 false，调用方忽略（返回约定待核对）。
bool Command::create(std::shared_ptr<RaytracingIO> &raytracingio){
    // for the firest init
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCheck(vkCreateFence(raytracingio->m_device, &fci, nullptr, &raytracingio->m_fence), "vkCreateFence");

    // Here 4 means number of pipelines + 1
    raytracingio->m_semaphores.resize(2);
    std::generate_n( raytracingio->m_semaphores.begin(), 2,
                     [&]
                     {
                         VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                         VkSemaphore semaphore;
                         vkCheck(vkCreateSemaphore( raytracingio->m_device, &sci, nullptr, &semaphore), "vkCreateSemaphore");
                         return semaphore;
                     });

    return false;
}

// 作用：把整幅图像按行分块追踪。每块：设置 setting.imageOffsetY（起始行）和 dispatchRowCount（行数）
// → submit → waitFence，并按实测耗时调整下一块的行数。结束后把两者复位为 0。返回 true。
bool Command::run(std::shared_ptr<RaytracingIO> &raytracingio){

    raytracingio->m_currentSemaphore = 1;

    const int imageHeight = std::max(1, raytracingio->imageSize.y);
    const int imageWidth = std::max(1, raytracingio->imageSize.x);
    // 步骤 1：确定图像尺寸（至少 1）。初始每块约 65536 像素（(1<<16)/宽 行），以后按耗时自适应。
    // Each submission is sized by measured GPU time, not by a fixed row
    // count. The old rule (2048 threads per submission whenever periodic
    // traversal was on, the whole image otherwise) left the GPU mostly idle
    // in the first case and risked a Windows TDR (2 s) for large images with
    // many samples in the second. Start with ~64k pixels, then grow or shrink
    // the slab towards ~120 ms per submission.
    constexpr double targetMilliseconds = 120.0;
    int rowsPerSubmit = std::clamp((1 << 16) / imageWidth, 1, imageHeight);
    // 步骤 2：逐块提交并同步。目标耗时 120 ms/块：下一块行数 = 本块行数 × clamp(120/实测毫秒, 0.25, 4)，
    // 并限制在 [1, imageHeight]；单块耗时下限取 1 ms 防止除零。
    for (int row = 0; row < imageHeight;) {
        const int rows = std::min(rowsPerSubmit, imageHeight - row);
        raytracingio->setting.imageOffsetY = row;
        raytracingio->dispatchRowCount = rows;
        const auto begin = std::chrono::steady_clock::now();
        submit(raytracingio, nullptr, nullptr);
        waitFence(raytracingio);
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
        row += rows;
        const double scale = std::clamp(
            targetMilliseconds / std::max(elapsed, 1.0), 0.25, 4.0);
        rowsPerSubmit = std::clamp(
            static_cast<int>(rows * scale), 1, imageHeight);
    }
    // 步骤 3：复位分块参数，使 dispatchRowCount = 0 表示“整幅图像”（见 recordCommandBuffer）。
    raytracingio->setting.imageOffsetY = 0;
    raytracingio->dispatchRowCount = 0;

    return true;
}



//void Command::submit(std::shared_ptr<RaytracingIO> &raytracingio, const std::optional<std::vector<VkSemaphore>> &inSemaphore, const std::optional<VkSemaphore> &outSemaphore)
//{
//    auto & m_currentSemaphore = raytracingio->m_currentSemaphore;
//    // Preparing for the compute shader
//    VkCommandBuffer cmdBuf = raytracingio->m_genCmdBuf.createCommandBuffer();
//    recordCommandBuffer(cmdBuf,raytracingio);
//    std::array<VkPipelineStageFlags, 1> waitStages{VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
//    VkSubmitInfo submitInfoCompute{VK_STRUCTURE_TYPE_SUBMIT_INFO};
//
//    submitInfoCompute.waitSemaphoreCount = static_cast<uint32_t>(inSemaphore.has_value() ? inSemaphore.value().size() : 1);
//    submitInfoCompute.pWaitSemaphores = inSemaphore.has_value() ? inSemaphore.value().data() : & raytracingio->m_semaphores[m_currentSemaphore - 1];
//    submitInfoCompute.pWaitDstStageMask = waitStages.data();
//    submitInfoCompute.commandBufferCount = 1;
//    submitInfoCompute.pCommandBuffers = &cmdBuf;
//    submitInfoCompute.signalSemaphoreCount = 1;
//    submitInfoCompute.pSignalSemaphores = outSemaphore.has_value() ? &outSemaphore.value() : & raytracingio->m_semaphores[m_currentSemaphore];
//    // Dispatching the shader
//   // vkCmdDispatch(cmdBuf, dispatchSize.x, dispatchSize.y, dispatchSize.z);
//
//
//    vkEndCommandBuffer(cmdBuf);
//    vkQueueSubmit( raytracingio->m_queue, 1, &submitInfoCompute,  raytracingio->m_fence);
//    m_currentSemaphore++;
//}

// 作用：创建并记录一个命令缓冲后提交到 m_queue，用 m_fence 同步。步骤：1 创建命令缓冲并登记到 m_inFlight；
// 2 记录全局内存屏障；3 recordCommandBuffer；4 vkEndCommandBuffer + vkQueueSubmit。
// inSemaphore / outSemaphore 参数当前未使用（旧信号量方案已注释），m_currentSemaphore 仅作计数。
void Command::submit(std::shared_ptr<RaytracingIO> &raytracingio,
                     const std::optional<VkSemaphore> &inSemaphore, const std::optional<VkSemaphore> &outSemaphore)
{
    auto & m_currentSemaphore = raytracingio->m_currentSemaphore;
    // Preparing for the compute shader
    VkCommandBuffer cmdBuf = raytracingio->m_genCmdBuf.createCommandBuffer();
    m_inFlight.push_back(cmdBuf);

    // Fence completion of earlier submissions (light/buffer uploads, previous
    // row chunks) does not make their writes visible to the ray-tracing stage.
    VkMemoryBarrier memoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);

//    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
//    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
//    vkBeginCommandBuffer(cmdBuf, &beginInfo);


    recordCommandBuffer(cmdBuf,raytracingio);

    // new semaphores
//    std::array<VkPipelineStageFlags, 1> waitStages{VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
//    VkSubmitInfo submitInfoCompute{VK_STRUCTURE_TYPE_SUBMIT_INFO};
//    submitInfoCompute.waitSemaphoreCount =  static_cast<uint32_t>(1);
//    submitInfoCompute.pWaitSemaphores =  inSemaphore.has_value() ? &inSemaphore.value() : & raytracingio->m_semaphores[m_currentSemaphore - 1];
//    submitInfoCompute.pWaitDstStageMask = waitStages.data();
//    submitInfoCompute.commandBufferCount = 1;
//    submitInfoCompute.pCommandBuffers = &cmdBuf;
//    submitInfoCompute.signalSemaphoreCount = 1;
//    submitInfoCompute.pSignalSemaphores = outSemaphore.has_value() ? &outSemaphore.value() : & raytracingio->m_semaphores[m_currentSemaphore];

     // Dispatching the shader only for the other;
    // vkCmdDispatch(cmdBuf, dispatchSize.x, dispatchSize.y, dispatchSize.z);

    // old using only fense
    const VkPipelineStageFlags waitStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.pWaitDstStageMask = &waitStageMask;
    submitInfo.pCommandBuffers = &cmdBuf;
    submitInfo.commandBufferCount = 1;

    vkEndCommandBuffer(cmdBuf);
    vkCheck(vkQueueSubmit( raytracingio->m_queue, 1, &submitInfo,  raytracingio->m_fence), "Raytracing vkQueueSubmit");
    m_currentSemaphore++;
}

// 作用：记录一次光线追踪。追踪尺寸 = 图像宽 × 本块行数（dispatchRowCount > 0 时）或整幅图像高；
// 行偏移通过推送常量 setting.imageOffsetY 传给着色器（着色器侧用法待核对）。
// SBT 区域来自 m_sbtWrapper（useSBTWrapper）或手工构造的 raygen/miss/hit/callable 区域。
void Command::recordCommandBuffer(const VkCommandBuffer& cmdBuf, std::shared_ptr<RaytracingIO> &raytracingio)
{
    auto &m_pipeline = raytracingio->m_pipeline;
    auto &m_pipelineLayout = raytracingio->m_pipelineLayout;
    auto &m_descSet = raytracingio->m_descSet;
    auto &m_setting = raytracingio->setting;
    auto & useSBTWrapper = raytracingio->useSBTWrapper;
    auto & m_rgenRegion = raytracingio->m_rgenRegion;
    auto & m_missRegion = raytracingio->m_missRegion;
    auto & m_hitRegion = raytracingio->m_hitRegion;
    auto & m_callRegion = raytracingio->m_callRegion;
//    auto & m_sceneio = raytracingio->m_sceneio;
    auto & m_sbtWrapper= raytracingio->m_sbtWrapper;
    VkExtent2D size;
    size.width = raytracingio->imageSize.x;
    size.height = raytracingio->dispatchRowCount > 0
        ? static_cast<uint32_t>(raytracingio->dispatchRowCount)
        : static_cast<uint32_t>(raytracingio->imageSize.y);

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_pipeline);
    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_pipelineLayout, 0,
                            static_cast<uint32_t>(1), &m_descSet, 0, nullptr);
    vkCmdPushConstants(cmdBuf, m_pipelineLayout, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                       0, sizeof(RayRTSetting), &(raytracingio->setting));

    if (useSBTWrapper)
    {
        auto& regions = m_sbtWrapper.getRegions();
        vkCmdTraceRaysKHR(cmdBuf, &regions[0], &regions[1], &regions[2], &regions[3], size.width, size.height, 1);
    }
    else
    {
        vkCmdTraceRaysKHR(cmdBuf, &m_rgenRegion, &m_missRegion, &m_hitRegion, &m_callRegion, size.width, size.height, 1);
    }

//
//    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
//    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0,
//                            static_cast<uint32_t>(1), &m_descSet, 0, nullptr);
//    // Sending the push constant information
//    vkCmdPushConstants(cmdBuf, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VoxelLstSetting), &m_setting);
}

// 作用：等待本次提交完成（无超时），重置 fence，并销毁已完成的命令缓冲。
void Command::waitFence(std::shared_ptr<RaytracingIO> &raytracingio)
{
    vkCheck(vkWaitForFences(raytracingio->m_device, 1, &(raytracingio->m_fence), VK_TRUE, UINT64_MAX), "Raytracing GPU execution");
    vkResetFences(raytracingio->m_device, 1,  &(raytracingio->m_fence));
    raytracingio->m_genCmdBuf.destroy(m_inFlight);
    m_inFlight.clear();
}


// 作用：若仍有在途命令缓冲，先等待设备空闲再回收；然后销毁 fence 与全部信号量。
void Command::destroy(std::shared_ptr<RaytracingIO> &raytracingio) {

    if (!m_inFlight.empty()) {
        vkDeviceWaitIdle(raytracingio->m_device);
        raytracingio->m_genCmdBuf.destroy(m_inFlight);
        m_inFlight.clear();
    }
    vkDestroyFence(raytracingio->m_device, raytracingio->m_fence, nullptr);
       // vkFreeCommandBuffers(m_device, m_cmdPool, 1, &m_commandBuffers[i]);

    for (auto& semaphore : raytracingio->m_semaphores)
    {
        vkDestroySemaphore(raytracingio->m_device, semaphore, nullptr);
    }

}
