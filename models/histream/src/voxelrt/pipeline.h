// ==========================================================================
// 文件：pipeline.h
// 功能：VoxelRT（体元辐射）模式 Pipeline 类的声明：计算管线的创建与销毁接口。
// 主要模块：
//   1. class Pipeline：createPipeline / destroy / createShaderBindingTable
//   2. 其余 createRTPipeline、createAeroPipeline、createBioPipeline、createETPipeline、
//      createEBPipeline 仅为沿用其他模式的声明，pipeline.cpp 中的实现已被注释，不可调用
// 数据流：所有接口都通过 VoxelrtIO（voxelrtio.h）读写 Vulkan 设备、描述符布局与管线句柄。
// 与其他文件的关系：实现见 pipeline.cpp；由 voxelrt.cpp 的 Voxelrt::create/destroy 调用。
// 注意：这里 include 了 nvvk 的 SBT、着色器管理等头文件，VoxelRT 的计算管线并未用到其中大部分。
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_PIPELINE_H
#define FIELD_PIPELINE_H

#include <nvvk/pipeline_vk.hpp>
#include <nvvk/sbtwrapper_vk.hpp>
#include <nvvk/shadermodulemanager_vk.hpp>
#include <nvvk/shaders_vk.hpp>
#include <nvh/shaderfilemanager.hpp>
#include <nvh/fileoperations.hpp>
#include <nvh/alignment.hpp>
#include <future>

#include "voxelrtio.h"


// 模块：VoxelRT 的计算管线管理器（无状态，所有数据存放在 VoxelrtIO 中）。
// 实际有实现的只有 createPipeline、createShaderBindingTable（空）和 destroy。
class Pipeline {
public:
    Pipeline(){};

    bool createRTPipeline(std::shared_ptr<VoxelrtIO> &modelio);
    bool createAeroPipeline(std::shared_ptr<VoxelrtIO> &modelio);
    bool createBioPipeline(std::shared_ptr<VoxelrtIO> &modelio);
    bool createETPipeline(std::shared_ptr<VoxelrtIO> &modelio);
    bool createEBPipeline(std::shared_ptr<VoxelrtIO> &modelio);
    bool createPipeline(std::shared_ptr<VoxelrtIO> &modelio);
    void createShaderBindingTable(std::shared_ptr<VoxelrtIO> &modelio);
    void destroy(std::shared_ptr<VoxelrtIO> &modelio);

};


#endif //FIELD_PIPELINE_H
