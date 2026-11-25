// ==========================================================================
// 文件：buffer.h（raytracing）
// 功能：RayTracing 模式 GPU 缓冲区创建/销毁的接口声明（Buffer 类）。
// 主要模块：
//   1. Buffer::createBuffer：上传材质、网格、实例链接、纹理因子、传感器/光源参数，并构建加速结构，
//      创建输出图像缓冲
//   2. Buffer::destroy：释放上述缓冲
// 数据流：全部句柄存放在 RaytracingIO（raytracingio.h）及其 meshio / instanceio / virtualio 中，
//   供 Descriptor 绑定、Command 提交使用。
// 与其他文件的关系：实现见 buffer.cpp；由 raytracing.cpp 的 Raytracing::create / destroy 调用。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_BUFFER_H
#define FIELD_BUFFER_H

#include <nvvk/buffers_vk.hpp>
#include "raytracingio.h"


// 类：Buffer —— RayTracing 的缓冲区阶段（无成员状态，所有资源保存在 RaytracingIO 中）。
class Buffer {
public:
    Buffer(){}
    bool createBuffer(std::shared_ptr<RaytracingIO> &raytracingio);
    void destroy(std::shared_ptr<RaytracingIO> &raytracingio);

};


#endif //FIELD_BUFFER_H
