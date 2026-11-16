#pragma once
#include "animation/scene/mesh_scene.h"
#include <vulkan/vulkan.h>
#include <memory>
class AppSetting;
namespace animation {
// Reuses HiStream's nvvk allocator/build service. Geometry is baked once into
// world coordinates; one immutable BLAS/TLAS is reused for every static view.
class HistreamMeshAdapter {
public:
    HistreamMeshAdapter(AppSetting& app,const MeshScene& scene);
    ~HistreamMeshAdapter();
    VkAccelerationStructureKHR tlas() const;
    VkDescriptorBufferInfo triangles() const;
    VkDescriptorBufferInfo materials() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
// Per-sample moving geometry (target/fragments) with a fixed triangle count. All GPU resources
// (allocator, buffers, BLAS/TLAS, scratch, staging, command buffer, fence) persist; update()
// uploads the new triangles and rebuilds BLAS and TLAS in place with mode BUILD and the same
// flags as HistreamMeshAdapter (no refit), then waits on one fence. No allocation, staging
// release or vkDeviceWaitIdle per sample.
class DynamicMeshAdapter {
public:
    DynamicMeshAdapter(AppSetting& app,const MeshScene& scene);
    ~DynamicMeshAdapter();
    size_t triangleCount()const;
    // Returns false (and changes nothing) when the triangle count differs from construction.
    bool update(const MeshScene& scene);
    VkAccelerationStructureKHR tlas() const;
    VkDescriptorBufferInfo triangles() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
// Instanced moving geometry (opt-in sequence.instancedGeometry): one BLAS per rigid part, built once
// from the part's vertices in the construction sample; update() only rewrites the instance transforms
// (current pose x inverse construction pose), rebuilds the small TLAS and uploads the world-space
// triangle records the shaders read. instanceCustomIndex = first triangle of the part, so the
// *_instanced shader variants index triangles[] with customIndex + primitiveIndex.
class InstancedMeshAdapter {
public:
    // parts must cover scene.triangles exactly, in order.
    InstancedMeshAdapter(AppSetting& app,const MeshScene& scene,const std::vector<MeshPart>& parts);
    ~InstancedMeshAdapter();
    // Returns false (and changes nothing) when the triangle count or the part layout differs.
    bool update(const MeshScene& scene,const std::vector<MeshPart>& parts);
    VkAccelerationStructureKHR tlas() const;
    VkDescriptorBufferInfo triangles() const;
    size_t partCount() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
}
