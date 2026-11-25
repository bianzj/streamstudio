// ==========================================================================
// 文件：virtualscreen.h
// 功能：VirtualScreen——把设备端缓冲拷贝到主机可读缓冲（回读）的统一入口声明。
// 主要模块：
//   1. VirtualScreen::bufferToBuffer：针对 RaytracingIO / VoxelebIO / VoxelrtIO 三个重载
// 数据流：模式引擎（raytracing.cpp、voxeleb.cpp、voxelrt.cpp）提供源缓冲、字节数、目标暂存缓冲
//         → 实现在 virtualscreen.cpp，三个重载共用同一个模板实现。
// 关系：被 raytracing.h、voxeleb.h、voxelrt.h 以 m_pVirtual 持有。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_VIRTUALSCREEN_H
#define FIELD_VIRTUALSCREEN_H

#include "src/raytracing/raytracingio.h"
#include "src/voxelrt/voxelrtio.h"
#include "src/voxeleb/voxelebio.h"

// 类：VirtualScreen。无状态，仅提供回读函数。
class VirtualScreen {
public:
    VirtualScreen(){};

    // 作用：把 bufferIn 的前 size 字节拷贝到 bufferOut（用于主机读取结果）。
    // 输入：模式 IO（提供 device/queue）、源缓冲、字节数 size、目标缓冲。
    // 返回：size 超过任一缓冲的内存需求时返回 false，否则 true。三个重载仅 IO 类型不同。
    bool bufferToBuffer(std::shared_ptr<RaytracingIO> & modelio,
                        const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut);

    bool bufferToBuffer(std::shared_ptr<VoxelebIO> & modelio,
                        const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut);

    bool bufferToBuffer(std::shared_ptr<VoxelrtIO> & modelio,
                        const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut);

};


#endif //FIELD_VIRTUALSCREEN_H
