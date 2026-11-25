// ==========================================================================
// 文件：pipeline.h
// 功能：Pipeline 类声明 —— 创建/销毁 VoxelEB 的 Vulkan 计算管线（实现见 pipeline.cpp）
// 主要模块：
//   1. createPipeline：当前实际使用的入口，一次建好 VoxelEBStage 全部阶段的管线
//   2. createRT/Aero/Bio/ET/EBPipeline、createShaderBindingTable：旧版分管线接口，
//      pipeline.cpp 中实现已被注释或为空，保留声明仅为兼容
//   3. destroy：销毁所有阶段管线与管线布局
// 与其他文件的关系：依赖 voxelebio.h（VoxelebIO 持有管线、布局和设备句柄）
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

#include "voxelebio.h"


// 模块：VoxelEB 管线工厂。无自身状态，所有输入输出都经由 VoxelebIO 传递。
class Pipeline {
public:
    Pipeline(){};

    // 以下 5 个 create*Pipeline 为旧版分管线接口：pipeline.cpp 中的定义已整体注释掉，
    // 当前仅 createPipeline 被实现，且 voxeleb.cpp 只调用它；这些旧接口在本目录内无调用方。
    bool createRTPipeline(std::shared_ptr<VoxelebIO> &modelio);
    bool createAeroPipeline(std::shared_ptr<VoxelebIO> &modelio);
    bool createBioPipeline(std::shared_ptr<VoxelebIO> &modelio);
    bool createETPipeline(std::shared_ptr<VoxelebIO> &modelio);
    bool createEBPipeline(std::shared_ptr<VoxelebIO> &modelio);

    // 作用：创建 VoxelEB 全部阶段的计算管线与共用的管线布局；成功返回 true，失败返回 false。
    bool createPipeline(std::shared_ptr<VoxelebIO> &modelio);

    // SBT 占位接口（计算管线不需要，实现为空）；destroy：销毁管线与管线布局并复位句柄。
    void createShaderBindingTable(std::shared_ptr<VoxelebIO> &modelio);
    void destroy(std::shared_ptr<VoxelebIO> &modelio);

};


#endif //FIELD_PIPELINE_H
