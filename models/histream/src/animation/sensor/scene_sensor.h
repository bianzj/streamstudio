#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include <vulkan/vulkan_core.h>

class AppSetting;
struct SceneSensorOptions {
    int width=1280,height=720,binning=2;
    float blurSigma=.7f,noiseKelvin=.09f,fixedPatternKelvin=.05f,responseSeconds=.35f;
    float wavelength=10,minK=260,maxK=500;
    bool bandIntegrated=false,agc=true;
    float agcMinimumSpanKelvin=20;
    bool logarithmicDisplay=false;
};

// Generic illustrative IR display chain, not a calibrated camera model.
class SceneSensor {
public:
    SceneSensor(AppSetting&,VkBuffer output,const std::filesystem::path& shader,
                const std::filesystem::path& snapshot,const SceneSensorOptions&);
    ~SceneSensor();
    VkBuffer radianceBuffer() const;
    VkDeviceSize radianceBytes() const;
    // Caller must finish the radiance dispatch and make its writes HOST_READ-visible.
    nlohmann::json process(int frame,double frameInterval);
    void exportRadiance(const std::filesystem::path&) const;
    nlohmann::json description() const;
    // Temporal AGC state (checkpoints).
    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>&);
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
