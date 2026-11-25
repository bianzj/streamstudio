// ==========================================================================
// 文件：accelstruct.h
// 功能：声明 AccelStruct —— 把已上传到 GPU 的网格与实例构建成 Vulkan 光线追踪加速结构（BLAS/TLAS）。
// 主要模块：
//   1. objectToVkGeometryKHR：单个网格缓冲 → BLAS 构建输入（三角形几何）
//   2. createAccelStruct：全部网格建底层加速结构 BLAS，再按实例建顶层加速结构 TLAS
//   3. getTlas：返回 TLAS 句柄，供各模式的 raygen/compute 描述符集绑定
// 数据流：MeshIO（顶点/索引缓冲）+ InstanceIO（变换矩阵、meshId）→ AccelStruct → TLAS
// 与其他文件：实现见 accelstruct.cpp；依赖 meshio.h、instanceio.h 与 nvpro_core 的 RaytracingBuilderKHR。
// 约定：instanceCustomIndex = 实例在 InstanceIO::instances 中的下标（着色器里的 gl_InstanceCustomIndexEXT）。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_ACCELSTRUCTURE_H
#define FIELD_ACCELSTRUCTURE_H

#include <nvh/gltfscene.hpp>
#include "nvvk/resourceallocator_vk.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/raytraceKHR_vk.hpp"
#include "meshio.h"
#include "instanceio.h"



// 类：AccelStruct，持有 nvpro 的 RaytracingBuilderKHR（BLAS 与 TLAS 的显存及构建状态）。
// 生命周期内需保持存活，否则 TLAS 句柄失效。
class AccelStruct {
public:
    AccelStruct(){};

    // 把一个 MeshBuffer（设备端顶点/索引缓冲）转成一个 BLAS 的三角形几何输入。
    nvvk::RaytracingBuilderKHR::BlasInput objectToVkGeometryKHR(const VkDevice device, const MeshBuffer& model);
    // 依次构建所有网格的 BLAS 与全部实例的 TLAS；成功返回 true，实例引用非法 meshId 时抛异常。
    bool createAccelStruct(VkDevice device, std::shared_ptr<MeshIO> &meshio, std::shared_ptr<InstanceIO> &instanceio);
    // 返回顶层加速结构（TLAS）句柄。
    VkAccelerationStructureKHR getTlas() { return m_rtBuilder.getAccelerationStructure(); }

    nvvk::RaytracingBuilderKHR     m_rtBuilder;

};


#endif //FIELD_ACCELSTRUCTURE_H
