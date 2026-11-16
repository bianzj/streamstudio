#pragma once
#include "animation/render/compute_pass.h"
#include "animation/scene/mesh_scene.h"
namespace animation {
struct GroundThermalOptions{nlohmann::json input;std::filesystem::path path;std::string digest;static GroundThermalOptions load(const nlohmann::json&,const std::filesystem::path&);};
// Local paired surface EB delta; no change to immutable FacetEB baseline files.
class GroundThermal {
public:
    GroundThermal(AppSetting&,const std::filesystem::path& shader,const GroundThermalOptions&,const MeshScene& base,const MeshScene& background,VkAccelerationStructureKHR baseAS,VkDescriptorBufferInfo baseTriangles,VkAccelerationStructureKHR backgroundAS,VkDescriptorBufferInfo backgroundTriangles,const std::filesystem::path& output);
    ~GroundThermal();
    VkDescriptorBufferInfo field()const;VkDescriptorBufferInfo parameters()const;nlohmann::json description()const;
private:struct Impl;std::unique_ptr<Impl> m;
};
}
