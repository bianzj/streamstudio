//
// Created by admin on 2024/1/26.
//

#include "command.h"
#include "base/vkcheck.h"

#include <algorithm>



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


bool Command::runRT(std::shared_ptr<VoxelrtIO> &modelio){

    modelio->m_currentSemaphore = 1;

    glm::ivec3 voxelSize2D = glm::ivec3((modelio->setting.imageSize.x + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY,
                                        (modelio->setting.imageSize.y + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY, 1);
    glm::ivec3 voxelSize1D = glm::ivec3((modelio->n_voxel + (GROUP_SIZEX - 1)) / GROUP_SIZEX, 1, 1);
    // DEFERRED[中] GPU同步: 1D 派发组数 = n_voxel/64，超过 maxComputeWorkGroupCount[0]（规范最小值 65535，部分 AMD/Intel 驱动即为 65535）时 vkCmdDispatch 属未定义行为，即 >4.19M 体元的场景在这些 GPU 上失败；建议：查询限制并拆成 2D 派发或分批 + 偏移推送常量（runRTAccelerated 同理）。
    auto & descSet = modelio->m_descSet;
    auto & setting = modelio->setting;


    //--------------------------------------------------
    // nvmath::vec3i size = nvmath::vec3i((m_setting.size.x + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY,
    //                                    (m_setting.size.y + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY, 1);


    submit(modelio, VoxelRTStage::gap, voxelSize1D, std::nullopt, std::nullopt);
    waitFence(modelio);

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
    setting.n_jump = 0;
    setting.collisionOrder = 1;

    submit(modelio, VoxelRTStage::out, voxelSize2D, std::nullopt, std::nullopt);
    waitFence(modelio);



    return true;
}

bool Command::runRTAccelerated(std::shared_ptr<VoxelrtIO>& modelio,
                               int spectralBatchSize)
{
    modelio->m_currentSemaphore = 1;
    spectralBatchSize = std::clamp(spectralBatchSize, 1, 4);

    const glm::ivec3 voxelSize2D(
        (modelio->setting.imageSize.x + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY,
        (modelio->setting.imageSize.y + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY,
        1);
    const glm::ivec3 voxelSize1D(
        (modelio->n_voxel + (GROUP_SIZEX - 1)) / GROUP_SIZEX, 1, 1);

    submit(modelio, VoxelRTStage::gap, voxelSize1D,
           std::nullopt, std::nullopt);
    waitFence(modelio);

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
    modelio->setting.n_jump = 0;
    modelio->setting.collisionOrder = 1;
    modelio->setting.spectralBatchSize = 1;

    submit(modelio, VoxelRTStage::out, voxelSize2D,
           std::nullopt, std::nullopt);
    waitFence(modelio);
    return true;
}


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



void Command::waitFence(std::shared_ptr<VoxelrtIO> &modelio)
{
    vkCheck(vkWaitForFences(modelio->m_device, 1, &(modelio->m_fence), VK_TRUE, UINT64_MAX), "VoxelRT GPU execution");
    vkResetFences(modelio->m_device, 1,  &(modelio->m_fence));
    modelio->m_genCmdBuf.destroy(m_inFlight);
    m_inFlight.clear();
}


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
