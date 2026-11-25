// ==========================================================================
// 文件：command.cpp
// 功能：VoxelRT 模式的计算命令录制、提交与同步
// 主要模块/步骤：
//   1. create：创建围栏与信号量
//   2. runRT：逐波段、逐碰撞阶数分次派发 diffuse 阶段，再派发 out 阶段
//   3. runRTAccelerated：按光谱批（最多 4 个波段）合并 diffuse 派发的加速版本
//   4. submit / recordCommandBuffer：录制单次派发（内存屏障、管线、描述符集、推送常量）并提交
//   5. waitFence / destroy：等待围栏、回收命令缓冲、销毁围栏与信号量
// 数据流：VoxelrtIO 中的 setting（VoxelRTSetting，作为推送常量）、m_pipelines、m_descSet
//   → vkCmdDispatch → 计算着色器读写 buffer.cpp 创建的缓冲
// 与其他文件的关系：声明见 command.h；阶段枚举 VoxelRTStage（gap/diffuse/out）与
//   GROUP_SIZEX(64)/GROUP_SIZEXY(8) 在 base/structs.h；描述符见 descriptor.cpp
// 约定：1D 派发按体元（每组 GROUP_SIZEX 个），2D 派发按影像像素（每组 GROUP_SIZEXY²）；
//   每次提交后立即 waitFence，阶段之间串行
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#include "command.h"
#include "base/vkcheck.h"

#include <algorithm>



// 模块：创建同步对象。输出：modelio->m_fence 与 m_semaphores（n_pipeline + 1 个）。
// 说明：当前 submit 中信号量相关代码已被注释，信号量实际未参与同步，同步靠围栏。
bool Command::create(std::shared_ptr<VoxelrtIO> &modelio){
    // for the firest init
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCheck(vkCreateFence(modelio->m_device, &fci, nullptr, &modelio->m_fence), "vkCreateFence");
    // vkWaitForFences(modelio->m_device, 1, &(modelio->m_fence), VK_TRUE, UINT64_MAX);
    vkResetFences(modelio->m_device, 1,  &(modelio->m_fence));

    // Here 4 means number of pipelines + 1
    modelio->m_semaphores.resize(modelio->n_pipeline+1);
    std::generate_n( modelio->m_semaphores.begin(), modelio->n_pipeline+1,
                     [&]
                     {
                         VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                         VkSemaphore semaphore;
                         vkCheck(vkCreateSemaphore( modelio->m_device, &sci, nullptr, &semaphore), "vkCreateSemaphore");
                         return semaphore;
                     });

    return false;
}


