#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace facetvk
{

struct EdgeHashOverflow : std::runtime_error
{
    EdgeHashOverflow() : std::runtime_error("Visibility edge hash overflow at the GPU storage-buffer limit")
    {
    }
};

// Geometry is uploaded as a non-indexed triangle list. The three vertices of
// one facet must be consecutive and carry the same unit normal.
struct Vertex
{
    float position[3]{};
    float normal[3]{};
};

// Direction points from the scene towards the virtual orthographic camera.
// Visibility directions must have equal solid-angle weight.
struct Direction
{
    float x{};
    float y{};
    float z{1.0f};
};

// One entry per oriented side: side 2*f is the geometric-normal side and
// side 2*f+1 is the opposite side.
struct SurfaceOptics
{
    float reflectance{};
    float transmittance{};
    float emission{};
    float directIrradiance{};
};

struct Config
{
    uint32_t rasterWidth{512};
    uint32_t rasterHeight{512};
    uint32_t maxFragmentsPerPixel{32};
    uint32_t edgeHashCapacity{0}; // 0 selects a power-of-two capacity automatically.
    bool enableValidation{false};
    uint32_t gpuIndex{0};
    uint32_t periodicNeighborCount{0};
    float periodicSizeX{1.0f};
    float periodicSizeZ{1.0f};
};

struct GraphDiagnostics
{
    uint32_t facetCount{};
    uint32_t surfaceCount{};
    uint32_t directedEdgeCount{};
    uint32_t fragmentOverflow{};
    uint32_t hashOverflow{};
    float maxClosureError{};
};

struct SolveResult
{
    std::vector<float> radiosity;
    uint32_t iterations{};
    float maxDelta{};
    // Irradiance computed from the same optics/state, without a CPU CSR walk.
    std::vector<float> incident;
};

struct EnergyPush
{
    uint32_t facets{}, objects{}, method{}, phase{};
    float air{}, rho{}, pressure{}, vapor{}, dt{}, co2{}, relaxation{}, wind{};
};
struct EnergySnapshot
{
    std::vector<float> temperatures, fluxes;
    std::vector<SurfaceOptics> optics;
    SolveResult radiation;
};

struct SpectralPush
{
    uint32_t phase{}, band{}, facets{}, materials{};
    float sunX{}, sunY{}, sunZ{}, strength{};
    float beam{}, scale{1.0f};
    uint32_t thermal{}, ping{}, bands{}, reserved{};
};
struct SpectralAudit
{
    std::vector<SurfaceOptics> optics;
    std::vector<std::array<float, 2>> emission;
    std::vector<float> raw, output, states;
};

struct DeviceCandidate
{
    VkPhysicalDevice device{VK_NULL_HANDLE};
    uint32_t queueFamily{};
    uint32_t score{};
};

// GPUs usable by the Facet Vulkan backend, in Config::gpuIndex order.
std::vector<DeviceCandidate> rankDevices(VkInstance instance);

// Raster-only Vulkan radiosity model:
// 1. Equal-solid-angle directions rasterize all facets into an A-buffer.
// 2. A compute pass depth-sorts each pixel and records adjacent oriented sides.
// 3. The visibility graph is compacted once to CSR.
// 4. Two-sided reflection/transmission is solved by GPU Jacobi iterations.
// No acceleration structure, ray query, or ray-tracing pipeline is created.
class FacetrtVulkan
{
  public:
    FacetrtVulkan() = default;
    ~FacetrtVulkan();

    FacetrtVulkan(const FacetrtVulkan &) = delete;
    FacetrtVulkan &operator=(const FacetrtVulkan &) = delete;

    void initialize(const Config &config, const std::string &shaderDirectory);
    void setGeometry(const std::vector<Vertex> &vertices);
    // Grows the edge hash table and rebuilds when it overflows.
    GraphDiagnostics buildVisibilityGraph(const std::vector<Direction> &directions);

    SolveResult solve(const std::vector<SurfaceOptics> &optics, float skyRadiosity, uint32_t iterations = 64,
                      float relaxation = 1.0f);
    SolveResult solveAccelerated(const std::vector<SurfaceOptics> &optics, float skyRadiosity,
                                 const std::vector<float> *initialRadiosity = nullptr, uint32_t maximumIterations = 64,
                                 uint32_t batchIterations = 8, float tolerance = 1.0e-4f, float relaxation = 1.0f);
    std::vector<float> incidentIrradiance(const SolveResult &solution, const std::vector<SurfaceOptics> &optics,
                                          float skyRadiosity) const;
    std::vector<float> computeSunlitFraction(const Direction &sunDirection);
    // Material spectra and facet geometry are uploaded once for all bands.
    // Packed facets: 12 floats; packed material/band records: four floats.
    void beginSpectral(const std::vector<float> &facets, const std::vector<float> &materialBands,
                       uint32_t bands, uint32_t materials, const Direction &sun, float backgroundStrength,
                       float beamIrradiance);
    SolveResult solveSpectralBand(uint32_t band, bool thermal, float sky, float scale, bool accelerated,
                                  bool warmStart, bool diffuse = false);
    std::vector<float> readSpectralOutput();
    SpectralAudit readSpectralAudit(uint32_t band);
    // Audit-only restore after the retained reference solver used m_optics.
    void restoreSpectralOptics(const std::vector<SurfaceOptics> &optics);
    // Pack layout is shared with energy.comp: 32 floats/facet, 12/state/side.
    // All iterative temperatures and fluxes stay device-resident between calls.
    bool supportsGpuEnergy() const noexcept
    {
        return m_energyPipeline != VK_NULL_HANDLE;
    }
    bool supportsGpuIncident() const noexcept
    {
        return m_incidentPipeline != VK_NULL_HANDLE;
    }
    void beginEnergyNode(const EnergyPush &push, const std::vector<float> &parameters, const std::vector<float> &inputs,
                         const std::vector<float> &temperatures);
    void updateEnergyInputs(const std::vector<float> &inputs);
    std::array<float, 5> evaluateEnergy(float skyRadiosity);
    float updateEnergyTemperature();
    EnergySnapshot readEnergyState(bool includeRadiation = false);

    void destroy();

    [[nodiscard]] bool initialized() const noexcept
    {
        return m_device != VK_NULL_HANDLE;
    }
    [[nodiscard]] uint32_t facetCount() const noexcept
    {
        return m_facetCount;
    }
    [[nodiscard]] uint32_t surfaceCount() const noexcept
    {
        return m_facetCount * 2U;
    }

  private:
    struct Buffer
    {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{};
    };

    struct CsrEdge
    {
        uint32_t neighbor{};
        uint32_t count{};
    };

    struct VisibilityPush
    {
        float viewProjection[16]{};
        float viewDirection[4]{};
        uint32_t raster[4]{}; // width, height, max layers, hash capacity
        float periodic[4]{};  // domain X/Z size, neighbour count, reserved
    };

    struct SolvePush
    {
        uint32_t facetCount{};
        uint32_t ping{};
        float skyRadiosity{};
        float relaxation{1.0f};
    };

    void createContext();
    void createCommandPool();
    void createVisibilityResources();
    void createVisibilityDescriptors();
    void createVisibilityPipelines();
    void createSolveResources(uint32_t directedEdgeCount);
    void createSolveDescriptors();
    void createSolvePipeline();
    void createEnergyResources();
    void destroyEnergyResources();
    uint32_t recordSolve(VkCommandBuffer commandBuffer, float skyRadiosity, uint32_t iterations, float relaxation);
    uint32_t recordIterations(VkCommandBuffer commandBuffer, uint32_t ping, float skyRadiosity,
                              uint32_t iterations, float relaxation, bool indirect = false);
    void recordIncident(VkCommandBuffer commandBuffer, uint32_t ping, float skyRadiosity);
    void recordEnergy(VkCommandBuffer commandBuffer, uint32_t phase);
    void computeBarrier(VkCommandBuffer commandBuffer) const;
    void recordSpectral(VkCommandBuffer commandBuffer, const SpectralPush &push);
    void destroySpectralResources();
    std::array<float, 5> readEnergyStatistics();

    void destroyVisibilityResources();
    void destroySolveResources();
    void destroyBuffer(Buffer &buffer) const;

    [[nodiscard]] Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                      VkMemoryPropertyFlags properties) const;
    void uploadBuffer(Buffer &destination, const void *data, VkDeviceSize size) const;
    void downloadBuffer(const Buffer &source, void *data, VkDeviceSize size) const;
    void copyBuffer(VkBuffer source, VkBuffer destination, VkDeviceSize size) const;

    [[nodiscard]] uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    [[nodiscard]] VkCommandBuffer beginCommands() const;
    void endCommands(VkCommandBuffer commandBuffer) const;
    [[nodiscard]] VkShaderModule loadShader(const std::string &fileName) const;
    [[nodiscard]] std::array<float, 16> makeProjection(const Direction &direction) const;
    [[nodiscard]] uint32_t chooseHashCapacity() const;
    [[nodiscard]] uint32_t maximumHashCapacity() const;
    [[nodiscard]] uint32_t nextPowerOfTwo(uint32_t value) const;
    GraphDiagnostics buildVisibilityGraphOnce(const std::vector<Direction> &directions);

    void require(VkResult result, const char *operation) const;
    void validateGeometry(const std::vector<Vertex> &vertices) const;
    void validateOptics(const std::vector<SurfaceOptics> &optics) const;

    Config m_config{};
    std::string m_shaderDirectory;
    std::vector<Vertex> m_geometry;

    VkInstance m_instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    VkDevice m_device{VK_NULL_HANDLE};
    VkQueue m_queue{VK_NULL_HANDLE};
    uint32_t m_queueFamily{};
    VkCommandPool m_commandPool{VK_NULL_HANDLE};

    uint32_t m_facetCount{};
    uint32_t m_hashCapacity{};
    std::array<float, 3> m_boundsMin{};
    std::array<float, 3> m_boundsMax{};

    Buffer m_vertices;
    Buffer m_fragments;
    Buffer m_pixelCounts;
    Buffer m_edgeKeys;
    Buffer m_edgeCounts;
    Buffer m_denominator;
    Buffer m_skyCounts;
    Buffer m_visibilityStats;

    VkDescriptorSetLayout m_visibilitySetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_visibilityDescriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_visibilityDescriptorSet{VK_NULL_HANDLE};
    VkPipelineLayout m_visibilityPipelineLayout{VK_NULL_HANDLE};
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    Buffer m_directTotal;
    Buffer m_directVisible;
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};
    VkPipeline m_rasterPipeline{VK_NULL_HANDLE};
    VkPipeline m_resolvePipeline{VK_NULL_HANDLE};

    std::vector<uint32_t> m_rowOffsetsCpu;
    std::vector<CsrEdge> m_edgesCpu;
    VkDescriptorSet m_directDescriptorSet{VK_NULL_HANDLE};
    VkPipeline m_directResolvePipeline{VK_NULL_HANDLE};
    std::vector<uint32_t> m_denominatorCpu;
    std::vector<uint32_t> m_skyCountsCpu;

    Buffer m_rowOffsets;
    Buffer m_edges;
    Buffer m_optics;
    Buffer m_radiosityA;
    Buffer m_radiosityB;
    Buffer m_residual;

    VkDescriptorSetLayout m_solveSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_solveDescriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_solveDescriptorSet{VK_NULL_HANDLE};
    VkPipelineLayout m_solvePipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_solvePipeline{VK_NULL_HANDLE};
    bool m_shaderFloat64{};
    Buffer m_incident;
    VkPipeline m_incidentPipeline{VK_NULL_HANDLE};
    Buffer m_energyParameters, m_energyInputs, m_energyTemperatures, m_energyFluxes, m_energyStats;
    VkDescriptorSetLayout m_energySetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_energyDescriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_energyDescriptorSet{VK_NULL_HANDLE};
    VkPipelineLayout m_energyPipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_energyPipeline{VK_NULL_HANDLE};
    EnergyPush m_energyPush{};
    Buffer m_spectralFacets, m_spectralMaterials, m_spectralEmission, m_spectralPrevious, m_spectralOutput;
    Buffer m_spectralConvergence;
    Buffer m_spectralDispatch;
    VkDescriptorSetLayout m_spectralSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_spectralPool{VK_NULL_HANDLE};
    VkDescriptorSet m_spectralSet{VK_NULL_HANDLE};
    VkPipelineLayout m_spectralLayout{VK_NULL_HANDLE};
    VkPipeline m_spectralPipeline{VK_NULL_HANDLE};
    SpectralPush m_spectralPush{};
};

} // namespace facetvk
