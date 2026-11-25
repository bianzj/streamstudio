// ==========================================================================
// 文件：buffer.h
// 功能：VoxelEB 模式 GPU 缓冲管理类 Buffer 的声明（无成员变量，只有两个操作）。
// 主要模块：
//   1. Buffer::createBuffer  创建全部 GPU 缓冲
//   2. Buffer::destroy       销毁缓冲
// 数据流：两者都以 VoxelebIO（voxelebio.h）为唯一输入/输出，缓冲句柄存放在它的子对象中。
// 关系：实现见 buffer.cpp；由 voxeleb.cpp 调用。
// 注意：raytracing/buffer.h 中也有同名类 Buffer，两者位于不同目录、绑定不同的 IO 类型
//       （是否同时编译进同一可执行文件会不会冲突，待核对）。
// ==========================================================================

//
// Created by admin on 2024/1/28.
//

#ifndef FIELD_BUFFER_H
#define FIELD_BUFFER_H

#include <nvvk/buffers_vk.hpp>
#include "voxelebio.h"



// 类：Buffer——VoxelEB 的缓冲创建/销毁工具；参数名 voxellstio 即 VoxelebIO（沿用旧名）。
class Buffer {

public:
    Buffer(){}
    bool createBuffer(std::shared_ptr<VoxelebIO> &voxellstio);
    void destroy(std::shared_ptr<VoxelebIO> &voxellstio);
};


#endif //FIELD_BUFFER_H
