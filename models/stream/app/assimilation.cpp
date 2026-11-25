// ==========================================================================
// 文件：assimilation.cpp
// 功能：STREAM 运行模式“同化”的实现：对每个像元独立做 MAP（最大后验）高斯-牛顿分析，
//   使用与成像共用、由 Dual 数求精确导数的可微观测算子 h(x)，由观测（亮温 / LST / 组分温度，
//   可选光学反射率）修正背景控制量。
// 主要模块/步骤：
//   1. 内置缺省常量（先验误差、搜索范围、迭代与收敛阈值）与控制量枚举 ControlKind
//   2. 辅助函数：进度/日志输出、小矩阵求逆 invert、读取观测波段 readBands/optionalBands、
//      QA 位掩码判定 acceptedQa、观测误差 sigmaAt
//   3. runAssimilation：
//      读取观测栅格 → 初始化结果与诊断容器 → OpenMP 按行并行，逐像元：输入检查、
//      构造热红外/光学场景与观测列表、确定控制量与背景协方差、构造算子 evaluate（Dual 数）、
//      MAP 高斯-牛顿迭代（带回溯，保证代价 J 下降）、质量标志与诊断 → 汇总统计、写出产品
// 数据流：config.h 的 AssimilationConfig + AssimilationInputs（场景栅格、物理表）+ 背景组分
//   温度 → runAssimilation → AssimilationResult（分析量、不确定度、QA 波段、通量、统计摘要）。
// 约定：温度 K；土壤湿度 m3 m-3；Vcmax 为 Vcmax25（μmol m-2 s-1，25 °C 参考温度下的最大羧化速率）；
//   控制量顺序 Tss/Tsh/Tcs/Tch/土壤湿度/Vcmax/LAI；RT 背景以组分温度为控制量，
//   EB 背景以土壤湿度和 Vcmax 为控制量，温度由能量平衡经隐函数求导得到；
//   QA 位常量来自 inversion.h，与 LST/LSCT 反演共享。
// ==========================================================================

#include "assimilation.h"
#include "parallel_error.h"
#include "inversion.h"  // QA bit constants shared with the LST/LSCT retrieval
#include "../forcing/utc.h"  // FIX(2026-11-21 C7): overpass day of year
#include "../physics/canopy.h"
#include "../physics/dual.h"
#include "../physics/shortwave.h"
#include "../physics/observation_errors.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace stream {
namespace {

// 模块：内置缺省值（工作台不暴露）。
// 先验：温度 sd 5 K、温度间相关 0.5；土壤湿度 sd 0.10（范围 0.02–0.60）；
// Vcmax 相对 sd 0.3（下限 10，范围 5–250）；LAI 相对 sd 0.3（下限 0.5，上限 10）。
// kTemperatureLow/High：控制量温度搜索域；kValid*：分析结果合理范围（K）。
// kMaxIterations*：RT 20 步、EB 10 步（EB 每步要重解能量平衡，较贵）。
// kLowSupportDfs：DFS = n − tr(P·B⁻¹)（n 为控制量个数，P 为分析协方差，B 为背景协方差，即平均核迹）；DFS < 0.2 表示
//   观测合计提供的独立信息不足 0.2 个自由度（分析几乎等于背景），像元置 QA 位 8（低支持，缺省拒收，见 README 质量位表）；
// kMoistureSteps：土壤反射率按湿度的查表步数。
// ---- built-in defaults (not exposed in the workbench) ----
constexpr double kTemperatureSd = 5.0;      // K, background error of each component
constexpr double kTemperatureCorrelation = 0.5;
constexpr double kMoistureSd = 0.10, kMoistureLow = 0.02, kMoistureHigh = 0.60;  // m3 m-3
constexpr double kVcmaxRelativeSd = 0.3, kVcmaxMinSd = 10.0, kVcmaxLow = 5.0, kVcmaxHigh = 250.0;
constexpr double kLaiRelativeSd = 0.3, kLaiMinSd = 0.5, kLaiHigh = 10.0;
constexpr double kTemperatureLow = 150.0, kTemperatureHigh = 400.0;  // search domain, K
constexpr double kValidLow = 200.0, kValidHigh = 380.0;              // plausible analysis, K
constexpr int kMaxIterationsRt = 20, kMaxIterationsEb = 10;
constexpr double kStepTolerance = 1e-3;  // converged when every step < 1e-3 of its background sd (5e-3 K for temperatures)
constexpr double kLowSupportDfs = 0.2;
constexpr int kMoistureSteps = 1000;
constexpr int kMaxControls = 7;
static_assert(kMaxControls <= kAdSize, "controls exceed the dual-number size");

// 控制量种类：前 4 个为组分温度（Tss/Tsh/Tcs/Tch），其后为土壤湿度、Vcmax、LAI。
// 每个像元实际启用的控制量按此枚举映射到 0..n-1 的紧凑下标。
enum ControlKind { kTss = 0, kTsh = 1, kTcs = 2, kTch = 3, kMoisture = 4, kVcmax = 5, kLai = 6, kKinds = 7 };
const char* kControlNames[kKinds] = {"Tss", "Tsh", "Tcs", "Tch", "soil moisture", "Vcmax25", "LAI"};

// 输出辅助：progress 打印进度行 STREAM\tprogress\t百分比\t阶段（供上层解析）；
// log 打印带 [STREAM] 前缀的日志；format 按位数格式化小数；
// isMainThread 判断是否为 OpenMP 主线程（只由主线程汇报进度）。
void progress(double percent, const std::string& stage)
{
    std::cout << "STREAM\tprogress\t" << std::fixed << std::setprecision(1) << percent << '\t' << stage << '\n';
}
void log(const std::string& text) { std::cout << "[STREAM] " << text << '\n'; }
std::string format(double value, int digits = 2)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(digits) << value;
    return stream.str();
}
bool isMainThread()
{
#ifdef _OPENMP
    return omp_get_thread_num() == 0;
#else
    return true;
#endif
}

// 控制量最多 kMaxControls = 7 个，故用固定大小的小矩阵（与 Dual 数导数维数 kAdSize 一致）。
using Matrix = std::array<std::array<double, kMaxControls>, kMaxControls>;

// 作用：对 a 的前 n×n 子块做带部分选主元的高斯-约当求逆，结果写回 a。
// 返回：奇异或非有限时返回 false（此时 a 的内容已被破坏）；
// 原因：控制量未归一化，故奇异性判据取相对于最大元素的 1e-12（见下方英文说明）。
// Gauss-Jordan inverse with partial pivoting of the leading n x n block. FIX(2026-11-24 A3): the singularity test is
// relative to the largest entry (1e-12 x scale, as in inversion.cpp) instead of the absolute 1e-14, because the controls
// are not normalised (Vcmax sigma >= 10 gives B^-1 ~ 1e-2, soil moisture sigma 0.1 gives 1e2).
bool invert(Matrix& a, int n)
{
    Matrix inverse{};
    for (int i = 0; i < n; ++i) inverse[i][i] = 1.0;
    double scale = 0.0;
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) scale = std::max(scale, std::abs(a[i][j]));
    if (!(scale > 0.0) || !std::isfinite(scale)) return false;
    for (int column = 0; column < n; ++column) {
        int pivot = column;
        for (int row = column + 1; row < n; ++row)
            if (std::abs(a[row][column]) > std::abs(a[pivot][column])) pivot = row;
        if (!(std::abs(a[pivot][column]) > 1e-12 * scale)) return false;
        std::swap(a[pivot], a[column]);
        std::swap(inverse[pivot], inverse[column]);
        const double diagonal = a[column][column];
        for (int k = 0; k < n; ++k) { a[column][k] /= diagonal; inverse[column][k] /= diagonal; }
        for (int row = 0; row < n; ++row) {
            if (row == column) continue;
            const double factor = a[row][column];
            if (factor == 0.0) continue;
            for (int k = 0; k < n; ++k) { a[row][k] -= factor * a[column][k]; inverse[row][k] -= factor * inverse[column][k]; }
        }
    }
    a = inverse;
    return true;
}

