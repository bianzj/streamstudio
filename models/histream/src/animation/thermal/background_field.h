#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace animation {
struct MeshScene;
class BackgroundField {
public:
    BackgroundField(MeshScene& scene,const std::filesystem::path& sceneDirectory);
    nlohmann::json snapshot(const std::filesystem::path& provenance) const;
private:
    struct File {std::filesystem::path path;std::string sha256,role;};
    nlohmann::json metadata;
    std::vector<File> sources;
    File records;
    std::string timestamp;
};
}
