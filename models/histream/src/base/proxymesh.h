// ==========================================================================
// 文件：proxymesh.h
// 功能：体元模式（VoxelRT / VoxelEB）代理网格的顶点格式与分块上传（control.voxelProxyGeometry）。
//   体元着色器（shade_state_voxel.glsl）只读顶点位置（法线由叉积得到），构建代理面时也只填位置，
//   但旧格式每个顶点是 44 字节的 VertexAttribute：每个代理面 6 个顶点 = 264 B（+ 6 个索引 24 B）。
//   一个网格的顶点缓冲超过 maxMemoryAllocationSize（NVIDIA 0xffe00000 = 4,292,870,144 B，约 16.26 M 个面）
//   时，旧版在 nvvk 分配器里崩溃（访问冲突）。本文件提供：
//     full            ：旧格式（逐位复现旧结果；上传仍用 nvvk 的一次性 staging，但先检查大小）
//     position        ：12 B 仅位置顶点 + 索引缓冲
//     positionNoIndex ：12 B 仅位置的三角形列表顶点，无索引缓冲（BLAS indexType NONE，primitiveCount = 顶点数/3）
//   position* 两种分块上传：设备本地缓冲一次创建，之后经固定大小（48 MiB）的 host-visible staging 逐块拷贝，
//   不再需要与缓冲等大的 staging，也不需要整块主机临时数组。三角形与顶点顺序保持不变，重心坐标不变。
// ==========================================================================
#pragma once

#include "structs.h"
#include "voxelflags.h"
#include "voxellimits.h"

#include <nvvk/commands_vk.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>

