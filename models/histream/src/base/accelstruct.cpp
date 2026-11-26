// ==========================================================================
// 文件：accelstruct.cpp
// 功能：用 VK_KHR_acceleration_structure 为场景构建 BLAS（每个网格一个）与 TLAS（每个实例一个）。
// 主要模块/步骤：
//   1. objectToVkGeometryKHR：取顶点/索引缓冲的设备地址，填写三角形几何与构建范围
//   2. createAccelStruct 步骤 1：对 MeshIO 中每个网格构建 BLAS（快速追踪 + 允许压缩）
//   3. createAccelStruct 步骤 2：遍历 InstanceIO 实例，写入变换、自定义索引并构建 TLAS
// 数据流：MeshIO::m_bufferMeshes + InstanceIO::instances → m_rtBuilder → TLAS（getTlas 取出）
// 与其他文件：声明在 accelstruct.h；各模式（RayTracing/VoxelRT/VoxelEB/Facet*）共用。
// 约定：矩阵为 object2worldMatrix（物体到世界）；所有实例强制 opaque，mask=0xFF，
//       共用同一个 hit group（SBT 记录偏移为 0）。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#include "accelstruct.h"
#include "voxellimits.h"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

// 函数：objectToVkGeometryKHR
// 作用：把一个网格的设备端顶点/索引缓冲描述成 BLAS 输入（单个三角形几何 + 构建范围）。
// 输入：device、model（含 vertexBuffer/indexBuffer、nbVertices、nbIndices）；输出：BlasInput。
// 说明：顶点只读位置（R32G32B32_SFLOAT），按局部 Vertex 结构的步长跳过其余属性；索引为 uint32。
nvvk::RaytracingBuilderKHR::BlasInput AccelStruct::objectToVkGeometryKHR(const VkDevice device,
                                                                         const MeshBuffer& model)
{
    // Building part
    // DEFERRED[中] Bug: 局部 Vertex 结构（pos/att/color/texCoord = 44 B）用于计算 vertexStride，若与 MeshBuffer 实际上传的顶点结构布局不一致，BLAS 会读错顶点位置而不报错；理由：需与上传侧（raytracing/voxelrt/voxeleb buffer.cpp）一起改用同一顶点类型，属结构性改动，明确禁止本轮修改。
    struct Vertex {
        glm::vec3 pos;
        glm::vec3 att;
        glm::vec3 color;
        glm::vec2 texCoord;
    };

    // Building part
    // 步骤 1：取顶点/索引缓冲的设备地址（需要缓冲创建时带 SHADER_DEVICE_ADDRESS 用法）。
    VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    info.buffer = model.vertexBuffer.buffer;
    VkDeviceAddress vertexAddress = vkGetBufferDeviceAddress(device, &info);
    info.buffer = model.indexBuffer.buffer;
    // Non-indexed voxel proxy meshes (MeshBuffer::indexed == false) have no index buffer.
    VkDeviceAddress indexAddress = model.indexed ? vkGetBufferDeviceAddress(device, &info) : 0;

    VkAccelerationStructureGeometryTrianglesDataKHR triangles{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR };
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = vertexAddress;
    // The stride is the mesh's own (44 B VertexAttribute unless a voxel mode packs position-only vertices).
    triangles.vertexStride = model.vertexStride;
    if (model.indexed) {
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = indexAddress;
    } else {
        triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        triangles.indexData.deviceAddress = 0;
    }
    // DEFERRED[低] Bug: Vulkan 规范中 maxVertex 是“最大可寻址顶点索引”，应为 nbVertices-1；此处多 1 通常无害但会让驱动按越界范围预取；理由：改 maxVertex 与空网格跳过会改变 BLAS 构建范围/几何语义，且不属本轮允许的缺陷类别，未改。
    triangles.maxVertex = model.nbVertices;

    // Setting up the build info of the acceleration
    // 步骤 2：组装几何描述；NO_DUPLICATE_ANY_HIT 保证同一三角形不会重复触发 any-hit。
    VkAccelerationStructureGeometryKHR asGeom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    asGeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    asGeom.flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;  // For AnyHit
    asGeom.geometry.triangles = triangles;

    // 步骤 3：构建范围——整个索引缓冲，每 3 个索引一个三角形。
    VkAccelerationStructureBuildRangeInfoKHR offset{};
    offset.firstVertex = 0;
    offset.primitiveCount = (model.indexed ? model.nbIndices : model.nbVertices) / 3;
    offset.primitiveOffset = 0;
    offset.transformOffset = 0;

    nvvk::RaytracingBuilderKHR::BlasInput input;
    input.asGeometry.emplace_back(asGeom);
    input.asBuildOffsetInfo.emplace_back(offset);

    return input;
}


