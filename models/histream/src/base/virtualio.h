// ==========================================================================
// 文件：virtualio.h
// 功能：VirtualIO——各模式 IO 对象（raytracingio/voxelebio/voxelrtio）共用的通用存储缓冲容器。
// 主要模块：
//   1. VirtualIO 类：持有一个 Vulkan 存储缓冲 m_pBufferStorage
// 数据流：RayTracing 等模式把结果写入 m_pBufferStorage → VirtualScreen::bufferToBuffer
//         回读到主机可见缓冲（见 virtualscreen.cpp）。
// 关系：被 raytracingio.h、voxelebio.h、voxelrtio.h 以 m_virtualio 持有。
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_VIRTUALIO_H
#define FIELD_VIRTUALIO_H


#include "structs.h"
#include "structs_cg.h"

// 类：VirtualIO。纯数据持有，缓冲的创建/销毁由各模式的 buffer.cpp 负责。
class VirtualIO {
public:
    VirtualIO(){};

    // 通用输出/存储缓冲（用途由所属模式决定，RayTracing 回读时直接使用它）。
    std::shared_ptr<nvvk::Buffer> m_pBufferStorage;
};


#endif //FIELD_VIRTUALIO_H
