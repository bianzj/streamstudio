#pragma once
#include "path_text.h"
#include "material_table.h"
#include <filesystem>
#include <array>
#include <memory>

namespace animation {
class BackgroundField;
// Explicit std430 layout; independent of the legacy VertexAttribute stride.
struct Triangle {
    glm::vec4 a,b,c;
    glm::vec4 na,nb,nc;
    glm::uvec4 ids; // material, instance, asset, reserved
    glm::vec4 thermal; // front/back baseline K, supplied delta K, reserved
};
static_assert(sizeof(Triangle)==128);
// A rigid group of consecutive triangles (instanced geometry): transform maps the group's reference
// coordinates to its current world pose; key identifies the group's (constant) shape.
struct MeshPart {uint64_t key=0;uint32_t first=0,count=0;glm::mat4 transform{1.f};};
struct Camera {
    std::string id;
    glm::vec3 position,target,up;
    float fovY;
};
struct MeshScene {
    // Parsing and provenance hashing must use the same immutable bytes.
    std::string sourceBytes;
    nlohmann::json input,receipt;
    MaterialTable materials;
    std::vector<Triangle> triangles;
    std::vector<Camera> cameras;
    std::vector<std::filesystem::path> assets;
    std::shared_ptr<BackgroundField> background;
    glm::vec3 sunDirection{-.45f,-.65f,1};
    int width=0,height=0,samples=1,gpuIndex=0;
    float wavelength=10,minK=280,maxK=320,skyK=275;
    explicit MeshScene(const std::filesystem::path& source);
    // Internal preset-asset view; the supplied JSON is captured in memory.
    MeshScene(const std::filesystem::path& source,const nlohmann::json& supplied);
};
std::string sha256File(const std::filesystem::path& source);
std::string sha256Bytes(const std::string& bytes);
}