// 函数：createAccelStruct
// 作用：构建场景的完整加速结构（先全部 BLAS，再 TLAS）。
// 输入：device、meshio（网格缓冲列表）、instanceio（实例变换与 meshId）；输出：成功返回 true。
// 副作用：结果保存在成员 m_rtBuilder，TLAS 通过 getTlas() 获取。
// 异常：实例的 meshId 越界（且不为 -1）时抛 std::runtime_error。
bool AccelStruct::createAccelStruct(VkDevice device, std::shared_ptr<MeshIO> &meshio,
                                    std::shared_ptr<InstanceIO> &instanceio) {


//    auto & meshio = raytracingio->m_meshio;
//    auto & instanceio = raytracingio->m_instanceio;
//    auto & device = raytracingio->m_device;

    // Bottom level acceleration structure.
    // 步骤 1：每个网格一个 BLAS，BLAS 下标与 meshId 一致（TLAS 阶段按 meshId 取地址）。
    std::vector<nvvk::RaytracingBuilderKHR::BlasInput> allBlas;
    allBlas.reserve(meshio->m_bufferMeshes.size());
    for (const auto& model : meshio->m_bufferMeshes)
    {
        auto blas = objectToVkGeometryKHR(device, model);
        allBlas.emplace_back(blas);
    }

    // Voxel modes: size every BLAS up front (the same query nvvk's buildBlas makes) so that a mesh whose acceleration
    // structure or scratch buffer cannot be allocated fails with a clear message instead of crashing the driver.
    if (m_physicalDevice) {
        const bool printStats = std::getenv("HISTREAM_VOXEL_PROXY_STATS") != nullptr;
        for (std::size_t i = 0; i < allBlas.size(); ++i) {
            VkAccelerationStructureBuildGeometryInfoKHR buildInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            buildInfo.flags = allBlas[i].flags | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                                                | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
            buildInfo.geometryCount = static_cast<uint32_t>(allBlas[i].asGeometry.size());
            buildInfo.pGeometries = allBlas[i].asGeometry.data();
            const uint32_t primitives = allBlas[i].asBuildOffsetInfo.empty() ? 0 : allBlas[i].asBuildOffsetInfo[0].primitiveCount;
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                                    &primitives, &sizes);
            if (printStats)
                std::cout << "PROXY_BLAS\tmodel=" << m_modelName << "\tmesh=" << i << "\ttriangles=" << primitives
                          << "\tvertexBytes=" << (uint64_t(meshio->m_bufferMeshes[i].nbVertices) * meshio->m_bufferMeshes[i].vertexStride)
                          << "\tindexed=" << int(meshio->m_bufferMeshes[i].indexed)
                          << "\tasBytes=" << sizes.accelerationStructureSize << "\tscratchBytes=" << sizes.buildScratchSize
                          << std::endl;
            const std::string name = std::string("mesh ") + std::to_string(i);
            const char* hint = "Reduce the scene size or use a larger voxel edge (control.voxelProxyGeometry cannot shrink the acceleration structure).";
            voxellimits::requireAllocation(m_physicalDevice, m_modelName,
                (name + " acceleration structure").c_str(), sizes.accelerationStructureSize, primitives, "triangles", hint);
            voxellimits::requireAllocation(m_physicalDevice, m_modelName,
                (name + " acceleration-structure build scratch").c_str(), sizes.buildScratchSize, primitives, "triangles", hint);
        }
    }

    // 偏向追踪速度而非构建速度；允许压缩以降低显存占用。
    m_rtBuilder.buildBlas(allBlas, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                                   | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);

    // Top level acceleration structure.
    // 步骤 2：每个实例一个 TLAS 条目；instanceCustomIndex 取实例下标 i（跳过的实例也占用下标）。
    std::vector<VkAccelerationStructureInstanceKHR> allTlas;
    allTlas.reserve(instanceio->instances.size());
    for (int i = 0; i < static_cast<int>(instanceio->instances.size()); i++)
    {
        const int meshId = instanceio->instances[i].meshId;
        // meshId -1: an instance that only carries ids (voxel links) and no
        // geometry of its own, e.g. a rotated OBJ placement whose proxies are
        // in a world-aligned mesh (scene.cpp). instanceCustomIndex stays i.
        if (meshId == -1) continue;
        // 所有几何按不透明处理，跳过 any-hit，加快遍历。
        VkGeometryInstanceFlagsKHR flags{};
        flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        VkAccelerationStructureInstanceKHR rayInst{};
        rayInst.transform = nvvk::toTransformMatrixKHR(instanceio->instances[i].object2worldMatrix);  // Position of the instance
        rayInst.instanceCustomIndex = i;                       // gl_InstanceCustomIndexEXT
        if (meshId < 0 || static_cast<std::size_t>(meshId) >= meshio->m_bufferMeshes.size()) {
            throw std::runtime_error("Instance " + std::to_string(i) + " references invalid meshId "
                + std::to_string(meshId) + " (mesh count "
                + std::to_string(meshio->m_bufferMeshes.size()) + ")");
        }
        // 引用对应网格的 BLAS 设备地址。
        rayInst.accelerationStructureReference = m_rtBuilder.getBlasDeviceAddress(meshId);
        rayInst.flags = flags;
        rayInst.instanceShaderBindingTableRecordOffset = 0;  // We will use the same hit group for all objects
        rayInst.mask = 0xFF;
        allTlas.emplace_back(rayInst);
    }
    // 步骤 3：构建 TLAS。
    m_rtBuilder.buildTlas(allTlas, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);

    return true;

}




