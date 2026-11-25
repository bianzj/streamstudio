// ==========================================================================
// 文件：command.h（raytracing）
// 功能：RayTracing 模式命令提交阶段的接口声明（Command 类）：创建同步对象、分块提交光线追踪、等待完成。
// 主要模块：
//   1. create：创建 fence 与信号量
//   2. run：按行分块追踪整幅图像（自适应块大小）
//   3. submit / recordCommandBuffer / waitFence：提交、记录命令、等待与回收命令缓冲
//   4. destroy：销毁同步对象
// 数据流：raytracing.cpp 的 Raytracing::run 调用 run；run 内部循环调用 submit 与 waitFence。
// 与其他文件的关系：实现见 command.cpp；管线、描述符集、SBT 区域等句柄来自 RaytracingIO。
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_COMMAND_H
#define FIELD_COMMAND_H

#include "raytracingio.h"
#include <vector>

// 类：Command —— 命令提交阶段。m_inFlight 记录已提交、尚未被 waitFence 回收的命令缓冲。
class Command {
public:
    Command();

    // 作用：创建 fence 与 2 个信号量（目前提交只用 fence，信号量路径已被注释掉）。
    bool create(std::shared_ptr<RaytracingIO> &raytracingio);

    // 作用：把整幅图像按行分块追踪，每块提交后等待完成；返回 true。
    bool run(std::shared_ptr<RaytracingIO> &raytracingio);
//    void submit(std::shared_ptr<RaytracingIO> &raytracingio,
//                const std::optional<std::vector<VkSemaphore>> &inSemaphore,
//                const std::optional<VkSemaphore> &outSemaphore);

    // 作用：记录并提交一块的命令缓冲（带内存屏障），用 fence 同步；inSemaphore / outSemaphore 参数当前未使用。
    void submit(std::shared_ptr<RaytracingIO> &raytracingio,
                const std::optional<VkSemaphore> &inSemaphore,
                const std::optional<VkSemaphore> &outSemaphore);

    // 作用：向 cmdBuf 记录绑定管线/描述符集、推送常量与 vkCmdTraceRaysKHR。
    void recordCommandBuffer(const VkCommandBuffer& cmdBuf,std::shared_ptr<RaytracingIO> &raytracingio);
    // 作用：等待 fence、重置，并释放已完成的命令缓冲（m_inFlight）。
    void waitFence(std::shared_ptr<RaytracingIO> &raytracingio);
    // 作用：等待设备空闲（若仍有在途命令），销毁 fence 与信号量。
    void destroy(std::shared_ptr<RaytracingIO> &raytracingio);

private:
    // Submitted command buffers, freed once waitFence() observes completion.
    std::vector<VkCommandBuffer> m_inFlight;
};


#endif //FIELD_COMMAND_H
