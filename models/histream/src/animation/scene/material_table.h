#pragma once
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace animation {
struct Material {
    glm::vec4 colorEmissivity;
    glm::vec4 thermal; // temperature K; optional RGB roughness, metallic, IOR
};
static_assert(sizeof(Material)==32);
struct MaterialTable {
    std::vector<std::string> names;
    std::vector<Material> values;
    explicit MaterialTable(const nlohmann::json& input);
    uint32_t index(const std::string& name) const;
};
}
