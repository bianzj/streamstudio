// ==========================================================================
// 文件：command.h
// 功能：Command 类声明 —— VoxelEB 的 GPU 命令录制、提交与同步（实现见 command.cpp）
// 主要模块：
//   1. create / destroy：fence 与信号量的生命周期
//   2. runEB / runRT / runFluid：能量平衡、传感器成像、LBM 流体三类高层调度
//   3. submit / recordCommandBuffer / waitFence：单阶段（VoxelEBStage）录制、提交与等待
// 与其他文件的关系：依赖 voxelebio.h（VoxelebIO 聚合设备、缓冲、管线、setting）；
//   由 voxeleb.cpp 按时间节点调用；管线与描述符集分别来自 pipeline.cpp / descriptor.cpp
// 注意：m_inFlight 保存已提交但尚未释放的命令缓冲，waitFence 成功后统一释放
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_COMMAND_H
#define FIELD_COMMAND_H

#include "voxelebio.h"
#include <vector>

// 模块：VoxelEB 命令调度器。状态仅有 m_inFlight，其余全部经 VoxelebIO 传递。
class Command {
public:
    Command();

    // 创建 fence 与信号量。
    bool create(std::shared_ptr<VoxelebIO> &VoxellstIO);
//    bool run(std::shared_ptr<VoxelebIO> &VoxelebIO);

    // 高层调度：runEB = 单节点能量平衡（短波/长波辐射 + 迭代求温度）；runRT = 传感器成像；
    // runFluid = 推进 iterations 步 LBM 流体。返回值恒为 true（出错抛异常）。
    bool runEB(std::shared_ptr<VoxelebIO> &modelio);
    bool runRT(std::shared_ptr<VoxelebIO> &modelio);
    bool runFluid(std::shared_ptr<VoxelebIO> &modelio, int iterations);

//    void submit(std::shared_ptr<VoxelebIO> &VoxelebIO,glm::ivec3 dispatchSize,
//                VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
//                std::map<VoxelRadStage, VkPipeline> pipelines, VoxelRadStage stage,VoxelLstSetting setting,
//                const std::optional<VkSemaphore> &inSemaphore, const std::optional<VkSemaphore> &outSemaphore);

    // 录制并提交一个阶段的计算派发（semaphore 参数当前未使用，同步靠 fence 与内存屏障）。
    void submit(std::shared_ptr<VoxelebIO> &VoxellstIO, VoxelEBStage stage, glm::ivec3 dispatchSize,
                const std::optional<std::vector<VkSemaphore>> &inSemaphore, const std::optional<VkSemaphore> &outSemaphore);


//    void submit(std::shared_ptr<VoxelebIO> &VoxelebIO,glm::ivec3 dispatchSize,
//                VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
//                VkPipeline pipeline, VoxelLstSetting setting,
//                const std::optional<VkSemaphore> &inSemaphore, const std::optional<VkSemaphore> &outSemaphore);

    // 向命令缓冲录入：绑定计算管线、绑定描述符集、推送 VoxelLstSetting 常量。
    // （类内声明不带 Command:: 限定：GCC/Clang 视多余限定为错误，MSVC 才容忍。）
    void recordCommandBuffer(VkCommandBuffer cmdBuf,
                             VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
                             VkPipeline pipeline,  VoxelLstSetting setting);

//    void Command::recordCommandBuffer(VkCommandBuffer cmdBuf, VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
//                                      std::map<VoxelEBStage, VkPipeline> pipelines, VoxelEBStage stage, VoxelLstSetting setting);

//    void recordCommandBuffer(const VkCommandBuffer& cmdBuf,std::shared_ptr<VoxelebIO> &VoxelebIO);
    // 等待 fence 并释放已完成的命令缓冲；destroy 销毁 fence 与信号量。
    void waitFence(std::shared_ptr<VoxelebIO> &VoxellstIO);
    void destroy(std::shared_ptr<VoxelebIO> &VoxellstIO);

private:
    // Submitted command buffers, freed once waitFence() observes completion.
    std::vector<VkCommandBuffer> m_inFlight;
};


#endif //FIELD_COMMAND_H
