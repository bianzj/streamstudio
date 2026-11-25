// ==========================================================================
// 文件：descriptor.h（raytracing）
// 功能：RayTracing 模式描述符集的接口声明（Descriptor 类）及着色器绑定编号枚举。
// 主要模块：
//   1. Descriptor::RaytracingbindingInd：绑定编号枚举
//   2. Descriptor::createDescriptor / destroy：创建/销毁描述符布局、池与描述符集
// 数据流：位于 Buffer 之后、Pipeline 之前；把 Buffer 创建的缓冲和 TLAS 绑到同一个描述符集（set 0）。
// 与其他文件的关系：枚举值须与 shader/bindinglayout.h 中 RAYTRACING 段的 B_* 宏一致（0–10）；
//   实现见 descriptor.cpp；由 raytracing.cpp 的 Raytracing::create / destroy 调用。
// ==========================================================================

//
// Created by admin on 2024/1/25.
//

#ifndef FIELD_DESCRIPTOR_H
#define FIELD_DESCRIPTOR_H

#include "raytracingio.h"

// 类：Descriptor —— RayTracing 的描述符阶段（无成员状态，句柄保存在 RaytracingIO 中）。
class Descriptor {
public:

    // 枚举：描述符绑定编号，自上而下为 0..10，与 shader/bindinglayout.h 的 B_SPECTRAL..B_TEXFACTOR 对应：
    // 光谱材料、热材料、模型链接、实例链接、TLAS、传感器、光源、波长集、输出存储、纹理偏移、纹理因子。
    enum RaytracingbindingInd
    {
        spectral,
        thermal,
        modelLink,
        instanceLink,
        tlas,
        sensor,
        //   waveInd,
        light,
        wave,
        storage,
        textureOffset,
        textureFactor
    };


    bool createDescriptor(std::shared_ptr<RaytracingIO> &raytracingio);
    void destroy(std::shared_ptr<RaytracingIO> &raytracingio);

};


#endif //FIELD_DESCRIPTOR_H
