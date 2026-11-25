// ==========================================================================
// 文件：facetrtcore.cpp（histream / facetrt）
// 功能：FacetRT（面元辐射）与 FacetEB（面元能量平衡）两种模式的完整核心实现。文件很大
//       （约 8100 行），把“场景/参数读取 → 面元可见性图 → 辐射求解 → 能量平衡 → 结果输出”
//       全部放在一个翻译单元里；对外只暴露 facetrtcore.h 中的 runFacetRTCore / runFacetEBCore。
// 主要模块/步骤（按文件先后顺序）：
//   1. 公共基础（匿名命名空间开头）：线程数与 OpenMP 并行、UTC 时间戳、FacetRunIndex
//      （faceteb_run.json 运行索引）、常量（光栅尺寸/层数、迭代数、Stefan-Boltzmann 等）、
//      2.5–6 um 反射太阳光谱形状 mwirSolarShape（与 shader/mwir_solar.glsl 保持一致）。
//   2. 数据结构：SurfaceParameters（单个材质的光学/热学/生理/船体/车辆参数）、
//      FacetRTParameters、Scene（三角面元顶点、材质索引、太阳角、光栅设置、周期邻域）、
//      CpuGraph（CPU 稀疏可见性图）、RunResult（一次辐射运行的结果与耗时）。
//   3. 向量小工具与 OBJ 三角形读取/按域裁剪（appendTriangle、clipPolygonToPlane、
//      loadTreeWithSoil）；半球方向、太阳方向、Planck 辐亮度、波段取值。
//   4. 背景（地面/土壤）Hapke 双向反射与红外方向发射率（含 Gauss-Legendre 积分表、
//      反照率表），以及反射率纹理方差因子 finalizeReflectanceTextureFactors。
//   5. 能量平衡用的辅助物理：饱和水汽压、潜热通量及其导数、叶片气孔阻抗（调用
//      leafbiochemistry.h）、土壤储热系数与历史记忆模态。
//   6. 光学参数组装：makeOptics / makeEnergyShortwaveOptics / makeEnergyLongwaveOptics /
//      makeSpectralOptics / makeStateSpectralOptics（把材质参数变成每个面元的反射/透射/发射）。
//   7. CPU 光栅化后端：CpuRasterizer（正交投影 + 每像素定长片元列表 A-buffer）→ buildCpuGraph（可见性图）→
//      computeCpuSunlit（受光比例）→ solveCpu / incidentIrradianceCpu（Jacobi 辐射迭代）→ runCpu。
//   8. GPU 后端：prepareGpuSpectral / runGpuSpectral / runGpu（经 facetrt_vulkan.h 的
//      FacetrtVulkan），并在 CPU/GPU 间做谱带一致性自检；writeWebResult 输出 facetrt.json。
//   9. 项目输入解析（第二个匿名命名空间）：XML/JSON 文本读取、标签/属性提取、材质与光学系数、
//      土壤与光源（太阳/天空光谱）、OBJ 几何与 DEM 地面、放置变换、loadProjectScene。
//  10. runFacetRTCore：读场景 → 选后端（cpu/gpu/auto，auto 时 GPU 失败回退 CPU）→ 求解 → 写结果。
//  11. runFacetEBCore（文件最后的巨型函数）：读气象与控制参数 → 建共享可见性图 →
//      选择 GPU/CPU 能量后端 → 初始化冠层柱/土壤/墙体/水体/船体/车辆状态 →
//      逐时间节点循环：太阳位置与受光比例 → 阳光/阴影状态迁移 → 短波窗口辐射求解 →
//      辐射-能量耦合迭代（残差判据收敛）→ 车辆舱室等附加模型 → 波段图像与过程/诊断产品输出。
// 数据流：project.json 与 OBJ/DEM/气象文件 → Scene + SurfaceParameters → 可见性图（GPU 或
//       CPU）→ 逐波段辐射求解 →（EB 模式）逐节点能量平衡 → faceteb.json / facetrt.json、
//       *.bin、diagnostics、PROGRESS/RESULT 标准输出行（由 Node 服务端解析进度）。
// 与其他文件的关系：
//   - 入口：facetrt/command.cpp 调 runFacetRTCore；faceteb/command.cpp 调 runFacetEBCore。
//   - 依赖：facetrt_vulkan.h（GPU 可见性/求解/能量内核）、energyphysics.h（EB 物理公式）、
//     leafbiochemistry.h（叶片光合/气孔）、lumpedthermal.h（集总热体）、
//     ../base/*（projectjson、compo、土壤 TIR 发射率、反射率纹理、shortwave_split）、
//     ../thirdparty/spa.h（太阳位置）；着色器见 models/histream/shader。
// 约定：温度 K；辐照/通量 W m-2；波长在输入中可为 nm 或 um（>50 视为 nm，见 mwirSolarShape）；
//       时间步 dTime 单位 s；每个面元有两侧（side = facet*2 + 0/1），每侧再分
//       受光(0)/阴影(1)两个状态；坐标系 y 向上、+X 为北、+Z 为东（SPA 方位角自北顺时针，见 sunDirection）；
//       Vec3 为 float[3]。
// 注意：本文件位于匿名命名空间内的函数多为内部实现；大量开关来自 HISTREAM_* 环境变量与
//       project.json 的 control/light 设置，缺省值用于兼容旧工程。
// ==========================================================================

#include "facetrtcore.h"
#include "../base/projectjson.h"
#include "facetrt_vulkan.h"
#include "leafbiochemistry.h"
#include "lumpedthermal.h"
#include "energyphysics.h"
#include "../base/compo.h"
#include "../base/soil_tir_emissivity.h"
#include "../base/reflectance_texture.h"
#include "../base/utils.h"
#include "../base/shortwave_split.h"
#include "../thirdparty/spa.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gdal_priv.h>
#include <omp.h>

// 模块：匿名命名空间 1（文件内部实现，至 `} // namespace` 为止）。
// 顺序：基础工具与常量 → 数据结构 → OBJ 读取 → Hapke/反射率纹理 → 能量平衡辅助物理 →
// 光学参数组装 → CPU 光栅化后端 → GPU 后端 → writeWebResult。
// 第 2 个匿名命名空间是项目输入（project.json / XML）解析，再后是两个对外入口。
namespace {

using Clock = std::chrono::steady_clock;
using Vec3 = std::array<float, 3>;

// Engine::input sets HISTREAM_CPU_THREADS from control.cpuThreads (default 32,
// at most the machine's logical processors).
int cpuThreadCount()
{
    if (const char* value = std::getenv("HISTREAM_CPU_THREADS")) {
        const int requested = std::atoi(value);
        if (requested > 0) return requested;
    }
    return std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 32);
}

// Runs body(begin, end) over [0, count) in chunks on the CPU worker threads;
// the first exception thrown by any chunk is rethrown on the calling thread.
template <typename Body>
void parallelRanges(size_t count, size_t grain, Body&& body)
{
    if (count == 0U) return;
    grain = std::max<size_t>(1U, grain);
    const long long chunks = static_cast<long long>((count + grain - 1U) / grain);
    std::exception_ptr failure;
    std::mutex failureMutex;
#pragma omp parallel for schedule(dynamic, 1) num_threads(cpuThreadCount())
    for (long long chunk = 0; chunk < chunks; ++chunk) {
        const size_t begin = static_cast<size_t>(chunk) * grain;
        try {
            body(begin, std::min(count, begin + grain));
        } catch (...) {
            std::lock_guard<std::mutex> lock(failureMutex);
            if (!failure) failure = std::current_exception();
        }
    }
    if (failure) std::rethrow_exception(failure);
}

// 作用：当前 UTC 时间的 ISO-8601 字符串（秒精度，如 2026-01-01T00:00:00Z）。
// 用途：faceteb_run.json 的 startedAt / finishedAt。
std::string utcTimestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream text;
    text << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return text.str();
}

// This index references only the current invocation. A stopped CLI run may
// leave older node files beside it; consumers must not infer completion from
// the directory's contents.
struct FacetRunIndex {
    std::filesystem::path path;
    ProjectJson::Json value;
    void write() {
        std::ofstream stream(path, std::ios::trunc);
        stream << value.dump(2) << '\n';
        if (!stream) throw std::runtime_error("FacetEB cannot write run index");
    }
    // 析构保护：运行中途抛异常或被中断而 status 仍为 running 时，改写为 failed 并记录
    // finishedAt；写盘失败被吞掉，避免在析构中再抛异常。
    ~FacetRunIndex() {
        if (value.value("status", "") == "running") {
            value["status"] = "failed";
            value["finishedAt"] = utcTimestamp();
            try { write(); } catch (...) {}
        }
    }
};
// 常量说明：
//   - kRasterSize/kRasterLayers：可见性光栅默认 1024×1024，每像素最多 64 个片元层（A-buffer）；
//     场景水平范围 >= kLargeSceneExtent 时改用 2048×2048 且只保留 16 层，使片元缓冲总量
//     不变（见 loadProjectScene）。kMaximumRasterLayers 是自适应层数的上限。
//   - kSkyDirectionCount：建可见性图时使用的半球方向数（见 makeHemisphereDirections）。
//   - kIterations：辐射 Jacobi 迭代的默认次数上限。
//   - kStandaloneSoilGridResolution：独立 OBJ 无地形时自动补的地面每边格数（48）。
//   - kBeamNormalIrradiance / kSkyDiffuseIrradiance：FacetRT 历史缺省光照（W m-2），
//     Scene 中水平天空散射缺省取 kSkyDiffuseIrradiance 的一半（即 50 W m-2）。
constexpr float kPi = 3.14159265358979323846f;
constexpr uint32_t kRasterSize = 1024;
constexpr uint32_t kRasterLayers = 64;
constexpr uint32_t kLargeSceneRasterSize = 2048;
constexpr uint32_t kLargeSceneRasterLayers = 16;
constexpr uint32_t kMaximumRasterLayers = 64;
// The retained CPU rasterizer and the GPU shaders can both resolve 256 layers;
// a run that overflows doubles the reserved depth (see growCpuRasterLayers and
// FacetrtVulkan::growRasterLayers) instead of failing.
constexpr uint32_t kMaximumCpuRasterLayers = 128;
constexpr uint64_t kMaximumCpuFragmentBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr float kLargeSceneExtent = 1000.0f;
constexpr uint32_t kSkyDirectionCount = 40;
constexpr uint32_t kIterations = 64;
constexpr uint32_t kStandaloneSoilGridResolution = 48;
constexpr float kBeamNormalIrradiance = 700.0f;
constexpr float kSkyDiffuseIrradiance = 100.0f;
constexpr float kStefanBoltzmann = 5.670374419e-8f;

// Reflected sunlight in thermal bands between 2.5 and 6 um: the same table
// and sky ratio as shader/mwir_solar.glsl (keep both identical). The table is
// the bundled Esun_.dat on its 2500-6000 nm, 100 nm grid divided by its
// 400-2400 nm integral in um, so (shortwave total, W m-2) x shape is the
// spectral irradiance in W m-2 um-1. Exactly 0 at <= 2.5 um and >= 6 um.
constexpr std::array<float, 36> kMwirSolarShape = {
    1.7800e-02f, 1.3376e-03f, 6.2215e-09f, 3.8114e-04f, 2.7788e-03f, 4.6614e-03f,
    6.4018e-03f, 6.2056e-03f, 7.0701e-03f, 1.1103e-02f, 1.3684e-02f, 1.3498e-02f,
    1.2362e-02f, 1.1301e-02f, 1.0473e-02f, 9.0935e-03f, 6.4206e-03f, 3.2183e-03f,
    1.2786e-04f, 1.4780e-04f, 1.0580e-03f, 2.4357e-03f, 3.0036e-03f, 2.3447e-03f,
    1.7338e-03f, 1.3681e-03f, 7.3824e-04f, 2.0866e-04f, 6.5639e-05f, 1.2040e-05f,
    1.1073e-06f, 3.5406e-08f, 9.0807e-14f, 0.0f, 0.0f, 0.0f};
constexpr float kMwirSkySolarRatio = 0.0847f;

// 作用：2.5–6 um 反射太阳光谱形状（kMwirSolarShape，100 nm 间隔）的线性插值。
// 输入：波长（>50 视为 nm，否则 um）；输出：um-1，乘以水平总短波（W m-2）得 W m-2 um-1。
// 区间 (2.5, 6) um 之外恒为 0。
float mwirSolarShape(float wavelength)
{
    const float um = wavelength > 50.0f ? wavelength * 0.001f : wavelength;
    if (!(um > 2.5f) || !(um < 6.0f)) return 0.0f;
    const float x = (um - 2.5f) * 10.0f;
    const int index = std::clamp(static_cast<int>(std::floor(x)), 0,
                                 static_cast<int>(kMwirSolarShape.size()) - 2);
    const float t = std::clamp(x - static_cast<float>(index), 0.0f, 1.0f);
    return std::max(kMwirSolarShape[index] +
                        (kMwirSolarShape[index + 1] - kMwirSolarShape[index]) * t,
                    0.0f);
}

using facetbio::LeafBiochemistry;

// 模块：一种材质（面元类别）的全部参数，既用于 FacetRT 也用于 FacetEB。
// 分组：光学（逐波段 reflectance / transmittance、热红外 tir*、energy* 能量窗口用）；
// 热学与土壤（比热/密度/导热、heatCapacityPerArea、soilTemperatureMethod）；
// 植被（leaf 生理参数、leafWidth、surfaceResistance）；光伏 pv*；水体 water*；
// 建筑墙体 wallSlab；船体 ship*/waterline*；车辆 vehicle*/cabin*/engine* 等。
// 缺省值多为兼容旧工程的历史值；各字段旁的注释说明了开关语义。
struct SurfaceParameters {
    std::vector<float> reflectance{0.10f};
    std::vector<float> transmittance{0.05f};
    float tirReflectance{0.03f};
    float tirTransmittance{};
    float sunlitTemperature{300.0f};
    float shadedTemperature{296.0f};
    float surfaceResistance{120.0f};
    float specificHeat{1180.0f};
    float density{1800.0f};
    float thermalConductivity{1.55f};
    float soilMoisture{0.25f};
    float heatCapacityPerArea{60000.0f};
    std::string lumpedGroup;
    facetphysics::LumpedThermalParameters lumped;
    double lumpedWeight{1};
    double lumpedMaxSurfaceK{500};
    int soilTemperatureMethod{1};
    std::vector<float> energyReflectance;
    std::vector<float> energyTransmittance;
    std::shared_ptr<const ReflectanceVarianceTexture> reflectanceTexture;
    float reflectanceTextureStrength{};
    float reflectanceTextureRelativeStd{};
    float reflectanceTextureRepeatSize{1.0f};
    bool photovoltaic{false};
    bool vegetation{true};
    float pvEta25{0.22f}, pvGamma{-0.0035f}, pvBifaciality{0.0f};
    float convectiveScale{1.0f};
    // Water mixed layer (componentType Water): heatCapacityPerArea holds
    // rho_w c_w h_mix, one face exchanges with the air, LE uses surfaceResistance.
    bool water{false};
    float evaporationCoefficient{1.0f};
    // Water start temperature: 0 first-node air (default), 1 mean air
    // temperature of the first 24 h from the start node, 2 waterInitialK.
    int waterInitialMode{0};
    float waterInitialK{293.15f};
    // Building wall/roof slab (wallMode "wall"); false keeps the thin shell.
    bool wall{false};
    facetphysics::WallSlab wallSlab;
    // Ship hull (material type Ship): speed through the air (m/s) added to the
    // wind for forced convection, u_rel = sqrt(u^2 + speed^2); facets whose
    // centre lies below waterlineHeight (world y) exchange heat with the water
    // (coefficient waterCoefficient, temperature waterK or the scene water).
    float shipSpeed{0.0f};
    bool waterline{false};
    float waterlineHeight{0.0f};
    float waterCoefficient{500.0f};
    float waterK{-1.0f};
    // Ship hull extras (all default off). shipWindCosine: cosine of the angle
    // between the ship course and the direction the wind blows from, so the
    // relative wind is |u_wind - v_ship|^2 = u^2 + v^2 + 2 u v cos (0 = old
    // quadrature sum). waterlineFromWater: the waterline is the scene water
    // surface (+ waterlineOffset) and the wetted fraction of every facet is the
    // exact triangle part below it. waterShield: submerged facets receive no sun
    // or sky radiation. waterFeedback: the hull heat flux warms/cools the scene
    // water. internalFlux (W m-2 facet area) and internalPower (W, spread over
    // the area of the material's facets): internal heat sources.
    float shipWindCosine{0.0f};
    bool waterlineFromWater{false};
    float waterlineOffset{0.0f};
    bool waterShield{false};
    bool waterFeedback{false};
    float internalFlux{0.0f};
    float internalPower{0.0f};
    // Vehicle (FacetEB car cabin, material type Vehicle). vehiclePart: 0 none,
    // 1 body (carries the cabin parameters), 2 glass (transmits sun into the
    // cabin), 3 underbody/engine bay (thin shell, engine heat). Cabin parameters
    // are read from the body material of each vehicle instance.
    int vehiclePart{0};
    bool vehicleHasParams{false};
    bool cabinEnabled{false};
    int occupants{0};
    double metabolicW{100}, cabinVolume{2.8}, interiorCapacity{80000}, interiorArea{8}, interiorH{8};
    double absorbedFraction{0.8}, ventilationAch{0.5}, crackedAch{10};
    bool windowCracked{false}, acOn{false}, engineOn{false};
    double acPower{3500}, acSetpointC{24}, acBandK{2}, enginePowerW{15000}, engineShare{0.3};
    float vehicleSpeed{0.0f}, vehicleWindCosine{0.0f};
    // Vehicle parts model (docs/vehicle-thermal.md section 3/4). Absent tags keep the
    // lumped interior node and the engine heat spread over the underbody (old results).
    // vehiclePart 4 = bonnet (wall slab whose indoor side is the engine-bay air).
    std::string meshName;                       // OBJ group of this material (per-window output)
    bool interiorMulti{false};                  // dashboard / seat / wheel / lining nodes
    std::array<double, 4> intShare{0.3, 0.35, 0.05, 0.3}, intCapacity{10000, 40000, 2000, 60000},
        intHcA{8, 24, 2, 40}, intRadArea{1, 3, 0.25, 8};
    double shellRadFraction{0.45}, seatEffusivity{300};
    bool engineMulti{false}, engineWarm{false};
    double engineWarmC{90}, engineBlockCapacity{70000}, engineBayCapacity{15000}, engineBlockBay{40}, bayVent0{0.04}, bayVentK{0.004}, firewallG{2};
    std::array<double, 3> engineSplit{0.5, 0.3, 0.2}; // radiator -> bay air, block, exhaust -> underbody
    float leafWidth{0.1f};
    LeafBiochemistry leaf;
    uint8_t outputComponent{0}; // 0 unknown, 1 soil/background, 2 vegetation, 3 building, 4 PV, 5 water

};

// 模块：FacetRT/FacetEB 的辐射参数。wavelengths 为波长（nm，缺省 550）；object / ground 为
// 物体与地面材质参数（ground 缺省 ρ=0.20、sunlit 305 K、shaded 295 K）；skyTemperature（K）；
// bandBeamNormal / bandSkyDiffuse 为逐波段光照，为空时回退到 Scene 的宽带缺省。
struct FacetRTParameters {
    std::vector<float> wavelengths{550.0f};
    SurfaceParameters object;
    SurfaceParameters ground{{0.20f}, {0.0f}, 0.05f, 0.0f, 305.0f, 295.0f};
    float skyTemperature{250.0f};
    bool acceleratedSolver{};
    // FacetRT per-band illumination (beam-normal direct and horizontal sky
    // diffuse) from the light settings; empty = Scene broadband defaults.
    std::vector<float> bandBeamNormal;
    std::vector<float> bandSkyDiffuse;
};

// 模块：面元场景。vertices 每 3 个顶点为一个三角面元（含几何法向），materialIndices 指向 materials。
// 前 leafFacetCount 个面元是“物体”面元（名称沿用旧工程的“叶”），其后是地面面元（待核对）。
// 每个面元有两侧：side = facet*2 + 0（法向侧）/ +1（反法向侧）。
// sunZenith / sunAzimuth 单位为度；minimum / maximum 为域包围盒（长度单位，通常 m）；
// visibilityRaster* 为可见性光栅分辨率与层数；periodic* 为周期边界邻域副本（个数与平铺尺寸）。
struct Scene {
    std::vector<facetvk::Vertex> vertices;
    std::vector<uint32_t> materialIndices;
    std::vector<SurfaceParameters> materials;
    std::vector<float> reflectanceTextureFactors;
    uint32_t leafFacetCount{};
    // Vehicle instance (1-based) of every facet; empty without vehicles.
    std::vector<uint32_t> facetInstance;
    std::vector<std::string> vehicleNames;
    Vec3 minimum{};
    Vec3 maximum{};
    float sunZenith{30.0f};
    float sunAzimuth{135.0f};
    float backgroundAngularStrength{};
    // FacetRT broadband illumination. Defaults are the historical values
    // (700 W m-2 beam normal, 50 W m-2 horizontal sky); configureFacetRTLight
    // replaces them from the project light settings when present.
    float beamNormalIrradiance{kBeamNormalIrradiance};
    float skyDiffuseIrradiance{kSkyDiffuseIrradiance * 0.5f};
    // Configured horizontal global shortwave (light.shortwaveIrradiance,
    // W m-2) for the FacetRT 2.5-6 um solar term; 0 = 700 cos + 50 W m-2.
    float shortwaveIrradiance{};
    uint32_t visibilityRasterSize{kRasterSize};
    uint32_t visibilityRasterLayers{kRasterLayers};
    uint32_t periodicNeighborCount{};
    float periodicSizeX{1.0f};
    float periodicSizeZ{1.0f};

    uint32_t facetCount() const { return static_cast<uint32_t>(vertices.size() / 3U); }
};

// 作用：取第 band 个波段的法向直射辐照（bandBeamIrradiance）或水平天空散射辐照
// （bandSkyIrradiance），单位 W m-2；没有逐波段值时回退到 Scene 的宽带缺省。
float bandBeamIrradiance(const Scene& scene, const FacetRTParameters& parameters, size_t band)
{
    return band < parameters.bandBeamNormal.size()
        ? parameters.bandBeamNormal[band] : scene.beamNormalIrradiance;
}

float bandSkyIrradiance(const Scene& scene, const FacetRTParameters& parameters, size_t band)
{
    return band < parameters.bandSkyDiffuse.size()
        ? parameters.bandSkyDiffuse[band] : scene.skyDiffuseIrradiance;
}

// 作用：按面元密度自适应选择可见性光栅每像素的片元层数。
// 输出：从 scene.visibilityRasterLayers 起翻倍，直到覆盖估计层数或达到 kMaximumRasterLayers。
uint32_t adaptiveVisibilityRasterLayers(const Scene& scene)
{
    const double pixelCount = static_cast<double>(scene.visibilityRasterSize) *
                              scene.visibilityRasterSize;
    if (pixelCount <= 0.0) return scene.visibilityRasterLayers;

    // Triangle projections are highly non-uniform in vegetation and urban
    // scenes. Reserve sixteen times the scene-wide average depth and round to
    // a power of two. This keeps sparse large scenes at 16 layers while dense
    // canopies automatically expand to 32 or 64 layers.
    const double averageFragments =
        static_cast<double>(scene.facetCount()) *
        static_cast<double>(scene.periodicNeighborCount + 1U) / pixelCount;
    const uint32_t estimatedLayers = static_cast<uint32_t>(std::ceil(
        std::max(1.0, averageFragments * 16.0)));
    uint32_t layers = scene.visibilityRasterLayers;
    while (layers < estimatedLayers && layers < kMaximumRasterLayers) {
        layers = std::min(kMaximumRasterLayers, layers * 2U);
    }
    return layers;
}

// 可见性图的一条边：neighbor 为被看到的面侧（side）索引，count 为该方向上看到它的光栅样本数
// （权重）。同一面侧的所有 count 与天空/地面计数之和等于该侧的 denominator。
struct Edge {
    uint32_t neighbor{};
    uint32_t count{};
};

// The retained CPU rasterizer fails on a full per-pixel fragment list and the
// run cannot resume afterwards, so grow the reserved depth and repeat the pass.
bool isCpuRasterOverflow(const std::exception& error)
{
    const std::string message = error.what();
    return message == "CPU A-buffer overflow" ||
           message == "CPU sunlight A-buffer overflow";
}

bool growCpuRasterLayers(Scene& scene)
{
    const uint32_t current = std::max(kRasterLayers, scene.visibilityRasterLayers);
    if (current >= kMaximumCpuRasterLayers) {
        return false;
    }
    const uint32_t next = std::min(kMaximumCpuRasterLayers, current * 2U);
    const uint64_t pixels = uint64_t(scene.visibilityRasterSize) *
                            uint64_t(scene.visibilityRasterSize);
    // Fragment is 12 bytes on the CPU rasterizer.
    if (pixels * uint64_t(next) * 12ULL > kMaximumCpuFragmentBytes) {
        return false;
    }
    scene.visibilityRasterLayers = next;
    std::cout << "facetrt: CPU A-buffer overflow at " << current
              << " layers; reserving " << next << " layers per pixel" << std::endl;
    return true;
}

// Lower-boundary (downward escape) diagnostics of a visibility graph.
ProjectJson::Json groundEscapeJson(const facetvk::GraphDiagnostics& graph)
{
    return {{"groundFacets", graph.groundFacets},
            {"inactiveGroundSides", graph.inactiveGroundSides},
            {"groundEscapeSides", graph.groundEscapeSides},
            {"groundEscapeFraction", graph.groundEscapeFraction},
            {"groundEscapeAreaFraction", graph.groundEscapeAreaFraction},
            {"groundEscapeMaxFraction", graph.groundEscapeMaxFraction},
            {"groundEscapeGroundFraction", graph.groundEscapeGroundFraction},
            {"objectSkyFraction", graph.skyFraction}};
}

// 作用：把下边界（物体侧向下逃逸）诊断格式化成中文日志行，写入 PROGRESS 标准输出。
std::string groundEscapeLog(const facetvk::GraphDiagnostics& graph)
{
    std::ostringstream text;
    text << std::setprecision(4) << "下边界：物体侧向下逃逸 " << 100.0 * graph.groundEscapeFraction
         << "%（面积加权 " << 100.0 * graph.groundEscapeAreaFraction << "%，单侧最大 "
         << 100.0 * graph.groundEscapeMaxFraction << "%，" << graph.groundEscapeSides
         << " 侧），按等效地面计；地面背面 " << graph.inactiveGroundSides << " 侧不参与求解";
    return text.str();
}

// 模块：CPU 可见性图（CSR 稀疏格式）。rowOffsets/edges：每个面侧可见的其他面侧；
// denominators：该侧总采样数；skyCounts / groundCounts：看到天空 / 等效地面的采样数
// （groundCounts == kInactiveSide 表示该侧不参与求解，如地面背面）；
// ground：下边界等效地面信息；diagnostics：图的统计与诊断。
struct CpuGraph {
    std::vector<uint32_t> rowOffsets;
    std::vector<Edge> edges;
    std::vector<uint32_t> denominators;
    std::vector<uint32_t> skyCounts;
    std::vector<uint32_t> groundCounts;
    facetvk::GroundBoundary ground;
    facetvk::GraphDiagnostics diagnostics;
};

// 模块：一次 FacetRT 辐射运行的结果与耗时（writeWebResult 据此写 facetrt.json）。
// solution 为宽带辐射度；bandRadiosity / bandStates 为逐波段结果；*Ms 为各阶段毫秒数；
// spectral*Error 为 GPU 谱段路径与 CPU 参考的最大偏差（自检用，待核对具体含义）。
struct RunResult {
    std::string backend;
    facetvk::GraphDiagnostics graph;
    facetvk::SolveResult solution;
    std::vector<float> sunlit;
    std::vector<float> lightEnhancement;
    std::vector<float> wavelengths;
    std::vector<float> bandRadiosity;
    // Per band and facet: front lit, front shaded, back lit, back shaded.
    std::vector<float> bandStates;
    std::string quantity{"hemispherical radiosity"};
    std::string units{"W m-2"};
    double visibilityMs{};
    double sunlightMs{};
    double solveMs{};
    double totalMs{};
    bool gpuSpectral{};
    uint32_t spectralBandsChecked{};
    float spectralOpticsError{}, spectralRawError{}, spectralStatesError{}, spectralOutputError{};
    uint32_t spectralIterationError{};
    std::vector<uint32_t> spectralIterations;
};

// 小型 Vec3（float[3]）向量工具：add / subtract / multiply / dot / cross（右手系）/ normalized。
// normalized 对零长度向量抛异常（用于发现退化三角形）。
Vec3 add(const Vec3& a, const Vec3& b)
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Vec3 subtract(const Vec3& a, const Vec3& b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vec3 multiply(const Vec3& value, float scale)
{
    return {value[0] * scale, value[1] * scale, value[2] * scale};
}

// 作用：周期边界副本的平铺偏移（X、Z 方向的格数）。index 0 为中心块（0,0）；
// 1–20 依次为 4 个边邻、4 个角邻、4 个二阶边邻、8 个“马步”邻，对应
// Scene.periodicNeighborCount 取 0/4/8/12/20；平移量 = 偏移 × periodicSizeX / periodicSizeZ。
std::array<int, 2> periodicTileOffset(uint32_t index)
{
    static constexpr std::array<std::array<int, 2>, 20> offsets{{
        {{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}},
        {{-1, -1}}, {{-1, 1}}, {{1, -1}}, {{1, 1}},
        {{-2, 0}}, {{2, 0}}, {{0, -2}}, {{0, 2}},
        {{-2, -1}}, {{-2, 1}}, {{2, -1}}, {{2, 1}},
        {{-1, -2}}, {{1, -2}}, {{-1, 2}}, {{1, 2}}
    }};
    return index == 0U ? std::array<int, 2>{0, 0}
                       : offsets[std::min<uint32_t>(index - 1U, 19U)];
}

float dot(const Vec3& a, const Vec3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}

Vec3 normalized(const Vec3& value)
{
    const float length = std::sqrt(dot(value, value));
    if (!(length > 1.0e-8f)) {
        throw std::runtime_error("Zero-length direction or degenerate triangle");
    }
    return multiply(value, 1.0f / length);
}

double milliseconds(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

// 作用：解析 OBJ 面记录中 v/vt/vn 的顶点位置索引，支持负数相对索引（-1 为最后一个顶点）。
// 输出：0 基索引；缺失或越界时抛异常。
int parsePositionIndex(const std::string& token, size_t vertexCount)
{
    const std::string text = token.substr(0, token.find('/'));
    if (text.empty()) {
        throw std::runtime_error("OBJ face has no position index");
    }
    const int raw = std::stoi(text);
    const int index = raw > 0 ? raw - 1 : static_cast<int>(vertexCount) + raw;
    if (index < 0 || static_cast<size_t>(index) >= vertexCount) {
        throw std::runtime_error("OBJ position index is out of range");
    }
    return index;
}

// 作用：向场景追加一个三角面元（3 个顶点 + 几何法向 (b-a)×(c-a) 归一化 + 材质索引）。
// 退化三角形（法向模长平方 <= 1e-16）被丢弃并返回 false。
bool appendTriangle(Scene& scene,
                    const Vec3& a,
                    const Vec3& b,
                    const Vec3& c,
                    uint32_t materialIndex = 0U)
{
    const Vec3 normalVector = cross(subtract(b, a), subtract(c, a));
    if (dot(normalVector, normalVector) <= 1.0e-16f) {
        return false;
    }
    const Vec3 normal = normalized(normalVector);
    for (const Vec3& position : {a, b, c}) {
        facetvk::Vertex vertex{};
        std::copy(position.begin(), position.end(), vertex.position);
        std::copy(normal.begin(), normal.end(), vertex.normal);
        scene.vertices.push_back(vertex);
    }
    scene.materialIndices.push_back(materialIndex);
    return true;
}

// 作用：Sutherland–Hodgman 单平面裁剪。保留坐标 axis 上 >= plane（keepGreater）或 <= plane
// 的部分，容差 1e-6；返回裁剪后的多边形顶点（可能为空）。
std::vector<Vec3> clipPolygonToPlane(const std::vector<Vec3>& source,
                                     size_t axis,
                                     float plane,
                                     bool keepGreater)
{
    std::vector<Vec3> result;
    if (source.empty()) return result;
    const auto inside = [axis, plane, keepGreater](const Vec3& point) {
        return keepGreater ? point[axis] >= plane - 1.0e-6f
                           : point[axis] <= plane + 1.0e-6f;
    };
    Vec3 previous = source.back();
    bool previousInside = inside(previous);
    for (const Vec3& current : source) {
        const bool currentInside = inside(current);
        if (currentInside != previousInside) {
            const float denominator = current[axis] - previous[axis];
            if (std::abs(denominator) > 1.0e-8f) {
                const float amount = std::clamp(
                    (plane - previous[axis]) / denominator, 0.0f, 1.0f);
                Vec3 intersection{};
                for (size_t component = 0; component < 3; ++component) {
                    intersection[component] = previous[component] + amount *
                        (current[component] - previous[component]);
                }
                result.push_back(intersection);
            }
        }
        if (currentInside) result.push_back(current);
        previous = current;
        previousInside = currentInside;
    }
    return result;
}

// 作用：把三角形依次对域盒 [minimum, maximum] 的 6 个面裁剪，剩余多边形按扇形剖分后写入场景，
// 保证面元不超出模拟域。
void appendTriangleInsideDomain(Scene& scene,
                                const Vec3& a,
                                const Vec3& b,
                                const Vec3& c,
                                const Vec3& minimum,
                                const Vec3& maximum,
                                uint32_t materialIndex)
{
    std::vector<Vec3> polygon{a, b, c};
    for (size_t axis = 0; axis < 3 && polygon.size() >= 3U; ++axis) {
        polygon = clipPolygonToPlane(polygon, axis, minimum[axis], true);
        polygon = clipPolygonToPlane(polygon, axis, maximum[axis], false);
    }
    for (size_t index = 1; index + 1U < polygon.size(); ++index) {
        appendTriangle(scene, polygon[0], polygon[index], polygon[index + 1U],
                       materialIndex);
    }
}

// 模块：读取单个 OBJ（输入不是 project.json 而是 .obj 时的独立模式）。
// 输入：OBJ 文件，可含名为 HISTREAM_TERRAIN 的对象（其后的三角形视为地形）。
// 输出：Scene，材质 0 = 物体缺省，材质 1 = 地面（ρ=0.20）；leafFacetCount 为物体面元数。
// 说明：高度整体平移使最低点 y = 0；无地形时在 y = -0.01 处补一块方形土壤。
Scene loadTreeWithSoil(const std::string& fileName)
{
    std::ifstream input(fileName);
    if (!input) {
        throw std::runtime_error("Cannot open tree OBJ: " + fileName);
    }

    // 步骤 1：逐行解析 o / v / f 记录；多边形面按扇形拆成三角形，记录地形三角形的起点。
    std::vector<Vec3> positions;
    std::vector<std::array<int, 3>> triangles;
    size_t terrainTriangleStart = std::numeric_limits<size_t>::max();
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream stream(line);
        std::string record;
        stream >> record;
        if (record == "o") {
            std::string name;
            stream >> name;
            if (name == "HISTREAM_TERRAIN" && terrainTriangleStart == std::numeric_limits<size_t>::max()) {
                terrainTriangleStart = triangles.size();
            }
        } else if (record == "v") {
            Vec3 position{};
            if (!(stream >> position[0] >> position[1] >> position[2])) {
                throw std::runtime_error("Invalid OBJ vertex");
            }
            positions.push_back(position);
        } else if (record == "f") {
            std::vector<int> face;
            std::string token;
            while (stream >> token) {
                face.push_back(parsePositionIndex(token, positions.size()));
            }
            for (size_t i = 1; i + 1 < face.size(); ++i) {
                triangles.push_back({face[0], face[i], face[i + 1]});
            }
        }
    }
    if (positions.empty() || triangles.empty()) {
        throw std::runtime_error("Tree OBJ contains no usable geometry");
    }

    // 步骤 2：求包围盒，并把所有顶点沿 y 平移使最低点为 0。
    Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max()};
    Vec3 maximum{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                 std::numeric_limits<float>::lowest()};
    for (const Vec3& position : positions) {
        for (size_t axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], position[axis]);
            maximum[axis] = std::max(maximum[axis], position[axis]);
        }
    }
    for (Vec3& position : positions) {
        position[1] -= minimum[1];
    }

    // 步骤 3：先写入物体三角形（计入 leafFacetCount），地形三角形使用材质 1。
    const bool hasTerrain = terrainTriangleStart != std::numeric_limits<size_t>::max() &&
                            terrainTriangleStart < triangles.size();
    Scene scene;
    scene.materials = {
        SurfaceParameters{},
        SurfaceParameters{{0.20f}, {0.0f}, 0.05f, 0.0f, 305.0f, 295.0f}
    };
    const uint32_t soilFacetCount = hasTerrain ? 0U :
        2U * kStandaloneSoilGridResolution * kStandaloneSoilGridResolution;
    scene.vertices.reserve((triangles.size() + soilFacetCount) * 3U);
    uint32_t leafFacetCount = 0;
    for (size_t triangleIndex = 0; triangleIndex < triangles.size(); ++triangleIndex) {
        const auto& triangle = triangles[triangleIndex];
        const uint32_t materialIndex =
            hasTerrain && triangleIndex >= terrainTriangleStart ? 1U : 0U;
        if (appendTriangle(scene, positions[triangle[0]], positions[triangle[1]],
                           positions[triangle[2]], materialIndex) &&
            (!hasTerrain || triangleIndex < terrainTriangleStart)) {
            ++leafFacetCount;
        }
    }
    scene.leafFacetCount = hasTerrain ? leafFacetCount : scene.facetCount();

    // 步骤 4：无地形时，在包围盒四周各外扩 0.5×最大水平边长，用
    // kStandaloneSoilGridResolution² 个方格（每格 2 个三角形）铺一块地面，y = -0.01。
    float x0 = minimum[0];
    float x1 = maximum[0];
    float z0 = minimum[2];
    float z1 = maximum[2];
    if (!hasTerrain) {
        const float margin = 0.5f * std::max(maximum[0] - minimum[0],
                                             maximum[2] - minimum[2]);
        x0 -= margin;
        x1 += margin;
        z0 -= margin;
        z1 += margin;
        for (uint32_t row = 0; row < kStandaloneSoilGridResolution; ++row) {
            const float za = z0 + (z1 - z0) * static_cast<float>(row) /
                                      static_cast<float>(kStandaloneSoilGridResolution);
            const float zb = z0 + (z1 - z0) * static_cast<float>(row + 1U) /
                                      static_cast<float>(kStandaloneSoilGridResolution);
            for (uint32_t column = 0; column < kStandaloneSoilGridResolution; ++column) {
                const float xa = x0 + (x1 - x0) * static_cast<float>(column) /
                                          static_cast<float>(kStandaloneSoilGridResolution);
                const float xb = x0 + (x1 - x0) * static_cast<float>(column + 1U) /
                                          static_cast<float>(kStandaloneSoilGridResolution);
                appendTriangle(scene, {xa, -0.01f, za}, {xb, -0.01f, zb},
                               {xb, -0.01f, za}, 1U);
                appendTriangle(scene, {xa, -0.01f, za}, {xa, -0.01f, zb},
                               {xb, -0.01f, zb}, 1U);
            }
        }
    }

    scene.minimum = {x0, hasTerrain ? 0.0f : -0.01f, z0};
    scene.maximum = {x1, maximum[1] - minimum[1], z1};
    return scene;
}

// 作用：生成 kSkyDirectionCount（40）个上半球方向，供建可见性图使用。
// 方法：黄金角螺旋，y = (i+0.5)/N 在高度上均匀（近似等立体角分布）。
std::vector<facetvk::Direction> makeHemisphereDirections()
{
    constexpr float goldenAngle = 2.39996322972865332f;
    std::vector<facetvk::Direction> directions;
    directions.reserve(kSkyDirectionCount);
    for (uint32_t i = 0; i < kSkyDirectionCount; ++i) {
        const float y = (static_cast<float>(i) + 0.5f) /
                        static_cast<float>(kSkyDirectionCount);
        const float radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float azimuth = goldenAngle * static_cast<float>(i);
        directions.push_back({radius * std::cos(azimuth), y,
                              radius * std::sin(azimuth)});
    }
    return directions;
}

// 作用：太阳天顶角/方位角（度）→ 指向太阳的单位向量（y 向上）。
facetvk::Direction sunDirection(const Scene& scene)
{
    const float zenith = scene.sunZenith * kPi / 180.0f;
    const float azimuth = scene.sunAzimuth * kPi / 180.0f;
    // SPA azimuth is clockwise from north: +X is north and +Z is east.
    return {std::sin(zenith) * std::cos(azimuth), std::cos(zenith),
            std::sin(zenith) * std::sin(azimuth)};
}

// FacetRT thermal-band solar sources in the thermal solver's radiance units
// (W m-2 sr-1 um-1, i.e. irradiance / pi): [0] beam normal to the sun, used
// like the optical beam (sunlit part x cos), [1] scattered sky added to the
// Planck sky. The FacetRT illumination split (beamNormalIrradiance,
// skyDiffuseIrradiance, from the light settings or the historical defaults)
// is scaled to the horizontal shortwave total: light.shortwaveIrradiance when
// set, otherwise the historical 700 cos + 50 W m-2. Exactly 0 outside
// 2.5-6 um and with the sun at or below the horizon.
std::array<float, 2> facetRtThermalSolar(const Scene& scene, float wavelength)
{
    const float shape = mwirSolarShape(wavelength);
    const float cosine = sunDirection(scene).y;
    if (!(shape > 0.0f) || !(cosine > 0.0f)) return {0.0f, 0.0f};
    const float total = scene.shortwaveIrradiance > 0.0f
        ? scene.shortwaveIrradiance
        : kBeamNormalIrradiance * cosine + kSkyDiffuseIrradiance * 0.5f;
    const float reference =
        scene.beamNormalIrradiance * cosine + scene.skyDiffuseIrradiance;
    if (!(reference > 1.0e-6f)) return {0.0f, 0.0f};
    const float scale = total / reference * shape / kPi;
    return {scale * scene.beamNormalIrradiance,
            scale * scene.skyDiffuseIrradiance * kMwirSkySolarRatio};
}

// 作用：最简宽带光学参数：物体 ρ=0.10、τ=0.05，地面 ρ=0.20、τ=0；
// directIrradiance = 受光比例 × max(0, n·s) × 法向直射辐照（W m-2）。
// side 的法向：偶数侧为几何法向，奇数侧取反。
std::vector<facetvk::SurfaceOptics> makeOptics(const Scene& scene,
                                                const std::vector<float>& sunlit)
{
    const facetvk::Direction direction = sunDirection(scene);
    const Vec3 sun{direction.x, direction.y, direction.z};
    std::vector<facetvk::SurfaceOptics> optics(scene.facetCount() * 2U);
    for (uint32_t facet = 0; facet < scene.facetCount(); ++facet) {
        const bool leaf = facet < scene.leafFacetCount;
        const facetvk::Vertex& vertex = scene.vertices[facet * 3U];
        const Vec3 normal{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
        for (uint32_t localSide = 0; localSide < 2U; ++localSide) {
            const uint32_t side = facet * 2U + localSide;
            const float sign = localSide == 0U ? 1.0f : -1.0f;
            const Vec3 sideNormal = multiply(normal, sign);
            optics[side].reflectance = leaf ? 0.10f : 0.20f;
            optics[side].transmittance = leaf ? 0.05f : 0.0f;
            optics[side].directIrradiance =
                sunlit[side] * std::max(0.0f, dot(sideNormal, sun)) *
                scene.beamNormalIrradiance;
        }
    }
    return optics;
}

// 作用：Planck 黑体光谱辐亮度，单位 W m-2 sr-1 um-1。
// 输入：波长（>50 视为 nm，否则 um）、温度 K（下限 1 K）。
// 常数：c1 = 2hc² = 1.19104e8 W um4 m-2 sr-1，c2 = hc/k = 14388.29 um K。
float planckRadiance(float wavelengthNanometers, float temperature)
{
    constexpr double c1 = 1.1910439340652e8;
    constexpr double c2 = 14388.291040407;
    const double wavelengthMicrometers = wavelengthNanometers > 50.0f
        ? static_cast<double>(wavelengthNanometers) / 1000.0
        : static_cast<double>(wavelengthNanometers);
    const double exponent = c2 /
        (std::max(1.0, static_cast<double>(temperature)) *
         std::max(1.0e-6, wavelengthMicrometers));
    return static_cast<float>(c1 /
        (std::pow(wavelengthMicrometers, 5.0) * std::expm1(exponent)));
}

// 作用：取逐波段数组的第 band 项；索引超出时取最后一项，数组为空时返回 fallback。
float bandValue(const std::vector<float>& values, size_t band, float fallback)
{
    if (values.empty()) {
        return fallback;
    }
    return values[std::min(band, values.size() - 1U)];
}

// Irradiance-weighted mean over the trapezoid window [first, last] so a
// window absorbs what its samples absorb (a centre sample misses the red edge).
float windowBandValue(const std::vector<float>& values, size_t firstSample,
                      size_t lastSample, const std::vector<float>& weight,
                      float fallback)
{
    if (values.empty()) return fallback;
    double weighted = 0.0;
    double weightSum = 0.0;
    for (size_t sample = firstSample; sample <= lastSample; ++sample) {
        const double edge = sample == firstSample || sample == lastSample ? 0.5 : 1.0;
        const double w = sample < weight.size()
            ? edge * std::max(0.0f, weight[sample]) : 0.0;
        weighted += w * bandValue(values, sample, fallback);
        weightSum += w;
    }
    return weightSum > 0.0
        ? static_cast<float>(weighted / weightSum)
        : bandValue(values, (firstSample + lastSample) / 2U, fallback);
}

// 模块：背景（非植被地表，backgroundHeterogeneity = Hapke）的 Hapke 双向反射模型。
// facetHapkeH：Hapke 多次散射 H 函数的近似 (1+2μ)/(1+2μγ)，γ = sqrt(1-w)，w 为单次散射反照率。
// facetHapkeKernel：w/(4π)·μ0/(μ0+μ)·[1 + B(g) + H(μ0)H(μ) - 1]，相函数取各向同性，
// B(g) 为反向散射（opposition）峰，幅度 1、宽度 0.1，g 为相位角（rad）；μ0、μ 分别为
// 入射与出射方向与法向夹角的余弦。
float facetHapkeH(float mu, float singleScatteringAlbedo)
{
    const float gamma = std::sqrt(std::max(1.0f - singleScatteringAlbedo, 1.0e-6f));
    return (1.0f + 2.0f * mu) / (1.0f + 2.0f * mu * gamma);
}

float facetHapkeKernel(float mu0, float mu, float phaseAngle,
                       float singleScatteringAlbedo)
{
    constexpr float oppositionAmplitude = 1.0f;
    constexpr float oppositionWidth = 0.1f;
    const float opposition = oppositionAmplitude /
        (1.0f + std::tan(0.5f * phaseAngle) / oppositionWidth);
    const float multiple = facetHapkeH(mu0, singleScatteringAlbedo) *
        facetHapkeH(mu, singleScatteringAlbedo) - 1.0f;
    return singleScatteringAlbedo / (4.0f * kPi) *
        mu0 / std::max(mu0 + mu, 1.0e-5f) * (1.0f + opposition + multiple);
}

// Full-strength (strength 1) Hapke reflectance factor of the heterogeneous
// background, clamped to [0,1]; `base` is the already clamped Lambert value.
float hapkeClampedFactor(float base, float mu0, float mu, float phaseAngle)
{
    // 由 Lambert 基准反射率 base 反解单次散射反照率：base 取作半无限介质的法向反射率
    // r0 = (1-γ)/(1+γ)，即 w = 4·base/(1+base)²；限幅避免 γ 趋于 0 或 1 时的奇异。
    const float singleScatteringAlbedo = std::clamp(
        4.0f * base / std::pow(1.0f + base, 2.0f), 1.0e-5f, 0.999f);
    const float directional = facetHapkeKernel(
        mu0, mu, phaseAngle, singleScatteringAlbedo);
    const float referenceH = facetHapkeH(1.0f, singleScatteringAlbedo);
    const float reference = singleScatteringAlbedo / (8.0f * kPi) *
        referenceH * referenceH;
    // Hapke's r is per beam irradiance normal to the beam; direct irradiance
    // is already projected onto the facet, so use the BRF r/mu0.
    return std::clamp(
        base * directional / mu0 / std::max(reference, 1.0e-6f), 0.0f, 1.0f);
}

// Raw (unclamped, unnormalized) Hapke directional emission shape of the
// thermal background, emission angle cosine mu; see Emissivity_Hapke.
float infraredHapkeShape(float base, float mu)
{
    const float singleScatteringAlbedo = std::clamp(
        1.0f - base * base, 1.0e-5f, 0.999f);
    const float root = std::sqrt(1.0f - singleScatteringAlbedo);
    return root * (1.0f + 2.0f * mu / 1.5f) /
        (1.0f + 2.0f * mu / 1.5f * root);
}

// Former image factor, normalized at nadir. Its hemispherical albedo exceeds
// the Lambert value; it is kept only for Hapke-off bitwise compatibility of
// the radiosity solve (strength 0) and documents the former convention.
float backgroundHapkeReflectance(float reflectance, Vec3 incoming, Vec3 outgoing,
                                 const Vec3& surfaceNormal, float strength)
{
    const Vec3 normal = normalized(surfaceNormal);
    incoming = normalized(incoming);
    outgoing = normalized(outgoing);
    if (dot(incoming, normal) < 0.0f) incoming = multiply(incoming, -1.0f);
    if (dot(outgoing, normal) < 0.0f) outgoing = multiply(outgoing, -1.0f);
    const float mu0 = std::max(dot(normal, incoming), 1.0e-4f);
    const float mu = std::max(dot(normal, outgoing), 1.0e-4f);
    const float phaseAngle = std::acos(std::clamp(dot(incoming, outgoing), -1.0f, 1.0f));
    const float base = std::clamp(reflectance, 0.0f, 0.999f);
    const float hapke = hapkeClampedFactor(base, mu0, mu, phaseAngle);
    return base + (hapke - base) * std::clamp(strength, 0.0f, 1.0f);
}

// Image products observe the background from the zenith (nadir sensor): the
// optical reflectance factor and the thermal directional emissivity both use
// this viewing direction, folded into the hemisphere of the receiving side.
// The engine receives no sensor direction; for oblique or perspective sensors
// the zenith-view value is an approximation.
const Vec3 kImageViewDirection{0.0f, 1.0f, 0.0f};

// 16-point Gauss-Legendre rule on [0,1], used for the hemispherical integrals.
struct UnitGaussLegendre {
    std::array<double, 16> node{}, weight{};
    UnitGaussLegendre()
    {
        constexpr int n = 16;
        for (int i = 0; i < n; ++i) {
            // 对第 i 个 Legendre 多项式根：用经验初值做牛顿迭代（最多 100 次，|步长| < 1e-15 停止），
            // 再把 [-1,1] 上的根映射到 [0,1]。
            double x = std::cos(3.14159265358979323846 * (i + 0.75) / (n + 0.5));
            double derivative = 1.0;
            for (int iteration = 0; iteration < 100; ++iteration) {
                double p0 = 1.0, p1 = x;
                for (int k = 2; k <= n; ++k) {
                    const double p2 = ((2.0 * k - 1.0) * x * p1 - (k - 1.0) * p0) / k;
                    p0 = p1; p1 = p2;
                }
                derivative = n * (x * p1 - p0) / (x * x - 1.0);
                const double step = p1 / derivative;
                x -= step;
                if (std::abs(step) < 1.0e-15) break;
            }
            node[size_t(i)] = 0.5 * (x + 1.0);
            weight[size_t(i)] = 1.0 / ((1.0 - x * x) * derivative * derivative);
        }
    }
};

const UnitGaussLegendre& unitGaussLegendre()
{
    static const UnitGaussLegendre rule;
    return rule;
}

// Directional-hemispherical albedo A(mu0) = (1/pi) Int BRF mu dOmega of the
// clamped full-strength Hapke factor, tabulated in Lambert base and mu0, and the
// bihemispherical albedo 2 Int A(mu0) mu0 dmu0 for diffuse illumination.
struct HapkeAlbedoTable {
    static constexpr int kBaseCount = 101;
    static constexpr int kMuCount = 65;
    static constexpr int kAzimuthCount = 32;
    std::vector<float> directional;
    std::vector<float> bihemispherical;
    // Thermal: hemispherical mean 2 Int shape(e, mu) mu dmu of the directional
    // emission shape, tabulated at e = i / (kBaseCount - 1).
    std::vector<float> thermalHemispherical;

    static float baseAt(int index) { return 0.999f * index / (kBaseCount - 1); }

    // 作用：方向-半球反照率 A(μ0) = (1/π)∫BRF·μ dΩ。出射天顶方向用 16 点 Gauss-Legendre，
    // 相对方位角用 32 点中点法（被积函数对称，只积 [0,π]）。
    static double albedo(float base, double mu0)
    {
        const auto& rule = unitGaussLegendre();
        mu0 = std::clamp(mu0, 1.0e-4, 1.0);
        const double sin0 = std::sqrt(std::max(0.0, 1.0 - mu0 * mu0));
        double sum = 0.0;
        for (size_t m = 0; m < rule.node.size(); ++m) {
            const double mu = rule.node[m];
            const double sinMu = std::sqrt(std::max(0.0, 1.0 - mu * mu));
            double azimuthal = 0.0;
            // The integrand is symmetric in relative azimuth: midpoints on [0,pi].
            for (int a = 0; a < kAzimuthCount; ++a) {
                const double phi = 3.14159265358979323846 * (a + 0.5) / kAzimuthCount;
                const double cosPhase = std::clamp(
                    mu0 * mu + sin0 * sinMu * std::cos(phi), -1.0, 1.0);
                azimuthal += hapkeClampedFactor(
                    base, static_cast<float>(std::max(mu0, 1.0e-4)),
                    static_cast<float>(std::max(mu, 1.0e-4)),
                    static_cast<float>(std::acos(cosPhase)));
            }
            sum += rule.weight[m] * mu * azimuthal / kAzimuthCount;
        }
        return 2.0 * sum;
    }

    // 构造：预计算 101 个 base × 65 个 μ0 的 A(μ0)、101 个 base 的双半球反照率，以及
    // 101 个发射率的热红外半球均值。开销较大，由 hapkeAlbedoTable() 以静态局部变量只算一次。
    HapkeAlbedoTable()
        : directional(size_t(kBaseCount) * kMuCount), bihemispherical(kBaseCount),
          thermalHemispherical(kBaseCount)
    {
        const auto& rule = unitGaussLegendre();
        for (int b = 0; b < kBaseCount; ++b) {
            for (int m = 0; m < kMuCount; ++m) {
                directional[size_t(b) * kMuCount + m] = static_cast<float>(
                    albedo(baseAt(b), double(m) / (kMuCount - 1)));
            }
            double sum = 0.0;
            for (size_t m = 0; m < rule.node.size(); ++m)
                sum += rule.weight[m] * rule.node[m] * albedo(baseAt(b), rule.node[m]);
            bihemispherical[size_t(b)] = static_cast<float>(2.0 * sum);
            double thermal = 0.0;
            const float emissivity = float(b) / float(kBaseCount - 1);
            for (size_t m = 0; m < rule.node.size(); ++m)
                thermal += rule.weight[m] * rule.node[m] *
                    infraredHapkeShape(emissivity, static_cast<float>(rule.node[m]));
            thermalHemispherical[size_t(b)] = static_cast<float>(2.0 * thermal);
        }
    }

    // GPU copy (spectral.comp, appended after the facet records): directional
    // [base][mu], bihemispherical [base], thermal hemispherical [emissivity].
    void append(std::vector<float>& out) const
    {
        out.insert(out.end(), directional.begin(), directional.end());
        out.insert(out.end(), bihemispherical.begin(), bihemispherical.end());
        out.insert(out.end(), thermalHemispherical.begin(), thermalHemispherical.end());
    }

    // sampleThermal / sampleDirectional / sampleBihemispherical：对上面的表做线性或双线性插值；
    // base 归一到 [0, 0.999]，μ0 归一到 [0, 1]。
    float sampleThermal(float emissivity) const
    {
        const float x = std::clamp(emissivity, 0.0f, 1.0f) * (kBaseCount - 1);
        const int b0 = std::min(static_cast<int>(x), kBaseCount - 2);
        const float fx = x - b0;
        return (1.0f - fx) * thermalHemispherical[size_t(b0)] +
            fx * thermalHemispherical[size_t(b0) + 1U];
    }

    float sampleDirectional(float base, float mu0) const
    {
        const float x = std::clamp(base / 0.999f, 0.0f, 1.0f) * (kBaseCount - 1);
        const float y = std::clamp(mu0, 0.0f, 1.0f) * (kMuCount - 1);
        const int b0 = std::min(static_cast<int>(x), kBaseCount - 2);
        const int m0 = std::min(static_cast<int>(y), kMuCount - 2);
        const float fx = x - b0, fy = y - m0;
        const auto at = [&](int b, int m) { return directional[size_t(b) * kMuCount + m]; };
        return (1.0f - fx) * ((1.0f - fy) * at(b0, m0) + fy * at(b0, m0 + 1)) +
               fx * ((1.0f - fy) * at(b0 + 1, m0) + fy * at(b0 + 1, m0 + 1));
    }

    float sampleBihemispherical(float base) const
    {
        const float x = std::clamp(base / 0.999f, 0.0f, 1.0f) * (kBaseCount - 1);
        const int b0 = std::min(static_cast<int>(x), kBaseCount - 2);
        const float fx = x - b0;
        return (1.0f - fx) * bihemispherical[size_t(b0)] + fx * bihemispherical[size_t(b0) + 1U];
    }
};

const HapkeAlbedoTable& hapkeAlbedoTable()
{
    static const HapkeAlbedoTable table;
    return table;
}

// Energy-balance reflectance of a background side. The Hapke factor above is
// normalized at nadir without the opposition peak, so its hemispherical albedo
// exceeds the Lambert value (about 1.3-2.5x for base 0.2); using the factor or
// its raw albedo would create energy. The energy balance therefore keeps the
// Lambert value as the white-sky (bihemispherical) albedo and only takes the
// sun-angle dependence from Hapke: diffuse light sees `base`, the direct beam
// base * A(mu0) / A_bihemispherical, weighted by the two incident fluxes.
// strength 0 returns the Lambert value unchanged.
float backgroundHapkeEnergyReflectance(float reflectance, float mu0,
                                       float directIrradiance,
                                       float diffuseIrradiance, float strength)
{
    const float base = std::clamp(reflectance, 0.0f, 0.999f);
    const float s = std::clamp(strength, 0.0f, 1.0f);
    if (!(s > 0.0f) || !(base > 0.0f)) return base;
    const HapkeAlbedoTable& table = hapkeAlbedoTable();
    const float direct = std::max(0.0f, directIrradiance);
    const float diffuse = std::max(0.0f, diffuseIrradiance);
    const float bihemispherical = std::max(table.sampleBihemispherical(base), 1.0e-6f);
    const float beamAlbedo = base * table.sampleDirectional(base, mu0) / bihemispherical;
    const float hapke = direct + diffuse > 0.0f
        ? (direct * beamAlbedo + diffuse * base) / (direct + diffuse)
        : base;
    return base + (std::clamp(hapke, 0.0f, 0.999f) - base) * s;
}

// 作用：热红外方向发射率因子 base·shape(cosine)/shape(1)，以天顶（μ=1）归一并限幅到 [0,1]。
// 与下面 infraredHapkeImageEmissivity 的“半球均值归一”新约定不同。
float infraredHapkeDirectionalFactor(float base, float cosine)
{
    const float singleScatteringAlbedo = std::clamp(
        1.0f - base * base, 1.0e-5f, 0.999f);
    const auto directional = [singleScatteringAlbedo](float mu) {
        const float root = std::sqrt(1.0f - singleScatteringAlbedo);
        return root * (1.0f + 2.0f * mu / 1.5f) /
            (1.0f + 2.0f * mu / 1.5f * root);
    };
    return std::clamp(
        base * directional(std::clamp(cosine, 1.0e-4f, 1.0f)) /
            std::max(directional(1.0f), 1.0e-6f),
        0.0f, 1.0f);
}

// Directional emissivity toward one viewing cosine (image products).
float infraredHapkeEmissivity(float baseEmissivity, float cosine,
                              float strength)
{
    const float base = std::clamp(baseEmissivity, 0.0f, 1.0f);
    const float hapke = infraredHapkeDirectionalFactor(base, cosine);
    return base + (hapke - base) * std::clamp(strength, 0.0f, 1.0f);
}

// Spectral (image) optics of a Hapke background side, strength > 0. The
// radiosity solve is hemispherical and follows the energy-balance convention:
// diffuse light (sky and every scene reflection) sees the white-sky albedo
// `diffuse` = Lambert base, the beam the directional-hemispherical albedo
// `beam` = base*A(mu0)/A_bh. The image (zenith view) uses the BRF normalized
// to the same albedo: beam factor*base/A_bh (its hemispherical integral is
// `beam`), diffuse light base*A(mu)/A_bh (the reflectance for isotropic
// incidence, by reciprocity; its hemispherical integral is base).
// spectral.comp backgroundHapke() mirrors this function.
struct BackgroundHapkeOptics {
    float diffuse{}, beam{}, imageBeam{}, imageDiffuse{};
};

BackgroundHapkeOptics backgroundHapkeSpectral(float reflectance, float mu0, float mu,
                                              float phaseAngle, float strength)
{
    const HapkeAlbedoTable& table = hapkeAlbedoTable();
    const float base = std::clamp(reflectance, 0.0f, 0.999f);
    const float s = std::clamp(strength, 0.0f, 1.0f);
    const float factor = hapkeClampedFactor(base, mu0, mu, phaseAngle);
    const float bihemispherical = std::max(table.sampleBihemispherical(base), 1.0e-6f);
    const float beamAlbedo = std::clamp(
        base * table.sampleDirectional(base, mu0) / bihemispherical, 0.0f, 0.999f);
    BackgroundHapkeOptics optics;
    optics.diffuse = base;
    optics.beam = base + (beamAlbedo - base) * s;
    optics.imageBeam = base + (factor * base / bihemispherical - base) * s;
    optics.imageDiffuse = base +
        (base * table.sampleDirectional(base, mu) / bihemispherical - base) * s;
    return optics;
}

// Image emissivity of a thermal Hapke background toward the zenith view: the
// directional shape normalized to its hemispherical mean, so the hemispherical
// emissivity stays the Lambert value used by the solve (formerly normalized at
// nadir, which lowered the hemispherical emissivity on every slope).
float infraredHapkeImageEmissivity(float baseEmissivity, float cosine, float strength)
{
    const float base = std::clamp(baseEmissivity, 0.0f, 1.0f);
    const float hapke = std::clamp(
        base * infraredHapkeShape(base, std::clamp(cosine, 1.0e-4f, 1.0f)) /
            std::max(hapkeAlbedoTable().sampleThermal(base), 1.0e-6f),
        0.0f, 1.0f);
    return base + (hapke - base) * std::clamp(strength, 0.0f, 1.0f);
}

// Image reflection of one side for the sunlit/shaded image states: the
// reflectance applied to the beam, and the factor applied to the solved
// diffuse part (reflected scene + sky light). beam < 0: same as the solve.
struct ImageReflection {
    float beam{-1.0f};
    float diffuseScale{1.0f};
};

// Geometry shared with prepareGpuSpectral(): the sun and the zenith view
// folded into the hemisphere of the facet normal.
struct BackgroundViewGeometry {
    float mu0{}, mu{}, phase{};
};

// 作用：把太阳方向与天顶视线翻折到面元法向所在半球后，求 mu0（太阳）、mu（视线）与法向
// 夹角的余弦（下限 1e-4），以及两者的相位角 phase（rad）。
BackgroundViewGeometry backgroundViewGeometry(const Vec3& rawNormal, const Vec3& sun)
{
    const Vec3 normal = normalized(rawNormal);
    Vec3 incoming = normalized(sun);
    if (dot(incoming, normal) < 0.0f) incoming = multiply(incoming, -1.0f);
    Vec3 outgoing = kImageViewDirection;
    if (dot(outgoing, normal) < 0.0f) outgoing = multiply(outgoing, -1.0f);
    BackgroundViewGeometry geometry;
    geometry.mu0 = std::max(dot(normal, incoming), 1.0e-4f);
    geometry.mu = std::max(dot(normal, outgoing), 1.0e-4f);
    geometry.phase = std::acos(std::clamp(dot(incoming, outgoing), -1.0f, 1.0f));
    return geometry;
}

// 作用：取面元所属材质参数；材质索引缺失或越界时按前 leafFacetCount 个面元为物体、
// 其余为地面，回退到默认物体/地面材质（兼容没有材质表的旧场景）。
const SurfaceParameters& surfaceForFacet(const Scene& scene, uint32_t facet)
{
    const uint32_t materialIndex =
        facet < scene.materialIndices.size()
        ? scene.materialIndices[facet]
        : std::numeric_limits<uint32_t>::max();
    if (materialIndex < scene.materials.size()) {
        return scene.materials[materialIndex];
    }
    static const SurfaceParameters objectFallback{};
    static const SurfaceParameters groundFallback{
        {0.20f}, {0.0f}, 0.05f, 0.0f, 305.0f, 295.0f};
    return facet < scene.leafFacetCount
        ? objectFallback : groundFallback;
}


// 作用：三角面元面积 0.5·|AB×AC|（场景长度单位的平方，通常 m2）。
float facetArea(const Scene& scene, uint32_t facet)
{
    const facetvk::Vertex& a = scene.vertices[facet * 3U];
    const facetvk::Vertex& b = scene.vertices[facet * 3U + 1U];
    const facetvk::Vertex& c = scene.vertices[facet * 3U + 2U];
    const Vec3 edgeA{b.position[0] - a.position[0],
                     b.position[1] - a.position[1],
                     b.position[2] - a.position[2]};
    const Vec3 edgeB{c.position[0] - a.position[0],
                     c.position[1] - a.position[1],
                     c.position[2] - a.position[2]};
    return 0.5f * std::sqrt(dot(cross(edgeA, edgeB), cross(edgeA, edgeB)));
}

// 作用：在面元重心处对反射率扰动纹理做世界坐标三平面采样（按 |法向| 加权，
// repeatSize 为纹理重复尺寸），返回零均值偏差；材质没有纹理时返回 0。
float sampledReflectanceVariance(const Scene& scene, uint32_t facet,
                                 const SurfaceParameters& surface)
{
    if (!surface.reflectanceTexture) return 0.0f;
    const facetvk::Vertex& a = scene.vertices[facet * 3U];
    const facetvk::Vertex& b = scene.vertices[facet * 3U + 1U];
    const facetvk::Vertex& c = scene.vertices[facet * 3U + 2U];
    const Vec3 center{
        (a.position[0] + b.position[0] + c.position[0]) / 3.0f,
        (a.position[1] + b.position[1] + c.position[1]) / 3.0f,
        (a.position[2] + b.position[2] + c.position[2]) / 3.0f};
    const Vec3 normal{std::abs(a.normal[0]), std::abs(a.normal[1]),
                      std::abs(a.normal[2])};
    return sampleReflectanceVarianceTriplanar(
        *surface.reflectanceTexture, center, normal,
        surface.reflectanceTextureRepeatSize);
}

// 作用：该材质允许的反射率缩放因子上限，保证 ρ·f + τ <= 1，即 f <= (1-τ)/ρ；
// 对 reflectance 与 energyReflectance 各波段取最小值，没有有效波段时为 +inf。
float maximumMeanPreservingFactor(const SurfaceParameters& surface)
{
    float maximumFactor = std::numeric_limits<float>::infinity();
    const auto includeBands = [&maximumFactor](
        const std::vector<float>& reflectance,
        const std::vector<float>& transmittance) {
        for (size_t band = 0; band < reflectance.size(); ++band) {
            const float base = std::clamp(reflectance[band], 0.0f, 1.0f);
            if (!(base > 1.0e-6f)) continue;
            const float tau = transmittance.empty() ? 0.0f : std::clamp(
                transmittance[std::min(band, transmittance.size() - 1U)],
                0.0f, 1.0f - base);
            maximumFactor = std::min(maximumFactor, (1.0f - tau) / base);
        }
    };
    includeBands(surface.reflectance, surface.transmittance);
    includeBands(surface.energyReflectance, surface.energyTransmittance);
    return maximumFactor;
}

// 模块：反射率纹理扰动因子。每个面元的因子 f = 1 + scale × 偏差，其中偏差先在材质内按面积
// 去均值，所以每种材质的面积加权平均因子保持为 1（不改变平均反射率）。
// 输出：scene.reflectanceTextureFactors（无纹理或强度为 0 的面元为 1），并向日志打印
// 各材质因子的均值、变异系数与范围。
// scale 由 physicalTextureScale 给出，受 maximumMeanPreservingFactor 限制。
void finalizeReflectanceTextureFactors(Scene& scene)
{
    const uint32_t facetCount = scene.facetCount();
    scene.reflectanceTextureFactors.assign(facetCount, 1.0f);
    if (facetCount == 0U || scene.materials.empty()) return;

    struct MaterialStatistics {
        double area{};
        double areaWeightedSample{};
        TextureDeviationStatistics deviation{};
        float scale{};
        double areaWeightedFactor{};
        double areaWeightedFactorSquare{};
        float minimumFactor{std::numeric_limits<float>::infinity()};
        float maximumFactor{std::numeric_limits<float>::lowest()};
    };
    std::vector<MaterialStatistics> statistics(scene.materials.size());
    std::vector<float> samples(facetCount, 0.0f);
    std::vector<float> areas(facetCount, 0.0f);
    // 步骤 1：逐面元采样偏差（只处理有纹理且 strength 或 relativeStd > 0 的材质），
    // 并按材质累计面积与面积加权和。
    for (uint32_t facet = 0; facet < facetCount; ++facet) {
        if (facet >= scene.materialIndices.size()) continue;
        const uint32_t materialIndex = scene.materialIndices[facet];
        if (materialIndex >= scene.materials.size()) continue;
        const SurfaceParameters& surface = scene.materials[materialIndex];
        if (!surface.reflectanceTexture ||
            !(surface.reflectanceTextureStrength > 0.0f ||
              surface.reflectanceTextureRelativeStd > 0.0f)) continue;
        areas[facet] = facetArea(scene, facet);
        samples[facet] = sampledReflectanceVariance(scene, facet, surface);
        statistics[materialIndex].area += areas[facet];
        statistics[materialIndex].areaWeightedSample +=
            static_cast<double>(areas[facet]) * samples[facet];
    }
    // 步骤 2：减去材质面积加权均值得到零均值偏差，并累计面积加权平方和与偏差极值，
    // 供 physicalTextureScale 换算缩放系数。
    for (uint32_t facet = 0; facet < facetCount; ++facet) {
        if (facet >= scene.materialIndices.size() || !(areas[facet] > 0.0f)) continue;
        const uint32_t materialIndex = scene.materialIndices[facet];
        MaterialStatistics& stats = statistics[materialIndex];
        const float mean = stats.area > 0.0
            ? static_cast<float>(stats.areaWeightedSample / stats.area) : 0.0f;
        const float deviation = samples[facet] - mean;
        samples[facet] = deviation;
        stats.deviation.area += areas[facet];
        stats.deviation.areaWeightedSquare +=
            static_cast<double>(areas[facet]) * deviation * deviation;
        stats.deviation.minimumDeviation = std::min(stats.deviation.minimumDeviation, deviation);
        stats.deviation.maximumDeviation = std::max(stats.deviation.maximumDeviation, deviation);
    }
    // 步骤 3：每种材质由 strength / relativeStd 求缩放系数 scale（受反射率上限约束）。
    for (size_t materialIndex = 0;
         materialIndex < scene.materials.size(); ++materialIndex) {
        MaterialStatistics& stats = statistics[materialIndex];
        if (!(stats.area > 0.0)) continue;
        const SurfaceParameters& surface = scene.materials[materialIndex];
        stats.scale = physicalTextureScale(
            surface.reflectanceTextureStrength,
            surface.reflectanceTextureRelativeStd, stats.deviation,
            maximumMeanPreservingFactor(surface));
    }
    // 步骤 4：因子 = 1 + scale × 偏差，写入 scene.reflectanceTextureFactors，同时统计均值与范围。
    for (uint32_t facet = 0; facet < facetCount; ++facet) {
        if (facet >= scene.materialIndices.size() || !(areas[facet] > 0.0f)) continue;
        const uint32_t materialIndex = scene.materialIndices[facet];
        const float factor =
            1.0f + statistics[materialIndex].scale * samples[facet];
        scene.reflectanceTextureFactors[facet] = factor;
        MaterialStatistics& stats = statistics[materialIndex];
        stats.areaWeightedFactor += static_cast<double>(areas[facet]) * factor;
        stats.areaWeightedFactorSquare += static_cast<double>(areas[facet]) * factor * factor;
        stats.minimumFactor = std::min(stats.minimumFactor, factor);
        stats.maximumFactor = std::max(stats.maximumFactor, factor);
    }
    // 步骤 5：逐材质打印因子的面积加权均值、变异系数 cv 与取值范围，便于核对纹理强度。
    for (size_t materialIndex = 0;
         materialIndex < statistics.size(); ++materialIndex) {
        const MaterialStatistics& stats = statistics[materialIndex];
        if (!(stats.area > 0.0)) continue;
        const double mean = stats.areaWeightedFactor / stats.area;
        std::cout << "facetrt: physical texture material " << materialIndex
                  << ", area-weighted factor mean=" << mean
                  << ", cv=" << std::sqrt(std::max(0.0, stats.areaWeightedFactorSquare / stats.area - mean * mean))
                  << ", range=[" << stats.minimumFactor << ','
                  << stats.maximumFactor << "]" << std::endl;
    }
}

float reflectanceTextureFactor(const Scene& scene, uint32_t facet)
{
    return facet < scene.reflectanceTextureFactors.size()
        ? scene.reflectanceTextureFactors[facet] : 1.0f;
}

// 作用：饱和水汽压 e_s（hPa），Tetens 型公式 6.107·10^(7.5·T/(237.3+T))，T 为摄氏度。
// 输入：温度 K，内部限幅到 [-80, 80] °C；saturationVaporPressureSlope 为其对 T 的解析导数
// （hPa K-1，2.3026 = ln10）。
float saturationVaporPressure(float temperature)
{
    const float celsius = std::clamp(temperature - 273.15f, -80.0f, 80.0f);
    return 6.107f * std::pow(10.0f, 7.5f * celsius / (237.3f + celsius));
}

float saturationVaporPressureSlope(float temperature)
{
    const float celsius = std::clamp(temperature - 273.15f, -80.0f, 80.0f);
    const float saturation = saturationVaporPressure(temperature);
    return saturation * 2.3026f * 7.5f * 237.3f /
        std::pow(237.3f + celsius, 2.0f);
}

// 作用：潜热通量 LE = ρ_air·λ·0.622·max(0, e_s(T)-e)/p / (ra + rs)，单位 W m-2。
// 输入：温度 K、空气密度 kg m-3、气压 hPa（下限 100）、水汽压 hPa、
// 空气动力阻抗 ra 与表面阻抗 rs（s m-1，两者之和下限 1）。
// λ = (2.501 - 0.002361·T_C)×1e6 J kg-1。
float latentHeatFlux(float temperature,
                     float airDensity,
                     float airPressure,
                     float vaporPressure,
                     float aerodynamicResistance,
                     float surfaceResistance)
{
    const float celsius = temperature - 273.15f;
    const float vaporizationHeat =
        (2.501f - 0.002361f * celsius) * 1.0e6f;
    const float humidityDeficit = 0.622f *
        std::max(0.0f, saturationVaporPressure(temperature) - vaporPressure) /
        std::max(100.0f, airPressure);
    // DEFERRED[低] 物理: 饱和差被截断为 >=0，夜间表面低于露点时的凝结（负潜热）被忽略，冷表面能量平衡缺少一个热源；导数函数也同样截断。建议：允许负值（或至少对土壤/水面允许凝结），或在诊断中记录被截断的面元数。
    return airDensity * vaporizationHeat * humidityDeficit /
        std::max(1.0f, aerodynamicResistance + surfaceResistance);
}

// 作用：latentHeatFlux 对温度的导数（W m-2 K-1），包含饱和水汽压斜率项和 λ 随温度变化项
// （-2361 J kg-1 K-1）；结果下限 0，用于能量残差对温度的斜率。
float latentHeatFluxDerivative(float temperature,
                               float airDensity,
                               float airPressure,
                               float vaporPressure,
                               float aerodynamicResistance,
                               float surfaceResistance)
{
    const float celsius = temperature - 273.15f;
    const float saturation = saturationVaporPressure(temperature);
    const float slope = saturationVaporPressureSlope(temperature);
    const float vaporizationHeat =
        (2.501f - 0.002361f * celsius) * 1.0e6f;
    const float humidityDeficit = 0.622f *
        std::max(0.0f, saturation - vaporPressure) /
        std::max(100.0f, airPressure);
    const float derivative = airDensity /
        std::max(1.0f, aerodynamicResistance + surfaceResistance) *
        (-2361.0f * humidityDeficit +
         vaporizationHeat * 0.622f * slope /
             std::max(100.0f, airPressure));
    return std::max(0.0f, derivative);
}

using CanopyAerodynamics = facetphysics::Aerodynamics;
using facetphysics::aerodynamics;

// Smaller (design <= 0) or larger root of a*x^2 + b*x + c, cancellation-free.
double quadraticRoot(double a, double b, double c, bool larger)
{
    if (a == 0.0) return b != 0.0 ? -c / b : std::numeric_limits<double>::quiet_NaN();
    const double discriminant = b * b - 4.0 * a * c;
    if (discriminant < -1.0e-12 * (b * b + std::abs(4.0 * a * c))) return std::numeric_limits<double>::quiet_NaN();
    const double radical = std::sqrt(std::max(discriminant, 0.0));
    if (radical == 0.0) return -b / (2.0 * a);
    const double q = -0.5 * (b + (b < 0.0 ? -radical : radical));
    return larger == (b < 0.0) ? q / a : c / q;
}

// 作用：整叶气孔阻抗 rs（s m-1）：Collatz 型 C3/C4 光合 + Ball-Berry 气孔导度，Ci 自洽求解。
// 输入：leaf 生理参数、叶温 K、吸收 PAR（单位待核对）、水汽压 hPa、气压 hPa、
// 冠层相对深度（限幅 [-1, 0]，用于 vcmax25 随深度衰减）、环境 CO2 ppm。
// 输出：rs，限幅在 [1, 625000] s m-1（625000 为气孔关闭时的上限）。
// Whole-leaf Collatz resistance [s m-1]. A(Ci), Ball-Berry and CO2 diffusion
// must use the SAME Ci, including the nonzero Ball-Berry intercept.
float leafStomatalResistance(const LeafBiochemistry& leaf, float temperature,
                             float absorbedPar, float vaporPressure, float pressure,
                             float relativeDepth, float co2Ppm)
{
    // 步骤 1：温度与深度修正的光合参数：vcmax25 随深度指数衰减；qt 为 0.1·(T-298.15)·修正系数，
    // 各酶动力学参数（kc、ko、kpep、vcmax、比选择性、暗呼吸 rd）按 Q10 型因子随温度变化；
    // je 为光限制的电子传输速率；gammaStar 为 C3 的 CO2 补偿点。
    constexpr double closed = 625000.0, o2Permil = 209.0;
    const double tc = temperature;
    const bool c4 = leaf.type > 3.5f;
    const double vcmax25 = leaf.vcmax25 * std::exp(leaf.kV * std::clamp(relativeDepth, -1.0f, 0.0f));
    // Keep the configured temperature-response option unchanged.
    const double ppm2bar = 1.0e-6 * (pressure * 1.0e-3);
    const double cs = co2Ppm * ppm2bar;
    const double o = c4 ? 0.0 : o2Permil * 1.0e-3 * (pressure * 1.0e-3);
    const double qt = 0.1 * (tc - 298.15) * leaf.temperatureCorrection;
    const double th = 1.0 + leaf.temperatureCorrection * std::exp(leaf.tparam[1] * (tc - leaf.tparam[3]));
    const double tl = 1.0 + leaf.temperatureCorrection * std::exp(leaf.tparam[0] * (leaf.tparam[2] - tc));
    const double kc = 350.0e-6 * std::exp(std::log(2.1) * qt);
    const double ko = 450.0e-3 * std::exp(std::log(1.2) * qt);
    const double kpep = vcmax25 / 56.0 * 1.0e6 * std::exp(std::log(1.8) * qt);
    const double vcmax = vcmax25 * std::exp(std::log(2.1) * qt) / (c4 ? th * tl : th) * leaf.stressFactor;
    const double specificity = 2600.0 * std::exp(std::log(0.75) * qt);
    const double rd = leaf.rdParam * vcmax25 * std::exp(std::log(1.8) * qt) /
        (1.0 + std::exp(1.3 * (tc - leaf.tparam[4])));
    const double kd = std::max(0.8738, 0.0301 * (tc - 273.15) + 0.0773);
    const double je = 0.5 * 4.0 / (0.05 + kd + 4.0) * std::max(0.0f, absorbedPar);
    const double gammaStar = 0.5 * o / specificity;
    const double relativeHumidity = std::clamp(
        static_cast<double>(vaporPressure) / saturationVaporPressure(temperature), 0.0, 1.0);
    const double product = leaf.ballBerrySlope * relativeHumidity;
    const double minCiPpm = (c4 ? 0.1 : 0.3) * co2Ppm;
    const double intercept = std::max(0.0f, leaf.ballBerry0);
    // 步骤 2：净同化 A(Ci) = 总同化 - rd。C4：vc = vcmax、vs = kpep·Ci、ve = je/6；
    // C3：Rubisco 限制 vc、光限制 ve、产物限制 vs；总同化由两次非直角双曲线（曲率 0.8、0.98）
    // 平滑取最小。无光、无 Ball-Berry 斜率或胁迫因子 <= 0 时总同化为 0。
    const auto assimilation = [&](double ciPpm) {
        const double ci = ciPpm * ppm2bar;
        double vc, ve, vs;
        if (c4) { vc = vcmax; vs = kpep * ci; ve = je / 6.0; }
        else {
            const double michaelis = kc * (1.0 + o / ko);
            vc = michaelis + ci > 0.0 ? vcmax * (ci - gammaStar) / (michaelis + ci) : 0.0;
            vs = vcmax25 / 2.0 * std::exp(std::log(1.8) * qt);
            ve = ci + 2.0 * gammaStar > 0.0 ? je * (ci - gammaStar) / (ci + 2.0 * gammaStar) / 5.0 : 0.0;
        }
        const double v = quadraticRoot(0.8, -(vc + ve), vc * ve, vc < 0.0);
        double ag = quadraticRoot(0.98, -(v + vs), v * vs, false);
        if (absorbedPar <= 0.0f || product <= 0.0 || leaf.stressFactor <= 0.0f) ag = 0.0;
        return ag - rd;
    };
    // 步骤 3：求 Ci 使 Ci = Cs - 1.6·A/gs，其中 gs = g0 + m·RH·A/Cs（Ball-Berry）。
    // 无截距 g0 时有解析闭式 Ci = Cs·(1 - 1.6/(m·RH))（下限 minCiPpm）；有截距时用二分法
    // （最多 24 次，容差 0.01 ppm）。
    double molarConductance = intercept;
    if (co2Ppm > 0.0 && assimilation(co2Ppm) > 0.0) {
        double ci = product > 0.0 ? std::max(minCiPpm, co2Ppm * (1.0 - 1.6 / product)) : minCiPpm;
        if (intercept > 0.0) {
            double lower = minCiPpm, upper = co2Ppm;
            // Bounded scalar Ci solve, ppm tolerance; outer rs/T tolerances are unchanged.
            for (int i = 0; i < 24; ++i) {
                ci = .5 * (lower + upper);
                const double a = assimilation(ci);
                const double gs = std::max(intercept, intercept + product * a / co2Ppm);
                const double target = std::max(minCiPpm, co2Ppm - 1.6 * a / gs);
                if (std::abs(ci - target) <= .01 || upper - lower <= .01) break;
                if (ci > target) upper = ci; else lower = ci;
            }
        }
        const double a = assimilation(ci);
        // Preserve the existing minimum-Ci safeguard as a conductance floor.
        // Never add g0 again: it already participated in the Ci closure.
        molarConductance = std::max(intercept, intercept + product * a / co2Ppm);
        molarConductance = std::max(molarConductance, 1.6 * a / (co2Ppm - minCiPpm));
    }
    // 步骤 4：摩尔导度 → 速度导度：g[m s-1] = g[mol m-2 s-1]·R·T/P（P 由 hPa 换成 Pa），
    // 阻抗取倒数并限幅；导度非正时按关闭处理。
    // Both intercept and assimilation terms use the local molar volume.
    const double conductance = molarConductance * 8.314 * tc / (pressure * 100.0);
    return conductance > 0.0
        ? static_cast<float>(std::clamp(1.0 / conductance, 1.0, closed)) : static_cast<float>(closed);
}

// 作用：土壤储热系数 = 2·sqrt(c·ρ·k)/sqrt(π·dTime)，单位 W m-2 K-1，其中 sqrt(c·ρ·k) 为热惯量；
// dTime 为时间步长 s（下限 1 s）。用于把土壤导热通量对地表温度线性化（推导待核对）。
float soilStorageCoefficient(const SurfaceParameters& surface, float dTime)
{
    const float thermalInertia = std::sqrt(std::max(
        0.0f, surface.specificHeat * surface.density *
                  surface.thermalConductivity));
    return 2.0f * thermalInertia /
        std::sqrt(kPi * std::max(1.0f, dTime));
}

// Soil method 1 history older than the explicit nodes (control.soilInertiaHistory
// = "full"), the double-precision twin of soilMemoryMode in shader/functions.glsl:
// tau^-1/2 = pi^-1/2 int s^-1/2 e^(-s tau) ds as a trapezoid sum over
// kSoilMemoryModes rates, geometric from 1/(10 years) to 30/(nodes dt), half
// weights at both ends, the part below s_min lumped into the slowest mode.
// Mode j keeps S_j = sum_{k > nodes} dT_k exp(-s_j (k - nodes - 1) dt) and is
// updated S_j <- decay_j S_j + (T_{n-7} - T_{n-8}); weight_j S_j adds to the
// normalised sum sum_k dT_k (sqrt(k) - sqrt(k-1)) of the flux evaluation.
constexpr size_t kSoilMemoryModes = 12U;
struct SoilMemoryModes {
    std::array<double, kSoilMemoryModes> weight{};
    std::array<double, kSoilMemoryModes> decay{};
};
SoilMemoryModes soilMemoryModes(float dTime, size_t explicitNodes)
{
    SoilMemoryModes modes;
    const double dt = std::max(1.0, static_cast<double>(dTime));
    const double logMin = -std::log(3.15e8);
    const double logMax = std::log(30.0 / (static_cast<double>(explicitNodes) * dt));
    const double h = (logMax - logMin) / static_cast<double>(kSoilMemoryModes - 1U);
    for (size_t j = 0; j < kSoilMemoryModes; ++j) {
        const double s = std::exp(logMin + h * static_cast<double>(j));
        const double coefficient = j == 0 ? 0.5 * h + 2.0 : (j == kSoilMemoryModes - 1U ? 0.5 * h : h);
        const double x = s * dt;
        modes.decay[j] = std::exp(-x);
        modes.weight[j] = coefficient * 0.5 / std::sqrt(kPi * s) * -std::expm1(-x) *
            std::exp(-static_cast<double>(explicitNodes) * x) / std::sqrt(dt);
    }
    return modes;
}

// 作用：把材质的逐波段值压成一个宽带短波值（材质没有 1 nm 的 energy* 数据时的回退）。
// 1 个值直接使用；2 个值视为可见光/近红外两个宽带，权重 0.43/0.57；3 个以上假设在
// 400–2400 nm 等距，以 5778 K 黑体（近似太阳光谱）为权重做加权平均；非有限值跳过或取 fallback。
float shortwaveWeightedBandValue(
    const std::vector<float>& values, float fallback)
{
    if (values.empty()) {
        return fallback;
    }
    if (values.size() == 1U) {
        return std::isfinite(values.front()) ? values.front() : fallback;
    }
    // Two-value materials represent visible and NIR/SWIR broad bands.
    if (values.size() == 2U) {
        const float visible = std::isfinite(values[0]) ? values[0] : fallback;
        const float infrared = std::isfinite(values[1]) ? values[1] : visible;
        return 0.43f * visible + 0.57f * infrared;
    }

    double weightedValue = 0.0;
    double weightSum = 0.0;
    // DEFERRED[中] 物理: 这里假设 values 在 400–2400 nm 上等距分布，但传入的 surface.reflectance 是按传感器 controlBand 波长解析的（可能只有几个任意波段），把波段序号映射为 400+2000*i/(n-1) nm 会给出错误的太阳光谱加权宽带反射率；建议：传入真实波长数组按其插值加权，或始终使用 energyReflectance（1 nm 网格）。
    for (size_t band = 0; band < values.size(); ++band) {
        if (!std::isfinite(values[band])) {
            continue;
        }
        const float wavelength = 400.0f + 2000.0f *
            static_cast<float>(band) /
            static_cast<float>(values.size() - 1U);
        const double weight = std::max(
            0.0f, planckRadiance(wavelength, 5778.0f));
        weightedValue += weight * static_cast<double>(values[band]);
        weightSum += weight;
    }
    return weightSum > 0.0
        ? static_cast<float>(weightedValue / weightSum)
        : fallback;
}

// 模块：FacetEB 一个短波能量窗口（1 nm 样本区间 [firstSample, lastSample]，spectralWeight
// 为窗口内太阳光谱权重）的面元光学参数。
// 输入：每侧受光比例 sunlit、法向直射辐照 directNormalIrradiance 与水平散射 diffuseIrradiance
// （W m-2）。输出：每侧 reflectance / transmittance / directIrradiance。
// 说明：窗口平均 ρ、τ 只与材质有关，先按材质算一次；反射率乘纹理因子并限幅，τ <= 1-ρ；
// 背景面元用 backgroundHapkeEnergyReflectance 保证半球能量守恒，物体面元不用 Hapke。
std::vector<facetvk::SurfaceOptics> makeEnergyShortwaveOptics(
    const Scene& scene,
    const std::vector<float>& sunlit,
    float directNormalIrradiance,
    float diffuseIrradiance,
    size_t firstSample,
    size_t lastSample,
    const std::vector<float>& spectralWeight)
{
    const facetvk::Direction direction = sunDirection(scene);
    const Vec3 sun{direction.x, direction.y, direction.z};
    std::vector<facetvk::SurfaceOptics> optics(scene.facetCount() * 2U);
    // 步骤 1：按材质预计算窗口平均 ρ/τ：有 energyReflectance 时按太阳权重做梯形平均，
    // 否则回退到 shortwaveWeightedBandValue；末尾一项存放回退材质。
    // Window-mean reflectance/transmittance depend only on the material (and
    // the leaf/ground fallback), not on the facet: evaluate the ~100-sample
    // window integral once per material instead of twice per facet.
    const auto windowOptics = [&](const SurfaceParameters& surface, bool leaf) {
        const float meanReflectance =
            surface.energyReflectance.empty()
                ? shortwaveWeightedBandValue(
                    surface.reflectance, leaf ? 0.10f : 0.20f)
                : windowBandValue(
                    surface.energyReflectance, firstSample, lastSample,
                    spectralWeight, leaf ? 0.10f : 0.20f);
        const float meanTransmittance =
            surface.energyTransmittance.empty()
                ? shortwaveWeightedBandValue(
                    surface.transmittance, leaf ? 0.05f : 0.0f)
                : windowBandValue(
                    surface.energyTransmittance, firstSample, lastSample,
                    spectralWeight, leaf ? 0.05f : 0.0f);
        return std::array<float, 2>{meanReflectance, meanTransmittance};
    };
    const size_t materialCount = scene.materials.size();
    // [material][leaf] ; index materialCount holds the fallback surfaces.
    std::vector<std::array<std::array<float, 2>, 2>> materialWindow(materialCount + 1U);
    for (size_t material = 0; material < materialCount; ++material)
        for (int leaf = 0; leaf < 2; ++leaf)
            materialWindow[material][leaf] = windowOptics(scene.materials[material], leaf != 0);
    // Same fallbacks as surfaceForFacet(): default object surface, and the
    // ground fallback returned for an out-of-range non-leaf facet index.
    materialWindow[materialCount][1] = windowOptics(SurfaceParameters{}, true);
    materialWindow[materialCount][0] = windowOptics(
        surfaceForFacet(scene, std::numeric_limits<uint32_t>::max()), false);
    // 步骤 2：并行逐面元：取材质窗口值 → 乘纹理因子 → 逐侧计算直射辐照与反射率/透射率。
    parallelRanges(scene.facetCount(), 4096U, [&](size_t begin, size_t end) {
    for (uint32_t facet = static_cast<uint32_t>(begin); facet < end; ++facet) {
        const bool leaf = facet < scene.leafFacetCount;
        const uint32_t materialIndex =
            facet < scene.materialIndices.size()
            ? scene.materialIndices[facet]
            : std::numeric_limits<uint32_t>::max();
        const auto& window = materialIndex < materialCount
            ? materialWindow[materialIndex][leaf ? 1 : 0]
            : materialWindow[materialCount][leaf ? 1 : 0];
        const float meanReflectance = window[0];
        const float reflectance = std::clamp(
            meanReflectance * reflectanceTextureFactor(scene, facet),
            0.0f, 1.0f);
        const float transmittance = std::clamp(
            window[1], 0.0f, 1.0f - reflectance);
        const facetvk::Vertex& vertex = scene.vertices[facet * 3U];
        const Vec3 normal{
            vertex.normal[0], vertex.normal[1], vertex.normal[2]};
        for (uint32_t localSide = 0; localSide < 2U; ++localSide) {
            const uint32_t side = facet * 2U + localSide;
            const float sign = localSide == 0U ? 1.0f : -1.0f;
            const Vec3 sideNormal = multiply(normal, sign);
            const float cosIncidence = dot(sideNormal, sun);
            const float directIrradiance =
                std::clamp(sunlit[side], 0.0f, 1.0f) *
                std::max(0.0f, cosIncidence) * directNormalIrradiance;
            // The radiosity solve is hemispherical: the background uses its
            // directional-hemispherical (beam) and bihemispherical (sky)
            // albedo, not the directional factor of one viewing direction.
            const float sideReflectance = leaf ? reflectance :
                backgroundHapkeEnergyReflectance(
                    reflectance, std::max(cosIncidence, 0.0f),
                    directIrradiance, diffuseIrradiance,
                    scene.backgroundAngularStrength);
            optics[side].reflectance = sideReflectance;
            optics[side].transmittance = std::min(
                transmittance, 1.0f - sideReflectance);
            optics[side].directIrradiance = directIrradiance;
        }
    }
    });
    return optics;
}

// 作用：FacetEB 长波（热红外宽带）面元光学参数。ρ = tirReflectance、τ = tirTransmittance，
// 半球发射率 ε = 1-ρ-τ（背景 Hapke 方向发射率只用于图像，不改变这里的 ε）。
// emission = ε·σ·[f·T_sunlit⁴ + (1-f)·T_shaded⁴]，f 为受光比例，温度限幅 [1, 1000] K。
// 输入：受光/阴影两个状态的逐侧温度 stateTemperature（K）与受光比例 sunlitPart。
std::vector<facetvk::SurfaceOptics> makeEnergyLongwaveOptics(
    const Scene& scene,
    const std::array<std::vector<float>, 2>& stateTemperature,
    const std::vector<float>& sunlitPart)
{
    std::vector<facetvk::SurfaceOptics> optics(scene.facetCount() * 2U);
    for (uint32_t facet = 0; facet < scene.facetCount(); ++facet) {
        const SurfaceParameters& surface = surfaceForFacet(scene, facet);
        const float reflectance =
            std::clamp(surface.tirReflectance, 0.0f, 1.0f);
        const float transmittance = std::clamp(
            surface.tirTransmittance, 0.0f, 1.0f - reflectance);
        const float emissivity =
            std::max(0.0f, 1.0f - reflectance - transmittance);
        for (uint32_t localSide = 0; localSide < 2U; ++localSide) {
            const uint32_t side = facet * 2U + localSide;
            // Energy exchange is hemispherical. As for the shortwave albedo,
            // the Lambert value is kept as the hemispherical emissivity; the
            // Hapke directional emissivity (normalized at nadir) only shapes
            // image products. Previously sloped ground used its zenith-view
            // directional value here.
            const float sideEmissivity = emissivity;
            optics[side].reflectance = std::clamp(
                1.0f - transmittance - sideEmissivity, 0.0f, 1.0f);
            optics[side].transmittance = transmittance;
            const auto blackbody = [&](int state) {
                return kStefanBoltzmann * std::pow(
                    std::clamp(stateTemperature[state][side], 1.0f, 1000.0f), 4.0f);
            };
            optics[side].emission = sideEmissivity *
                (sunlitPart[side] * blackbody(0) + (1.0f - sunlitPart[side]) * blackbody(1));
        }
    }
    return optics;
}

// 作用：把短波与长波两次辐射求解的辐射度逐侧相加，得到总辐射度；迭代次数与 maxDelta
// 取两者较大值；清空 incident 缓存（它只对应未混合的解）。两者尺寸不一致时抛异常。
facetvk::SolveResult combineRadiativeSolutions(
    const facetvk::SolveResult& shortwave,
    const facetvk::SolveResult& longwave)
{
    if (shortwave.radiosity.size() != longwave.radiosity.size()) {
        throw std::runtime_error(
            "Shortwave and longwave radiosity size mismatch");
    }
    facetvk::SolveResult combined = longwave;
    combined.incident.clear(); // The cached field belongs to the unmixed solve.
    for (size_t side = 0; side < combined.radiosity.size(); ++side) {
        combined.radiosity[side] += shortwave.radiosity[side];
    }
    combined.iterations = std::max(
        shortwave.iterations, longwave.iterations);
    combined.maxDelta = std::max(
        shortwave.maxDelta, longwave.maxDelta);
    return combined;
}

// 模块：FacetRT 第 band 个波段的面元光学参数（反射、透射、发射、直射辐照）。
// 波长 > 2500 nm 视为热红外：ρ/τ 取 tir*，发射 = ε·Planck(T)，按受光比例在材质的
// sunlitTemperature / shadedTemperature（K）之间混合，并反射 2.5–6 um 的太阳光；
// 否则为光学波段：ρ/τ 取逐波段值（ρ 乘纹理因子），directIrradiance = 受光 × cos × 波段法向直射。
// 输出：每侧 SurfaceOptics；可选输出 stateEmission（受光点/阴影点发射）与 image（图像反射）。
// stateEmission (optional): per side, the emission of a sunlit and of a shaded
// point of that facet, for the sunlit/shaded image states.
std::vector<facetvk::SurfaceOptics> makeSpectralOptics(
    const Scene& scene,
    const std::vector<float>& sunlit,
    const FacetRTParameters& parameters,
    size_t band,
    std::vector<std::array<float, 2>>* stateEmission = nullptr,
    std::vector<ImageReflection>* image = nullptr)
{
    const float wavelength = parameters.wavelengths[band];
    const bool thermal = wavelength > 2500.0f;
    const facetvk::Direction direction = sunDirection(scene);
    const Vec3 sun{direction.x, direction.y, direction.z};
    // Thermal bands below 6 um also reflect the absolute solar beam (radiance
    // units, see facetRtThermalSolar); 0 elsewhere.
    const float thermalBeam = thermal ? facetRtThermalSolar(scene, wavelength)[0] : 0.0f;
    const bool hapke = scene.backgroundAngularStrength > 0.0f;
    std::vector<facetvk::SurfaceOptics> optics(scene.facetCount() * 2U);
    if (stateEmission) stateEmission->assign(optics.size(), {0.0f, 0.0f});
    if (image) image->assign(optics.size(), ImageReflection{});
    for (uint32_t facet = 0; facet < scene.facetCount(); ++facet) {
        const bool object = facet < scene.leafFacetCount;
        // A thin facet has one temperature: it is sunlit when either face is.
        const float facetIlluminated = std::clamp(
            sunlit[facet * 2U] + sunlit[facet * 2U + 1U], 0.0f, 1.0f);
        const uint32_t materialIndex =
            facet < scene.materialIndices.size()
            ? scene.materialIndices[facet] : std::numeric_limits<uint32_t>::max();
        const SurfaceParameters& surface =
            materialIndex < scene.materials.size()
            ? scene.materials[materialIndex]
            : (object ? parameters.object : parameters.ground);
        const facetvk::Vertex& vertex = scene.vertices[facet * 3U];
        const Vec3 normal{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
        for (uint32_t localSide = 0; localSide < 2U; ++localSide) {
            const uint32_t side = facet * 2U + localSide;
            const float meanReflectance = thermal
                ? surface.tirReflectance
                : bandValue(surface.reflectance, band,
                            object ? 0.10f : 0.20f);
            float reflectance = std::clamp(
                meanReflectance * (thermal
                    ? 1.0f : reflectanceTextureFactor(scene, facet)),
                0.0f, 1.0f);
            const float transmittance = std::clamp(
                thermal ? surface.tirTransmittance
                        : bandValue(surface.transmittance, band, object ? 0.05f : 0.0f),
                0.0f, 1.0f - reflectance);
            const float sign = localSide == 0U ? 1.0f : -1.0f;
            const Vec3 sideNormal = multiply(normal, sign);
            float emissivity = std::max(0.0f, 1.0f - reflectance - transmittance);
            // Hapke on: the solve keeps the hemispherical (Lambert) emissivity
            // and the conserving albedos; the zenith-view shape only enters the
            // image states (imageEmissivity, ImageReflection).
            float imageEmissivity = emissivity;
            float beamReflectance = -1.0f;
            ImageReflection sideImage;
            // 背景（非物体）面元的 Hapke 处理，四种情形：
            //   1. Hapke + 热红外：求解保持 Lambert 半球发射率，图像用按半球均值归一的方向发射率；
            //   2. Hapke + 光学：漫射用 base，光束用方向-半球反照率 beamReflectance，图像用 BRF；
            //   3、4. Hapke 关闭（strength = 0）：旧公式，数值上等于 Lambert 值，保留以与旧结果一致。
            if (!object && hapke && thermal) {
                reflectance = std::clamp(
                    1.0f - transmittance - emissivity, 0.0f, 1.0f);
                imageEmissivity = infraredHapkeImageEmissivity(
                    emissivity, std::abs(sideNormal[1]),
                    scene.backgroundAngularStrength);
                sideImage.beam = std::clamp(
                    1.0f - transmittance - imageEmissivity, 0.0f, 1.0f);
                sideImage.diffuseScale = reflectance > 0.0f
                    ? sideImage.beam / reflectance : 1.0f;
            } else if (!object && hapke) {
                const BackgroundViewGeometry geometry = backgroundViewGeometry(normal, sun);
                const BackgroundHapkeOptics h = backgroundHapkeSpectral(
                    reflectance, geometry.mu0, geometry.mu, geometry.phase,
                    scene.backgroundAngularStrength);
                reflectance = h.diffuse;
                beamReflectance = h.beam;
                sideImage.beam = h.imageBeam;
                sideImage.diffuseScale = h.diffuse > 0.0f
                    ? h.imageDiffuse / h.diffuse : 1.0f;
            } else if (!object && thermal) {
                emissivity = infraredHapkeEmissivity(
                    emissivity, std::abs(sideNormal[1]),
                    scene.backgroundAngularStrength);
                imageEmissivity = emissivity;
                reflectance = std::clamp(
                    1.0f - transmittance - emissivity, 0.0f, 1.0f);
            } else if (!object) {
                reflectance = backgroundHapkeReflectance(
                    reflectance, sun, kImageViewDirection, sideNormal,
                    scene.backgroundAngularStrength);
            }
            if (image) (*image)[side] = sideImage;
            optics[side].reflectance = reflectance;
            optics[side].transmittance = std::min(
                transmittance, 1.0f - reflectance);
            if (thermal) {
                // 热红外：发射 = ε·B(λ, T)，按 facetIlluminated 混合受光/阴影温度；stateEmission 另存
                // 受光点与阴影点各自的图像发射；directIrradiance 为反射的太阳光（辐亮度单位）。
                const float sunlitBlackbody = planckRadiance(wavelength, surface.sunlitTemperature);
                const float shadedBlackbody = planckRadiance(wavelength, surface.shadedTemperature);
                const float sunlitEmission = emissivity * sunlitBlackbody;
                const float shadedEmission = emissivity * shadedBlackbody;
                optics[side].emission = facetIlluminated * sunlitEmission +
                    (1.0f - facetIlluminated) * shadedEmission;
                optics[side].directIrradiance =
                    sunlit[side] * std::max(0.0f, dot(sideNormal, sun)) * thermalBeam;
                if (stateEmission) (*stateEmission)[side] = {
                    imageEmissivity * sunlitBlackbody, imageEmissivity * shadedBlackbody};
            } else {
                optics[side].directIrradiance =
                    sunlit[side] * std::max(0.0f, dot(sideNormal, sun)) *
                    bandBeamIrradiance(scene, parameters, band);
                // Beam reflected with its own albedo: the solve adds
                // (beam - diffuse) * E as a source term (optical emission is
                // otherwise 0; spectral diffuse baselines remove it).
                if (beamReflectance >= 0.0f)
                    optics[side].emission =
                        (beamReflectance - reflectance) * optics[side].directIrradiance;
            }
        }
    }
    return optics;
}

// Splits the solved, facet-averaged radiosity of each side into the radiance of
// a sunlit point and of a shaded point. The solve used the facet's mean direct
// irradiance and mean emission; removing both leaves the diffuse and multiple
// scattering part shared by every point of the side. A sunlit point receives
// the full beam on the sun-facing side (reflected there, transmitted to the
// other side) and emits at the sunlit temperature on both sides.
// Output per facet: front lit, front shaded, back lit, back shaded.
void appendSurfaceStates(const Scene& scene,
                         const std::vector<facetvk::SurfaceOptics>& optics,
                         const std::vector<std::array<float, 2>>& stateEmission,
                         const std::vector<float>& radiosity,
                         float beamIrradiance,
                         float outputScale,
                         bool clampUnit,
                         std::vector<float>& states,
                         const std::vector<ImageReflection>* image = nullptr)
{
    const facetvk::Direction direction = sunDirection(scene);
    const Vec3 sun{direction.x, direction.y, direction.z};
    const auto finish = [&](float value) {
        // Apparent reflectance above 1 is physical on sun-facing facets; the
        // historical clampUnit flag no longer caps it (GPU spectral.comp
        // finish() matches). Only negative values are removed.
        (void)clampUnit;
        return std::max(0.0f, value * outputScale);
    };
    for (uint32_t facet = 0; facet < scene.facetCount(); ++facet) {
        const facetvk::Vertex& vertex = scene.vertices[facet * 3U];
        const Vec3 normal{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
        const float frontDirect = std::max(0.0f, dot(normal, sun)) * beamIrradiance;
        const float backDirect = std::max(0.0f, -dot(normal, sun)) * beamIrradiance;
        for (uint32_t localSide = 0; localSide < 2U; ++localSide) {
            const uint32_t side = facet * 2U + localSide;
            const uint32_t opposite = facet * 2U + (1U - localSide);
            const facetvk::SurfaceOptics& surface = optics[side];
            // shared：辐射度中扣除本侧直射反射、对侧直射透射与发射后剩下的漫射 + 多次散射部分，
            // 受光点与阴影点共用。
            const float shared = radiosity[side]
                - surface.reflectance * surface.directIrradiance
                - surface.transmittance * optics[opposite].directIrradiance
                - surface.emission;
            const float ownDirect = localSide == 0U ? frontDirect : backDirect;
            const float otherDirect = localSide == 0U ? backDirect : frontDirect;
            if (image && (*image)[side].beam >= 0.0f) {
                // Hapke background: the image sees the zenith-view reflection
                // of the solved diffuse field and of the beam (spectral.comp).
                const ImageReflection& view = (*image)[side];
                const float diffuse = shared * view.diffuseScale;
                const float lit = diffuse + view.beam * ownDirect
                    + surface.transmittance * otherDirect + stateEmission[side][0];
                const float shaded = diffuse + stateEmission[side][1];
                states.push_back(finish(lit));
                states.push_back(finish(shaded));
                continue;
            }
            const float lit = shared + surface.reflectance * ownDirect
                + surface.transmittance * otherDirect + stateEmission[side][0];
            const float shaded = shared + stateEmission[side][1];
            states.push_back(finish(lit));
            states.push_back(finish(shaded));
        }
    }
}

// DEFERRED[中] Bug: 波长单位推断规则不一致：此处 >2.5 视为 nm，planckRadiance 却把 <=50 视为 µm，makeSpectralOptics/prepareGpuSpectral 又用 wavelength>2500 判断热红外；若用户以 µm 给出热红外波段（如 10.5），这里会被当成 10.5 nm 的光学波段，GPU 光谱路径与 CPU 审计路径的判定也互不一致；建议：在解析 controlBand 时统一换算为 nm 一次，并删除各处的启发式判断。
bool isOpticalWavelength(float wavelength)
{
    const float wavelengthNanometers = wavelength <= 2.5f
        ? wavelength * 1000.0f : wavelength;
    return wavelengthNanometers <= 2500.0f;
}

// 作用：把 FacetRT 逐波段谱段求解所需的数据一次性上传到 GPU（model.beginSpectral）。
// 输入：场景、每侧受光比例 sunlit、波段参数 parameters；副作用：GPU 端保存面元记录与材质谱表。
// 说明：材质数 = scene.materials + 2（末尾两个为默认物体 / 默认地面回退材质）。
// 数据布局与 shader/spectral.comp 对应，修改时需同步。
void prepareGpuSpectral(facetvk::FacetrtVulkan& model, const Scene& scene, const std::vector<float>& sunlit,
                        const FacetRTParameters& parameters)
{
    const auto direction = sunDirection(scene);
    const Vec3 sun{direction.x, direction.y, direction.z};
    const uint32_t materials = static_cast<uint32_t>(scene.materials.size()) + 2U;
    // 步骤 1：每个面元 12 个 float：[0..2] 原始法向，[3] 反射率纹理因子，[4] 材质索引，
    // [5] 是否物体面元，[6..7] 两侧受光比例，[8] 太阳入射余弦 mu0，[9] 反向散射峰因子
    // 1/(1+tan(g/2)/0.1)（与 facetHapkeKernel 同参数），[10] |法向 y|，[11] 天顶视线余弦 mu。
    std::vector<float> facets(size_t(scene.facetCount()) * 12U);
    for (uint32_t f = 0; f < scene.facetCount(); ++f) {
        float* p = facets.data() + size_t(f) * 12U;
        const auto& v = scene.vertices[f * 3U];
        const Vec3 rawNormal{v.normal[0], v.normal[1], v.normal[2]};
        const Vec3 normal = normalized(rawNormal);
        Vec3 incoming = normalized(sun);
        if (dot(incoming, normal) < 0) incoming = multiply(incoming, -1.0f);
        p[0] = rawNormal[0]; p[1] = rawNormal[1]; p[2] = rawNormal[2];
        p[3] = reflectanceTextureFactor(scene, f);
        const bool object = f < scene.leafFacetCount;
        p[4] = float(f < scene.materialIndices.size() && scene.materialIndices[f] < scene.materials.size()
            ? scene.materialIndices[f] : materials - (object ? 2U : 1U));
        p[5] = object ? 1.0f : 0.0f;
        p[6] = sunlit[2U * f]; p[7] = sunlit[2U * f + 1U];
        p[8] = std::max(dot(normal, incoming), 1.0e-4f);
        // Image view direction (zenith) folded into this facet's hemisphere,
        // exactly as backgroundHapkeReflectance does on the CPU path. Flipping
        // both directions for the back side leaves mu and the phase unchanged.
        Vec3 outgoing = kImageViewDirection;
        if (dot(outgoing, normal) < 0) outgoing = multiply(outgoing, -1.0f);
        const float phase = std::acos(std::clamp(dot(incoming, outgoing), -1.0f, 1.0f));
        p[9] = 1.0f / (1.0f + std::tan(0.5f * phase) / 0.1f);
        p[10] = std::abs(rawNormal[1]);
        p[11] = std::max(dot(normal, outgoing), 1.0e-4f);
    }
    // Hapke background: the albedo tables follow the facet records (see
    // HapkeAlbedoTable::append and spectral.comp); not built otherwise.
    if (scene.backgroundAngularStrength > 0.0f) hapkeAlbedoTable().append(facets);
    // 步骤 2：逐波段 × 材质预计算 4 个值：ρ、τ、受光温度 Planck 辐亮度、阴影温度 Planck 辐亮度
    // （热红外波段 > 2500 nm 才有后两项），然后调用 beginSpectral 上传。
    // Spectral tables and blackbody radiance are material properties: evaluate
    // once per material/band instead of repeating expm1 for every oriented side.
    std::vector<float> materialBands(parameters.wavelengths.size() * materials * 4U);
    for (size_t b = 0; b < parameters.wavelengths.size(); ++b) {
        const float wavelength = parameters.wavelengths[b];
        const bool thermal = wavelength > 2500.0f;
        for (uint32_t m = 0; m < materials; ++m) {
            const bool object = m != materials - 1U;
            const SurfaceParameters& surface = m < scene.materials.size() ? scene.materials[m]
                : object ? parameters.object : parameters.ground;
            float* p = materialBands.data() + (b * materials + m) * 4U;
            p[0] = thermal ? surface.tirReflectance : bandValue(surface.reflectance, b, object ? 0.10f : 0.20f);
            p[1] = thermal ? surface.tirTransmittance : bandValue(surface.transmittance, b, object ? 0.05f : 0.0f);
            p[2] = thermal ? planckRadiance(wavelength, surface.sunlitTemperature) : 0.0f;
            p[3] = thermal ? planckRadiance(wavelength, surface.shadedTemperature) : 0.0f;
        }
    }
    model.beginSpectral(facets, materialBands, static_cast<uint32_t>(parameters.wavelengths.size()), materials,
                        direction, scene.backgroundAngularStrength,
                        bandBeamIrradiance(scene, parameters, 0));
}

// 作用：FacetEB 逐波段图像（光学与热红外）的面元光学参数，构造方式与 makeSpectralOptics
// 相同，区别是温度来自 stateTemperature（逐侧受光/阴影两个状态，K，限幅 [1, 2000]），
// 波段类型用 isOpticalWavelength 判断，directIrradiance 用传入的 directNormalIrradiance
// （光学波段为归一光束，热红外为 2.5–6 um 太阳光辐亮度，其余为 0）。
// 输出：每侧 SurfaceOptics；stateEmission 为受光/阴影点的图像发射；image 为 Hapke 背景的图像反射。
std::vector<facetvk::SurfaceOptics> makeStateSpectralOptics(
    const Scene& scene,
    const std::vector<float>& sunlit,
    const FacetRTParameters& parameters,
    size_t band,
    const std::array<std::vector<float>, 2>& stateTemperature,
    const std::vector<float>& sunlitPart,
    float directNormalIrradiance,
    std::vector<std::array<float, 2>>& stateEmission,
    std::vector<ImageReflection>* image = nullptr)
{
    const float wavelength = parameters.wavelengths[band];
    const bool optical = isOpticalWavelength(wavelength);
    const facetvk::Direction direction = sunDirection(scene);
    const Vec3 sun{direction.x, direction.y, direction.z};
    const bool hapke = scene.backgroundAngularStrength > 0.0f;
    std::vector<facetvk::SurfaceOptics> optics(scene.facetCount() * 2U);
    stateEmission.assign(optics.size(), {0.0f, 0.0f});
    if (image) image->assign(optics.size(), ImageReflection{});
    for (uint32_t facet = 0; facet < scene.facetCount(); ++facet) {
        const bool object = facet < scene.leafFacetCount;
        const SurfaceParameters& surface = surfaceForFacet(scene, facet);
        const facetvk::Vertex& vertex = scene.vertices[facet * 3U];
        const Vec3 normal{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
        for (uint32_t localSide = 0; localSide < 2U; ++localSide) {
            const uint32_t side = facet * 2U + localSide;
            const float meanReflectance = optical
                ? bandValue(surface.reflectance, band,
                            object ? 0.10f : 0.20f)
                : surface.tirReflectance;
            float reflectance = std::clamp(
                meanReflectance * (optical
                    ? reflectanceTextureFactor(scene, facet) : 1.0f),
                0.0f, 1.0f);
            const float transmittance = std::clamp(
                optical
                    ? bandValue(surface.transmittance, band,
                                object ? 0.05f : 0.0f)
                    : surface.tirTransmittance,
                0.0f, 1.0f - reflectance);
            const float sign = localSide == 0U ? 1.0f : -1.0f;
            const Vec3 sideNormal = multiply(normal, sign);
            float emissivity = std::max(0.0f, 1.0f - reflectance - transmittance);
            // Hapke on: same split as makeSpectralOptics (conserving solve,
            // zenith-view shape only in the image states).
            float imageEmissivity = emissivity;
            float beamReflectance = -1.0f;
            ImageReflection sideImage;
            if (!object && hapke && optical) {
                const BackgroundViewGeometry geometry = backgroundViewGeometry(normal, sun);
                const BackgroundHapkeOptics h = backgroundHapkeSpectral(
                    reflectance, geometry.mu0, geometry.mu, geometry.phase,
                    scene.backgroundAngularStrength);
                reflectance = h.diffuse;
                beamReflectance = h.beam;
                sideImage.beam = h.imageBeam;
                sideImage.diffuseScale = h.diffuse > 0.0f
                    ? h.imageDiffuse / h.diffuse : 1.0f;
            } else if (!object && hapke) {
                reflectance = std::clamp(
                    1.0f - transmittance - emissivity, 0.0f, 1.0f);
                imageEmissivity = infraredHapkeImageEmissivity(
                    emissivity, std::abs(sideNormal[1]),
                    scene.backgroundAngularStrength);
                sideImage.beam = std::clamp(
                    1.0f - transmittance - imageEmissivity, 0.0f, 1.0f);
                sideImage.diffuseScale = reflectance > 0.0f
                    ? sideImage.beam / reflectance : 1.0f;
            } else if (!object && optical) {
                reflectance = backgroundHapkeReflectance(
                    reflectance, sun, kImageViewDirection, sideNormal,
                    scene.backgroundAngularStrength);
            } else if (!object) {
                emissivity = infraredHapkeEmissivity(
                    emissivity, std::abs(sideNormal[1]),
                    scene.backgroundAngularStrength);
                imageEmissivity = emissivity;
                reflectance = std::clamp(
                    1.0f - transmittance - emissivity, 0.0f, 1.0f);
            }
            if (image) (*image)[side] = sideImage;
            optics[side].reflectance = reflectance;
            optics[side].transmittance = std::min(
                transmittance, 1.0f - reflectance);
            // Optical bands: the normalized-to-W m-2 nm-1 beam; thermal bands:
            // the 2.5-6 um solar beam in radiance units (0 elsewhere).
            optics[side].directIrradiance =
                std::clamp(sunlit[side], 0.0f, 1.0f) *
                std::max(0.0f, dot(sideNormal, sun)) *
                directNormalIrradiance;
            if (optical && beamReflectance >= 0.0f) {
                optics[side].emission =
                    (beamReflectance - reflectance) * optics[side].directIrradiance;
            }
            if (!optical) {
                const auto blackbody = [&](int state) {
                    return planckRadiance(
                        wavelength,
                        std::clamp(stateTemperature[state][side], 1.0f, 2000.0f));
                };
                const float sunlitBlackbody = blackbody(0), shadedBlackbody = blackbody(1);
                optics[side].emission = sunlitPart[side] * (emissivity * sunlitBlackbody) +
                    (1.0f - sunlitPart[side]) * (emissivity * shadedBlackbody);
                stateEmission[side] = {imageEmissivity * sunlitBlackbody,
                                       imageEmissivity * shadedBlackbody};
            }
        }
    }
    return optics;
}

// 投影后顶点：x、y 为像素坐标（浮点），depth 为归一化深度 [0,1]（0 为离观察点最近）。
struct ProjectedVertex {
    float x{};
    float y{};
    float depth{};
};

// A-buffer 片元：depth 同上；cameraSide 为面元朝向观察点的那一侧的 side 索引；
// central 表示来自中心块（false 为周期副本，只遮挡和发射，不作接收者）。
struct Fragment {
    float depth{};
    uint32_t cameraSide{};
    // Periodic replicas occlude and emit but are never receivers.
    bool central{true};
};

// 模块：保留的 CPU 光栅化器（GPU 后端之外的参考/回退实现）。
// 对一个方向做正交投影：每个三角形（含周期副本）按像素光栅化，把片元写入该像素的定长列表
// （A-buffer，层由原子计数分配）；超出 layers 的片元只计入溢出数，由调用方加深层数后重来
// （见 growCpuRasterLayers）。深度按包含全部周期副本的包围球归一，x/y 开窗只含中心块。
class CpuRasterizer {
public:
    // 构造：分配每像素计数器与片元数组；计算两个包围球：m_center / m_radius 含全部周期副本
    // （用于深度归一），m_windowCenter / m_windowRadius 只含中心块（用于 x/y 开窗）；
    // 半径放大 1.001 倍并设下限，避免边缘面元落到窗口外。
    CpuRasterizer(const Scene& scene, uint32_t width, uint32_t height, uint32_t layers)
        : m_scene(scene),
          m_width(width),
          m_height(height),
          m_layers(layers),
          m_pixelCount(static_cast<size_t>(width) * height),
          m_counts(new std::atomic<uint32_t>[m_pixelCount]()),
          m_fragments(m_pixelCount * layers)
    {
        Vec3 extendedMinimum = scene.minimum;
        Vec3 extendedMaximum = scene.maximum;
        for (uint32_t tileIndex = 1U;
             tileIndex <= scene.periodicNeighborCount; ++tileIndex) {
            const auto tile = periodicTileOffset(tileIndex);
            const float offsetX = tile[0] * scene.periodicSizeX;
            const float offsetZ = tile[1] * scene.periodicSizeZ;
            extendedMinimum[0] = std::min(
                extendedMinimum[0], scene.minimum[0] + offsetX);
            extendedMaximum[0] = std::max(
                extendedMaximum[0], scene.maximum[0] + offsetX);
            extendedMinimum[2] = std::min(
                extendedMinimum[2], scene.minimum[2] + offsetZ);
            extendedMaximum[2] = std::max(
                extendedMaximum[2], scene.maximum[2] + offsetZ);
        }
        const Vec3 diagonal = subtract(extendedMaximum, extendedMinimum);
        m_center = multiply(add(extendedMinimum, extendedMaximum), 0.5f);
        m_radius = std::max(0.5f * std::sqrt(dot(diagonal, diagonal)) * 1.001f,
                            1.0e-4f);
        // x/y window over the central block only (see makeProjection in
        // facetrt_vulkan.cpp); depth keeps the replica-wide sphere.
        const Vec3 centralDiagonal = subtract(scene.maximum, scene.minimum);
        m_windowCenter = multiply(add(scene.minimum, scene.maximum), 0.5f);
        m_windowRadius = std::max(
            0.5f * std::sqrt(dot(centralDiagonal, centralDiagonal)) * 1.001f, 1.0e-4f);
    }

    // 作用：沿 inputDirection 对整个场景（含周期复制块）做一次正交投影的 CPU 软光栅化，
    //       把每个像素上覆盖到的所有三角形片元（深度 + 面元侧 id）存入 A-buffer（多层）。
    // 输入：inputDirection 为指向“相机/观察点”一侧的单位方向；jitterX/Y 为亚像素抖动（像素）。
    // 输出：返回超出层数上限 m_layers 而被丢弃的片元数（非 0 表示 A-buffer 溢出）。
    // 说明：深度归一化到 [0,1]（越小越靠近相机）；cameraSide = 2*facet + (法线朝向相机 ? 0 : 1)。
    // 作用：沿 inputDirection（指向观察点的方向）做一次正交光栅化；jitterX/jitterY 为像素偏移。
    // 输出：溢出片元数（> 0 表示层数不够）。结果保存在 count() / fragments() 中。
    uint32_t rasterize(const facetvk::Direction& inputDirection,
                       float jitterX = 0.0f, float jitterY = 0.0f)
    {
        // 步骤 1：清零每个像素的片元计数，上一方向的结果不能带入本方向。
        // 步骤 1：清零每像素片元计数。
        parallelRanges(m_pixelCount, 1U << 16U, [&](size_t begin, size_t end) {
            for (size_t pixel = begin; pixel < end; ++pixel)
                m_counts[pixel].store(0U, std::memory_order_relaxed);
        });
        // 步骤 2：由视线方向构造正交基（right, up）；参考轴在视线接近竖直时改用 y 轴，避免叉积退化。
        // 步骤 2：建立正交相机基 (right, up, view)；参考向量避开与 view 近平行的方向。
        const Vec3 view = normalized({inputDirection.x, inputDirection.y, inputDirection.z});
        const Vec3 reference = std::abs(view[2]) < 0.9f ? Vec3{0.0f, 0.0f, 1.0f}
                                                        : Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 right = normalized(cross(reference, view));
        const Vec3 up = cross(view, right);
        std::atomic<uint32_t> overflow{0U};
        const size_t facetCount = m_scene.facetCount();
        const size_t triangleCount = facetCount * (m_scene.periodicNeighborCount + 1U);

        // 步骤 3：并行遍历“中心块 + 周期邻块”的全部三角形：按 tileIndex 平移顶点，
        //         x/y 用中心块窗口（m_windowCenter/Radius）映射到像素，深度用整体外接球（m_center/Radius）。
        // 步骤 3：并行遍历所有三角形（triangle = tile × facetCount + facet；tile 0 为中心块，
        // 其余为周期副本，平移 tile 偏移 × periodicSize），投影后逐三角形光栅化。
        // cameraSide 取面元朝向观察点的那一侧（法向与 view 同向为 side0，否则为 side1）。
        parallelRanges(triangleCount, 512U, [&](size_t begin, size_t end) {
          for (size_t triangle = begin; triangle < end; ++triangle) {
            const uint32_t tileIndex = static_cast<uint32_t>(triangle / facetCount);
            const uint32_t facet = static_cast<uint32_t>(triangle % facetCount);
            const auto tile = periodicTileOffset(tileIndex);
            const Vec3 tileTranslation{
                tile[0] * m_scene.periodicSizeX, 0.0f,
                tile[1] * m_scene.periodicSizeZ};
            ProjectedVertex projected[3];
            for (uint32_t corner = 0; corner < 3U; ++corner) {
                const facetvk::Vertex& vertex = m_scene.vertices[facet * 3U + corner];
                const Vec3 position{
                    vertex.position[0] + tileTranslation[0], vertex.position[1],
                    vertex.position[2] + tileTranslation[2]};
                const Vec3 relative = subtract(position, m_center);
                const Vec3 windowRelative = subtract(position, m_windowCenter);
                projected[corner].x =
                    (0.5f + 0.5f * dot(windowRelative, right) / m_windowRadius) * m_width - jitterX;
                projected[corner].y =
                    (0.5f + 0.5f * dot(windowRelative, up) / m_windowRadius) * m_height - jitterY;
                projected[corner].depth =
                    0.5f - dot(relative, view) / (2.0f * m_radius);
            }
            // 该面元的几何法线与视线同向则相机看到的是 side 0（正面），否则是 side 1（背面）；
            // tileIndex == 0 才是中心块，周期副本只作遮挡/发射、不作为接收者。
            const facetvk::Vertex& first = m_scene.vertices[facet * 3U];
            const Vec3 normal{first.normal[0], first.normal[1], first.normal[2]};
            const uint32_t cameraSide = facet * 2U + (dot(normal, view) >= 0.0f ? 0U : 1U);
            rasterTriangle(projected, cameraSide, tileIndex == 0U, overflow);
          }
        });
        return overflow.load();
    }

    // 只读访问器：某像素实际可用的片元层数（计数可能因溢出超过 m_layers，这里截断到 m_layers）。
    // 访问器：count 为像素已写入的片元数（被 layers 截断）；fragments 为 [pixel*layers + layer]
    // 排布的扁平数组，片元顺序与线程调度有关，使用前须按 fragmentBefore 排序。
    uint32_t count(size_t pixel) const
    {
        return std::min(m_counts[pixel].load(std::memory_order_relaxed), m_layers);
    }
    size_t pixelCount() const { return m_pixelCount; }
    const std::vector<Fragment>& fragments() const { return m_fragments; }
    uint32_t layers() const { return m_layers; }

private:
    // 边函数：(a→b) 边与点 p 的二维叉积（带符号的两倍三角形面积），用于重心坐标与内外判定。
    // 边函数：点 p 相对有向边 a→b 的叉积（两倍三角形有向面积，用于重心坐标与内外判定）。
    static float edge(float ax, float ay, float bx, float by, float px, float py)
    {
        return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
    }

    // 作用：把一个已投影到像素坐标的三角形光栅化，向覆盖到的每个像素追加一个片元。
    // 输入：p 为三个顶点的像素坐标与归一化深度；side 为 cameraSide；central 标记是否中心块。
    // 说明：像素中心采样；用原子计数 fetch_add 分配层号，故可被多线程并行调用；
    //       层号 >= m_layers 时只累加 overflow，不写入片元。
    // 作用：用边函数光栅化一个投影三角形：遍历包围盒内的像素，像素中心（+0.5）落入三角形内
    // 则在该像素原子地取一层，写入重心插值的深度、side 与 central；层数不够时只累加溢出数。
    // 面积接近 0（< 1e-8）的退化投影直接跳过。
    void rasterTriangle(const ProjectedVertex (&p)[3], uint32_t side, bool central,
                        std::atomic<uint32_t>& overflow)
    {
        // 步骤 1：投影后面积接近 0 的退化三角形（与视线几乎平行）直接跳过。
        const float area = edge(p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y);
        if (std::abs(area) < 1.0e-8f) {
            return;
        }
        // 步骤 2：求三角形的像素包围盒，并裁剪到图像范围内。
        const int x0 = std::max(0, static_cast<int>(std::floor(
                                       std::min({p[0].x, p[1].x, p[2].x}))));
        const int x1 = std::min(static_cast<int>(m_width) - 1,
                                static_cast<int>(std::ceil(
                                    std::max({p[0].x, p[1].x, p[2].x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(
                                       std::min({p[0].y, p[1].y, p[2].y}))));
        const int y1 = std::min(static_cast<int>(m_height) - 1,
                                static_cast<int>(std::ceil(
                                    std::max({p[0].y, p[1].y, p[2].y}))));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                // 步骤 3：对包围盒内每个像素中心，用三条边函数得到（未归一化的）重心坐标 w0/w1/w2，
                //         并按三角形朝向（area 的符号）判断是否在内部。
                const float px = static_cast<float>(x) + 0.5f;
                const float py = static_cast<float>(y) + 0.5f;
                const float w0 = edge(p[1].x, p[1].y, p[2].x, p[2].y, px, py);
                const float w1 = edge(p[2].x, p[2].y, p[0].x, p[0].y, px, py);
                const float w2 = edge(p[0].x, p[0].y, p[1].x, p[1].y, px, py);
                // DEFERRED[中] 数值: 内部测试对三条边都用闭区间（>=0/<=0），没有 top-left 填充规则；像素中心恰落在共享边上时相邻两个三角形都会写片元（同深度），产生虚假的相邻面元可见边并重复计入 denominator，与 GPU 光栅化（遵循 top-left 规则）结果不一致；建议：实现 top-left 规则（对非 top/left 边用严格不等式）。
                const bool inside = area > 0.0f ? (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
                                               : (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);
                if (!inside) {
                    continue;
                }
                // 步骤 4：分配层号；超出层数上限则只计入溢出；否则用重心坐标插值深度并写入片元。
                const size_t pixel = static_cast<size_t>(y) * m_width + x;
                const uint32_t layer = m_counts[pixel].fetch_add(1U, std::memory_order_relaxed);
                if (layer >= m_layers) {
                    overflow.fetch_add(1U, std::memory_order_relaxed);
                    continue;
                }
                const float invArea = 1.0f / area;
                m_fragments[pixel * m_layers + layer] = {
                    (w0 * p[0].depth + w1 * p[1].depth + w2 * p[2].depth) * invArea,
                    side, central};
            }
        }
    }

    // 成员：场景引用、A-buffer 尺寸、深度球（m_center/m_radius，覆盖周期副本）、
    //       x/y 窗口球（m_windowCenter/m_windowRadius，仅覆盖中心块）、每像素原子计数与片元数组
    //       （第 pixel 个像素的第 layer 层在 m_fragments[pixel * m_layers + layer]）。
    const Scene& m_scene;
    uint32_t m_width{};
    uint32_t m_height{};
    uint32_t m_layers{};
    Vec3 m_center{};
    float m_radius{};
    Vec3 m_windowCenter{};
    float m_windowRadius{};
    size_t m_pixelCount{};
    std::unique_ptr<std::atomic<uint32_t>[]> m_counts;
    std::vector<Fragment> m_fragments;
};

// Fragments of one pixel arrive in thread-dependent order; this total order
// keeps the depth sort, and so the visibility graph, reproducible. Equal depths
// use the GPU key (visibility_resolve/direct_resolve.comp compare cameraSide
// with the replica bit 0x80000000 set): central fragments first, then side id.
bool fragmentBefore(const Fragment& a, const Fragment& b)
{
    if (a.depth != b.depth) return a.depth < b.depth;
    if (a.central != b.central) return a.central;
    return a.cameraSide < b.cameraSide;
}

using Progress = std::function<void(int, const std::string&)>;

// 模块：CPU 版可见性图构建（与 GPU 的 visibility_resolve 流程对应的参考实现）。
// 作用：对半球方向集合逐方向光栅化，按像素深度排序后统计每个“面元侧”（side）的
//       接收分母 denominators、直达天空计数 skyCounts、向下逃逸计数 groundCounts，
//       以及面元侧之间的可见边（CSR 稀疏图：rowOffsets + edges{neighbor,count}）。
// 输入：场景 scene、进度回调 progress（进度区间约 5–70%）。
// 输出：CpuGraph；若 A-buffer 层数溢出则抛出 runtime_error（结果不可信）。
// 约定：side = 2*facet + {0,1}；计数里的“2”是权重，使两侧各计一次（见下方各处说明）。
CpuGraph buildCpuGraph(const Scene& scene, const Progress& progress)
{
    // 步骤 1：创建光栅器与各类计数器（原子计数，供多线程像素循环并发累加）。
    CpuRasterizer raster(scene, scene.visibilityRasterSize,
                         scene.visibilityRasterSize,
                         scene.visibilityRasterLayers);
    const uint32_t surfaceCount = scene.facetCount() * 2U;
    CpuGraph graph;
    std::unique_ptr<std::atomic<uint32_t>[]> denominators(new std::atomic<uint32_t>[surfaceCount]());
    std::unique_ptr<std::atomic<uint32_t>[]> skyCounts(new std::atomic<uint32_t>[surfaceCount]());
    std::unique_ptr<std::atomic<uint32_t>[]> groundCounts(new std::atomic<uint32_t>[surfaceCount]());
    using EdgeMap = std::unordered_map<uint64_t, uint32_t>;
    // Thread maps hold one direction only and are folded into key-sharded totals after
    // each direction, so peak memory does not grow with the thread count.
    constexpr size_t edgeShardCount = 64U;
    // 按 key 的乘法哈希高 6 位分到 64 个分片（与 edgeShardCount 对应），便于分片并行合并。
    const auto edgeShard = [](uint64_t key) {
        return static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 58U);
    };
    std::vector<std::vector<EdgeMap>> threadEdgeCounts(
        static_cast<size_t>(cpuThreadCount()), std::vector<EdgeMap>(edgeShardCount));
    std::vector<EdgeMap> mergedShards(edgeShardCount);
    uint32_t overflow = 0;
    const std::vector<facetvk::Direction> directions = makeHemisphereDirections();
    const uint32_t layers = raster.layers();
    const auto& fragments = raster.fragments();

    // 步骤 2：逐个半球方向：先光栅化，再并行处理每个像素的片元链。
    for (uint32_t directionIndex = 0; directionIndex < directions.size(); ++directionIndex) {
        overflow += raster.rasterize(directions[directionIndex]);
        parallelRanges(raster.pixelCount(), 4096U, [&](size_t begin, size_t end) {
        auto& edgeCounts = threadEdgeCounts[static_cast<size_t>(omp_get_thread_num())];
        std::vector<Fragment> local;
        local.reserve(layers);
        for (size_t pixel = begin; pixel < end; ++pixel) {
            const uint32_t count = raster.count(pixel);
            if (count == 0U) {
                continue;
            }
            local.assign(fragments.begin() + pixel * layers,
                         fragments.begin() + pixel * layers + count);
            // 该像素的片元按深度由近到远排序（相机侧在前），相邻片元之间即为互相可见的一对。
            std::sort(local.begin(), local.end(), fragmentBefore);
            // Same weighting as visibility_resolve.comp: central fragments
            // receive with weight 2, a depth-adjacent pair weighs its number
            // of central members, so replica-truncated tiles never receive.
            for (const Fragment& fragment : local) {
                if (!fragment.central) continue;
                denominators[fragment.cameraSide].fetch_add(2U, std::memory_order_relaxed);
                denominators[fragment.cameraSide ^ 1U].fetch_add(2U, std::memory_order_relaxed);
            }
            // 最靠近相机的中心片元，其朝向相机的一侧沿视线方向直接看到天空。
            if (local.front().central)
                skyCounts[local.front().cameraSide].fetch_add(2U, std::memory_order_relaxed);
            // Downward escape: the lower boundary, not the sky.
            if (local.back().central)
                groundCounts[local.back().cameraSide ^ 1U].fetch_add(2U, std::memory_order_relaxed);
            // 相邻片元对：前一片元背向相机的一侧（cameraSide ^ 1）与后一片元朝向相机的一侧互相可见；
            // 累加到无向边 (lo, hi)，权重为这一对中的中心片元数（0 表示两者都是周期副本，忽略）。
            for (uint32_t i = 0; i + 1U < count; ++i) {
                const uint32_t sideA = local[i].cameraSide ^ 1U;
                const uint32_t sideB = local[i + 1U].cameraSide;
                const uint32_t weight = (local[i].central ? 1U : 0U) +
                                        (local[i + 1U].central ? 1U : 0U);
                // Same rule as visibility_resolve.comp: a periodic self-replica
                // edge (2f, 2f+1) is kept; only an identical side pair is skipped.
                if (weight == 0U || sideA == sideB) {
                    continue;
                }
                const uint32_t lo = std::min(sideA, sideB);
                const uint32_t hi = std::max(sideA, sideB);
                const uint64_t key = (uint64_t(lo) << 32U) | hi;
                edgeCounts[edgeShard(key)][key] += weight;
            }
        }
        });
        // 本方向处理完后，把各线程的边计数并入按 key 分片的总表并清空线程表，
        // 避免线程表的内存随方向数累积。
        parallelRanges(edgeShardCount, 1U, [&](size_t begin, size_t end) {
            for (size_t shard = begin; shard < end; ++shard) {
                EdgeMap& merged = mergedShards[shard];
                for (auto& perThread : threadEdgeCounts) {
                    for (const auto& entry : perThread[shard]) merged[entry.first] += entry.second;
                    perThread[shard].clear();
                }
            }
        });
        progress(5 + static_cast<int>(65U * (directionIndex + 1U) / directions.size()),
                 "CPU visibility raster " + std::to_string(directionIndex + 1U) + "/" +
                     std::to_string(directions.size()));
    }
    threadEdgeCounts = {};

    // 步骤 3：原子计数拷贝到 graph，并施加下边界（地面面元的下表面置为无效，
    //         向下逃逸的射线记为“等效地面”）；同时填写 graph.diagnostics 的地面逃逸统计。
    graph.denominators.resize(surfaceCount);
    graph.skyCounts.resize(surfaceCount);
    graph.groundCounts.resize(surfaceCount);
    for (uint32_t side = 0; side < surfaceCount; ++side) {
        graph.denominators[side] = denominators[side].load();
        graph.skyCounts[side] = skyCounts[side].load();
        graph.groundCounts[side] = groundCounts[side].load();
    }
    graph.ground = facetvk::applyGroundBoundary(scene.vertices, scene.leafFacetCount, graph.denominators,
                                                graph.skyCounts, graph.groundCounts, graph.diagnostics);
    // 步骤 4：把分片表摊平成 (key, count) 数组并按 key 排序，保证图的构造与线程数无关、可复现。
    size_t edgeTotal = 0;
    for (const EdgeMap& shard : mergedShards) edgeTotal += shard.size();
    std::vector<std::pair<uint64_t, uint32_t>> edgeCounts;
    edgeCounts.reserve(edgeTotal);
    for (EdgeMap& shard : mergedShards) {
        edgeCounts.insert(edgeCounts.end(), shard.begin(), shard.end());
        shard = {};
    }
    std::sort(edgeCounts.begin(), edgeCounts.end());

    // 步骤 5：由无向边生成对称的 CSR 邻接表：先统计每个顶点的度，前缀和得 rowOffsets，
    //         再把每条边 (a,b) 同时写入 a 与 b 的行，count 相同。
    graph.rowOffsets.assign(surfaceCount + 1U, 0U);
    for (const auto& entry : edgeCounts) {
        ++graph.rowOffsets[static_cast<uint32_t>(entry.first >> 32U) + 1U];
        ++graph.rowOffsets[static_cast<uint32_t>(entry.first) + 1U];
    }
    std::partial_sum(graph.rowOffsets.begin(), graph.rowOffsets.end(),
                     graph.rowOffsets.begin());
    graph.edges.resize(graph.rowOffsets.back());
    std::vector<uint32_t> cursor = graph.rowOffsets;
    for (const auto& entry : edgeCounts) {
        const uint32_t a = static_cast<uint32_t>(entry.first >> 32U);
        const uint32_t b = static_cast<uint32_t>(entry.first);
        graph.edges[cursor[a]++] = {b, entry.second};
        graph.edges[cursor[b]++] = {a, entry.second};
    }

    // 步骤 6：闭合性检验：对每个有效 side，(sky + ground + 全部邻边 count) / denominator 理论上应为 1，
    //         取最大偏差记入 diagnostics.maxClosureError（仅诊断，不修正图）。
    float closureError = 0.0f;
    for (uint32_t side = 0; side < surfaceCount; ++side) {
        if (graph.groundCounts[side] == facetvk::kInactiveSide) continue;
        uint64_t visible = uint64_t(graph.skyCounts[side]) + graph.groundCounts[side];
        for (uint32_t edgeIndex = graph.rowOffsets[side];
             edgeIndex < graph.rowOffsets[side + 1U]; ++edgeIndex) {
            visible += graph.edges[edgeIndex].count;
        }
        if (graph.denominators[side] != 0U) {
            const float closure = static_cast<float>(visible) /
                                  static_cast<float>(graph.denominators[side]);
            closureError = std::max(closureError, std::abs(closure - 1.0f));
        }
    }
    graph.diagnostics.facetCount = scene.facetCount();
    graph.diagnostics.surfaceCount = surfaceCount;
    graph.diagnostics.directedEdgeCount = static_cast<uint32_t>(graph.edges.size());
    graph.diagnostics.fragmentOverflow = overflow;
    graph.diagnostics.maxClosureError = closureError;
    // A-buffer 层数不够会丢片元、破坏闭合性，这里直接报错而不是静默返回错误的图。
    if (overflow != 0U) {
        throw std::runtime_error("CPU A-buffer overflow");
    }
    return graph;
}

// 模块：CPU 版受太阳直射比例（sunlit fraction）计算。
// 作用：从太阳方向光栅化，每像素最近的中心片元视为被太阳照亮，
//       统计每个 side 的“被照亮样本数 / 总样本数”。亚像素抖动 4×4 共 16 次以降低混叠。
// 输出：长度 2*facetCount 的数组；没有任何样本的 side 为 NaN（由 fillUnsampledSunlit 补全）。
std::vector<float> computeCpuSunlit(const Scene& scene)
{
    CpuRasterizer raster(scene, scene.visibilityRasterSize,
                         scene.visibilityRasterSize,
                         scene.visibilityRasterLayers);
    const size_t sideCount = static_cast<size_t>(scene.facetCount()) * 2U;
    // total：每个 side 在太阳视角下被覆盖的样本总数；visible：其中处于最近层（被照亮）的样本数。
    std::unique_ptr<std::atomic<uint32_t>[]> total(new std::atomic<uint32_t>[sideCount]());
    std::unique_ptr<std::atomic<uint32_t>[]> visible(new std::atomic<uint32_t>[sideCount]());
    const uint32_t layers = raster.layers();
    const auto& fragments = raster.fragments();
    // Stratified sub-pixel jitter, as in FacetrtVulkan::computeSunlitFraction.
    constexpr uint32_t kSunJitter = 4U;
    for (uint32_t jitter = 0; jitter < kSunJitter * kSunJitter; ++jitter) {
        const float jitterX = (static_cast<float>(jitter % kSunJitter) + 0.5f) /
            static_cast<float>(kSunJitter) - 0.5f;
        const float jitterY = (static_cast<float>(jitter / kSunJitter) + 0.5f) /
            static_cast<float>(kSunJitter) - 0.5f;
        if (raster.rasterize(sunDirection(scene), jitterX, jitterY) != 0U) {
            throw std::runtime_error("CPU sunlight A-buffer overflow");
        }
        parallelRanges(raster.pixelCount(), 4096U, [&](size_t begin, size_t end) {
        for (size_t pixel = begin; pixel < end; ++pixel) {
            const uint32_t count = raster.count(pixel);
            if (count == 0U) {
                continue;
            }
            // 该像素所有中心片元都计入 total；只有最近的那个（且为中心块）计入 visible。
            // 周期副本只参与“最近”的遮挡判断，不累加自己的计数。
            const Fragment* nearest = nullptr;
            for (uint32_t layer = 0; layer < count; ++layer) {
                const Fragment& fragment = fragments[pixel * layers + layer];
                if (fragment.central)
                    total[fragment.cameraSide].fetch_add(1U, std::memory_order_relaxed);
                if (!nearest || fragmentBefore(fragment, *nearest)) nearest = &fragment;
            }
            if (nearest && nearest->central)
                visible[nearest->cameraSide].fetch_add(1U, std::memory_order_relaxed);
        }
        });
    }
    // NaN marks a side with no sample; fillUnsampledSunlit estimates it.
    std::vector<float> fraction(sideCount, std::numeric_limits<float>::quiet_NaN());
    for (size_t side = 0; side < sideCount; ++side) {
        const uint32_t sampled = total[side].load();
        if (sampled != 0U) {
            fraction[side] = static_cast<float>(visible[side].load()) / sampled;
        }
    }
    return fraction;
}

// 补全未采样受光比例用的空间哈希：整数网格单元（1 m）的键及其哈希函数。
struct SunCellKey {
    int x, y, z;
    bool operator==(const SunCellKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct SunCellKeyHash {
    size_t operator()(const SunCellKey& key) const noexcept {
        return (static_cast<size_t>(key.x) * 73856093U) ^
               (static_cast<size_t>(key.y) * 19349663U) ^
               (static_cast<size_t>(key.z) * 83492791U);
    }
};

// A side that received no sun-raster sample is not known to be shaded.
// Estimate it from sampled sun-facing sides nearby (1 m cells, then the
// 3x3x3 neighbourhood, then the scene mean); sides facing away get 0.
void fillUnsampledSunlit(const Scene& scene, std::vector<float>& sunlit)
{
    const facetvk::Direction direction = sunDirection(scene);
    const Vec3 sun{direction.x, direction.y, direction.z};
    constexpr float kCell = 1.0f;
    struct Accumulator { double sum{}; uint32_t count{}; };
    std::unordered_map<SunCellKey, Accumulator, SunCellKeyHash> cells;
    Accumulator global;
    auto centroidCell = [&](uint32_t facet) {
        const auto& a = scene.vertices[facet * 3U].position;
        const auto& b = scene.vertices[facet * 3U + 1U].position;
        const auto& c = scene.vertices[facet * 3U + 2U].position;
        return SunCellKey{
            static_cast<int>(std::floor((a[0] + b[0] + c[0]) / (3.0f * kCell))),
            static_cast<int>(std::floor((a[1] + b[1] + c[1]) / (3.0f * kCell))),
            static_cast<int>(std::floor((a[2] + b[2] + c[2]) / (3.0f * kCell)))};
    };
    auto facesSun = [&](uint32_t side) {
        const facetvk::Vertex& vertex = scene.vertices[(side / 2U) * 3U];
        const float sign = (side & 1U) == 0U ? 1.0f : -1.0f;
        return sign * (vertex.normal[0] * sun[0] + vertex.normal[1] * sun[1] +
                       vertex.normal[2] * sun[2]) > 0.0f;
    };
    // 步骤 1：把已有样本的、朝向太阳的 side 的受光比例累加到所在单元和全局，用于后续估计。
    bool anyMissing = false;
    for (uint32_t side = 0; side < sunlit.size(); ++side) {
        if (std::isnan(sunlit[side])) { anyMissing = true; continue; }
        if (!facesSun(side)) continue;
        Accumulator& cell = cells[centroidCell(side / 2U)];
        cell.sum += sunlit[side];
        ++cell.count;
        global.sum += sunlit[side];
        ++global.count;
    }
    // 步骤 2：对缺样本的 side：背向太阳取 0；否则依次取同单元均值、3×3×3 邻域均值、全局均值，
    //         全局也没有样本时取 1（缺省视为被照亮）。
    if (!anyMissing) return;
    for (uint32_t side = 0; side < sunlit.size(); ++side) {
        if (!std::isnan(sunlit[side])) continue;
        if (!facesSun(side)) { sunlit[side] = 0.0f; continue; }
        const SunCellKey key = centroidCell(side / 2U);
        const auto own = cells.find(key);
        if (own != cells.end() && own->second.count > 0U) {
            sunlit[side] = static_cast<float>(own->second.sum / own->second.count);
            continue;
        }
        Accumulator nearby;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto found = cells.find({key.x + dx, key.y + dy, key.z + dz});
                    if (found == cells.end()) continue;
                    nearby.sum += found->second.sum;
                    nearby.count += found->second.count;
                }
        const Accumulator& source = nearby.count > 0U ? nearby : global;
        sunlit[side] = source.count > 0U
            ? static_cast<float>(source.sum / source.count) : 1.0f;
    }
}

// 模块：CPU 版辐射度（radiosity）迭代求解。
// 作用：在可见性图上解 B = E + ρ·H(同侧) + τ·H(对侧)，其中入射 H = 图上邻居 B 的加权平均
//       + 天空 skyRadiosity·skyCounts/分母 + 等效地面 + 直射辐照 directIrradiance。
// 输入：graph、每侧光学量 optics（emission/reflectance/transmittance/directIrradiance）、
//       skyRadiosity（天空入射）、maximumIterations、relaxation（欠松弛系数）。
// 输出：radiosity（每个 side）、迭代次数、最后一次迭代的最大变化 maxDelta、等效地面辐射度。
// 说明：固定迭代次数，不做收敛提前退出；用 Jacobi 式（double buffer）更新，便于并行。
facetvk::SolveResult solveCpu(const CpuGraph& graph,
                              const std::vector<facetvk::SurfaceOptics>& optics,
                              const Progress& progress,
                              float skyRadiosity,
                              uint32_t maximumIterations = kIterations,
                              float relaxation = 1.0f)
{
    const uint32_t surfaceCount = graph.diagnostics.surfaceCount;
    std::vector<float> current(surfaceCount, 0.0f);
    std::vector<float> next(surfaceCount, 0.0f);
    float maxDelta = 0.0f;
    maximumIterations = std::max(1U, maximumIterations);
    // relaxation 限制在 [0.01, 1]：1 为纯 Jacobi，小于 1 为欠松弛（B ← B + ω·(新 − 旧)）。
    relaxation = std::clamp(relaxation, 0.01f, 1.0f);

    float groundValue = skyRadiosity;
    // 入射漫射 H(side)：(sky 计数·天空 + ground 计数·等效地面 + Σ 邻边 count·邻居 B) / 分母。
    // 无效 side（地面下表面）返回 0；分母为 0 的 side 视为只看见天空。
    auto incoming = [&](uint32_t side) {
        const uint32_t ground = graph.groundCounts[side];
        if (ground == facetvk::kInactiveSide) return 0.0f;
        const uint32_t denominator = graph.denominators[side];
        // DEFERRED[中] 物理: 没有任何栅格样本的面（亚像素小面元，冠层中很常见）被当作完全开阔、只接收天空辐射，冠层内部会系统性高估入射与吸收（长波同理）；建议：统计并报告此类面数量，用邻近面/空间单元的平均可见性估计，类似 fillUnsampledSunlit。
        if (denominator == 0U) {
            return skyRadiosity;
        }
        double value = static_cast<double>(graph.skyCounts[side]) *
                       skyRadiosity;
        if (ground != 0U) value += static_cast<double>(ground) * groundValue;
        for (uint32_t edgeIndex = graph.rowOffsets[side];
             edgeIndex < graph.rowOffsets[side + 1U]; ++edgeIndex) {
            const Edge& edge = graph.edges[edgeIndex];
            value += static_cast<double>(edge.count) * current[edge.neighbor];
        }
        return static_cast<float>(value / denominator);
    };

    constexpr size_t kFacetGrain = 2048U;
    const size_t facetCount = surfaceCount / 2U;
    std::vector<float> chunkDelta((facetCount + kFacetGrain - 1U) / kFacetGrain, 0.0f);
    // 迭代主循环：每轮先由当前解更新等效地面，再按面元并行更新两侧（透射项耦合同一面元的两侧）。
    for (uint32_t iteration = 0; iteration < maximumIterations; ++iteration) {
        // Equivalent ground of the current iterate, as ground_mean.comp.
        groundValue = facetvk::groundBoundaryRadiosity(graph.ground, current, skyRadiosity);
        parallelRanges(facetCount, kFacetGrain, [&](size_t begin, size_t end) {
        float delta = 0.0f;
        for (uint32_t facet = static_cast<uint32_t>(begin); facet < end; ++facet) {
            const uint32_t side0 = facet * 2U;
            const uint32_t side1 = side0 + 1U;
            const float h0 = incoming(side0) + optics[side0].directIrradiance;
            const float h1 = incoming(side1) + optics[side1].directIrradiance;
            const float candidate0 = optics[side0].emission +
                optics[side0].reflectance * h0 + optics[side0].transmittance * h1;
            const float candidate1 = optics[side1].emission +
                optics[side1].reflectance * h1 + optics[side1].transmittance * h0;
            next[side0] = current[side0] + relaxation * (candidate0 - current[side0]);
            next[side1] = current[side1] + relaxation * (candidate1 - current[side1]);
            delta = std::max(delta, std::abs(next[side0] - current[side0]));
            delta = std::max(delta, std::abs(next[side1] - current[side1]));
        }
        chunkDelta[begin / kFacetGrain] = delta;
        });
        maxDelta = chunkDelta.empty() ? 0.0f
            : *std::max_element(chunkDelta.begin(), chunkDelta.end());
        current.swap(next);
        // 每 8 轮报告一次进度（80–100%）。
        if ((iteration & 7U) == 7U) {
            progress(80 + static_cast<int>(20U * (iteration + 1U) / maximumIterations),
                     "CPU radiosity iteration " + std::to_string(iteration + 1U) + "/" +
                         std::to_string(maximumIterations));
        }
    }
    facetvk::SolveResult result{std::move(current), maximumIterations, maxDelta};
    result.groundRadiosity = facetvk::groundBoundaryRadiosity(graph.ground, result.radiosity, skyRadiosity);
    return result;
}

// 作用：由已解出的辐射度计算每个 side 的总入射辐照度（漫射 + 直射），供后续能量平衡使用。
// 输入：可见性图 graph、辐射度解 solution、光学量 optics、天空辐射 skyRadiosity。
// 输出：每个 side 的入射辐照度（≥ 0）。无效 side（地面下表面）只保留直射项；
//       分母为 0 的 side 漫射项取 skyRadiosity。尺寸不一致时抛 invalid_argument。
std::vector<float> incidentIrradianceCpu(
    const CpuGraph& graph,
    const facetvk::SolveResult& solution,
    const std::vector<facetvk::SurfaceOptics>& optics,
    float skyRadiosity)
{
    const uint32_t surfaceCount = graph.diagnostics.surfaceCount;
    if (solution.radiosity.size() != surfaceCount || optics.size() != surfaceCount) {
        throw std::invalid_argument("CPU incident irradiance input size mismatch");
    }
    std::vector<float> irradiance(surfaceCount, 0.0f);
    const double groundValue = facetvk::groundBoundaryRadiosity(graph.ground, solution.radiosity, skyRadiosity);
    parallelRanges(surfaceCount, 4096U, [&](size_t begin, size_t end) {
    for (uint32_t side = static_cast<uint32_t>(begin); side < end; ++side) {
        if (graph.groundCounts[side] == facetvk::kInactiveSide) {
            irradiance[side] = std::max(0.0f, optics[side].directIrradiance);
            continue;
        }
        double diffuse = skyRadiosity;
        const uint32_t denominator = graph.denominators[side];
        if (denominator != 0U) {
            diffuse = static_cast<double>(graph.skyCounts[side]) * skyRadiosity +
                static_cast<double>(graph.groundCounts[side]) * groundValue;
            for (uint32_t edgeIndex = graph.rowOffsets[side];
                 edgeIndex < graph.rowOffsets[side + 1U]; ++edgeIndex) {
                const Edge& edge = graph.edges[edgeIndex];
                diffuse += static_cast<double>(edge.count) *
                           solution.radiosity[edge.neighbor];
            }
            diffuse /= denominator;
        }
        irradiance[side] = std::max(
            0.0f, static_cast<float>(diffuse) + optics[side].directIrradiance);
    }
    });
    return irradiance;
}

// 作用：复制光学量并把直射辐照置 0（仍保留 emission），用于求“仅漫射”基线解。
std::vector<facetvk::SurfaceOptics> withoutDirectLight(
    const std::vector<facetvk::SurfaceOptics>& optics)
{
    std::vector<facetvk::SurfaceOptics> diffuse = optics;
    for (facetvk::SurfaceOptics& surface : diffuse) {
        surface.directIrradiance = 0.0f;
    }
    return diffuse;
}

// Optical spectral optics without the beam: the emission of an optical band
// only carries the Hapke beam-albedo term (makeSpectralOptics), so it goes too.
std::vector<facetvk::SurfaceOptics> withoutOpticalDirectLight(
    const std::vector<facetvk::SurfaceOptics>& optics)
{
    std::vector<facetvk::SurfaceOptics> diffuse = optics;
    for (facetvk::SurfaceOptics& surface : diffuse) {
        surface.directIrradiance = 0.0f;
        surface.emission = 0.0f;
    }
    return diffuse;
}

// 作用：直射增强量 = max(0, 含直射辐射度 − 仅漫射辐射度)，逐 side；两者长度必须一致。
std::vector<float> lightEnhancement(
    const std::vector<float>& illuminated,
    const std::vector<float>& diffuseOnly)
{
    if (illuminated.size() != diffuseOnly.size()) {
        throw std::runtime_error("Radiosity baseline size mismatch");
    }
    std::vector<float> enhancement(illuminated.size());
    for (size_t index = 0; index < illuminated.size(); ++index) {
        enhancement[index] = std::max(0.0f, illuminated[index] - diffuseOnly[index]);
    }
    return enhancement;
}

// 模块：CPU 光栅路径的完整宽波段（单波段）求解流程，用于无 GPU 或对照验证。
// 输入：场景 scene、进度回调；输出：RunResult（图诊断、sunlit、辐射度解、直射增强、耗时）。
// 步骤：可见性图 → 受光比例 → 光学量 + 辐射度求解 → 无直射基线求解并取差。
RunResult runCpu(const Scene& scene, const Progress& progress)
{
    RunResult result;
    result.backend = "CPU raster";
    const auto totalBegin = Clock::now();
    progress(2, "Preparing CPU raster buffers");

    const auto visibilityBegin = Clock::now();
    // 步骤 1：构建可见性图（进度 2–70%）。
    const CpuGraph graph = buildCpuGraph(scene, progress);
    const auto visibilityEnd = Clock::now();
    result.graph = graph.diagnostics;
    result.visibilityMs = milliseconds(visibilityBegin, visibilityEnd);

    // 步骤 2：太阳方向光栅化得到受光比例，缺样本的 side 用邻近值补全。
    progress(74, "CPU sunlight raster");
    const auto sunlightBegin = Clock::now();
    result.sunlit = computeCpuSunlit(scene);
    fillUnsampledSunlit(scene, result.sunlit);
    const auto sunlightEnd = Clock::now();
    result.sunlightMs = milliseconds(sunlightBegin, sunlightEnd);

    // 步骤 3：由受光比例生成每侧光学量，并求含直射的辐射度解。
    const std::vector<facetvk::SurfaceOptics> optics = makeOptics(scene, result.sunlit);
    progress(80, "CPU radiosity solve");
    const auto solveBegin = Clock::now();
    // Same sky as the spectral path (Scene::skyDiffuseIrradiance; the legacy
    // broadband path previously used 100 W m-2 against 50 in the spectral path).
    result.solution = solveCpu(graph, optics, progress, scene.skyDiffuseIrradiance);
    // 步骤 4：去掉直射再解一次作为漫射基线（进度回调置空），两者之差为直射增强。
    progress(98, "CPU diffuse baseline and direct-light enhancement");
    const facetvk::SolveResult diffuseSolution =
        solveCpu(graph, withoutDirectLight(optics),
                 [](int, const std::string&) {}, scene.skyDiffuseIrradiance);
    result.lightEnhancement =
        lightEnhancement(result.solution.radiosity, diffuseSolution.radiosity);
    const auto solveEnd = Clock::now();
    result.solveMs = milliseconds(solveBegin, solveEnd);
    result.totalMs = milliseconds(totalBegin, solveEnd);
    progress(100, "CPU complete");
    return result;
}

// Diagnostic (HISTREAM_FACET_HAPKE_AUDIT=1, Hapke background only): ground
// energy closure of the optical spectral radiosity solve. On every upward
// background side the solved incident field is split into the beam E and the
// diffuse part H (sky + scene; the soil-facing back side receives nothing).
// The conserving convention reflects base*A(mu0)/A_bh * E + base * H; the
// ground absorbs the rest. Reported per band, area weighted and normalized by
// the band reference irradiance (scene incident = 1 per unit domain area):
// ground incident, the solve's re-emitted flux, the conserving flux, the
// closure residual (solve - conserving) / ground incident and per unit domain
// area, and the residual of the FacetEB energy-balance mixture (one flux-
// weighted albedo for beam and sky, also applied to scene reflections).
void reportHapkeEnergyClosure(const Scene& scene, const FacetRTParameters& parameters,
                              const std::vector<float>& sunlit, const std::vector<float>& output)
{
    const char* flag = std::getenv("HISTREAM_FACET_HAPKE_AUDIT");
    if (!flag || std::string(flag) != "1" || !(scene.backgroundAngularStrength > 0.0f)) return;
    const facetvk::Direction direction = sunDirection(scene);
    const Vec3 sun{direction.x, direction.y, direction.z};
    const float cosineSolarZenith = std::max(0.0f, sun[1]);
    const size_t n = size_t(scene.facetCount()) * 2U;
    for (size_t band = 0; band < parameters.wavelengths.size(); ++band) {
        if (!isOpticalWavelength(parameters.wavelengths[band])) continue;
        const float beam = bandBeamIrradiance(scene, parameters, band);
        const float sky = bandSkyIrradiance(scene, parameters, band);
        const float reference = beam * cosineSolarZenith + sky;
        if (!(reference > 1.0e-8f) || output.size() < (band + 1U) * n) continue;
        const std::vector<facetvk::SurfaceOptics> optics =
            makeSpectralOptics(scene, sunlit, parameters, band);
        double area = 0.0, incident = 0.0, beamSum = 0.0, solved = 0.0;
        double conserving = 0.0, mixture = 0.0, litArea = 0.0;
        for (uint32_t facet = scene.leafFacetCount; facet < scene.facetCount(); ++facet) {
            const facetvk::Vertex& vertex = scene.vertices[facet * 3U];
            const Vec3 normal{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
            const uint32_t localSide = normal[1] >= 0.0f ? 0U : 1U;
            const uint32_t side = facet * 2U + localSide;
            const Vec3 sideNormal = multiply(normal, localSide == 0U ? 1.0f : -1.0f);
            const facetvk::SurfaceOptics& o = optics[side];
            const float a = facetArea(scene, facet);
            const SurfaceParameters& surface = surfaceForFacet(scene, facet);
            const float lambert = std::clamp(
                bandValue(surface.reflectance, band, 0.20f) * reflectanceTextureFactor(scene, facet),
                0.0f, 1.0f);
            const float base = std::clamp(lambert, 0.0f, 0.999f);
            const double scale = 1.0 / reference;
            const double raw = double(output[band * n + side]) / scale;
            const double e = o.directIrradiance;
            const double h = o.reflectance > 0.0f
                ? (raw - o.reflectance * e - o.emission) / o.reflectance : 0.0;
            const float cosine = std::max(0.0f, dot(normalized(sideNormal), sun));
            const double beamAlbedo = backgroundHapkeEnergyReflectance(
                lambert, cosine, 1.0f, 0.0f, scene.backgroundAngularStrength);
            const double mixAlbedo = backgroundHapkeEnergyReflectance(
                lambert, cosine, float(e), sky, scene.backgroundAngularStrength);
            area += a;
            incident += a * (e + h) * scale;
            beamSum += a * e * scale;
            solved += a * raw * scale;
            conserving += a * (beamAlbedo * e + base * h) * scale;
            mixture += a * mixAlbedo * (e + h) * scale;
            if (sunlit[side] > 0.5f) litArea += a;
        }
        if (!(area > 0.0) || !(incident > 0.0)) continue;
        std::ostringstream line;
        line << std::setprecision(6) << "HAPKE_ENERGY band=" << parameters.wavelengths[band]
             << " groundArea=" << area << " litFraction=" << litArea / area
             << " groundIncident=" << incident / area << " groundBeam=" << beamSum / area
             << " reflectedSolve=" << solved / area << " reflectedConserving=" << conserving / area
             << " closureResidual=" << (solved - conserving) / incident
             << " sceneResidual=" << (solved - conserving) / area
             << " energyBalanceMixtureResidual=" << (mixture - conserving) / incident;
        std::cout << line.str() << std::endl;
    }
}

// 模块：GPU 常驻的多波段（光谱）求解。逐波段调用 solveSpectralBand，光学量/状态在 GPU 上生成。
// 输入：已加载几何与可见性图的 model、scene、parameters（波长列表、求解器类型、天空温度等）。
// 输出：写入 result：sunlit、各波段迭代数、solution（取第 0 波段）、bandRadiosity、
//       bandStates、lightEnhancement，以及 quantity/units 说明。
// 约定：光学波段输出为表观反射率（除以 参考辐照 = beam·cosθs + sky）；
//       热红外波段输出为光谱辐亮度 W m-2 sr-1 um-1，scale 取 1。
// 环境变量：HISTREAM_FACET_VALIDATE_SPECTRAL=1 时对每个波段做 GPU 与 CPU 参考的逐项校验。
void runGpuSpectral(facetvk::FacetrtVulkan& model, const Scene& scene, const FacetRTParameters& parameters,
                    const Progress& progress, RunResult& result)
{
    prepareGpuSpectral(model, scene, result.sunlit, parameters);
    result.gpuSpectral = true;
    result.wavelengths = parameters.wavelengths;
    const char* auditFlag = std::getenv("HISTREAM_FACET_VALIDATE_SPECTRAL");
    const bool auditEnabled = auditFlag && std::string(auditFlag) == "1";
    std::vector<float> previousAuditRaw;
    bool previousThermal = false, allThermal = true, anyThermal = false;
    const float cosineSolarZenith = std::max(0.0f, sunDirection(scene).y);
    for (size_t band = 0; band < parameters.wavelengths.size(); ++band) {
        const float wavelength = parameters.wavelengths[band];
        const bool thermal = !isOpticalWavelength(wavelength);
        // 加速求解器时，上一波段与本波段同为光学/同为热红外则用上一波段的解作初值（热启动）。
        const bool warm = parameters.acceleratedSolver && band > 0 && previousThermal == thermal;
        allThermal = allThermal && thermal; anyThermal = anyThermal || thermal;
        // Per-band light from the project settings (configureFacetRTLight);
        // thermal bands below 6 um: the absolute solar beam and sky (radiance).
        const std::array<float, 2> thermalSolar = thermal
            ? facetRtThermalSolar(scene, wavelength) : std::array<float, 2>{0.0f, 0.0f};
        const float beam = thermal ? thermalSolar[0] : bandBeamIrradiance(scene, parameters, band);
        const float reference = beam * cosineSolarZenith + bandSkyIrradiance(scene, parameters, band);
        // 光学波段 scale = 1/参考辐照（把输出归一为表观反射率）；热红外或参考辐照过小时不缩放。
        // 热红外波段的天空项为天空温度的黑体辐亮度 + thermalSolar[1]（6 um 以下的太阳天空部分）。
        const float scale = thermal || reference <= 1e-8f ? 1.0f : 1.0f / reference;
        const float sky = thermal
            ? planckRadiance(wavelength, parameters.skyTemperature) + thermalSolar[1]
            : bandSkyIrradiance(scene, parameters, band);
        progress(82 + static_cast<int>(15U * band / parameters.wavelengths.size()),
                 "GPU resident spectral solve " + std::to_string(band + 1U) + "/" + std::to_string(parameters.wavelengths.size()));
        const auto stats = model.solveSpectralBand(static_cast<uint32_t>(band), thermal, sky, scale,
                                                   parameters.acceleratedSolver, warm, false, beam);
        result.spectralIterations.push_back(stats.iterations);
        if (band == 0) {
            result.solution.iterations = stats.iterations; result.solution.maxDelta = stats.maxDelta;
        }
        // 校验分支（仅 HISTREAM_FACET_VALIDATE_SPECTRAL=1）：用 CPU 重新生成光学量并用同一图、
        // 同一 sunlit 求参考解，与 GPU 的光学量、原始辐射度、输出和表面状态逐项比较；
        // 超出容差（5e-5 + 5e-6·|cpu|）抛 runtime_error。参考解只用于比较，不写入 GPU 输出。
        if (auditEnabled) {
            const auto audit = model.readSpectralAudit(static_cast<uint32_t>(band));
            std::vector<std::array<float, 2>> emission;
            std::vector<ImageReflection> image;
            const auto optics = makeSpectralOptics(scene, result.sunlit, parameters, band, &emission, &image);
            const auto check = [](float gpu, float cpu, float& maximum) {
                const float error = std::abs(gpu - cpu);
                maximum = std::max(maximum, error);
                if (!std::isfinite(gpu) || error > 5.0e-5f + 5.0e-6f * std::abs(cpu))
                    throw std::runtime_error("FacetRT GPU spectral shadow mismatch");
            };
            for (size_t side = 0; side < optics.size(); ++side) {
                const auto& a = audit.optics[side]; const auto& b = optics[side];
                check(a.reflectance, b.reflectance, result.spectralOpticsError);
                check(a.transmittance, b.transmittance, result.spectralOpticsError);
                check(a.emission, b.emission, result.spectralOpticsError);
                check(a.directIrradiance, b.directIrradiance, result.spectralOpticsError);
                for (size_t state = 0; state < 2; ++state)
                    check(audit.emission[side][state], emission[side][state], result.spectralOpticsError);
            }
            // Same visibility graph, sunlit mask and preceding physical band.
            // Reference results are compared, never substituted into GPU output.
            const auto referenceSolve = parameters.acceleratedSolver
                ? model.solveAccelerated(optics, sky, warm ? &previousAuditRaw : nullptr, kIterations, 8U, 1e-4f, 1.0f)
                : model.solve(optics, sky, kIterations, 1.0f);
            result.spectralIterationError = std::max(result.spectralIterationError,
                stats.iterations > referenceSolve.iterations ? stats.iterations - referenceSolve.iterations
                                                            : referenceSolve.iterations - stats.iterations);
            std::vector<float> states;
            appendSurfaceStates(scene, optics, emission, referenceSolve.radiosity,
                                beam, scale, !thermal, states, &image);
            for (size_t side = 0; side < optics.size(); ++side) {
                check(audit.raw[side], referenceSolve.radiosity[side], result.spectralRawError);
                const float value = thermal ? referenceSolve.radiosity[side]
                    : std::clamp(referenceSolve.radiosity[side] * scale, 0.0f, 1.0f);
                check(audit.output[side], value, result.spectralOutputError);
            }
            for (size_t k = 0; k < states.size(); ++k) check(audit.states[k], states[k], result.spectralStatesError);
            previousAuditRaw = audit.raw;
            // 校验用的参考解可能改写了 GPU 上的光学量，这里恢复（具体机制待核对）。
            model.restoreSpectralOptics(audit.optics);
            ++result.spectralBandsChecked;
        }
        // 第 0 波段（光学）额外求一次“仅漫射”基线（solveSpectralBand 的 diffuse = true），
        // 供 readSpectralOutput 末段的 lightEnhancement 使用。
        if (band == 0 && !thermal) {
            model.solveSpectralBand(0, false, sky, scale, parameters.acceleratedSolver, false, true, beam);
        }
        previousThermal = thermal;
    }
    // 读回 GPU 输出，布局（n = 2·facetCount，B = 波段数，bandValues = n·B）：
    // [0, bandValues) 各波段辐射度；[bandValues, 3·bandValues) 各波段表面状态；
    // [3·bandValues, 末尾) 直射增强。第 0 波段的 n 个值同时作为 solution.radiosity。
    const auto output = model.readSpectralOutput();
    reportHapkeEnergyClosure(scene, parameters, result.sunlit, output);
    const size_t n = size_t(scene.facetCount()) * 2U, bandValues = n * parameters.wavelengths.size();
    result.solution.radiosity.assign(output.begin(), output.begin() + n);
    if (parameters.wavelengths.size() > 1)
        result.bandRadiosity.assign(output.begin(), output.begin() + bandValues);
    result.bandStates.assign(output.begin() + bandValues, output.begin() + 3U * bandValues);
    result.lightEnhancement.assign(output.begin() + 3U * bandValues, output.end());
    // 按波段构成标注输出物理量与单位。
    if (allThermal) { result.quantity = "spectral radiance"; result.units = "W m-2 sr-1 um-1"; }
    else if (!anyThermal) { result.quantity = "reflectance"; result.units = "1"; }
    else { result.quantity = "reflectance and spectral radiance"; result.units = "band-dependent"; }
}

// 模块：Vulkan GPU 路径的完整流程。spectralParameters 为空时走单波段（宽波段）求解，
//       非空时走光谱求解；返回含耗时、图诊断、sunlit、辐射度与直射增强的 RunResult。
// 步骤：建设备/上传几何 → GPU 构建可见性图 → 受光比例 → 求解（见下方三个分支）。
RunResult runGpu(const Scene& scene,
                 const std::string& shaderDirectory,
                 const Progress& progress,
                 const FacetRTParameters* spectralParameters = nullptr)
{
    RunResult result;
    result.backend = "Vulkan GPU";
    const auto totalBegin = Clock::now();
    progress(3, "Creating Vulkan device and GPU buffers");
    // 步骤 1：用场景的光栅尺寸、每像素最大片元层数、周期邻块数与周期尺寸配置 GPU 模型。
    facetvk::Config config{};
    config.rasterWidth = scene.visibilityRasterSize;
    config.rasterHeight = scene.visibilityRasterSize;
    config.maxFragmentsPerPixel = scene.visibilityRasterLayers;
    config.enableValidation = false;
    config.periodicNeighborCount = scene.periodicNeighborCount;
    config.periodicSizeX = scene.periodicSizeX;
    config.periodicSizeZ = scene.periodicSizeZ;

    facetvk::FacetrtVulkan model;
    model.initialize(config, shaderDirectory);
    model.setGeometry(scene.vertices);
    model.setGroundFacets(scene.leafFacetCount);

    // 步骤 2：GPU 构建半球方向的可见性图（进度 10–70%）。
    progress(10, "GPU visibility graph: 40 directions at " +
                     std::to_string(scene.visibilityRasterSize) + " x " +
                     std::to_string(scene.visibilityRasterSize));
    const auto visibilityBegin = Clock::now();
    result.graph = model.buildVisibilityGraph(makeHemisphereDirections());
    const auto visibilityEnd = Clock::now();
    progress(70, groundEscapeLog(result.graph));
    result.visibilityMs = milliseconds(visibilityBegin, visibilityEnd);

    // 步骤 3：太阳方向的受光比例，缺样本的 side 用邻近值补全（与 CPU 路径相同）。
    progress(72, "GPU sunlight visibility raster");
    const auto sunlightBegin = Clock::now();
    result.sunlit = model.computeSunlitFraction(sunDirection(scene));
    fillUnsampledSunlit(scene, result.sunlit);
    const auto sunlightEnd = Clock::now();
    result.sunlightMs = milliseconds(sunlightBegin, sunlightEnd);

    const auto solveBegin = Clock::now();
    // 步骤 4：求解有三个分支：
    //   a) 单波段：GPU Jacobi 求含直射解，再求去直射的漫射基线，两者之差为直射增强；
    //   b) 光谱且未指定 HISTREAM_FACET_SPECTRAL_BACKEND=cpu：runGpuSpectral（光学量在 GPU 上生成）；
    //   c) 光谱且指定为 cpu：光学量/状态在 CPU 上逐波段生成，仍调用 GPU 的 solve 求辐射度。
    if (spectralParameters == nullptr) {
        progress(82, "GPU Jacobi radiosity: 64 iterations");
        const std::vector<facetvk::SurfaceOptics> optics =
            makeOptics(scene, result.sunlit);
        result.solution =
            model.solve(optics, scene.skyDiffuseIrradiance, kIterations, 1.0f);
        progress(98, "GPU diffuse baseline and direct-light enhancement");
        const facetvk::SolveResult diffuseSolution =
            model.solve(withoutDirectLight(optics), scene.skyDiffuseIrradiance,
                        kIterations, 1.0f);
        result.lightEnhancement =
            lightEnhancement(result.solution.radiosity,
                             diffuseSolution.radiosity);
    } else if (const char* selected = std::getenv("HISTREAM_FACET_SPECTRAL_BACKEND");
               selected == nullptr || std::string(selected) != "cpu") {
        runGpuSpectral(model, scene, *spectralParameters, progress, result);
    } else {
        result.wavelengths = spectralParameters->wavelengths;
        const size_t surfaceCount = static_cast<size_t>(scene.facetCount()) * 2U;
        const bool multiBand = spectralParameters->wavelengths.size() > 1U;
        if (multiBand) {
            result.bandRadiosity.reserve(
                surfaceCount * spectralParameters->wavelengths.size());
        }
        result.lightEnhancement.assign(surfaceCount, 0.0f);
        bool allThermal = true;
        bool anyThermal = false;
        bool previousBandWasThermal = false;
        bool hasPreviousBand = false;
        std::vector<float> previousRawRadiosity;
        const float cosineSolarZenith =
            std::max(0.0f, sunDirection(scene).y);
        // 分支 c 的逐波段循环：生成光学量 → 求解（加速求解器可用上一波段解作初值）→
        // 收集表面状态 → 光学波段除以参考辐照转为表观反射率 → 第 0 波段额外求漫射基线。
        for (size_t band = 0; band < spectralParameters->wavelengths.size(); ++band) {
            const float wavelength = spectralParameters->wavelengths[band];
            const bool thermal = !isOpticalWavelength(wavelength);
            // Per-band light, identical to runGpuSpectral.
            const std::array<float, 2> thermalSolar = thermal
                ? facetRtThermalSolar(scene, wavelength)
                : std::array<float, 2>{0.0f, 0.0f};
            const float bandBeam = thermal ? thermalSolar[0]
                : bandBeamIrradiance(scene, *spectralParameters, band);
            const float opticalReferenceIrradiance = bandBeam * cosineSolarZenith +
                bandSkyIrradiance(scene, *spectralParameters, band);
            allThermal = allThermal && thermal;
            anyThermal = anyThermal || thermal;
            progress(82 + static_cast<int>(
                15U * band / spectralParameters->wavelengths.size()),
                std::string(spectralParameters->acceleratedSolver
                    ? "GPU accelerated spectral solve "
                    : "GPU traditional spectral solve ") +
                std::to_string(band + 1U) + "/" +
                std::to_string(spectralParameters->wavelengths.size()));
            std::vector<std::array<float, 2>> stateEmission;
            std::vector<ImageReflection> image;
            const std::vector<facetvk::SurfaceOptics> optics =
                makeSpectralOptics(scene, result.sunlit,
                                   *spectralParameters, band, &stateEmission, &image);
            const float skyRadiance = thermal
                ? planckRadiance(wavelength,
                                 spectralParameters->skyTemperature) + thermalSolar[1]
                : bandSkyIrradiance(scene, *spectralParameters, band);
            const std::vector<float>* initialRadiosity =
                spectralParameters->acceleratedSolver && hasPreviousBand &&
                previousBandWasThermal == thermal
                    ? &previousRawRadiosity : nullptr;
            facetvk::SolveResult bandSolution =
                spectralParameters->acceleratedSolver
                    ? model.solveAccelerated(
                          optics, skyRadiance, initialRadiosity,
                          kIterations, 8U, 1.0e-4f, 1.0f)
                    : model.solve(
                          optics, skyRadiance, kIterations, 1.0f);
            if (spectralParameters->acceleratedSolver) {
                // Preserve physical radiosity as the next-band initial state.
                // Apparent-reflectance conversion below is output-only.
                previousRawRadiosity = bandSolution.radiosity;
                previousBandWasThermal = thermal;
                hasPreviousBand = true;
            }
            appendSurfaceStates(scene, optics, stateEmission, bandSolution.radiosity,
                                bandBeam,
                                thermal || opticalReferenceIrradiance <= 1.0e-8f
                                    ? 1.0f : 1.0f / opticalReferenceIrradiance,
                                !thermal, result.bandStates, &image);
            // The optical solver returns outgoing radiosity.  Image products
            // require dimensionless apparent reflectance, referenced to the
            // same direct and diffuse illumination used by this solve.
            if (!thermal && opticalReferenceIrradiance > 1.0e-8f) {
                for (float& value : bandSolution.radiosity) {
                    value = std::max(
                        value / opticalReferenceIrradiance, 0.0f);
                }
            }
            if (multiBand) {
                result.bandRadiosity.insert(
                    result.bandRadiosity.end(),
                    bandSolution.radiosity.begin(), bandSolution.radiosity.end());
            }
            // 第 0 波段的解作为主解 result.solution；光学波段还用去直射的漫射解来计算直射增强。
            if (band == 0U) {
                result.solution = bandSolution;
                if (!thermal) {
                    facetvk::SolveResult diffuseSolution =
                        spectralParameters->acceleratedSolver
                            ? model.solveAccelerated(
                                  withoutOpticalDirectLight(optics), skyRadiance,
                                  nullptr, kIterations, 8U, 1.0e-4f, 1.0f)
                            : model.solve(
                                  withoutOpticalDirectLight(optics), skyRadiance,
                                  kIterations, 1.0f);
                    if (opticalReferenceIrradiance > 1.0e-8f) {
                        for (float& value : diffuseSolution.radiosity) {
                            value = std::max(
                                value / opticalReferenceIrradiance, 0.0f);
                        }
                    }
                    result.lightEnhancement = lightEnhancement(
                        bandSolution.radiosity, diffuseSolution.radiosity);
                }
            }
        }
        if (allThermal) {
            result.quantity = "spectral radiance";
            result.units = "W m-2 sr-1 um-1";
        } else if (!anyThermal) {
            result.quantity = "reflectance";
            result.units = "1";
        } else {
            result.quantity = "reflectance and spectral radiance";
            result.units = "band-dependent";
        }
    }
    const auto solveEnd = Clock::now();
    result.solveMs = milliseconds(solveBegin, solveEnd);
    result.totalMs = milliseconds(totalBegin, solveEnd);
    progress(100, "Vulkan GPU complete");
    return result;
}

// 作用：把 RunResult 写成 Web 前端读取的 JSON 文件（UTF-8 文本，浮点保留 9 位有效数字）。
// 内容：backend/quantity/units、执行位置（execution）、光谱校验统计、面元数与顶点位置、
//       图诊断（graph）、耗时（timing）、迭代数与 maxDelta，以及数组字段
//       wavelengths、sunlit、radiosity、lightEnhancement、bandRadiosity、bandStates。
//       bandRadiosity/bandStates 为空时不写；数组中出现非有限值会抛 runtime_error。
void writeWebResult(const std::string& fileName,
                    const Scene& scene,
                    const RunResult& result)
{
    std::ofstream output(fileName, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Cannot write web result: " + fileName);
    }
    output << std::setprecision(9)
           << "{\n"
           << "  \"backend\": \"" << result.backend << "\",\n"
           << "  \"quantity\": \"" << result.quantity << "\",\n"
           << "  \"units\": \"" << result.units << "\",\n"
           << "  \"execution\": {\"radiation\":\"" << (result.backend == "Vulkan GPU" ? "gpu" : "cpu")
           << "\",\"spectralOptics\":\"" << (result.gpuSpectral ? "gpu" : "cpu")
           << "\",\"surfaceStates\":\"" << (result.gpuSpectral ? "gpu" : "cpu")
           << "\",\"bandNormalization\":\"" << (result.gpuSpectral ? "gpu" : "cpu")
           << "\",\"spectralStateDeviceResident\":" << (result.gpuSpectral ? "true" : "false")
           << ",\"spectralConvergence\":\"" << (result.gpuSpectral ? "gpu" : "cpu") << "\""
           << ",\"spectralTables\":\"cpu once per material and band\",\"resultWriting\":\"cpu\"},\n"
           << "  \"spectralVerification\": {\"enabled\":" << (result.spectralBandsChecked > 0 ? "true" : "false")
           << ",\"bandsChecked\":" << result.spectralBandsChecked
           << ",\"status\":\"" << (result.spectralBandsChecked > 0 ? "PASS" : "not_run") << "\""
           << ",\"maxAbsOptics\":" << result.spectralOpticsError << ",\"maxAbsRawRadiosity\":" << result.spectralRawError
           << ",\"maxAbsSurfaceStates\":" << result.spectralStatesError << ",\"maxAbsOutput\":" << result.spectralOutputError
           << ",\"maxAbsIterationCountDifference\":" << result.spectralIterationError
           << ",\"sameGraphAndSunlit\":" << (result.spectralBandsChecked > 0 ? "true" : "false") << "},\n"
           << "  \"facetCount\": " << scene.facetCount() << ",\n"
           << "  \"leafFacetCount\": " << scene.leafFacetCount << ",\n"
           << "  \"vertexPositions\": [";
    for (size_t index = 0; index < scene.vertices.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        const auto& vertex = scene.vertices[index];
        output << vertex.position[0] << ',' << vertex.position[1] << ',' << vertex.position[2];
    }
    output << "],\n"
           << "  \"graph\": {"
           << "\"directedEdges\":" << result.graph.directedEdgeCount << ","
           << "\"closureError\":" << result.graph.maxClosureError << ","
           << "\"fragmentOverflow\":" << result.graph.fragmentOverflow << ","
           << "\"hashOverflow\":" << result.graph.hashOverflow << ","
           << "\"lowerBoundary\":" << groundEscapeJson(result.graph).dump() << "},\n"
           << "  \"timing\": {"
           << "\"totalMs\":" << result.totalMs << ","
           << "\"visibilityMs\":" << result.visibilityMs << ","
           << "\"sunlightMs\":" << result.sunlightMs << ","
           << "\"solveMs\":" << result.solveMs << "},\n"
           << "  \"iterations\": " << result.solution.iterations << ",\n"
           << "  \"maxDelta\": " << result.solution.maxDelta << ",\n";

    // 写一个浮点数组字段：用 to_chars 批量序列化到 64 KiB 缓冲再写出（避免逐值的 ostream 开销）。
    const auto writeArray = [&output](const char* name,
                                      const std::vector<float>& values,
                                      bool trailingComma) {
        output << "  \"" << name << "\": [";
        // Keep nine significant digits, as setprecision(9) did, while avoiding
        // millions of locale-aware ostream conversions for dense spectra.
        std::array<char, 65536> buffer{};
        char* cursor = buffer.data();
        const auto flush = [&]() {
            output.write(buffer.data(), cursor - buffer.data());
            cursor = buffer.data();
        };
        for (size_t i = 0; i < values.size(); ++i) {
            if (!std::isfinite(values[i])) throw std::runtime_error("Non-finite FacetRT output field: " + std::string(name));
            if (buffer.data() + buffer.size() - cursor < 64) flush();
            if (i != 0U) *cursor++ = ',';
            const auto written = std::to_chars(cursor, buffer.data() + buffer.size(), values[i], std::chars_format::general, 9);
            if (written.ec != std::errc{}) throw std::runtime_error("Cannot serialize FacetRT output field");
            cursor = written.ptr;
        }
        flush();
        output << ']' << (trailingComma ? "," : "") << '\n';
    };
    writeArray("wavelengths", result.wavelengths, true);
    writeArray("sunlit", result.sunlit, true);
    writeArray("radiosity", result.solution.radiosity, true);
    writeArray("lightEnhancement", result.lightEnhancement,
               !result.bandRadiosity.empty() || !result.bandStates.empty());
    if (!result.bandRadiosity.empty()) {
        writeArray("bandRadiosity", result.bandRadiosity, !result.bandStates.empty());
    }
    if (!result.bandStates.empty()) {
        writeArray("bandStates", result.bandStates, false);
    }
    output << "}\n";
}

} // namespace

namespace {

// XML 文本转义：& < > " ' 依次替换为对应实体（与后面的 decodeXmlText 互逆）。
std::string xmlEscaped(std::string value)
{
    const std::pair<const char*, const char*> entities[] = {
        {"&", "&amp;"}, {"<", "&lt;"}, {">", "&gt;"},
        {"\"", "&quot;"}, {"'", "&apos;"}
    };
    for (const auto& entity : entities) {
        size_t position = 0;
        while ((position = value.find(entity.first, position)) != std::string::npos) {
            value.replace(position, std::strlen(entity.first), entity.second);
            position += std::strlen(entity.second);
        }
    }
    return value;
}

// 把浮点数组用逗号拼接成文本（用于 <controlBand> 等标签）。
std::string joined(const std::vector<float>& values)
{
    std::ostringstream output;
    for (size_t index = 0; index < values.size(); ++index) {
        if (index) output << ',';
        output << values[index];
    }
    return output.str();
}

// 把 object["meshes"] 数组里每个网格的 key 字段（缺省 fallback）用逗号拼接；
// 没有 meshes 或结果为空时返回 fallback。用于一个对象含多个网格时的名称列表。
std::string joinedNames(const ProjectJson::Json& object, const char* key,
                        const std::string& fallback)
{
    std::ostringstream output;
    if (object.contains("meshes") && object["meshes"].is_array()) {
        size_t index = 0;
        for (const auto& mesh : object["meshes"]) {
            if (index++) output << ',';
            output << ProjectJson::string(mesh, key, fallback);
        }
    }
    return output.str().empty() ? fallback : output.str();
}

// 模块：project.json → 内存中的旧式 XML（HiStreamProject）适配层。
// 作用：把统一的 project.json（control/light/scene/objects/spectra/thermals/materials/
//       canopies/meteorology/sensor）转成 FacetRT 原有的标签式解析器所读的 XML 文本，
//       不落盘；缺省值与旧工程保持一致。
// 输入：project.json 路径；输出：XML 字符串。
// XML 结构：Control → Geometry(Light/Sensor) → Scene(Object) → Attribute(Optical/Thermal/
//       Biochemistry：soilSet、surfaceEnergy、canopyAero、nodeTimeStepsSeconds) → Meteorology。
// 异常：背景地面材料为光伏板、集总热参数非法、timeStepSeconds 不合法等情形抛 runtime_error。
// The facet solver still uses its proven tag-oriented parsing routines. This
// adapter is memory-only: project.json remains the sole file and CLI contract.
std::string facetInputView(const std::string& path)
{
    // 步骤 1：读取工程并取各节；背景地面不允许使用光伏材料（光伏板必须是场景对象）。
    const ProjectJson project = ProjectJson::load(path);
    const auto& scene = project.scene();
    const auto& light = project.light();
    const auto& sensor = project.sensor();
    const auto& control = project.control();
    const auto& meteo = project.meteorology();
    const auto background = scene.contains("background")
        ? scene["background"] : ProjectJson::Json::object();
    for(const auto& m:project.materials())
        if(ProjectJson::string(m,"energyModel")=="photovoltaic" && ProjectJson::string(m,"name")==ProjectJson::string(background,"materialName"))
            throw std::runtime_error("Photovoltaic panels must be scene objects, not background ground");
    const std::filesystem::path defined = project.runtimeDefinedDirectory();
    std::ostringstream xml;
    // 步骤 2：写 Control 段（后端、GPU 编号、递归深度、周期邻块数、耦合迭代、温度容差与松弛等）。
    // 周期邻块数：先取 periodicTraversalCount；缺省时若 periodicBoundary 为真则取
    // periodicNeighborCount（缺省 8），否则为 0；最后限幅到 [0, 20]。
    // 随后依次写 Light（天顶/方位角、天空温度、直射比例、短波辐照限幅 0–2000 W m-2）、
    // Sensor（波段）、Scene（场景尺寸、背景材料与 Hapke/Lambert 选择）。
    xml << "<HiStreamProject><Control>"
        << "<outDir>" << xmlEscaped(project.outputDirectory().string()) << "</outDir>"
        << "<facetBackend>" << xmlEscaped(ProjectJson::string(control, "facetBackend", "gpu")) << "</facetBackend>"
        << "<GPU>" << ProjectJson::integer(control, "gpu", 0) << "</GPU>"
        << "<rayTracingDepth>" << ProjectJson::integer(control, "depth", 4) << "</rayTracingDepth>"
        << "<periodicNeighborCount>"
        << std::clamp(ProjectJson::integer(
               control, "periodicTraversalCount",
               ProjectJson::boolean(control, "periodicBoundary", false)
                   ? ProjectJson::integer(control, "periodicNeighborCount", 8) : 0),
               0, 20)
        << "</periodicNeighborCount>"
        << "<couplingIterations>" << ProjectJson::integer(control, "couplingIterations", 60) << "</couplingIterations>"
        << "<temperatureTolerance>" << ProjectJson::number(control, "temperatureTolerance", 0.05f) << "</temperatureTolerance>"
        << "<stabilityCorrection>" << (ProjectJson::boolean(control, "stabilityCorrection", false) ? 1 : 0) << "</stabilityCorrection>"
        << "<temperatureRelaxation>" << ProjectJson::number(control, "temperatureRelaxation", 0.8f) << "</temperatureRelaxation>"
        << "<spectralAccelerationWidth>" << ProjectJson::number(control, "spectralAccelerationWidth", 100.0f) << "</spectralAccelerationWidth>"
        << "<radiationSolver>" << xmlEscaped(ProjectJson::string(control, "radiationSolver", "traditional")) << "</radiationSolver>"
        << "<vegetationTemperatureMethod>" << ProjectJson::integer(control, "vegetationTemperatureMethod", 0) << "</vegetationTemperatureMethod>"
        << "<soilInertiaHistory>" << (ProjectJson::string(control, "soilInertiaHistory", "truncated") == "full" ? 1 : 0) << "</soilInertiaHistory>"
        << "<isDEM>" << (ProjectJson::boolean(scene, "terrain", false) ? 1 : 0) << "</isDEM>"
        << "<isProcess>" << (ProjectJson::boolean(sensor, "process", false) ? 1 : 0) << "</isProcess>"
        << "<isRadiationProcess>" << (ProjectJson::boolean(sensor, "radiationProcess", false) ? 1 : 0) << "</isRadiationProcess>"
        << "<isEnergyProcess>" << (ProjectJson::boolean(sensor, "energyProcess", false) ? 1 : 0) << "</isEnergyProcess>"
        << "</Control><Geometry><Light><light name=\"Solar\">"
        << "<lightAngle><viewAngles>" << ProjectJson::number(light, "zenith", 30.0f)
        << ',' << ProjectJson::number(light, "azimuth", 135.0f) << "</viewAngles></lightAngle>"
        << "<skyTemperature>" << ProjectJson::number(light, "skyTemperature", 250.0f) << "</skyTemperature>"
        << "<directScatteringRatio>" << ProjectJson::number(light, "direct", 0.8f) << "</directScatteringRatio>"
        << "<shortwaveIrradiance>" << std::clamp(ProjectJson::number(light, "shortwaveIrradiance", 0.0f), 0.0f, 2000.0f)
        << "</shortwaveIrradiance>"
        << "<diffuseFractionMode>" << xmlEscaped(ProjectJson::string(light, "diffuseFractionMode", "fixed"))
        << "</diffuseFractionMode>"
        << "<esunFileName>" << xmlEscaped((defined / "Esun_.dat").string()) << "</esunFileName>"
        << "<eskyFileName>" << xmlEscaped((defined / "Esky_.dat").string()) << "</eskyFileName>"
        << "</light></Light><Sensor><sensor name=\"MainSensor\">"
        << "<controlBand>" << joined(project.sensorBands()) << "</controlBand>"
        << "</sensor></Sensor></Geometry><Scene>"
        << "<sceneSizeX>" << ProjectJson::number(scene, "x", 60.0f) << "</sceneSizeX>"
        << "<sceneSizeY>" << ProjectJson::number(scene, "y", 60.0f) << "</sceneSizeY>"
        << "<Height>" << ProjectJson::number(scene, "height", 15.0f) << "</Height>"
        << "<voxelSize>" << ProjectJson::number(scene, "voxel", 1.0f) << "</voxelSize>"
        << "<bgSpectral>" << xmlEscaped(ProjectJson::string(background, "spectralName", "soil")) << "</bgSpectral>"
        << "<bgThermal>" << xmlEscaped(ProjectJson::string(background, "thermalName", "soil_temperature")) << "</bgThermal>"
        << "<bgBioName>" << xmlEscaped(ProjectJson::string(background, "materialName", "soilset")) << "</bgBioName>"
        << "<backgroundHeterogeneity>" << (ProjectJson::boolean(background, "heterogeneityEnabled", false) ? "Hapke" : "Lambert") << "</backgroundHeterogeneity>"
        << "<backgroundAngularStrength>" << ProjectJson::number(background, "angularEffectStrength", 0.5f) << "</backgroundAngularStrength>";
    if (ProjectJson::boolean(scene, "terrain", false) && !ProjectJson::string(scene, "demFile").empty())
        xml << "<DEM>" << xmlEscaped(project.resolve(ProjectJson::string(scene, "demFile")).string()) << "</DEM>";
    // 步骤 3：对象列表；Fire、Fog 与带 medium 字段的对象不属于面元场景，跳过。
    // dimensions 不足 3 个分量时补 1。
    xml << "<Object>";
    for (const auto& object : project.objects()) {
        const std::string type = ProjectJson::string(object, "type", "Vegetation");
        if (type == "Fire" || type == "Fog" || object.contains("medium")) continue;
        const std::string name = ProjectJson::string(object, "name", "object");
        const std::string spectral = ProjectJson::string(object, "spectralName", "soil");
        const std::string thermal = ProjectJson::string(object, "thermalName", "soil_temperature");
        const std::string canopy = ProjectJson::string(object, "canopyName",
            type == "Vegetation" ? "canopy_default" : "rigid_body");
        const std::string material = ProjectJson::string(object, "materialName",
            type == "Vegetation" ? "leaf_c3" : type == "Water" ? "water_set" : "soilset");
        auto dimensions = ProjectJson::numbers(object.contains("dimensions") ? object["dimensions"] : ProjectJson::Json::array());
        while (dimensions.size() < 3) dimensions.push_back(1.0f);
        xml << "<object objName=\"" << xmlEscaped(name) << "\">"
            << "<componentType>" << xmlEscaped(type) << "</componentType>"
            << "<meshNames>" << xmlEscaped(joinedNames(object, "name", name)) << "</meshNames>"
            << "<spectralNames>" << xmlEscaped(joinedNames(object, "spectralName", spectral)) << "</spectralNames>"
            << "<thermalNames>" << xmlEscaped(joinedNames(object, "thermalName", thermal)) << "</thermalNames>"
            << "<canopyNames>" << xmlEscaped(joinedNames(object, "canopyName", canopy)) << "</canopyNames>"
            << "<bioNames>" << xmlEscaped(joinedNames(object, "materialName", material)) << "</bioNames>"
            << "<propNames>" << xmlEscaped(joinedNames(object, "materialName", material)) << "</propNames>"
            << "<shapes>" << dimensions[0] << ',' << dimensions[1] << ',' << dimensions[2] << "</shapes>";
        const std::string model = ProjectJson::string(object, "fileName");
        const std::string positions = ProjectJson::string(object, "positionFile");
        if (!model.empty()) xml << "<fileName>" << xmlEscaped(project.resolve(model).string()) << "</fileName>";
        if (!positions.empty()) xml << "<objectPosition>" << xmlEscaped(project.resolve(positions).string()) << "</objectPosition>";
        xml << "</object>";
    }
    // 步骤 4：光学（反射率/透射率）材料：写模型类型、反射/透射谱、热红外反射/透射、
    //         PROSPECT/BSM 参数（缺省值与旧工程一致）以及物理纹理参数与外部谱文件。
    xml << "</Object></Scene><Attribute><Optical>";
    for (const auto& item : project.spectra()) {
        const auto params = item.contains("params") ? item["params"] : ProjectJson::Json::object();
        const auto physicalTexture = item.contains("physicalTexture")
            ? item["physicalTexture"] : ProjectJson::Json::object();
        xml << "<spectral name=\"" << xmlEscaped(ProjectJson::string(item, "name", "spectral"))
            << "\" type=\"" << xmlEscaped(ProjectJson::string(item, "model", "custom")) << "\">"
            << "<reflectance>" << xmlEscaped(ProjectJson::string(item, "reflectance", "0.2")) << "</reflectance>"
            << "<transmittance>" << xmlEscaped(ProjectJson::string(item, "transmittance", "0")) << "</transmittance>"
            << "<ref_TIR>" << ProjectJson::number(item, "refTir", 0.05f) << "</ref_TIR>"
            << "<ref_TIR_wet>" << ProjectJson::number(item, "refTirWet", -1.0f) << "</ref_TIR_wet>"
            << "<tau_TIR>" << ProjectJson::number(item, "tauTir", 0.0f) << "</tau_TIR>"
            << "<Cab>" << ProjectJson::number(params, "Cab", 40) << "</Cab><Cw>" << ProjectJson::number(params, "Cw", .01f)
            << "</Cw><Cdm>" << ProjectJson::number(params, "Cdm", .01f) << "</Cdm><Cs>" << ProjectJson::number(params, "Cs", 0)
            << "</Cs><N>" << ProjectJson::number(params, "N", 1.5f) << "</N><SMC>" << ProjectJson::number(params, "SMC", 25)
            << "</SMC><BSMBrightness>" << ProjectJson::number(params, "BSMBrightness", .5f) << "</BSMBrightness><BSMlat>"
            << ProjectJson::number(params, "BSMlat", 25) << "</BSMlat><BSMlon>" << ProjectJson::number(params, "BSMlon", 45) << "</BSMlon>"
            << "<physicalTextureEnabled>" << (ProjectJson::boolean(physicalTexture, "enabled", false) ? 1 : 0) << "</physicalTextureEnabled>"
            << "<physicalTextureStrength>" << ProjectJson::number(physicalTexture, "strength", 0.2f) << "</physicalTextureStrength>"
            << "<physicalTextureRelativeStd>" << ProjectJson::number(physicalTexture, "relativeStd", 0.0f) << "</physicalTextureRelativeStd>"
            << "<physicalTextureRepeatSize>" << ProjectJson::number(physicalTexture, "repeatSize", 2.0f) << "</physicalTextureRepeatSize>";
        if (!ProjectJson::string(physicalTexture, "fileName").empty())
            xml << "<physicalTextureFile>" << xmlEscaped(project.resolve(ProjectJson::string(physicalTexture, "fileName")).string()) << "</physicalTextureFile>";
        if (!ProjectJson::string(item, "fileName").empty())
            xml << "<spectral_file>" << xmlEscaped(project.resolve(ProjectJson::string(item, "fileName")).string()) << "</spectral_file>";
        xml << "</spectral>";
    }
    // 步骤 5：热状态：受光/背光面的初始温度（缺省 305 K / 295 K）。
    xml << "</Optical><Thermal>";
    for (const auto& item : project.thermals())
        xml << "<thermal name=\"" << xmlEscaped(ProjectJson::string(item, "name", "temperature")) << "\"><sunlitTemperature>"
            << ProjectJson::number(item, "sunlitTemperature", 305) << "</sunlitTemperature><shadedTemperature>"
            << ProjectJson::number(item, "shadedTemperature", 295) << "</shadedTemperature></thermal>";
    // 步骤 6：Biochemistry 段。先写土壤材料的 soilSet（rss、cs、rhos、lambdas、SMC，
    //         method 优先取 control.soilTemperatureMethod）。
    xml << "</Thermal><Biochemistry>";
    for (const auto& item : project.materials()) {
        if (ProjectJson::string(item, "type", "Soil") != "Soil") continue;
        const auto params = item.contains("params") ? item["params"] : ProjectJson::Json::object();
        xml << "<property><soilSet name=\"" << xmlEscaped(ProjectJson::string(item, "name", "soilset")) << "\">"
            << "<method>" << ProjectJson::integer(control, "soilTemperatureMethod", ProjectJson::integer(params, "method", 1)) << "</method>"
            << "<rss>" << ProjectJson::number(params, "rss", 2000) << "</rss><cs>" << ProjectJson::number(params, "cs", 1180)
            << "</cs><rhos>" << ProjectJson::number(params, "rhos", 1800) << "</rhos><lambdas>"
            << ProjectJson::number(params, "lambdas", 1.55f) << "</lambdas><SMC>" << ProjectJson::number(params, "SMC", 25)
            << "</SMC></soilSet></property>";
    }
    for (const auto& item : project.materials()) {
        // 步骤 7：逐材料写 surfaceEnergy（FacetEB 的能量平衡参数）。依材料类别分支：
        //         光伏、叶片（光合/气孔参数）、水体、建筑墙体（wallMode = wall）、船体、车辆，
        //         以及可选的集总热（lumpedThermal）参数。字段缺省时多数不写标签，保持旧 XML。
        const auto p=item.contains("params")?item["params"]:ProjectJson::Json::object();
        const bool pv=ProjectJson::string(item,"energyModel")=="photovoltaic";
        const bool leaf=ProjectJson::string(item,"type")=="Vegetation" && ProjectJson::string(item,"energyModel")!="wood";
        const bool water=!pv && ProjectJson::string(item,"type")=="Water";
        // A thin leaf stores about 0.2 kg m-2 of water: ~1000 J m-2 K-1.
        // A water facet stores its mixed layer: heatCapacity (J m-3 K-1) x mixingDepth.
        const float waterCapacity = water
            ? ProjectJson::number(p,"heatCapacity",4186000.0f)*std::max(0.01f,ProjectJson::number(p,"mixingDepth",.5f)) : 0.0f;
        // 缺省单位面积热容（J m-2 K-1）：光伏 12000、叶片 1000、水体按上式混合层热容、其余 60000。
        const float defaultHeatCapacity = pv ? 12000.0f : leaf ? 1000.0f : water ? waterCapacity : 60000.0f;
        xml << "<surfaceEnergy name=\"" << xmlEscaped(ProjectJson::string(item,"name")) << "\">"
            << "<photovoltaic>" << (pv?1:0) << "</photovoltaic>"
            << "<vegetation>" << (leaf?1:0) << "</vegetation>"
            << "<eta25>" << ProjectJson::number(p,"eta25",.22f) << "</eta25>"
            << "<gamma>" << ProjectJson::number(p,"gamma",-.0035f) << "</gamma>"
            << "<bifaciality>" << ProjectJson::number(p,"bifaciality",0) << "</bifaciality>"
            << "<heatCapacityPerArea>" << ProjectJson::number(p,"heatCapacityPerArea",defaultHeatCapacity) << "</heatCapacityPerArea>"
            << "<convectiveScale>" << ProjectJson::number(p,"convectiveScale",1) << "</convectiveScale>";
        if (water)
            xml << "<water>1</water><waterRss>" << std::max(0.0f,ProjectJson::number(p,"rss",0)) << "</waterRss>"
                << "<evaporationCoefficient>" << std::clamp(ProjectJson::number(p,"evaporationCoefficient",1),0.0f,1.0f)
                << "</evaporationCoefficient>";
        // Optional water start temperature; absent = first-node air temperature.
        if (water && ProjectJson::string(p,"initialTemperatureMode")=="dailyMeanAir")
            xml << "<waterInitialMode>dailyMeanAir</waterInitialMode>";
        else if (water && ProjectJson::string(p,"initialTemperatureMode")=="fixed")
            xml << "<waterInitialMode>fixed</waterInitialMode><waterInitialTemperature>"
                << ProjectJson::number(p,"initialTemperature",20) << "</waterInitialTemperature>";
        // Building wall/roof slab: only written for wallMode "wall" (shell = old thin facet).
        if (!pv && !leaf && !water && ProjectJson::string(item,"energyModel")!="membrane" &&
            ProjectJson::string(p,"wallMode","shell")=="wall") {
            xml << "<wallMode>wall</wallMode><thickness>" << ProjectJson::number(p,"thickness",.2f) << "</thickness>"
                << "<lambdas>" << ProjectJson::number(p,"lambdas",1.4f) << "</lambdas>"
                << "<rhos>" << ProjectJson::number(p,"rhos",2300) << "</rhos><cs>" << ProjectJson::number(p,"cs",880) << "</cs>"
                << "<indoorTemperature>" << ProjectJson::number(p,"indoorTemperature",22) << "</indoorTemperature>"
                << "<indoorHeatTransferCoefficient>" << ProjectJson::number(p,"indoorHeatTransferCoefficient",8)
                << "</indoorHeatTransferCoefficient>";
            // Layered construction (outer to inner): "t k rho cp;" per layer.
            if (p.contains("layers") && p["layers"].is_array() && !p["layers"].empty()) {
                std::ostringstream layers;
                layers.precision(12);
                for (const auto& layer : p["layers"]) {
                    if (!layer.is_object()) throw std::runtime_error("FacetEB wall layers must be objects");
                    layers << layer.value("thickness", 0.0) << ' ' << layer.value("lambda", 0.0)
                           << ' ' << layer.value("rho", 0.0) << ' ' << layer.value("cp", 0.0) << ';';
                }
                xml << "<wallLayers>" << layers.str() << "</wallLayers>";
            }
        }
        // Ship hull: relative wind (speed > 0) and waterline contact; absent
        // fields write nothing, so other materials keep the old XML.
        if (!pv && !leaf && !water && ProjectJson::string(item,"type")=="Ship") {
            if (ProjectJson::number(p,"speed",0) > 0)
                xml << "<shipSpeed>" << ProjectJson::number(p,"speed",0) << "</shipSpeed>";
            if (p.contains("waterlineHeight") && p["waterlineHeight"].is_number()) {
                xml << "<waterlineHeight>" << ProjectJson::number(p,"waterlineHeight",0) << "</waterlineHeight>"
                    << "<waterHeatTransferCoefficient>" << ProjectJson::number(p,"waterHeatTransferCoefficient",500)
                    << "</waterHeatTransferCoefficient>";
                if (p.contains("waterTemperature") && p["waterTemperature"].is_number())
                    xml << "<waterTemperature>" << ProjectJson::number(p,"waterTemperature",20) << "</waterTemperature>";
            } else if (ProjectJson::string(p,"waterlineSource") == "water") {
                // Waterline = the scene water surface (+ offset): real draft.
                xml << "<waterlineSource>water</waterlineSource><waterlineOffset>" << ProjectJson::number(p,"waterlineOffset",0)
                    << "</waterlineOffset><waterHeatTransferCoefficient>" << ProjectJson::number(p,"waterHeatTransferCoefficient",500)
                    << "</waterHeatTransferCoefficient>";
                if (p.contains("waterTemperature") && p["waterTemperature"].is_number())
                    xml << "<waterTemperature>" << ProjectJson::number(p,"waterTemperature",20) << "</waterTemperature>";
            }
            if (ProjectJson::number(p,"heading",-1) >= 0 && ProjectJson::number(p,"windDirection",-1) >= 0)
                xml << "<shipHeading>" << ProjectJson::number(p,"heading",0) << "</shipHeading><shipWindDirection>"
                    << ProjectJson::number(p,"windDirection",0) << "</shipWindDirection>";
            if (ProjectJson::boolean(p,"waterShielding",false)) xml << "<waterShielding>1</waterShielding>";
            if (ProjectJson::boolean(p,"waterFeedback",false)) xml << "<waterFeedback>1</waterFeedback>";
            if (ProjectJson::number(p,"internalHeatFlux",0) != 0)
                xml << "<internalHeatFlux>" << ProjectJson::number(p,"internalHeatFlux",0) << "</internalHeatFlux>";
            if (ProjectJson::number(p,"internalHeatPower",0) != 0)
                xml << "<internalHeatPower>" << ProjectJson::number(p,"internalHeatPower",0) << "</internalHeatPower>";
        }
        // Vehicle (material type Vehicle): part role and, on the body material,
        // the cabin / driving parameters (params.cabin). Absent writes nothing.
        if (!pv && !leaf && !water && ProjectJson::string(item,"type")=="Vehicle") {
            const std::string part = ProjectJson::string(p,"vehiclePart","");
            const int partCode = part=="body" ? 1 : part=="glass" ? 2 : part=="underbody" ? 3 : part=="hood" ? 4 : 0;
            if (partCode) xml << "<vehiclePart>" << partCode << "</vehiclePart>";
            if (partCode == 1 && p.contains("cabin") && p["cabin"].is_object()) {
                const auto& c = p["cabin"];
                const auto num = [&](const char* key, float fallback) { return ProjectJson::number(c, key, fallback); };
                // Parts model (all optional): interior nodes and engine/bay nodes as comma lists.
                const auto list = [&](const ProjectJson::Json& o, const char* key, std::initializer_list<const char*> ids, float fallback) {
                    std::string text;
                    for (const char* id : ids) {
                        const float v = o.contains(id) && o[id].is_object() ? ProjectJson::number(o[id], key, fallback) : fallback;
                        text += (text.empty() ? "" : ",") + std::to_string(v);
                    }
                    return text;
                };
                if (c.contains("interior") && c["interior"].is_object() && ProjectJson::boolean(c["interior"],"enabled",false)) {
                    const auto& in = c["interior"];
                    const auto ids = {"dashboard","seat","wheel","lining"};
                    xml << "<vcIntModel>1</vcIntModel>"
                        << "<vcIntShares>" << list(in,"share",ids,0.25f) << "</vcIntShares>"
                        << "<vcIntCapacities>" << list(in,"capacity",ids,10000) << "</vcIntCapacities>"
                        << "<vcIntHcA>" << list(in,"hcA",ids,4) << "</vcIntHcA>"
                        << "<vcIntRadArea>" << list(in,"radArea",ids,1) << "</vcIntRadArea>"
                        << "<vcShellRadFraction>" << ProjectJson::number(in,"shellRadFraction",0.45f) << "</vcShellRadFraction>"
                        << "<vcSeatEffusivity>" << ProjectJson::number(in,"seatEffusivity",300) << "</vcSeatEffusivity>";
                }
                if (c.contains("engine") && c["engine"].is_object() && ProjectJson::boolean(c["engine"],"enabled",false)) {
                    const auto& en = c["engine"];
                    xml << "<vcEngModel>1</vcEngModel>"
                        << "<vcEngWarm>" << (ProjectJson::boolean(en,"warm",false) ? 1 : 0) << "</vcEngWarm>"
                        << "<vcEngWarmTemp>" << ProjectJson::number(en,"warmTemperature",90) << "</vcEngWarmTemp>"
                        << "<vcEngCaps>" << ProjectJson::number(en,"blockCapacity",70000) << "," << ProjectJson::number(en,"bayCapacity",15000) << "</vcEngCaps>"
                        << "<vcEngBlockBay>" << ProjectJson::number(en,"blockToBay",40) << "</vcEngBlockBay>"
                        << "<vcEngBayVent>" << ProjectJson::number(en,"bayVent0",0.04f) << "," << ProjectJson::number(en,"bayVentK",0.004f) << "</vcEngBayVent>"
                        << "<vcEngSplit>" << ProjectJson::number(en,"radiatorShare",0.5f) << "," << ProjectJson::number(en,"blockShare",0.3f) << ","
                        << ProjectJson::number(en,"exhaustShare",0.2f) << "</vcEngSplit>"
                        << "<vcFirewallG>" << ProjectJson::number(en,"firewallG",2) << "</vcFirewallG>";
                }
                xml << "<vcEnabled>" << (ProjectJson::boolean(c,"enabled",false) ? 1 : 0) << "</vcEnabled>"
                    << "<vcSpeed>" << num("speed",0) << "</vcSpeed>"
                    << "<vcOccupants>" << num("occupants",0) << "</vcOccupants>"
                    << "<vcMetabolicW>" << num("metabolicW",100) << "</vcMetabolicW>"
                    << "<vcVolume>" << num("volume",2.8f) << "</vcVolume>"
                    << "<vcInteriorCapacity>" << num("interiorCapacity",80000) << "</vcInteriorCapacity>"
                    << "<vcInteriorArea>" << num("interiorArea",8) << "</vcInteriorArea>"
                    << "<vcInteriorH>" << num("interiorH",8) << "</vcInteriorH>"
                    << "<vcAbsorbedFraction>" << num("absorbedFraction",0.8f) << "</vcAbsorbedFraction>"
                    << "<vcVentilationAch>" << num("ventilationACH",0.5f) << "</vcVentilationAch>"
                    << "<vcCrackedAch>" << num("crackedACH",10) << "</vcCrackedAch>"
                    << "<vcWindowCracked>" << (ProjectJson::boolean(c,"windowCracked",false) ? 1 : 0) << "</vcWindowCracked>"
                    << "<vcAcOn>" << (ProjectJson::boolean(c,"acOn",false) ? 1 : 0) << "</vcAcOn>"
                    << "<vcAcPower>" << num("acPower",3500) << "</vcAcPower>"
                    << "<vcAcSetpoint>" << num("acSetpoint",24) << "</vcAcSetpoint>"
                    << "<vcAcBand>" << num("acBand",2) << "</vcAcBand>"
                    << "<vcEngineOn>" << (ProjectJson::boolean(c,"engineOn",false) ? 1 : 0) << "</vcEngineOn>"
                    << "<vcEnginePower>" << num("enginePower",15000) << "</vcEnginePower>"
                    << "<vcEngineShare>" << num("engineShare",0.3f) << "</vcEngineShare>";
                if (num("heading",-1) >= 0 && num("windDirection",-1) >= 0)
                    xml << "<vcHeading>" << num("heading",0) << "</vcHeading><vcWindDirection>" << num("windDirection",0) << "</vcWindDirection>";
            }
        }
        // 叶片光合与气孔参数（Vcmax、Ball-Berry 的 m 与截距、Type 等），缺省为 C3 叶片典型值；
        // Tparam 缺省 "0.2,0.3,288,313,328"，写出时逗号换成空格（各项含义待核对）。
        if (leaf) {
            std::string tparam = p.contains("Tparam") && p["Tparam"].is_string()
                ? p["Tparam"].get<std::string>() : std::string("0.2,0.3,288,313,328");
            std::replace(tparam.begin(), tparam.end(), ',', ' ');
            xml << "<Vcmax>" << ProjectJson::number(p,"Vcmax",60) << "</Vcmax>"
                << "<m>" << ProjectJson::number(p,"m",8) << "</m>"
                << "<BallBerry>" << ProjectJson::number(p,"BallBerry",.01f) << "</BallBerry>"
                << "<Type>" << ProjectJson::number(p,"Type",3) << "</Type>"
                << "<kV>" << ProjectJson::number(p,"kV",.6396f) << "</kV>"
                << "<Rdparam>" << ProjectJson::number(p,"Rdparam",.015f) << "</Rdparam>"
                << "<stressfactor>" << ProjectJson::number(p,"stressfactor",1) << "</stressfactor>"
                << "<Tcor>" << ProjectJson::number(p,"Tcor",0) << "</Tcor>"
                << "<Tyear>" << ProjectJson::number(p,"Tyear",25) << "</Tyear>"
                << "<beta>" << ProjectJson::number(p,"beta",.507f) << "</beta>"
                << "<qLs>" << ProjectJson::number(p,"qLs",1) << "</qLs>"
                << "<kNPQs>" << ProjectJson::number(p,"kNPQs",0) << "</kNPQs>"
                << "<Tparam>" << xmlEscaped(tparam) << "</Tparam>";
        }
        // 集总热（单节点热容）参数：capacityJPerK、conductanceWPerK、stopSeconds、group 为必填；
        // 校验：group 非空、weight ∈ [0,1]、maxSurfaceK ∈ [initialK, 1500]，不满足则抛异常。
        if (p.contains("lumpedThermal")) {
            const auto& h=p.at("lumpedThermal");
            facetphysics::LumpedThermalParameters checked{
                h.at("capacityJPerK").get<double>(),h.at("conductanceWPerK").get<double>(),
                h.value("airConductanceWPerK",0.),h.value("powerW",0.),
                h.value("startSeconds",0.),h.at("stopSeconds").get<double>(),h.value("initialK",298.15)};
            checked.validate();
            const auto group=h.at("group").get<std::string>();
            const double weight=h.value("weight",1.);
            const double maxSurfaceK=h.value("maxSurfaceK",500.);
            if(group.empty() || !std::isfinite(weight) || weight<0 || weight>1)
                throw std::runtime_error("Invalid lumped thermal group/weight");
            if(!std::isfinite(maxSurfaceK) || maxSurfaceK<checked.initialK || maxSurfaceK>1500)
                throw std::runtime_error("Invalid lumped thermal surface-temperature range");
            xml << "<lumpedThermalGroup>" << xmlEscaped(group) << "</lumpedThermalGroup>"
                << "<lumpedCapacity>" << checked.capacityJPerK << "</lumpedCapacity>"
                << "<lumpedConductance>" << checked.conductanceWPerK << "</lumpedConductance>"
                << "<lumpedAirConductance>" << checked.airConductanceWPerK << "</lumpedAirConductance>"
                << "<lumpedPower>" << checked.powerW << "</lumpedPower>"
                << "<lumpedStart>" << checked.startSeconds << "</lumpedStart>"
                << "<lumpedStop>" << checked.stopSeconds << "</lumpedStop>"
                << "<lumpedInitialK>" << checked.initialK << "</lumpedInitialK>"
                << "<lumpedWeight>" << weight << "</lumpedWeight>";
            xml << "<lumpedMaxSurfaceK>" << maxSurfaceK << "</lumpedMaxSurfaceK>";
        }
        xml << "</surfaceEnergy>";
    }
    // 步骤 8：冠层空气动力参数（叶宽 leafwidth，缺省 0.1）。
    for (const auto& item : project.canopies())
        xml << "<canopyAero name=\"" << xmlEscaped(ProjectJson::string(item, "name")) << "\"><leafwidth>"
            << ProjectJson::number(item, "leafwidth", 0.1f) << "</leafwidth></canopyAero>";
    // 步骤 9：可选的逐节点时间步长（秒）：个数必须等于 meteo.end，每个值须为 [1, 86400] 内的有限数。
    if(meteo.contains("timeStepSeconds")) {
        const auto& widths=meteo.at("timeStepSeconds");
        if(!widths.is_array() || widths.size()!=size_t(ProjectJson::integer(meteo,"end",1)))
            throw std::runtime_error("timeStepSeconds must contain every requested node");
        xml << "<nodeTimeStepsSeconds>";
        for(const auto& v:widths) {
            double step=v.get<double>();
            if(!std::isfinite(step) || step<1 || step>86400)
                throw std::runtime_error("Invalid timeStepSeconds value");
            xml << step << ' ';
        }
        xml << "</nodeTimeStepsSeconds>";
    }
    // 步骤 10：Meteorology 段：气象文件、参考高度、起止节点、时间步 dTime、经纬度、年份、时区；
    //          缺省值（dTime 1800、纬度 40、经度 116、年 2019、时区 8）沿用旧工程设置。
    xml << "</Biochemistry></Attribute><Meteorology>"
        << "<filePath>" << xmlEscaped(project.meteorologyPath().string()) << "</filePath>"
        << "<referenceHeight>" << ProjectJson::number(meteo, "z", 10) << "</referenceHeight>";
    // Initial soil temperature only when the project sets a number. Absent or
    // null Tsold leaves the tag out, and FacetEB starts the ground from the
    // first meteorological air temperature (the documented default).
    if (const float tsold = ProjectJson::number(meteo, "Tsold", -1.0f); tsold > 0.0f)
        xml << "<initialSoilTemperature>" << tsold << "</initialSoilTemperature>";
    xml << "<carbonDioxide>" << ProjectJson::number(meteo, "Ca", 380) << "</carbonDioxide>"
        << "<startTimeNode>" << ProjectJson::integer(meteo, "start", 0) << "</startTimeNode>"
        << "<endTimeNode>" << ProjectJson::integer(meteo, "end", 1) << "</endTimeNode>"
        << "<dTime>" << ProjectJson::number(meteo, "dTime", 1800) << "</dTime>"
        << "<Latitude>" << ProjectJson::number(meteo, "latitude", 40) << "</Latitude>"
        << "<Longitude>" << ProjectJson::number(meteo, "longitude", 116) << "</Longitude>"
        << "<Year>" << ProjectJson::integer(meteo, "year", 2019) << "</Year>"
        << "<TimeZone>" << ProjectJson::number(meteo, "timezone", 8) << "</TimeZone>"
        << "</Meteorology></HiStreamProject>";
    return xml.str();
}

// 读取输入文本：.json 扩展名（不区分大小写）经 facetInputView 转成内存 XML，其余按原样读取。
std::string readTextFile(const std::string& path)
{
    std::filesystem::path inputPath(path);
    std::string extension = inputPath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == ".json") return facetInputView(path);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open HiStream input: " + path);
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// XML 实体解码（& < > " '），与 xmlEscaped 互逆。
std::string decodeXmlText(std::string value)
{
    const std::pair<const char*, const char*> entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&apos;", "'"}
    };
    for (const auto& entity : entities) {
        size_t position = 0;
        while ((position = value.find(entity.first, position)) != std::string::npos) {
            value.replace(position, std::strlen(entity.first), entity.second);
            position += std::strlen(entity.second);
        }
    }
    return value;
}

// 作用：取 xml 中第一个 <tag ...>内容</tag> 的内容（已解码实体）；找不到返回空串。
// 说明：按前缀 "<tag" 查找，不检查标签名边界，也不处理嵌套同名标签（待核对：
//       例如 ref_TIR 与 ref_TIR_wet 并存时依赖其书写顺序）。
std::string firstXmlTag(const std::string& xml, const char* tag)
{
    const std::string open = "<" + std::string(tag);
    const std::string close = "</" + std::string(tag) + ">";
    const size_t begin = xml.find(open);
    if (begin == std::string::npos) {
        return {};
    }
    const size_t contentBegin = xml.find('>', begin + open.size());
    if (contentBegin == std::string::npos) {
        return {};
    }
    const size_t contentEnd = xml.find(close, contentBegin + 1U);
    if (contentEnd == std::string::npos) {
        return {};
    }
    return decodeXmlText(xml.substr(contentBegin + 1U, contentEnd - contentBegin - 1U));
}

// 去掉首尾空白字符（按 std::isspace）。
std::string trim(std::string value)
{
    const auto whitespace = [](unsigned char character) { return std::isspace(character) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                            [&](char character) { return !whitespace(character); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](char character) { return !whitespace(character); }).base(),
                value.end());
    return value;
}

// 读取标签内的浮点数；标签缺失、为空、无法解析或非有限值时返回 fallback。
float xmlFloat(const std::string& xml, const char* tag, float fallback)
{
    const std::string value = trim(firstXmlTag(xml, tag));
    if (value.empty()) {
        return fallback;
    }
    try {
        const float parsed = std::stof(value);
        return std::isfinite(parsed) ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

std::vector<std::string> xmlObjectBlocks(const std::string& xml);

// 解析数值列表：';' 与 ',' 都当作分隔符，片段内再按空白切分；非有限值丢弃。
std::vector<float> numberList(const std::string& text)
{
    std::vector<float> values;
    std::string normalizedText = text;
    std::replace(normalizedText.begin(), normalizedText.end(), ';', ',');
    std::istringstream input(normalizedText);
    std::string token;
    while (std::getline(input, token, ',')) {
        std::istringstream words(token);
        float value = 0.0f;
        while (words >> value) {
            if (std::isfinite(value)) {
                values.push_back(value);
            }
        }
    }
    return values;
}

// 取 ','/';' 分隔列表的第一个名字（去空白）。
std::string firstListName(const std::string& text)
{
    const size_t separator = text.find_first_of(",;");
    return trim(text.substr(0, separator));
}

// 解析名字列表：';' 与 ',' 分隔，去空白并丢弃空项。
std::vector<std::string> nameList(const std::string& text)
{
    std::vector<std::string> values;
    std::string normalizedText = text;
    std::replace(normalizedText.begin(), normalizedText.end(), ';', ',');
    std::istringstream input(normalizedText);
    std::string value;
    while (std::getline(input, value, ',')) {
        value = trim(value);
        if (!value.empty()) {
            values.push_back(value);
        }
    }
    return values;
}

// ASCII 大小写不敏感的字符串相等比较。
bool equalIgnoringCase(const std::string& left, const std::string& right)
{
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(),
                   [](unsigned char a, unsigned char b) {
                       return std::tolower(a) == std::tolower(b);
                   });
}

// 从起始标签文本中取属性值（支持单/双引号，属性名前必须是行首或空白），返回已解码的值；
// 找不到返回空串。
std::string xmlAttribute(const std::string& openingTag, const char* attribute)
{
    const std::string prefix = std::string(attribute) + '=';
    size_t position = openingTag.find(prefix);
    while (position != std::string::npos) {
        if (position == 0U ||
            std::isspace(static_cast<unsigned char>(openingTag[position - 1U]))) {
            size_t valueStart = position + prefix.size();
            if (valueStart < openingTag.size() &&
                (openingTag[valueStart] == '"' || openingTag[valueStart] == '\'')) {
                const char quote = openingTag[valueStart++];
                const size_t valueEnd = openingTag.find(quote, valueStart);
                if (valueEnd != std::string::npos) {
                    return decodeXmlText(
                        openingTag.substr(valueStart, valueEnd - valueStart));
                }
            }
        }
        position = openingTag.find(prefix, position + prefix.size());
    }
    return {};
}

// 取第一个 <element ...>...</element> 整块文本，其 name 属性等于 name；name 为空则取第一块。
// 找不到或标签不闭合时返回空串。不支持同名元素嵌套。
std::string namedXmlBlock(const std::string& xml,
                          const char* element,
                          const std::string& name)
{
    const std::string open = "<" + std::string(element);
    const std::string close = "</" + std::string(element) + ">";
    size_t position = 0;
    while ((position = xml.find(open, position)) != std::string::npos) {
        const size_t suffix = position + open.size();
        if (suffix < xml.size() &&
            !(xml[suffix] == '>' ||
              std::isspace(static_cast<unsigned char>(xml[suffix])))) {
            position = suffix;
            continue;
        }
        const size_t openingEnd = xml.find('>', suffix);
        if (openingEnd == std::string::npos) {
            return {};
        }
        const std::string openingTag =
            xml.substr(position, openingEnd - position + 1U);
        const size_t end = xml.find(close, openingEnd + 1U);
        if (end == std::string::npos) {
            return {};
        }
        if (name.empty() || xmlAttribute(openingTag, "name") == name) {
            return xml.substr(position, end + close.size() - position);
        }
        position = end + close.size();
    }
    return {};
}

std::filesystem::path resolveProjectAsset(const std::filesystem::path& inputPath,
                                          const std::string& value);

// 作用：读取叶片/土壤光谱模型（PROSPECT 等）所需的 optipar 系数表。
// 来源：与 esunFileName 同目录的 optipar.txt，每行 18 列浮点数，列顺序见 columns 数组；
//       列数不足的行跳过；没有 esunFileName 或文件打不开时返回空表。
OptCoeff loadOpticalCoefficients(const std::string& xml,
                                 const std::filesystem::path& inputPath)
{
    OptCoeff coefficients;
    const std::string esunFile = trim(firstXmlTag(xml, "esunFileName"));
    if (esunFile.empty()) return coefficients;
    const std::filesystem::path coefficientFile =
        resolveProjectAsset(inputPath, esunFile).parent_path() / "optipar.txt";
    std::ifstream input(coefficientFile);
    if (!input) return coefficients;
    std::array<std::vector<float>*, 18> columns{
        &coefficients.wl_, &coefficients.nr_, &coefficients.kab_,
        &coefficients.kca_, &coefficients.ks_, &coefficients.kw_,
        &coefficients.kdm_, &coefficients.phiI_, &coefficients.phiII_,
        &coefficients.kcaV_, &coefficients.kcaZ_, &coefficients.kcant_,
        &coefficients.kcaV2_, &coefficients.phi_, &coefficients.gsv1_,
        &coefficients.gsv2_, &coefficients.gsv3_, &coefficients.nw_
    };
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream stream(line);
        std::array<float, 18> values{};
        bool valid = true;
        for (float& value : values) valid = valid && static_cast<bool>(stream >> value);
        if (!valid) continue;
        for (std::size_t index = 0; index < columns.size(); ++index)
            columns[index]->push_back(values[index]);
    }
    return coefficients;
}

// 模块：按名字从 XML 读取一类表面的光谱与热状态参数，写入 surface。
// 输入：xml（含 spectral/thermal 块）、spectralName、thermalName、成像波段 wavelengths、
//       光谱模型系数 coefficients、inputPath（用于解析相对路径的外部谱/纹理文件）。
// 输出：surface 的 reflectance/transmittance（成像波段）、energyReflectance/
//       energyTransmittance（400–2400 nm、1 nm 间隔共 N1 = 2001 点，供能量平衡）、
//       热红外反射/透射、物理纹理参数、受光/背光面温度。找不到对应块则保持原值。
void loadSurfaceParameters(const std::string& xml,
                           const std::string& spectralName,
                           const std::string& thermalName,
                           const std::vector<float>& wavelengths,
                           const OptCoeff& coefficients,
                           const std::filesystem::path& inputPath,
                           SurfaceParameters& surface)
{
    // 步骤 1：取名为 spectralName 的光谱材料块，并按 type 判定模型（Prospect/BSM/file/custom）。
    const std::string spectral =
        namedXmlBlock(xml, "spectral", spectralName);
    if (!spectral.empty()) {
        const size_t openingEnd = spectral.find('>');
        const std::string openingTag = openingEnd == std::string::npos
            ? std::string{} : spectral.substr(0, openingEnd + 1U);
        const std::string type = xmlAttribute(openingTag, "type");
        SpectralXml material{};
        material.spectralName = spectralName;
        material.type = equalIgnoringCase(type, "Prospect") ? spectralType::PROSPECT
            : equalIgnoringCase(type, "BSM") ? spectralType::BSM
            : equalIgnoringCase(type, "file") ? spectralType::OTHER
            : spectralType::CUSTOM;
        material.reflectances = numberList(firstXmlTag(spectral, "reflectance"));
        material.transmittance = numberList(firstXmlTag(spectral, "transmittance"));
        material.refl_tir = xmlFloat(spectral, "ref_TIR", surface.tirReflectance);
        material.tau_tir = xmlFloat(spectral, "tau_TIR", surface.tirTransmittance);
        material.refl_tir_wet = xmlFloat(spectral, "ref_TIR_wet", -1.0f);
        material.fp = {
            xmlFloat(spectral, "Cab", 40.0f), xmlFloat(spectral, "Cw", 0.01f),
            xmlFloat(spectral, "Cdm", 0.01f), xmlFloat(spectral, "Cs", 0.0f),
            xmlFloat(spectral, "N", 1.5f)
        };
        material.bsm = {
            xmlFloat(spectral, "SMC", 25.0f),
            xmlFloat(spectral, "BSMBrightness", 0.5f),
            xmlFloat(spectral, "BSMlat", 25.0f),
            xmlFloat(spectral, "BSMlon", 45.0f)
        };
        if (material.type == spectralType::OTHER) {
            const std::string fileName = trim(firstXmlTag(spectral, "spectral_file"));
            material.path = resolveProjectAsset(inputPath, fileName).string();
        }
        // 步骤 2：用 Compo::resolveSpectrals 在成像波段上求反射/透射（自定义谱按波长插值，
        //         Prospect/BSM 由模型生成）。
        Compo compo;
        const std::vector<Spectral> resolved = compo.resolveSpectrals(
            material, wavelengths, wavelengths, coefficients);
        surface.reflectance.clear();
        surface.transmittance.clear();
        for (const Spectral& value : resolved) {
            surface.reflectance.push_back(value.reflectance);
            surface.transmittance.push_back(value.transmittance);
        }
        // 步骤 3：同一材料再在 400–2400 nm 的 1 nm 网格上求一次，作为能量平衡用的宽谱反射/透射。
        std::vector<float> energyWavelengths;
        energyWavelengths.reserve(N1);
        for (int index = 0; index < N1; ++index)
            energyWavelengths.push_back(400.0f + static_cast<float>(index));
        const std::vector<Spectral> energyResolved = compo.resolveSpectrals(
            material, energyWavelengths, wavelengths, coefficients);
        surface.energyReflectance.clear();
        surface.energyTransmittance.clear();
        for (const Spectral& value : energyResolved) {
            surface.energyReflectance.push_back(value.reflectance);
            surface.energyTransmittance.push_back(value.transmittance);
        }
        // Soil TIR optics follow the spectral material's water content, the same
        // rule resolveSpectrals applies to the imaging bands, so the energy
        // balance and the sensor band see one emissivity.
        surface.tirReflectance = material.type == spectralType::BSM
            ? soiloptics::tirReflectance(material.refl_tir, material.tau_tir,
                                         material.refl_tir_wet, material.bsm.SMC)
            : xmlFloat(spectral, "ref_TIR", surface.tirReflectance);
        surface.tirTransmittance =
            xmlFloat(spectral, "tau_TIR", surface.tirTransmittance);

        // 步骤 4：物理纹理（反射率方差纹理）：先复位，再按 physicalTextureEnabled 决定是否读取。
        surface.reflectanceTexture.reset();
        surface.reflectanceTextureStrength = 0.0f;
        surface.reflectanceTextureRelativeStd = 0.0f;
        surface.reflectanceTextureRepeatSize = 1.0f;
        const std::string textureEnabled = trim(
            firstXmlTag(spectral, "physicalTextureEnabled"));
        const bool useTexture = textureEnabled == "1" ||
            equalIgnoringCase(textureEnabled, "true") ||
            equalIgnoringCase(textureEnabled, "yes") ||
            equalIgnoringCase(textureEnabled, "on");
        // 启用纹理必须给出文件，否则抛异常；限幅：strength ∈ [0,1]、relativeStd ∈ [0,0.5]、
        // repeatSize ≥ 0.01（纹理重复尺寸）。
        if (useTexture) {
            const std::string textureFile = trim(
                firstXmlTag(spectral, "physicalTextureFile"));
            if (textureFile.empty()) {
                throw std::runtime_error(
                    "Physical texture is enabled but has no file: " + spectralName);
            }
            surface.reflectanceTextureStrength = std::clamp(
                xmlFloat(spectral, "physicalTextureStrength", 0.2f),
                0.0f, 1.0f);
            surface.reflectanceTextureRelativeStd = std::clamp(
                xmlFloat(spectral, "physicalTextureRelativeStd", 0.0f),
                0.0f, 0.5f);
            surface.reflectanceTextureRepeatSize = std::max(
                0.01f, xmlFloat(
                    spectral, "physicalTextureRepeatSize", 2.0f));
            surface.reflectanceTexture = loadReflectanceVarianceTexture(
                resolveProjectAsset(inputPath, textureFile));
        }
    }

    // 步骤 5：热状态块：受光/背光面温度，缺省沿用 surface 当前值。
    const std::string thermal =
        namedXmlBlock(xml, "thermal", thermalName);
    if (!thermal.empty()) {
        surface.sunlitTemperature =
            xmlFloat(thermal, "sunlitTemperature",
                     surface.sunlitTemperature);
        surface.shadedTemperature =
            xmlFloat(thermal, "shadedTemperature",
                     surface.shadedTemperature);
    }
}


// 作用：读取名为 soilName 的 soilSet 块，写入地面的热/水分参数；找不到则不改。
// 限幅：method ∈ {0,1,2}；rss ≥ 0；cs（比热）、rhos（密度）≥ 1；lambdas（导热系数）≥ 0.001；
//       SMC 大于 1 时视为百分数，乘 0.01 后限制到 [0,1]。
void loadSoilParameters(const std::string& xml,
                        const std::string& soilName,
                        SurfaceParameters& surface)
{
    const std::string soil = namedXmlBlock(xml, "soilSet", soilName);
    if (soil.empty()) {
        return;
    }
    surface.soilTemperatureMethod = std::clamp(
        static_cast<int>(std::lround(xmlFloat(soil, "method", 1.0f))), 0, 2);
    surface.surfaceResistance = std::max(
        0.0f, xmlFloat(soil, "rss", surface.surfaceResistance));
    surface.specificHeat = std::max(
        1.0f, xmlFloat(soil, "cs", surface.specificHeat));
    surface.density = std::max(
        1.0f, xmlFloat(soil, "rhos", surface.density));
    surface.thermalConductivity = std::max(
        0.001f, xmlFloat(
            soil, "lambdas", surface.thermalConductivity));
    const float moisture =
        xmlFloat(soil, "SMC", surface.soilMoisture);
    surface.soilMoisture = std::clamp(
        moisture > 1.0f ? moisture * 0.01f : moisture, 0.0f, 1.0f);
}

// 模块：从输入 XML 组装 FacetRTParameters（波段、天空温度、求解器类型、地物与背景表面参数）。
// 步骤：controlBand → 过滤非正/非有限波长（为空则默认 550 nm）；skyTemperature 限幅
//       100–400 K（缺省 250）；radiationSolver == "accelerated" 选加速求解器；
//       对象表面取第一个对象的首个 spectral/thermal 名；背景取 bgSpectral/bgThermal，
//       再叠加 bgBioName 对应的土壤参数。
FacetRTParameters facetRTParametersFromInput(const std::string& xml,
                                             const std::filesystem::path& inputPath)
{
    FacetRTParameters parameters;
    parameters.wavelengths = numberList(firstXmlTag(xml, "controlBand"));
    parameters.wavelengths.erase(
        std::remove_if(parameters.wavelengths.begin(),
                       parameters.wavelengths.end(),
                       [](float wavelength) {
                           return !std::isfinite(wavelength) ||
                                  wavelength <= 0.0f;
                       }),
        parameters.wavelengths.end());
    if (parameters.wavelengths.empty()) {
        parameters.wavelengths.push_back(550.0f);
    }
    parameters.skyTemperature =
        std::clamp(xmlFloat(xml, "skyTemperature", 250.0f),
                   100.0f, 400.0f);
    parameters.acceleratedSolver =
        trim(firstXmlTag(xml, "radiationSolver")) == "accelerated";
    const OptCoeff coefficients = loadOpticalCoefficients(xml, inputPath);

    const std::vector<std::string> objects = xmlObjectBlocks(xml);
    if (!objects.empty()) {
        loadSurfaceParameters(
            xml,
            firstListName(firstXmlTag(objects.front(), "spectralNames")),
            firstListName(firstXmlTag(objects.front(), "thermalNames")),
            parameters.wavelengths, coefficients, inputPath,
            parameters.object);
    }
    loadSurfaceParameters(
        xml,
        trim(firstXmlTag(xml, "bgSpectral")),
        trim(firstXmlTag(xml, "bgThermal")),
        parameters.wavelengths, coefficients, inputPath,
        parameters.ground);
    loadSoilParameters(
        xml, trim(firstXmlTag(xml, "bgBioName")), parameters.ground);
    return parameters;
}

// 按顺序切出所有 <object ...>...</object> 块；标签名后必须是 '>' 或空白，
// 以免误匹配 <objectPosition> 等标签。缺少结束标签时停止。
std::vector<std::string> xmlObjectBlocks(const std::string& xml)
{
    std::vector<std::string> blocks;
    const std::string open = "<object";
    const std::string close = "</object>";
    size_t position = 0;
    while ((position = xml.find(open, position)) != std::string::npos) {
        const size_t suffix = position + open.size();
        if (suffix >= xml.size() ||
            !(xml[suffix] == '>' || std::isspace(static_cast<unsigned char>(xml[suffix])))) {
            position = suffix;
            continue;
        }
        const size_t end = xml.find(close, suffix);
        if (end == std::string::npos) {
            break;
        }
        blocks.push_back(xml.substr(position, end + close.size() - position));
        position = end + close.size();
    }
    return blocks;
}

// 资源路径解析：绝对路径原样返回，相对路径相对于输入文件所在目录并规范化。
std::filesystem::path resolveProjectAsset(const std::filesystem::path& inputPath,
                                          const std::string& value)
{
    const std::filesystem::path path(trim(value));
    return path.is_absolute() ? path : (inputPath.parent_path() / path).lexically_normal();
}

// 短波光谱形状：direct/diffuse 为 400 nm 起、1 nm 间隔的采样（每微米归一化，见 integrate）。
// sample：波长 ≤ 2.5 视为 µm 并换算成 nm，下标 = round(波长 − 400) 并夹到有效范围；
// integrate：梯形积分后乘 0.001，把 1 nm 间隔换算为 µm。
struct ShortwaveSpectrum {
    std::vector<float> direct;
    std::vector<float> diffuse;

    float sample(const std::vector<float>& values, float wavelength) const
    {
        if (values.empty()) return 0.0f;
        const float wavelengthNanometers = wavelength <= 2.5f
            ? wavelength * 1000.0f : wavelength;
        const int index = std::clamp(
            static_cast<int>(std::lround(wavelengthNanometers - 400.0f)),
            0, static_cast<int>(values.size()) - 1);
        return values[static_cast<size_t>(index)];
    }

    float directAt(float wavelength) const { return sample(direct, wavelength); }
    float diffuseAt(float wavelength) const { return sample(diffuse, wavelength); }

    size_t sampleCount() const
    {
        return std::min(direct.size(), diffuse.size());
    }

    float integrate(const std::vector<float>& values,
                    size_t firstSample,
                    size_t lastSample) const
    {
        if (values.size() < 2U || firstSample >= values.size() - 1U) {
            return 0.0f;
        }
        lastSample = std::min(lastSample, values.size() - 1U);
        double integral = 0.0;
        for (size_t sample = firstSample; sample < lastSample; ++sample) {
            integral += 0.5 * static_cast<double>(
                values[sample] + values[sample + 1U]);
        }
        // Source spectra use 1 nm samples and are normalized per micrometre.
        return static_cast<float>(integral * 0.001);
    }
};

// Factor converting horizontal direct irradiance to the beam-normal value
// applied by FacetEB, with the broadband beam-normal flux capped at 1400 W m-2.
// The energy solve and the spectral image both use it.
static float directHorizontalToNormal(float totalShortwave, float directSpectrumIntegral,
                                      float cosineSolarZenith)
{
    if (!(cosineSolarZenith > 1.0e-4f) || !(totalShortwave > 0.0f) ||
        !(directSpectrumIntegral > 1.0e-8f)) {
        return 0.0f;
    }
    const float horizontalDirect = totalShortwave * directSpectrumIntegral;
    return std::min(1400.0f, horizontalDirect / cosineSolarZenith) / horizontalDirect;
}

// Part of the horizontal direct irradiance that survives the beam-normal
// conversion above (1 normally; < 1 when the sun is below cos 1e-4 or the
// 1400 W m-2 cap applies). Callers add the remainder to the sky diffuse so
// the horizontal shortwave total stays equal to the forcing (energy and image).
static float directHorizontalKeptFraction(float directNormalFactor,
                                          float cosineSolarZenith)
{
    return std::clamp(directNormalFactor * std::max(cosineSolarZenith, 0.0f), 0.0f, 1.0f);
}

// 读取以空白分隔的标量谱文件；非有限值与负值记为 0，最多保留 N1（2001）个采样点。
std::vector<float> readScalarSpectrum(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::vector<float> values;
    float value = 0.0f;
    while (input >> value) {
        values.push_back(std::isfinite(value) ? std::max(0.0f, value) : 0.0f);
    }
    if (values.size() > static_cast<size_t>(N1)) values.resize(N1);
    return values;
}

// 作用：读取 Esun（直射）/Esky（漫射）谱形，并按 directFraction 归一化直射/漫射积分。
// 输入：xml 中的 esunFileName/eskyFileName；任一缺失或有效点数 < 2 时退回平谱。
// 输出：direct 积分 = directFraction，diffuse 积分 = 1 − directFraction（积分按 µm 计，见 integrate）。
ShortwaveSpectrum loadShortwaveSpectrum(
    const std::string& xml,
    const std::filesystem::path& inputPath,
    float directFraction)
{
    ShortwaveSpectrum spectrum;
    const std::string directName = trim(firstXmlTag(xml, "esunFileName"));
    const std::string diffuseName = trim(firstXmlTag(xml, "eskyFileName"));
    if (!directName.empty() && !diffuseName.empty()) {
        spectrum.direct = readScalarSpectrum(
            resolveProjectAsset(inputPath, directName));
        spectrum.diffuse = readScalarSpectrum(
            resolveProjectAsset(inputPath, diffuseName));
    }
    const size_t count = std::min(spectrum.direct.size(), spectrum.diffuse.size());
    if (count < 2U) {
        // Flat per-micrometre fallback whose 400-2400 nm integral is one.
        spectrum.direct.assign(N1, 0.5f * directFraction);
        spectrum.diffuse.assign(N1, 0.5f * (1.0f - directFraction));
        return spectrum;
    }
    spectrum.direct.resize(count);
    spectrum.diffuse.resize(count);
    // As VoxelEB (fileio.cpp atomConds): each file only supplies a spectral
    // shape; the configured directScatteringRatio sets the direct/diffuse split.
    // (The shapes were previously normalized jointly, so the split came from
    // the files, about 0.806 for the bundled Esun_/Esky_, and the setting was
    // ignored whenever the files existed.)
    // 把一条谱缩放到指定积分值 fraction；积分非正或非有限时改用满足该积分的平谱。
    const auto normalizeShape = [count](std::vector<float>& values, float fraction) {
        double integral = 0.0;
        for (size_t index = 0; index + 1U < count; ++index)
            integral += 0.5 * (values[index] + values[index + 1U]);
        // Samples are spaced by 1 nm; convert the integral to micrometres.
        integral *= 0.001;
        if (!(integral > 0.0) || !std::isfinite(integral)) {
            // Flat per-micrometre shape with the requested integral.
            values.assign(count, static_cast<float>(fraction / (0.001 * double(count - 1U))));
            return;
        }
        const float scale = static_cast<float>(fraction / integral);
        for (float& value : values) value *= scale;
    };
    const float fraction = std::clamp(directFraction, 0.0f, 1.0f);
    normalizeShape(spectrum.direct, fraction);
    normalizeShape(spectrum.diffuse, 1.0f - fraction);
    return spectrum;
}

// 模块：把光照设置写入 scene 与 parameters（各光学波段的直射法向辐照与天空漫射辐照）。
// 输入：xml（shortwaveIrradiance、directScatteringRatio、Esun/Esky 文件）、scene、parameters。
// 输出：scene.shortwaveIrradiance（0–2000 W m-2，供 2.5–6 um 太阳项）、
//       scene.beamNormalIrradiance / skyDiffuseIrradiance、逐波段 bandBeamNormal/bandSkyDiffuse
//       （W m-2 nm-1；热红外波段保持 0，天空由 skyTemperature 给出）。
// FacetRT illumination. Without light settings (OBJ input, or XML without
// directScatteringRatio/Esun/Esky) the historical 700 W m-2 beam-normal and
// 50 W m-2 sky are kept. Otherwise the same Esun/Esky shapes and
// directScatteringRatio as FacetEB are used with the historical horizontal
// total for this sun (700 cos + 50 W m-2; optical outputs are normalized, so
// only the direct/diffuse split and the spectral shape matter). Direct light
// lost to low sun or the beam cap is returned to the sky diffuse.
void configureFacetRTLight(const std::string& xml, const std::filesystem::path& inputPath,
                           Scene& scene, FacetRTParameters& parameters)
{
    parameters.bandBeamNormal.clear();
    parameters.bandSkyDiffuse.clear();
    // Absolute shortwave for the 2.5-6 um solar term (facetRtThermalSolar);
    // optical outputs are normalized and do not depend on it.
    scene.shortwaveIrradiance = xml.empty() ? 0.0f : std::clamp(
        xmlFloat(xml, "shortwaveIrradiance", 0.0f), 0.0f, 2000.0f);
    if (xml.empty()) return;
    const bool hasRatio = !trim(firstXmlTag(xml, "directScatteringRatio")).empty();
    const bool hasSpectra = !trim(firstXmlTag(xml, "esunFileName")).empty() &&
                            !trim(firstXmlTag(xml, "eskyFileName")).empty();
    if (!hasRatio && !hasSpectra) return;
    const float directFraction = std::clamp(
        xmlFloat(xml, "directScatteringRatio", 0.8f), 0.0f, 1.0f);
    const ShortwaveSpectrum spectrum = loadShortwaveSpectrum(xml, inputPath, directFraction);
    const size_t samples = spectrum.sampleCount();
    if (samples < 2U) return;
    const float cosine = std::max(0.0f, sunDirection(scene).y);
    // 水平面总辐照：沿用历史取值 kBeamNormalIrradiance·cosθs + 0.5·kSkyDiffuseIrradiance；
    // 光学输出已归一，因此只有直射/漫射比例与谱形有影响。
    const float total = kBeamNormalIrradiance * cosine + kSkyDiffuseIrradiance * 0.5f;
    const float directIntegral = spectrum.integrate(spectrum.direct, 0U, samples - 1U);
    const float diffuseIntegral = spectrum.integrate(spectrum.diffuse, 0U, samples - 1U);
    const float normalFactor = directHorizontalToNormal(total, directIntegral, cosine);
    // lost：因低太阳高度或 1400 W m-2 上限而没有进入直射的那部分，并入天空漫射以保持水平面总量。
    const float lost = 1.0f - directHorizontalKeptFraction(normalFactor, cosine);
    scene.beamNormalIrradiance = total * directIntegral * normalFactor;
    scene.skyDiffuseIrradiance = total * (diffuseIntegral + directIntegral * lost);
    parameters.bandBeamNormal.resize(parameters.wavelengths.size(), 0.0f);
    parameters.bandSkyDiffuse.resize(parameters.wavelengths.size(), 0.0f);
    for (size_t band = 0; band < parameters.wavelengths.size(); ++band) {
        const float wavelength = parameters.wavelengths[band];
        if (!isOpticalWavelength(wavelength)) continue; // thermal: sky from skyTemperature
        // Spectral irradiance per nm, the same units as the FacetEB images.
        const float direct = total * spectrum.directAt(wavelength) * 0.001f;
        parameters.bandBeamNormal[band] = direct * normalFactor;
        parameters.bandSkyDiffuse[band] =
            total * spectrum.diffuseAt(wavelength) * 0.001f + direct * lost;
    }
    std::cout << "facetrt: illumination from light settings, direct fraction "
              << directFraction << ", beam normal " << scene.beamNormalIrradiance
              << " W m-2, sky " << scene.skyDiffuseIrradiance << " W m-2" << std::endl;
}

// 模块：OBJ 几何读取
// ObjGeometry：OBJ 的顶点（OBJ 本地坐标，未经摆放变换）、三角形顶点索引（0 起）以及
// 每个三角形所属的 g/o/usemtl 组名（用于把网格名映射到材质，见 appendGeometry）。
struct ObjGeometry {
    std::vector<Vec3> positions;
    std::vector<std::array<int, 3>> triangles;
    std::vector<std::string> triangleGroups;
};

// 作用：读取场景 OBJ，只解析 g/o/usemtl/v/f 记录；多边形面按扇形拆成三角形。
// 说明：组名取最近的 g/o，若尚无 g/o 则用首个 usemtl 名；无顶点或无面时抛出异常。
ObjGeometry readObjGeometry(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open scene OBJ: " + path.string());
    }
    ObjGeometry geometry;
    std::string activeGroup;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream stream(line);
        std::string record;
        stream >> record;
        if (record == "g" || record == "o") {
            std::string group;
            if (stream >> group) {
                activeGroup = group;
            }
        } else if (record == "usemtl" && activeGroup.empty()) {
            stream >> activeGroup;
        } else if (record == "v") {
            Vec3 position{};
            if (!(stream >> position[0] >> position[1] >> position[2])) {
                throw std::runtime_error("Invalid OBJ vertex in " + path.string());
            }
            geometry.positions.push_back(position);
        } else if (record == "f") {
            std::vector<int> face;
            std::string token;
            while (stream >> token) {
                face.push_back(parsePositionIndex(token, geometry.positions.size()));
            }
            for (size_t index = 1; index + 1 < face.size(); ++index) {
                geometry.triangles.push_back({face[0], face[index], face[index + 1]});
                geometry.triangleGroups.push_back(activeGroup);
            }
        }
    }
    if (geometry.positions.empty() || geometry.triangles.empty()) {
        throw std::runtime_error("Scene OBJ contains no usable geometry: " + path.string());
    }
    // Keep the authored heights (e.g. crowns without trunks float above the
    // ground), as the voxel and ray-tracing modes do.
    return geometry;
}

// Placement：对象的一次摆放（位置文件的一行）。x、y 为场景平面坐标（m；x 对应场景 x 轴/宽度方向，
// y 对应场景 z 轴/深度方向，原点在域角），z 为抬升高度（m），scale 为缩放，rotation 为绕竖直轴转角（度）。
struct Placement {
    float x{};
    float y{};
    float z{};
    float scale{1.0f};
    float rotation{};
};

// 模块：DEM 高程场
// DemHeightField：把 DEM 栅格（行从北向南、列从西向东）保存为 float 数组，
// 并以其中的有效最小值为基准，给出场景坐标处的双线性插值相对高度（m）。
struct DemHeightField {
    int width{};
    int height{};
    float minimum{};
    std::vector<float> values;

    bool valid() const
    {
        return width >= 2 && height >= 2 &&
               values.size() == static_cast<size_t>(width) * height;
    }

    float heightAboveMinimum(float northX, float eastZ,
                             float sceneWidth, float sceneDepth) const
    {
        if (!valid()) return 0.0f;
        // GeoTIFF columns grow eastward; rows grow southward.
        const float u = std::clamp(eastZ / sceneDepth, 0.0f, 1.0f) * (width - 1);
        const float v = (1.0f - std::clamp(northX / sceneWidth, 0.0f, 1.0f)) * (height - 1);
        const int x0 = static_cast<int>(std::floor(u));
        const int y0 = static_cast<int>(std::floor(v));
        const int x1 = std::min(width - 1, x0 + 1);
        const int y1 = std::min(height - 1, y0 + 1);
        const auto at = [this](int column, int row) {
            return values[static_cast<size_t>(row) * width + column];
        };
        const float dx = u - x0;
        const float dy = v - y0;
        const float top = at(x0, y0) * (1.0f - dx) + at(x1, y0) * dx;
        const float bottom = at(x0, y1) * (1.0f - dx) + at(x1, y1) * dx;
        return std::max(0.0f, top * (1.0f - dy) + bottom * dy - minimum);
    }
};

// 作用：按工程 XML 的 isDEM/DEM 读取 GDAL 栅格第 1 波段为 Float32 高程。
// 说明：未启用（isDEM 非空且不为 1）或未给 DEM 文件时返回无效（空）高程场；
// NoData 与非有限值先不参与求最小值，随后被替换为最小值，使这些格点相对高度为 0。
DemHeightField readDemHeightField(const std::string& xml,
                                  const std::filesystem::path& inputPath)
{
    DemHeightField dem;
    const std::string enabled = trim(firstXmlTag(xml, "isDEM"));
    if (!enabled.empty() && enabled != "1") return dem;
    const std::string fileName = trim(firstXmlTag(xml, "DEM"));
    if (fileName.empty()) return dem;
    const std::filesystem::path path = resolveProjectAsset(inputPath, fileName);
    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(
        GDALOpen(path.string().c_str(), GA_ReadOnly));
    if (!dataset || dataset->GetRasterCount() < 1) {
        if (dataset) GDALClose(dataset);
        throw std::runtime_error("Cannot open DEM: " + path.string());
    }
    dem.width = dataset->GetRasterXSize();
    dem.height = dataset->GetRasterYSize();
    if (dem.width < 2 || dem.height < 2) {
        GDALClose(dataset);
        throw std::runtime_error("DEM must contain at least 2 x 2 cells: " + path.string());
    }
    dem.values.resize(static_cast<size_t>(dem.width) * dem.height);
    GDALRasterBand* band = dataset->GetRasterBand(1);
    int hasNoData = 0;
    const float noData = static_cast<float>(band->GetNoDataValue(&hasNoData));
    const CPLErr result = band->RasterIO(
        GF_Read, 0, 0, dem.width, dem.height, dem.values.data(),
        dem.width, dem.height, GDT_Float32, 0, 0);
    GDALClose(dataset);
    if (result != CE_None) {
        throw std::runtime_error("Cannot read DEM elevations: " + path.string());
    }
    dem.minimum = std::numeric_limits<float>::infinity();
    for (const float value : dem.values) {
        if (!std::isfinite(value) || (hasNoData && value == noData)) continue;
        dem.minimum = std::min(dem.minimum, value);
    }
    if (!std::isfinite(dem.minimum)) {
        throw std::runtime_error("DEM contains no valid elevations: " + path.string());
    }
    for (float& value : dem.values) {
        if (!std::isfinite(value) || (hasNoData && value == noData)) value = dem.minimum;
    }
    return dem;
}

// 作用：读取 objectPosition 文件，每行 "x y z [scale [rotation]]"，得到摆放列表。
// 说明：无法解析前三个数的行被跳过；缺省 scale=1、rotation=0；含非有限数或 scale<=0 的行丢弃；
// 没有任何有效摆放时退化为在场景中心放一个对象（高度 0、缩放 1、不旋转）。
std::vector<Placement> readPlacements(const std::filesystem::path& inputPath,
                                      const std::string& objectXml,
                                      float sceneWidth,
                                      float sceneDepth)
{
    std::vector<Placement> placements;
    const std::string positionFile = trim(firstXmlTag(objectXml, "objectPosition"));
    if (!positionFile.empty()) {
        const std::filesystem::path path = resolveProjectAsset(inputPath, positionFile);
        std::ifstream input(path);
        if (!input) {
            throw std::runtime_error("Cannot open object positions: " + path.string());
        }
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream stream(line);
            Placement placement;
            if (!(stream >> placement.x >> placement.y >> placement.z)) {
                continue;
            }
            if (!(stream >> placement.scale)) {
                placement.scale = 1.0f;
                stream.clear();
            }
            if (!(stream >> placement.rotation)) {
                placement.rotation = 0.0f;
            }
            if (std::isfinite(placement.x) && std::isfinite(placement.y) &&
                std::isfinite(placement.z) && std::isfinite(placement.scale) &&
                std::isfinite(placement.rotation) && placement.scale > 0.0f) {
                placements.push_back(placement);
            }
        }
    }
    if (placements.empty()) {
        placements.push_back({sceneWidth * 0.5f, sceneDepth * 0.5f, 0.0f, 1.0f, 0.0f});
    }
    return placements;
}

// 作用：把 OBJ 本地顶点依次缩放、绕竖直（y）轴旋转并平移到场景坐标，
// 场景域以中心为原点（x、z 范围 ±尺寸/2），y 为高度（m）。
// 说明：旋转矩阵里对 rotation 取负号，转角正方向的几何含义（待核对）。
Vec3 transformPosition(const Vec3& position,
                       const Placement& placement,
                       float sceneWidth,
                       float sceneDepth)
{
    const float radians = -placement.rotation * kPi / 180.0f;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const float x = position[0] * placement.scale;
    const float y = position[1] * placement.scale;
    const float z = position[2] * placement.scale;
    return {
        cosine * x + sine * z + placement.x - sceneWidth * 0.5f,
        y + placement.z,
        -sine * x + cosine * z + placement.y - sceneDepth * 0.5f
    };
}

// ObjectMaterialBinding：对象内某个网格（OBJ 组名）到场景材质表索引的绑定。
struct ObjectMaterialBinding {
    std::string meshName;
    uint32_t materialIndex{};
};

// 作用：把一个 OBJ 几何按给定摆放变换后逐三角形加入场景。
// 说明：材质按三角形所属组名与绑定的 meshName 忽略大小写匹配；无匹配时用第一条绑定的材质
// （无绑定则为 0）。超出场景域 [±宽/2, 高, ±深/2] 的部分由 appendTriangleInsideDomain 处理。
void appendGeometry(Scene& scene,
                    const ObjGeometry& geometry,
                    const Placement& placement,
                    float sceneWidth,
                    float sceneDepth,
                    float sceneHeight,
                    const std::vector<ObjectMaterialBinding>& bindings)
{
    const Vec3 domainMinimum{-sceneWidth * 0.5f, 0.0f,
                             -sceneDepth * 0.5f};
    const Vec3 domainMaximum{sceneWidth * 0.5f, sceneHeight,
                             sceneDepth * 0.5f};
    for (size_t triangleIndex = 0;
         triangleIndex < geometry.triangles.size(); ++triangleIndex) {
        const auto& triangle = geometry.triangles[triangleIndex];
        uint32_t materialIndex =
            bindings.empty() ? 0U : bindings.front().materialIndex;
        const std::string group =
            triangleIndex < geometry.triangleGroups.size()
            ? geometry.triangleGroups[triangleIndex] : std::string{};
        for (const ObjectMaterialBinding& binding : bindings) {
            if (!binding.meshName.empty() &&
                equalIgnoringCase(binding.meshName, group)) {
                materialIndex = binding.materialIndex;
                break;
            }
        }
        appendTriangleInsideDomain(
            scene,
            transformPosition(geometry.positions[triangle[0]], placement,
                              sceneWidth, sceneDepth),
            transformPosition(geometry.positions[triangle[1]], placement,
                              sceneWidth, sceneDepth),
            transformPosition(geometry.positions[triangle[2]], placement,
                              sceneWidth, sceneDepth),
            domainMinimum, domainMaximum, materialIndex);
    }
}

// 作用：由 shapes 标签（"长,宽,高"）生成底面在 y=0 的长方体（12 个三角形），
// 用于没有 OBJ 文件的简单对象；解析失败或尺寸非正时退化为 1 m 立方体。
ObjGeometry cubeGeometry(const std::string& objectXml)
{
    float length = 1.0f;
    float width = 1.0f;
    float height = 1.0f;
    std::istringstream values(firstXmlTag(objectXml, "shapes"));
    char separator = 0;
    values >> length >> separator >> width >> separator >> height;
    if (!(length > 0.0f) || !(width > 0.0f) || !(height > 0.0f)) {
        length = width = height = 1.0f;
    }
    const float halfX = length * 0.5f;
    const float halfZ = width * 0.5f;
    ObjGeometry geometry;
    geometry.positions = {
        {-halfX, 0.0f, -halfZ}, {halfX, 0.0f, -halfZ},
        {halfX, 0.0f, halfZ}, {-halfX, 0.0f, halfZ},
        {-halfX, height, -halfZ}, {halfX, height, -halfZ},
        {halfX, height, halfZ}, {-halfX, height, halfZ}
    };
    geometry.triangles = {
        {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
        {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}
    };
    geometry.triangleGroups.assign(geometry.triangles.size(), std::string{});
    return geometry;
}

// 作用：为工程场景生成地面三角网：在整个模拟域 [±宽/2, ±深/2] 上按 voxelSize 划分网格，
// 每格 2 个三角形（法向朝上 +y），顶点高度取 DEM 相对最小值的双线性插值（无 DEM 时为 0）。
// 说明：每边格数上限为 maximumGridResolution；DEM 被重采样到该网格，与 DEM 原始像素数无关。
// 高度整体减 0.01 m，使地面略低于 y=0 的物体底面（与 loadTreeWithSoil 的 -0.01 一致，
// 推测为避免共面，待核对）。
void appendProjectGround(Scene& scene,
                         float sceneWidth,
                         float sceneDepth,
                         float voxelSize,
                         uint32_t maximumGridResolution,
                         uint32_t materialIndex,
                         const DemHeightField& dem)
{
    const float x0 = -sceneWidth * 0.5f;
    const float z0 = -sceneDepth * 0.5f;
    const uint32_t requestedColumns = std::max(
        1U, static_cast<uint32_t>(std::ceil(sceneWidth / voxelSize)));
    const uint32_t requestedRows = std::max(
        1U, static_cast<uint32_t>(std::ceil(sceneDepth / voxelSize)));
    // The complete DEM is bilinearly resampled to the scene grid.  Its source
    // pixel dimensions therefore do not constrain the configured scene size.
    const uint32_t columns = std::clamp(
        requestedColumns, 1U, maximumGridResolution);
    const uint32_t rows = std::clamp(
        requestedRows, 1U, maximumGridResolution);
    const float cellWidth = sceneWidth / static_cast<float>(columns);
    const float cellDepth = sceneDepth / static_cast<float>(rows);
    scene.vertices.reserve(
        scene.vertices.size() + static_cast<size_t>(columns) * rows * 6U);
    // 最后一行/列的终点直接取域边界，避免浮点累积误差使地面与域边界之间留缝。
    for (uint32_t row = 0; row < rows; ++row) {
        const float za = z0 + cellDepth * static_cast<float>(row);
        const float zb = row + 1U == rows
            ? z0 + sceneDepth
            : z0 + cellDepth * static_cast<float>(row + 1U);
        for (uint32_t column = 0; column < columns; ++column) {
            const float xa = x0 + cellWidth * static_cast<float>(column);
            const float xb = column + 1U == columns
                ? x0 + sceneWidth
                : x0 + cellWidth * static_cast<float>(column + 1U);
            const float sceneXa = xa + sceneWidth * 0.5f;
            const float sceneXb = xb + sceneWidth * 0.5f;
            const float sceneYa = za + sceneDepth * 0.5f;
            const float sceneYb = zb + sceneDepth * 0.5f;
            const float haa = dem.heightAboveMinimum(sceneXa, sceneYa, sceneWidth, sceneDepth) - 0.01f;
            const float hab = dem.heightAboveMinimum(sceneXa, sceneYb, sceneWidth, sceneDepth) - 0.01f;
            const float hba = dem.heightAboveMinimum(sceneXb, sceneYa, sceneWidth, sceneDepth) - 0.01f;
            const float hbb = dem.heightAboveMinimum(sceneXb, sceneYb, sceneWidth, sceneDepth) - 0.01f;
            appendTriangle(scene, {xa, haa, za}, {xb, hbb, zb},
                           {xb, hba, za}, materialIndex);
            appendTriangle(scene, {xa, haa, za}, {xa, hab, zb},
                           {xb, hbb, zb}, materialIndex);
        }
    }
}

// 作用：读取水体面元的初始温度模式，写入 material.waterInitialMode / waterInitialK。
// 模式：firstAir 或空 = 0（取首个气象节点气温）；dailyMeanAir = 1（首日气温均值）；
// fixed = 2（waterInitialTemperature，单位 °C，转为 K，须在 223–373 K 之间）。
// 实际赋初值在逐步积分入口处按此模式完成。
// Optional water start temperature (waterSet or surfaceEnergy block).
void readWaterInitial(const std::string& block, SurfaceParameters& material)
{
    const std::string mode = trim(firstXmlTag(block, "waterInitialMode"));
    if (mode == "dailyMeanAir") material.waterInitialMode = 1;
    else if (mode == "fixed") {
        material.waterInitialMode = 2;
        material.waterInitialK = xmlFloat(block, "waterInitialTemperature", 20.0f) + 273.15f;
        if (!(material.waterInitialK > 223.0f && material.waterInitialK < 373.0f))
            throw std::runtime_error("Invalid FacetEB water initial temperature");
    } else if (!mode.empty() && mode != "firstAir")
        throw std::runtime_error("waterInitialMode must be firstAir, dailyMeanAir or fixed");
}

// 模块：工程/XML 场景加载（FacetRT 与 FacetEB 共用）
// 输入：工程 XML 文本；输出：Scene（域大小、周期邻域、太阳角、对象与地面三角形、材质表等）。
Scene loadProjectScene(const std::string& inputFile)
{
    const std::filesystem::path inputPath(inputFile);
    const std::string xml = readTextFile(inputFile);
    // 步骤 1：读取域尺寸（宽 sceneSizeX、深 sceneSizeY、高 Height，m）与体元尺寸 voxelSize，
    // 均限幅到不小于 0.01 m；读取 DEM（如有）。
    const float sceneWidth = std::max(0.01f, xmlFloat(xml, "sceneSizeX", 8.0f));
    const float sceneDepth = std::max(0.01f, xmlFloat(xml, "sceneSizeY", 8.0f));
    const float sceneHeight = std::max(0.01f, xmlFloat(xml, "Height", 4.0f));
    const float voxelSize = std::max(0.01f, xmlFloat(xml, "voxelSize", 1.0f));
    const DemHeightField dem = readDemHeightField(xml, inputPath);
    Scene scene;
    // 步骤 2：周期邻域（在水平方向加入若干圈邻接副本参与遮挡），个数上限 20，并向上取整到完整环。
    const int requestedNeighbors = std::clamp(static_cast<int>(std::lround(
        xmlFloat(xml, "periodicNeighborCount", 0.0f))), 0, 20);
    // Tile offsets come in symmetric rings (4 edge, 4 corner, 4 second-edge,
    // 8 knight); a partial ring would shadow the scene from one side only.
    scene.periodicNeighborCount = requestedNeighbors == 0 ? 0U
        : requestedNeighbors <= 4 ? 4U
        : requestedNeighbors <= 8 ? 8U
        : requestedNeighbors <= 12 ? 12U : 20U;
    scene.periodicSizeX = sceneWidth;
    scene.periodicSizeZ = sceneDepth;
    if (std::max(sceneWidth, sceneDepth) >= kLargeSceneExtent) {
        scene.visibilityRasterSize = kLargeSceneRasterSize;
        // Preserve the existing A-buffer memory footprint when the raster
        // doubles in each dimension. Large scenes gain four times the spatial
        // sampling without requiring a multi-gigabyte fragment buffer.
        scene.visibilityRasterLayers = kLargeSceneRasterLayers;
    }
    // Developer/validation override of the visibility raster (e.g. a 4096
    // reference run to measure sampling bias): 512..8192, power of two.
    if (const char* requestedRaster = std::getenv("HISTREAM_FACET_RASTER_SIZE")) {
        const int size = std::atoi(requestedRaster);
        if (size >= 512 && size <= 8192 && (size & (size - 1)) == 0)
            scene.visibilityRasterSize = static_cast<uint32_t>(size);
    }
    // 步骤 3：读取辐射参数（波段、对象/地面等缺省光学参数）与光学系数库；背景各向异性强度限幅 0–1，
    // 仅 Hapke 模型保留，其他背景模型置 0。
    const FacetRTParameters radiativeParameters =
        facetRTParametersFromInput(xml, inputPath);
    scene.backgroundAngularStrength = std::clamp(
        xmlFloat(xml, "backgroundAngularStrength", 0.0f), 0.0f, 1.0f);
    const std::string backgroundModel = trim(
        firstXmlTag(xml, "backgroundHeterogeneity"));
    if (!backgroundModel.empty() && backgroundModel != "Hapke" &&
        backgroundModel != "hapke") {
        scene.backgroundAngularStrength = 0.0f;
    }
    const OptCoeff opticalCoefficients = loadOpticalCoefficients(xml, inputPath);

    // 步骤 4：读取太阳角 lightAngle = "天顶角,方位角"（度）；解析失败时缺省 30°、135°。
    std::string lightAngles = firstXmlTag(xml, "lightAngle");
    const size_t nestedTagEnd = lightAngles.find('>');
    if (nestedTagEnd != std::string::npos) {
        lightAngles = lightAngles.substr(nestedTagEnd + 1U);
    }
    std::istringstream angleValues(lightAngles);
    char separator = 0;
    if (!(angleValues >> scene.sunZenith >> separator >> scene.sunAzimuth)) {
        scene.sunZenith = 30.0f;
        scene.sunAzimuth = 135.0f;
    }

    // 步骤 5：逐个对象块，建立“网格名 → 材质”绑定（光学名、热学名、生理名按索引一一对应，
    // 名字数量不足时沿用最后一个），再读取该对象的几何与摆放。
    for (const std::string& objectXml : xmlObjectBlocks(xml)) {
        const std::vector<std::string> meshNames =
            nameList(firstXmlTag(objectXml, "meshNames"));
        const std::vector<std::string> spectralNames =
            nameList(firstXmlTag(objectXml, "spectralNames"));
        const std::vector<std::string> thermalNames =
            nameList(firstXmlTag(objectXml, "thermalNames"));
        const auto physicalNames=nameList(firstXmlTag(objectXml,"bioNames"));
        const auto canopyNames=nameList(firstXmlTag(objectXml,"canopyNames"));
        const size_t bindingCount = std::max<size_t>(
            1U, std::max(meshNames.size(),
                         std::max(spectralNames.size(), thermalNames.size())));
        std::vector<ObjectMaterialBinding> bindings;
        bindings.reserve(bindingCount);
        for (size_t index = 0; index < bindingCount; ++index) {
            // 对每个绑定：以对象缺省光学参数为底，按 componentType 标记输出分量
            // （Soil=1、Vegetation=2、Building=3、Water=5，其余 0），再叠加光谱/热学/生理参数。
            SurfaceParameters material = radiativeParameters.object;
            const std::string componentType = trim(firstXmlTag(objectXml, "componentType"));
            material.outputComponent = componentType == "Soil" ? 1 : componentType == "Vegetation" ? 2
                : componentType == "Building" ? 3 : componentType == "Water" ? 5 : 0;
            const std::string spectralName = spectralNames.empty()
                ? std::string{}
                : spectralNames[std::min(index, spectralNames.size() - 1U)];
            const std::string thermalName = thermalNames.empty()
                ? std::string{}
                : thermalNames[std::min(index, thermalNames.size() - 1U)];
            loadSurfaceParameters(xml, spectralName, thermalName,
                                  radiativeParameters.wavelengths,
                                  opticalCoefficients, inputPath, material);
            const auto physicalName=physicalNames.empty()?std::string{}:physicalNames[std::min(index,physicalNames.size()-1U)];
            // An empty name would match the first surfaceEnergy block of any material.
            const auto physical=physicalName.empty()?std::string{}:namedXmlBlock(xml,"surfaceEnergy",physicalName);
            // 无 surfaceEnergy 块时的兼容分支：按对象类型推断植被/水体（直接 XML 输入或对象材质缺失）。
            if(physical.empty()) {
                // No explicit surface-energy block (direct XML input, or an object
                // whose material is missing from the project): classify by the
                // object type instead of assuming leaves. Rule, in order:
                //  1. componentType Vegetation -> leaf, unless the material name
                //     marks woody parts (*wood*, e.g. tree_trunk_wood);
                //  2. any other non-empty componentType (Building, Water, Soil, ...)
                //     -> solid surface, no stomata/photosynthesis;
                //  3. no componentType (legacy XML): leafBio block -> leaf,
                //     soilSet/waterSet block -> solid, otherwise leaf (legacy default).
                std::string lowered = physicalName;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const bool woody = lowered.find("wood") != std::string::npos;
                if (componentType == "Vegetation") material.vegetation = !woody;
                else if (!componentType.empty()) material.vegetation = false;
                else if (!physicalName.empty() && !namedXmlBlock(xml, "leafBio", physicalName).empty())
                    material.vegetation = true;
                else if (!physicalName.empty() &&
                         (!namedXmlBlock(xml, "soilSet", physicalName).empty() ||
                          !namedXmlBlock(xml, "waterSet", physicalName).empty()))
                    material.vegetation = false;
                else material.vegetation = !woody;
                // Direct XML: a Water object reads its mixed layer from waterSet.
                const auto waterBlock = physicalName.empty() ? std::string{} : namedXmlBlock(xml, "waterSet", physicalName);
                if (!material.vegetation && (componentType == "Water" || (componentType.empty() && !waterBlock.empty()))) {
                    material.water = true;
                    material.heatCapacityPerArea = xmlFloat(waterBlock, "heatCapacity", 4186000.0f) *
                        std::max(0.01f, xmlFloat(waterBlock, "mixingDepth", 0.5f));
                    material.surfaceResistance = std::max(0.0f, xmlFloat(waterBlock, "rss", 0.0f));
                    material.evaporationCoefficient = std::clamp(xmlFloat(waterBlock, "evaporationCoefficient", 1.0f), 0.0f, 1.0f);
                    readWaterInitial(waterBlock, material);
                }
            }
            // 有 surfaceEnergy 块时：读取光伏、植被、水体、集总热质量及叶片生化参数。
            // 植被与光伏互斥，水体要求非植被非光伏；缺省值保持与旧工程一致。
            if(!physical.empty()) {
                material.photovoltaic=xmlFloat(physical,"photovoltaic",0)>0.5f;
                material.vegetation=xmlFloat(physical,"vegetation",1)>0.5f && !material.photovoltaic;
                material.pvEta25=xmlFloat(physical,"eta25",.22f);
                material.pvGamma=xmlFloat(physical,"gamma",-.0035f);
                material.pvBifaciality=xmlFloat(physical,"bifaciality",0);
                material.heatCapacityPerArea=xmlFloat(physical,"heatCapacityPerArea",60000);
                material.convectiveScale=xmlFloat(physical,"convectiveScale",1);
                material.water=xmlFloat(physical,"water",0)>0.5f && !material.vegetation && !material.photovoltaic;
                if(material.water) {
                    material.surfaceResistance=std::max(0.0f,xmlFloat(physical,"waterRss",0));
                    material.evaporationCoefficient=std::clamp(xmlFloat(physical,"evaporationCoefficient",1),0.0f,1.0f);
                    readWaterInitial(physical, material);
                }
                material.lumpedGroup=trim(firstXmlTag(physical,"lumpedThermalGroup"));
                if(!material.lumpedGroup.empty()) {
                    material.lumped={xmlFloat(physical,"lumpedCapacity",0),xmlFloat(physical,"lumpedConductance",0),
                        xmlFloat(physical,"lumpedAirConductance",0),xmlFloat(physical,"lumpedPower",0),
                        xmlFloat(physical,"lumpedStart",0),xmlFloat(physical,"lumpedStop",0),xmlFloat(physical,"lumpedInitialK",298.15f)};
                    material.lumped.validate();
                    material.lumpedWeight=xmlFloat(physical,"lumpedWeight",1);
                    material.lumpedMaxSurfaceK=xmlFloat(physical,"lumpedMaxSurfaceK",500);
                    if(material.vegetation || material.photovoltaic || !std::isfinite(material.lumpedWeight) ||
                       material.lumpedWeight<0 || material.lumpedWeight>1 || !std::isfinite(material.lumpedMaxSurfaceK) ||
                       material.lumpedMaxSurfaceK<material.lumped.initialK || material.lumpedMaxSurfaceK>1500)
                        throw std::runtime_error("Lumped thermal group requires finite solid-material weights");
                }
                if(material.vegetation) {
                    LeafBiochemistry& leaf=material.leaf;
                    leaf.vcmax25=std::max(0.0f,xmlFloat(physical,"Vcmax",leaf.vcmax25));
                    leaf.ballBerrySlope=std::max(0.0f,xmlFloat(physical,"m",leaf.ballBerrySlope));
                    leaf.ballBerry0=std::max(0.0f,xmlFloat(physical,"BallBerry",leaf.ballBerry0));
                    leaf.type=xmlFloat(physical,"Type",3)>3.5f?4:3;
                    leaf.kV=std::max(0.0f,xmlFloat(physical,"kV",leaf.kV));
                    leaf.rdParam=std::max(0.0f,xmlFloat(physical,"Rdparam",leaf.rdParam));
                    leaf.stressFactor=std::clamp(xmlFloat(physical,"stressfactor",1),0.0f,1.0f);
                    leaf.temperatureCorrection=xmlFloat(physical,"Tcor",0)>0.5f?1:0;
                    leaf.annualTemperature=xmlFloat(physical,"Tyear",leaf.annualTemperature);
                    leaf.beta=xmlFloat(physical,"beta",leaf.beta);
                    leaf.qLs=xmlFloat(physical,"qLs",leaf.qLs);
                    leaf.sustainedNPQ=xmlFloat(physical,"kNPQs",leaf.sustainedNPQ);
                    std::istringstream tparam(trim(firstXmlTag(physical,"Tparam")));
                    std::array<float,5> values{};
                    if(tparam>>values[0]>>values[1]>>values[2]>>values[3]>>values[4]) leaf.tparam=values;
                }
                if(material.photovoltaic && (!(material.pvEta25>=0 && material.pvEta25<=.5f) ||
                    !(material.pvGamma>=-.02f && material.pvGamma<=0) || !(material.pvBifaciality>=0 && material.pvBifaciality<=1) ||
                    !(material.heatCapacityPerArea>=0) || !(material.convectiveScale>0)))
                    throw std::runtime_error("Invalid photovoltaic material parameters: "+physicalName);
                if(material.photovoltaic && (material.tirTransmittance>1e-6f ||
                    std::any_of(material.energyTransmittance.begin(),material.energyTransmittance.end(),[](float t){return t>1e-6f;})))
                    throw std::runtime_error("Photovoltaic surface must be opaque: "+physicalName);
            }
            {
                // 以下三段共用同一参数块 wallBlock：墙/屋顶多层板、船体吃水线与内热源、车辆舱体参数。
                // Wall/roof slab: surfaceEnergy (project JSON) or soilSet (direct XML).
                const std::string wallBlock = !physical.empty() ? physical
                    : physicalName.empty() ? std::string{} : namedXmlBlock(xml, "soilSet", physicalName);
                const std::string wallMode = trim(firstXmlTag(wallBlock, "wallMode"));
                if ((wallMode == "wall" || wallMode == "1") && !material.vegetation && !material.photovoltaic && !material.water) {
                    if (!material.lumpedGroup.empty())
                        throw std::runtime_error("wallMode wall cannot be combined with lumpedThermal: " + physicalName);
                    auto& slab = material.wallSlab;
                    // Layered construction "t k rho c; ..." (outer to inner);
                    // absent = one homogeneous layer from thickness/lambdas/rhos/cs.
                    const std::string layerText = trim(firstXmlTag(wallBlock, "wallLayers"));
                    if (!layerText.empty()) {
                        std::istringstream rows(layerText);
                        std::string row;
                        slab.layerCount = 0;
                        while (std::getline(rows, row, ';')) {
                            if (trim(row).empty()) continue;
                            if (slab.layerCount == facetphysics::maxWallLayers)
                                throw std::runtime_error("FacetEB wall has more than 6 layers: " + physicalName);
                            std::istringstream values(row);
                            double thickness = 0, conductivity = 0, density = 0, specificHeat = 0;
                            if (!(values >> thickness >> conductivity >> density >> specificHeat))
                                throw std::runtime_error("Invalid FacetEB wall layer (thickness lambda rho cp): " + physicalName);
                            slab.layers[slab.layerCount++] = {thickness, conductivity, density * specificHeat};
                        }
                    } else {
                        slab.layers[0].thickness = xmlFloat(wallBlock, "thickness", .2f);
                        slab.layers[0].conductivity = xmlFloat(wallBlock, "lambdas", 1.4f);
                        slab.layers[0].volumetricCapacity = double(xmlFloat(wallBlock, "rhos", 2300)) * xmlFloat(wallBlock, "cs", 880);
                    }
                    slab.indoorK = double(xmlFloat(wallBlock, "indoorTemperature", 22)) + 273.15;
                    slab.indoorCoefficient = xmlFloat(wallBlock, "indoorHeatTransferCoefficient", 8);
                    slab.validate();
                    material.wall = true;
                }
                // 船体选项：航速（0–100 m/s）、吃水线（固定高度或取场景水体）、水侧换热、航向与风向夹角、
                // 内热源（flux W m-2 / power W）。吃水线以下面元的处理在后续时间步积分中完成。
                // Ship hull options (same block as the wall tags).
                if (!material.vegetation && !material.photovoltaic && !material.water) {
                    material.shipSpeed = xmlFloat(wallBlock, "shipSpeed", 0);
                    if (!std::isfinite(material.shipSpeed) || material.shipSpeed < 0 || material.shipSpeed > 100)
                        throw std::runtime_error("Ship speed must be 0-100 m/s: " + physicalName);
                    const bool fixedWaterline = !trim(firstXmlTag(wallBlock, "waterlineHeight")).empty();
                    material.waterlineFromWater = !fixedWaterline && trim(firstXmlTag(wallBlock, "waterlineSource")) == "water";
                    if (fixedWaterline || material.waterlineFromWater) {
                        material.waterline = true;
                        material.waterlineHeight = fixedWaterline ? xmlFloat(wallBlock, "waterlineHeight", 0) : 0.0f;
                        material.waterlineOffset = material.waterlineFromWater ? xmlFloat(wallBlock, "waterlineOffset", 0) : 0.0f;
                        material.waterCoefficient = xmlFloat(wallBlock, "waterHeatTransferCoefficient", 500);
                        if (!trim(firstXmlTag(wallBlock, "waterTemperature")).empty())
                            material.waterK = xmlFloat(wallBlock, "waterTemperature", 20) + 273.15f;
                        if (!std::isfinite(material.waterlineHeight) || !std::isfinite(material.waterlineOffset) ||
                            !(material.waterCoefficient > 0) ||
                            !std::isfinite(material.waterCoefficient) ||
                            (material.waterK >= 0 && !(material.waterK > 250 && material.waterK < 340)))
                            throw std::runtime_error("Invalid ship waterline parameters: " + physicalName);
                        material.waterShield = trim(firstXmlTag(wallBlock, "waterShielding")) == "1";
                        material.waterFeedback = trim(firstXmlTag(wallBlock, "waterFeedback")) == "1";
                        if (material.waterFeedback && material.waterK >= 0)
                            throw std::runtime_error("waterFeedback needs the scene water temperature (no waterTemperature): " + physicalName);
                    }
                    if (!trim(firstXmlTag(wallBlock, "shipHeading")).empty() && !trim(firstXmlTag(wallBlock, "shipWindDirection")).empty()) {
                        const float heading = xmlFloat(wallBlock, "shipHeading", 0), windFrom = xmlFloat(wallBlock, "shipWindDirection", 0);
                        if (!std::isfinite(heading) || !std::isfinite(windFrom) || heading < 0 || heading > 360 || windFrom < 0 || windFrom > 360)
                            throw std::runtime_error("Ship heading and wind direction must be 0-360 degrees: " + physicalName);
                        material.shipWindCosine = static_cast<float>(std::cos((double(heading) - double(windFrom)) * kPi / 180.0));
                    }
                    material.internalFlux = xmlFloat(wallBlock, "internalHeatFlux", 0);
                    material.internalPower = xmlFloat(wallBlock, "internalHeatPower", 0);
                    if (!std::isfinite(material.internalFlux) || !std::isfinite(material.internalPower) ||
                        std::abs(material.internalFlux) > 1.0e5f || std::abs(material.internalPower) > 1.0e9f)
                        throw std::runtime_error("Invalid internal heat source (|flux| <= 1e5 W/m2, |power| <= 1e9 W): " + physicalName);
                    // 车辆热模型参数：vehiclePart（0–4，部件类别）；出现 vcSpeed 或 vcEnabled 标签才启用
                    // 座舱参数（乘员、空调、发动机、通风等），均带范围检查，超界即抛异常。
                    // 参数含义详见 docs/vehicle-thermal.md。
                    // Vehicle parts and cabin parameters (absent = old behaviour).
                    material.vehiclePart = static_cast<int>(xmlFloat(wallBlock, "vehiclePart", 0));
                    if (material.vehiclePart < 0 || material.vehiclePart > 4)
                        throw std::runtime_error("vehiclePart must be 0-4: " + physicalName);
                    material.meshName = meshNames.empty() ? std::string{} : meshNames[std::min(index, meshNames.size() - 1U)];
                    if (!trim(firstXmlTag(wallBlock, "vcSpeed")).empty() || !trim(firstXmlTag(wallBlock, "vcEnabled")).empty()) {
                        material.vehicleHasParams = true;
                        material.cabinEnabled = xmlFloat(wallBlock, "vcEnabled", 0) > 0.5f;
                        material.occupants = static_cast<int>(xmlFloat(wallBlock, "vcOccupants", 0));
                        material.metabolicW = xmlFloat(wallBlock, "vcMetabolicW", 100);
                        material.cabinVolume = xmlFloat(wallBlock, "vcVolume", 2.8f);
                        material.interiorCapacity = xmlFloat(wallBlock, "vcInteriorCapacity", 80000);
                        material.interiorArea = xmlFloat(wallBlock, "vcInteriorArea", 8);
                        material.interiorH = xmlFloat(wallBlock, "vcInteriorH", 8);
                        material.absorbedFraction = xmlFloat(wallBlock, "vcAbsorbedFraction", 0.8f);
                        material.ventilationAch = xmlFloat(wallBlock, "vcVentilationAch", 0.5f);
                        material.crackedAch = xmlFloat(wallBlock, "vcCrackedAch", 10);
                        material.windowCracked = xmlFloat(wallBlock, "vcWindowCracked", 0) > 0.5f;
                        material.acOn = xmlFloat(wallBlock, "vcAcOn", 0) > 0.5f;
                        material.acPower = xmlFloat(wallBlock, "vcAcPower", 3500);
                        material.acSetpointC = xmlFloat(wallBlock, "vcAcSetpoint", 24);
                        material.acBandK = xmlFloat(wallBlock, "vcAcBand", 2);
                        material.engineOn = xmlFloat(wallBlock, "vcEngineOn", 0) > 0.5f;
                        material.enginePowerW = xmlFloat(wallBlock, "vcEnginePower", 15000);
                        material.engineShare = xmlFloat(wallBlock, "vcEngineShare", 0.3f);
                        material.vehicleSpeed = xmlFloat(wallBlock, "vcSpeed", 0);
                        if (!trim(firstXmlTag(wallBlock, "vcHeading")).empty() && !trim(firstXmlTag(wallBlock, "vcWindDirection")).empty())
                            material.vehicleWindCosine = static_cast<float>(std::cos((double(xmlFloat(wallBlock, "vcHeading", 0)) -
                                double(xmlFloat(wallBlock, "vcWindDirection", 0))) * kPi / 180.0));
                        if (!(material.vehicleSpeed >= 0 && material.vehicleSpeed <= 100) || material.occupants < 0 || material.occupants > 8 ||
                            !(material.metabolicW >= 0 && material.metabolicW <= 1000) || !(material.cabinVolume > 0.2 && material.cabinVolume < 100) ||
                            !(material.interiorCapacity > 100) || !(material.interiorArea > 0) || !(material.interiorH > 0) ||
                            !(material.absorbedFraction >= 0 && material.absorbedFraction <= 1) ||
                            !(material.ventilationAch >= 0 && material.ventilationAch <= 100) || !(material.crackedAch >= 0 && material.crackedAch <= 200) ||
                            !(material.acPower >= 0 && material.acPower <= 2.0e5) || !(material.acBandK > 0.01) ||
                            !(material.acSetpointC > -20 && material.acSetpointC < 50) ||
                            !(material.enginePowerW >= 0 && material.enginePowerW <= 1.0e6) || !(material.engineShare >= 0 && material.engineShare <= 1))
                            throw std::runtime_error("Invalid vehicle cabin parameters: " + physicalName);
                        // Interior nodes and engine / bay nodes (absent = lumped interior, engine heat on the underbody).
                        const auto csv = [&](const char* tag, size_t count, std::vector<double>& out) {
                            const std::string text = trim(firstXmlTag(wallBlock, tag));
                            if (text.empty()) return false;
                            std::string spaced = text;
                            std::replace(spaced.begin(), spaced.end(), ',', ' ');
                            std::istringstream stream(spaced);
                            out.clear();
                            double value = 0;
                            while (stream >> value) out.push_back(value);
                            if (out.size() != count) throw std::runtime_error(std::string("Vehicle ") + tag + " needs " + std::to_string(count) + " values: " + physicalName);
                            for (double v : out) if (!std::isfinite(v)) throw std::runtime_error(std::string("Invalid vehicle ") + tag + ": " + physicalName);
                            return true;
                        };
                        if (xmlFloat(wallBlock, "vcIntModel", 0) > 0.5f) {
                            std::vector<double> v;
                            material.interiorMulti = true;
                            if (csv("vcIntShares", 4, v)) std::copy(v.begin(), v.end(), material.intShare.begin());
                            if (csv("vcIntCapacities", 4, v)) std::copy(v.begin(), v.end(), material.intCapacity.begin());
                            if (csv("vcIntHcA", 4, v)) std::copy(v.begin(), v.end(), material.intHcA.begin());
                            if (csv("vcIntRadArea", 4, v)) std::copy(v.begin(), v.end(), material.intRadArea.begin());
                            material.shellRadFraction = xmlFloat(wallBlock, "vcShellRadFraction", 0.45f);
                            material.seatEffusivity = xmlFloat(wallBlock, "vcSeatEffusivity", 300);
                            double shareSum = 0;
                            bool valid = material.shellRadFraction >= 0 && material.shellRadFraction <= 0.95 && material.seatEffusivity > 0;
                            for (size_t k = 0; k < 4; ++k) {
                                shareSum += material.intShare[k];
                                valid = valid && material.intShare[k] >= 0 && material.intCapacity[k] > 10 && material.intHcA[k] > 0 && material.intRadArea[k] >= 0;
                            }
                            if (!valid || !(shareSum > 0.999 && shareSum < 1.001))
                                throw std::runtime_error("Invalid vehicle interior nodes (shares sum to 1, capacity > 10 J/K, hcA > 0): " + physicalName);
                        }
                        if (xmlFloat(wallBlock, "vcEngModel", 0) > 0.5f) {
                            std::vector<double> v;
                            material.engineMulti = true;
                            material.engineWarm = xmlFloat(wallBlock, "vcEngWarm", 0) > 0.5f;
                            material.engineWarmC = xmlFloat(wallBlock, "vcEngWarmTemp", 90);
                            if (csv("vcEngCaps", 2, v)) { material.engineBlockCapacity = v[0]; material.engineBayCapacity = v[1]; }
                            material.engineBlockBay = xmlFloat(wallBlock, "vcEngBlockBay", 40);
                            if (csv("vcEngBayVent", 2, v)) { material.bayVent0 = v[0]; material.bayVentK = v[1]; }
                            if (csv("vcEngSplit", 3, v)) std::copy(v.begin(), v.end(), material.engineSplit.begin());
                            material.firewallG = xmlFloat(wallBlock, "vcFirewallG", 2);
                            const double splitSum = material.engineSplit[0] + material.engineSplit[1] + material.engineSplit[2];
                            if (!(material.engineWarmC > -20 && material.engineWarmC < 200) || !(material.engineBlockCapacity > 100) ||
                                !(material.engineBayCapacity > 10) || !(material.engineBlockBay > 0) || !(material.bayVent0 >= 0 && material.bayVentK >= 0) ||
                                !(material.bayVent0 + material.bayVentK > 0) || !(material.firewallG >= 0 && material.firewallG <= 1000) ||
                                std::any_of(material.engineSplit.begin(), material.engineSplit.end(), [](double s) { return !(s >= 0); }) ||
                                !(splitSum > 0.999 && splitSum < 1.001))
                                throw std::runtime_error("Invalid vehicle engine model (shares sum to 1, capacities and conductances positive): " + physicalName);
                        }
                    }
                }
            }
            const uint32_t materialIndex =
                static_cast<uint32_t>(scene.materials.size());
            if (material.photovoltaic) material.outputComponent = 4;
            const auto canopyName=canopyNames.empty()?std::string{}:canopyNames[std::min(index,canopyNames.size()-1U)];
            const auto canopyBlock=namedXmlBlock(xml,"canopyAero",canopyName);
            if(!canopyBlock.empty()) material.leafWidth=std::max(0.001f,xmlFloat(canopyBlock,"leafwidth",material.leafWidth));
            scene.materials.push_back(std::move(material));
            bindings.push_back({
                meshNames.empty()
                    ? std::string{}
                    : meshNames[std::min(index, meshNames.size() - 1U)],
                materialIndex
            });
        }
        // 几何来源：优先 fileName，其次 objectfile；两者都为空时按 shapes 生成长方体。
        std::string modelFile = trim(firstXmlTag(objectXml, "fileName"));
        if (modelFile.empty()) {
            modelFile = trim(firstXmlTag(objectXml, "objectfile"));
        }
        const ObjGeometry geometry = modelFile.empty()
            ? cubeGeometry(objectXml)
            : readObjGeometry(resolveProjectAsset(inputPath, modelFile));
        // 若该对象有任一材质 vehiclePart > 0，则视为车辆对象：每个摆放实例登记名字（objName#序号），
        // 并在 facetInstance 中记录面元所属实例编号（0 = 非车辆面元，实例编号从 1 起）。
        bool vehicleObject = false;
        for (const auto& binding : bindings)
            if (binding.materialIndex < scene.materials.size() && scene.materials[binding.materialIndex].vehiclePart > 0)
                vehicleObject = true;
        size_t placementIndex = 0;
        for (Placement placement :
             readPlacements(inputPath, objectXml, sceneWidth, sceneDepth)) {
            // 对象随地形抬升：摆放高度加上 DEM 在摆放点（场景坐标，未减域中心）处相对最低点的高度。
            placement.z += dem.heightAboveMinimum(
                placement.x, placement.y, sceneWidth, sceneDepth);
            const uint32_t facetsBefore = scene.facetCount();
            appendGeometry(scene, geometry, placement, sceneWidth, sceneDepth,
                           sceneHeight, bindings);
            if (vehicleObject) {
                scene.facetInstance.resize(facetsBefore, 0U);
                scene.vehicleNames.push_back(xmlAttribute(objectXml.substr(0, objectXml.find('>') + 1), "objName") + "#" + std::to_string(placementIndex));
                scene.facetInstance.resize(scene.facetCount(), static_cast<uint32_t>(scene.vehicleNames.size()));
            }
            ++placementIndex;
        }
    }

    // 步骤 6：对象三角形全部加入后，其数目记为 leafFacetCount（对象面元数，地面在其后追加），
    // 再追加地面材质与地面三角网。
    scene.leafFacetCount = scene.facetCount();
    const uint32_t groundMaterialIndex =
        static_cast<uint32_t>(scene.materials.size());
    scene.materials.push_back(radiativeParameters.ground);
    appendProjectGround(scene, sceneWidth, sceneDepth, voxelSize,
                        scene.visibilityRasterSize / 4U,
                        groundMaterialIndex, dem);
    // 步骤 7：确定可见性栅格（A-buffer）层数：先按场景自适应，再允许环境变量
    // HISTREAM_FACET_RASTER_LAYERS 显式加深（仅 64/128/256，只升不降）。
    const uint32_t initialRasterLayers = scene.visibilityRasterLayers;
    // DEFERRED[中] 内存: 大场景先把层数降到 16 以控制 2048x2048 A-buffer 内存，但这里自适应又可把层数升回 64：CPU 端 Fragment(12B) 需 2048²×64×12≈3.2 GB，GPU 端 2 GB 片元缓冲；且估计基于全场平均深度，局部密集冠层仍可能溢出，溢出时 CPU/GPU 都直接抛错而无自动扩层重试；建议：按可用显存/内存给出层数上限，并在溢出时自动翻倍层数重建（类似边哈希表的重试）。
    scene.visibilityRasterLayers = adaptiveVisibilityRasterLayers(scene);
    // Dense vegetation can exceed the default per-pixel depth. Explicitly
    // reserve more storage without changing geometry or radiation parameters.
    if (const char* requested = std::getenv("HISTREAM_FACET_RASTER_LAYERS")) {
        uint32_t layers = 0;
        const char* end = requested + std::strlen(requested);
        const auto parsed = std::from_chars(requested, end, layers);
        if (parsed.ec != std::errc{} || parsed.ptr != end ||
            (layers != 64U && layers != 128U && layers != 256U)) {
            throw std::invalid_argument("HISTREAM_FACET_RASTER_LAYERS must be 64, 128 or 256");
        }
        scene.visibilityRasterLayers = std::max(scene.visibilityRasterLayers, layers);
    }
    if (scene.visibilityRasterLayers != initialRasterLayers) {
        std::cout << "facetrt: automatically increased A-buffer layers from "
                  << initialRasterLayers << " to "
                  << scene.visibilityRasterLayers << " for "
                  << scene.facetCount() << " facets" << std::endl;
    }
    // 步骤 8：统计全部顶点的包围盒，并完成反射率贴图因子的归一化。
    scene.minimum = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max()};
    scene.maximum = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                     std::numeric_limits<float>::lowest()};
    for (const facetvk::Vertex& vertex : scene.vertices) {
        for (size_t axis = 0; axis < 3; ++axis) {
            scene.minimum[axis] = std::min(scene.minimum[axis], vertex.position[axis]);
            scene.maximum[axis] = std::max(scene.maximum[axis], vertex.position[axis]);
        }
    }
    finalizeReflectanceTextureFactors(scene);
    return scene;
}

// 作用：工程中没有任何 OBJ 时，按 shapes（"长,宽,高"）在临时目录写出一个长方体 OBJ 并返回路径，
// 文件名由输入路径哈希决定；尺寸非法时为 1 m 立方体。面的绕向与 cubeGeometry 一致。
std::string generatedCubeFromInput(const std::string& inputPath, const std::string& xml)
{
    float length = 1.0f;
    float width = 1.0f;
    float height = 1.0f;
    const std::string dimensions = firstXmlTag(xml, "shapes");
    std::istringstream values(dimensions);
    char separator = 0;
    values >> length >> separator >> width >> separator >> height;
    if (!(length > 0.0f) || !(width > 0.0f) || !(height > 0.0f)) {
        length = width = height = 1.0f;
    }

    const size_t hash = std::hash<std::string>{}(inputPath);
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("histream_facetrt_default_" + std::to_string(hash) + ".obj");
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("project.json contains no OBJ fileName and fallback OBJ could not be created");
    }
    const float hx = length * 0.5f;
    const float hz = width * 0.5f;
    output << "# Generated from project.json cube dimensions\n"
           << "v " << -hx << " 0 " << -hz << "\n"
           << "v " << hx << " 0 " << -hz << "\n"
           << "v " << hx << " 0 " << hz << "\n"
           << "v " << -hx << " 0 " << hz << "\n"
           << "v " << -hx << " " << height << " " << -hz << "\n"
           << "v " << hx << " " << height << " " << -hz << "\n"
           << "v " << hx << " " << height << " " << hz << "\n"
           << "v " << -hx << " " << height << " " << hz << "\n"
           << "f 1 3 2\n f 1 4 3\n"
           << "f 5 6 7\n f 5 7 8\n"
           << "f 1 2 6\n f 1 6 5\n"
           << "f 2 3 7\n f 2 7 6\n"
           << "f 3 4 8\n f 3 8 7\n"
           << "f 4 1 5\n f 4 5 8\n";
    return path.string();
}

// 作用：由输入文件得到场景 OBJ 路径：输入本身是 .obj 则直接用；
// 否则取工程 XML 的 fileName/objectfile（相对路径按输入所在目录解析），都没有时生成默认长方体。
std::string sceneFileFromInput(const std::string& inputPath)
{
    const std::filesystem::path input(inputPath);
    if (input.extension() == ".obj" || input.extension() == ".OBJ") {
        return input.string();
    }
    const std::string xml = readTextFile(inputPath);
    std::string scene = firstXmlTag(xml, "fileName");
    if (scene.empty()) {
        scene = firstXmlTag(xml, "objectfile");
    }
    if (scene.empty()) {
        std::cerr << "facetrt: project.json has no OBJ; using the first cube shape as a generated scene\n";
        return generatedCubeFromInput(inputPath, xml);
    }
    const std::filesystem::path scenePath(scene);
    return scenePath.is_absolute() ? scenePath.string() :
        (input.parent_path() / scenePath).lexically_normal().string();
}

// 作用：确定结果文件路径：显式指定的输出优先，否则取工程的 outDir（缺省为输入所在目录）加 defaultName。
std::string outputFileFromInput(const std::string& inputPath,
                                const std::string& requestedOutput,
                                const char* defaultName)
{
    if (!requestedOutput.empty()) {
        return requestedOutput;
    }
    const std::filesystem::path input(inputPath);
    const std::string xml = readTextFile(inputPath);
    std::string output = firstXmlTag(xml, "outDir");
    if (output.empty()) {
        output = input.parent_path().string();
    }
    return (std::filesystem::path(output) / defaultName).lexically_normal().string();
}

// 作用：选择 FacetRT 后端：环境变量 HISTREAM_FACET_BACKEND 优先，其次 facetBackend 标签；
// 取值 cpu / auto，其他（含缺省）均为 gpu。auto = 先试 GPU，失败回退 CPU 光栅。
std::string facetBackendFromInput(const std::string& xml)
{
    const char* environment = std::getenv("HISTREAM_FACET_BACKEND");
    std::string backend = environment && *environment
        ? trim(environment) : trim(firstXmlTag(xml, "facetBackend"));
    std::transform(backend.begin(), backend.end(), backend.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return backend == "cpu" || backend == "auto" ? backend : "gpu";
}

} // namespace

// 模块：FacetRT（面元辐射）入口
// 输入：project.json（或单个 .obj）、着色器目录、输出文件（可为空）；输出：facetrt.json，
// 并向 stdout 输出 PROGRESS / RESULT 行供上层解析；返回 0 成功，1 失败（错误写 stderr）。
// 流程：载入场景 → 读取辐射参数与光照 → 按后端运行（GPU / CPU / auto 回退）→ 写出结果。
int runFacetRTCore(const std::string& inputPath,
               const std::string& shaderDirectory,
               const std::string& outputFile)
try {
    if (inputPath.empty()) {
        throw std::invalid_argument("FacetRT requires project.json");
    }
    const std::string resultFile = outputFileFromInput(inputPath, outputFile, "facetrt.json");
    const std::filesystem::path input(inputPath);
    Scene scene = input.extension() == ".obj" || input.extension() == ".OBJ"
        ? loadTreeWithSoil(inputPath)
        : loadProjectScene(inputPath);
    const std::string xml =
        input.extension() == ".obj" || input.extension() == ".OBJ"
        ? std::string{} : readTextFile(inputPath);
    FacetRTParameters parameters =
        xml.empty() ? FacetRTParameters{} : facetRTParametersFromInput(xml, input);
    // Same light source as FacetEB (Esun/Esky shapes, directScatteringRatio).
    configureFacetRTLight(xml, input, scene, parameters);
    const Progress progress = [](int value, const std::string& stage) {
        std::cout << "PROGRESS\t" << value << '\t' << stage << std::endl;
    };
    // 步骤：选择后端并运行；CPU 端遇到片元链表溢出时加深栅格层数并整体重跑。
    const std::string requestedBackend = facetBackendFromInput(xml);
    // The retained CPU rasterizer cannot resume after a full fragment list, so
    // repeat the whole run with a deeper A-buffer instead of failing.
    const auto runCpuWithLayerRetry = [&]() {
        for (;;) {
            try {
                return runCpu(scene, progress);
            } catch (const std::exception& error) {
                if (!isCpuRasterOverflow(error) || !growCpuRasterLayers(scene)) throw;
            }
        }
    };
    RunResult result;
    if (requestedBackend == "cpu") {
        std::cout << "facetrt: using retained CPU raster backend, " << cpuThreadCount() << " threads" << std::endl;
        result = runCpuWithLayerRetry();
    } else if (requestedBackend == "auto") {
        try {
            result = runGpu(scene, shaderDirectory, progress,
                            xml.empty() ? nullptr : &parameters);
        } catch (const std::exception& gpuError) {
            std::cerr << "facetrt: GPU backend unavailable (" << gpuError.what()
                      << "); falling back to retained CPU raster backend\n";
            result = runCpuWithLayerRetry();
        }
    } else {
        result = runGpu(scene, shaderDirectory, progress,
                        xml.empty() ? nullptr : &parameters);
    }
    std::filesystem::create_directories(std::filesystem::path(resultFile).parent_path());
    writeWebResult(resultFile, scene, result);
    std::cout << "RESULT\t" << resultFile << std::endl;
    return 0;
} catch (const std::exception& error) {
    std::cerr << "facetrt: " << error.what() << '\n';
    return 1;
}


// 模块：FacetEB（面元能量平衡）入口
// 输入：project.json（或单个 .obj）、着色器目录、输出文件；输出：faceteb.json 与过程文件，
// faceteb_run.json 记录运行状态、收敛判据与已完成产物。返回 0 成功，1 失败。
// 主要步骤：
//   1. 载入场景并读取时间节点、迭代与收敛控制参数
//   2. 读取辐射参数、短波光谱与气象驱动文件
//   其后的可见性图、耦合迭代与结果输出各步骤见函数内部注释。
int runFacetEBCore(const std::string& inputPath,
                   const std::string& shaderDirectory,
                   const std::string& outputFile)
try {
    if (inputPath.empty()) {
        throw std::invalid_argument("FacetEB requires project.json");
    }

    // MeteoStep：一行气象驱动。单位：气温 °C、水汽压 hPa（待核对）、气压 hPa（待核对）、
    // 风速 m s-1、短波/长波 W m-2；缺省值为文件缺列时的占位。
    struct MeteoStep {
        int node{};
        float julianTime{};
        float airTemperature{25.0f};
        float vaporPressure{15.0f};
        float pressure{839.0f};
        float windSpeed{1.0f};
        float shortwave{};
        float longwave{320.0f};
        // Optional columns 8-9: horizontal direct/diffuse W m-2 (NaN = absent).
        float measuredDirect{std::numeric_limits<float>::quiet_NaN()};
        float measuredDiffuse{std::numeric_limits<float>::quiet_NaN()};
    };

    const std::filesystem::path input(inputPath);
    const std::string xml = readTextFile(inputPath);
    Scene scene = input.extension() == ".obj" || input.extension() == ".OBJ"
        ? loadTreeWithSoil(inputPath)
        : loadProjectScene(inputPath);
    const std::string resultFile =
        outputFileFromInput(inputPath, outputFile, "faceteb.json");
    const std::filesystem::path outputDirectory =
        std::filesystem::path(resultFile).parent_path();
    std::filesystem::create_directories(outputDirectory);

    // 步骤 1：时间节点控制。节点区间 [startTimeNode, endTimeNode)，至少含 1 个节点；
    // dTime 为节点步长（s，至少 1，缺省 1800）；nodeTimeStepsSeconds 给出变步长时，个数须等于 endNode。
    const int startNode = std::max(0, static_cast<int>(
        std::lround(xmlFloat(xml, "startTimeNode", 0.0f))));
    const int endNode = std::max(startNode + 1, static_cast<int>(
        std::lround(xmlFloat(xml, "endTimeNode", static_cast<float>(startNode + 1)))));
    const float dTime = std::max(1.0f, xmlFloat(xml, "dTime", 1800.0f));
    std::vector<float> nodeTimeSteps;
    const auto variableSteps=firstXmlTag(xml,"nodeTimeStepsSeconds");
    if(!variableSteps.empty()) {
        std::istringstream values(variableSteps);double step;
        while(values>>step) {
            if(!std::isfinite(step) || step<1 || step>86400)throw std::runtime_error("Invalid native node time step");
            nodeTimeSteps.push_back(float(step));
        }
        if(!values.eof() || nodeTimeSteps.size()!=size_t(endNode))throw std::runtime_error("Native node time-step count mismatch");
    }
    // 过程输出开关：isRadiationProcess / isEnergyProcess；仅给旧开关 isProcess 时按能量过程处理（兼容旧工程）。
    bool saveRadiationProcess = trim(firstXmlTag(xml, "isRadiationProcess")) == "1";
    bool saveEnergyProcess = trim(firstXmlTag(xml, "isEnergyProcess")) == "1";
    if (!saveRadiationProcess && !saveEnergyProcess &&
        trim(firstXmlTag(xml, "isProcess")) == "1") {
        saveEnergyProcess = true;
    }
    const bool saveProcess = saveRadiationProcess || saveEnergyProcess;
    // 耦合迭代控制：stabilityCorrection（稳定度修正开关）、couplingIterations（1–100，缺省 60）、
    // temperatureTolerance（K，缺省 0.05）、temperatureRelaxation（松弛系数 0.05–1，缺省 0.8）。
    const bool stabilityCorrection = xmlFloat(xml, "stabilityCorrection", 0.0f) > 0.5f;
    const uint32_t maximumCouplingIterations = static_cast<uint32_t>(std::clamp(
        static_cast<int>(std::lround(xmlFloat(xml, "couplingIterations", 60.0f))),
        1, 100));
    const float temperatureTolerance = std::clamp(
        xmlFloat(xml, "temperatureTolerance", 0.05f), 0.001f, 10.0f);
    const float temperatureRelaxation = std::clamp(
        xmlFloat(xml, "temperatureRelaxation", 0.8f), 0.05f, 1.0f);
    // 运行索引文件：先以 status=running 写出，记录请求区间与收敛判据，便于中途失败后检查；
    // 判据为“能量残差 ≥ 2 W m-2 的面元占比 < 0.001 且残差折合温度不超过 0.5 K”。
    FacetRunIndex runIndex{outputDirectory / "faceteb_run.json", {
        {"kind", "faceteb-run"}, {"schemaVersion", 1}, {"status", "running"},
        {"startedAt", utcTimestamp()}, {"input", inputPath},
        {"requested", {{"startNode", startNode}, {"endNodeExclusive", endNode}}},
        {"convergence", {{"temperatureTolerance_K", temperatureTolerance},
            // The setting is kept for compatibility; convergence is decided by
            // the energy-residual criterion below only.
            {"temperatureToleranceRole", "recorded only; not part of the convergence criterion"},
            {"iterationBudget", maximumCouplingIterations},
            {"stomatalSearchMaxIterations", facetphysics::leafStomatalMaxIterations},
            {"stomatalInnerLogTolerance", facetphysics::leafStomatalLogTolerance},
            {"thermalSolveMaxIterations", 32},
            {"thermalInnerTolerance_K", facetphysics::leafThermalToleranceK},
            {"stomatalMismatchDiagnosticOnly", true},
            {"energyResidualThreshold_W_m2", facetphysics::facetEnergyResidualThreshold},
            {"maximumFailingFacetFractionExclusive", 0.001},
            {"maximumTemperatureEquivalentResidual_K", facetphysics::facetEnergyMaxEquivalentK},
            {"facetCounting", "one facet; maximum absolute residual over both solved states and independent sides"},
            {"criterion", "count(facets with abs(energy residual) >= 2 W/m2) / total facets < 0.001 AND max over facet sides of abs(residual)/(dR/dT) <= 0.5 K"}}},
        {"completed", ProjectJson::Json::array()}, {"products", ProjectJson::Json::array()}
    }};
    runIndex.write();
    // 步骤 2：读取模型选项与地理/天空参数：光谱加速宽度（1–1000，缺省 100）、植被温度方法（0/1）、
    // 土壤惯性历史、经纬度（°）、年份、时区（h）、天空温度（K，150–350，缺省 250）。
    const size_t spectralAccelerationWidth = static_cast<size_t>(std::clamp(
        static_cast<int>(std::lround(
            xmlFloat(xml, "spectralAccelerationWidth", 100.0f))),
        1, 1000));
    const int vegetationTemperatureMethod = std::clamp(
        static_cast<int>(std::lround(xmlFloat(
            xml, "vegetationTemperatureMethod", 0.0f))), 0, 1);
    // Soil method 1: 1 adds the history older than the 8 stored nodes as
    // exponential modes (control.soilInertiaHistory = "full"); 0 legacy.
    const bool soilFullHistory = xmlFloat(xml, "soilInertiaHistory", 0.0f) > 0.5f;
    const float latitude = std::clamp(xmlFloat(xml, "Latitude", 40.0f), -90.0f, 90.0f);
    const float longitude = std::clamp(xmlFloat(xml, "Longitude", 116.0f), -180.0f, 180.0f);
    const int calendarYear = std::clamp(static_cast<int>(std::lround(xmlFloat(xml, "Year", 2019.0f))), -2000, 6000);
    const double timeZoneHours = std::clamp(xmlFloat(xml, "TimeZone", 8.0f), -18.0f, 18.0f);
    const float skyTemperature = std::clamp(
        xmlFloat(xml, "skyTemperature", 250.0f), 150.0f, 350.0f);
    const float directShortwaveFraction = std::clamp(
        xmlFloat(xml, "directScatteringRatio", 0.8f), 0.0f, 1.0f);
    // 读取辐射参数与短波光谱；光谱直射/散射比例由 directScatteringRatio 给定（缺省 0.8）。
    const FacetRTParameters radiativeParameters =
        facetRTParametersFromInput(xml, input);
    const ShortwaveSpectrum configuredShortwaveSpectrum = loadShortwaveSpectrum(
        xml, input, directShortwaveFraction);
    // light.diffuseFractionMode: fixed keeps the spectrum above for every
    // node; erbs rescales fraction-1 shapes per node (shortwave_split.h).
    const int diffuseFractionMode = shortwave_split::parseMode(
        trim(firstXmlTag(xml, "diffuseFractionMode")));
    ShortwaveSpectrum unitDirectSpectrum, unitDiffuseSpectrum;
    if (diffuseFractionMode == shortwave_split::kModeErbs) {
        unitDirectSpectrum = loadShortwaveSpectrum(xml, input, 1.0f);
        unitDiffuseSpectrum = loadShortwaveSpectrum(xml, input, 0.0f);
    }

    // 气象驱动：文件 filePath 首行为表头，之后每行 "儒略时 气温 水汽压 气压 风速 短波 长波 [直射 散射]"；
    // 非有限值或列不足时抛异常；可选的第 8、9 列为水平面实测直射/散射（W m-2）。
    std::vector<MeteoStep> meteorology;
    const std::string meteoName = trim(firstXmlTag(xml, "filePath"));
    if (!meteoName.empty()) {
        std::ifstream meteo(resolveProjectAsset(input, meteoName));
        if (!meteo) {
            throw std::runtime_error("FacetEB cannot open meteorology: " + meteoName);
        }
        std::string line;
        if (!std::getline(meteo, line)) {
            throw std::runtime_error("FacetEB meteorology has no header or data: " + meteoName);
        }
        int row = 0;
        while (std::getline(meteo, line)) {
            std::istringstream values(line);
            MeteoStep step;
            step.node = row++;
            if (!(values >> step.julianTime >> step.airTemperature >>
                  step.vaporPressure >> step.pressure >> step.windSpeed >>
                  step.shortwave >> step.longwave)) {
                throw std::runtime_error("FacetEB invalid meteorology row at " +
                    meteoName + ":" + std::to_string(row + 1));
            }
            if (!std::isfinite(step.julianTime) ||
                !std::isfinite(step.airTemperature) || !std::isfinite(step.vaporPressure) ||
                !std::isfinite(step.pressure) || !std::isfinite(step.windSpeed) ||
                !std::isfinite(step.shortwave) || !std::isfinite(step.longwave)) {
                throw std::runtime_error("FacetEB non-finite meteorology values at " +
                    meteoName + ":" + std::to_string(row + 1));
            }
            float measuredDirect = 0.0f, measuredDiffuse = 0.0f;
            if (values >> measuredDirect >> measuredDiffuse &&
                std::isfinite(measuredDirect) && std::isfinite(measuredDiffuse) &&
                measuredDirect >= 0.0f && measuredDiffuse >= 0.0f) {
                step.measuredDirect = measuredDirect;
                step.measuredDiffuse = measuredDiffuse;
            }
            meteorology.push_back(step);
        }
        if (meteo.bad()) {
            throw std::runtime_error("FacetEB cannot read meteorology: " + meteoName);
        }
        if (meteorology.empty()) {
            throw std::runtime_error("FacetEB meteorology has no data rows: " + meteoName);
        }
    }
    // 未提供气象文件时：按 dTime 合成时间轴（儒略日从 1 起，每节点 dTime 秒），
    // 其余气象量取 MeteoStep 的缺省值；随后检查时间范围不超过气象行数。
    if (meteoName.empty()) {
        meteorology.resize(static_cast<size_t>(endNode));
        for (int node = 0; node < endNode; ++node) {
            meteorology[static_cast<size_t>(node)].node = node;
            meteorology[static_cast<size_t>(node)].julianTime =
                1.0f + static_cast<float>(node) * dTime / 86400.0f;
        }
    }
    if (endNode > static_cast<int>(meteorology.size())) {
        throw std::runtime_error("FacetEB time range exceeds meteorology rows");
    }

    // 作用：把儒略日（日序 + 日内小数）转为输出文件名用的时间标签 DOY<日序>_<时>-<分>，
    // 按分钟四舍五入并处理进位到下一日。
    const auto timeToken = [](float julianTime) {
        int day = static_cast<int>(std::floor(julianTime));
        int minutes = static_cast<int>(std::lround(
            (static_cast<double>(julianTime) - day) * 1440.0));
        if (minutes >= 1440) {
            day += minutes / 1440;
            minutes %= 1440;
        }
        std::ostringstream token;
        token << "DOY" << day << '_' << std::setw(2) << std::setfill('0')
              << minutes / 60 << '-' << std::setw(2) << std::setfill('0')
              << minutes % 60;
        return token.str();
    };

    // 作用：用 NREL SPA 算法按该节点时刻、经纬度、时区、气压和气温计算太阳天顶角/方位角（度），
    // 写入 scene.sunZenith / scene.sunAzimuth。
    // 说明：秒数取最近整秒并封顶 86399，与 VoxelEB 一致，避免日期进位；海拔固定 100 m（SPA 输入，
    // 其取值依据待核对）；气压下限 100。
    const auto updateSolarPosition = [&](const MeteoStep& step) {
        int month = 1;
        int day = 1;
        const int dayOfYear = static_cast<int>(std::floor(step.julianTime));
        Utils::calculateMonthAndDay(calendarYear, dayOfYear, &month, &day);
        // Same rounding as VoxelEB (geometry.cpp): nearest second, capped
        // at the last second of the day so the date never rolls over.
        const long long secondOfDay = std::clamp(
            std::llround((static_cast<double>(step.julianTime) - dayOfYear) * 86400.0),
            0LL, 86399LL);
        spa_data data{};
        data.year = calendarYear;
        data.month = month;
        data.day = day;
        data.hour = static_cast<int>(secondOfDay / 3600);
        data.minute = static_cast<int>((secondOfDay % 3600) / 60);
        data.second = static_cast<int>(secondOfDay % 60);
        data.timezone = timeZoneHours;
        data.delta_t = Utils::calculateDeltaT(data.year, data.month);
        data.longitude = longitude;
        data.latitude = latitude;
        data.elevation = 100.0;
        data.pressure = std::max(100.0f, step.pressure);
        data.temperature = step.airTemperature;
        data.slope = 0.0;
        data.azm_rotation = 0.0;
        data.atmos_refract = 0.5667;
        data.function = SPA_ZA;
        SPACalc calculator;
        const int error = calculator.spa_calculate(&data);
        if (error != 0) {
            throw std::runtime_error("FacetEB solar-position calculation failed");
        }
        scene.sunZenith = static_cast<float>(data.zenith);
        scene.sunAzimuth = static_cast<float>(data.azimuth);
    };

    // 步骤 3：配置 Vulkan 可见性模型（栅格尺寸、每像素片元层数、周期邻域、GPU 序号 GPU）。
    facetvk::Config config{};
    config.rasterWidth = scene.visibilityRasterSize;
    config.rasterHeight = scene.visibilityRasterSize;
    config.maxFragmentsPerPixel = scene.visibilityRasterLayers;
    config.periodicNeighborCount = scene.periodicNeighborCount;
    config.periodicSizeX = scene.periodicSizeX;
    config.periodicSizeZ = scene.periodicSizeZ;
    config.gpuIndex = static_cast<uint32_t>(std::max(
        0, static_cast<int>(std::lround(xmlFloat(xml, "GPU", 0.0f)))));

    std::cout << "PROGRESS\t3\t加载面元场景与气象驱动" << std::endl;
    std::cout << "PROGRESS\t5\t温度方法：土壤按材质 0/1/2，植被="
              << (vegetationTemperatureMethod == 0
                      ? "Ball-Berry 经验法" : "Farquhar 机制法")
              << std::endl;
    const std::string requestedBackend = facetBackendFromInput(xml);
    // 步骤 4：构建面元之间的可见性图（半球方向离散）：后端为 cpu 直接用 CPU 光栅；
    // 否则先用 GPU，auto 时 GPU 失败回退到 CPU。可见性图在所有时间节点间共用（几何不变）。
    facetvk::FacetrtVulkan model;
    CpuGraph cpuGraph;
    bool useCpuBackend = requestedBackend == "cpu";
    facetvk::GraphDiagnostics graph;
    const auto buildCpuBackend = [&]() {
        for (;;) {
            try {
                std::cout << "PROGRESS\t5\t构建 CPU 面元可见性图（" << cpuThreadCount() << " 线程）" << std::endl;
                cpuGraph = buildCpuGraph(scene, [](int value, const std::string& stage) {
                    std::cout << "PROGRESS\t" << value << '\t' << stage << std::endl;
                });
                graph = cpuGraph.diagnostics;
                useCpuBackend = true;
                return;
            } catch (const std::exception& error) {
                if (!isCpuRasterOverflow(error) || !growCpuRasterLayers(scene)) throw;
            }
        }
    };
    if (useCpuBackend) {
        buildCpuBackend();
    } else {
        try {
            model.initialize(config, shaderDirectory);
            model.setGeometry(scene.vertices);
            model.setGroundFacets(scene.leafFacetCount);
            std::cout << "PROGRESS\t10\t构建共享面元可见性图" << std::endl;
            graph = model.buildVisibilityGraph(makeHemisphereDirections());
        } catch (const std::exception& gpuError) {
            if (requestedBackend != "auto") throw;
            std::cerr << "faceteb: GPU backend unavailable (" << gpuError.what()
                      << "); falling back to retained CPU raster backend\n";
            buildCpuBackend();
        }
    }
    std::cout << "PROGRESS\t10\t" << groundEscapeLog(graph) << std::endl;
    // 以下几个 lambda 统一封装 CPU / GPU 两种后端：solveRadiation 迭代求解面元间多次散射
    // （返回各面元两侧的辐射量），incidentRadiation 由解求入射辐照，computeSunlit 求受晒面积比例
    // （CPU 栅格溢出时加深层数重试）。
    const auto solveRadiation = [&](const std::vector<facetvk::SurfaceOptics>& optics,
                                    float skyRadiosity,
                                    uint32_t iterations,
                                    float relaxation) {
        return useCpuBackend
            ? solveCpu(cpuGraph, optics, [](int, const std::string&) {},
                       skyRadiosity, iterations, relaxation)
            : model.solve(optics, skyRadiosity, iterations, relaxation);
    };
    // Shortwave energy windows: visible windows (r+t ~ 0.1-0.2) converge in a
    // handful of Jacobi sweeps, NIR windows need many more. On the GPU stop
    // once the last sweep changes no side by more than 1e-5 of the window
    // irradiance (error <= delta * w / (1 - w), i.e. <= 1e-4 relative for
    // w = 0.9), checked every 8 sweeps, instead of always running 64.
    const auto solveShortwaveWindow = [&](const std::vector<facetvk::SurfaceOptics>& optics,
                                          float skyRadiosity,
                                          float referenceIrradiance) {
        if (useCpuBackend)
            return solveRadiation(optics, skyRadiosity, kIterations, 1.0f);
        const float tolerance = std::max(1.0e-6f, 1.0e-5f * referenceIrradiance);
        return model.solveAccelerated(optics, skyRadiosity, nullptr,
                                      kIterations, 8U, tolerance, 1.0f);
    };
    const auto incidentRadiation = [&](const facetvk::SolveResult& solution,
                                       const std::vector<facetvk::SurfaceOptics>& optics,
                                       float skyRadiosity) {
        return useCpuBackend
            ? incidentIrradianceCpu(cpuGraph, solution, optics, skyRadiosity)
            : model.incidentIrradiance(solution, optics, skyRadiosity);
    };
    const auto computeSunlit = [&]() {
        for (;;) {
            try {
                std::vector<float> sunlit = useCpuBackend
                    ? computeCpuSunlit(scene)
                    : model.computeSunlitFraction(sunDirection(scene));
                fillUnsampledSunlit(scene, sunlit);
                return sunlit;
            } catch (const std::exception& error) {
                // Only the retained CPU rasterizer throws here; the GPU grows
                // its own A-buffer and rebuilds the visibility graph.
                if (!useCpuBackend || !isCpuRasterOverflow(error) ||
                    !growCpuRasterLayers(scene)) throw;
            }
        }
    };

    // 步骤 5：选择能量平衡后端。HISTREAM_FACET_ENERGY_BACKEND=cpu、含集总热质量，或 auto 且设备
    // 不支持 GPU 能量内核时走 CPU 能量平衡（辐射仍可用 GPU）；否则用 GPU 能量内核。
    // HISTREAM_FACET_VALIDATE_ENERGY=1/2 在 GPU 能量下并行跑 CPU 影子计算做逐项对比（审计/普查）。
    const char* energyBackend = std::getenv("HISTREAM_FACET_ENERGY_BACKEND");
    const bool hasLumpedThermal=std::any_of(scene.materials.begin(),scene.materials.end(),
        [](const SurfaceParameters& s){return !s.lumpedGroup.empty();});
    if(hasLumpedThermal && startNode!=0)throw std::runtime_error("Lumped thermal runs must restore history from node zero");
    const bool cpuEnergyRequested = (energyBackend && std::string(energyBackend) == "cpu") || hasLumpedThermal;
    // "auto" must not fail on a device without shaderFloat64 (or without
    // energy.comp.spv): keep GPU radiation and run the CPU energy path, which
    // HISTREAM_FACET_ENERGY_BACKEND=cpu already uses with GPU radiation.
    const bool autoCpuEnergy = requestedBackend == "auto" && !useCpuBackend &&
        !cpuEnergyRequested && !model.supportsGpuEnergy();
    if (autoCpuEnergy)
        std::cerr << "faceteb: GPU energy kernel unavailable (requires shaderFloat64 and energy.comp.spv); "
                     "auto backend keeps GPU radiation and uses the CPU energy balance\n";
    const bool cpuEnergy = cpuEnergyRequested || autoCpuEnergy;
    const bool gpuEnergy = !useCpuBackend && !cpuEnergy && model.supportsGpuEnergy();
    const char* verifyEnergy = std::getenv("HISTREAM_FACET_VALIDATE_ENERGY");
    // "1": strict audit, stops at the first CPU/GPU mismatch; "2": survey, runs to the
    // end and reports every mismatching (surface class, field) pair in the run index.
    const std::string verifyMode = verifyEnergy ? std::string(verifyEnergy) : std::string();
    const bool shadowEnergy = gpuEnergy && (verifyMode == "1" || verifyMode == "2");
    const bool shadowSurvey = shadowEnergy && verifyMode == "2";
    float shadowFluxMaximum = 0.0f, shadowIncidentMaximum = 0.0f, shadowUpdateMaximum = 0.0f, shadowResistanceMaximum = 0.0f;
    uint64_t shadowEvaluations = 0, shadowUpdates = 0;
    struct ShadowMismatch {
        uint64_t count{0};
        float maxDifference{0.0f}, cpu{0.0f}, gpu{0.0f};
        uint32_t facet{0}, node{0};
        int state{0};
    };
    std::map<std::string, ShadowMismatch> shadowMismatches;
    std::map<std::string, uint64_t> shadowClassFacets; // facets audited per surface class
    if (!useCpuBackend && !cpuEnergy && !gpuEnergy)
        throw std::runtime_error("GPU FacetEB energy requires shaderFloat64 and energy.comp.spv; select the CPU or auto backend");
    runIndex.value["execution"] = {{"radiation", useCpuBackend ? "cpu" : "gpu"},
        {"incidentIrradiance", !useCpuBackend && model.supportsGpuIncident() ? "gpu" : "cpu"},
        {"leafPhysiology", gpuEnergy ? "gpu" : "cpu"},
        {"energyBalance", gpuEnergy ? "gpu" : "cpu"},
        {"temperatureUpdate", gpuEnergy ? "gpu" : "cpu"},
        {"iterativeStateDeviceResident", gpuEnergy},
        {"soilColumnPreparation", "cpu once per node"}, {"stabilityAggregation", "cpu when enabled"}};
    runIndex.write();
    std::cout << "PROGRESS\t10\t" << (gpuEnergy ? "GPU 能量平衡/叶片生理，迭代状态驻留显存" : "CPU 能量平衡对照路径") << std::endl;

    // 步骤 6：状态数组初始化。每个面元有两侧（正反面），共 facetCount×2 个表面；
    // 温度初值为 NaN，表示尚未初始化（首节点由气温或配置的土壤温度给定）。
    const size_t surfaceCount = static_cast<size_t>(scene.facetCount()) * 2U;
    std::vector<float> temperature(
        surfaceCount, std::numeric_limits<float>::quiet_NaN());
    // As in FacetRT, the sunlit (0) and shaded (1) parts of every side have
    // their own temperature; `temperature` is their area-weighted mean.
    std::array<std::vector<float>, 2> stateTemperature{temperature, temperature};

    // Ground columns carry the local canopy structure (leaf area, canopy top,
    // leaf width) that sets the leaf and under-canopy soil resistances.
    struct CanopyColumn {
        double leafArea{}, leafWidthArea{};
        float top{-std::numeric_limits<float>::infinity()};
        float ground{std::numeric_limits<float>::infinity()};
        float lai{}, height{}, leafWidth{0.1f};
    };
    // 参考高度 referenceHeight（m，下限 0.5，缺省 10）与大气 CO2 浓度 carbonDioxide（限幅 100–2000，缺省 380，
    // 单位按 ppm，待核对）；initialSoilTemperature 仅在 150–400 K 内才采用，否则取首个气象气温。
    const float referenceHeight = std::max(0.5f, xmlFloat(xml, "referenceHeight", 10.0f));
    const float carbonDioxide = std::clamp(xmlFloat(xml, "carbonDioxide", 380.0f), 100.0f, 2000.0f);
    // Configured initial soil temperature Tsold [K]; ground surfaces start there.
    const float configuredSoilTemperature = xmlFloat(xml, "initialSoilTemperature", -1.0f);
    const bool hasInitialSoilTemperature =
        configuredSoilTemperature >= 150.0f && configuredSoilTemperature <= 400.0f;
    runIndex.value["initialSoilTemperature"] = hasInitialSoilTemperature
        ? ProjectJson::Json{{"source", "configured"}, {"value_K", configuredSoilTemperature}}
        : ProjectJson::Json{{"source", "first meteorological air temperature"}};
    const float columnExtentX = std::max(1.0e-3f, scene.maximum[0] - scene.minimum[0]);
    const float columnExtentZ = std::max(1.0e-3f, scene.maximum[2] - scene.minimum[2]);
    const float columnSize = std::clamp(
        std::max(columnExtentX, columnExtentZ) / 256.0f, 1.0f, std::max(columnExtentX, columnExtentZ));
    // 冠层柱网格：柱边长 = 场景最大水平尺寸/256，不小于 1 m、不大于该尺寸，故每边最多约 256 柱。
    const int columnsX = std::max(1, static_cast<int>(std::ceil(columnExtentX / columnSize)));
    const int columnsZ = std::max(1, static_cast<int>(std::ceil(columnExtentZ / columnSize)));
    std::vector<CanopyColumn> columns(static_cast<size_t>(columnsX) * columnsZ);
    std::vector<uint32_t> facetColumn(scene.facetCount());
    std::vector<float> facetHeight(scene.facetCount());
    // 步骤 7：把每个面元按质心归入水平冠层柱，累积各柱的叶面积、叶宽加权面积、冠层顶和地面高度。
    // 叶面元 = 对象面元中的植被且非光伏；地面面元（编号 ≥ leafFacetCount）用于取柱内最低地面高度。
    const auto isLeafFacet = [&](uint32_t facet) {
        const SurfaceParameters& surface = surfaceForFacet(scene, facet);
        return facet < scene.leafFacetCount && surface.vegetation && !surface.photovoltaic;
    };
    for (uint32_t facet = 0; facet < scene.facetCount(); ++facet) {
        Vec3 centroid{};
        float top = -std::numeric_limits<float>::infinity();
        for (uint32_t corner = 0; corner < 3U; ++corner) {
            const auto& position = scene.vertices[facet * 3U + corner].position;
            for (int axis = 0; axis < 3; ++axis) centroid[axis] += position[axis] / 3.0f;
            top = std::max(top, position[1]);
        }
        const int cx = std::clamp(static_cast<int>((centroid[0] - scene.minimum[0]) / columnSize), 0, columnsX - 1);
        const int cz = std::clamp(static_cast<int>((centroid[2] - scene.minimum[2]) / columnSize), 0, columnsZ - 1);
        const uint32_t index = static_cast<uint32_t>(cz * columnsX + cx);
        facetColumn[facet] = index;
        facetHeight[facet] = centroid[1];
        CanopyColumn& column = columns[index];
        if (isLeafFacet(facet)) {
            const double area = facetArea(scene, facet);
            column.leafArea += area;
            column.leafWidthArea += area * surfaceForFacet(scene, facet).leafWidth;
            column.top = std::max(column.top, top);
        } else if (facet >= scene.leafFacetCount) {
            column.ground = std::min(column.ground, centroid[1]);
        }
    }
    // 由累积量得到每柱 LAI（叶面积/柱底面积）、冠层高度（顶 - 地面）与面积加权叶宽；
    // 没有地面面元的柱以场景最低点作地面。
    for (CanopyColumn& column : columns) {
        if (!std::isfinite(column.ground)) column.ground = scene.minimum[1];
        if (column.leafArea <= 0.0) continue;
        column.lai = static_cast<float>(column.leafArea / (columnSize * columnSize));
        column.height = std::max(0.0f, column.top - column.ground);
        column.leafWidth = static_cast<float>(column.leafWidthArea / column.leafArea);
    }
    std::vector<CanopyAerodynamics> columnAerodynamics(columns.size());
    std::vector<double> columnInverseL(columns.size(), 0.0);
    std::vector<double> facetAreas(scene.facetCount());
    for (uint32_t f = 0; f < scene.facetCount(); ++f) facetAreas[f] = facetArea(scene, f);
    // 集总热质量体：同一 lumpedGroup 的所有面元共享一个内部节点温度（单一容量与导热系数），
    // weightedArea 为按 lumpedWeight 加权的面积，用于把总导热 W K-1 分摊到各面元。
    // 同组各面元参数必须完全一致，否则抛异常；集总面元必须是显式固体对象实例，不能是地面。
    struct LumpedBody {
        std::string id;facetphysics::LumpedThermalParameters p;
        double weightedArea{},temperatureK{};facetphysics::LumpedThermalStep step;
    };
    std::vector<LumpedBody> lumpedBodies;
    std::unordered_map<std::string,size_t> lumpedIds;
    std::vector<int> lumpedFacet(scene.facetCount(),-1);
    std::vector<double> lumpedConductanceDensity(scene.facetCount(),0);
    for(uint32_t f=0;f<scene.facetCount();++f) {
        const auto& s=surfaceForFacet(scene,f);
        if(s.lumpedGroup.empty())continue;
        if(f>=scene.leafFacetCount)throw std::runtime_error("Lumped thermal surface must be an explicit solid instance");
        auto found=lumpedIds.find(s.lumpedGroup);
        if(found==lumpedIds.end()) {
            found=lumpedIds.emplace(s.lumpedGroup,lumpedBodies.size()).first;
            lumpedBodies.push_back({s.lumpedGroup,s.lumped,0,s.lumped.initialK,{}});
        }
        const size_t index=found->second;auto& body=lumpedBodies[index];
        const auto& a=body.p;const auto& b=s.lumped;
        if(a.capacityJPerK!=b.capacityJPerK || a.conductanceWPerK!=b.conductanceWPerK ||
           a.airConductanceWPerK!=b.airConductanceWPerK || a.powerW!=b.powerW ||
           a.startSeconds!=b.startSeconds || a.stopSeconds!=b.stopSeconds || a.initialK!=b.initialK)
            throw std::runtime_error("Inconsistent parameters in lumped group: "+body.id);
        lumpedFacet[f]=int(index);body.weightedArea+=facetAreas[f]*s.lumpedWeight;
    }
    runIndex.value["lumpedThermalBodies"]=ProjectJson::Json::array();
    for(const auto& b:lumpedBodies) {
        if(!(b.weightedArea>0))throw std::runtime_error("Lumped group has no conductive area: "+b.id);
        runIndex.value["lumpedThermalBodies"].push_back({{"id",b.id},{"capacity_J_K",b.p.capacityJPerK},
            {"conductance_W_K",b.p.conductanceWPerK},{"weightedArea_m2",b.weightedArea}});
    }
    for(uint32_t f=0;f<scene.facetCount();++f)if(lumpedFacet[f]>=0) {
        const auto& b=lumpedBodies[size_t(lumpedFacet[f])];
        lumpedConductanceDensity[f]=b.p.conductanceWPerK*surfaceForFacet(scene,f).lumpedWeight/b.weightedArea;
    }
    if(hasLumpedThermal)runIndex.value["execution"]["lumpedThermal"]="CPU implicit internal nodes + surface EB; GPU coupled radiation";
    // Sunlit/shaded thermal history across nodes. "remap" (default) moves the
    // area whose state changed together with its stored heat; "legacy" keeps
    // each state's own history (the old behaviour, heat not conserved when the
    // sunlit part changes); "mixed" collapses both states to one area-weighted
    // history at every node (diagnostic reference, loses the sunlit/shaded
    // contrast of the stored heat).
    const char* thermalHistoryEnvironment = std::getenv("HISTREAM_THERMAL_HISTORY");
    const std::string thermalHistoryMode = thermalHistoryEnvironment && *thermalHistoryEnvironment
        ? std::string(thermalHistoryEnvironment) : std::string("remap");
    if (thermalHistoryMode != "remap" && thermalHistoryMode != "legacy" && thermalHistoryMode != "mixed")
        throw std::runtime_error("HISTREAM_THERMAL_HISTORY must be remap, legacy or mixed");
    runIndex.value["thermalHistory"] = thermalHistoryMode;
    // Building wall/roof slabs (material wallMode "wall"). The indoor side of a
    // facet is the one that sees no sky: per-side sky view factors from one
    // black-scene solve (sky radiosity 1, every side black and non-emitting),
    // independent of the OBJ winding. A side with view >= kIndoorSkyView is
    // outdoors; a facet whose both sides see the sky (open structure) keeps the
    // thin shell. Ties (both enclosed) take the geometric-normal side as outer.
    constexpr float kIndoorSkyView = 0.02f;
    std::vector<int32_t> wallIndex(scene.leafFacetCount, -1);
    std::vector<uint32_t> wallFacets;
    std::vector<uint8_t> wallOuter;
    {
        bool anyWall = false;
        for (uint32_t f = 0; f < scene.leafFacetCount && !anyWall; ++f) anyWall = surfaceForFacet(scene, f).wall;
        if (anyWall) {
            const std::vector<facetvk::SurfaceOptics> black(surfaceCount);
            const auto skyView = incidentRadiation(solveRadiation(black, 1.0f, 1U, 1.0f), black, 1.0f);
            uint32_t shellFallback = 0;
            float maximumIndoorView = 0.0f;
            double wallArea = 0;
            for (uint32_t f = 0; f < scene.leafFacetCount; ++f) {
                if (!surfaceForFacet(scene, f).wall) continue;
                const float front = skyView[size_t(f) * 2U], back = skyView[size_t(f) * 2U + 1U];
                if (std::min(front, back) >= kIndoorSkyView) { ++shellFallback; continue; }
                wallIndex[f] = static_cast<int32_t>(wallFacets.size());
                wallFacets.push_back(f);
                wallOuter.push_back(back > front ? 1U : 0U);
                maximumIndoorView = std::max(maximumIndoorView, std::min(front, back));
                wallArea += facetAreas[f];
            }
            runIndex.value["buildingWalls"] = {{"facets", wallFacets.size()}, {"area_m2", wallArea},
                {"shellFallbackFacets", shellFallback}, {"indoorSkyViewThreshold", kIndoorSkyView},
                {"maximumIndoorSideSkyView", maximumIndoorView}, {"layers", facetphysics::wallCellsPerLayer}};
        }
    }
    // Ship hulls: facets whose centre lies below the material waterline exchange
    // heat with the water instead of the air on their outer face (wall slab) or
    // on one of their two faces (thin shell); no latent heat either way.
    std::vector<uint8_t> submergedFacet;
    // Wetted fraction of a ship-hull facet (1 for a fixed waterline facet below it).
    std::vector<float> wetFraction;
    bool anyShipSpeed = false, anyWaterFeedback = false, anyFromWater = false;
    // Real draft: the scene water surface is the area-weighted mean height of the
    // water facets; hull facets below it (exact triangle part) are wetted.
    double waterSurfaceHeight = 0, sceneWaterArea = 0;
    {
        double heightSum = 0;
        for (uint32_t f = 0; f < scene.leafFacetCount; ++f) {
            const auto& s = surfaceForFacet(scene, f);
            if (!s.water || s.vegetation) continue;
            heightSum += facetAreas[f] * facetHeight[f];
            sceneWaterArea += facetAreas[f];
        }
        if (sceneWaterArea > 0) waterSurfaceHeight = heightSum / sceneWaterArea;
    }
    {
        // 船体浸没判定：对每个带吃水线（waterline）的面元求湿润比例 wet。
        //   - waterlineSource=water：水位 = 场景水面平均高度 + waterlineOffset，按三角形三个顶点高度
        //     排序后求位于水位以下的面积占比（分段二次式，精确）；
        //   - 固定 waterlineHeight：面元质心低于吃水线则 wet=1，否则 0。
        // wet>0 的面元记入 submergedFacet / wetFraction，并统计浸没面积与湿润面积供运行索引记录。
        uint32_t submergedCount = 0;
        double submergedArea = 0, wettedArea = 0;
        for (uint32_t f = 0; f < scene.leafFacetCount; ++f) {
            const auto& s = surfaceForFacet(scene, f);
            anyShipSpeed = anyShipSpeed || s.shipSpeed > 0;
            anyWaterFeedback = anyWaterFeedback || s.waterFeedback;
            anyFromWater = anyFromWater || s.waterlineFromWater;
            if (!s.waterline) continue;
            float wet = 0.0f;
            if (s.waterlineFromWater) {
                if (!(sceneWaterArea > 0))
                    throw std::runtime_error("Ship waterlineSource water needs a water surface in the scene");
                const float level = static_cast<float>(waterSurfaceHeight) + s.waterlineOffset;
                std::array<float, 3> h{};
                for (uint32_t corner = 0; corner < 3U; ++corner) h[corner] = scene.vertices[f * 3U + corner].position[1];
                std::sort(h.begin(), h.end());
                if (level <= h[0]) wet = 0.0f;
                else if (level >= h[2]) wet = 1.0f;
                else if (level < h[1]) wet = (level - h[0]) * (level - h[0]) / ((h[1] - h[0]) * (h[2] - h[0]));
                else wet = 1.0f - (h[2] - level) * (h[2] - level) / ((h[2] - h[0]) * (h[2] - h[1]));
            } else if (facetHeight[f] < s.waterlineHeight) {
                wet = 1.0f;
            }
            if (!(wet > 0.0f)) continue;
            if (submergedFacet.empty()) {
                submergedFacet.assign(scene.leafFacetCount, 0);
                wetFraction.assign(scene.leafFacetCount, 0.0f);
            }
            submergedFacet[f] = 1;
            wetFraction[f] = wet;
            ++submergedCount;
            submergedArea += facetAreas[f];
            wettedArea += facetAreas[f] * wet;
        }
        if (anyWaterFeedback && !(sceneWaterArea > 0))
            throw std::runtime_error("Ship waterFeedback needs a water surface in the scene");
        if (submergedCount) runIndex.value["shipWaterline"] = {{"facets", submergedCount}, {"area_m2", submergedArea}};
        if (submergedCount && anyFromWater) {
            runIndex.value["shipWaterline"]["wettedArea_m2"] = wettedArea;
            runIndex.value["shipWaterline"]["waterSurfaceHeight_m"] = waterSurfaceHeight;
        }
    }
    // Internal heat sources (Ship material internalHeatFlux W m-2 plus
    // internalHeatPower W spread over the area of the facets of the same scene
    // material), W m-2 of facet area. A wall slab receives it in its innermost
    // cell (wallResponse), a thin shell as a source term of the facet balance.
    std::vector<float> internalHeat;
    {
        std::vector<double> materialArea(scene.materials.size(), 0.0);
        for (uint32_t f = 0; f < scene.leafFacetCount; ++f)
            if (f < scene.materialIndices.size() && scene.materialIndices[f] < materialArea.size())
                materialArea[scene.materialIndices[f]] += facetAreas[f];
        double power = 0, area = 0;
        for (uint32_t f = 0; f < scene.leafFacetCount; ++f) {
            const auto& s = surfaceForFacet(scene, f);
            if (s.internalFlux == 0.0f && s.internalPower == 0.0f) continue;
            if (internalHeat.empty()) internalHeat.assign(scene.leafFacetCount, 0.0f);
            const double perArea = double(s.internalFlux) +
                (s.internalPower != 0.0f ? double(s.internalPower) / materialArea[scene.materialIndices[f]] : 0.0);
            internalHeat[f] = static_cast<float>(perArea);
            power += perArea * facetAreas[f];
            area += facetAreas[f];
        }
        if (!internalHeat.empty()) runIndex.value["internalHeat"] = {{"area_m2", area}, {"power_W", power}};
    }
    // Vehicles (material type Vehicle): one lumped cabin per object instance.
    // Body and glass are wall slabs whose indoor temperature is the cabin air
    // temperature of the instance; the cabin equation is solved on the host
    // after each node (see the node loop), so CPU and GPU energy paths share it.
    struct VehicleRuntime {
        std::string name;
        const SurfaceParameters* body{nullptr}; // body material (cabin parameters)
        std::vector<uint32_t> bodyFacets, glassFacets, underFacets;
        std::vector<size_t> walls;              // wall indices of body + glass facets
        double bodyArea{0}, glassArea{0}, underArea{0};
        double airK{0}, interiorK{0};
        double maxAirK{-1}, maxAirTime{0}, maxInteriorK{-1};
        int maxAirNode{-1};
        double maxClosure{0};
        // Parts model (interior nodes / engine and bay): bonnet facets and node temperatures.
        std::vector<uint32_t> hoodFacets;
        std::vector<size_t> hoodWalls;          // wall indices of the bonnet (indoor side = bay air)
        double hoodArea{0};
        std::array<double, 4> intK{}, maxIntK{-1, -1, -1, -1};
        double boundaryK{0}, blockK{0}, bayK{0}, maxBlockK{-1}, maxBayK{-1}, maxEngineClosure{0};
        bool parts{false};                      // interior nodes and/or engine model of the body material
        std::map<std::string, double> maxWindowW; // peak transmitted power per window group (W)
    };
    // 按场景中登记的车辆实例（facetInstance，从 1 起编号）建立运行时状态，
    // 并在下面按 vehiclePart 归类面元：1 = 车身，2 = 车窗玻璃，3 = 底盘，4 = 发动机盖。
    std::vector<VehicleRuntime> vehicles(scene.vehicleNames.size());
    const auto vehicleOf = [&](uint32_t facet) -> uint32_t {
        return facet < scene.facetInstance.size() ? scene.facetInstance[facet] : 0U;
    };
    std::vector<int32_t> wallVehicle(wallFacets.size(), -1);
    for (size_t i = 0; i < vehicles.size(); ++i) vehicles[i].name = scene.vehicleNames[i];
    // 归类各实例的面元与面积，建立墙板索引到车辆的映射 wallVehicle，
    // 并把发动机热量按所选模型折算为底盘面元的附加内热源 internalHeat（W m-2）。
    if (!vehicles.empty()) {
        for (uint32_t f = 0; f < scene.leafFacetCount; ++f) {
            const uint32_t instance = vehicleOf(f);
            if (!instance) continue;
            auto& v = vehicles[instance - 1U];
            const auto& s = surfaceForFacet(scene, f);
            if (s.vehiclePart == 1) {
                v.bodyFacets.push_back(f);
                v.bodyArea += facetAreas[f];
                if (!v.body && s.vehicleHasParams) v.body = &s;
            } else if (s.vehiclePart == 2) {
                v.glassFacets.push_back(f);
                v.glassArea += facetAreas[f];
            } else if (s.vehiclePart == 3) {
                v.underFacets.push_back(f);
                v.underArea += facetAreas[f];
            } else if (s.vehiclePart == 4) {
                v.hoodFacets.push_back(f);
                v.hoodArea += facetAreas[f];
            }
            if ((s.vehiclePart == 1 || s.vehiclePart == 2) && wallIndex[f] >= 0) wallVehicle[size_t(wallIndex[f])] = int32_t(instance - 1U);
        }
        ProjectJson::Json list = ProjectJson::Json::array();
        for (auto& v : vehicles) {
            v.parts = v.body && v.body->cabinEnabled && (v.body->interiorMulti || v.body->engineMulti);
            if (v.body && v.body->cabinEnabled)
                for (size_t w = 0; w < wallVehicle.size(); ++w)
                    if (wallVehicle[w] == int32_t(&v - vehicles.data())) v.walls.push_back(w);
            // Bonnet slabs: indoor side = engine-bay air with the engine model, else they behave like the body.
            for (uint32_t f : v.hoodFacets) {
                if (wallIndex[f] < 0 || !v.body || !v.body->cabinEnabled) continue;
                if (!v.body->engineMulti) wallVehicle[size_t(wallIndex[f])] = int32_t(&v - vehicles.data());
                (v.body->engineMulti ? v.hoodWalls : v.walls).push_back(size_t(wallIndex[f]));
            }
            // Engine heat: legacy = spread over the underbody shell; engine model = exhaust share only.
            if (v.body && v.body->engineOn && v.underArea > 0) {
                if (internalHeat.empty()) internalHeat.assign(scene.leafFacetCount, 0.0f);
                const double perArea = v.body->enginePowerW * v.body->engineShare * (v.body->engineMulti ? v.body->engineSplit[2] : 1.0) / v.underArea;
                for (uint32_t f : v.underFacets) internalHeat[f] += static_cast<float>(perArea);
            }
            list.push_back({{"name", v.name}, {"cabin", v.body && v.body->cabinEnabled}, {"speed_m_s", v.body ? v.body->vehicleSpeed : 0.0f},
                {"bodyArea_m2", v.bodyArea}, {"glassArea_m2", v.glassArea}, {"underbodyArea_m2", v.underArea},
                {"wallFacets", v.walls.size()}});
            if (v.parts) {
                list.back()["hoodArea_m2"] = v.hoodArea;
                list.back()["interiorModel"] = v.body->interiorMulti ? "nodes" : "lumped";
                list.back()["engineModel"] = v.body->engineMulti;
            }
        }
        runIndex.value["vehicles"] = list;
    }
    // 作用：若面元属于车速 > 0 的车辆实例，返回该车车身材质（供对流换热按行驶风修正），否则返回空。
    const auto facetConvectiveScaleVehicle = [&](uint32_t facet) -> const SurfaceParameters* {
        const uint32_t instance = vehicleOf(facet);
        return instance && vehicles[instance - 1U].body && vehicles[instance - 1U].body->vehicleSpeed > 0.0f
            ? vehicles[instance - 1U].body : nullptr;
    };
    // Indoor temperature of vehicle wall slabs (K), NaN = the material value.
    std::vector<double> wallIndoor(wallFacets.size(), std::numeric_limits<double>::quiet_NaN());
    const auto slabOf = [&](size_t w) {
        facetphysics::WallSlab slab = surfaceForFacet(scene, wallFacets[w]).wallSlab;
        if (std::isfinite(wallIndoor[w])) slab.indoorK = wallIndoor[w];
        return slab;
    };
    runIndex.write();
    // 地面表面（侧）的起始序号：对象面元占前 leafFacetCount×2 个表面，其后为地面；
    // 土壤剖面、土壤历史等数组按 side - firstGroundSide 紧凑存储，各设受晒(0)与阴影(1)两套状态。
    const size_t firstGroundSide = static_cast<size_t>(scene.leafFacetCount) * 2U;
    const size_t groundSides = surfaceCount - firstGroundSide;
    // The soil-facing side of a ground facet (normal pointing down, as in
    // facetvk::applyGroundBoundary) is not an energy-balance surface: it has
    // no fluxes and carries the temperature of the upper side.
    const auto groundDownSide = [&](uint32_t facet) -> uint32_t {
        return scene.vertices[size_t(facet) * 3U].normal[1] >= 0.0f ? 1U : 0U;
    };
    std::array<std::vector<facetphysics::SoilProfile>, 2> soilProfiles{
        std::vector<facetphysics::SoilProfile>(groundSides), std::vector<facetphysics::SoilProfile>(groundSides)};
    std::vector<double> soilBottomTemperature(groundSides);
    // Sunlit/shaded slab profiles of the wall facets (index wallIndex[facet]).
    std::array<std::vector<facetphysics::WallProfile>, 2> wallProfiles{
        std::vector<facetphysics::WallProfile>(wallFacets.size()), std::vector<facetphysics::WallProfile>(wallFacets.size())};
    std::vector<double> previousWallHeat(wallFacets.size(), 0.0);

    // Surface temperatures of the preceding nodes, newest first, for the
    // thermal-inertia soil heat flux (SCOPE/VoxelEB soil method 1).
    constexpr size_t kSoilHistory = 8U;
    std::array<std::vector<float>, 2> soilHistory{
        std::vector<float>(surfaceCount * kSoilHistory, 0.0f),
        std::vector<float>(surfaceCount * kSoilHistory, 0.0f)};
    // Older history as soilMemoryModes (ground sides only, both states; zero
    // for the uniform start history). Allocated only for soilInertiaHistory=full.
    bool soilMemoryUsed = false;
    if (soilFullHistory)
        for (uint32_t facet = scene.leafFacetCount; facet < scene.facetCount() && !soilMemoryUsed; ++facet)
            soilMemoryUsed = surfaceForFacet(scene, facet).soilTemperatureMethod == 1;
    std::array<std::vector<double>, 2> soilMemory;
    if (soilMemoryUsed)
        for (auto& memory : soilMemory) memory.assign(groundSides * kSoilMemoryModes, 0.0);
    if (soilMemoryUsed) runIndex.value["soilInertiaHistory"] = "full";
    const auto soilMemoryChange = [&](int state, size_t side, const SoilMemoryModes& modes) {
        if (!soilMemoryUsed || side < firstGroundSide) return 0.0;
        const double* memory = soilMemory[size_t(state)].data() + (side - firstGroundSide) * kSoilMemoryModes;
        double change = 0.0;
        for (size_t j = 0; j < kSoilMemoryModes; ++j) change += modes.weight[j] * memory[j];
        return change;
    };
    // Sunlit part of every side at the preceding node (the area weights of the
    // stored states) and the stored heat of each object facet / method-2 soil
    // side at the end of that node, for the conservation diagnostics.
    std::vector<float> previousSunlitPart;
    std::vector<double> previousObjectHeat(scene.leafFacetCount, 0.0);
    std::vector<double> previousSoilHeat(groundSides, 0.0);
    const auto soilColumnHeat = [&](const SurfaceParameters& soil, const facetphysics::SoilProfile& profile) {
        double heat = 0;
        for (size_t layer = 0; layer < facetphysics::soilLayers; ++layer)
            heat += soil.specificHeat * soil.density *
                (facetphysics::soilEdges[layer + 1] - facetphysics::soilEdges[layer]) * profile[layer];
        return heat;
    };

    // 最近一个节点的结果汇总（后端名、可见性图诊断等）；下面的 latest* 摘要数组只保留最终状态，
    // 收敛情况跨节点累积到 allNodesConverged。
    RunResult latest;
    latest.backend = useCpuBackend
        ? "CPU raster Facet RT-EB coupled"
        : "Vulkan GPU Facet RT-EB coupled";
    latest.graph = graph;
    // Only the final state needs these summary arrays; do not retain a second
    // copy of the preceding node throughout the next solve.
    std::vector<float> latestNetRadiation;
    std::vector<float> latestSensibleHeat;
    std::vector<float> latestLatentHeat;
    std::vector<float> latestPhotovoltaicPower;
    std::vector<float> latestStorageHeat;
    uint32_t latestCouplingIterations = 0;
    float latestTemperatureDelta = 0.0f;
    float latestEnergyResidual = 0.0f;
    float latestEnergyResidualEquivalent = 0.0f;
    bool allNodesConverged = true;
    std::string latestTime;

    // 逐节点二进制中间结果目录 .facet_steps（与几何和节点记录同序）；
    // 先写 components.bin：每个面元一字节的输出分量编号（地面固定为 1，对象面元取材质 outputComponent）。
    const std::filesystem::path stepDirectory = outputDirectory / ".facet_steps";
    std::filesystem::create_directories(stepDirectory);
    // Shared classification, in exactly the same order as geometry and node records.
    {
        std::ofstream components(stepDirectory / "components.bin", std::ios::binary | std::ios::trunc);
        for (uint32_t facet = 0; facet < scene.facetCount(); ++facet) {
            const uint8_t component = facet >= scene.leafFacetCount ? 1 : surfaceForFacet(scene, facet).outputComponent;
            components.write(reinterpret_cast<const char*>(&component), sizeof(component));
        }
        components.close();
        if (!components) throw std::runtime_error("FacetEB cannot write component identities");
    }
    if (saveProcess) {
        std::filesystem::create_directories(outputDirectory / "process");
    }

    // 光伏汇总 CSV：逐节点记录光伏面元的面积、温度、入射/吸收/发电功率等，并累计发电量（kWh）。
    std::ofstream pvSummary(outputDirectory/"photovoltaic_summary.csv");
    pvSummary << "node,time,area_m2,temperature_C,incident_W,absorbed_W,electric_W,net_longwave_W,sensible_W,storage_W,max_residual_W_m2,energy_kWh\n";
    double pvEnergyKwh=0;
    runIndex.value["products"].push_back("photovoltaic_summary.csv");
    runIndex.value["products"].push_back(".facet_steps/components.bin");
    const char* stepSyncEnvironment = std::getenv("HISTREAM_FACET_STEP_SYNC");
    const bool synchronizeStepOutput = stepSyncEnvironment &&
        std::string(stepSyncEnvironment) == "1";
    if (synchronizeStepOutput) {
        // Geometry is shared by all nodes and is available before the first
        // image/process preview. The final summary replaces this file later.
        std::ofstream geometry(resultFile, std::ios::binary | std::ios::trunc);
        geometry << std::setprecision(9)
                 << "{\n  \"backend\": \"" << latest.backend << "\",\n"
                 << "  \"componentFile\": \".facet_steps/components.bin\",\n"
                 << "  \"facetCount\": " << scene.facetCount() << ",\n"
                 << "  \"leafFacetCount\": " << scene.leafFacetCount << ",\n"
                 << "  \"vertexPositions\": [";
        for (size_t index = 0; index < scene.vertices.size(); ++index) {
            if (index) geometry << ',';
            const auto& vertex = scene.vertices[index];
            geometry << vertex.position[0] << ',' << vertex.position[1]
                     << ',' << vertex.position[2];
        }
        geometry << "],\n  \"wavelengths\": [";
        // Actual engine band list and per-side node-record width
        // (energy_T=*.bin: 11 scalars + lit/shaded per band).
        for (size_t band = 0; band < radiativeParameters.wavelengths.size(); ++band) {
            if (band) geometry << ',';
            geometry << radiativeParameters.wavelengths[band];
        }
        geometry << "],\n  \"stepRecordFloats\": "
                 << (11U + 2U * radiativeParameters.wavelengths.size()) << "\n}\n";
        geometry.close();
        if (!geometry) throw std::runtime_error("FacetEB cannot write shared geometry");
    }
    // Ship waterline contact: the water temperature is the area-mean temperature
    // of the scene water facets at the start of each node; a material
    // waterTemperature overrides it; without scene water the first-node air.
    const auto sceneWaterTemperature = [&]() {
        double sum = 0, area = 0;
        for (uint32_t f = 0; f < scene.leafFacetCount; ++f) {
            const auto& s = surfaceForFacet(scene, f);
            if (!s.water || s.vegetation) continue;
            sum += facetAreas[f] * temperature[size_t(f) * 2U];
            area += facetAreas[f];
        }
        return area > 0 ? static_cast<float>(sum / area) : -1.0f;
    };
    const float shipFirstAirK = meteorology.empty() ? 293.15f
        : std::clamp(meteorology[size_t(startNode)].airTemperature + 273.15f, 220.0f, 340.0f);
    const auto shipWaterK = [&](const SurfaceParameters& s, float sceneWaterK) {
        return s.waterK >= 0 ? s.waterK : sceneWaterK >= 0 ? sceneWaterK : shipFirstAirK;
    };
    // Submerged facets with waterShielding receive no sun or sky radiation on
    // their wetted part: the net radiation is scaled by 1 - wetted fraction.
    const auto shieldFraction = [&](uint32_t facet) {
        if (wetFraction.empty() || facet >= scene.leafFacetCount || !surfaceForFacet(scene, facet).waterShield) return 1.0f;
        return 1.0f - wetFraction[facet];
    };
    // Hull-to-water heat of the preceding node (W m-2 of scene water area) that
    // warms the water facets of the next node (Ship waterFeedback).
    float waterFeedbackSource = 0.0f;
    // 步骤 8：逐时间节点主循环 [startNode, endNode)。每个节点先更新太阳位置并计算受晒比例，
    // 首节点初始化各表面温度，再处理受晒/阴影状态间的热历史转移等（其后部分见下）。
    // 节点步长 dTime 优先取 nodeTimeStepsSeconds 的变步长；集总热质量累计经过的秒数。
    const auto totalBegin = Clock::now();
    double lumpedElapsedSeconds=0;
    for (int node = startNode; node < endNode; ++node) {
        const auto nodeBegin = Clock::now();
        auto shortwaveDone = nodeBegin;
        auto couplingDone = nodeBegin;
        const float dTime=nodeTimeSteps.empty()?std::max(1.0f,xmlFloat(xml,"dTime",1800.0f)):nodeTimeSteps[size_t(node)];
        const double lumpedPreviousSeconds=lumpedElapsedSeconds;
        lumpedElapsedSeconds+=dTime;
        std::vector<double> previousLumpedK;
        for(const auto& b:lumpedBodies)previousLumpedK.push_back(b.temperatureK);
        const MeteoStep& meteo = meteorology[static_cast<size_t>(node)];
        updateSolarPosition(meteo);
        const std::string token = timeToken(meteo.julianTime);
        {
        // All per-node output/solver scratch arrays die before image export.
        const std::vector<float> sunlit = computeSunlit();

        if (node == startNode) {
            // FacetEB temperatures are solved states.  XML thermal-material
            // temperatures are inputs only for the RT modes, so use the
            // current air temperature solely as the EB solver's neutral seed.
            const float initialTemperature = std::clamp(
                meteo.airTemperature + 273.15f, 220.0f, 340.0f);
            std::fill(
                temperature.begin(), temperature.end(), initialTemperature);
            if (hasInitialSoilTemperature) {
                std::fill(temperature.begin() + static_cast<size_t>(scene.leafFacetCount) * 2U,
                          temperature.end(), configuredSoilTemperature);
            }
            // Water bodies with an explicit start temperature (material
            // initialTemperatureMode): the mean air temperature of the first
            // 24 h from the start node, or a fixed value.
            double dailyAirSum = 0;
            int dailyAirNodes = 0;
            for (size_t n = static_cast<size_t>(startNode); n < meteorology.size() &&
                 meteorology[n].julianTime < meteorology[static_cast<size_t>(startNode)].julianTime + 1.0f - 1e-4f; ++n) {
                dailyAirSum += meteorology[n].airTemperature;
                ++dailyAirNodes;
            }
            const float dailyMeanAirK = std::clamp(static_cast<float>(dailyAirSum / std::max(1, dailyAirNodes)) + 273.15f, 220.0f, 340.0f);
            for (uint32_t facet = 0; facet < scene.leafFacetCount; ++facet) {
                const auto& surface = surfaceForFacet(scene, facet);
                if (!surface.water || surface.waterInitialMode == 0) continue;
                const float start = surface.waterInitialMode == 1 ? dailyMeanAirK : surface.waterInitialK;
                temperature[size_t(facet) * 2U] = temperature[size_t(facet) * 2U + 1U] = start;
            }
            // Submerged hull facets start at the water temperature.
            if (!submergedFacet.empty()) {
                const float startWaterK = sceneWaterTemperature();
                for (uint32_t facet = 0; facet < scene.leafFacetCount; ++facet)
                    if (submergedFacet[facet]) {
                        const float waterK = shipWaterK(surfaceForFacet(scene, facet), startWaterK);
                        const float wet = wetFraction[facet];
                        const float start = wet >= 1.0f ? waterK
                            : wet * waterK + (1.0f - wet) * temperature[size_t(facet) * 2U];
                        temperature[size_t(facet) * 2U] = temperature[size_t(facet) * 2U + 1U] = start;
                    }
            }
            stateTemperature = {temperature, temperature};
        }
        const float nodeWaterK = submergedFacet.empty() ? -1.0f : sceneWaterTemperature();
        // Illuminated part of each side. A thin object facet has one sunlit
        // part shared by both faces (sunlit when either face is), as in FacetRT.
        std::vector<float> sunlitPart(surfaceCount);
        for (uint32_t facet = 0; facet < scene.facetCount(); ++facet) {
            const size_t side0 = static_cast<size_t>(facet) * 2U;
            if (facet < scene.leafFacetCount) {
                const float part = std::clamp(sunlit[side0] + sunlit[side0 + 1U], 0.0f, 1.0f);
                sunlitPart[side0] = sunlitPart[side0 + 1U] = part;
            } else {
                sunlitPart[side0] = std::clamp(sunlit[side0], 0.0f, 1.0f);
                sunlitPart[side0 + 1U] = std::clamp(sunlit[side0 + 1U], 0.0f, 1.0f);
                // The soil-facing side mirrors the upper side's states, so it
                // also takes its sunlit part (same mean temperature on output).
                const size_t down = side0 + groundDownSide(facet);
                sunlitPart[down] = sunlitPart[down ^ 1U];
            }
        }
        // Conservative sunlit/shaded transfer before the stored states become
        // this node's previous states. Every stored quantity of a side (skin
        // temperature, method-1 surface history, method-2 profile) moves with
        // the same area weights, so sum(p*C*T) is that of the preceding node.
        // A side whose sunlit part is unchanged is not touched (bitwise).
        size_t historyChangedSides = 0;
        double legacyObjectJump = 0, legacyObjectJumpAbs = 0, legacySoilJump = 0, legacySoilJumpAbs = 0;
        double historyMaximumGap = 0;
        if (node != startNode && previousSunlitPart.size() == surfaceCount) {
            const bool legacy = thermalHistoryMode == "legacy";
            const bool mixed = thermalHistoryMode == "mixed";
            for (size_t side = 0; side < surfaceCount; ++side) {
                const double pOld = std::clamp(static_cast<double>(previousSunlitPart[side]), 0.0, 1.0);
                const double pNew = std::clamp(static_cast<double>(sunlitPart[side]), 0.0, 1.0);
                const facetphysics::SunlitStateTransfer transfer =
                    facetphysics::sunlitStateTransfer(previousSunlitPart[side], sunlitPart[side]);
                if (transfer.target < 0 && !mixed) continue;
                const uint32_t facet = static_cast<uint32_t>(side / 2U);
                const bool groundSide = side >= firstGroundSide;
                if (groundSide && side % 2U == groundDownSide(facet)) continue;
                const auto& surface = surfaceForFacet(scene, facet);
                const bool profileSide = groundSide && surface.soilTemperatureMethod == 2;
                // A wall facet stores its slab profile once (side 0).
                const int32_t wallSide = !groundSide && side % 2U == 0U ? wallIndex[facet] : -1;
                if (transfer.target >= 0) {
                    ++historyChangedSides;
                    historyMaximumGap = std::max(historyMaximumGap,
                        std::abs(static_cast<double>(stateTemperature[0][side]) - stateTemperature[1][side]));
                    // Object facets store one heat content per facet (side 0).
                    double gap = 0, area = facetAreas[facet];
                    if (wallSide >= 0)
                        gap = surface.wallSlab.heat(wallProfiles[0][size_t(wallSide)]) -
                            surface.wallSlab.heat(wallProfiles[1][size_t(wallSide)]);
                    else if (!groundSide && side % 2U == 0U)
                        gap = std::max(0.0f, surface.heatCapacityPerArea) *
                            (static_cast<double>(stateTemperature[0][side]) - stateTemperature[1][side]);
                    else if (profileSide)
                        gap = soilColumnHeat(surface, soilProfiles[0][side - firstGroundSide]) -
                            soilColumnHeat(surface, soilProfiles[1][side - firstGroundSide]);
                    const double jump = (pNew - pOld) * gap * area;
                    (groundSide ? legacySoilJump : legacyObjectJump) += jump;
                    (groundSide ? legacySoilJumpAbs : legacyObjectJumpAbs) += std::abs(jump);
                }
                if (legacy) continue;
                const auto move = [&](auto& sunlitValue, auto& shadedValue) {
                    if (mixed) {
                        using V = std::decay_t<decltype(sunlitValue)>;
                        const V mean = static_cast<V>(pOld * static_cast<double>(sunlitValue) +
                            (1.0 - pOld) * static_cast<double>(shadedValue));
                        sunlitValue = shadedValue = mean;
                    } else {
                        facetphysics::applySunlitStateTransfer(transfer, sunlitValue, shadedValue);
                    }
                };
                move(stateTemperature[0][side], stateTemperature[1][side]);
                float* sunlitHistory = soilHistory[0].data() + side * kSoilHistory;
                float* shadedHistory = soilHistory[1].data() + side * kSoilHistory;
                for (size_t k = 0; k < kSoilHistory; ++k) move(sunlitHistory[k], shadedHistory[k]);
                if (soilMemoryUsed && groundSide) {
                    double* sunlitMemory = soilMemory[0].data() + (side - firstGroundSide) * kSoilMemoryModes;
                    double* shadedMemory = soilMemory[1].data() + (side - firstGroundSide) * kSoilMemoryModes;
                    for (size_t j = 0; j < kSoilMemoryModes; ++j) move(sunlitMemory[j], shadedMemory[j]);
                }
                if (profileSide) {
                    const size_t g = side - firstGroundSide;
                    for (size_t layer = 0; layer < facetphysics::soilLayers; ++layer)
                        move(soilProfiles[0][g][layer], soilProfiles[1][g][layer]);
                }
                if (wallSide >= 0)
                    for (size_t cell = 0; cell < surface.wallSlab.cells(); ++cell)
                        move(wallProfiles[0][size_t(wallSide)][cell], wallProfiles[1][size_t(wallSide)][cell]);
            }
        }
        // 步骤 9：保存转移后的上一节点状态温度，并滚动土壤表面温度历史 soilHistory（最新在前，
        // 共 kSoilHistory = 8 个节点）：首节点用初始温度填满；之后整体后移一位并写入最新值。
        // 土壤惯性历史为 full 时，被挤出显式求和的最老增量（history[6] - history[7]）进入指数模态 memory，
        // 每节点按 decay 衰减。
        const std::array<std::vector<float>, 2> previousStateTemperature = stateTemperature;
        const SoilMemoryModes soilModes = soilMemoryModes(dTime, kSoilHistory);
        for (int state = 0; state < 2; ++state) {
            for (size_t side = 0; side < surfaceCount; ++side) {
                float* history = soilHistory[state].data() + side * kSoilHistory;
                if (node == startNode) {
                    std::fill(history, history + kSoilHistory, previousStateTemperature[state][side]);
                } else {
                    // The change between the two oldest stored nodes leaves the
                    // explicit sum and enters the exponential modes.
                    if (soilMemoryUsed && side >= firstGroundSide) {
                        double* memory = soilMemory[size_t(state)].data() + (side - firstGroundSide) * kSoilMemoryModes;
                        const double leaving = static_cast<double>(history[kSoilHistory - 2U]) - history[kSoilHistory - 1U];
                        for (size_t j = 0; j < kSoilMemoryModes; ++j)
                            memory[j] = soilModes.decay[j] * memory[j] + leaving;
                    }
                    std::copy_backward(history, history + kSoilHistory - 1U, history + kSoilHistory);
                    history[0] = previousStateTemperature[state][side];
                }
            }
        }

        // 步骤 10：土壤方法 2（多层剖面）：首节点用上一状态表面温度均匀初始化剖面与底边界温度；
        // 每节点按剖面、底边界温度、热导率、体积热容（比热×密度）和 dTime 求各状态的土壤响应 soilResponse，
        // 供后面的表面能量平衡使用。
        std::array<std::vector<facetphysics::SoilResponse>, 2> soilResponses{
            std::vector<facetphysics::SoilResponse>(groundSides), std::vector<facetphysics::SoilResponse>(groundSides)};
        for (size_t g = 0; g < groundSides; ++g) {
            const size_t side = firstGroundSide + g;
            const auto& soil = surfaceForFacet(scene, static_cast<uint32_t>(side / 2));
            if (soil.soilTemperatureMethod != 2) continue;
            if (node == startNode) soilBottomTemperature[g] = previousStateTemperature[0][side];
            for (int state = 0; state < 2; ++state) {
                if (node == startNode) soilProfiles[state][g].fill(previousStateTemperature[state][side]);
                soilResponses[state][g] = facetphysics::soilResponse(soilProfiles[state][g], soilBottomTemperature[g],
                    soil.thermalConductivity, soil.specificHeat * soil.density, dTime);
            }
        }
        // 车辆实例的舱内温度初始化（先于下面的墙板响应）：首节点把舱内空气与内饰温度设为室外气温
        // （220–340 K 限幅）；有部件模型时各内部节点与机舱同取气温，engineWarm 时发动机块起始为
        // engineWarmC（°C 转 K），机舱空气取气温与块温之间 30% 处。
        // 各墙板的室内侧温度 wallIndoor 取舱内空气（或部件模型的边界温度），机罩墙板取机舱空气温度。
        // 下面一段注释对应其后的墙板响应循环。
        // Wall slabs: start from steady conduction between the seed surface
        // temperature and the indoor air; eliminate the interior once per node.
        for (auto& v : vehicles) {
            if (v.walls.empty()) continue;
            if (node == startNode) {
                v.airK = v.interiorK = std::clamp(meteo.airTemperature + 273.15f, 220.0f, 340.0f);
                if (v.parts) {
                    // Interiors start at the outdoor air; a warm engine (engineWarm) starts hot with a bay between.
                    const auto& b = *v.body;
                    v.intK.fill(v.airK);
                    v.boundaryK = v.airK;
                    v.blockK = v.bayK = v.airK;
                    if (b.engineMulti && b.engineWarm) {
                        v.blockK = std::max(v.airK, b.engineWarmC + 273.15);
                        v.bayK = v.airK + 0.3 * (v.blockK - v.airK);
                    }
                }
            }
            for (size_t w : v.walls) wallIndoor[w] = v.parts ? v.boundaryK : v.airK;
            for (size_t w : v.hoodWalls) wallIndoor[w] = v.bayK;
        }
        // 对每个墙/屋顶面元及受晒、阴影两个状态求墙板响应 wallResponse（含内热源 internalHeat）；
        // 首节点先以种子表面温度对应的稳态剖面初始化，并记录初始储热 previousWallHeat。
        std::array<std::vector<facetphysics::WallResponse>, 2> wallResponses{
            std::vector<facetphysics::WallResponse>(wallFacets.size()), std::vector<facetphysics::WallResponse>(wallFacets.size())};
        for (size_t w = 0; w < wallFacets.size(); ++w) {
            const auto slab = slabOf(w);
            const size_t side0 = size_t(wallFacets[w]) * 2U;
            for (int state = 0; state < 2; ++state) {
                if (node == startNode) {
                    wallProfiles[state][w] = slab.steadyProfile(previousStateTemperature[state][side0]);
                    if (state == 0) previousWallHeat[w] = slab.heat(wallProfiles[0][w]);
                }
                wallResponses[state][w] = facetphysics::wallResponse(wallProfiles[state][w], slab, dTime,
                    internalHeat.empty() ? 0.0 : double(internalHeat[wallFacets[w]]));
            }
        }

        // 步骤 11：本节点短波输入。totalShortwave 为水平面入射总短波（W m-2，负值截为 0）；
        // shortwave_split 按 diffuseFractionMode（fixed / erbs）及可选实测直射/散射，
        // 给出直射比例 directFraction（erbs 模式还给出晴空指数 kt）。
        const float totalShortwave = std::max(0.0f, meteo.shortwave);
        const float cosineSolarZenith =
            std::max(0.0f, sunDirection(scene).y);
        // Direct/diffuse split of this node's Rin (light.diffuseFractionMode).
        const shortwave_split::Split shortwaveSplit = shortwave_split::split(
            diffuseFractionMode, directShortwaveFraction, meteo.shortwave,
            static_cast<float>(std::acos(std::clamp(sunDirection(scene).y, -1.0f, 1.0f)) * 180.0 / kPi),
            static_cast<int>(std::floor(meteo.julianTime)), meteo.measuredDirect, meteo.measuredDiffuse);
        // erbs 模式：用直射比例为 1 / 0 的两条单位谱形状，按本节点直射比例重新缩放；
        // fixed 模式直接使用配置好的光谱。随后在 stdout 输出一行 SHORTWAVE_SPLIT 记录本节点划分结果。
        ShortwaveSpectrum nodeShortwaveSpectrum;
        if (diffuseFractionMode == shortwave_split::kModeErbs) {
            nodeShortwaveSpectrum.direct = unitDirectSpectrum.direct;
            nodeShortwaveSpectrum.diffuse = unitDiffuseSpectrum.diffuse;
            for (float& value : nodeShortwaveSpectrum.direct) value *= shortwaveSplit.directFraction;
            for (float& value : nodeShortwaveSpectrum.diffuse) value *= 1.0f - shortwaveSplit.directFraction;
        }
        const ShortwaveSpectrum& shortwaveSpectrum =
            diffuseFractionMode == shortwave_split::kModeErbs ? nodeShortwaveSpectrum
                                                              : configuredShortwaveSpectrum;
        std::cout << "SHORTWAVE_SPLIT\tnode=" << node
                  << "\tmode=" << (diffuseFractionMode == shortwave_split::kModeErbs ? "erbs" : "fixed")
                  << "\tsource=" << shortwaveSplit.source
                  << "\trin=" << meteo.shortwave
                  << "\tkt=" << shortwaveSplit.clearnessIndex
                  << "\tdirectFraction=" << shortwaveSplit.directFraction << std::endl;
        // 以下为本节点的短波累加器（均按面元“侧”索引，surfaceCount = 2 × 面元数）。
        // absorbed*：吸收的短波（W m-2，宽波段累加）；incident*：入射；*Direct：其中的直射分量；
        // *Par*：400–700 nm 光合有效辐射的光子通量（umol m-2 s-1）；transmitted：透过车窗的短波。
        std::vector<float> absorbedShortwave(surfaceCount, 0.0f);
        // Direct-beam parts of the side-mean fluxes; a sunlit point receives
        // them divided by the side's sunlit part, a shaded point none.
        std::vector<float> absorbedDirect(surfaceCount, 0.0f);
        std::vector<float> incidentDirect(surfaceCount, 0.0f);
        std::vector<float> absorbedParDirect(surfaceCount, 0.0f);
        std::vector<float> incidentShortwave(surfaceCount,0), photovoltaicPower(surfaceCount,0), photovoltaicSlope(surfaceCount,0);
        // Shortwave transmitted through the vehicle glass (W m-2 per side).
        std::vector<float> transmittedShortwave(vehicles.empty() ? 0U : surfaceCount, 0.0f);
        facetvk::SolveResult shortwaveSolution;
        shortwaveSolution.radiosity.assign(surfaceCount, 0.0f);
        facetvk::SolveResult diffuseSolution;
        diffuseSolution.radiosity.assign(surfaceCount, 0.0f);

        // 步骤 A：分窗口短波求解。把 1 nm 光谱样本按 spectralAccelerationWidth 分成若干窗口，
        // 每个窗口用窗口平均光学量求一次辐射度，再把各窗口的吸收/入射累加成宽波段结果。
        // 窗口总能量守恒：总短波 = 直射（折算为法向）+ 漫射，被丢弃的水平直射并入漫射。
        // FacetEB uses the same shortwave acceleration definition as VoxelEB:
        // integrate direct and diffuse irradiance inside each configured
        // wavelength window, solve once with the window-centre optics, then
        // accumulate all windows into the broadband absorbed shortwave flux.
        const size_t shortwaveSampleCount = shortwaveSpectrum.sampleCount();
        const float directSpectrumIntegral = shortwaveSampleCount >= 2U
            ? shortwaveSpectrum.integrate(
                shortwaveSpectrum.direct, 0U, shortwaveSampleCount - 1U)
            : 0.0f;
        const float directNormalFactor = directHorizontalToNormal(
            totalShortwave, directSpectrumIntegral, cosineSolarZenith);
        // Horizontal direct dropped by the conversion (sun below cos 1e-4 or
        // the beam cap) is added to the sky diffuse: total shortwave is conserved.
        const float lostDirectFraction = 1.0f -
            directHorizontalKeptFraction(directNormalFactor, cosineSolarZenith);
        std::vector<float> absorbedParPhotons(surfaceCount, 0.0f);
        std::vector<float> incidentParPhotons(surfaceCount, 0.0f);
        if (totalShortwave > 0.0f && shortwaveSampleCount >= 2U) {
            // Samples are 1 nm from 400 nm; index 300 (700 nm) closes the PAR range.
            constexpr size_t parLastSample = 300U;
            std::vector<float> spectralWeight(shortwaveSampleCount);
            std::vector<float> wavelengthNm(shortwaveSampleCount);
            for (size_t sample = 0; sample < shortwaveSampleCount; ++sample) {
                spectralWeight[sample] = shortwaveSpectrum.direct[sample] +
                    shortwaveSpectrum.diffuse[sample];
                wavelengthNm[sample] = 400.0f + static_cast<float>(sample);
            }
            for (size_t firstSample = 0U, lastSample = 0U;
                 firstSample + 1U < shortwaveSampleCount;
                 firstSample = lastSample) {
                lastSample = std::min(
                    firstSample + spectralAccelerationWidth,
                    shortwaveSampleCount - 1U);
                // 窗口在 700 nm（parLastSample）处截断，保证每个窗口要么完全在 PAR 内、要么完全在外。
                if (firstSample < parLastSample) lastSample = std::min(lastSample, parLastSample);
                const bool parWindow = lastSample <= parLastSample;
                const float directFraction = shortwaveSpectrum.integrate(
                    shortwaveSpectrum.direct, firstSample, lastSample);
                const float diffuseFraction = shortwaveSpectrum.integrate(
                    shortwaveSpectrum.diffuse, firstSample, lastSample);
                // 该窗口光谱权重几乎为 0（例如光谱表末端），跳过以省去一次无意义的求解。
                if (directFraction + diffuseFraction <= 1.0e-10f) {
                    continue;
                }
                // 窗口的法向直射与水平漫射辐照（W m-2）：直射按光谱份额分配并换算到垂直光线的平面；
                // 漫射 = 天空散射份额 + 换算时丢弃的直射份额，使窗口总量与 Rin 一致。
                const float windowDirectNormal =
                    totalShortwave * directFraction * directNormalFactor;
                const float windowDiffuse =
                    totalShortwave * (diffuseFraction + directFraction * lostDirectFraction);
                const std::vector<facetvk::SurfaceOptics> windowOptics =
                    makeEnergyShortwaveOptics(
                        scene, sunlit, windowDirectNormal, windowDiffuse, firstSample,
                        lastSample, spectralWeight);
                // Photon conversion at the irradiance-weighted window wavelength.
                const float windowWavelength = windowBandValue(
                    wavelengthNm, firstSample, lastSample, spectralWeight,
                    400.0f + static_cast<float>(firstSample + lastSample) / 2.0f);
                // 求解本窗口含直射的辐射度，再换算为各侧入射辐照度 windowIncident（W m-2）。
                const facetvk::SolveResult windowSolution = solveShortwaveWindow(
                    windowOptics, windowDiffuse, windowDirectNormal + windowDiffuse);
                const std::vector<float> windowIncident =
                    incidentRadiation(
                        windowSolution, windowOptics, windowDiffuse);
                // The direct-free solve only feeds lightEnhancement, which is
                // reported for the final node; skip it on every other node.
                facetvk::SolveResult windowDiffuseSolution;
                if (node == endNode - 1) {
                    windowDiffuseSolution = solveShortwaveWindow(
                        withoutDirectLight(windowOptics), windowDiffuse,
                        windowDirectNormal + windowDiffuse);
                } else {
                    windowDiffuseSolution.radiosity.assign(surfaceCount, 0.0f);
                }
                for (size_t side = 0; side < surfaceCount; ++side) {
                    // 逐侧累加：吸收率 = 1 - ρ - τ（限幅 [0,1]），吸收 = 吸收率 × 入射；直射分量单独累计，
                    // 供后续对“受光点”按受光比例放大；PAR 窗口再把 W m-2 换算为光子通量。
                    const float absorptance = std::clamp(
                        1.0f - windowOptics[side].reflectance -
                            windowOptics[side].transmittance,
                        0.0f, 1.0f);
                    incidentShortwave[side] += windowIncident[side];
                    if (!transmittedShortwave.empty())
                        transmittedShortwave[side] += windowOptics[side].transmittance * windowIncident[side];
                    absorbedShortwave[side] +=
                        absorptance * windowIncident[side];
                    const float windowDirect = windowOptics[side].directIrradiance;
                    incidentDirect[side] += windowDirect;
                    absorbedDirect[side] += absorptance * windowDirect;
                    if (parWindow) {
                        // W m-2 * wavelength_nm / (N_A h c) -> umol photons m-2 s-1
                        incidentParPhotons[side] += windowIncident[side] *
                            windowWavelength / 119.7117122f;
                        absorbedParPhotons[side] += absorptance * windowIncident[side] *
                            windowWavelength / 119.7117122f;
                        absorbedParDirect[side] += absorptance * windowDirect *
                            windowWavelength / 119.7117122f;
                    }
                    shortwaveSolution.radiosity[side] +=
                        windowSolution.radiosity[side];
                    diffuseSolution.radiosity[side] +=
                        windowDiffuseSolution.radiosity[side];
                }
                shortwaveSolution.iterations = std::max(
                    shortwaveSolution.iterations,
                    windowSolution.iterations);
                shortwaveSolution.maxDelta = std::max(
                    shortwaveSolution.maxDelta,
                    windowSolution.maxDelta);
                diffuseSolution.iterations = std::max(
                    diffuseSolution.iterations,
                    windowDiffuseSolution.iterations);
                diffuseSolution.maxDelta = std::max(
                    diffuseSolution.maxDelta,
                    windowDiffuseSolution.maxDelta);
            }
        }

        // 步骤 B：节点气象量。长波下行缺测（<=0）时用天空等效温度的黑体辐射代替；
        // 气温限幅在 [220, 340] K；气压（hPa）下限 100；空气密度用理想气体 ρ = P/(287.05·T)。
        // 空气动力学导热 = 5.8 + 4.1·sqrt(风速)（W m-2 K-1，风速下限 0.1 m s-1），
        // 阻抗 ra = ρ·cp/导热（cp = 1005 J kg-1 K-1）。
        shortwaveDone = Clock::now();
        const float skyLongwave = meteo.longwave > 0.0f
            ? meteo.longwave
            : kStefanBoltzmann * std::pow(skyTemperature, 4.0f);
        const float airTemperature =
            std::clamp(meteo.airTemperature + 273.15f, 220.0f, 340.0f);
        const float aerodynamicConductance =
            5.8f + 4.1f * std::sqrt(std::max(0.1f, meteo.windSpeed));
        const float airPressure = std::max(100.0f, meteo.pressure);
        const float airDensity = airPressure * 100.0f /
            (287.05f * airTemperature);
        const float aerodynamicResistance = airDensity * 1005.0f /
            std::max(0.1f, aerodynamicConductance);
        // 步骤 C：节点级对流/接触辅助量（下面 3 个 lambda）：objectConvectiveScale（船体相对风）、
        // facetConvectiveScale（车辆面元用车速的相对风，否则同船体）、waterContact（浸没面元的
        // 总显热导热与参考温度）。三者的结果都作为 GPU 参数 P[3]、P[30]、P[31] 的来源。
        // Ship hulls (energy.comp P[3]): the forced-convection coefficient at the
        // relative wind |u_wind - v_ship| as a multiplier of the material
        // convective scale; speed 0 keeps the material value. Without a heading
        // and wind direction the wind is taken perpendicular to the course,
        // sqrt(u^2 + v^2); with them u^2 + v^2 + 2 u v cos(heading - wind from).
        const auto objectConvectiveScale = [&](const SurfaceParameters& s) {
            if (!(s.shipSpeed > 0)) return s.convectiveScale;
            const float squared = s.shipWindCosine == 0.0f ? meteo.windSpeed * meteo.windSpeed + s.shipSpeed * s.shipSpeed
                : meteo.windSpeed * meteo.windSpeed + s.shipSpeed * s.shipSpeed +
                    2.0f * meteo.windSpeed * s.shipSpeed * s.shipWindCosine;
            const float relative = std::sqrt(std::max(0.0f, squared));
            return s.convectiveScale * (5.8f + 4.1f * std::sqrt(std::max(0.1f, relative))) / aerodynamicConductance;
        };
        // Facets of a vehicle instance use the vehicle's own speed / course
        // (body material cabin parameters), same relative-wind rule as the ship.
        const auto facetConvectiveScale = [&](uint32_t facet, const SurfaceParameters& s) {
            const SurfaceParameters* body = facet < scene.leafFacetCount ? facetConvectiveScaleVehicle(facet) : nullptr;
            if (!body) return objectConvectiveScale(s);
            const float squared = body->vehicleWindCosine == 0.0f
                ? meteo.windSpeed * meteo.windSpeed + body->vehicleSpeed * body->vehicleSpeed
                : meteo.windSpeed * meteo.windSpeed + body->vehicleSpeed * body->vehicleSpeed +
                    2.0f * meteo.windSpeed * body->vehicleSpeed * body->vehicleWindCosine;
            const float relative = std::sqrt(std::max(0.0f, squared));
            return s.convectiveScale * (5.8f + 4.1f * std::sqrt(std::max(0.1f, relative))) / aerodynamicConductance;
        };
        // Submerged hull facet (energy.comp P[30], P[31]): total sensible
        // conductance and its reference temperature; {0, air} = air exchange.
        // A wall slab's outer face touches the water; a thin shell has one face
        // on the water and the other in the air. A partly wetted facet (draft
        // from the water surface) mixes the water and air conductances by the
        // wetted fraction; a fully wetted one keeps the original expressions.
        const auto waterContact = [&](uint32_t facet, const SurfaceParameters& s) -> std::array<float, 2> {
            if (submergedFacet.empty() || facet >= scene.leafFacetCount || !submergedFacet[facet])
                return {0.0f, airTemperature};
            const float waterK = shipWaterK(s, nodeWaterK);
            const float wet = wetFraction[facet];
            const float air = aerodynamicConductance * objectConvectiveScale(s);
            if (wallIndex[facet] >= 0) {
                if (wet >= 1.0f) return {s.waterCoefficient, waterK};
                const float water = wet * s.waterCoefficient, dry = (1.0f - wet) * air, total = water + dry;
                return {total, (water * waterK + dry * airTemperature) / total};
            }
            if (wet >= 1.0f) {
                const float total = s.waterCoefficient + air;
                return {total, (s.waterCoefficient * waterK + air * airTemperature) / total};
            }
            const float water = wet * s.waterCoefficient, dry = (2.0f - wet) * air, total = water + dry;
            return {total, (water * waterK + dry * airTemperature) / total};
        };
        // 按冠层柱（高度、LAI、叶宽、当前 1/L）初始化每柱的空气动力学参数（叶片阻抗、土壤阻抗等）。
        for (size_t index = 0; index < columns.size(); ++index) {
            columnAerodynamics[index] = aerodynamics(
                referenceHeight, meteo.windSpeed, columns[index].height,
                columns[index].lai, columns[index].leafWidth, columnInverseL[index]);
        }

        // 步骤 D：能量平衡的通量容器与收敛量。每个面元侧都有两个状态：state 0 = 受光点，
        // state 1 = 阴影点；最终输出通量按 sunlitPart 在两态间加权混合。
        // Side fluxes for output: area-weighted sunlit/shaded combinations.
        std::vector<float> netRadiation(surfaceCount);
        std::vector<float> sensibleHeat(surfaceCount);
        std::vector<float> latentHeat(surfaceCount);
        std::vector<float> storageHeat(surfaceCount);
        uint32_t couplingIterations = 0;
        float maximumTemperatureDelta = 0.0f;
        float maximumEnergyResidual = 0.0f;
        float maximumEnergyResidualEquivalent = 0.0f;

        // 模块：单个状态（受光/阴影）下逐面侧的通量（W m-2）及其对温度的斜率（W m-2 K-1）。
        // net 净辐射；sensible/latent/storage 显热/潜热/储热；photovoltaic 光伏取走的电功率；
        // internal 集总体内部热传导；source 热源（船舶内热等）；*Slope 为各项对表面温度的导数；
        // temperatureStep 叶片联立求解给出的温度更新量（K）；stomatalMismatch 气孔阻抗自洽失配；
        // leafResistance 叶片气孔阻抗（s m-1）。
        struct StateFluxes {
            std::vector<float> net, sensible, sensibleSlope, latent, latentSlope,
                storage, storageSlope, photovoltaic, photovoltaicSlope, temperatureStep, stomatalMismatch,internal,internalSlope,source,
                leafResistance;
        };
        const auto makeStateFluxes = [&]() {
            const std::vector<float> zero(surfaceCount, 0.0f);
            return StateFluxes{zero, zero, zero, zero, zero, zero, zero, zero, zero, zero, zero,zero,zero,zero,zero};
        };
        // CPU/GPU audit only: stomatal resistance imposed on the CPU leaf solve
        // (index state * surfaceCount + side; <= 0 solves it freely).
        const std::vector<float>* auditLeafResistance = nullptr;
        std::array<StateFluxes, 2> stateFlux{makeStateFluxes(), makeStateFluxes()};
        // Heat source of the facet balance (W m-2 facet area; energy.comp F[11]):
        // ship internal heat of a thin shell and, on the scene water facets, the
        // hull heat of the preceding node (waterFeedback). Wall slabs take their
        // internal heat in the slab (wallResponse) instead.
        if (!internalHeat.empty() || waterFeedbackSource != 0.0f)
            for (uint32_t facet = 0; facet < scene.leafFacetCount; ++facet) {
                if (wallIndex[facet] >= 0) continue;
                const auto& surface = surfaceForFacet(scene, facet);
                float source = internalHeat.empty() ? 0.0f : internalHeat[facet];
                if (waterFeedbackSource != 0.0f && surface.water && !surface.vegetation) source += waterFeedbackSource;
                for (int state = 0; state < 2; ++state)
                    stateFlux[state].source[size_t(facet) * 2U] = stateFlux[state].source[size_t(facet) * 2U + 1U] = source;
            }
        // 收敛诊断量：稳定度（1/L）最大变化、气孔阻抗自洽最大失配、叶片能量残差最大值、
        // 能量残差超限的面元数、集总体表面最大残差。
        float maximumStabilityChange = 0.0f;
        float maximumStomatalMismatch = 0.0f, maximumLeafEnergyResidual = 0.0f;
        uint32_t failingEnergyFacets = 0;
        double lumpedSurfaceResidual=0;
        // 能量平衡收敛判据：超限面元数与温度等效残差由 facetEnergyConverged 判定，
        // 且集总体表面的最大残差 < 2 W m-2（阈值为经验取值，来源待核对）。
        const auto energyConverged = [&]() {
            return facetphysics::facetEnergyConverged(
                       failingEnergyFacets, scene.facetCount(),
                       maximumEnergyResidualEquivalent) &&
                lumpedSurfaceResidual<2;
        };
        // The maximum over states/sides prevents cancellation and double counting.
        // Faces whose emission enters the facet balance, x4 sigma T^3 each: both
        // faces of a thin object facet, the outdoor face of a wall slab.
        const auto radiatingFaces = [&](uint32_t facet) {
            return facet < scene.leafFacetCount ? (wallIndex[facet] >= 0 ? 4.0f : 8.0f) : 4.0f;
        };
        // 作用：统计能量残差超限（facetEnergyViolation）的面元数。残差 = 净辐射 + 内部传热 + 热源
        // - 显热 - 潜热 - 储热 - 光伏（仅物体面元）；物体面元只检查第一侧，地面面元检查两侧。
        const auto countFailingEnergyFacets = [&]() {
            uint32_t count = 0;
            for (uint32_t facet = 0; facet < scene.facetCount(); ++facet) {
                const bool object = facet < scene.leafFacetCount;
                bool failing = false;
                for (int state = 0; state < 2; ++state) {
                    const auto& flux = stateFlux[state];
                    const size_t first = size_t(facet) * 2U;
                    for (size_t side = first; side < first + (object ? 1U : 2U); ++side) {
                        const float residual = flux.net[side] + flux.internal[side] + flux.source[side] - flux.sensible[side] - flux.latent[side] -
                            flux.storage[side] - (object ? flux.photovoltaic[side] : 0.0f);
                        failing = failing || facetphysics::facetEnergyViolation(residual);
                    }
                }
                count += failing ? 1U : 0U;
            }
            return count;
        };
        // A sunlit point receives the side's direct-beam flux divided by the
        // side's sunlit part; a shaded point receives only the diffuse part.
        const auto stateValue = [&](const std::vector<float>& total,
                                    const std::vector<float>& direct,
                                    int state, size_t side) {
            const float diffuse = total[side] - direct[side];
            if (state != 0) return diffuse;
            return sunlitPart[side] > 1.0e-6f
                ? diffuse + direct[side] / sunlitPart[side] : diffuse;
        };
        // 作用：由受光/阴影两态温度按受光比例 sunlitPart 加权，得到每侧平均温度 temperature。
        const auto updateMeanTemperature = [&]() {
            for (size_t side = 0; side < surfaceCount; ++side) {
                temperature[side] = sunlitPart[side] * stateTemperature[0][side] +
                    (1.0f - sunlitPart[side]) * stateTemperature[1][side];
            }
        };
        // 作用：给定两态温度 stateTemperature、长波光学量与长波入射，计算所有面侧的净辐射、
        // 显热、潜热、储热、光伏发电及其对温度的斜率，写入 stateFlux，并混合成输出通量。
        // 输入：longwaveOptics（每侧 ρ/τ）、longwaveIncident（每侧入射长波，W m-2）。
        // 步骤：1 集总体温度 → 2 每侧辐射净通量 → 3 并行逐面元通量（物体/地面两类）→ 4 受光比例混合。
        const auto evaluateEnergyFluxes =
            [&](const std::vector<facetvk::SurfaceOptics>& longwaveOptics,
                const std::vector<float>& longwaveIncident) {
                // 步骤 1：集总体（lumpedBodies，如车辆舱体/船体热容）：用其面元的面积×权重加权表面温度，
                // 解集总体核心温度（solveLumpedCore），其结果在步骤 3 以内部传热项耦合回面元。
                if(!lumpedBodies.empty()) {
                    std::vector<double> weighted(lumpedBodies.size(),0);
                    for(uint32_t f=0;f<scene.facetCount();++f)if(lumpedFacet[f]>=0) {
                        const size_t side=size_t(f)*2;const double sun=sunlitPart[side];
                        const double skin=sun*stateTemperature[0][side]+(1-sun)*stateTemperature[1][side];
                        weighted[size_t(lumpedFacet[f])]+=facetAreas[f]*surfaceForFacet(scene,f).lumpedWeight*skin;
                    }
                    for(size_t i=0;i<lumpedBodies.size();++i) {
                        auto& b=lumpedBodies[i];
                        b.step=facetphysics::solveLumpedCore(b.p,previousLumpedK[i],weighted[i]/b.weightedArea,
                            airTemperature,dTime,lumpedPreviousSeconds);
                        b.temperatureK=b.step.temperatureK;
                    }
                }
                // 步骤 2（逐状态）：每侧辐射净通量 = 吸收短波 + ε·入射长波 - ε·σT⁴，其中 ε = 1-ρ-τ，
                // T 限幅在 [1, 1000] K；受光态的直射短波按受光比例放大（见 stateValue）。
                std::vector<float> radiativeNet(surfaceCount);
                for (int state = 0; state < 2; ++state) {
                    const std::vector<float>& T = stateTemperature[state];
                    const std::vector<float>& previousT = previousStateTemperature[state];
                    StateFluxes& flux = stateFlux[state];
                    parallelRanges(surfaceCount, 4096U, [&](size_t begin, size_t end) {
                    for (size_t side = begin; side < end; ++side) {
                        const float emissivity = std::clamp(
                            1.0f - longwaveOptics[side].reflectance -
                            longwaveOptics[side].transmittance,
                            0.0f, 1.0f);
                        radiativeNet[side] =
                            stateValue(absorbedShortwave, absorbedDirect, state, side) +
                            emissivity * longwaveIncident[side] -
                            emissivity * kStefanBoltzmann *
                                std::pow(std::clamp(T[side], 1.0f, 1000.0f), 4.0f);
                    }
                    });
                    // 步骤 3：并行逐面元（grain 64）计算各项通量；每个面元只写自己的两个侧，无数据竞争。
                    // Each facet writes only its own two sides of `flux`.
                    parallelRanges(scene.facetCount(), 64U, [&](size_t begin, size_t end) {
                    for (uint32_t facet = static_cast<uint32_t>(begin);
                         facet < end; ++facet) {
                        const size_t side0 =
                            static_cast<size_t>(facet) * 2U;
                        const SurfaceParameters& surface =
                            surfaceForFacet(scene, facet);
                        if (facet < scene.leafFacetCount) {
                            // 分支 1：物体面元（叶片、薄壳、水面、墙体、光伏、船体等）。整片面元共用一组通量，
                            // 最后对其两侧写入相同的值；净辐射取两侧之和（墙体只取室外侧）并乘遮蔽系数 shieldFraction。
                            // Wall slab: only the outdoor face is in the outdoor energy
                            // balance; the indoor face conducts through the slab to the room.
                            const int32_t wall = wallIndex[facet];
                            const float facetNetRadiation = shieldFraction(facet) * (wall >= 0
                                ? radiativeNet[side0 + wallOuter[size_t(wall)]]
                                : radiativeNet[side0] +
                                radiativeNet[side0 + 1U]);
                            // Leaves exchange whole-leaf fluxes through the SCOPE canopy
                            // resistance of their ground column; other objects keep the
                            // two-face forced-convection coefficient.
                            const bool leafExchange = surface.vegetation;
                            // A water surface exchanges heat and vapour through its upper
                            // face only; its lower face lies on the water body.
                            const bool waterSurface = surface.water && !leafExchange;
                            const CanopyAerodynamics& aerodynamics =
                                columnAerodynamics[facetColumn[facet]];
                            const std::array<float, 2> contact = leafExchange
                                ? std::array<float, 2>{0.0f, airTemperature} : waterContact(facet, surface);
                            // 显热导热（W m-2 K-1）：叶片 = ρ·cp·scale / 叶片阻抗；浸没面元用 waterContact 的总导热；
                            // 其余 = 空气动力导热 × 对流缩放 × 换热面数（水面/墙体 1 个面，薄壳 2 个面）。
                            // 显热 = 导热 × (T - 参考温度)，参考温度为气温或浸没时的水温。
                            const float facetSensibleConductance = leafExchange
                                ? airDensity * 1005.0f * surface.convectiveScale /
                                    aerodynamics.leafResistance
                                : contact[0] > 0.0f ? contact[0]
                                : (waterSurface || wall >= 0 ? 1.0f : 2.0f) * aerodynamicConductance * facetConvectiveScale(facet, surface);
                            const float facetSensibleHeat = facetSensibleConductance *
                                (T[side0] - contact[1]);
                            const CanopyColumn& column = columns[facetColumn[facet]];
                            // 叶片在冠层柱内的相对深度 = -(柱顶 - 面元高度)/柱高：冠顶为 0，向下为负；
                            // 供光合参数随深度衰减（vcmax25 等）。
                            const float relativeDepth = column.height > 0.0f
                                ? -(column.top - facetHeight[facet]) / column.height : 0.0f;
                            // 本状态下该面元两侧吸收的 PAR 光子通量之和（umol m-2 s-1），作为叶片光合输入。
                            const float statePar = std::max(0.0f,
                                stateValue(absorbedParPhotons, absorbedParDirect, state, side0) +
                                stateValue(absorbedParPhotons, absorbedParDirect, state, side0 + 1U));
                            // Wall: conduction into the slab, linear in the surface
                            // temperature about the preceding state (energy.comp: I[7], I[8]).
                            const float facetStorageSlope = wall >= 0
                                ? static_cast<float>(wallResponses[state][size_t(wall)].fluxSlope())
                                : std::max(0.0f, surface.heatCapacityPerArea) /
                                dTime;
                            const float facetStorageHeat = wall >= 0
                                ? facetStorageSlope * (T[side0] - previousT[side0]) +
                                    static_cast<float>(wallResponses[state][size_t(wall)].flux(previousT[side0]))
                                : facetStorageSlope *
                                (T[side0] - previousT[side0]);
                            // 光伏发电：有效入射 = 正面入射 + 双面系数 × 背面入射；效率 η = η25·(1 + γ·(T - 298.15))；
                            // 发电功率限幅在 [0, 吸收的短波]，不超过可用能量；斜率 dP/dT = η25·γ·有效入射，
                            // 仅在功率未触及限幅时非零。非光伏面元功率为 0。
                            const float effectiveIncident =
                                stateValue(incidentShortwave, incidentDirect, state, side0) +
                                surface.pvBifaciality *
                                    stateValue(incidentShortwave, incidentDirect, state, side0 + 1U);
                            const float eta=surface.pvEta25*(1.0f+surface.pvGamma*(T[side0]-298.15f));
                            const float available =
                                stateValue(absorbedShortwave, absorbedDirect, state, side0) +
                                stateValue(absorbedShortwave, absorbedDirect, state, side0 + 1U);
                            const float rawPower=eta*effectiveIncident;
                            const float pvPower=surface.photovoltaic?std::clamp(rawPower,0.0f,std::max(0.0f, available)):0.0f;
                            const float pvSlope=surface.photovoltaic && rawPower>0 && rawPower<available ?
                                surface.pvEta25*surface.pvGamma*effectiveIncident:0.0f;
                            float facetLatentHeat = 0, facetLatentHeatSlope = 0;
                            if (waterSurface) {
                                // Open-water evaporation (VoxelEB water): saturated vapour at the
                                // mixed-layer temperature, aerodynamic resistance of the upper
                                // face rho cp / h, surface resistance rss, times the coefficient.
                                const float waterResistance = airDensity * 1005.0f /
                                    std::max(1.0e-6f, facetSensibleConductance);
                                facetLatentHeat = surface.evaporationCoefficient * latentHeatFlux(T[side0], airDensity,
                                    airPressure, meteo.vaporPressure, waterResistance, surface.surfaceResistance);
                                facetLatentHeatSlope = surface.evaporationCoefficient * latentHeatFluxDerivative(T[side0],
                                    airDensity, airPressure, meteo.vaporPressure, waterResistance, surface.surfaceResistance);
                            }
                            // 叶片：温度与气孔阻抗联立求解。thermal(rs) 在给定气孔阻抗下用牛顿法求能量平衡的叶温根；
                            // physiology(T, rs) 在给定叶温下求气孔阻抗（Collatz 或 Farquhar，由 vegetationTemperatureMethod
                            // 选择）；coupledLeaf 迭代二者至自洽。
                            if (leafExchange) {
                                const double emissivitySum = std::clamp(1.0f - longwaveOptics[side0].reflectance - longwaveOptics[side0].transmittance, 0.0f, 1.0f) +
                                    std::clamp(1.0f - longwaveOptics[side0 + 1].reflectance - longwaveOptics[side0 + 1].transmittance, 0.0f, 1.0f);
                                const auto latent = [&](double t, double resistance) {
                                    return latentHeatFlux(static_cast<float>(t), airDensity, airPressure,
                                        meteo.vaporPressure, aerodynamics.leafResistance, static_cast<float>(resistance));
                                };
                                const auto physiology = [&](double t, double resistance) {
                                    const float saturation = saturationVaporPressure(static_cast<float>(t));
                                    const double vapor = meteo.vaporPressure + std::max(0.0f, saturation - meteo.vaporPressure) *
                                        aerodynamics.leafResistance / (aerodynamics.leafResistance + resistance);
                                    return vegetationTemperatureMethod == 0
                                        ? leafStomatalResistance(surface.leaf, static_cast<float>(t), statePar,
                                            static_cast<float>(vapor), airPressure, relativeDepth, carbonDioxide)
                                        : facetbio::farquhar(surface.leaf, t, statePar, std::clamp(vapor / saturation, 0.0, 1.0),
                                            airPressure, relativeDepth, carbonDioxide).resistance;
                                };
                                const auto thermal = [&](double resistance) {
                                    return facetphysics::temperatureRoot(T[side0], [&](double trial) {
                                        const double raw = surface.pvEta25 * (1 + surface.pvGamma * (trial - 298.15)) * effectiveIncident;
                                        const double power = surface.photovoltaic ? std::clamp(raw, 0.0, static_cast<double>(std::max(0.0f, available))) : 0;
                                        const double powerSlope = surface.photovoltaic && raw > 0 && raw < available ?
                                            surface.pvEta25 * surface.pvGamma * effectiveIncident : 0;
                                        const double residual = facetNetRadiation - emissivitySum * kStefanBoltzmann * (std::pow(trial, 4) - std::pow(T[side0], 4)) -
                                            facetSensibleConductance * (trial - airTemperature) - latent(trial, resistance) -
                                            facetStorageSlope * (trial - previousT[side0]) - power;
                                        const double response = 4 * emissivitySum * kStefanBoltzmann * std::pow(trial, 3) +
                                            facetSensibleConductance + facetStorageSlope + powerSlope +
                                            latentHeatFluxDerivative(static_cast<float>(trial), airDensity, airPressure, meteo.vaporPressure,
                                                aerodynamics.leafResistance, static_cast<float>(resistance));
                                        return std::array<double, 2>{residual, response};
                                    });
                                };
                                // CPU/GPU 审计时可强加气孔阻抗（只解叶温）；正常情况联立求解 (T, rs)。
                                const float imposedResistance = auditLeafResistance
                                    ? (*auditLeafResistance)[size_t(state) * surfaceCount + side0] : 0.0f;
                                const auto coupled = imposedResistance > 0.0f
                                    ? facetphysics::LeafState{thermal(imposedResistance), double(imposedResistance)}
                                    : facetphysics::coupledLeaf(thermal, physiology);
                                flux.leafResistance[side0] = static_cast<float>(coupled.resistance);
                                // All unknowns are re-solved at every outer radiation step. Relax
                                // temperature towards the joint root, never freeze the physiology.
                                flux.temperatureStep[side0] = temperatureRelaxation * static_cast<float>(
                                    std::clamp(coupled.temperature - T[side0], -5.0, 5.0));
                                facetLatentHeat = latent(T[side0], coupled.resistance);
                                facetLatentHeatSlope = latentHeatFluxDerivative(T[side0], airDensity, airPressure,
                                    meteo.vaporPressure, aerodynamics.leafResistance, static_cast<float>(coupled.resistance));
                                flux.stomatalMismatch[side0] = static_cast<float>(std::abs(std::log(
                                    coupled.resistance / physiology(T[side0], coupled.resistance))));
                            }
                            // 把本面元的通量写入两侧（两侧相同）；有集总体时叠加内部传热 G·(T_body - T) 与斜率 G。
                            for (uint32_t localSide = 0;
                                 localSide < 2U; ++localSide) {
                                const size_t side = side0 + localSide;
                                flux.photovoltaic[side] = pvPower;
                                flux.photovoltaicSlope[side] = pvSlope;
                                flux.net[side] = facetNetRadiation;
                                flux.sensible[side] = facetSensibleHeat;
                                flux.sensibleSlope[side] = facetSensibleConductance;
                                flux.latent[side] = facetLatentHeat;
                                flux.latentSlope[side] = facetLatentHeatSlope;
                                flux.storage[side] = facetStorageHeat;
                                flux.storageSlope[side] = facetStorageSlope;
                                if(lumpedFacet[facet]>=0) {
                                    const auto& b=lumpedBodies[size_t(lumpedFacet[facet])];
                                    flux.internal[side]=float(lumpedConductanceDensity[facet]*(b.temperatureK-T[side]));
                                    flux.internalSlope[side]=float(lumpedConductanceDensity[facet]);
                                }
                            }
                        } else {
                            // 分支 2：地面面元。逐侧计算，朝下（接触土壤）的一侧通量全部置 0；
                            // 朝上一侧：显热用冠层/裸地阻抗，潜热在干土（含水率 < 0.03）时为 0，
                            // 储热按 soilTemperatureMethod：0 = 0.35×净辐射的经验取值；1 = 半无限导热 + 表面温度历史；
                            // 其余 = 多层土壤热响应 soilResponses。
                            // Ground under vegetation is sheltered by its column's
                            // canopy, (LAI+1)*(raa+rws+rbs); open ground uses raa;
                            // both add free convection (facetphysics::groundExchange).
                            const float columnResistance =
                                columnAerodynamics[facetColumn[facet]].soilResistance;
                            for (uint32_t localSide = 0;
                                 localSide < 2U; ++localSide) {
                                const size_t side = side0 + localSide;
                                if (localSide == groundDownSide(facet)) {
                                    for (auto field : {&StateFluxes::net, &StateFluxes::sensible,
                                                       &StateFluxes::sensibleSlope, &StateFluxes::latent,
                                                       &StateFluxes::latentSlope, &StateFluxes::storage,
                                                       &StateFluxes::storageSlope})
                                        (flux.*field)[side] = 0.0f;
                                    continue;
                                }
                                // 地面对流阻抗（可含自由对流并联与 MOST 稳定度修正，见 groundExchange）。
                                // Free convection when the side is warmer than the
                                // air (energy.comp: same rule, P[28] = 1 + MOST).
                                const auto exchange = facetphysics::groundExchange(
                                    columnResistance, T[side] - airTemperature, stabilityCorrection);
                                const float groundResistance = exchange.resistance;
                                const float groundConductance = airDensity * 1005.0f / groundResistance;
                                flux.net[side] = radiativeNet[side];
                                flux.sensible[side] =
                                    groundConductance * (T[side] - airTemperature);
                                flux.sensibleSlope[side] = exchange.extraSlope > 0.0f
                                    ? groundConductance + airDensity * 1005.0f * exchange.extraSlope
                                    : groundConductance;
                                const bool drySoil =
                                    surface.soilMoisture < 0.03f;
                                flux.latent[side] = drySoil ? 0.0f :
                                    latentHeatFlux(
                                        T[side], airDensity,
                                        airPressure, meteo.vaporPressure,
                                        groundResistance,
                                        surface.surfaceResistance);
                                flux.latentSlope[side] = drySoil ? 0.0f :
                                    latentHeatFluxDerivative(
                                        T[side], airDensity,
                                        airPressure, meteo.vaporPressure,
                                        groundResistance,
                                        surface.surfaceResistance);
                                if (surface.soilTemperatureMethod == 0) {
                                    flux.storageSlope[side] = 0.0f;
                                    flux.storage[side] = 0.35f * radiativeNet[side];
                                } else if (surface.soilTemperatureMethod == 1) {
                                    // Semi-infinite conduction driven by the full surface
                                    // temperature history: G = 2*Gamma/sqrt(pi*dt) *
                                    // sum_k dT_k*(sqrt(k+1)-sqrt(k)), newest change first.
                                    // Sunlit and shaded soil keep separate histories (SCOPE).
                                    const float* history = soilHistory[state].data() + side * kSoilHistory;
                                    flux.storageSlope[side] = soilStorageCoefficient(surface, dTime);
                                    double weightedChange = T[side] - history[0];
                                    for (size_t step = 1; step < kSoilHistory; ++step) {
                                        weightedChange += (history[step - 1] - history[step]) *
                                            (std::sqrt(static_cast<double>(step + 1)) -
                                             std::sqrt(static_cast<double>(step)));
                                    }
                                    if (soilMemoryUsed) weightedChange += soilMemoryChange(state, side, soilModes);
                                    flux.storage[side] = flux.storageSlope[side] *
                                        static_cast<float>(weightedChange);
                                } else {
                                    const auto& soil = soilResponses[state][side - firstGroundSide];
                                    flux.storageSlope[side] = static_cast<float>(soil.fluxSlope());
                                    flux.storage[side] = static_cast<float>(soil.flux(T[side]));
                                }
                            }
                        }
                    }
                    });
                }
                // 步骤 4：按受光比例 sunlitPart 把受光/阴影两态的通量混合成每侧的输出通量。
                parallelRanges(surfaceCount, 4096U, [&](size_t begin, size_t end) {
                for (size_t side = begin; side < end; ++side) {
                    const float sunlitWeight = sunlitPart[side];
                    const auto mix = [&](std::vector<float> StateFluxes::*field) {
                        return sunlitWeight * (stateFlux[0].*field)[side] +
                            (1.0f - sunlitWeight) * (stateFlux[1].*field)[side];
                    };
                    netRadiation[side] = mix(&StateFluxes::net);
                    sensibleHeat[side] = mix(&StateFluxes::sensible);
                    latentHeat[side] = mix(&StateFluxes::latent);
                    storageHeat[side] = mix(&StateFluxes::storage);
                    photovoltaicPower[side] = mix(&StateFluxes::photovoltaic);
                    photovoltaicSlope[side] = mix(&StateFluxes::photovoltaicSlope);
                }
                });
            };

        const auto updateStability = [&](bool advance = true) {
            maximumStabilityChange = 0.0f;
            if (!stabilityCorrection) return;
            std::vector<double> heat(columns.size(), 0.0);
            for (uint32_t f = 0; f < scene.facetCount(); ++f) {
                const size_t side = 2U * static_cast<size_t>(f) +
                    (f >= scene.leafFacetCount && scene.vertices[f * 3U].normal[1] < 0.0f ? 1U : 0U);
                const double mixed = sunlitPart[side] * stateFlux[0].sensible[side] +
                    (1 - sunlitPart[side]) * stateFlux[1].sensible[side];
                heat[facetColumn[f]] += facetAreas[f] * mixed;
            }
            for (size_t i = 0; i < columns.size(); ++i) {
                const double desired = facetphysics::inverseObukhov(heat[i] / (columnSize * columnSize),
                    airDensity, airTemperature, columnAerodynamics[i]);
                maximumStabilityChange = std::max(maximumStabilityChange,
                    static_cast<float>(std::abs(desired - columnInverseL[i]) * columnAerodynamics[i].effectiveHeight));
                if (advance) {
                    columnInverseL[i] += .5 * (desired - columnInverseL[i]);
                    columnAerodynamics[i] = aerodynamics(referenceHeight, meteo.windSpeed,
                        columns[i].height, columns[i].lai, columns[i].leafWidth, columnInverseL[i]);
                }
            }
        };

        // Maxima and temperature-equivalent residuals are diagnostics only.
        const auto measureEnergyResidual = [&](const std::vector<facetvk::SurfaceOptics>& optics) {
            maximumEnergyResidual = 0.0f;
            lumpedSurfaceResidual=0;
            maximumEnergyResidualEquivalent = 0.0f;
            maximumStomatalMismatch = maximumLeafEnergyResidual = 0.0f;
            for (int state = 0; state < 2; ++state) {
                const StateFluxes& flux = stateFlux[state];
                const std::vector<float>& T = stateTemperature[state];
                for (uint32_t facet = 0; facet < scene.facetCount(); ++facet) {
                    const bool object = facet < scene.leafFacetCount;
                    const size_t first = static_cast<size_t>(facet) * 2U;
                    for (size_t side = first; side < first + (object ? 1U : 2U); ++side) {
                        const float emissivity = std::clamp(1.0f - optics[side].reflectance - optics[side].transmittance, 0.0f, 1.0f);
                        const float residual = flux.net[side] + flux.internal[side] + flux.source[side] - flux.sensible[side] - flux.latent[side] - flux.storage[side] - (object ? flux.photovoltaic[side] : 0.0f);
                        if (object && surfaceForFacet(scene, facet).vegetation) {
                            maximumStomatalMismatch = std::max(maximumStomatalMismatch, flux.stomatalMismatch[side]);
                            maximumLeafEnergyResidual = std::max(maximumLeafEnergyResidual, std::abs(residual));
                        }
                        const float response = radiatingFaces(facet) * emissivity * kStefanBoltzmann * std::pow(T[side], 3.0f) +
                            flux.sensibleSlope[side] +
                            flux.latentSlope[side] + flux.storageSlope[side] + flux.internalSlope[side] + (object ? flux.photovoltaicSlope[side] : 0.0f);
                        if (!std::isfinite(residual) || !std::isfinite(response)) throw std::runtime_error("FacetEB energy residual is not finite");
                        maximumEnergyResidual = std::max(maximumEnergyResidual, std::abs(residual));
                        if(lumpedFacet[facet]>=0)lumpedSurfaceResidual=std::max(lumpedSurfaceResidual,double(std::abs(residual)));
                        maximumEnergyResidualEquivalent = std::max(maximumEnergyResidualEquivalent, std::abs(residual) / std::max(1.0f, response));
                    }
                }
            }
            failingEnergyFacets = countFailingEnergyFacets();
        };

        std::vector<facetvk::SurfaceOptics> finalLongwaveOptics;
        facetvk::SolveResult finalLongwaveSolution;
        std::vector<float> finalLongwaveIncident;
        if (gpuEnergy)
        {
            // Static scene and per-node forcing are uploaded once. Inner RT/EB
            // iterations exchange only seven reduction scalars with the host.
            std::vector<float> gpuParameters(size_t(scene.facetCount()) * 32U, 0.0f);
            std::vector<float> gpuInputs(surfaceCount * 2U * 12U, 0.0f);
            std::vector<float> gpuTemperatures;
            gpuTemperatures.reserve(surfaceCount * 2U);
            for (const auto &values : stateTemperature)
                gpuTemperatures.insert(gpuTemperatures.end(), values.begin(), values.end());
            const auto initialOptics = makeEnergyLongwaveOptics(scene, stateTemperature, sunlitPart);
            for (uint32_t facet = 0; facet < scene.facetCount(); ++facet)
            {
                const auto &mat = surfaceForFacet(scene, facet);
                float *data = gpuParameters.data() + size_t(facet) * 32U;
                data[0] = mat.vegetation ? 1.0f : 0.0f;
                data[1] = mat.photovoltaic ? 1.0f : 0.0f;
                data[2] = mat.heatCapacityPerArea;
                data[3] = mat.vegetation ? mat.convectiveScale : facetConvectiveScale(facet, mat);
                data[4] = mat.pvEta25;
                data[5] = mat.pvGamma;
                data[6] = mat.pvBifaciality;
                data[7] = mat.surfaceResistance;
                data[8] = mat.soilMoisture < 0.03f ? 1.0f : 0.0f;
                data[9] = float(mat.soilTemperatureMethod);
                const auto &leaf = mat.leaf;
                // Retain the CPU model's input-domain checks before dispatch.
                // Checking once per material avoids repeated validation per leaf.
                if (vegetationTemperatureMethod == 1 && mat.vegetation && !mat.photovoltaic &&
                    (facet == 0 || scene.materialIndices[facet] != scene.materialIndices[facet - 1]))
                    (void)facetbio::farquhar(leaf, airTemperature, 0.0, 0.5, airPressure, 0.0, carbonDioxide);
                data[10] = leaf.vcmax25;
                data[11] = leaf.ballBerrySlope;
                data[12] = leaf.ballBerry0;
                data[13] = leaf.rdParam;
                data[14] = leaf.kV;
                data[15] = leaf.stressFactor;
                data[16] = float(leaf.type);
                data[17] = float(leaf.temperatureCorrection);
                std::copy(leaf.tparam.begin(), leaf.tparam.end(), data + 18);
                data[23] = leaf.annualTemperature;
                data[24] = leaf.beta;
                data[25] = leaf.qLs;
                data[26] = leaf.sustainedNPQ;
                // energy.comp: 1 + local index of the soil-facing ground side.
                data[27] = facet >= scene.leafFacetCount ? 1.0f + float(groundDownSide(facet)) : 0.0f;
                // energy.comp P[28]: 0 thin shell, 1 water mixed layer (one exchanging
                // face, open-water LE; P[29] evaporation coefficient), 2 wall slab
                // (outdoor face only; P[29] its local side, conduction from I[7], I[8]).
                // Ground facets: P[28] = 1 neutral (free convection in parallel),
                // 2 MOST (larger of the two conductances), as groundExchange().
                const bool waterFacet = facet < scene.leafFacetCount && mat.water && !mat.vegetation;
                const bool wallFacet = facet < scene.leafFacetCount && wallIndex[facet] >= 0;
                data[28] = facet >= scene.leafFacetCount ? (stabilityCorrection ? 2.0f : 1.0f)
                    : wallFacet ? 2.0f : waterFacet ? 1.0f : 0.0f;
                data[29] = wallFacet ? float(wallOuter[size_t(wallIndex[facet])])
                    : waterFacet ? mat.evaporationCoefficient : 0.0f;
                // P[30] > 0: submerged ship-hull facet, total sensible conductance
                // P[30] towards the reference temperature P[31].
                if (!mat.vegetation) {
                    const auto contact = waterContact(facet, mat);
                    if (contact[0] > 0.0f) { data[30] = contact[0]; data[31] = contact[1]; }
                }
                // Non-leaf objects (their leaf-only slots 23, 24 are free):
                // P[23] heat source of the facet balance (W m-2), P[24] factor of the
                // net radiation (1 - wetted fraction of a shielded hull, else 1).
                if (facet < scene.leafFacetCount && !mat.vegetation) {
                    data[23] = 0.0f;
                    if (!wallFacet) {
                        data[23] = internalHeat.empty() ? 0.0f : internalHeat[facet];
                        if (waterFeedbackSource != 0.0f && mat.water) data[23] += waterFeedbackSource;
                    }
                    data[24] = shieldFraction(facet);
                }
            }
            for (int state = 0; state < 2; ++state)
                for (size_t side = 0; side < surfaceCount; ++side)
                {
                    const auto facet = static_cast<uint32_t>(side / 2U);
                    const auto &mat = surfaceForFacet(scene, facet);
                    const auto &column = columns[facetColumn[facet]];
                    float *data = gpuInputs.data() + (size_t(state) * surfaceCount + side) * 12U;
                    data[0] = previousStateTemperature[state][side];
                    data[1] = stateValue(absorbedShortwave, absorbedDirect, state, side);
                    data[2] = stateValue(absorbedParPhotons, absorbedParDirect, state, side);
                    data[3] = stateValue(incidentShortwave, incidentDirect, state, side);
                    data[4] = columnAerodynamics[facetColumn[facet]].leafResistance;
                    data[5] = columnAerodynamics[facetColumn[facet]].soilResistance;
                    data[6] = column.height > 0.0f ? -(column.top - facetHeight[facet]) / column.height : 0.0f;
                    if (facet >= scene.leafFacetCount && mat.soilTemperatureMethod == 1)
                    {
                        data[7] = soilStorageCoefficient(mat, dTime);
                        const float *history = soilHistory[state].data() + side * kSoilHistory;
                        double offset = -history[0];
                        for (size_t k = 1; k < kSoilHistory; ++k)
                            offset += (history[k - 1] - history[k]) * (std::sqrt(double(k + 1)) - std::sqrt(double(k)));
                        if (soilMemoryUsed) offset += soilMemoryChange(state, side, soilModes);
                        data[8] = data[7] * static_cast<float>(offset);
                    }
                    else if (facet >= scene.leafFacetCount && mat.soilTemperatureMethod == 2)
                    {
                        const auto &soil = soilResponses[state][side - firstGroundSide];
                        data[7] = static_cast<float>(soil.fluxSlope());
                        data[8] = static_cast<float>(soil.flux(0));
                    }
                    else if (facet < scene.leafFacetCount && wallIndex[facet] >= 0)
                    {
                        const auto &wall = wallResponses[state][size_t(wallIndex[facet])];
                        const size_t side0 = size_t(facet) * 2U;
                        data[7] = static_cast<float>(wall.fluxSlope());
                        data[8] = static_cast<float>(wall.flux(previousStateTemperature[state][side0]));
                    }
                    data[9] = initialOptics[side].reflectance;
                    data[10] = initialOptics[side].transmittance;
                    data[11] = sunlitPart[side];
                }
            // 节点常量（EnergyPush）：面元数、物体面元数、植被温度方法、气温（K）、空气密度、气压、
            // 水汽压、时间步 dTime、CO2、温度松弛系数、风速；第 4 项为 0（含义待核对）。
            // 之后 beginEnergyNode 把参数 / 输入 / 初始温度一次性上传到 GPU 能量求解器。
            const facetvk::EnergyPush push{scene.facetCount(),
                                           scene.leafFacetCount,
                                           static_cast<uint32_t>(vegetationTemperatureMethod),
                                           0,
                                           airTemperature,
                                           airDensity,
                                           airPressure,
                                           meteo.vaporPressure,
                                           dTime,
                                           carbonDioxide,
                                           temperatureRelaxation,
                                           meteo.windSpeed};
            model.beginEnergyNode(push, gpuParameters, gpuInputs, gpuTemperatures);
            // 作用：把 GPU 读回的快照导入主机：每态每侧的温度和 12 个通量槽（槽 0–10 依次为 fields
            // 的各项，槽 11 为叶片气孔阻抗 r_s，仅供审计），再更新平均温度并按受光比例混合输出通量。
            const auto importGpuState = [&](const facetvk::EnergySnapshot &snapshot) {
                const std::array<std::vector<float> StateFluxes::*, 11> fields{&StateFluxes::net,
                                                                               &StateFluxes::sensible,
                                                                               &StateFluxes::sensibleSlope,
                                                                               &StateFluxes::latent,
                                                                               &StateFluxes::latentSlope,
                                                                               &StateFluxes::storage,
                                                                               &StateFluxes::storageSlope,
                                                                               &StateFluxes::photovoltaic,
                                                                               &StateFluxes::photovoltaicSlope,
                                                                               &StateFluxes::temperatureStep,
                                                                               &StateFluxes::stomatalMismatch};
                for (int state = 0; state < 2; ++state)
                    for (size_t side = 0; side < surfaceCount; ++side)
                    {
                        const auto index = size_t(state) * surfaceCount + side;
                        stateTemperature[state][side] = snapshot.temperatures[index];
                        for (size_t field = 0; field < fields.size(); ++field)
                            (stateFlux[state].*fields[field])[side] = snapshot.fluxes[index * 12U + field];
                    }
                updateMeanTemperature();
                for (size_t side = 0; side < surfaceCount; ++side)
                {
                    const float w = sunlitPart[side];
                    const auto mix = [&](std::vector<float> StateFluxes::*field) {
                        return w * (stateFlux[0].*field)[side] + (1.0f - w) * (stateFlux[1].*field)[side];
                    };
                    netRadiation[side] = mix(&StateFluxes::net);
                    sensibleHeat[side] = mix(&StateFluxes::sensible);
                    latentHeat[side] = mix(&StateFluxes::latent);
                    storageHeat[side] = mix(&StateFluxes::storage);
                    photovoltaicPower[side] = mix(&StateFluxes::photovoltaic);
                    photovoltaicSlope[side] = mix(&StateFluxes::photovoltaicSlope);
                }
            };
            // 作用：导入 GPU 端归约出的统计量：最大能量残差、温度等效残差、气孔失配、叶片残差，
            // 以及残差超限的面元数。
            const auto importGpuStatistics = [&](const facetvk::EnergyStatistics &statistics) {
                maximumEnergyResidual = statistics.maxima[0];
                maximumEnergyResidualEquivalent = statistics.maxima[1];
                maximumStomatalMismatch = statistics.maxima[2];
                maximumLeafEnergyResidual = statistics.maxima[3];
                failingEnergyFacets = statistics.failingFacets;
            };
            // 以下为可选的 CPU/GPU “影子”审计（shadowEnergy）：每次 GPU 评估后，用 CPU 实现复算入射、
            // 通量与温度更新并和 GPU 结果比较；严格模式遇到首个不一致即抛异常，调查模式（shadowSurvey）只
            // 按“类别/字段”计数。仅用于验证，不改变计算结果（导入的始终是 GPU 状态）。
            std::vector<float> shadowNextTemperature;
            // Surface class of a facet, for the mismatch report.
            const auto shadowClass = [&](uint32_t facet) -> std::string {
                if (facet >= scene.leafFacetCount) return stabilityCorrection ? "ground(MOST)" : "ground";
                const auto& surface = surfaceForFacet(scene, facet);
                if (surface.vegetation) return surface.photovoltaic ? "leaf-pv" : "leaf";
                if (wallIndex[facet] >= 0) return "wall";
                if (surface.water) return "water";
                if (gpuParameters[size_t(facet) * 32U + 30U] > 0.0f) return "submerged-hull";
                return "solid-shell";
            };
            // Strict mode (1) throws at the first mismatch, survey mode (2) counts them.
            const auto shadowMismatch = [&](uint32_t facet, int state, const std::string& field,
                                            float difference, float cpuValue, float gpuValue) {
                const std::string label = shadowClass(facet) + "/" + field;
                auto& entry = shadowMismatches[label];
                if (entry.count++ == 0 || difference > entry.maxDifference || !std::isfinite(difference)) {
                    entry.facet = facet; entry.state = state; entry.node = static_cast<uint32_t>(node);
                    entry.cpu = cpuValue; entry.gpu = gpuValue;
                }
                if (std::isfinite(difference)) entry.maxDifference = std::max(entry.maxDifference, difference);
                if (!shadowSurvey)
                    throw std::runtime_error("CPU/GPU energy shadow mismatch (" + label + ") at node " + std::to_string(node) +
                        ", facet " + std::to_string(facet) + ", state " + std::to_string(state) +
                        ", difference " + std::to_string(difference) +
                        ", CPU " + std::to_string(cpuValue) + ", GPU " + std::to_string(gpuValue));
            };
            const std::array<const char*, 9> shadowFieldNames{"net", "sensible", "sensibleSlope", "latent",
                "latentSlope", "storage", "storageSlope", "photovoltaic", "photovoltaicSlope"};
            // 作用：执行一次影子审计。步骤：1 比较长波入射辐照（容差 1e-4 W m-2）；
            // 2 CPU 自由求解叶片并只松散比较气孔阻抗；3 强加 GPU 的气孔阻抗后严格比较各项通量；
            // 4 用 CPU 通量预测下一步温度（供 evaluate 后与 GPU 的温度更新比较）。
            // Optional audit: compare both implementations inside one run so
            // stochastic coplanar raster ties cannot change their forcing.
            const auto verifyGpuEvaluation = [&]() {
                if (!shadowEnergy) return;
                const auto audit = model.readEnergyState(true);
                importGpuState(audit);
                auto cpuRadiation = audit.radiation;
                cpuRadiation.incident.clear();
                const auto cpuIncident = model.incidentIrradiance(cpuRadiation, audit.optics, skyLongwave);
                for (size_t side = 0; side < surfaceCount; ++side) {
                    const float difference = std::abs(cpuIncident[side] - audit.radiation.incident[side]);
                    shadowIncidentMaximum = std::max(shadowIncidentMaximum, difference);
                    if (!(difference <= 0.0001f))
                        shadowMismatch(static_cast<uint32_t>(side / 2U), 0, "incident", difference,
                            cpuIncident[side], audit.radiation.incident[side]);
                }
                // The coupled leaf solve (stomatal bisection with a log tolerance, each
                // trial solving the leaf temperature) stops at the first trial inside
                // the tolerance. Any rounding difference between the CPU float and the
                // GPU double physiology can move that exit by one bisection step, so
                // the two resistances may differ by about the tolerance (ln units) and
                // leaf latent heat by a corresponding fraction. Therefore: (1) solve the
                // CPU freely and compare only the resistance, loosely (0.1 in ln, a sanity
                // bound: the exit band is the tolerance divided by the slope of
                // ln(rs) - ln(target), which is small where humidity feedback is strong,
                // so the difference can exceed twice the tolerance); (2) impose the GPU resistance
                // on the CPU leaf and compare every flux and the
                // stomatal mismatch tightly, which tests the formulas, not the exit step.
                evaluateEnergyFluxes(audit.optics, cpuIncident);
                std::vector<float> imposedResistance(2U * surfaceCount, 0.0f);
                for (int state = 0; state < 2; ++state)
                    for (uint32_t facet = 0; facet < scene.leafFacetCount; ++facet) {
                        if (!surfaceForFacet(scene, facet).vegetation) continue;
                        const size_t side = size_t(facet) * 2U, index = size_t(state) * surfaceCount + side;
                        // energy.comp reports the leaf r_s in slot 11 (a leaf has no heat source).
                        // Recovering it from the latent heat would be ill-conditioned near
                        // saturation, where the vapour deficit is a difference of two nearly
                        // equal float values.
                        const double resistance = audit.fluxes[index * 12U + 11U];
                        if (!(resistance >= 1.0 && resistance <= 625000.0)) continue;
                        imposedResistance[index] = static_cast<float>(resistance);
                        const float freeResistance = stateFlux[state].leafResistance[side];
                        const float difference = static_cast<float>(std::abs(std::log(freeResistance / resistance)));
                        shadowResistanceMaximum = std::max(shadowResistanceMaximum, difference);
                        if (!(difference <= 0.1f))
                            shadowMismatch(facet, state, "stomatalResistance(ln)", difference,
                                freeResistance, static_cast<float>(resistance));
                    }
                auditLeafResistance = &imposedResistance;
                evaluateEnergyFluxes(audit.optics, cpuIncident);
                auditLeafResistance = nullptr;
                shadowNextTemperature = audit.temperatures;
                const std::array<std::vector<float> StateFluxes::*, 9> fields{
                    &StateFluxes::net, &StateFluxes::sensible, &StateFluxes::sensibleSlope,
                    &StateFluxes::latent, &StateFluxes::latentSlope, &StateFluxes::storage,
                    &StateFluxes::storageSlope, &StateFluxes::photovoltaic, &StateFluxes::photovoltaicSlope};
                for (int state = 0; state < 2; ++state)
                    for (uint32_t facet = 0; facet < scene.facetCount(); ++facet) {
                        const bool object = facet < scene.leafFacetCount;
                        const auto& surface = surfaceForFacet(scene, facet);
                        const auto& flux = stateFlux[state];
                        const size_t first = size_t(facet) * 2;
                        for (size_t side = first; side < first + (object ? 1U : 2U); ++side) {
                            const size_t index = size_t(state) * surfaceCount + side;
                            for (size_t field = 0; field < fields.size(); ++field) {
                                const float difference = std::abs((flux.*fields[field])[side] - audit.fluxes[index * 12 + field]);
                                if (!std::isfinite(difference) || difference > 0.01f)
                                    shadowMismatch(facet, state, shadowFieldNames[field], difference,
                                        (flux.*fields[field])[side], audit.fluxes[index * 12 + field]);
                                if (field == 0 || field == 1 || field == 3 || field == 5 || field == 7)
                                    shadowFluxMaximum = std::max(shadowFluxMaximum, difference);
                            }
                            if (object && surface.vegetation && imposedResistance[index] > 0.0f) {
                                const float difference = std::abs(flux.stomatalMismatch[side] - audit.fluxes[index * 12 + 10]);
                                if (!std::isfinite(difference) || difference > 0.0005f)
                                    shadowMismatch(facet, state, "stomatalMismatch(ln)", difference,
                                        flux.stomatalMismatch[side], audit.fluxes[index * 12 + 10]);
                            }
                            const float t = stateTemperature[state][side];
                            const float e = std::clamp(1.0f - audit.optics[side].reflectance - audit.optics[side].transmittance, 0.0f, 1.0f);
                            const float residual = flux.net[side] + flux.internal[side] + flux.source[side] - flux.sensible[side] - flux.latent[side] - flux.storage[side] - (object ? flux.photovoltaic[side] : 0.0f);
                            const float response = radiatingFaces(facet) * e * kStefanBoltzmann * std::pow(t, 3.0f) +
                                flux.sensibleSlope[side] + flux.latentSlope[side] + flux.storageSlope[side] + flux.internalSlope[side] + (object ? flux.photovoltaicSlope[side] : 0.0f);
                            const float candidate = std::clamp(t + std::clamp(residual / std::max(1.0f, response), -5.0f, 5.0f), 220.0f, 360.0f);
                            shadowNextTemperature[index] = t + (object && surface.vegetation ? flux.temperatureStep[side] : temperatureRelaxation * (candidate - t));
                            if (object) shadowNextTemperature[index + 1] = shadowNextTemperature[index];
                        }
                        if (!object) {
                            const size_t down = size_t(state) * surfaceCount + first + groundDownSide(facet);
                            shadowNextTemperature[down] = shadowNextTemperature[down ^ 1U];
                        }
                    }
                shadowClassFacets.clear();
                for (uint32_t facet = 0; facet < scene.facetCount(); ++facet) ++shadowClassFacets[shadowClass(facet)];
                ++shadowEvaluations;
                importGpuState(audit); // Auditing never substitutes CPU states.
            };
            // 步骤 E（GPU 路径）：能量-辐射耦合迭代，最多 maximumCouplingIterations 轮。每轮：
            // GPU 评估能量（含长波辐射度）→ 导入统计 → 可选审计 → 稳定度修正时检查稳定度变化 →
            // 收敛则退出 → GPU 更新温度 → 稳定度修正时更新 1/L 并把新的叶片/土壤阻抗回传 GPU（I[4]、I[5]）。
            // 主机与 GPU 每轮只交换少量归约标量。
            for (uint32_t coupling = 0; coupling < maximumCouplingIterations; ++coupling)
            {
                importGpuStatistics(model.evaluateEnergy(skyLongwave));
                verifyGpuEvaluation();
                if (stabilityCorrection)
                {
                    importGpuState(model.readEnergyState());
                    updateStability(false);
                }
                if (energyConverged())
                    break;
                maximumTemperatureDelta = model.updateEnergyTemperature();
                if (shadowEnergy) {
                    const auto updated = model.readEnergyState();
                    for (size_t side = 0; side < shadowNextTemperature.size(); ++side) {
                        const float difference = std::abs(updated.temperatures[side] - shadowNextTemperature[side]);
                        const uint32_t updatedFacet = static_cast<uint32_t>((side % surfaceCount) / 2U);
                        // A leaf temperature is the root of the coupled leaf equation, which both
                        // solvers stop once a Newton correction is below 0.01 K (leafThermalToleranceK):
                        // rounding can move that exit by one iteration, so leaves get 1e-3 K.
                        const float updateTolerance = updatedFacet < scene.leafFacetCount &&
                            surfaceForFacet(scene, updatedFacet).vegetation ? 0.001f : 0.0002f;
                        if (!std::isfinite(difference) || difference > updateTolerance)
                            shadowMismatch(updatedFacet, int(side / surfaceCount),
                                "temperatureUpdate(K)", difference, shadowNextTemperature[side], updated.temperatures[side]);
                        if (std::isfinite(difference)) shadowUpdateMaximum = std::max(shadowUpdateMaximum, difference);
                    }
                    ++shadowUpdates;
                }
                if (stabilityCorrection)
                {
                    importGpuState(model.readEnergyState());
                    updateStability();
                    for (int state = 0; state < 2; ++state)
                        for (size_t side = 0; side < surfaceCount; ++side)
                        {
                            float *data = gpuInputs.data() + (size_t(state) * surfaceCount + side) * 12U;
                            const auto &aero = columnAerodynamics[facetColumn[side / 2U]];
                            data[4] = aero.leafResistance;
                            data[5] = aero.soilResistance;
                        }
                    model.updateEnergyInputs(gpuInputs);
                }
                couplingIterations = coupling + 1;
            }
            // 退出迭代后再评估一次，使最终报告的通量/温度/统计对应同一状态。
            importGpuStatistics(model.evaluateEnergy(skyLongwave));
            verifyGpuEvaluation();
            auto snapshot = model.readEnergyState(true);
            importGpuState(snapshot);
            // Verify the GPU reduction from the already downloaded final fluxes.
            if (countFailingEnergyFacets() != failingEnergyFacets)
                throw std::runtime_error("CPU/GPU energy failing-facet count mismatch");
            finalLongwaveOptics = std::move(snapshot.optics);
            finalLongwaveSolution = std::move(snapshot.radiation);
            finalLongwaveIncident = finalLongwaveSolution.incident;
            updateStability(false);
        }
        else
        {
            // 步骤 E（CPU 路径）：同样的耦合迭代。每轮：按当前温度重解长波辐射度 → 入射 →
            // evaluateEnergyFluxes → 残差诊断 → 收敛则退出 → 逐面侧阻尼牛顿更新温度 →
            // 更新平均温度与稳定度（1/L）。
            for (uint32_t coupling = 0; coupling < maximumCouplingIterations; ++coupling)
            {
                const std::vector<facetvk::SurfaceOptics> longwaveOptics =
                    makeEnergyLongwaveOptics(scene, stateTemperature, sunlitPart);
                const facetvk::SolveResult longwaveSolution =
                    solveRadiation(longwaveOptics, skyLongwave, kIterations, 1.0f);
                const std::vector<float> longwaveIncident =
                    incidentRadiation(longwaveSolution, longwaveOptics, skyLongwave);
                evaluateEnergyFluxes(longwaveOptics, longwaveIncident);
                measureEnergyResidual(longwaveOptics);
                updateStability(false);
                // Test the same state that will be reported. A pre-update residual
                // cannot certify temperatures/resistances changed later in the step.
                if (energyConverged())
                    break;
                const float stepRelaxation = temperatureRelaxation;

                maximumTemperatureDelta = 0.0f;
                for (int state = 0; state < 2; ++state)
                {
                    std::vector<float> &T = stateTemperature[state];
                    const StateFluxes &flux = stateFlux[state];
                    for (uint32_t facet = 0; facet < scene.facetCount(); ++facet)
                    {
                        const size_t side0 = static_cast<size_t>(facet) * 2U;
                        const bool object = facet < scene.leafFacetCount;
                        for (size_t side = side0; side < side0 + (object ? 1U : 2U); ++side)
                        {
                            const float emissivity =
                                std::clamp(1.0f - longwaveOptics[side].reflectance - longwaveOptics[side].transmittance,
                                           0.0f, 1.0f);
                            const float residual = flux.net[side] + flux.internal[side] + flux.source[side] - flux.sensible[side] - flux.latent[side] -
                                                   flux.storage[side] - (object ? flux.photovoltaic[side] : 0.0f);
                            const float derivative =
                                radiatingFaces(facet) * emissivity * kStefanBoltzmann * std::pow(T[side], 3.0f) +
                                flux.sensibleSlope[side] + flux.latentSlope[side] + flux.storageSlope[side] + flux.internalSlope[side] +
                                (object ? flux.photovoltaicSlope[side] : 0.0f);
                            // 阻尼牛顿更新：ΔT = 残差 / max(1, 斜率)，单步限幅 ±5 K；候选温度限幅在 [220 K, 上限]，
                            // 上限默认 360 K，集总体表面用其 lumpedMaxSurfaceK。叶片直接采用联立求解的 temperatureStep，
                            // 其余按 temperatureRelaxation 松弛。物体面元两侧同温；地面朝下一侧复制朝上一侧。
                            const float maximumSurfaceK=lumpedFacet[facet]>=0
                                ? float(surfaceForFacet(scene,facet).lumpedMaxSurfaceK) : 360.0f;
                            const float candidate =
                                std::clamp(T[side] + std::clamp(residual / std::max(1.0f, derivative), -5.0f, 5.0f),
                                           220.0f, maximumSurfaceK);
                            const float updated = T[side] + (object && surfaceForFacet(scene, facet).vegetation
                                                                 ? flux.temperatureStep[side]
                                                                 : stepRelaxation * (candidate - T[side]));
                            maximumTemperatureDelta = std::max(maximumTemperatureDelta, std::abs(updated - T[side]));
                            T[side] = updated;
                            if (object)
                                T[side0 + 1U] = updated;
                        }
                        if (!object) {
                            const size_t down = side0 + groundDownSide(facet);
                            T[down] = T[down ^ 1U];
                        }
                    }
                }
                updateMeanTemperature();
                updateStability();
                couplingIterations = coupling + 1U;
            }

            finalLongwaveOptics = makeEnergyLongwaveOptics(scene, stateTemperature, sunlitPart);
            finalLongwaveSolution = solveRadiation(finalLongwaveOptics, skyLongwave, kIterations, 1.0f);
            finalLongwaveIncident = incidentRadiation(finalLongwaveSolution, finalLongwaveOptics, skyLongwave);
            evaluateEnergyFluxes(finalLongwaveOptics, finalLongwaveIncident);
            measureEnergyResidual(finalLongwaveOptics);
            updateStability(false);
        }
        // 步骤 F：本节点耦合结束后的诊断与闭合检查。converged 为最终状态的收敛判定；
        // diagnostic 汇总迭代信息、稳定度、气孔失配和下边界辐射，随后检查土壤/物体热量闭合。
        const bool converged = energyConverged();
        couplingDone = Clock::now();
        allNodesConverged = allNodesConverged && converged;
        ProjectJson::Json diagnostic{
            {"node", node}, {"converged", converged}, {"stabilityCorrection", stabilityCorrection},
            {"energyFailingFacets", failingEnergyFacets}, {"energyTotalFacets", scene.facetCount()},
            {"energyFailingFacetFraction", double(failingEnergyFacets) / scene.facetCount()},
            {"energyResidualThreshold_W_m2", facetphysics::facetEnergyResidualThreshold},
            {"maximumStabilityChange", maximumStabilityChange}, {"stomataFrozen", false},
            {"maximumStomatalLogMismatch", maximumStomatalMismatch}, {"maximumLeafEnergyResidual_W_m2", maximumLeafEnergyResidual},
            {"soilLayerEdges_m", facetphysics::soilEdges}, {"soilBottomBoundary", "fixed initial temperature at 3 m"},
            {"columns", ProjectJson::Json::array()}, {"soilProfiles", ProjectJson::Json::array()}};
        diagnostic["lowerBoundary"] = groundEscapeJson(graph);
        diagnostic["lowerBoundary"]["longwaveGroundRadiosity_W_m2"] = finalLongwaveSolution.groundRadiosity;
        diagnostic["lowerBoundary"]["skyLongwave_W_m2"] = skyLongwave;
        for (size_t i = 0; i < columns.size(); ++i)
            if (saveEnergyProcess) diagnostic["columns"].push_back({{"index", i}, {"inverseObukhovLength_m-1", columnInverseL[i]},
                {"leafResistance_s_m", columnAerodynamics[i].leafResistance}, {"soilResistance_s_m", columnAerodynamics[i].soilResistance}});
        // 步骤 G：土壤热量闭合检查（仅 soilTemperatureMethod == 2 的朝上地面侧；仅诊断，但有副作用）：
        // 对受光/阴影两态按受光比例加权，求表面通量 topFlux、底部通量 bottomFlux 与层储热 storage，
        // 闭合残差 = topFlux - bottomFlux - storage；同时把新土壤剖面写回 soilProfiles、
        // 把土柱热含量写入 previousSoilHeat，供下一节点使用。
        double maxSoilClosure = 0;
        // Heat-content closure across nodes (W m-2): change of the stored heat
        // of the whole surface, sum over states of p*C*T, from the end of the
        // preceding node minus the conducted/storage flux of this node. Zero
        // when the sunlit/shaded transfer conserves heat; equal to the legacy
        // jump / dt otherwise.
        double soilClosureArea = 0, soilClosureSum = 0, soilClosureAbs = 0, soilClosureMax = 0;
        // Area-weighted soil-column summary (method 2, upward side of the ground): conducted
        // flux into the column at the surface and out of it at the bottom, column heat, and
        // the bottom-layer temperature, for the multi-day drift/closure check.
        double columnArea = 0, columnTop = 0, columnBottom = 0, columnHeat = 0, columnCapacity = 0,
            columnLastLayer = 0, columnSurface = 0;
        ProjectJson::Json soilColumnSummary;
        double objectClosureArea = 0, objectClosureSum = 0, objectClosureAbs = 0, objectClosureMax = 0;
        const bool closureAvailable = node != startNode;
        for (size_t g = 0; g < groundSides; ++g) {
            const size_t side = firstGroundSide + g;
            const auto& soil = surfaceForFacet(scene, static_cast<uint32_t>(side / 2));
            if (soil.soilTemperatureMethod != 2) continue;
            if (side % 2U == groundDownSide(static_cast<uint32_t>(side / 2))) continue;
            double conducted = 0, heat = 0, topSum = 0, bottomSum = 0, lastSum = 0, surfaceSum = 0;
            for (int state = 0; state < 2; ++state) {
                const double weight = state == 0 ? sunlitPart[side] : 1.0 - sunlitPart[side];
                const auto next = soilResponses[state][g].profile(stateTemperature[state][side]);
                const double topFlux = soilResponses[state][g].flux(stateTemperature[state][side]);
                const double bottomFlux = soil.thermalConductivity * (next.back() - soilBottomTemperature[g]) /
                    (.5 * (facetphysics::soilEdges.back() - facetphysics::soilEdges[facetphysics::soilLayers - 1]));
                conducted += weight * (topFlux - bottomFlux);
                heat += weight * soilColumnHeat(soil, next);
                topSum += weight * topFlux; bottomSum += weight * bottomFlux;
                lastSum += weight * next.back(); surfaceSum += weight * stateTemperature[state][side];
                double storage = 0;
                for (size_t layer = 0; layer < facetphysics::soilLayers; ++layer)
                    storage += soil.specificHeat * soil.density * (facetphysics::soilEdges[layer + 1] - facetphysics::soilEdges[layer]) *
                        (next[layer] - soilProfiles[state][g][layer]) / dTime;
                maxSoilClosure = std::max(maxSoilClosure, std::abs(topFlux - bottomFlux - storage));
                if (saveEnergyProcess) diagnostic["soilProfiles"].push_back({{"side", side}, {"state", state},
                    {"temperature_K", next}, {"surfaceTemperature_K", stateTemperature[state][side]},
                    {"bottomTemperature_K", soilBottomTemperature[g]}, {"surfaceFlux_W_m2", topFlux},
                    {"bottomFlux_W_m2", bottomFlux}, {"storage_W_m2", storage}});
                soilProfiles[state][g] = next;
            }
            if (closureAvailable) {
                const double area = facetAreas[side / 2];
                const double residual = (heat - previousSoilHeat[g]) / dTime - conducted;
                soilClosureArea += area; soilClosureSum += area * residual; soilClosureAbs += area * std::abs(residual);
                soilClosureMax = std::max(soilClosureMax, std::abs(residual));
            }
            previousSoilHeat[g] = heat;
            {
                const double area = facetAreas[side / 2];
                columnArea += area; columnTop += area * topSum; columnBottom += area * bottomSum;
                columnHeat += area * heat; columnLastLayer += area * lastSum; columnSurface += area * surfaceSum;
                columnCapacity += area * soil.specificHeat * soil.density * facetphysics::soilEdges.back();
            }
        }
        // 土壤柱按面积加权的汇总（表面/底部通量、柱热量、平均温度），用于多日漂移与闭合检查。
        if (columnArea > 0) soilColumnSummary = {{"area_m2", columnArea},
            {"meanSurfaceTemperature_K", columnSurface / columnArea},
            {"meanSurfaceFlux_W_m2", columnTop / columnArea}, {"meanBottomFlux_W_m2", columnBottom / columnArea},
            {"meanColumnHeat_J_m2", columnHeat / columnArea},
            {"meanColumnTemperature_K", columnHeat / columnCapacity},
            {"meanBottomLayerTemperature_K", columnLastLayer / columnArea}};
        diagnostic["maximumSoilClosure_W_m2"] = maxSoilClosure;
        // 物体面元（墙体除外）的热含量闭合：热含量 = 单位面积热容 × 受光/阴影加权温度，
        // 残差 = Δ热含量/dTime - 储热通量；同时更新 previousObjectHeat（有副作用）。
        for (uint32_t facet = 0; facet < scene.leafFacetCount; ++facet) {
            if (wallIndex[facet] >= 0) continue; // slab heat: buildingWalls closure below
            const size_t side = static_cast<size_t>(facet) * 2U;
            const double capacity = std::max(0.0f, surfaceForFacet(scene, facet).heatCapacityPerArea);
            const double heat = capacity * (sunlitPart[side] * static_cast<double>(stateTemperature[0][side]) +
                (1.0 - sunlitPart[side]) * static_cast<double>(stateTemperature[1][side]));
            if (closureAvailable && capacity > 0) {
                const double area = facetAreas[facet];
                const double residual = (heat - previousObjectHeat[facet]) / dTime - storageHeat[side];
                objectClosureArea += area; objectClosureSum += area * residual; objectClosureAbs += area * std::abs(residual);
                objectClosureMax = std::max(objectClosureMax, std::abs(residual));
            }
            previousObjectHeat[facet] = heat;
        }
        // 步骤 H：车辆座舱（及发动机舱）的隐式热平衡，在每个节点的表面温度解出后进行。
        // 流程：各车辆累计透射太阳功率 → 把壁板响应对室内温度线性化 → 解座舱空气/内饰
        // （部件模型再含发动机块与舱内空气）→ 用新的室内温度推进壁板剖面。方程与参数见 docs/vehicle-thermal.md。
        // Vehicle cabins: implicit lumped air + interior-mass step. The wall inner
        // heat flux is linear in the cabin air temperature (backward Euler
        // response), q(Ta) = q0 - K (Ta - Ta0), so the walls are coupled to the
        // cabin without iterating the surface solution; the slab profiles are then
        // advanced with the new cabin temperature (exact energy closure per slab).
        // 模块：一辆车在本节点的座舱计算数据。active 表示有壁板参与；parts 表示使用部件分建模。
        // qTrans 透过车窗的太阳功率（W）；qSolar 内饰吸收的太阳功率；qOcc 乘员代谢；
        // qWall0 / kWall 为壁板向室内传热的线性响应 q = q0 - K(T - Tref)（K 单位 W K-1）；
        // gVent 通风导热（W K-1）；ach 换气次数（h-1）；qAc 空调制冷功率（W）；airCap / interiorCap 热容（J K-1）；
        // *Old 为上一节点温度。其余为部件模型的节点数据（内饰 4 节点、发动机块与发动机舱）。
        struct CabinNode {
            bool active{false}, parts{false};
            double airOld{}, interiorOld{}, outdoor{}, qTrans{}, qSolar{}, qWall0{}, kWall{}, qOcc{}, gVent{}, ach{}, qAc{}, airCap{}, interiorCap{};
            // Parts model: node data of the step (interior nodes k = 0..3, air, engine block and bay).
            int m{0};
            double phi{0}, qFirewall{0}, gBay{0}, qEngine{0}, qBlock{0}, qRadiator{0}, qHood0{0}, kHood{0}, bayOld{0}, blockOld{0};
            std::array<double, 4> intOld{}, share{}, cap{}, hcA{}, w{}, qIn{};
            std::map<std::string, double> window;  // transmitted power per window group (W)
        };
        std::vector<CabinNode> cabinNodes(vehicles.size());
        std::vector<double> wallInnerK(wallFacets.size(), 0.0), wallOuterK(wallFacets.size(), 0.0), wallIndoorQ(wallFacets.size(), 0.0);
        for (size_t vi = 0; vi < vehicles.size(); ++vi) {
            auto& v = vehicles[vi];
            if (v.walls.empty()) continue;
            const auto& b = *v.body;
            CabinNode& c = cabinNodes[vi];
            c.active = true;
            c.airOld = v.airK;
            c.interiorOld = v.interiorK;
            c.outdoor = airTemperature;
            // 壁板线性化用的室内参考温度：部件模型取壳体边界温度 boundaryK，否则取座舱空气温度。
            const double indoorRef = v.parts ? v.boundaryK : v.airK;
            // 步骤 1：累计透过车窗的太阳功率 qTrans（W）= Σ 面元面积 × 透射短波（取室外侧）；
            // 部件模型再按车窗网格组分别累计，用于逐窗输出。
            for (uint32_t f : v.glassFacets) {
                const int32_t gw = wallIndex[f];
                const size_t gside = size_t(f) * 2U + (gw >= 0 ? wallOuter[size_t(gw)] : 0U);
                const double watts = double(facetAreas[f]) * transmittedShortwave[gside];
                c.qTrans += watts;
                if (v.parts) c.window[surfaceForFacet(scene, f).meshName] += watts;
            }
            // 步骤 2：壁板线性化。对每块壁板把室内温度加 1 K 重新求一次壁板响应，得到向室内传热 q 对
            // 室内温度的斜率 K = G·(1 - dT_inner/dT_room)；受光/阴影两态按受光比例加权。
            // 这样壁板与座舱耦合时不需要迭代表面解。
            // Linear response of the slabs: q(T) = q0 - K (T - T_ref) for the cabin walls (T_ref = indoorRef)
            // and the bonnet slabs (T_ref = bay air).
            const auto linearize = [&](size_t w, double reference, double& q0, double& k) {
                const uint32_t f = wallFacets[w];
                const size_t side = size_t(f) * 2U;
                auto slab1 = slabOf(w);
                const double conductance = slab1.indoorConductance();
                slab1.indoorK += 1.0;
                const size_t last = slab1.cells() - 1;
                for (int state = 0; state < 2; ++state) {
                    const double weight = state == 0 ? sunlitPart[side] : 1.0 - sunlitPart[side];
                    if (!(weight > 0)) continue;
                    const double surface = stateTemperature[state][side];
                    const auto& r0 = wallResponses[state][w];
                    const double t0 = r0.offset[last] + r0.slope[last] * surface;
                    const auto r1 = facetphysics::wallResponse(wallProfiles[state][w], slab1, dTime,
                        internalHeat.empty() ? 0.0 : double(internalHeat[f]));
                    const double t1 = r1.offset[last] + r1.slope[last] * surface;
                    q0 += weight * facetAreas[f] * conductance * (t0 - reference);
                    k += weight * facetAreas[f] * conductance * (1.0 - (t1 - t0));
                }
            };
            for (size_t w : v.walls) linearize(w, indoorRef, c.qWall0, c.kWall);
            // 步骤 3：座舱热容与通风。airCap = ρ·cp·V（J K-1）；ACH 取设定通风与开窗缝换气中较大者；
            // 行驶时 ACH 不低于 clamp(8 + 1.2·v, 10, 30)；开空调时取内循环，ACH 上限 5；
            // gVent = airCap·ACH/3600（W K-1）；qSolar = 内饰吸收比例 × 透射太阳；qOcc = 人数 × 人均代谢。
            c.airCap = double(airDensity) * 1005.0 * b.cabinVolume;
            c.interiorCap = b.interiorCapacity;
            c.ach = b.windowCracked ? std::max(b.ventilationAch, b.crackedAch) : b.ventilationAch;
            if (b.vehicleSpeed > 0.0f) c.ach = std::max(c.ach, std::clamp(8.0 + 1.2 * double(b.vehicleSpeed), 10.0, 30.0));
            if (b.acOn) c.ach = std::min(c.ach, 5.0);
            c.gVent = c.airCap * c.ach / 3600.0;
            c.qSolar = b.absorbedFraction * c.qTrans;
            c.qOcc = double(b.occupants) * b.metabolicW;
            // 步骤 4a：部件分建模。未知数 x = [T_air, 内饰节点 k = 0..m-1, (T_block, T_bay)]；
            // 向后欧拉，稠密线性方程组（n <= 7）用部分主元高斯消元求解；phi 为壳体辐射份额，
            // 把壁板得热的一部分分配给内饰节点而不是空气。求解后写回车辆状态并推进壁板剖面。
            if (v.parts) {
                // Parts model: unknowns x = [T_air, T_interior k..., (T_block, T_bay)]; backward Euler, dense solve (n <= 7).
                c.parts = true;
                const bool engine = b.engineMulti;
                const int m = b.interiorMulti ? 4 : 1;
                c.m = m;
                double radSum = 0;
                for (int k = 0; k < m; ++k) {
                    c.share[k] = b.interiorMulti ? b.intShare[k] : 1.0;
                    c.cap[k] = b.interiorMulti ? b.intCapacity[k] : b.interiorCapacity;
                    c.hcA[k] = b.interiorMulti ? b.intHcA[k] : b.interiorH * b.interiorArea;
                    c.w[k] = b.interiorMulti ? b.intRadArea[k] : 0.0;
                    c.intOld[k] = v.intK[k];
                    radSum += c.w[k];
                }
                c.phi = b.interiorMulti && radSum > 0 ? b.shellRadFraction : 0.0;
                for (int k = 0; k < m; ++k) c.w[k] = radSum > 0 ? c.w[k] / radSum : 0.0;
                for (size_t w : v.hoodWalls) linearize(w, v.bayK, c.qHood0, c.kHood);
                c.bayOld = v.bayK;
                c.blockOld = v.blockK;
                const int n = 1 + m + (engine ? 2 : 0), iBlock = 1 + m, iBay = 2 + m;
                c.qEngine = engine && b.engineOn ? b.enginePowerW * b.engineShare : 0.0;
                c.qBlock = c.qEngine * b.engineSplit[1];
                c.qRadiator = c.qEngine * b.engineSplit[0] + (v.underArea > 0 ? 0.0 : c.qEngine * b.engineSplit[2]);
                c.gBay = engine ? double(airDensity) * 1005.0 * (b.bayVent0 + b.bayVentK * double(b.vehicleSpeed)) : 0.0;
                const double phi = c.phi, storeA = c.airCap / dTime, Tref = indoorRef;
                // 作用：组装并求解部件模型的线性方程组。行 0 为空气节点，行 1..m 为内饰节点，
                // iBlock / iBay 为发动机块与发动机舱空气；acGain·(acSet - T_air) 为比例空调项，
                // acFixed 为定功率制冷（W）。返回 x（单位 K）。
                const auto solve = [&](double acGain, double acSet, double acFixed) {
                    std::array<std::array<double, 8>, 8> a{};
                    std::array<double, 8> rhs{};
                    // Air row.
                    double hcSum = 0;
                    for (int k = 0; k < m; ++k) hcSum += c.hcA[k];
                    a[0][0] = storeA + hcSum + (1 - phi) * (1 - phi) * c.kWall + c.gVent + acGain + (engine ? b.firewallG : 0.0);
                    rhs[0] = storeA * v.airK + (1 - phi) * (c.qWall0 + c.kWall * Tref) + c.qOcc + c.gVent * airTemperature + acGain * acSet - acFixed;
                    for (int k = 0; k < m; ++k) a[0][1 + k] = -c.hcA[k] + (1 - phi) * c.kWall * phi * c.w[k];
                    if (engine) a[0][iBay] = -b.firewallG;
                    // Interior rows.
                    for (int k = 0; k < m; ++k) {
                        const double sk = c.cap[k] / dTime, wk = phi * c.w[k];
                        a[1 + k][0] = -c.hcA[k] + wk * c.kWall * (1 - phi);
                        for (int j = 0; j < m; ++j) a[1 + k][1 + j] = wk * c.kWall * phi * c.w[j];
                        a[1 + k][1 + k] += sk + c.hcA[k];
                        rhs[1 + k] = sk * v.intK[k] + c.share[k] * c.qSolar + wk * (c.qWall0 + c.kWall * Tref);
                    }
                    if (engine) {
                        const double sE = b.engineBlockCapacity / dTime, sB = b.engineBayCapacity / dTime;
                        a[iBlock][iBlock] = sE + b.engineBlockBay;
                        a[iBlock][iBay] = -b.engineBlockBay;
                        rhs[iBlock] = sE * v.blockK + c.qBlock;
                        a[iBay][iBay] = sB + b.engineBlockBay + c.gBay + b.firewallG + c.kHood;
                        a[iBay][iBlock] = -b.engineBlockBay;
                        a[iBay][0] = -b.firewallG;
                        rhs[iBay] = sB * v.bayK + c.qRadiator + c.gBay * airTemperature + c.qHood0 + c.kHood * c.bayOld;
                    }
                    for (int i = 0; i < n; ++i) {            // Gaussian elimination, partial pivoting
                        int p = i;
                        for (int r = i + 1; r < n; ++r) if (std::abs(a[r][i]) > std::abs(a[p][i])) p = r;
                        if (!(std::abs(a[p][i]) > 0)) throw std::runtime_error("Singular vehicle cabin system");
                        std::swap(a[i], a[p]); std::swap(rhs[i], rhs[p]);
                        for (int r = i + 1; r < n; ++r) {
                            const double factor = a[r][i] / a[i][i];
                            for (int cc = i; cc < n; ++cc) a[r][cc] -= factor * a[i][cc];
                            rhs[r] -= factor * rhs[i];
                        }
                    }
                    std::array<double, 8> x{};
                    for (int i = n - 1; i >= 0; --i) {
                        double s = rhs[i];
                        for (int cc = i + 1; cc < n; ++cc) s -= a[i][cc] * x[cc];
                        x[i] = s / a[i][i];
                    }
                    return x;
                };
                // 空调逻辑：先按无空调求解；若座舱空气高于设定温度 setpoint，先试比例控制
                // （增益 acPower/acBand），若结果仍高于 setpoint + acBand 则改为满功率 acPower；
                // qAc 记录实际制冷功率（W）。
                auto x = solve(0, 0, 0);
                if (b.acOn && b.acPower > 0) {
                    const double setpoint = b.acSetpointC + 273.15;
                    if (x[0] > setpoint) {
                        const double gain = b.acPower / b.acBandK;
                        const auto proportional = solve(gain, setpoint, 0);
                        if (proportional[0] <= setpoint + b.acBandK) { x = proportional; c.qAc = gain * (x[0] - setpoint); }
                        else { x = solve(0, 0, b.acPower); c.qAc = b.acPower; }
                    }
                }
                // 写回车辆状态：空气、各内饰节点、壳体边界温度 boundaryK = (1-phi)·T_air + phi·内饰辐射加权平均，
                // 以及发动机块/舱内空气；随后用新的室内边界温度重算各壁板（和引擎盖壁板）的响应。
                v.airK = x[0];
                for (int k = 0; k < m; ++k) v.intK[k] = x[1 + k];
                double radMean = 0;
                for (int k = 0; k < m; ++k) radMean += c.w[k] * v.intK[k];
                v.boundaryK = (1 - phi) * v.airK + phi * radMean;
                v.interiorK = v.intK[0];
                if (engine) { v.blockK = x[iBlock]; v.bayK = x[iBay]; }
                for (size_t w : v.walls) {
                    wallIndoor[w] = v.boundaryK;
                    const auto slab = slabOf(w);
                    for (int state = 0; state < 2; ++state)
                        wallResponses[state][w] = facetphysics::wallResponse(wallProfiles[state][w], slab, dTime,
                            internalHeat.empty() ? 0.0 : double(internalHeat[wallFacets[w]]));
                }
                for (size_t w : v.hoodWalls) {
                    wallIndoor[w] = v.bayK;
                    const auto slab = slabOf(w);
                    for (int state = 0; state < 2; ++state)
                        wallResponses[state][w] = facetphysics::wallResponse(wallProfiles[state][w], slab, dTime,
                            internalHeat.empty() ? 0.0 : double(internalHeat[wallFacets[w]]));
                }
                continue;
            }
            // 步骤 4b：集总座舱（无部件分建模）。内饰为单个热容节点：先对内饰做向后欧拉并消去，
            // 得到只含空气温度的单个方程 lhs·T_air = rhs；空调逻辑同上（比例控制 / 满功率）。
            const double conv = b.interiorH * b.interiorArea, storeI = c.interiorCap / dTime, storeA = c.airCap / dTime;
            const double total = storeI + conv, convEff = conv * storeI / total;
            const double rhs = storeA * v.airK + c.qWall0 + c.kWall * v.airK + c.qOcc + c.gVent * airTemperature +
                conv * (storeI * v.interiorK + c.qSolar) / total;
            const double lhs = storeA + c.kWall + c.gVent + convEff;
            double air = rhs / lhs;
            if (b.acOn && b.acPower > 0) {
                const double setpoint = b.acSetpointC + 273.15;
                if (air > setpoint) {
                    const double gain = b.acPower / b.acBandK;
                    const double proportional = (rhs + gain * setpoint) / (lhs + gain);
                    if (proportional <= setpoint + b.acBandK) { air = proportional; c.qAc = gain * (air - setpoint); }
                    else { air = (rhs - b.acPower) / lhs; c.qAc = b.acPower; }
                }
            }
            v.airK = air;
            v.interiorK = (storeI * c.interiorOld + c.qSolar + conv * air) / total;
            for (size_t w : v.walls) {
                wallIndoor[w] = air;
                const auto slab = slabOf(w);
                for (int state = 0; state < 2; ++state)
                    wallResponses[state][w] = facetphysics::wallResponse(wallProfiles[state][w], slab, dTime,
                        internalHeat.empty() ? 0.0 : double(internalHeat[wallFacets[w]]));
            }
        }
        // 步骤 I：把建筑/车辆壁板（wallFacets）推进到已求解的表面温度。副作用：更新 wallProfiles、
        // previousWallHeat、wallInnerK/OuterK/IndoorQ，并写入 diagnostic["buildingWalls"]（面积加权汇总与热量闭合）。
        if (!wallFacets.empty()) {
            // Advance the slabs to the solved surface temperatures. Closure per
            // facet: change of the area-weighted slab heat since the end of the
            // preceding node minus (surface conduction - indoor flux).
            double area = 0, indoorSum = 0, outerSum = 0, innerSum = 0, closureSum = 0, closureMax = 0;
            for (size_t w = 0; w < wallFacets.size(); ++w) {
                const uint32_t facet = wallFacets[w];
                const auto slab = slabOf(w);
                const size_t side = size_t(facet) * 2U;
                double heat = 0, conducted = 0, indoor = 0, inner = 0, outer = 0;
                for (int state = 0; state < 2; ++state) {
                    const double weight = state == 0 ? sunlitPart[side] : 1.0 - sunlitPart[side];
                    const double surface = stateTemperature[state][side];
                    const auto next = wallResponses[state][w].profile(surface);
                    conducted += weight * wallResponses[state][w].flux(surface);
                    indoor += weight * slab.indoorFlux(next);
                    inner += weight * slab.innerSurfaceK(next);
                    outer += weight * surface;
                    heat += weight * slab.heat(next);
                    wallProfiles[state][w] = next;
                }
                const double residual = (heat - previousWallHeat[w]) / dTime - (conducted - indoor) -
                    (internalHeat.empty() ? 0.0 : double(internalHeat[facet]));
                previousWallHeat[w] = heat;
                wallInnerK[w] = inner;
                wallOuterK[w] = outer;
                wallIndoorQ[w] = indoor;
                area += facetAreas[facet];
                indoorSum += facetAreas[facet] * indoor;
                outerSum += facetAreas[facet] * outer;
                innerSum += facetAreas[facet] * inner;
                closureSum += facetAreas[facet] * residual;
                closureMax = std::max(closureMax, std::abs(residual));
            }
            diagnostic["buildingWalls"] = {{"facets", wallFacets.size()}, {"area_m2", area},
                {"meanOuterSurfaceTemperature_K", outerSum / area}, {"meanInnerSurfaceTemperature_K", innerSum / area},
                {"meanIndoorFlux_W_m2", indoorSum / area},
                {"heatClosureMean_W_m2", closureAvailable ? closureSum / area : 0.0},
                {"heatClosureMaxAbs_W_m2", closureAvailable ? closureMax : 0.0}};
        }
        // 步骤 J：输出车辆座舱诊断。每辆车一行 JSON（温度、能量平衡分项、相对闭合残差），
        // 写入 diagnostics/vehicle_cabin_node=<n>.json 并打印 VEHICLE_PARTS / VEHICLE_CABIN 日志行；
        // 同时更新各车的运行峰值（座舱最高温、内饰、发动机、最大相对闭合）供运行汇总使用。
        if (!vehicles.empty()) {
            // Cabin balance per vehicle (temperatures, no comfort indices):
            //   dE/dt = Q_wall + Q_occ + Q_solar - G_vent (Ta - Tout) - Q_ac.
            ProjectJson::Json rows = ProjectJson::Json::array();
            std::vector<double> wallQ(vehicles.size(), 0.0);
            for (size_t w = 0; w < wallFacets.size(); ++w)
                if (wallVehicle[w] >= 0 && cabinNodes[size_t(wallVehicle[w])].active)
                    wallQ[size_t(wallVehicle[w])] += facetAreas[wallFacets[w]] * wallIndoorQ[w];
            for (size_t vi = 0; vi < vehicles.size(); ++vi) {
                auto& v = vehicles[vi];
                const CabinNode& c = cabinNodes[vi];
                // mean(facets, which)：which = 0 为面元表面温度（受光/阴影加权）；1 / 2 对墙体面元分别取
                // 室外 / 室内表面温度；结果按面积加权，无面元时返回 NaN。
                // Area-weighted surface temperatures of the body / glass / underbody.
                const auto mean = [&](const std::vector<uint32_t>& facets, int which) {
                    double sum = 0, a = 0;
                    for (uint32_t f : facets) {
                        const size_t side = size_t(f) * 2U;
                        double value;
                        if (wallIndex[f] >= 0 && which >= 1) value = which == 1 ? wallOuterK[size_t(wallIndex[f])] : wallInnerK[size_t(wallIndex[f])];
                        else value = sunlitPart[side] * stateTemperature[0][side] + (1.0 - sunlitPart[side]) * stateTemperature[1][side];
                        sum += facetAreas[f] * value;
                        a += facetAreas[f];
                    }
                    return a > 0 ? sum / a : std::numeric_limits<double>::quiet_NaN();
                };
                ProjectJson::Json row{{"name", v.name}, {"node", node}, {"outdoorAirTemperature_K", double(airTemperature)},
                    {"speed_m_s", v.body ? v.body->vehicleSpeed : 0.0f},
                    {"bodyOuterSurface_K", mean(v.bodyFacets, 0)}, {"glassOuterSurface_K", mean(v.glassFacets, 0)},
                    {"underbodySurface_K", mean(v.underFacets, 0)}};
                // 部件模型的输出与闭合：每个内饰节点残差 = 储热 - (太阳吸收 - 对流到空气 + 壳体辐射得热)；
                // 空气残差同理；总残差除以各分项最大绝对值得相对闭合（取历史最大值 maxClosure）。
                // 发动机块 + 舱内空气另算一套闭合 engineResidual。
                if (c.active && c.parts) {
                    // Parts model: air + interior nodes (+ engine block and bay), every node closes separately.
                    const auto& b = *v.body;
                    const int m = c.m;
                    static const char* const names[4] = {"dashboard", "seat", "steeringWheel", "lining"};
                    double hoodQ = 0;
                    for (size_t w : v.hoodWalls) hoodQ += facetAreas[wallFacets[w]] * wallIndoorQ[w];
                    const double airStore = c.airCap * (v.airK - c.airOld) / dTime;
                    const double ventilation = c.gVent * (v.airK - airTemperature);
                    const double firewall = b.engineMulti ? b.firewallG * (v.bayK - v.airK) : 0.0;
                    const double qw = wallQ[vi];
                    double convection = 0, intStore = 0, intResidual = 0, capSum = 0, tempSum = 0, nodeScale = 1.0;
                    ProjectJson::Json nodes = ProjectJson::Json::array();
                    for (int k = 0; k < m; ++k) {
                        const double toAir = c.hcA[k] * (v.intK[k] - v.airK);
                        const double store = c.cap[k] * (v.intK[k] - c.intOld[k]) / dTime;
                        const double solar = c.share[k] * c.qSolar, shell = c.phi * c.w[k] * qw;
                        const double residualK = store - (solar - toAir + shell);
                        convection += toAir; intStore += store; intResidual += residualK;
                        capSum += c.cap[k]; tempSum += c.cap[k] * v.intK[k];
                        nodeScale = std::max({nodeScale, std::abs(solar), std::abs(toAir), std::abs(shell), std::abs(store)});
                        v.maxIntK[k] = std::max(v.maxIntK[k], v.intK[k]);
                        ProjectJson::Json node{{"name", b.interiorMulti ? names[k] : "lumped"}, {"temperature_K", v.intK[k]},
                            {"solarAbsorbed_W", solar}, {"convectionToAir_W", toAir}, {"shellRadiationGain_W", shell},
                            {"storage_W", store}, {"residual_W", residualK}};
                        if (b.interiorMulti && k == 1) {
                            // Seat surface temperature a bare hand/leg would touch: two semi-infinite bodies,
                            // T_c = (e_seat T_seat + e_skin T_skin) / (e_seat + e_skin), e_skin = 1100, T_skin = 34 C.
                            node["contactTemperature_K"] = (b.seatEffusivity * v.intK[k] + 1100.0 * 307.15) / (b.seatEffusivity + 1100.0);
                        }
                        nodes.push_back(std::move(node));
                    }
                    const double airResidual = airStore - (convection + (1.0 - c.phi) * qw + c.qOcc + firewall - ventilation - c.qAc);
                    const double residual = airResidual + intResidual;
                    const double scale = std::max({nodeScale, std::abs(qw), std::abs(c.qOcc), std::abs(ventilation), std::abs(c.qAc), std::abs(airStore), std::abs(firewall)});
                    v.maxClosure = std::max(v.maxClosure, std::abs(residual) / scale);
                    if (v.airK > v.maxAirK) { v.maxAirK = v.airK; v.maxAirNode = node; v.maxAirTime = meteo.julianTime; }
                    v.interiorK = capSum > 0 ? tempSum / capSum : v.intK[0];
                    v.maxInteriorK = std::max(v.maxInteriorK, v.interiorK);
                    double bodyInner = 0, bodyArea = 0, glassInner = 0, glassArea = 0;
                    for (size_t w : v.walls) {
                        const uint32_t f = wallFacets[w];
                        if (surfaceForFacet(scene, f).vehiclePart == 2) { glassInner += facetAreas[f] * wallInnerK[w]; glassArea += facetAreas[f]; }
                        else { bodyInner += facetAreas[f] * wallInnerK[w]; bodyArea += facetAreas[f]; }
                    }
                    row["cabinAirTemperature_K"] = v.airK;
                    row["interiorTemperature_K"] = v.interiorK;
                    row["cabinMinusOutdoor_K"] = v.airK - airTemperature;
                    row["bodyInnerSurface_K"] = bodyArea > 0 ? bodyInner / bodyArea : 0.0;
                    row["glassInnerSurface_K"] = glassArea > 0 ? glassInner / glassArea : 0.0;
                    row["ventilationACH"] = c.ach;
                    row["shellBoundaryTemperature_K"] = v.boundaryK;
                    row["interiorNodes"] = nodes;
                    ProjectJson::Json windows = ProjectJson::Json::object();
                    for (const auto& [name, watts] : c.window) {
                        windows[name] = watts;
                        v.maxWindowW[name] = std::max(v.maxWindowW[name], watts);
                    }
                    row["windowTransmitted_W"] = windows;
                    row["balance_W"] = {{"solarTransmitted", c.qTrans}, {"solarAbsorbedByInterior", c.qSolar},
                        {"wallToCabin", qw}, {"occupants", c.qOcc}, {"ventilationLoss", ventilation}, {"firewallFromBay", firewall},
                        {"airConditioning", c.qAc}, {"airStorage", airStore}, {"interiorStorage", intStore},
                        {"residual", residual}, {"relativeResidual", std::abs(residual) / scale}};
                    if (b.engineMulti) {
                        const double blockStore = b.engineBlockCapacity * (v.blockK - c.blockOld) / dTime;
                        const double bayStore = b.engineBayCapacity * (v.bayK - c.bayOld) / dTime;
                        const double blockToBay = b.engineBlockBay * (v.blockK - v.bayK);
                        const double bayLoss = c.gBay * (v.bayK - airTemperature);
                        const double engineResidual = blockStore + bayStore - (c.qBlock + c.qRadiator - bayLoss - firewall + hoodQ);
                        const double engineScale = std::max({1.0, std::abs(c.qEngine), std::abs(blockToBay), std::abs(bayLoss), std::abs(hoodQ), std::abs(blockStore), std::abs(bayStore)});
                        v.maxEngineClosure = std::max(v.maxEngineClosure, std::abs(engineResidual) / engineScale);
                        v.maxBlockK = std::max(v.maxBlockK, v.blockK);
                        v.maxBayK = std::max(v.maxBayK, v.bayK);
                        row["engine"] = {{"on", b.engineOn}, {"blockTemperature_K", v.blockK}, {"bayAirTemperature_K", v.bayK},
                            {"hoodOuterSurface_K", mean(v.hoodFacets, 0)}, {"hoodInnerSurface_K", mean(v.hoodFacets, 2)},
                            {"balance_W", {{"engineHeat", c.qEngine}, {"toBlock", c.qBlock}, {"toBayAir", c.qRadiator},
                                {"toExhaust", c.qEngine * b.engineSplit[2]}, {"blockToBay", blockToBay}, {"bayVentilationLoss", bayLoss},
                                {"firewallToCabin", firewall}, {"hoodFromSlabs", hoodQ}, {"blockStorage", blockStore}, {"bayStorage", bayStore},
                                {"residual", engineResidual}, {"relativeResidual", std::abs(engineResidual) / engineScale}}}};
                    }
                    std::cout << "VEHICLE_PARTS\tnode=" << node << "\tname=" << v.name << "\tcabinC=" << v.airK - 273.15;
                    for (int k = 0; k < m; ++k) std::cout << "\t" << (b.interiorMulti ? names[k] : "lumped") << "C=" << v.intK[k] - 273.15;
                    if (b.engineMulti) std::cout << "\tblockC=" << v.blockK - 273.15 << "\tbayC=" << v.bayK - 273.15;
                    std::cout << "\tqTrans=" << c.qTrans << std::endl;
                } else if (c.active) {
                    const double airStore = c.airCap * (v.airK - c.airOld) / dTime;
                    // 集总座舱的闭合：残差 = 空气与内饰储热 - (壁板 + 乘员 + 太阳 - 通风 - 空调)。
                    const double interiorStore = c.interiorCap * (v.interiorK - c.interiorOld) / dTime;
                    const double ventilation = c.gVent * (v.airK - airTemperature);
                    const double inputs = wallQ[vi] + c.qOcc + c.qSolar;
                    const double residual = airStore + interiorStore - (inputs - ventilation - c.qAc);
                    const double scale = std::max({1.0, std::abs(inputs), std::abs(ventilation), std::abs(c.qAc), std::abs(airStore + interiorStore)});
                    v.maxClosure = std::max(v.maxClosure, std::abs(residual) / scale);
                    if (v.airK > v.maxAirK) { v.maxAirK = v.airK; v.maxAirNode = node; v.maxAirTime = meteo.julianTime; }
                    v.maxInteriorK = std::max(v.maxInteriorK, v.interiorK);
                    double bodyInner = 0, bodyArea = 0, glassInner = 0, glassArea = 0;
                    for (size_t w : v.walls) {
                        const uint32_t f = wallFacets[w];
                        if (surfaceForFacet(scene, f).vehiclePart == 2) { glassInner += facetAreas[f] * wallInnerK[w]; glassArea += facetAreas[f]; }
                        else { bodyInner += facetAreas[f] * wallInnerK[w]; bodyArea += facetAreas[f]; }
                    }
                    row["cabinAirTemperature_K"] = v.airK;
                    row["interiorTemperature_K"] = v.interiorK;
                    row["cabinMinusOutdoor_K"] = v.airK - airTemperature;
                    row["bodyInnerSurface_K"] = bodyArea > 0 ? bodyInner / bodyArea : 0.0;
                    row["glassInnerSurface_K"] = glassArea > 0 ? glassInner / glassArea : 0.0;
                    row["ventilationACH"] = c.ach;
                    row["balance_W"] = {{"solarTransmitted", c.qTrans}, {"solarAbsorbedByInterior", c.qSolar},
                        {"wallToCabin", wallQ[vi]}, {"occupants", c.qOcc}, {"ventilationLoss", ventilation},
                        {"airConditioning", c.qAc}, {"airStorage", airStore}, {"interiorStorage", interiorStore},
                        {"residual", residual}, {"relativeResidual", std::abs(residual) / scale}};
                    std::cout << "VEHICLE_CABIN\tnode=" << node << "\tname=" << v.name << "\tcabinC=" << v.airK - 273.15
                              << "\tinteriorC=" << v.interiorK - 273.15 << "\toutdoorC=" << airTemperature - 273.15
                              << "\tqTrans=" << c.qTrans << "\tacW=" << c.qAc << std::endl;
                }
                rows.push_back(std::move(row));
            }
            diagnostic["vehicleCabin"] = rows;
            std::ofstream(outputDirectory / "diagnostics" / ("vehicle_cabin_node=" + std::to_string(node) + ".json")) << rows.dump(2);
            // 运行汇总：每辆车的最高座舱空气温度及其节点/时间、最高内饰温度、最大相对闭合；
            // 部件模型再含各内饰节点、各车窗透射、发动机块与舱内空气的峰值。
            ProjectJson::Json summary = ProjectJson::Json::array();
            for (const auto& v : vehicles) {
                summary.push_back({{"name", v.name}, {"maxCabinAirTemperature_C", v.maxAirK - 273.15}, {"maxCabinNode", v.maxAirNode},
                    {"maxCabinTime", v.maxAirTime}, {"maxInteriorTemperature_C", v.maxInteriorK - 273.15},
                    {"maxRelativeClosure", v.maxClosure}});
                if (v.parts) {
                    auto& entry = summary.back();
                    const int m = v.body->interiorMulti ? 4 : 1;
                    ProjectJson::Json maxima = ProjectJson::Json::array();
                    for (int k = 0; k < m; ++k) maxima.push_back(v.maxIntK[k] - 273.15);
                    entry["maxInteriorNodeTemperature_C"] = maxima;
                    ProjectJson::Json windows = ProjectJson::Json::object();
                    for (const auto& [name, watts] : v.maxWindowW) windows[name] = watts;
                    entry["maxWindowTransmitted_W"] = windows;
                    if (v.body->engineMulti) {
                        entry["maxEngineBlockTemperature_C"] = v.maxBlockK - 273.15;
                        entry["maxEngineBayTemperature_C"] = v.maxBayK - 273.15;
                        entry["maxEngineRelativeClosure"] = v.maxEngineClosure;
                    }
                }
            }
            runIndex.value["vehicleCabinSummary"] = summary;
            runIndex.write();
        }
        // 步骤 K：船体向水体的热反馈（滞后一个节点）：本节点湿润船体面元传给水体的热量，
        // 按水面面积摊成下一节点的水面热源 waterFeedbackSource（W m-2）。
        if (anyWaterFeedback && nodeWaterK >= 0) {
            // Heat the wetted hull gives the water this node (W): area-weighted
            // water-side flux of the solved surface temperatures, spread over the
            // water facets as the source of the next node (lagged one node).
            double heatToWater = 0;
            for (uint32_t facet = 0; facet < scene.leafFacetCount; ++facet) {
                if (!submergedFacet[facet]) continue;
                const auto& surface = surfaceForFacet(scene, facet);
                if (!surface.waterFeedback) continue;
                const size_t side = size_t(facet) * 2U;
                const double surfaceK = sunlitPart[side] * double(stateTemperature[0][side]) +
                    (1.0 - sunlitPart[side]) * double(stateTemperature[1][side]);
                heatToWater += facetAreas[facet] * wetFraction[facet] * surface.waterCoefficient * (surfaceK - nodeWaterK);
            }
            waterFeedbackSource = static_cast<float>(heatToWater / sceneWaterArea);
            diagnostic["shipWater"] = {{"heatToWater_W", heatToWater}, {"waterSource_W_m2", heatToWater / sceneWaterArea}};
        }
        {
            // 热历史与热量闭合诊断：thermalHistoryMode、受光/阴影状态转移的状态温差、legacy* 为旧算法
            // （状态切换时热量不守恒）下的热量跳变（J，仅作对照）；object/soil 闭合给出面积、均值、
            // 平均绝对值、最大绝对值（W m-2），均来自上面的步骤 G。
            const auto perArea = [](double value, double area) { return area > 0 ? value / area : 0.0; };
            diagnostic["thermalHistory"] = {
                {"mode", thermalHistoryMode}, {"changedSides", historyChangedSides},
                {"maximumStateGapOfChangedSides_K", historyMaximumGap},
                {"legacyObjectHeatJump_J", legacyObjectJump}, {"legacyObjectHeatJumpAbs_J", legacyObjectJumpAbs},
                {"legacySoilHeatJump_J", legacySoilJump}, {"legacySoilHeatJumpAbs_J", legacySoilJumpAbs},
                {"objectHeatClosureArea_m2", objectClosureArea},
                {"objectHeatClosureMean_W_m2", perArea(objectClosureSum, objectClosureArea)},
                {"objectHeatClosureMeanAbs_W_m2", perArea(objectClosureAbs, objectClosureArea)},
                {"objectHeatClosureMaxAbs_W_m2", objectClosureMax},
                {"soilHeatClosureArea_m2", soilClosureArea},
                {"soilHeatClosureMean_W_m2", perArea(soilClosureSum, soilClosureArea)},
                {"soilHeatClosureMeanAbs_W_m2", perArea(soilClosureAbs, soilClosureArea)},
                {"soilHeatClosureMaxAbs_W_m2", soilClosureMax}};
        }
        // 保存本节点的受光比例，下一节点据此做受光/阴影状态之间的温度与热量转移。
        previousSunlitPart = sunlitPart;
        // 集总热体（lumpedBodies）诊断：核心温度、输入功率、传到表皮的功率、空气损失、储热和核心残差；
        // 并比较“核心导出的热量 conductedW”与“各表皮面元收到的热量 exchanged”，得最大失配。
        // 最后把 diagnostic 写入 diagnostics/facet_physics_node=<n>.json。
        diagnostic["timeStepSeconds"]=dTime;
        diagnostic["elapsedSeconds"]=lumpedElapsedSeconds;
        diagnostic["lumpedThermal"]=ProjectJson::Json::array();
        double maxCoreResidual=0,maxExchangeMismatch=0;
        std::vector<double> exchanged(lumpedBodies.size(),0);
        for(uint32_t f=0;f<scene.facetCount();++f)if(lumpedFacet[f]>=0) {
            const size_t side=size_t(f)*2;const double sun=sunlitPart[side];
            exchanged[size_t(lumpedFacet[f])]+=facetAreas[f]*(sun*stateFlux[0].internal[side]+(1-sun)*stateFlux[1].internal[side]);
        }
        for(size_t i=0;i<lumpedBodies.size();++i) {
            const auto& b=lumpedBodies[i];const auto& s=b.step;
            maxCoreResidual=std::max(maxCoreResidual,std::abs(s.residualW));
            maxExchangeMismatch=std::max(maxExchangeMismatch,std::abs(s.conductedW-exchanged[i]));
            diagnostic["lumpedThermal"].push_back({{"id",b.id},{"coreTemperature_K",s.temperatureK},
                {"inputPower_W",s.powerW},{"conductedToSkin_W",s.conductedW},{"surfaceReceived_W",exchanged[i]},
                {"airLoss_W",s.airLossW},{"storage_W",s.storageW},{"coreResidual_W",s.residualW}});
        }
        diagnostic["lumpedThermalMaxCoreResidual_W"]=maxCoreResidual;
        diagnostic["lumpedThermalMaxSurfaceResidual_W_m2"]=lumpedSurfaceResidual;
        diagnostic["lumpedThermalMaxExchangeMismatch_W"]=maxExchangeMismatch;
        std::filesystem::create_directories(outputDirectory / "diagnostics");
        std::ofstream(outputDirectory / "diagnostics" / ("facet_physics_node=" + std::to_string(node) + ".json")) << diagnostic.dump(2);


        // 步骤 L：光伏输出。逐光伏面元写 photovoltaic_node_<n>.csv（面积 m2、温度 °C、入射/吸收/发电/
        // 残差/潜热/有效入射/净长波/显热/储热，W m-2），并累计面积加权汇总写入 photovoltaic_summary.csv；
        // pvEnergyKwh 累计发电量 = Σ 功率(W) × dTime / 3.6e6（kWh）。残差 = 净辐射 - 显热 - 潜热 - 储热 - 发电。
        std::ofstream pvCsv(outputDirectory/("photovoltaic_node_"+std::to_string(node)+".csv"));
        pvCsv << "area_m2,temperature_C,incident_W_m2,absorbed_W_m2,electric_W_m2,residual_W_m2,latent_W_m2,effective_W_m2,net_longwave_W_m2,sensible_W_m2,storage_W_m2,facet_index\n";
        double pa=0,pt=0,pg=0,pabs=0,pp=0,plw=0,ph=0,ps=0,pres=0;
        for(uint32_t f=0;f<scene.leafFacetCount;++f){
            const auto& mat=surfaceForFacet(scene,f);if(!mat.photovoltaic)continue;
            const size_t k=2U*f;const float a=facetArea(scene,f),ab=absorbedShortwave[k]+absorbedShortwave[k+1];
            const float inc=incidentShortwave[k]+incidentShortwave[k+1],power=photovoltaicPower[k];
            const float lw=netRadiation[k]-ab,res=netRadiation[k]-sensibleHeat[k]-latentHeat[k]-storageHeat[k]-power;
            pvCsv << a << ',' << temperature[k]-273.15f << ',' << inc << ',' << ab << ',' << power << ',' << res << ',' << latentHeat[k] << ','
                  << incidentShortwave[k]+mat.pvBifaciality*incidentShortwave[k+1] << ',' << lw << ',' << sensibleHeat[k] << ',' << storageHeat[k] << ',' << f << '\n';
            pa+=a;pt+=a*(temperature[k]-273.15f);pg+=a*inc;pabs+=a*ab;pp+=a*power;plw+=a*lw;ph+=a*sensibleHeat[k];ps+=a*storageHeat[k];pres=std::max(pres,double(std::abs(res)));
        }
        if(pa>0){pvEnergyKwh+=pp*dTime/3600000.;pvSummary << node << ',' << token << ',' << pa << ',' << pt/pa << ',' << pg << ',' << pabs << ',' << pp << ',' << plw << ',' << ph << ',' << ps << ',' << pres << ',' << pvEnergyKwh << '\n';}
        // 合并短波与长波辐射度（逐侧相加）为总辐射度，供输出产品与最终节点结果使用。
        const facetvk::SolveResult radiativeSolution =
            combineRadiativeSolutions(
                shortwaveSolution, finalLongwaveSolution);
        // 步骤 M：成像波段（传感器产品）。对 radiativeParameters.wavelengths 的每个波段独立求解，
        // 不使用能量平衡的宽带结果：光学波段用分谱短波辐照，热红外波段用 Planck 天空辐射 +
        // 2.5–6 um 的反射太阳；每个面侧输出（受光点、阴影点）两个状态值（见 appendSurfaceStates）。
        // Image bands must remain spectral even though the energy balance uses
        // broadband shortwave/longwave fluxes. Optical bands are normalized to
        // apparent reflectance; thermal bands retain spectral radiance. As in
        // FacetRT, each side has a sunlit-point and a shaded-point value
        // (per facet: front lit, front shaded, back lit, back shaded).
        const size_t bandCount = radiativeParameters.wavelengths.size();
        std::vector<std::vector<float>> spectralStates(bandCount);
        // Energy balance above uses integrated broadband shortwave/longwave
        // fluxes. Sensor products are a separate calculation: solve every
        // requested wavelength independently so a selected band is never an
        // interpolation of neighbouring bands.
        for (size_t band = 0; band < bandCount; ++band) {
            const float wavelength = radiativeParameters.wavelengths[band];
            const bool optical = isOpticalWavelength(wavelength);
            // 步骤 M1：本波段的照度。光学波段：直射/漫射 = Rin × 光谱表在该波长的值 × 0.001
            // （单位换算因子 0.001 的含义待核对）；漫射再加上换算中丢弃的直射份额 lostDirectFraction。
            const float directSpectralIrradiance = optical
                ? totalShortwave * shortwaveSpectrum.directAt(wavelength) * 0.001f
                : 0.0f;
            const float diffuseSpectralIrradiance = optical
                ? totalShortwave * shortwaveSpectrum.diffuseAt(wavelength) * 0.001f +
                    directSpectralIrradiance * lostDirectFraction
                : 0.0f;
            // Same illumination as the energy solve: horizontal direct
            // irradiance converted to the beam-normal value, sky diffuse plus
            // any direct share dropped by that conversion.
            // Thermal bands below 6 um reflect the same meteorological
            // shortwave (Rin x direct share, mwirSolarShape spectral shape,
            // as VoxelEB), in radiance units (/pi); exactly 0 above 6 um and
            // at night. The energy balance already holds this energy in Rin.
            const float mwirShape = optical ? 0.0f : mwirSolarShape(wavelength);
            const float mwirDirect = totalShortwave *
                std::clamp(directSpectrumIntegral, 0.0f, 1.0f) * mwirShape;
            const float mwirSky = mwirShape > 0.0f
                ? (totalShortwave * (1.0f - std::clamp(directSpectrumIntegral, 0.0f, 1.0f)) *
                       kMwirSkySolarRatio * mwirShape +
                   mwirDirect * lostDirectFraction) / kPi
                : 0.0f;
            const float directNormalSpectralIrradiance = optical
                ? directSpectralIrradiance * directNormalFactor
                : mwirDirect * directNormalFactor / kPi;
            // 步骤 M2：按两态温度与受光比例生成该波段的光学量与发射（stateEmission 为每侧受光/阴影两态
            // 的发射），spectralImage 保存图像产品所需的反射量；随后求解辐射度。
            std::vector<std::array<float, 2>> stateEmission;
            std::vector<ImageReflection> spectralImage;
            const std::vector<facetvk::SurfaceOptics> spectralOptics =
                makeStateSpectralOptics(
                    scene, sunlit, radiativeParameters, band,
                    stateTemperature, sunlitPart,
                    directNormalSpectralIrradiance, stateEmission, &spectralImage);
            // 天空项：光学波段为漫射辐照（W m-2）；热红外波段为 Planck(skyTemperature) + 2.5–6 um 反射太阳的
            // 天空份额（W m-2 sr-1 um-1）。
            const float skySpectralRadiance = optical
                ? diffuseSpectralIrradiance
                // DEFERRED[低] 物理: 热红外图像的天空辐射用固定 skyTemperature 的普朗克辐射，而能量平衡用的是气象驱动 meteo.longwave（Rli）；两者不一致会使热红外图像与求得的温度场不自洽；建议：由 Rli 反推等效天空亮温 (Rli/σ)^(1/4) 用于光谱天空辐射。
                : planckRadiance(wavelength, skyTemperature) + mwirSky;
            // Same residual-based early exit as the shortwave windows, scaled
            // by the band's illumination (optical) or by the Planck radiance
            // at air temperature (thermal; r+t is small there).
            const float bandReference = optical
                ? directNormalSpectralIrradiance + diffuseSpectralIrradiance
                : std::max(skySpectralRadiance, planckRadiance(wavelength, airTemperature));
            const facetvk::SolveResult spectralSolution = solveShortwaveWindow(
                spectralOptics, skySpectralRadiance, bandReference);
            spectralStates[band].reserve(surfaceCount * 2U);
            if (optical) {
                // Reflectance factor relative to the global horizontal
                // irradiance actually applied (beam normal x cos + diffuse).
                const float referenceIrradiance =
                    directNormalSpectralIrradiance * std::max(cosineSolarZenith, 0.0f) +
                    diffuseSpectralIrradiance;
                appendSurfaceStates(
                    scene, spectralOptics, stateEmission, spectralSolution.radiosity,
                    directNormalSpectralIrradiance,
                    referenceIrradiance > 1.0e-8f ? 1.0f / referenceIrradiance : 0.0f,
                    true, spectralStates[band], &spectralImage);
            } else {
                appendSurfaceStates(
                    scene, spectralOptics, stateEmission, spectralSolution.radiosity,
                    directNormalSpectralIrradiance, 1.0f, false, spectralStates[band], &spectralImage);
            }
        }

        // 步骤 N：写节点二进制 .facet_steps/energy_T=<token>.bin。按面侧交错，每侧 11 个 float：
        // 平均温度、总/短波/长波辐射度、净辐射、显热、潜热、储热、受光比例、受光点温度、阴影点温度，
        // 其后依次接各成像波段的（受光点, 阴影点）两个值。
        const std::filesystem::path binaryPath =
            stepDirectory / ("energy_T=" + token + ".bin");
        std::ofstream binary(binaryPath, std::ios::binary | std::ios::trunc);
        if (!binary) {
            throw std::runtime_error(
                "FacetEB cannot write time-step energy file");
        }
        for (size_t side = 0; side < surfaceCount; ++side) {
            const float values[11] = {
                temperature[side],
                radiativeSolution.radiosity[side],
                shortwaveSolution.radiosity[side],
                finalLongwaveSolution.radiosity[side],
                netRadiation[side],
                sensibleHeat[side],
                latentHeat[side],
                storageHeat[side],
                sunlitPart[side],
                stateTemperature[0][side],
                stateTemperature[1][side]
            };
            binary.write(reinterpret_cast<const char*>(values), sizeof(values));
            for (const std::vector<float>& bandStates : spectralStates) {
                binary.write(
                    reinterpret_cast<const char*>(bandStates.data() + side * 2U),
                    2U * sizeof(float));
            }
        }

        // 作用：写过程产品 process/<model>_T=<token>.bin 与 .json 并登记到 runIndex。type：
        // radiation（短波/长波辐射度、净辐射）、energy（潜热、显热、表面热通量、温度、净辐射、受光点/阴影点温度，
        // 每侧 7 个 float）、photovoltaic（发电功率、温度、吸收短波）；前两者之外只写每侧 3 个 float。
        // JSON 记录字段表与本节点的收敛信息（迭代数、温差、能量残差，W m-2）。
        const auto writeProcessMetadata = [&](const std::string& type) {
            // Radiation in FacetEB is a time sequence of FacetRT fields;
            // energy fluxes retain the FacetEB model name.
            const std::string processModel =
                type == "radiation" ? "facetrt" : "faceteb";
            const std::string processStem = processModel + (type == "photovoltaic" ? "_pv_T=" : "_T=") + token;
            const std::filesystem::path processDirectory = outputDirectory / "process";
            // PV results are also written when the optional RT/EB process flags
            // are off. Ensure their destination exists at the point of use.
            std::filesystem::create_directories(processDirectory);
            const std::filesystem::path processBinaryPath =
                processDirectory / (processStem + ".bin");
            const std::filesystem::path metadataPath =
                processDirectory / (processStem + ".json");
            runIndex.value["products"].push_back("process/" + processStem + ".bin");
            runIndex.value["products"].push_back("process/" + processStem + ".json");
            std::ofstream processBinary(
                processBinaryPath, std::ios::binary | std::ios::trunc);
            if (!processBinary) {
                throw std::runtime_error(
                    "FacetEB cannot write process data: " +
                    processBinaryPath.string());
            }
            for (size_t side = 0; side < surfaceCount; ++side) {
                const float values[7] = {
                    type == "photovoltaic" ? photovoltaicPower[side] : type == "radiation" ? shortwaveSolution.radiosity[side]
                                        : latentHeat[side],
                    type == "photovoltaic" ? temperature[side] : type == "radiation" ? finalLongwaveSolution.radiosity[side]
                                        : sensibleHeat[side],
                    type == "photovoltaic" ? absorbedShortwave[side] : type == "radiation" ? netRadiation[side]
                                        : storageHeat[side],
                    temperature[side], netRadiation[side],
                    stateTemperature[0][side], stateTemperature[1][side]
                };
                processBinary.write(
                    reinterpret_cast<const char*>(values), (type == "energy" ? 7 : 3) * sizeof(float));
            }
            processBinary.close();
            if (!processBinary) throw std::runtime_error("FacetEB cannot finish process binary");
            std::ofstream metadata(metadataPath, std::ios::trunc);
            metadata << std::setprecision(9)
                     << "{\n  \"kind\": \"facet-" << type << "-process\",\n"
                     << "  \"model\": \"" << processModel << "\",\n"
                     << "  \"processType\": \"" << type << "\",\n"
                     << "  \"geometry\": \"facet\",\n"
                     << "  \"node\": " << node << ",\n"
                     << "  \"julianTime\": " << meteo.julianTime << ",\n"
                     << "  \"time\": \"" << token << "\",\n"
                     << "  \"solarZenith\": " << scene.sunZenith << ",\n"
                     << "  \"solarAzimuth\": " << scene.sunAzimuth << ",\n"
                     << "  \"facetCount\": " << scene.facetCount() << ",\n"
                     << "  \"surfaceCount\": " << surfaceCount << ",\n"
                     << "  \"geometryFile\": \"../faceteb.json\",\n"
                     << "  \"componentFile\": \"../.facet_steps/components.bin\",\n"
                     << "  \"dataFile\": \""
                     << processBinaryPath.filename().string() << "\",\n"
                     << "  \"dataType\": \"float32-little-endian\",\n"
                     << "  \"layout\": \"surface-interleaved\",\n"
                     << "  \"recordFloats\": " << (type == "energy" ? 7 : 3) << ",\n"
                     << "  \"couplingIterations\": " << couplingIterations << ",\n"
                     << "  \"temperatureDelta\": "
                     << maximumTemperatureDelta
                     << ",\n  \"converged\": " << (converged ? "true" : "false")
                     << ",\n  \"energyResidual\": " << maximumEnergyResidual
                     << ",\n  \"energyResidualUnits\": \"W m-2\""
                     << ",\n  \"energyResidualTemperatureEquivalent\": " << maximumEnergyResidualEquivalent
                     << ",\n  \"temperatureTolerance\": " << temperatureTolerance
                     << ",\n  \"iterationBudget\": " << maximumCouplingIterations << ",\n  \"fields\": ";
            if (type == "photovoltaic") {
                metadata << "[{\"id\":\"photovoltaicPower\",\"label\":\"光伏功率 [W m⁻²]\",\"offset\":0},"
                            "{\"id\":\"temperature\",\"label\":\"温度 [K]\",\"offset\":1},"
                            "{\"id\":\"absorbedShortwave\",\"label\":\"单面吸收短波 [W m⁻²]\",\"offset\":2}]\n}\n";
            } else if (type == "radiation") {
                metadata << "[{\"id\":\"shortwaveRadiation\",\"label\":\"短波辐射 [W m⁻²]\",\"offset\":0},"
                            "{\"id\":\"longwaveRadiation\",\"label\":\"长波辐射 [W m⁻²]\",\"offset\":1},"
                            "{\"id\":\"netRadiation\",\"label\":\"净辐射 [W m⁻²]\",\"offset\":2}]\n}\n";
            } else {
                metadata << "[{\"id\":\"latentHeat\",\"label\":\"潜热 [W m⁻²]\",\"offset\":0},"
                            "{\"id\":\"sensibleHeat\",\"label\":\"显热 [W m⁻²]\",\"offset\":1},"
                            "{\"id\":\"surfaceHeatFlux\",\"label\":\"表面热通量 [W m⁻²]\",\"offset\":2},"
                            "{\"id\":\"temperature\",\"label\":\"温度 [K]\",\"offset\":3},"
                            "{\"id\":\"netRadiation\",\"label\":\"净辐射 [W m⁻²]\",\"offset\":4},"
                            "{\"id\":\"sunlitTemperature\",\"label\":\"光照部分温度 [K]\",\"offset\":5},"
                            "{\"id\":\"shadedTemperature\",\"label\":\"阴影部分温度 [K]\",\"offset\":6}]\n}\n";
            }
        };
        // 过程输出开关：radiation（saveRadiationProcess，另附 PAR 光子通量产品 facetpar：每侧入射/吸收
        // 各 1 个 float，umol m-2 s-1）、energy（saveEnergyProcess）、photovoltaic（存在光伏面元时）。
        if (saveRadiationProcess) {
            writeProcessMetadata("radiation");
            // Keep the existing radiation record layout stable. PAR is a
            // separate per-side product using the energy solver's 400-700 nm windows.
            const std::string stem = "facetpar_T=" + token;
            const auto directory = outputDirectory / "process";
            std::ofstream parBinary(directory / (stem + ".bin"), std::ios::binary | std::ios::trunc);
            if (!parBinary) throw std::runtime_error("FacetEB cannot create PAR process binary");
            for (size_t side = 0; side < surfaceCount; ++side) {
                const float values[2] = {incidentParPhotons[side], absorbedParPhotons[side]};
                parBinary.write(reinterpret_cast<const char*>(values), sizeof(values));
            }
            parBinary.close();
            if (!parBinary) throw std::runtime_error("FacetEB cannot finish PAR process binary");
            const ProjectJson::Json parMetadata = {
                {"kind", "facet-par-process"}, {"model", "faceteb"},
                {"processType", "par"}, {"geometry", "facet"},
                {"node", node}, {"julianTime", meteo.julianTime}, {"time", token},
                {"solarZenith", scene.sunZenith}, {"solarAzimuth", scene.sunAzimuth},
                {"facetCount", scene.facetCount()}, {"surfaceCount", surfaceCount},
                {"geometryFile", "../faceteb.json"}, {"dataFile", stem + ".bin"},
                {"dataType", "float32-little-endian"}, {"layout", "surface-interleaved"},
                {"recordFloats", 2}, {"units", "umol photons m-2 s-1"},
                {"wavelengthRange_nm", {400, 700}},
                {"spectralWindowWidth_nm", spectralAccelerationWidth},
                {"aggregation", "per side; sum both sides for whole-leaf incident or absorbed PAR"},
                {"fields", {{{"id", "incidentParPhotons"}, {"offset", 0}},
                            {{"id", "absorbedParPhotons"}, {"offset", 1}}}}
            };
            std::ofstream parJson(directory / (stem + ".json"), std::ios::trunc);
            parJson << parMetadata.dump(2);
            parJson.close();
            if (!parJson) throw std::runtime_error("FacetEB cannot finish PAR process metadata");
            runIndex.value["products"].push_back("process/" + stem + ".bin");
            runIndex.value["products"].push_back("process/" + stem + ".json");
        }
        if (saveEnergyProcess) writeProcessMetadata("energy");
        if (pa > 0) writeProcessMetadata("photovoltaic");

        binary.close();
        pvCsv.close();
        pvSummary.flush();
        if (!binary || !pvCsv || !pvSummary) {
            throw std::runtime_error("FacetEB cannot finish time-step output");
        }
        runIndex.value["products"].push_back(".facet_steps/energy_T=" + token + ".bin");
        runIndex.value["products"].push_back("photovoltaic_node_" + std::to_string(node) + ".csv");
        // 把 CPU/GPU 影子审计汇总写入 run index（energyVerification：PASS/MISMATCH 与各类最大差）。
        if (shadowEnergy) {
            ProjectJson::Json mismatches = ProjectJson::Json::array();
            for (const auto& [label, entry] : shadowMismatches)
                mismatches.push_back({{"class_field", label}, {"count", entry.count},
                    {"maxDifference", entry.maxDifference}, {"firstOrWorstNode", entry.node},
                    {"facet", entry.facet}, {"state", entry.state}, {"cpu", entry.cpu}, {"gpu", entry.gpu}});
            runIndex.value["energyVerification"] = {
                {"status", shadowMismatches.empty() ? "PASS" : "MISMATCH"}, {"sameRadiativeInputs", true},
                {"mode", shadowSurvey ? "survey" : "strict"},
                {"evaluations", shadowEvaluations}, {"updates", shadowUpdates},
                {"maxAbsIncident_W_m2", shadowIncidentMaximum},
                {"maxAbsFlux_W_m2", shadowFluxMaximum},
                {"maxAbsTemperatureUpdate_K", shadowUpdateMaximum},
                {"maxLeafResistanceDifference_ln", shadowResistanceMaximum},
                {"auditedFacetsByClass", shadowClassFacets}, {"mismatches", mismatches}};
        }
        // 登记本节点完成记录（收敛、迭代数、残差等）并写 run index；中断的运行可能留有旧节点文件，
        // 是否完成以此处记录为准。
        runIndex.value["completed"].push_back({
            {"node", node}, {"token", token}, {"julianTime", meteo.julianTime},
            {"solarZenith", scene.sunZenith}, {"solarAzimuth", scene.sunAzimuth},
            {"converged", converged}, {"iterations", couplingIterations},
            {"energyFailingFacets", failingEnergyFacets}, {"energyTotalFacets", scene.facetCount()},
            {"energyFailingFacetFraction", double(failingEnergyFacets) / scene.facetCount()},
            {"iterationBudget", maximumCouplingIterations},
            {"temperatureDelta", maximumTemperatureDelta},
            {"temperatureTolerance", temperatureTolerance},
            {"energyResidual", maximumEnergyResidual}, {"energyResidualUnits", "W m-2"},
            {"energyResidualTemperatureEquivalent", maximumEnergyResidualEquivalent}
        });
        if (!soilColumnSummary.is_null()) runIndex.value["completed"].back()["soilColumn"] = soilColumnSummary;
        runIndex.write();

        // 步骤 O：最后一个节点的结果保留为 latest（用于总结果 faceteb.json 的图层）；
        // 用 std::move 取走通量数组，此后本节点不再使用它们。
        if (node == endNode - 1) {
            latest.sunlit = sunlit;
            latest.solution = radiativeSolution;
            latest.lightEnhancement = lightEnhancement(
                shortwaveSolution.radiosity, diffuseSolution.radiosity);
            latestNetRadiation = std::move(netRadiation);
            latestSensibleHeat = std::move(sensibleHeat);
            latestLatentHeat = std::move(latentHeat);
            latestPhotovoltaicPower = std::move(photovoltaicPower);
            latestStorageHeat = std::move(storageHeat);
            latestCouplingIterations = couplingIterations;
            latestTemperatureDelta = maximumTemperatureDelta;
            latestEnergyResidual = maximumEnergyResidual;
            latestEnergyResidualEquivalent = maximumEnergyResidualEquivalent;
            latestTime = token;
        }

        // 进度从 10% 线性推进到 96%，并打印 PROGRESS / FACET_CONVERGENCE / FACET_TIMING 日志行
        // （三段耗时：短波窗口、RT-EB 耦合、波段图像与产品，单位 s），由服务端解析。
        const int progress = 10 + static_cast<int>(
            86.0 * (node - startNode + 1) / (endNode - startNode));
        std::cout << "PROGRESS\t" << progress
                  << "\t面元 RT-EB 时间节点 " << (node - startNode + 1)
                  << "/" << (endNode - startNode) << " " << token
                  << "，迭代=" << couplingIterations
                  << "，最大温差=" << maximumTemperatureDelta << " K"
                  << std::endl;
        std::cout << "FACET_CONVERGENCE\t" << node << '\t' << (converged ? 1 : 0)
                  << '\t' << couplingIterations << '\t' << maximumTemperatureDelta
                  << '\t' << maximumEnergyResidual << '\t' << maximumEnergyResidualEquivalent << std::endl;
        {
            const auto seconds = [](Clock::time_point a, Clock::time_point b) {
                return std::chrono::duration<double>(b - a).count();
            };
            const auto nodeEnd = Clock::now();
            // Per-node phase timing (s): shortwave windows, RT-EB coupling,
            // band images + products. Parsed by the server for ETA/diagnostics.
            std::cout << "FACET_TIMING\t" << node << '\t'
                      << seconds(nodeBegin, shortwaveDone) << '\t'
                      << seconds(shortwaveDone, couplingDone) << '\t'
                      << seconds(couplingDone, nodeEnd) << std::endl;
        }
        }
        // 步骤 P：逐节点输出同步。打印 FACET_STEP 后阻塞等待父进程回复 FACET_ACK，
        // 以免下一节点的 GPU 求解与当前节点的 CPU 图像缓冲重叠（背压）。
        if (synchronizeStepOutput) {
            // Backpressure: never overlap the next GPU solve with the current
            // node's CPU image buffers. EOF/error aborts instead of hanging.
            std::cout << "FACET_STEP\t" << node << '\t' << token << '\t'
                      << std::setprecision(9) << meteo.julianTime << '\t'
                      << scene.sunZenith << '\t' << scene.sunAzimuth << std::endl;
            std::string acknowledgment;
            if (!std::getline(std::cin, acknowledgment) ||
                acknowledgment != "FACET_ACK\t" + std::to_string(node)) {
                throw std::runtime_error("FacetEB time-step output was interrupted or failed");
            }
        }
    }

    // 步骤 Q：全部节点结束后写总结果 resultFile（JSON，手写流式输出）。内容：顶点坐标、
    // 可见性图诊断与下边界、迭代/收敛信息（最后一个节点），以及最后一个节点的逐面侧场。
    // allNodesConverged 为所有节点收敛的与；能量残差单位 W m-2。
    latest.totalMs = milliseconds(totalBegin, Clock::now());
    latest.solveMs = latest.totalMs;

    std::ofstream output(resultFile, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("FacetEB cannot write result: " + resultFile);
    }
    output << std::setprecision(9)
           << "{\n  \"backend\": \"" << latest.backend << "\",\n"
           << "  \"componentFile\": \".facet_steps/components.bin\",\n"
           << "  \"facetCount\": " << scene.facetCount() << ",\n"
           << "  \"leafFacetCount\": " << scene.leafFacetCount << ",\n"
           << "  \"time\": \"" << latestTime << "\",\n"
           << "  \"vertexPositions\": [";
    for (size_t index = 0; index < scene.vertices.size(); ++index) {
        if (index != 0U) output << ',';
        const auto& vertex = scene.vertices[index];
        output << vertex.position[0] << ',' << vertex.position[1] << ','
               << vertex.position[2];
    }
    output << "],\n"
           << "  \"graph\": {\"directedEdges\":"
           << latest.graph.directedEdgeCount
           << ",\"closureError\":" << latest.graph.maxClosureError
           << ",\"fragmentOverflow\":" << latest.graph.fragmentOverflow
           << ",\"hashOverflow\":" << latest.graph.hashOverflow
           << ",\"lowerBoundary\":" << groundEscapeJson(latest.graph).dump() << "},\n"
           << "  \"iterations\": " << latest.solution.iterations << ",\n"
           << "  \"maxDelta\": " << latest.solution.maxDelta << ",\n"
           << "  \"couplingIterations\": " << latestCouplingIterations << ",\n"
           << "  \"temperatureDelta\": " << latestTemperatureDelta << ",\n";
    output << "  \"converged\": " << (allNodesConverged ? "true" : "false") << ",\n"
           << "  \"energyResidual\": " << latestEnergyResidual << ",\n"
           << "  \"energyResidualUnits\": \"W m-2\",\n"
           << "  \"energyResidualTemperatureEquivalent\": " << latestEnergyResidualEquivalent << ",\n"
           << "  \"temperatureTolerance\": " << temperatureTolerance << ",\n"
           << "  \"iterationBudget\": " << maximumCouplingIterations << ",\n";

    // 作用：把逐面侧 float 数组写成一个 JSON 数组字段；comma 指示末尾是否加逗号。
    const auto writeArray = [&output](
        const char* name, const std::vector<float>& values, bool comma) {
        output << "  \"" << name << "\": [";
        // DEFERRED[低] 错误处理: 与 FacetRT 的 writeWebResult 不同，这里不检查非有限值，NaN/Inf 会被写成 nan/inf，生成非法 JSON 使前端解析失败；建议：写出前检查 std::isfinite 并报错或写 null。
        for (size_t index = 0; index < values.size(); ++index) {
            if (index != 0U) output << ',';
            output << values[index];
        }
        output << ']' << (comma ? "," : "") << '\n';
    };
    // 以下逐面侧数组长度均为 surfaceCount（= 2 × 面元数）：温度为 K，通量为 W m-2，
    // 其中 temperature* 为最终状态，其余通量取最后一个节点。
    // Actual engine band list and per-side record width of the node files
    // (11 scalars + lit/shaded per band), so readers need not rebuild them.
    writeArray("wavelengths", radiativeParameters.wavelengths, true);
    output << "  \"stepRecordFloats\": " << (11U + 2U * radiativeParameters.wavelengths.size()) << ",\n";
    // （wavelengths 为波段列表，不是逐面侧数组；从 sunlit 起才是逐面侧数组。）
    writeArray("sunlit", latest.sunlit, true);
    writeArray("radiosity", latest.solution.radiosity, true);
    writeArray("lightEnhancement", latest.lightEnhancement, true);
    writeArray("temperature", temperature, true);
    writeArray("temperatureSunlit", stateTemperature[0], true);
    writeArray("temperatureShaded", stateTemperature[1], true);
    writeArray("netRadiation", latestNetRadiation, true);
    writeArray("sensibleHeat", latestSensibleHeat, true);
    writeArray("latentHeat", latestLatentHeat, true);
    writeArray("storageHeat", latestStorageHeat, true);
    writeArray("photovoltaicPower", latestPhotovoltaicPower, false);
    output << "}\n";

    // 收尾：关闭结果文件，登记产品，状态记为 completed / non_converged，写完成时间；
    // 开启影子审计时把审计汇总打印到标准错误。
    output.close();
    if (!output) throw std::runtime_error("FacetEB cannot finish result output");
    runIndex.value["products"].push_back(std::filesystem::path(resultFile).filename().generic_string());
    runIndex.value["status"] = allNodesConverged ? "completed" : "non_converged";
    runIndex.value["finishedAt"] = utcTimestamp();
    runIndex.write();
    if (shadowEnergy) {
        std::cerr << "faceteb: audited facets by class:";
        for (const auto& [label, count] : shadowClassFacets) std::cerr << " " << label << "=" << count;
        std::cerr << "\n";
        std::cerr << "faceteb: CPU/GPU energy audit " << (shadowMismatches.empty() ? "PASS" : "MISMATCH")
                  << ", evaluations " << shadowEvaluations << ", temperature updates " << shadowUpdates
                  << ", max |flux| " << shadowFluxMaximum << " W m-2, max |T update| " << shadowUpdateMaximum << " K, max leaf ln(rs) free-vs-GPU " << shadowResistanceMaximum << "\n";
        for (const auto& [label, entry] : shadowMismatches)
            std::cerr << "faceteb:   " << label << ": " << entry.count << " mismatches, max " << entry.maxDifference
                      << ", at node " << entry.node << " facet " << entry.facet << " state " << entry.state
                      << " CPU " << entry.cpu << " GPU " << entry.gpu << "\n";
    }
    std::cout << "PROGRESS\t100\t" << (allNodesConverged ? "面元辐射传输与能量平衡耦合收敛" : "面元计算结束：存在未收敛节点，结果已保留") << std::endl;
    std::cout << "RESULT\t" << resultFile << std::endl;
    // 退出码：3 = CPU/GPU 影子审计有不一致；0 = 全部节点收敛；2 = 存在未收敛节点（结果已保留）；
    // 异常时由下面的 catch 返回 1。
    return !shadowMismatches.empty() ? 3 : allNodesConverged ? 0 : 2;
} catch (const std::exception& error) {
    std::cerr << "faceteb: " << error.what() << '\n';
    return 1;
}