namespace proxymesh {

constexpr VkDeviceSize kChunkBytes = 48ull << 20;  // multiple of 12 (vertex) and 4 (index)

inline const char* modeName(int mode)
{
    return mode == voxelflags::kProxyPositionNoIndex ? "positionNoIndex" : mode == voxelflags::kProxyPosition ? "position" : "full";
}

// Check the buffers of a legacy-format ("full") mesh before nvvk allocates them: the vertex buffer, the index buffer and
// their same-size staging buffers (createBuffer(cmd, vector, usage)) must each fit one allocation.
inline void requireFullMeshFits(VkPhysicalDevice physicalDevice, const char* model, size_t meshIndex, const PrimMesh& mesh)
{
    const std::string name = std::string("mesh ") + std::to_string(meshIndex);
    const char* hint = "Set control.voxelProxyGeometry to \"position\" or \"positionNoIndex\" (12 B per vertex instead of 44 B), "
                       "or reduce the scene size / use a larger voxel edge.";
    const uint64_t vertexBytes = uint64_t(mesh.vertices.size()) * sizeof(VertexAttribute);
    const uint64_t indexBytes = uint64_t(mesh.indices.size()) * sizeof(uint32_t);
    voxellimits::requireAllocation(physicalDevice, model, (name + " proxy vertex").c_str(), vertexBytes, mesh.vertices.size(), "vertices", hint);
    voxellimits::requireAllocation(physicalDevice, model, (name + " proxy vertex staging").c_str(), vertexBytes, mesh.vertices.size(), "vertices", hint);
    voxellimits::requireAllocation(physicalDevice, model, (name + " proxy index").c_str(), indexBytes, mesh.indices.size(), "indices", hint);
}

// Create a device-local buffer of `totalBytes` and fill it in kChunkBytes steps: `fill(destination, offset, bytes)` writes
// the bytes [offset, offset + bytes) of the buffer into the mapped staging memory.
template <class Allocator>
nvvk::Buffer uploadChunked(Allocator& allocator, VkDevice device, uint32_t queueFamily, VkQueue queue, VkDeviceSize totalBytes,
                           VkBufferUsageFlags usage, const std::function<void(char*, VkDeviceSize, VkDeviceSize)>& fill)
{
    nvvk::Buffer destination = allocator.createBuffer(
        std::max<VkDeviceSize>(totalBytes, 4), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!destination.buffer) throw std::runtime_error("Cannot allocate a voxel proxy mesh buffer of " + voxellimits::grouped(totalBytes) + " bytes");
    if (totalBytes == 0) return destination;
    const VkDeviceSize chunk = std::min(kChunkBytes, totalBytes);
    nvvk::Buffer staging = allocator.createBuffer(chunk, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    char* mapped = static_cast<char*>(allocator.map(staging));
    nvvk::CommandPool pool(device, queueFamily, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, queue);
    for (VkDeviceSize offset = 0; offset < totalBytes; offset += chunk) {
        const VkDeviceSize bytes = std::min(chunk, totalBytes - offset);
        fill(mapped, offset, bytes);
        VkCommandBuffer command = pool.createCommandBuffer();
        const VkBufferCopy region{0, offset, bytes};
        vkCmdCopyBuffer(command, staging.buffer, destination.buffer, 1, &region);
        pool.submitAndWait(command);
    }
    allocator.unmap(staging);
    allocator.destroy(staging);
    return destination;
}

// Position-only upload of one mesh (mode 1 or 2). The returned MeshBuffer carries the stride and index flag that
// AccelStruct::objectToVkGeometryKHR uses; for mode 2 no index buffer exists and nbIndices == nbVertices (triangle list).
template <class Allocator>
MeshBuffer uploadPositionMesh(Allocator& allocator, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamily,
                              VkQueue queue, const char* model, size_t meshIndex, const PrimMesh& mesh, int mode)
{
    const bool indexed = mode != voxelflags::kProxyPositionNoIndex;
    const uint64_t vertexCount = indexed ? mesh.vertices.size() : mesh.indices.size();
    const uint64_t vertexBytes = vertexCount * sizeof(float) * 3;
    const uint64_t indexBytes = indexed ? uint64_t(mesh.indices.size()) * sizeof(uint32_t) : 0;
    const std::string name = std::string("mesh ") + std::to_string(meshIndex);
    const char* hint = "Reduce the scene size or use a larger voxel edge (position-only proxy vertices are already 12 B).";
    voxellimits::requireAllocation(physicalDevice, model, (name + " proxy vertex").c_str(), vertexBytes, vertexCount, "vertices", hint);
    voxellimits::requireAllocation(physicalDevice, model, (name + " proxy index").c_str(), indexBytes, mesh.indices.size(), "indices", hint);

    MeshBuffer buffer;
    buffer.nbVertices = static_cast<uint32_t>(vertexCount);
    buffer.nbIndices = static_cast<uint32_t>(mesh.indices.size());
    buffer.vertexStride = sizeof(float) * 3;
    buffer.indexed = indexed;
    const VkBufferUsageFlags vertexUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    buffer.vertexBuffer = uploadChunked(allocator, device, queueFamily, queue, vertexBytes, vertexUsage,
        [&](char* destination, VkDeviceSize offset, VkDeviceSize bytes) {
            float* out = reinterpret_cast<float*>(destination);
            const size_t first = static_cast<size_t>(offset / (sizeof(float) * 3));
            const size_t count = static_cast<size_t>(bytes / (sizeof(float) * 3));
            for (size_t i = 0; i < count; ++i) {
                const glm::vec3& position = indexed ? mesh.vertices[first + i].pos
                                                    : mesh.vertices[mesh.indices[first + i]].pos;
                out[3 * i] = position.x;
                out[3 * i + 1] = position.y;
                out[3 * i + 2] = position.z;
            }
        });
    if (indexed) {
        const VkBufferUsageFlags indexUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        buffer.indexBuffer = uploadChunked(allocator, device, queueFamily, queue, indexBytes, indexUsage,
            [&](char* destination, VkDeviceSize offset, VkDeviceSize bytes) {
                std::memcpy(destination, reinterpret_cast<const char*>(mesh.indices.data()) + offset, static_cast<size_t>(bytes));
            });
    }
    return buffer;
}

}  // namespace proxymesh
