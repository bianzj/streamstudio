#pragma once
#include <filesystem>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include <vulkan/vulkan_core.h>

class AppSetting;
struct SceneTransportOptions {
    float impactTime=3, windX=.8f, windY=.15f;
    // 0 heated plume, 1 ambient equilibrium, 2 passive tracer fixture.
    int scenario=0;
    bool detailed=false;
    VkBuffer meshMask=VK_NULL_HANDLE;
    VkBuffer fireSources=VK_NULL_HANDLE;
    std::array<float,3> origin{-32,-40,0},contact{0,-8.15f,12};
    bool flightSmoke=false;
    std::array<float,3> flightStart{};
    bool explicitFlightEnd=false;std::array<float,3> flightEnd{};float outwardAzimuth=0; // rad; horizontal outward effect direction (sin a,-cos a); 0 = -Y facade
    // impact-v1 heat/dust source (binding 4 of transport_impact.comp); null for the other backends.
    VkBuffer impactSource=VK_NULL_HANDLE;
};

// Coarse low-speed transport with supplied heat/tracer sources. No blast solver.
class SceneTransport {
public:
    SceneTransport(AppSetting& app, const std::filesystem::path& shader,
                   const std::filesystem::path& snapshot,
                   const SceneTransportOptions& options);
    ~SceneTransport();
    void advanceTo(double presentationTime);
    void setObstacleMask(VkBuffer);
    VkBuffer buffer() const;
    VkDeviceSize byteSize() const;
    // Copy of the current cells (GPU copy + one host memcpy). summarize() is the CPU scan and may run on
    // another thread; statistics() == summarize(*capture()).
    struct Snapshot;
    std::shared_ptr<const Snapshot> capture();
    static nlohmann::json summarize(const Snapshot&);
    nlohmann::json statistics();
    // Exact solver state (step counter and cells) for checkpoints.
    std::vector<uint8_t> saveState();
    void loadState(const std::vector<uint8_t>&);
    int step() const;
    static nlohmann::json description();
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
