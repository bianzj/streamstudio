// ==========================================================================
// 文件：facetrt_vulkan.h
// 功能：FacetRT / FacetEB 共用的 Vulkan 光栅辐射度后端 FacetrtVulkan 的接口声明与数据结构。
//   以栅格化 A-buffer 估计面元各侧之间的可见性，压缩成 CSR 边表后在 GPU 上求解双面
//   反射/透射的辐射度；同时提供日照可见比例、光谱波段求解和 GPU 能量平衡的入口。
// 主要模块：
//   1. 异常类型：EdgeHashOverflow（边哈希表溢出）、RasterLayerOverflow（A-buffer 层数不足）
//   2. 输入数据：Vertex（几何）、Direction（可见性方向）、SurfaceOptics（逐 side 光学参数）、Config
//   3. 可见性图诊断 GraphDiagnostics 与下边界 GroundBoundary（等效地面）
//   4. 求解结果 SolveResult，GPU 能量平衡与光谱的推送/快照结构
//   5. 设备枚举 rankDevices 与类 FacetrtVulkan（公共接口 + 私有 Vulkan 资源）
// 流程：initialize → setGeometry → buildVisibilityGraph → solve / solveAccelerated，
//   或在其上使用 beginEnergyNode/evaluateEnergy（能量平衡）、beginSpectral/solveSpectralBand（光谱）。
// 与其他文件：实现在 facetrt_vulkan.cpp；着色器在 models/histream/shader；
//   使用者包括 facetrtcore.cpp、facetrtio.h、faceteb/facetebio.h、base/appsetting.cpp。
// 约定：每面元两个 oriented side：side = 2*facet 为几何法向侧，2*facet+1 为反面；
//   Vertex 为非索引三角形列表，同一面元的 3 个顶点相邻且法向相同；
//   Direction 指向虚拟正交相机，各方向须有相同立体角权重。
// ==========================================================================

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

// 异常：边哈希表容量已达 GPU storage buffer 上限仍放不下所有可见面对（不可恢复）。
struct EdgeHashOverflow : std::runtime_error
{
    EdgeHashOverflow() : std::runtime_error("Visibility edge hash overflow at the GPU storage-buffer limit")
    {
    }
};

