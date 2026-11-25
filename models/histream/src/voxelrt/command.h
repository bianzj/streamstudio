// ==========================================================================
// 文件：command.h
// 功能：VoxelRT 模式的计算命令提交与同步接口声明
// 主要模块：
//   1. Command::create：创建围栏与信号量
//   2. runRT / runRTAccelerated：按 gap → diffuse → out 三个阶段依次派发计算着色器
//   3. submit / recordCommandBuffer：录制并提交单个阶段的计算命令
//   4. waitFence / destroy：等待完成并释放命令缓冲、围栏与信号量
// 数据流：VoxelrtIO 中的管线、描述符集与 VoxelRTSetting（推送常量）→ GPU 派发
// 与其他文件的关系：实现在 command.cpp；描述符见 descriptor.h；VoxelRTStage 在 base/structs.h
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_COMMAND_H
#define FIELD_COMMAND_H

#include "voxelrtio.h"
#include <vector>

// 类：VoxelRT 的命令提交器；m_inFlight 记录已提交、待 waitFence 回收的命令缓冲。
class Command {
public:
    Command();

    // 创建围栏与信号量（信号量个数 = n_pipeline + 1）。
    bool create(std::shared_ptr<VoxelrtIO> &VoxellstIO);
//    bool run(std::shared_ptr<VoxelrtIO> &VoxelrtIO);

    // reuseTransport: the sun direction is unchanged since the previous call,
    // so the gap and diffuse fields (view independent) are still valid and
    // only the image pass runs.
    bool runRT(std::shared_ptr<VoxelrtIO> &modelio, bool reuseTransport = false);
    bool runRTAccelerated(std::shared_ptr<VoxelrtIO> &modelio,
                          int spectralBatchSize = 4, bool reuseTransport = false);

//    void submit(std::shared_ptr<VoxelrtIO> &VoxelrtIO,glm::ivec3 dispatchSize,
//                VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
//                std::map<VoxelRadStage, VkPipeline> pipelines, VoxelRadStage stage,VoxelLstSetting setting,
//                const std::optional<VkSemaphore> &inSemaphore, const std::optional<VkSemaphore> &outSemaphore);

    // 录制并提交某一阶段（stage）的一次派发，不等待完成；需随后调用 waitFence。
    void submit(std::shared_ptr<VoxelrtIO> &VoxellstIO, VoxelRTStage stage, glm::ivec3 dispatchSize,
                const std::optional<std::vector<VkSemaphore>> &inSemaphore, const std::optional<VkSemaphore> &outSemaphore);


//    void submit(std::shared_ptr<VoxelrtIO> &VoxelrtIO,glm::ivec3 dispatchSize,
//                VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
//                VkPipeline pipeline, VoxelLstSetting setting,
//                const std::optional<VkSemaphore> &inSemaphore, const std::optional<VkSemaphore> &outSemaphore);

    // 录制：内存屏障 + 绑定管线/描述符集 + 推送常量 VoxelRTSetting（按值传入）。
    // （类内声明不带 Command:: 限定：GCC/Clang 视多余限定为错误，MSVC 才容忍。）
    void recordCommandBuffer(VkCommandBuffer cmdBuf,
                             VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
                             VkPipeline pipeline,  VoxelRTSetting setting);

//    void Command::recordCommandBuffer(VkCommandBuffer cmdBuf, VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
//                                      std::map<VoxelEBStage, VkPipeline> pipelines, VoxelEBStage stage, VoxelLstSetting setting);

//    void recordCommandBuffer(const VkCommandBuffer& cmdBuf,std::shared_ptr<VoxelrtIO> &VoxelrtIO);
    void waitFence(std::shared_ptr<VoxelrtIO> &VoxellstIO);
    void destroy(std::shared_ptr<VoxelrtIO> &VoxellstIO);

private:
    // Submitted command buffers, freed once waitFence() observes completion.
    std::vector<VkCommandBuffer> m_inFlight;
};


#endif //FIELD_COMMAND_H
