#pragma once
#include <filesystem>
namespace animation {
int runMeshSequence(const std::filesystem::path&,const std::filesystem::path&,const std::filesystem::path& shaderDirectory,const std::filesystem::path& executable);
}