// 模块：VoxelRT 主流程（逐波段、逐碰撞阶数分次提交）。
// 输入：reuseTransport 为 true 表示太阳方向未变，gap 与 diffuse 结果（与视角无关）仍有效，
//   只重跑 out 阶段；输出：写入 m_pBufferStorage 等缓冲，返回 true。
// 阶段：gap（体元间隙/透过率相关，待核对）→ diffuse（多次碰撞的漫射辐射）→ out（成像）。
bool Command::runRT(std::shared_ptr<VoxelrtIO> &modelio, bool reuseTransport){

    modelio->m_currentSemaphore = 1;

    glm::ivec3 voxelSize2D = glm::ivec3((modelio->setting.imageSize.x + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY,
                                        (modelio->setting.imageSize.y + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY, 1);
    glm::ivec3 voxelSize1D = glm::ivec3((modelio->n_voxel + (GROUP_SIZEX - 1)) / GROUP_SIZEX, 1, 1);
    // DEFERRED[中] GPU同步: 1D 派发组数 = n_voxel/64，超过 maxComputeWorkGroupCount[0]（规范最小值 65535，部分 AMD/Intel 驱动即为 65535）时 vkCmdDispatch 属未定义行为，即 >4.19M 体元的场景在这些 GPU 上失败；建议：查询限制并拆成 2D 派发或分批 + 偏移推送常量（runRTAccelerated 同理）。
    auto & descSet = modelio->m_descSet;
    auto & setting = modelio->setting;

    // 步骤 1：复用上次的传输结果，只做成像。
    if (reuseTransport) {
        submit(modelio, VoxelRTStage::out, voxelSize2D, std::nullopt, std::nullopt);
        waitFence(modelio);
        return true;
    }


    //--------------------------------------------------
    // nvmath::vec3i size = nvmath::vec3i((m_setting.size.x + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY,
    //                                    (m_setting.size.y + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY, 1);


    // 步骤 2：gap 阶段，按体元 1D 派发。
    submit(modelio, VoxelRTStage::gap, voxelSize1D, std::nullopt, std::nullopt);
    waitFence(modelio);

    // 步骤 3：diffuse 阶段，循环 波段(n_jump) × 碰撞阶数(collisionOrder)，每次单独提交并等待。
    // A wavelength is independent at this stage. Dispatch it separately so
    // dense hyperspectral jobs do not become one long GPU command and trip
    // the Windows TDR watchdog.
    // Each collision order reads the previous order of every voxel, so the
    // orders of one band are separate, sequential dispatches.
    for (int band = 0; band < modelio->n_wave; ++band) {
        setting.n_jump = band;
        for (int order = 1; order <= setting.collisionOrders; ++order) {
            setting.collisionOrder = order;
            submit(modelio, VoxelRTStage::diffuse, voxelSize1D, std::nullopt, std::nullopt);
            waitFence(modelio);
        }
    }
    // 步骤 4：恢复推送常量缺省值（n_jump = 0、collisionOrder = 1），再做 out 阶段（2D 派发）。
    setting.n_jump = 0;
    setting.collisionOrder = 1;

    submit(modelio, VoxelRTStage::out, voxelSize2D, std::nullopt, std::nullopt);
    waitFence(modelio);



    return true;
}

// 模块：VoxelRT 加速版主流程。与 runRT 的区别：diffuse 阶段每次提交处理一批波段
//   （spectralBatchSize，限制在 1..4），减少提交次数；其余阶段相同。
bool Command::runRTAccelerated(std::shared_ptr<VoxelrtIO>& modelio,
                               int spectralBatchSize, bool reuseTransport)
{
    modelio->m_currentSemaphore = 1;
    // 批大小限幅到 1..4（上限与着色器一次处理的波段数有关，待核对）。
    spectralBatchSize = std::clamp(spectralBatchSize, 1, 4);

    const glm::ivec3 voxelSize2D(
        (modelio->setting.imageSize.x + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY,
        (modelio->setting.imageSize.y + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY,
        1);
    const glm::ivec3 voxelSize1D(
        (modelio->n_voxel + (GROUP_SIZEX - 1)) / GROUP_SIZEX, 1, 1);

    // 步骤 1：复用上次的传输结果，只做成像。
    if (reuseTransport) {
        submit(modelio, VoxelRTStage::out, voxelSize2D,
               std::nullopt, std::nullopt);
        waitFence(modelio);
        return true;
    }

    // 步骤 2：gap 阶段，按体元 1D 派发。
    submit(modelio, VoxelRTStage::gap, voxelSize1D,
           std::nullopt, std::nullopt);
    waitFence(modelio);

    // 步骤 3：按批遍历波段；n_jump = 本批第一个波段，最后一批可能不足 spectralBatchSize。
    //   同一批内每个碰撞阶数各提交一次，需读上一阶全部体元，因此阶数之间串行。
    for (int firstBand = 0; firstBand < modelio->n_wave;
         firstBand += spectralBatchSize) {
      for (int order = 1; order <= modelio->setting.collisionOrders; ++order) {
        modelio->setting.collisionOrder = order;
        VkCommandBuffer commandBuffer =
            modelio->m_genCmdBuf.createCommandBuffer();
        m_inFlight.push_back(commandBuffer);

        modelio->setting.n_jump = firstBand;
        modelio->setting.spectralBatchSize = std::min(
            spectralBatchSize, modelio->n_wave - firstBand);
        recordCommandBuffer(
            commandBuffer, modelio->m_descSet,
            modelio->m_pipelineLayout,
            modelio->m_pipelines[VoxelRTStage::diffuse],
            modelio->setting);
        vkCmdDispatch(commandBuffer, voxelSize1D.x,
                      voxelSize1D.y, voxelSize1D.z);

        vkEndCommandBuffer(commandBuffer);
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        vkCheck(vkQueueSubmit(modelio->m_queue, 1, &submitInfo, modelio->m_fence), "VoxelRT vkQueueSubmit");
        waitFence(modelio);
      }
    }
    // 步骤 4：恢复推送常量缺省值，再做 out 阶段。
    modelio->setting.n_jump = 0;
    modelio->setting.collisionOrder = 1;
    modelio->setting.spectralBatchSize = 1;

    submit(modelio, VoxelRTStage::out, voxelSize2D,
           std::nullopt, std::nullopt);
    waitFence(modelio);
    return true;
}


// 模块：录制并提交某阶段的一次计算派发（不等待，需随后 waitFence）。
// 输入：stage 选择管线，dispatchSize 为工作组数；inSemaphores/outSemaphore 当前未使用。
// 副作用：命令缓冲记入 m_inFlight，待 waitFence 回收；m_currentSemaphore 自增。
void Command::submit(std::shared_ptr<VoxelrtIO> &modelio, VoxelRTStage stage, glm::ivec3 dispatchSize,
                     const std::optional<std::vector<VkSemaphore>> &inSemaphores, const std::optional<VkSemaphore> &outSemaphore)
{

    auto & descSet = modelio->m_descSet;
    auto & setting = modelio->setting;
    auto & pipelineLayout = modelio->m_pipelineLayout;
    auto & pipeline = modelio->m_pipelines[stage];


    auto & m_currentSemaphore = modelio->m_currentSemaphore;
    // Preparing for the compute shader
    VkCommandBuffer cmdBuf = modelio->m_genCmdBuf.createCommandBuffer();
    m_inFlight.push_back(cmdBuf);

    // VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    // beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    // beginInfo.pNext = nullptr;
    // beginInfo.pInheritanceInfo = nullptr;
    // vkBeginCommandBuffer(cmdBuf, &beginInfo);

    // Dispatching the shader only for the other;
    recordCommandBuffer(cmdBuf, descSet, pipelineLayout, pipeline, setting );
    vkCmdDispatch(cmdBuf, dispatchSize.x, dispatchSize.y, dispatchSize.z);

    auto & semaphores = modelio->m_semaphores;
    // new semaphores
    std::array<VkPipelineStageFlags, 1> waitStages{VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    VkSubmitInfo submitInfoCompute{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfoCompute.commandBufferCount = 1;
    submitInfoCompute.pCommandBuffers = &cmdBuf;
    // submitInfoCompute.waitSemaphoreCount =  static_cast<uint32_t>(1);
    // submitInfoCompute.pWaitSemaphores =  inSemaphores.has_value() ? inSemaphores->data() : &semaphores[m_currentSemaphore-1];
    // submitInfoCompute.pWaitDstStageMask = waitStages.data();
    // submitInfoCompute.signalSemaphoreCount = 1;
    // submitInfoCompute.pSignalSemaphores = outSemaphore.has_value() ? &outSemaphore.value() : & semaphores[m_currentSemaphore];


    vkEndCommandBuffer(cmdBuf);
    vkCheck(vkQueueSubmit( modelio->m_queue, 1, &submitInfoCompute,  modelio->m_fence), "VoxelRT vkQueueSubmit");
    m_currentSemaphore++;
}


// 模块：向 cmdBuf 录制一次派发前的公共命令（不含 vkCmdDispatch 与提交）。
// 输入：管线、描述符集、推送常量 setting（VoxelRTSetting，按值传入）。
void Command::recordCommandBuffer(VkCommandBuffer cmdBuf, VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
                                  VkPipeline pipeline,  VoxelRTSetting setting)
{
    // gap -> diffuse -> out are separate submissions; fence completion alone
    // does not make the previous stage's SSBO writes visible to this dispatch.
    VkMemoryBarrier memoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0,
                            static_cast<uint32_t>(1), &descSet, 0, nullptr);
    // Sending the push constant information
    vkCmdPushConstants(cmdBuf, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VoxelRTSetting), &setting);
}



// 模块：等待最近一次提交完成，重置围栏并回收 m_inFlight 中的命令缓冲。
void Command::waitFence(std::shared_ptr<VoxelrtIO> &modelio)
{
    vkCheck(vkWaitForFences(modelio->m_device, 1, &(modelio->m_fence), VK_TRUE, UINT64_MAX), "VoxelRT GPU execution");
    vkResetFences(modelio->m_device, 1,  &(modelio->m_fence));
    modelio->m_genCmdBuf.destroy(m_inFlight);
    m_inFlight.clear();
}


// 模块：销毁；若仍有未回收的命令缓冲，先 vkDeviceWaitIdle 再释放，然后销毁围栏与信号量。
void Command::destroy(std::shared_ptr<VoxelrtIO> &modelio) {

    if (!m_inFlight.empty()) {
        vkDeviceWaitIdle(modelio->m_device);
        modelio->m_genCmdBuf.destroy(m_inFlight);
        m_inFlight.clear();
    }
    vkDestroyFence(modelio->m_device, modelio->m_fence, nullptr);
       // vkFreeCommandBuffers(m_device, m_cmdPool, 1, &m_commandBuffers[i]);

    for (auto& semaphore : modelio->m_semaphores)
    {
        vkDestroySemaphore(modelio->m_device, semaphore, nullptr);
    }

}
