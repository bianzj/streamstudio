#pragma once
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <array>

namespace animation {
struct FireSource {glm::vec4 positionRadius,timing,flame;};
struct FireBlock {glm::ivec4 count{0};std::array<FireSource,8> sources{};};
static_assert(sizeof(FireSource)==48&&sizeof(FireBlock)==400);
class FireSources {
public:
    FireBlock block;
    FireSources(const nlohmann::json& options,glm::vec3 contact,glm::vec3 domainOrigin);
    nlohmann::json description()const;
};
}
