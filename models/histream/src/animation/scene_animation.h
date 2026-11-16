#pragma once
#include <filesystem>

// Native GPU illustration mode, separate from calibrated RT/EB simulation.
int runSceneAnimation(const std::filesystem::path& input,
                      const std::filesystem::path& output,
                      const std::filesystem::path& shader,
                      const std::filesystem::path& executable);
