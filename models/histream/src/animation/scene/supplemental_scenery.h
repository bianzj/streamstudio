#pragma once
#include "mesh_scene.h"
namespace animation {
class SupplementalScenery {
public:
    SupplementalScenery(const nlohmann::json& reference,const std::filesystem::path& inputDirectory,const MeshScene& base);
    void appendTo(MeshScene& background)const;
    nlohmann::json snapshot(const std::filesystem::path& provenance)const;
    nlohmann::json description()const;
private:
    std::filesystem::path path;
    std::string digest;
    std::unique_ptr<MeshScene> scene;
    std::vector<bool> thinLeaves;
    size_t thinLeafTriangles=0;
};
}
