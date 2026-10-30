//
// Created by admin on 2024/1/24.
//

#include "accelstruct.h"

#include <cstddef>
#include <stdexcept>
#include <string>

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
    VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    info.buffer = model.vertexBuffer.buffer;
    VkDeviceAddress vertexAddress = vkGetBufferDeviceAddress(device, &info);
    info.buffer = model.indexBuffer.buffer;
    VkDeviceAddress indexAddress = vkGetBufferDeviceAddress(device, &info);

    VkAccelerationStructureGeometryTrianglesDataKHR triangles{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR };
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = vertexAddress;
    triangles.vertexStride = sizeof(Vertex);
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = indexAddress;
    // DEFERRED[低] Bug: Vulkan 规范中 maxVertex 是“最大可寻址顶点索引”，应为 nbVertices-1；此处多 1 通常无害但会让驱动按越界范围预取；理由：改 maxVertex 与空网格跳过会改变 BLAS 构建范围/几何语义，且不属本轮允许的缺陷类别，未改。
    triangles.maxVertex = model.nbVertices;

    // Setting up the build info of the acceleration
    VkAccelerationStructureGeometryKHR asGeom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    asGeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    asGeom.flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;  // For AnyHit
    asGeom.geometry.triangles = triangles;

    VkAccelerationStructureBuildRangeInfoKHR offset{};
    offset.firstVertex = 0;
    offset.primitiveCount = model.nbIndices / 3;
    offset.primitiveOffset = 0;
    offset.transformOffset = 0;

    nvvk::RaytracingBuilderKHR::BlasInput input;
    input.asGeometry.emplace_back(asGeom);
    input.asBuildOffsetInfo.emplace_back(offset);

    return input;
}


bool AccelStruct::createAccelStruct(VkDevice device, std::shared_ptr<MeshIO> &meshio,
                                    std::shared_ptr<InstanceIO> &instanceio) {


//    auto & meshio = raytracingio->m_meshio;
//    auto & instanceio = raytracingio->m_instanceio;
//    auto & device = raytracingio->m_device;

    // Bottom level acceleration structure.
    std::vector<nvvk::RaytracingBuilderKHR::BlasInput> allBlas;
    allBlas.reserve(meshio->m_bufferMeshes.size());
    for (const auto& model : meshio->m_bufferMeshes)
    {
        auto blas = objectToVkGeometryKHR(device, model);
        allBlas.emplace_back(blas);
    }

    m_rtBuilder.buildBlas(allBlas, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                                   | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);

    // Top level acceleration structure.
    std::vector<VkAccelerationStructureInstanceKHR> allTlas;
    allTlas.reserve(instanceio->instances.size());
    for (int i = 0; i < static_cast<int>(instanceio->instances.size()); i++)
    {
        VkGeometryInstanceFlagsKHR flags{};
        flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        VkAccelerationStructureInstanceKHR rayInst{};
        rayInst.transform = nvvk::toTransformMatrixKHR(instanceio->instances[i].object2worldMatrix);  // Position of the instance
        rayInst.instanceCustomIndex = i;                       // gl_InstanceCustomIndexEXT
        // FIXED[中] 安全: meshId 未校验是否 < BLAS 数量，越界时 getBlasDeviceAddress 越界访问；已修复：构建实例前校验 meshId ∈ [0, BLAS 数量)，越界抛 std::runtime_error 并报出实例下标、meshId 与网格数量；instanceCustomIndex 的 24 位截断需改场景组织，仍为 DEFERRED。
        const int meshId = instanceio->instances[i].meshId;
        if (meshId < 0 || static_cast<std::size_t>(meshId) >= meshio->m_bufferMeshes.size()) {
            throw std::runtime_error("Instance " + std::to_string(i) + " references invalid meshId "
                + std::to_string(meshId) + " (mesh count "
                + std::to_string(meshio->m_bufferMeshes.size()) + ")");
        }
        rayInst.accelerationStructureReference = m_rtBuilder.getBlasDeviceAddress(meshId);
        rayInst.flags = flags;
        rayInst.instanceShaderBindingTableRecordOffset = 0;  // We will use the same hit group for all objects
        rayInst.mask = 0xFF;
        allTlas.emplace_back(rayInst);
    }
    m_rtBuilder.buildTlas(allTlas, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);

    return true;

}