// 模块：单个观测的描述。Source：场景亮温 / 附加视角亮温 / LST / 组分温度 / 光学反射率；
// band 为波段号（组分温度时为 0..3），view 为附加视角号（-1 表示场景视角），
// y 观测值，sigma 观测误差标准差（亮温单位 K，反射率无量纲），thermal 区分热红外与光学。
enum class Source { SceneBrightness, ViewBrightness, Lst, Component, Reflectance };
struct Observation {
    Source source;
    int band = 0, view = -1;
    double y = 0, sigma = 1;
    bool thermal = true;
};

// 作用：把栅格文件前 count 个波段读成与 grid 对齐的 float 数组；波段数不足则抛异常。
std::vector<std::vector<float>> readBands(const std::string& file, int count, const Grid& grid, const std::string& label)
{
    auto dataset = openRaster(file, label);
    if (dataset->GetRasterCount() < count)
        throw std::runtime_error(label + "须有 " + std::to_string(count) + " 个波段，实际 " + std::to_string(dataset->GetRasterCount()));
    std::vector<std::vector<float>> bands(count);
    for (int band = 0; band < count; ++band) bands[band] = readBand(*dataset, band + 1, grid, label + " 波段 " + std::to_string(band + 1));
    return bands;
}

// 作用：读取可选栅格（不确定度、QA）；空路径返回空；波段数须为 1（所有观测共用）或 count。
// One band may be shared by all observations; otherwise the band counts must match.
std::vector<std::vector<float>> optionalBands(const std::string& file, int count, const Grid& grid, const std::string& label)
{
    if (file.empty()) return {};
    auto dataset = openRaster(file, label);
    const int bands = dataset->GetRasterCount();
    if (bands != 1 && bands != count) throw std::runtime_error(label + " must have 1 or " + std::to_string(count) + " bands");
    std::vector<std::vector<float>> result(count);
    for (int b = 0; b < count; ++b) result[b] = readBand(*dataset, bands == 1 ? 1 : b + 1, grid, label);
    return result;
}

// 作用：判断像元 QA 值是否可接受。无 QA 栅格时一律接受；否则 QA 须为 0..65535 的整数，
// 且与剔除掩码 reject 按位与为 0。
bool acceptedQa(const std::vector<std::vector<float>>& qa, int band, size_t pixel, unsigned reject)
{
    if (qa.empty()) return true;
    const double value = qa[band][pixel];
    return std::isfinite(value) && value >= 0 && value <= 65535 && std::floor(value) == value &&
           (static_cast<unsigned>(value) & reject) == 0;
}

// 作用：取像元观测误差：未提供误差栅格时用缺省 fallback；提供了则直接用栅格值，
// 无效值（NaN/非正）不会被缺省值替换，而是使该观测被拒绝（见 addThermal 的检查）。
double sigmaAt(const std::vector<std::vector<float>>& raster, int band, size_t pixel, double fallback)
{
    return raster.empty() ? fallback : raster[band][pixel];  // invalid supplied errors never use the default
}

}  // namespace

