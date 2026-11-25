// ==========================================================================
// 文件：buffer.h
// 功能：VoxelRT 模式（体元辐射）GPU 缓冲区的创建/销毁接口声明
// 主要模块：
//   1. Buffer::createBuffer：把 VoxelrtIO 中的场景/体元/传感器数据上传为 Vulkan 缓冲
//   2. Buffer::destroy：释放上述缓冲
// 数据流：VoxelrtIO（CPU 侧数据）→ 本类 → nvvk::Buffer（供 descriptor.cpp 绑定）
// 与其他文件的关系：实现在 buffer.cpp；缓冲由 descriptor.cpp 写入描述符集；
//   着色器通过 descriptor.h 中的 binding 序号读写这些缓冲
// ==========================================================================

//
// Created by admin on 2024/1/28.
//

#ifndef FIELD_BUFFER_H
#define FIELD_BUFFER_H

#include <nvvk/buffers_vk.hpp>
#include "voxelrtio.h"



// 类：VoxelRT 的缓冲区管理器（无成员状态，缓冲句柄都保存在 VoxelrtIO 的各子结构中）。
class Buffer {

public:
    Buffer(){}
    // 创建全部缓冲；注意 buffer.cpp 中成功路径当前返回 false（见该处说明）。
    bool createBuffer(std::shared_ptr<VoxelrtIO> &voxellstio);
    void destroy(std::shared_ptr<VoxelrtIO> &voxellstio);
};


#endif //FIELD_BUFFER_H
