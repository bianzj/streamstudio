#pragma once
#include <filesystem>
namespace animation {
int runMeshScene(const std::filesystem::path& input,const std::filesystem::path& output,
                 const std::filesystem::path& shader,const std::filesystem::path& executable);
}
