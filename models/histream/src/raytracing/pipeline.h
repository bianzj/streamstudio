// ==========================================================================
// 文件：pipeline.h
// 功能：RayTracing 模式的 Vulkan 光线追踪管线封装（Pipeline 类）的声明。
// 主要模块：
//   1. Pipeline::createPipeline        创建管线布局、着色器阶段、着色器组与光追管线
//   2. Pipeline::createShaderBindingTable  手工构建 SBT（useSBTWrapper=false 时使用）
//   3. Pipeline::destroy               销毁管线与管线布局
// 数据流：所有状态都放在 RaytracingIO（raytracingio.h）里，本类无成员变量，
//         每个函数通过 shared_ptr<RaytracingIO> 取引用读写其中的 Vulkan 句柄。
// 关系：实现见 pipeline.cpp；调用方为 raytracing.cpp；着色器在 shader/raytracing/ 下。
// ==========================================================================

//
// Created by admin on 2024/1/25.
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

#include "raytracingio.h"



// 类：Pipeline——无状态的管线构建工具，三个方法都操作传入的 RaytracingIO。
class Pipeline {

public:
    Pipeline(){};

    bool createPipeline(std::shared_ptr<RaytracingIO> &raytracingio);
    void createShaderBindingTable(std::shared_ptr<RaytracingIO> &raytracingio);
    void destroy(std::shared_ptr<RaytracingIO> &raytracingio);

};


#endif //FIELD_PIPELINE_H
