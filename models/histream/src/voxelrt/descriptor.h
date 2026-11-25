// ==========================================================================
// 文件：descriptor.h
// 功能：VoxelRT 模式的描述符集布局（binding 序号枚举）及创建/销毁接口声明
// 主要模块：
//   1. VoxelrtbindingInd：描述符 binding 序号枚举（须与 VoxelRT 着色器中的 binding 一致）
//   2. Descriptor::createDescriptor / destroy：创建/销毁布局、描述符池与描述符集
// 数据流：buffer.cpp 创建的缓冲 → descriptor.cpp 写入描述符集 → 计算着色器读写
// 与其他文件的关系：实现在 descriptor.cpp；管线布局与推送常量见 command.cpp
// 注意：枚举项顺序即 binding 号（从 0 起），增删项需同步修改着色器（待核对着色器路径）
// ==========================================================================

//
// Created by admin on 2024/1/25.
//

#ifndef FIELD_DESCRIPTOR_H
#define FIELD_DESCRIPTOR_H

#include "voxelrtio.h"

class Descriptor {
public:

    // 模块：binding 序号。顺序与 descriptor.cpp 中 addBinding 的顺序一一对应，
    // 着色器中的声明也须一致（待核对）。
    enum VoxelrtbindingInd
    {
        spectral,  // for the specific band
        thermal,  // for the voxel with constant temperature;
        canopy,
        meshLink,
        instanceLink,
        voxelLink,
        nano,
        tlas,
        sensor,
        wave,
        light,
        dir,
        rads,
        netRad,
        storage,
        lad,
        hex,
        waterSet,
    };


    // 创建布局、池和描述符集，并把各缓冲/TLAS 写入；结果存于 VoxelrtIO。
    bool createDescriptor(std::shared_ptr<VoxelrtIO> &raytracingio);
    void destroy(std::shared_ptr<VoxelrtIO> &raytracingio);

};


#endif //FIELD_DESCRIPTOR_H