// A per-pixel fragment list that still fits the device is recoverable: the
// layer count is doubled and the visibility resources are rebuilt, which is
// much cheaper than failing a multi-node run.
struct RasterLayerOverflow : std::runtime_error
{
    RasterLayerOverflow()
        : std::runtime_error(
              "Visibility A-buffer overflow: the per-pixel fragment list was full and the "
              "device memory budget does not allow more layers; reduce scene density or "
              "restore the retained CPU rasterizer")
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

// 初始化配置。rasterWidth/Height：每个可见性方向的栅格分辨率（像素）；
// maxFragmentsPerPixel：A-buffer 每像素初始层数（1..256，溢出时自动扩大）；
// gpuIndex：rankDevices 排序后的设备序号；
// periodicNeighborCount：周期边界的邻接复制瓦片数（0..20，只参与遮挡）；
// periodicSizeX/Z：周期域在 x、z 方向的尺寸（与几何同单位）。
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

// 可见性图的诊断信息：面元/side/有向边数量，A-buffer 与哈希表溢出计数，
// 闭合误差 maxClosureError（每个 side 的各类计数之和与采样数之比偏离 1 的最大值），
// 以及下边界相关的逃逸/天空比例（见各字段注释）。
struct GraphDiagnostics
{
    uint32_t facetCount{};
    uint32_t surfaceCount{};
    uint32_t directedEdgeCount{};
    uint32_t fragmentOverflow{};
    uint32_t hashOverflow{};
    float maxClosureError{};
    // Lower boundary (applyGroundBoundary). Fractions are visibility samples
    // (facet x direction) of receiving object sides; the soil-facing back sides
    // of ground facets are excluded from every denominator.
    uint32_t groundFacets{};
    uint32_t inactiveGroundSides{};
    uint32_t groundEscapeSides{};       // object sides with any downward escape
    float groundEscapeFraction{};       // sum(ground) / sum(denominator), object sides
    float groundEscapeAreaFraction{};   // area-weighted mean per-side fraction
    float groundEscapeMaxFraction{};    // largest per-side fraction
    float groundEscapeGroundFraction{}; // same for the upward ground sides
    float skyFraction{};                // sum(sky) / sum(denominator), object sides
};

// Marks a side that is not a receiving surface (the soil-facing back side of a
// ground facet) in the ground-count array: no sky, no ground, no exchange.
constexpr uint32_t kInactiveSide = 0xffffffffU;

// Lower boundary of the visibility graph. Downward lines that leave the scene
// without a hit see the "equivalent ground": the area-weighted mean radiosity of
// the upward ground sides (weights sum to 1).
struct GroundBoundary
{
    std::vector<uint32_t> sides;
    std::vector<float> weights;
};

// Applies the lower boundary to a freshly resolved graph: facets from
// firstGroundFacet on are ground; their side facing down (by the facet normal)
// becomes inactive, the other side contributes to the equivalent ground.
// Fills the ground-escape diagnostics.
GroundBoundary applyGroundBoundary(const std::vector<Vertex> &vertices, uint32_t firstGroundFacet,
                                   std::vector<uint32_t> &denominators, std::vector<uint32_t> &skyCounts,
                                   std::vector<uint32_t> &groundCounts, GraphDiagnostics &diagnostics);
// Equivalent-ground radiosity of a solution (sky value without ground sides).
float groundBoundaryRadiosity(const GroundBoundary &ground, const std::vector<float> &radiosity, float skyRadiosity);

// 求解结果：radiosity 为逐 side 辐射度；iterations 为实际迭代数；
// maxDelta 为最后一次扫描的最大变化量（收敛判据）；incident 为逐 side 入射辐照度。
struct SolveResult
{
    std::vector<float> radiosity;
    uint32_t iterations{};
    float maxDelta{};
    // Irradiance computed from the same optics/state, without a CPU CSR walk.
    std::vector<float> incident;
    // Equivalent-ground radiosity seen by downward escapes in the final state.
    float groundRadiosity{};
};

// GPU 能量平衡的 push constant（48 字节，与 faceteb/energy.comp 一致）：
// facets 面元数；objects 物体面元数（其余为地面）；method 气孔模型（0 Collatz，其他 Farquhar）；
// phase 阶段（0 通量/残差，1 温度更新，2 长波光学参数）；air 气温（K）；rho 空气密度；
// pressure 气压（hPa）；vapor 水汽压（hPa）；dt 时间步（s）；co2（ppm）；
// relaxation 温度松弛因子；wind 风速（m/s，对应 shader 的 reserved 字段）。
struct EnergyPush
{
    uint32_t facets{}, objects{}, method{}, phase{};
    float air{}, rho{}, pressure{}, vapor{}, dt{}, co2{}, relaxation{}, wind{};
};
// 能量平衡状态快照：temperatures（K，每（状态,侧）一个）、fluxes（每（状态,侧）12 个 float）；
// optics 与 radiation 仅在 readEnergyState(true) 时填充。
struct EnergySnapshot
{
    std::vector<float> temperatures, fluxes;
    std::vector<SurfaceOptics> optics;
    SolveResult radiation;
};
// 一次 evaluateEnergy 的统计：maxima[0..4] 依次为最大残差、最大残差/响应、最大气孔失配、
// 最大叶残差、最大温度步长；failingFacets 为能量不闭合（|残差| 超限）的面元数。
struct EnergyStatistics
{
    std::array<float, 5> maxima{};
    uint32_t failingFacets{};
};

// 光谱求解的 push constant（56 字节，与 facetrt/spectral.comp 一致）：phase 阶段；band 当前波段；
// facets/materials 数量；sunX/Y/Z 太阳方向；strength Hapke 背景强度；beam 光束法向辐照度；
// scale 输出缩放；thermal 热红外标志；ping 当前解所在缓冲；bands 波段总数。
struct SpectralPush
{
    uint32_t phase{}, band{}, facets{}, materials{};
    float sunX{}, sunY{}, sunZ{}, strength{};
    float beam{}, scale{1.0f};
    uint32_t thermal{}, ping{}, bands{}, reserved{};
};
// 光谱审计数据：某波段的 optics、（亮,阴）发射、上次物理辐射度、输出辐射度与亮/阴状态。
struct SpectralAudit
{
    std::vector<SurfaceOptics> optics;
    std::vector<std::array<float, 2>> emission;
    std::vector<float> raw, output, states;
};

// 可用 GPU 候选：物理设备、图形+计算队列族、排序得分（独显优先）。
struct DeviceCandidate
{
    VkPhysicalDevice device{VK_NULL_HANDLE};
    uint32_t queueFamily{};
    uint32_t score{};
};

// GPUs usable by the Facet Vulkan backend, in Config::gpuIndex order.
std::vector<DeviceCandidate> rankDevices(VkInstance instance);

// 类：FacetrtVulkan（不可拷贝）。生命周期：initialize → setGeometry →
// buildVisibilityGraph → solve*；setGeometry 会使已建的可见性图失效。
// 接口均为同步调用（内部提交后等待 GPU 完成）。
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
    // Facets from firstGroundFacet on are the scene ground (lower boundary);
    // call before buildVisibilityGraph. Default: no ground facets.
    void setGroundFacets(uint32_t firstGroundFacet)
    {
        m_firstGroundFacet = firstGroundFacet;
    }
    // Equivalent-ground radiosity of the last recorded solve or incident pass.
    [[nodiscard]] float groundRadiosity() const;
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
    // beam >= 0 overrides the beam-normal irradiance given to beginSpectral for
    // this band (per-band light; the push-constant layout is unchanged).
    SolveResult solveSpectralBand(uint32_t band, bool thermal, float sky, float scale, bool accelerated,
                                  bool warmStart, bool diffuse = false, float beam = -1.0f);
    std::vector<float> readSpectralOutput();
    SpectralAudit readSpectralAudit(uint32_t band);
    // Audit-only restore after the retained reference solver used m_optics.
    void restoreSpectralOptics(const std::vector<SurfaceOptics> &optics);
    // GPU 能量平衡接口。supportsGpuEnergy/Incident 取决于设备是否支持 shaderFloat64 且
    // 对应着色器存在。
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
    EnergyStatistics evaluateEnergy(float skyRadiosity);
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
    // 以下为私有实现细节：Vulkan 缓冲包装、CSR 边、push constant 结构与各类资源句柄。
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