// 作用：对 [startRow,endRow) × [startColumn,endColumn) 内每个像元做 MAP 同化分析。
// 输入：in（场景栅格、物理表、EB 求解回调、进度区间）、background（四个背景组分温度栅格）。
// 输出：AssimilationResult：分析组分温度（未分析处保持背景）、分析 LAI/土壤湿度/Vcmax、
//       分析标记、产品列表与统计摘要（summary）。
// 说明：像元之间相互独立，按行用 OpenMP 并行；每个像元的 QA 标志写入 qaBands[0]。
AssimilationResult runAssimilation(const AssimilationInputs& in, const std::array<std::vector<float>, 4>& background)
{
    const ImageConfig& config = in.config;
    const AssimilationConfig& as = config.assimilation;
    const Grid& grid = in.grid;
    const size_t pixels = grid.size();
    const bool eb = in.eb;
    const AssimilationObservation kind = as.observation;

    // 步骤 1：读取观测栅格。数据波段数：亮温 = 热红外波段数，LST = 1，组分温度 = 4；
    //         可选：不确定度、协方差（N×N 行主序，K^2）、QA、附加视角及光学反射率。
    progress(in.progressStart, "读取同化观测");
    std::vector<double> brightnessWavelengths;
    for (const auto& band : config.thermalBands) brightnessWavelengths.push_back(band.center);
    const int dataBands = kind == AssimilationObservation::Brightness ? static_cast<int>(brightnessWavelengths.size())
                        : kind == AssimilationObservation::Lst ? 1 : 4;
    const std::string dataLabel = kind == AssimilationObservation::Brightness ? "亮温观测"
                                : kind == AssimilationObservation::Lst ? "地表温度观测" : "组分温度观测";
    const auto data = readBands(as.data, dataBands, grid, dataLabel);
    const auto errors = optionalBands(as.uncertainty, dataBands, grid, "observation uncertainty");
    std::vector<std::vector<float>> covarianceRaster;
    if (!as.covariance.empty()) {
        auto dataset = openRaster(as.covariance, "observation covariance");
        if (dataset->GetRasterCount() != dataBands * dataBands)
            throw std::runtime_error("observation covariance needs N*N row-major bands (K^2)");
        covarianceRaster = readBands(as.covariance, dataBands * dataBands, grid, "observation covariance");
    }
    const auto qa = optionalBands(as.qa, dataBands, grid, "observation QA");
    const double defaultSigma = kind == AssimilationObservation::Brightness ? as.sigmaBrightnessK
                              : kind == AssimilationObservation::Lst ? as.sigmaLstK : as.sigmaComponentK;
    std::vector<std::vector<std::vector<float>>> viewData;
    std::vector<std::vector<std::vector<float>>> viewErrors, viewQa;
    std::vector<std::vector<float>> viewVza, viewVaa;
    for (size_t v = 0; v < as.views.size(); ++v) {
        const std::string label = "附加视角 " + std::to_string(v + 1) + " 亮温";
        viewData.push_back(readBands(as.views[v].data, dataBands, grid, label));
        viewErrors.push_back(optionalBands(as.views[v].uncertainty, dataBands, grid, label + " uncertainty"));
        viewQa.push_back(optionalBands(as.views[v].qa, dataBands, grid, label + " QA"));
        viewVza.push_back(fieldRaster(as.views[v].vza, grid, label + " VZA", 0, true));
        viewVaa.push_back(fieldRaster(as.views[v].vaa, grid, label + " VAA", 0, true));
    }
    const bool withOptical = !as.optical.empty();
    std::vector<std::vector<float>> reflectance;
    if (withOptical) reflectance = readBands(as.optical, static_cast<int>(config.opticalBands.size()), grid, "光学反射率观测");
    const auto opticalErrors = optionalBands(as.opticalUncertainty, static_cast<int>(config.opticalBands.size()), grid, "reflectance uncertainty");
    const auto opticalQa = optionalBands(as.opticalQa, static_cast<int>(config.opticalBands.size()), grid, "reflectance QA");
    // 步骤 1b：缺省参数：LST 观测波长（μm）、迭代上限（EB 与 RT 不同）、
    //         过境日在一年中的序号（用于 Erbs 晴空指数中的轨道因子）。
    const std::vector<double> lstWavelength{as.lstWavelengthUm};
    const int maxIterations = eb ? kMaxIterationsEb : kMaxIterationsRt;
    // FIX(2026-11-21 C7): day of year of the overpass for the orbital factor of the Erbs clearness index.
    const UtcTime overpassTime = utcFromSeconds(in.overpass);
    const int dayOfYear = static_cast<int>(daysFromCivil(overpassTime.year, overpassTime.month, overpassTime.day) -
                                           daysFromCivil(overpassTime.year, 1, 1)) + 1;

    // 步骤 2：初始化结果与诊断容器。分析量缺省为 NaN；组分温度先取背景。
    //         qaBands 九个波段依次为：QA 标志、O−B、O−A、O−A 的 rms、DFS、观测数、
    //         迭代次数、背景代价 J_b、分析代价 J_a。
    AssimilationResult result;
    for (int k = 0; k < 4; ++k) result.components[k] = background[k];
    result.lai.assign(pixels, kNaN);
    result.soilMoisture.assign(pixels, kNaN);
    result.vcmax.assign(pixels, kNaN);
    result.analysed.assign(pixels, 0);
    std::vector<std::vector<float>> uncertainty(kKinds, std::vector<float>(pixels, kNaN));
    // FIX(2026-11-21 S-5 QA): bands 8-9 (1-based) = MAP cost at the background / at the analysis (appended).
    std::vector<std::vector<float>> qaBands(9, std::vector<float>(pixels, kNaN));  // flags, O-B, O-A, O-A rms, DFS, n, iterations, J_b, J_a
    std::vector<std::vector<float>> backgroundControls(3, std::vector<float>(pixels, kNaN));
    std::vector<std::vector<float>> fluxes(eb ? 4 : 0, std::vector<float>(pixels, kNaN));

    // 各类计数与按行部分和：并行区内用 reduction 累加；O−B/O−A/DFS 按行累加后再按行序求和，
    // 使最后一位小数确定（见下方 E3 注释）。
    long long analysed = 0, noObservation = 0, failed = 0, notConverged = 0, illConditioned = 0, lowSupport = 0, observationsUsed = 0;
    // FIX(2026-11-21 S-5 QA): stop reasons of the analysed pixels
    long long stopConverged = 0, stopStalled = 0, stopNotUpdated = 0, stopIterationLimit = 0, stopSingular = 0, stopStepFailed = 0;
    double sumOmb = 0, sumOma = 0, sumDfs = 0;
    // FIX(2026-11-24 E3): per-row partial sums added in row order (deterministic last digit, see inversion.cpp).
    std::vector<double> rowOmb(static_cast<size_t>(std::max(grid.height, 0)), 0.0), rowOma(rowOmb.size(), 0.0), rowDfs(rowOmb.size(), 0.0);
    long long thermalPixels = 0;
    std::atomic<int> rowsDone{0}; int progressMark = 0;  // FIX(2026-11-24 E2): main thread only; reports whenever it finishes a row after enough rows are done
    const int rowCount = in.endRow - in.startRow;

    // 步骤 3：按行并行（dynamic 调度，块大小 2）。任一线程抛异常时记录到 ParallelError，
    //         其余行跳过，循环结束后统一重新抛出。
    ParallelError errorsAssimilation;
    #pragma omp parallel for schedule(dynamic, 2) reduction(+:analysed, noObservation, failed, notConverged, illConditioned, lowSupport, observationsUsed, thermalPixels, stopConverged, stopStalled, stopNotUpdated, stopIterationLimit, stopSingular, stopStepFailed)
    for (int row = in.startRow; row < in.endRow; ++row) {
        if (errorsAssimilation.failed()) continue;
        try {
        for (int column = in.startColumn; column < in.endColumn; ++column) {
            const size_t pixel = static_cast<size_t>(row) * grid.width + column;
            // 步骤 3a：只分析植被类地表（entry->surface == Vegetation）；其余像元跳过，保持背景。
            const auto* entry = in.classes.find(in.type[pixel]);
            if (!entry || entry->surface != SurfaceKind::Vegetation) continue;  // land pixels only
            unsigned flags = 0;
            const auto finish = [&](unsigned value) { qaBands[0][pixel] = static_cast<float>(value); };
            const float lat = in.latitude[pixel], lon = in.longitude[pixel];
            const float vz = in.vza[pixel], va = in.vaa[pixel], leafArea = in.lai[pixel];
            if (!(std::isfinite(lat) && std::isfinite(lon) && std::abs(lat) <= 90 && std::abs(lon) <= 180) ||
                !(vz >= 0 && vz < 90) || !(va >= -360 && va <= 360) || !validLai(leafArea)) {  // FIX(2026-11-24 M1): LAI <= kMaxLai (20), nodata invalid
                finish(kQaInputInvalid);
                continue;
            }
            // 步骤 3b：输入合法性检查通过后，计算该像元过境时刻的太阳位置（需 DEM 高程）。
            const Sun sun = solarPosition(in.overpass, lat, lon, in.dem[pixel]);
            if (!sun.ok) { finish(kQaInputInvalid); continue; }
            const int code = static_cast<int>(std::lround(in.type[pixel]));
            const double moistureB = std::isfinite(in.soilMoisture[pixel]) ? in.soilMoisture[pixel] : in.classes.defaultSoilMoisture;
            const double vcmaxB = entry->photosynthesis.vcmax;

            // 步骤 3c：构建热红外场景（含地形/坡面几何）与观测列表 obs。
            //         每个观测须：温度在 [kTemperatureLow, kTemperatureHigh]、误差为有限正数、QA 被接受；
            //         光学观测须太阳在地平线上（sun.zenith < 90°）、该类有叶片反射率，
            //         且反射率在 [0,1] 内。
            // ---- scenes and observations ----
            ThermalScene scene;
            const bool sceneOk = thermalSceneAt(in.terrain, pixel, sun.zenith, sun.azimuth, vz, va, scene);
            setThermalMaterial(scene, config, *entry, in.soilEmissivity, code);
            const double skyTemperature = in.skyLongwave ? skyTemperatureFromLongwave((*in.skyLongwave)[pixel]) : NAN;
            scene.skyTemperature = skyTemperature;
            std::vector<ThermalScene> viewScenes(as.views.size());
            std::vector<char> viewOk(as.views.size(), 0);
            for (size_t v = 0; v < as.views.size(); ++v) {
                const float z = viewVza[v][pixel], a = viewVaa[v][pixel];
                if (!(z >= 0 && z < 90) || !(a >= -360 && a <= 360)) continue;
                viewOk[v] = thermalSceneAt(in.terrain, pixel, sun.zenith, sun.azimuth, z, a, viewScenes[v]) ? 1 : 0;
                setThermalMaterial(viewScenes[v], config, *entry, in.soilEmissivity, code);
                viewScenes[v].skyTemperature = skyTemperature;
            }
            std::vector<Observation> obs;
            const auto temperatureValid = [](float y) { return std::isfinite(y) && y >= kTemperatureLow && y <= kTemperatureHigh; };
            const auto addThermal = [&](Source source, int band, int view, double value,
                                        const auto& errorRaster, const auto& qaRaster) {
                const double sigma = sigmaAt(errorRaster, band, pixel, defaultSigma);
                if (temperatureValid(static_cast<float>(value)) && std::isfinite(sigma) && sigma > 0 &&
                    acceptedQa(qaRaster, band, pixel, as.rejectQaBits))
                    obs.push_back({source, band, view, value, sigma, true});
            };
            if (kind == AssimilationObservation::Brightness) {
                if (sceneOk)
                    for (int b = 0; b < dataBands; ++b)
                        addThermal(Source::SceneBrightness, b, -1, data[b][pixel], errors, qa);
                for (size_t v = 0; v < as.views.size(); ++v) {
                    if (!viewOk[v]) continue;
                    for (int b = 0; b < dataBands; ++b)
                        addThermal(Source::ViewBrightness, b, static_cast<int>(v), viewData[v][b][pixel], viewErrors[v], viewQa[v]);
                }
            } else if (kind == AssimilationObservation::Lst) {
                if (sceneOk) addThermal(Source::Lst, 0, -1, data[0][pixel], errors, qa);
            } else {
                for (int k = 0; k < 4; ++k)
                    addThermal(Source::Component, k, -1, data[k][pixel], errors, qa);
            }
            bool opticalPixel = false;
            if (withOptical && sun.zenith < 90.0 && in.leafBandReflectance.count(code) > 0)
                for (size_t b = 0; b < config.opticalBands.size(); ++b) {
                    const float y = reflectance[b][pixel];
                    const double sigma = sigmaAt(opticalErrors, static_cast<int>(b), pixel, as.sigmaReflectance);
                    if (std::isfinite(y) && y >= 0.0f && y <= 1.0f && std::isfinite(sigma) && sigma > 0 &&
                        acceptedQa(opticalQa, static_cast<int>(b), pixel, as.rejectQaBits)) {
                        obs.push_back({Source::Reflectance, static_cast<int>(b), -1, y, sigma, false});
                        opticalPixel = true;
                    }
                }

            // 步骤 3d：确定控制量与背景协方差。RT：背景温度有效的组分温度；
            //         EB：土壤湿度，加 Vcmax（仅 C3/C4 光合途径且 LAI>0）；有光学观测时再加 LAI。
            //         各控制量的搜索范围 [low, high] 会扩展到包含背景值。
            //         背景协方差 B：对角 sd²，组分温度之间相关 0.5；随后求逆得到 B^-1。
            // ---- controls and background covariance ----
            int n = 0;
            std::array<int, kKinds> index;
            index.fill(-1);
            std::array<int, kMaxControls> kindOf{};
            std::array<double, kMaxControls> xb{}, sd{}, low{}, high{};
            const auto add = [&](int which, double value, double sigma, double lo, double hi) {
                index[which] = n;
                kindOf[n] = which;
                xb[n] = value;
                sd[n] = sigma;
                low[n] = std::min(lo, value);
                high[n] = std::max(hi, value);
                ++n;
            };
            if (!eb) {
                for (int k = 0; k < 4; ++k)
                    if (temperatureValid(background[k][pixel])) add(k, background[k][pixel], kTemperatureSd, kTemperatureLow, kTemperatureHigh);
            } else {
                add(kMoisture, moistureB, kMoistureSd, kMoistureLow, kMoistureHigh);
                if (leafArea > 0 && (entry->photosynthesis.pathway == 3 || entry->photosynthesis.pathway == 4))
                    add(kVcmax, vcmaxB, std::max(kVcmaxMinSd, kVcmaxRelativeSd * vcmaxB), kVcmaxLow, kVcmaxHigh);
            }
            if (opticalPixel) add(kLai, leafArea, std::max(kLaiMinSd, kLaiRelativeSd * leafArea), 0.0, kLaiHigh);
            backgroundControls[0][pixel] = static_cast<float>(moistureB);
            backgroundControls[1][pixel] = static_cast<float>(vcmaxB);
            backgroundControls[2][pixel] = leafArea;
            if (obs.empty() || n == 0) { ++noObservation; finish(kQaInputInvalid | kQaNoObservation); qaBands[5][pixel] = 0; continue; }

            // 步骤 3e：若提供观测协方差栅格，则对主视角热红外观测（不含附加视角）取子矩阵并求精度矩阵；
            //         其余观测各自独立，权重为 1/sigma²（applyPrecision 把残差乘以 R^-1）。
            // Covariance covers the primary thermal observation. Additional views
            // and optical observations remain independent blocks with their own sigma.
            std::vector<size_t> correlated;
            std::vector<double> precision;
            if (!covarianceRaster.empty()) {
                for (size_t i = 0; i < obs.size(); ++i)
                    if (obs[i].thermal && obs[i].source != Source::ViewBrightness) correlated.push_back(i);
                const int nc = static_cast<int>(correlated.size());
                std::vector<double> covariance(nc * nc);
                for (int i = 0; i < nc; ++i) for (int j = 0; j < nc; ++j)
                    covariance[i * nc + j] = covarianceRaster[obs[correlated[i]].band * dataBands + obs[correlated[j]].band][pixel];
                if (!observationPrecision(covariance, nc, precision)) {
                    ++failed; finish(kQaInputInvalid | kQaIllConditioned); continue;
                }
            }
            const auto applyPrecision = [&](const std::vector<double>& values) {
                std::vector<double> weighted(values.size());
                for (size_t i = 0; i < values.size(); ++i) weighted[i] = values[i] / (obs[i].sigma * obs[i].sigma);
                const size_t nc = correlated.size();
                for (size_t i = 0; i < nc; ++i) {
                    weighted[correlated[i]] = 0.0;
                    for (size_t j = 0; j < nc; ++j) weighted[correlated[i]] += precision[i * nc + j] * values[correlated[j]];
                }
                return weighted;
            };

            Matrix backgroundInverse{};
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    const bool temperatures = kindOf[i] < 4 && kindOf[j] < 4;
                    backgroundInverse[i][j] = i == j ? sd[i] * sd[i]
                                            : temperatures ? kTemperatureCorrelation * sd[i] * sd[j] : 0.0;
                }
            if (!invert(backgroundInverse, n)) { ++failed; finish(kQaIllConditioned); continue; }

            // 步骤 3f：观测算子 evaluate(x)：在控制量 x 处计算每个观测的模型值 h 及其对控制量的
            //         偏导（Dual 数 Ad）。RT：温度即控制量；EB：重解能量平衡得到温度及对
            //         [湿度, Vcmax, LAI] 的梯度（隐函数求导）并按链式法则传入；
            //         随后按观测类型计算亮温、LST（地表辐射温度）、组分温度或冠层反射率。
            //         反射率中的土壤项由湿度查表（步长 1/kMoistureSteps）及中心差分斜率给出。
            //         任一值非有限即返回 false，调用方视为该候选状态不可用。
            // ---- observation operator with exact derivatives ----
            EbOutcome outcome;
            std::vector<Ad> h(obs.size());
            const auto evaluate = [&](const std::array<double, kMaxControls>& x) {
                const auto value = [&](int which, double fallback) {
                    const int c = index[which];
                    return c >= 0 ? Ad::seed(x[c], c) : Ad(fallback);
                };
                const Ad laiAd = value(kLai, leafArea);
                const Ad moistureAd = eb ? value(kMoisture, moistureB) : Ad(moistureB);
                std::array<Ad, 4> temperatures;
                if (!eb) {
                    for (int k = 0; k < 4; ++k) temperatures[k] = value(k, background[k][pixel]);
                } else {
                    const EbControls controls{moistureAd.v, index[kVcmax] >= 0 ? x[index[kVcmax]] : vcmaxB, laiAd.v};
                    if (!in.ebSolver || !in.ebSolver(pixel, controls, outcome)) return false;
                    if (!outcome.gradientValid) { flags |= kQaIllConditioned; return false; }
                    for (int k = 0; k < 4; ++k) {
                        Ad t(outcome.temperatures[k]);
                        for (int c = 0; c < n; ++c)
                            if (kindOf[c] >= kMoisture) t.d[c] = outcome.gradient[k][kindOf[c] - kMoisture];
                        temperatures[k] = t;
                    }
                }
                std::vector<Ad> sceneBrightness, sceneSurface, viewBrightness, viewSurface;
                bool sceneDone = false;
                std::vector<std::vector<Ad>> views(as.views.size());
                double diffuse = -1.0;
                for (size_t i = 0; i < obs.size(); ++i) {
                    const Observation& o = obs[i];
                    switch (o.source) {
                        case Source::SceneBrightness:
                            if (!sceneDone) {
                                if (!thermalPixel<Ad>(scene, temperatures, laiAd, moistureAd, brightnessWavelengths, sceneBrightness, sceneSurface)) return false;
                                sceneDone = true;
                            }
                            h[i] = sceneBrightness[o.band];
                            break;
                        case Source::ViewBrightness:
                            if (views[o.view].empty() &&
                                !thermalPixel<Ad>(viewScenes[o.view], temperatures, laiAd, moistureAd, brightnessWavelengths, views[o.view], viewSurface))
                                return false;
                            h[i] = views[o.view][o.band];
                            break;
                        case Source::Lst:
                            if (!thermalPixel<Ad>(scene, temperatures, laiAd, moistureAd, lstWavelength, sceneBrightness, sceneSurface)) return false;
                            h[i] = sceneSurface[0];
                            break;
                        case Source::Component:
                            if (!ad::isfinite(temperatures[o.band])) return false;
                            h[i] = temperatures[o.band];
                            break;
                        case Source::Reflectance: {
                            if (diffuse < 0.0) {
                                diffuse = eb && in.diffuse && std::isfinite((*in.diffuse)[pixel]) ? (*in.diffuse)[pixel] : config.diffuseFraction;
                                if (!(diffuse >= 0.0 && diffuse <= 1.0))
                                    diffuse = requestedDiffuseFraction(
                                        eb && in.shortwave && std::isfinite((*in.shortwave)[pixel]) ? (*in.shortwave)[pixel] : 0.0,
                                        sun.zenith, config.diffuseFraction, dayOfYear);  // FIX(2026-11-21 C7): E0 of the overpass day
                            }
                            const auto& leafR = in.leafBandReflectance.at(code);
                            const auto& leafT = in.leafBandTransmittance.at(code);
                            Ad soil(entry->groundReflectance);
                            if (entry->groundKind == GroundKind::Soil) {
                                const double m = std::clamp(moistureAd.v, 0.0, 1.0);
                                const int step = static_cast<int>(std::lround(m * kMoistureSteps));
                                const auto& table = in.soilBandReflectance;
                                if (table[step].empty()) return false;
                                const int lower = std::max(step - 1, 0), upper = std::min(step + 1, kMoistureSteps);
                                double slope = 0.0;
                                if (!table[lower].empty() && !table[upper].empty() && upper > lower)
                                    slope = (table[upper][o.band] - table[lower][o.band]) * kMoistureSteps / (upper - lower);
                                soil = Ad(table[step][o.band]) + slope * (moistureAd - static_cast<double>(step) / kMoistureSteps);
                            }
                            const double lr = leafR[o.band], lt = std::min(leafT[o.band], 1.0 - leafR[o.band]);
                            h[i] = canopyReflectanceT<Ad>(laiAd, entry->canopy.hotspot, sun.zenith, sun.azimuth, std::min<double>(vz, 89.9),
                                                          va, lr, lt, soil, diffuse, sun.zenith < 90.0);
                            break;
                        }
                    }
                    if (!ad::isfinite(h[i])) return false;
                }
                return true;
            };

            // 步骤 3g：MAP 高斯-牛顿（围绕背景的增量形式）：
            //         x_{k+1} = xb + (B^-1 + H^T R^-1 H)^-1 H^T R^-1 [ y - h(x_k) + H (x_k - xb) ]，
            //         再限幅到 [low, high]；代价 J = (y-h)^T R^-1 (y-h) + (x-xb)^T B^-1 (x-xb)。
            //         收敛判据：各控制量步长 / 其先验 sd 的最大值 < kStepTolerance。
            //         局部函数 mapCost 计算上述代价 J。
            // ---- MAP Gauss-Newton (incremental form around the background) ----
            const size_t m = obs.size();
            std::array<double, kMaxControls> x = xb, xGood = xb;
            std::vector<Ad> hGood;
            EbOutcome outcomeGood;
            bool converged = false, ok = true, conditioned = true;
            int iterations = 0;
            double omb = 0.0;
            int thermalCount = 0;
            Matrix posterior{};
            // FIX(2026-11-21 S-4/S-5): (a) every GN step is accepted only after the observation operator evaluated at the
            // candidate and the MAP cost J decreased (backtracking fractions 1, 1/2, 1/4 as the StreamEarth native-MAP
            // tool); a candidate whose evaluation fails is treated as a rejected step, not as a usable analysis.
            // (b) The analysis (x, h, EB outcome) is therefore always one consistently evaluated state; a pixel is
            // flagged RetrievalFailed and NOT analysed only when the operator fails at the background. Previously a
            // failed evaluate() at iteration >= 1 left ok = true, the pixel was counted as analysed and written with
            // the previous iterate while also carrying RetrievalFailed.
            const auto mapCost = [&](const std::array<double, kMaxControls>& state, const std::vector<Ad>& values) {
                std::vector<double> residual(m);
                for (size_t i = 0; i < m; ++i) residual[i] = obs[i].y - values[i].v;
                const auto weighted = applyPrecision(residual);
                double total = 0.0;
                for (size_t i = 0; i < m; ++i) total += residual[i] * weighted[i];
                for (int c = 0; c < n; ++c)
                    for (int d = 0; d < n; ++d) total += (state[c] - xb[c]) * backgroundInverse[c][d] * (state[d] - xb[d]);
                return total;
            };
            double costGood = 0.0, costBackground = std::numeric_limits<double>::quiet_NaN();
            bool stalled = false, stepFailed = false;  // FIX(2026-11-21 S-5 QA): stop reasons
            int acceptedSteps = 0;
            // 先在背景状态 xb 处评估算子：失败则该像元不分析（kQaRetrievalFailed）；
            // 成功则记录 O−B（观测减背景模拟，仅热红外）与背景代价 J_b。
            if (!evaluate(x)) ok = false;
            else {
                xGood = x; hGood = h; outcomeGood = outcome;
                for (size_t i = 0; i < m; ++i)
                    if (obs[i].thermal) { omb += obs[i].y - h[i].v; ++thermalCount; }
                costGood = mapCost(xGood, hGood);
                costBackground = costGood;
            }
            // 迭代循环：每步由当前已接受状态 xGood 线性化（雅可比 H = hs[i].d），解增量并限幅。
            for (int iteration = 0; ok && iteration < maxIterations; ++iteration) {
                const std::array<double, kMaxControls>& xs = xGood;
                const std::vector<Ad>& hs = hGood;
                Matrix a = backgroundInverse;
                std::array<double, kMaxControls> g{};
                std::vector<double> innovation(m);
                for (size_t i = 0; i < m; ++i) {
                    innovation[i] = obs[i].y - hs[i].v;
                    for (int c = 0; c < n; ++c) innovation[i] += hs[i].d[c] * (xs[c] - xb[c]);
                }
                const auto weightedInnovation = applyPrecision(innovation);
                for (int c = 0; c < n; ++c) {
                    std::vector<double> column(m);
                    for (size_t i = 0; i < m; ++i) column[i] = hs[i].d[c];
                    const auto weightedColumn = applyPrecision(column);
                    for (size_t i = 0; i < m; ++i) {
                        g[c] += hs[i].d[c] * weightedInnovation[i];
                        for (int d = 0; d < n; ++d) a[d][c] += hs[i].d[d] * weightedColumn[i];
                    }
                }
                if (!invert(a, n)) { conditioned = false; break; }
                // 解高斯-牛顿增量：a 已被求逆为后验协方差 (B^-1 + H^T R^-1 H)^-1，g 为 H^T R^-1 创新量；
                // 候选状态 next 限幅在 [low, high]，step 为各控制量变化相对先验 sd 的最大值（无量纲）。
                double step = 0.0;
                std::array<double, kMaxControls> next{};
                for (int c = 0; c < n; ++c) {
                    double increment = 0.0;
                    for (int d = 0; d < n; ++d) increment += a[c][d] * g[d];
                    next[c] = std::clamp(xb[c] + increment, low[c], high[c]);
                    step = std::max(step, std::abs(next[c] - xs[c]) / sd[c]);
                }
                ++iterations;
                const unsigned flagsBefore = flags;
                if (step < kStepTolerance) {
                    // Converged: take the (tiny) final step when the operator evaluates there.
                    if (evaluate(next)) { xGood = next; hGood = h; outcomeGood = outcome; costGood = mapCost(xGood, hGood); }
                    else flags = flagsBefore;
                    converged = true;
                    break;
                }
                // 回溯线搜索：依次尝试完整步长、1/2、1/4 步长，仅当算子可评估且代价 J 下降（容差
                // 1e-12·(1+J)）才接受；全部失败则按“评估成功但无更低代价”（stalled）或
                // “每次试探算子都失败”（stepFailed）分类并结束迭代。
                bool accepted = false, evaluated = false;
                unsigned failureFlags = 0;
                for (const double fraction : {1.0, 0.5, 0.25}) {
                    std::array<double, kMaxControls> candidate = xs;
                    for (int c = 0; c < n; ++c) candidate[c] = xs[c] + fraction * (next[c] - xs[c]);
                    if (!evaluate(candidate)) { failureFlags |= flags & ~flagsBefore; flags = flagsBefore; continue; }
                    evaluated = true;
                    const double candidateCost = mapCost(candidate, h);
                    if (std::isfinite(candidateCost) && candidateCost < costGood - 1e-12 * (1.0 + costGood)) {
                        xGood = candidate; hGood = h; outcomeGood = outcome; costGood = candidateCost;
                        accepted = true;
                        break;
                    }
                }
                // FIX(2026-11-21 S-5 QA): classify the stop instead of calling every rejected step "not converged".
                if (!accepted) {
                    if (evaluated) stalled = true;                        // evaluated, but no lower cost along the GN direction
                    else { stepFailed = true; flags |= failureFlags; }    // the operator failed on every trial step
                    break;
                }
                ++acceptedSteps;
            }
            // 步骤 3h：迭代结束后的状态与停止原因分类：背景处算子失败 → 未分析；
            //         否则取最后被接受（代价最低且已一致评估）的状态作为分析结果；
            //         按 converged / stalled / notUpdated / 迭代上限·奇异·算子失败 统计并置对应 QA 位。
            if (!ok || hGood.empty()) { ++failed; finish(flags | kQaRetrievalFailed); continue; }
            if (!conditioned) flags |= kQaIllConditioned;
            // Analysis state: the last accepted (evaluated) iterate = the lowest-cost evaluated state.
            x = xGood;
            h = hGood;
            outcome = outcomeGood;
            // FIX(2026-11-21 S-5 QA): bit 10 / bit 11 for a line-search stop (not failures, outside the default reject
            // mask); bit 9 only for the iteration limit, a singular normal matrix or an operator failure on every trial.
            if (converged) ++stopConverged;
            else if (stalled && acceptedSteps > 0) { flags |= kQaLineSearchStalled; ++stopStalled; }
            else if (stalled) { flags |= kQaNotUpdated; ++stopNotUpdated; }
            else {
                flags |= kQaNotConverged;
                if (stepFailed) ++stopStepFailed;
                else if (!conditioned) ++stopSingular;
                else ++stopIterationLimit;
            }
            qaBands[7][pixel] = static_cast<float>(costBackground);  // FIX(2026-11-21 S-5 QA): MAP cost at the background
            qaBands[8][pixel] = static_cast<float>(costGood);        //                          and at the analysis
            // (Former loop and post-loop re-evaluation replaced by the above — FIX(2026-11-21 S-4/S-5).)

            // 步骤 3i：分析点处的后验协方差 P = (B^-1 + H^T R^-1 H)^-1，及信号自由度
            //         DFS = n − tr(P B^-1)（n 为控制量个数）；求逆失败则置 kQaIllConditioned。
            // Posterior covariance and DFS at the analysis.
            Matrix a = backgroundInverse;
            for (int c = 0; c < n; ++c) {
                std::vector<double> column(m);
                for (size_t i = 0; i < m; ++i) column[i] = h[i].d[c];
                const auto weighted = applyPrecision(column);
                for (size_t i = 0; i < m; ++i)
                    for (int d = 0; d < n; ++d) a[d][c] += h[i].d[d] * weighted[i];
            }
            double dfs = std::numeric_limits<double>::quiet_NaN();
            if (invert(a, n)) {
                posterior = a;
                double trace = 0.0;
                for (int c = 0; c < n; ++c)
                    for (int d = 0; d < n; ++d) trace += posterior[c][d] * backgroundInverse[d][c];
                dfs = n - trace;
            } else flags |= kQaIllConditioned;

            // 步骤 3j：写出分析组分温度与不确定度（1-sigma）。RT：控制量温度取后验方差对角元，
            //         无控制量的组分取先验 sd；EB：温度为控制量的函数，方差 = g^T P g，
            //         g 为 dT/d[湿度, Vcmax, LAI]。分析温度超出 [kValidLow, kValidHigh]、
            //         或控制量落在搜索边界（而背景不在边界）时置 kQaOutOfRange。
            // Analysis components and their uncertainty.
            std::array<double, 4> analysisT{};
            if (!eb) {
                for (int k = 0; k < 4; ++k) analysisT[k] = index[k] >= 0 ? x[index[k]] : background[k][pixel];
            } else analysisT = outcome.temperatures;
            for (int k = 0; k < 4; ++k) {
                result.components[k][pixel] = static_cast<float>(analysisT[k]);
                if (!std::isfinite(analysisT[k])) continue;
                if (analysisT[k] < kValidLow || analysisT[k] > kValidHigh) flags |= kQaOutOfRange;
                if (!std::isfinite(dfs)) continue;
                double variance = 0.0;
                if (!eb) variance = index[k] >= 0 ? posterior[index[k]][index[k]] : kTemperatureSd * kTemperatureSd;
                else {
                    std::array<double, kMaxControls> gk{};
                    for (int c = 0; c < n; ++c)
                        if (kindOf[c] >= kMoisture) gk[c] = outcome.gradient[k][kindOf[c] - kMoisture];
                    for (int c = 0; c < n; ++c)
                        for (int d = 0; d < n; ++d) variance += gk[c] * posterior[c][d] * gk[d];
                }
                uncertainty[k][pixel] = static_cast<float>(std::sqrt(std::max(variance, 0.0)));
            }
            for (int c = 0; c < n; ++c) {
                if (std::isfinite(dfs)) uncertainty[kindOf[c]][pixel] = static_cast<float>(std::sqrt(std::max(posterior[c][c], 0.0)));
                const bool atBound = (x[c] <= low[c] + 1e-9 && xb[c] > low[c] + 1e-9) || (x[c] >= high[c] - 1e-9 && xb[c] < high[c] - 1e-9);
                if (atBound) flags |= kQaOutOfRange;
            }
            if (index[kMoisture] >= 0) result.soilMoisture[pixel] = static_cast<float>(x[index[kMoisture]]);
            if (index[kVcmax] >= 0) result.vcmax[pixel] = static_cast<float>(x[index[kVcmax]]);
            if (index[kLai] >= 0) result.lai[pixel] = static_cast<float>(x[index[kLai]]);
            if (eb) for (int f = 0; f < 4; ++f) fluxes[f][pixel] = static_cast<float>(outcome.fluxes[f]);

            // 步骤 3k：统计量与 QA 波段：O−A（观测减分析模拟）均值与 rms、DFS、观测个数、迭代次数；
            //         DFS < kLowSupportDfs 标记观测支持不足；按行累加以保证并行下结果可复现。
            double oma = 0.0, omaSquares = 0.0;
            for (size_t i = 0; i < m; ++i)
                if (obs[i].thermal) { const double r = obs[i].y - h[i].v; oma += r; omaSquares += r * r; }
            if (std::isfinite(dfs) && dfs < kLowSupportDfs) { flags |= kQaLowSupport; ++lowSupport; }
            if (thermalCount > 0) {
                qaBands[1][pixel] = static_cast<float>(omb / thermalCount);
                qaBands[2][pixel] = static_cast<float>(oma / thermalCount);
                qaBands[3][pixel] = static_cast<float>(std::sqrt(omaSquares / thermalCount));
                rowOmb[row] += omb / thermalCount;
                rowOma[row] += oma / thermalCount;
                ++thermalPixels;
            }
            qaBands[4][pixel] = static_cast<float>(dfs);
            qaBands[5][pixel] = static_cast<float>(m);
            qaBands[6][pixel] = static_cast<float>(iterations);
            finish(flags);
            result.analysed[pixel] = 1;
            ++analysed;
            observationsUsed += static_cast<long long>(m);
            if (std::isfinite(dfs)) rowDfs[row] += dfs;
            if (flags & kQaNotConverged) ++notConverged;  // FIX(2026-11-21 S-5 QA): bit 9 only (stalls / not updated counted apart)
            if (flags & kQaIllConditioned) ++illConditioned;
        }
        // 进度：每完成约 rowCount/40 行由主线程报告一次。
        const int done = ++rowsDone;
        if (isMainThread() && done >= progressMark + std::max(1, rowCount / 40) && (progressMark = done, true))
            progress(in.progressStart + in.progressSpan * done / rowCount, "同化分析");
        } catch (...) { errorsAssimilation.capture(); }
    }
    // 步骤 4：并行结束后，按行序合并部分和，再统一重新抛出并行区内捕获的异常。
    for (size_t row = 0; row < rowOmb.size(); ++row) { sumOmb += rowOmb[row]; sumOma += rowOma[row]; sumDfs += rowDfs[row]; }
    errorsAssimilation.rethrow("同化");

    // 步骤 5：写出产品（GeoTIFF，写入 config.outputDir）：
    //         analysis/background 组分温度、analysis_uncertainty、analysis_qa（9 个波段，含 MAP 代价）；
    //         EB 或有光学观测时另写 analysis/background_controls（土壤湿度、Vcmax、LAI；
    //         已分析像元缺失处用背景填补）；EB 另写 analysis_fluxes（Rn、H、LE、G，W m-2）。
    // ---- outputs ----
    const auto& dir = config.outputDir;
    const std::vector<std::string> componentNames{"Tss sunlit soil temperature (K)", "Tsh shaded soil temperature (K)",
                                                  "Tcs sunlit leaf temperature (K)", "Tch shaded leaf temperature (K)"};
    std::vector<std::string> analysisNames, backgroundNames;
    for (const auto& name : componentNames) { analysisNames.push_back("analysis " + name); backgroundNames.push_back("background " + name); }
    writeTiff(dir / "analysis_components.tif", grid, {result.components.begin(), result.components.end()}, analysisNames);
    writeTiff(dir / "background_components.tif", grid, {background.begin(), background.end()}, backgroundNames);
    writeTiff(dir / "analysis_uncertainty.tif", grid, uncertainty,
              {"Tss analysis 1-sigma (K)", "Tsh analysis 1-sigma (K)", "Tcs analysis 1-sigma (K)", "Tch analysis 1-sigma (K)",
               "soil moisture analysis 1-sigma (m3 m-3)", "Vcmax25 analysis 1-sigma (umol m-2 s-1)", "LAI analysis 1-sigma"});
    writeTiff(dir / "analysis_qa.tif", grid, qaBands,
              {"QA flags", "mean O-B thermal (K)", "mean O-A thermal (K)", "RMS O-A thermal (K)",
               "degrees of freedom for signal", "observations used", "iterations",
               "MAP cost at background", "MAP cost at analysis"});  // FIX(2026-11-21 S-5 QA): bands 8-9 appended
    result.products = {"analysis_components.tif", "background_components.tif", "analysis_uncertainty.tif", "analysis_qa.tif"};
    if (eb || withOptical) {
        std::vector<std::vector<float>> controls{result.soilMoisture, result.vcmax, result.lai};
        for (size_t p = 0; p < pixels; ++p) {
            if (!result.analysed[p]) continue;
            if (!std::isfinite(controls[0][p])) controls[0][p] = backgroundControls[0][p];
            if (!std::isfinite(controls[1][p])) controls[1][p] = backgroundControls[1][p];
            if (!std::isfinite(controls[2][p])) controls[2][p] = backgroundControls[2][p];
        }
        const std::vector<std::string> controlNames{"soil moisture (m3 m-3)", "Vcmax25 (umol m-2 s-1)", "LAI"};
        std::vector<std::string> a, b;
        for (const auto& name : controlNames) { a.push_back("analysis " + name); b.push_back("background " + name); }
        writeTiff(dir / "analysis_controls.tif", grid, controls, a);
        writeTiff(dir / "background_controls.tif", grid, backgroundControls, b);
        result.products.push_back("analysis_controls.tif");
        result.products.push_back("background_controls.tif");
    }
    if (eb) {
        writeTiff(dir / "analysis_fluxes.tif", grid, fluxes,
                  {"analysis net radiation (W m-2)", "analysis sensible heat (W m-2)", "analysis latent heat (W m-2)",
                   "analysis ground heat (W m-2)"});
        result.products.push_back("analysis_fluxes.tif");
    }

    // 步骤 6：汇总统计 summary（JSON）：方法说明、使用的观测与默认误差、控制量列表、
    //         各类像元计数、停止原因、QA 位说明、平均 O−B/O−A/DFS 与各产品波段统计。
    CPLJSONObject summary;
    summary.Add("observation", kind == AssimilationObservation::Brightness ? "brightness" : kind == AssimilationObservation::Lst ? "lst" : "components");
    summary.Add("background", eb ? "eb" : "rt");
    summary.Add("method", "MAP Gauss-Newton per pixel; Dual-number observation Jacobian; tabulated optical moisture slopes are local approximations" +
                std::string(eb ? "; implicit overpass EB derivative conditional on history, 1/L and convective gust (all frozen at the converged state, so quasi-Newton; the aerodynamic resistances ARE differentiated w.r.t. LAI since 2026-11-24 M3; not a full trajectory derivative)" : ""));
    summary.Add("observationLevel", as.observationLevel);
    summary.Add("uncertaintyRaster", as.uncertainty);
    summary.Add("covarianceRaster", as.covariance);
    summary.Add("qaRaster", as.qa);
    summary.Add("rejectQaBits", static_cast<int>(as.rejectQaBits));
    CPLJSONArray sourceIds, priorIds;
    for (const auto& id : as.sourceIds) sourceIds.Add(id);
    for (const auto& id : as.priorSourceIds) priorIds.Add(id);
    summary.Add("sourceIds", sourceIds);
    summary.Add("priorSourceIds", priorIds);
    CPLJSONArray controlList;
    if (eb) { controlList.Add(kControlNames[kMoisture]); controlList.Add(kControlNames[kVcmax]); }
    else for (int k = 0; k < 4; ++k) controlList.Add(kControlNames[k]);
    if (withOptical) controlList.Add(kControlNames[kLai]);
    summary.Add("controls", controlList);
    CPLJSONObject defaults;
    defaults.Add("sigmaBrightnessK", as.sigmaBrightnessK);
    defaults.Add("sigmaLstK", as.sigmaLstK);
    defaults.Add("sigmaComponentK", as.sigmaComponentK);
    defaults.Add("sigmaReflectance", as.sigmaReflectance);
    defaults.Add("backgroundTemperatureSdK", kTemperatureSd);
    defaults.Add("backgroundCorrelation", kTemperatureCorrelation);
    defaults.Add("backgroundMoistureSd", kMoistureSd);
    defaults.Add("backgroundVcmaxRelativeSd", kVcmaxRelativeSd);
    defaults.Add("backgroundLaiRelativeSd", kLaiRelativeSd);
    defaults.Add("lstWavelengthUm", as.lstWavelengthUm);
    defaults.Add("maxIterations", maxIterations);
    summary.Add("defaults", defaults);
    summary.Add("views", static_cast<int>(as.views.size()) + 1);
    summary.Add("optical", withOptical);
    summary.Add("analysedPixels", static_cast<GInt64>(analysed));
    summary.Add("noObservationPixels", static_cast<GInt64>(noObservation));
    summary.Add("failedPixels", static_cast<GInt64>(failed));
    summary.Add("notConvergedPixels", static_cast<GInt64>(notConverged));
    // FIX(2026-11-21 S-5 QA): why the Gauss-Newton iteration stopped (analysed pixels) and the QA bits involved.
    {
        CPLJSONObject stops;
        stops.Add("converged", static_cast<GInt64>(stopConverged));
        stops.Add("lineSearchStalled", static_cast<GInt64>(stopStalled));
        stops.Add("notUpdated", static_cast<GInt64>(stopNotUpdated));
        stops.Add("iterationLimit", static_cast<GInt64>(stopIterationLimit));
        stops.Add("singularNormalMatrix", static_cast<GInt64>(stopSingular));
        stops.Add("stepEvaluationFailed", static_cast<GInt64>(stopStepFailed));
        summary.Add("stopReasons", stops);
        summary.Add("qaBits", "0 input invalid, 1 cloud, 2 out of range, 3 retrieval failed (not analysed), 4 ill-conditioned, "
                              "8 low support, 9 not converged (iteration limit / singular / operator failed on every trial step), "
                              "10 line-search stalled (cost lowered, best state kept), 11 not updated (analysis = background); "
                              "default reject mask 895 = bits 0-6, 8, 9");
    }
    summary.Add("illConditionedPixels", static_cast<GInt64>(illConditioned));
    summary.Add("lowSupportPixels", static_cast<GInt64>(lowSupport));
    summary.Add("observationsUsed", static_cast<GInt64>(observationsUsed));
    if (thermalPixels > 0) {
        summary.Add("meanOmbK", sumOmb / thermalPixels);
        summary.Add("meanOmaK", sumOma / thermalPixels);
    }
    if (analysed > 0) summary.Add("meanDegreesOfFreedom", sumDfs / analysed);
    CPLJSONArray productList;
    for (const auto& name : result.products) productList.Add(name);
    summary.Add("products", productList);
    CPLJSONArray stats;
    for (int k = 0; k < 4; ++k) stats.Add(bandStatistics(analysisNames[k], result.components[k]));
    stats.Add(bandStatistics("degrees of freedom for signal", qaBands[4]));
    stats.Add(bandStatistics("mean O-B thermal (K)", qaBands[1]));
    stats.Add(bandStatistics("mean O-A thermal (K)", qaBands[2]));
    summary.Add("statistics", stats);
    result.summary = summary;

    // 步骤 7：向日志输出一行汇总（分析/无观测/失败/未收敛像元数、平均 O−B、O−A、DFS）。
    log("同化（" + std::string(eb ? "EB 背景：土壤水分 + Vcmax" : "RT 背景：四组分温度") + (withOptical ? " + LAI" : "") + "）：分析 " +
        std::to_string(analysed) + " 个像元，无观测 " + std::to_string(noObservation) + "，失败 " + std::to_string(failed) +
        "，未收敛 " + std::to_string(notConverged) +
        // FIX(2026-11-21 S-5 QA): stop reasons
        "（收敛 " + std::to_string(stopConverged) + "，线搜索停滞 " + std::to_string(stopStalled) + "，未更新 " +
        std::to_string(stopNotUpdated) + "，迭代上限 " + std::to_string(stopIterationLimit) + "）" +
        (thermalPixels > 0 ? "，平均 O−B " + format(sumOmb / thermalPixels) + " K，O−A " + format(sumOma / thermalPixels) + " K" : "") +
        (analysed > 0 ? "，平均 DFS " + format(sumDfs / analysed) : ""));
    if (config.terrainRadiation == TerrainRadiation::Hill || config.terrainRadiation == TerrainRadiation::Ridge)
        log("同化：复合地形像元按与成像相同的面元合成计算观测算子");
    return result;
}

}  // namespace stream