    // 可见性阶段 push constant：viewProjection 为列主序的 4×4 视图投影矩阵；
    // viewDirection 为单位视线方向；raster = (宽, 高, 每像素最大层数, 哈希容量)；
    // periodic = (x 尺寸, z 尺寸, 邻接瓦片数, 保留)。
    struct VisibilityPush
    {
        float viewProjection[16]{};
        float viewDirection[4]{};
        uint32_t raster[4]{}; // width, height, max layers, hash capacity
        float periodic[4]{};  // domain X/Z size, neighbour count, reserved
    };

    // 求解阶段 push constant：ping 为当前读取的辐射度缓冲；skyRadiosity 天空辐射度；
    // relaxation 松弛因子；groundSideCount 为等效地面的 side 数。
    struct SolvePush
    {
        uint32_t facetCount{};
        uint32_t ping{};
        float skyRadiosity{};
        float relaxation{1.0f};
        uint32_t groundSideCount{};
        uint32_t reserved[3]{};
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
    // Equivalent-ground reduction of the iterate in buffer `ping`; leaves the
    // radiosity pipeline bound.
    void recordGroundMean(VkCommandBuffer commandBuffer, uint32_t ping, float skyRadiosity);
    void recordEnergy(VkCommandBuffer commandBuffer, uint32_t phase);
    void computeBarrier(VkCommandBuffer commandBuffer) const;
    void recordSpectral(VkCommandBuffer commandBuffer, const SpectralPush &push);
    void destroySpectralResources();
    EnergyStatistics readEnergyStatistics();

    void destroyVisibilityResources();
    void destroySolveResources();
    void destroyBuffer(Buffer &buffer) const;

    // Doubles the A-buffer depth when the device still has room for it. The
    // caller rebuilds the visibility resources afterwards. Returns false once
    // the shader limit (256 layers) or the memory budget is reached.
    [[nodiscard]] bool growRasterLayers();
    [[nodiscard]] uint64_t rasterLayerBudgetBytes() const;
    // Directions of the most recent visibility graph, so an overflow detected
    // later (sun visibility) can rebuild the graph the solve resources use.
    std::vector<Direction> m_visibilityDirections;
    std::vector<float> computeSunlitFractionOnce(const Direction &sunDirection);

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

    // 成员：配置与几何；以下按可见性资源 → 求解资源 → incident/能量 → 光谱的顺序排列。
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
    Buffer m_groundCounts;
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
    std::vector<uint32_t> m_groundCountsCpu;
    GroundBoundary m_ground;
    uint32_t m_firstGroundFacet{0xffffffffU};
    bool m_groundUsed{false};

    Buffer m_rowOffsets;
    Buffer m_edges;
    Buffer m_optics;
    Buffer m_radiosityA;
    Buffer m_radiosityB;
    Buffer m_residual;
    Buffer m_groundSides;
    Buffer m_groundWeights;
    Buffer m_groundMean;
    VkPipeline m_groundPipeline{VK_NULL_HANDLE};

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
    // Ping-pong buffer and iteration count of the last evaluateEnergy solve,
    // read back by readEnergyState(true).
    uint32_t m_energyRadiosityPing{0};
    uint32_t m_energyIterations{0};
    // Every recordIterations() call bumps m_solveGeneration. evaluateEnergy()
    // warm-starts from its previous radiosity only if no other solve has
    // touched the ping-pong buffers since (generation unchanged).
    uint64_t m_solveGeneration{0};
    uint64_t m_energyWarmGeneration{0};
    // Deepest A-buffer list reported by the last overflowing raster pass.
    uint32_t m_requiredRasterLayers{0};
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
