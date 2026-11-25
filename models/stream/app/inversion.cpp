// ==========================================================================
// 文件：inversion.cpp
// 功能：STREAM 热红外温度反演：LST（每像元一个辐射温度）与 LSCT（四组分温度），
//       观测算子与正向成像共用 thermal_view.cpp 的四组分权重
// 主要模块/步骤：
//   1. 数值工具：Planck 导数、小矩阵求逆（Gauss-Jordan）、Sample 观测样本与 model 观测模型
//   2. runInversion 步骤 1：读取观测栅格（亮温/LST、可选视角、掩膜）
//   3. runInversion 步骤 2：逐像元准备样本（太阳位置、反射天空、四组分权重、QA 初值）
//   4. runInversion 步骤 3a（LST）：加权 Gauss-Newton，发射率误差并入观测方差
//   5. runInversion 步骤 3b（LSCT）：MAP Gauss-Newton + 回溯线搜索，可选 s×s 同地类窗口
//   6. 写出 GeoTIFF 产品与 summary（计数、统计、停止原因），汇总日志
// 数据流：image.cpp 构造 InversionInputs（栅格、地形、反射天空）与背景组分温度
//         → runInversion → lst*.tif / lsct*.tif + summary；LSCT 的后验组分温度回传给成像
// 约定：温度 K；波长 μm；Planck 辐亮度 W m-2 sr-1 μm-1；组分顺序 [Tss, Tsh, Tcs, Tch]
//       （土壤光照/阴影、叶片光照/阴影）；QA 位定义见 inversion.h 与 README“温度反演”
//       并行：OpenMP 按行并行，每行只由一个线程写入，行级统计量再按行序求和以保证结果可复现
// ==========================================================================

#include "inversion.h"
#include "parallel_error.h"
#include "../physics/brightness.h"
#include "../physics/radiosity.h"
#include "../physics/soil_emissivity.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace stream {
namespace {

namespace fs = std::filesystem;
// 常量：组分数；迭代的温度搜索域 150–400 K；产品有效范围 200–380 K（与 LS4T LSCT 一致）；
// kAbsentWeight：窗口内某组分权重和低于此值时视为未被观测，不参与 LSCT 控制量。
constexpr int kComponents = 4;
constexpr double kMinTemperature = 150.0, kMaxTemperature = 400.0;    // search domain (K)
constexpr double kValidLow = 200.0, kValidHigh = 380.0;               // product range (K), as LS4T LSCT
constexpr double kAbsentWeight = 1e-4;                                // summed weight below which a component is unobserved

using Matrix = std::array<std::array<double, kComponents>, kComponents>;

// 作用：输出进度行 `STREAM\tprogress\t<百分比>\t<阶段>`（服务端按此格式解析）。
void progress(double percent, const std::string& stage)
{
    std::cout << "STREAM\tprogress\t" << std::fixed << std::setprecision(1) << percent << '\t' << stage << '\n';
}

void log(const std::string& text) { std::cout << "[STREAM] " << text << '\n'; }

std::string format(double value, int digits = 2)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(digits) << value;
    return out.str();
}

bool isMainThread()
{
#ifdef _OPENMP
    return omp_get_thread_num() == 0;
#else
    return true;
#endif
}

// dB/dT of the Planck radiance [W m-2 sr-1 um-1 K-1].
double planckDerivative(double temperature, double wavelength)
{
    const double x = kPlanckC2 / (wavelength * temperature);
    const double e = std::exp(std::min(x, 700.0));
    return planck(temperature, wavelength) * x / temperature * e / (e - 1.0);
}

// 说明：奇异判据为相对阈值，主元 ≤ 1e-12 × 矩阵最大元即视为奇异（与同化一致）。
// In-place inverse of the leading n x n block (Gauss-Jordan, partial pivoting). False when singular.
bool invert(Matrix& a, int n)
{
    Matrix inverse{};
    for (int i = 0; i < n; ++i) inverse[i][i] = 1.0;
    double scale = 0.0;
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) scale = std::max(scale, std::abs(a[i][j]));
    if (!(scale > 0.0) || !std::isfinite(scale)) return false;
    for (int column = 0; column < n; ++column) {
        int pivot = column;
        for (int row = column + 1; row < n; ++row) if (std::abs(a[row][column]) > std::abs(a[pivot][column])) pivot = row;
        if (!(std::abs(a[pivot][column]) > 1e-12 * scale)) return false;
        std::swap(a[pivot], a[column]);
        std::swap(inverse[pivot], inverse[column]);
        const double divisor = a[column][column];
        for (int k = 0; k < n; ++k) { a[column][k] /= divisor; inverse[column][k] /= divisor; }
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

// One observation of one pixel: value, error, wavelength and the forward weights of its view geometry.
struct Sample {
    float value = kNaN, sigma = 0, wavelength = 0;
    float weights[kComponents]{};
    float fixedEmissivity = 1;  // sum of the weights with the fixed class soil emissivity (LST-product convention)
    // Brightness observations: isotropic sky radiance B(T_sky, λ) and the reflected sky term (1 − Σw)·B(T_sky, λ)
    // of the shared forward operator (thermal_view.cpp). LST observations are emitted-only (0).
    float sky = 0, reflected = 0;
    bool lst = false;
    bool valid() const { return std::isfinite(value); }
};

// 说明：LST 观测的辐亮度除以固定地类发射率 fixedEmissivity（LST 产品约定），
// 亮温观测则在辐亮度上加反射天空项 reflected；返回的 gradient 是模型亮温对各组分温度的偏导，
// 即 w_k B'(T_k) / (divisor · B'(模型亮温))。组分温度非有限或权重为 0 的组分不贡献。
// Model value of one sample for component temperatures `t` and its gradient d(model)/d(t_k).
bool model(const Sample& sample, const double t[kComponents], double& value, double gradient[kComponents])
{
    const double wavelength = sample.wavelength;
    double radiance = 0.0;
    for (int k = 0; k < kComponents; ++k)
        if (sample.weights[k] > 0.0f && std::isfinite(t[k])) radiance += sample.weights[k] * planck(t[k], wavelength);
    const double divisor = sample.lst ? std::max(static_cast<double>(sample.fixedEmissivity), 1e-6) : 1.0;
    if (!(radiance > 0.0)) return false;
    value = inversePlanck((radiance + sample.reflected) / divisor, wavelength);
    if (!std::isfinite(value)) return false;
    const double slope = divisor * planckDerivative(value, wavelength);
    for (int k = 0; k < kComponents; ++k)
        gradient[k] = sample.weights[k] > 0.0f && std::isfinite(t[k]) ? sample.weights[k] * planckDerivative(t[k], wavelength) / slope : 0.0;
    return true;
}

}  // namespace

// 模块：温度反演主函数（LST 或 LSCT 由 config.inversion.target 决定）。
// 输入：in（配置、地类表、栅格与地形、观测配置、反射天空设置）；background 为 LSCT 的背景四组分温度
//       （RT 输入或 EB 解，NaN 表示该组分无背景）。
// 输出：写入 config.outputDir 下的 GeoTIFF；返回 InversionResult（LSCT 的后验组分温度、产品名、summary）。
// 副作用：向 stdout 写进度行与 [STREAM] 日志；并行区内异常经 ParallelError 汇总后重抛。
InversionResult runInversion(const InversionInputs& in, const std::array<std::vector<float>, 4>& background)
{
    const ImageConfig& config = in.config;
    const InversionConfig& inv = config.inversion;
    const bool lsct = inv.target == InversionTarget::Lsct;
    const Grid& grid = in.grid;
    const int width = in.endColumn - in.startColumn, height = in.endRow - in.startRow;
    const size_t regionPixels = static_cast<size_t>(width) * height;
    const size_t observations = inv.observations.size();
    InversionResult result;

    progress(in.progressStart, lsct ? "组分温度反演：准备观测" : "地表温度反演：准备观测");
    log(std::string(lsct ? "LSCT 组分温度反演" : "LST 地表温度反演") + "：" + std::to_string(observations) + " 个热红外观测" +
        (lsct ? "，背景场 σ = " + format(inv.priorSd, 1) + " K、相关系数 " + format(inv.priorCorrelation, 2) +
                (inv.window > 1 ? "，" + std::to_string(inv.window) + "×" + std::to_string(inv.window) + " 同地类窗口" : "，逐像元")
              : "，发射率误差 " + format(inv.emissivitySd, 3)));
    // FIX(2026-11-21 S-1): the observation weights now come from the imaging operator (thermalSceneAt +
    // thermalSceneWeights), terrain included; the former warning "weights on a horizontal surface" no longer applies.
    if (config.terrainRadiation != TerrainRadiation::None)
        log(std::string("反演的观测权重按与正向成像相同的地形观测算子计算（") +
            (in.terrain ? "单坡/面元按局地几何，山包/山脊按面元合成" : "警告：未提供地形场，按水平地表") + "）");

    // 步骤 1：读取各观测的栅格（亮温/LST 值，可选逐观测视角天顶角/方位角）与云/无效掩膜；
    //         观测未给视角时沿用场景的 vza/vaa。
    // ---------- observations ----------
    std::vector<std::vector<float>> values(observations), obsVza(observations), obsVaa(observations);
    for (size_t j = 0; j < observations; ++j) {
        const auto& item = inv.observations[j];
        const std::string label = "观测 " + item.name;
        values[j] = fieldRaster(item.value, grid, label, 0, true);
        if (item.vza.present()) obsVza[j] = fieldRaster(item.vza, grid, label + " 天顶角", 0, true);
        if (item.vaa.present()) obsVaa[j] = fieldRaster(item.vaa, grid, label + " 方位角", 0, true);
    }
    std::vector<float> mask;
    if (inv.mask.present()) mask = fieldRaster(inv.mask, grid, "反演云/无效掩膜", 0, true);

    std::vector<Sample> samples(regionPixels * observations);
    std::vector<unsigned> flags(regionPixels, 0);
    std::vector<char> usable(regionPixels, 0);
    long long outOfRangeValues = 0;
    // 步骤 2：逐像元准备观测样本 Sample：太阳位置 → 反射天空温度 → 每个观测的四组分权重；
    //         同时登记 QA 初值（云/掩膜、输入无效）与可用像元标记。范围外的观测值（不在 150–400 K）被剔除并计数。
    ParallelError errorsPrepare;
    #pragma omp parallel for schedule(dynamic, 4) reduction(+:outOfRangeValues)
    for (int row = in.startRow; row < in.endRow; ++row) {
        if (errorsPrepare.failed()) continue;
        try {
        for (int column = in.startColumn; column < in.endColumn; ++column) {
            const size_t pixel = static_cast<size_t>(row) * grid.width + column;
            const size_t local = static_cast<size_t>(row - in.startRow) * width + (column - in.startColumn);
            const auto* entry = in.classes.find(in.type[pixel]);
            if (!entry || entry->surface == SurfaceKind::Fill) continue;
            if (!mask.empty() && std::isfinite(mask[pixel]) && mask[pixel] != 0.0f) { flags[local] |= kQaCloud; continue; }
            // 非植被地类允许 LAI 无效，按 0 处理；植被地类 LAI 无效则下面标记为输入无效。
            float leafArea = in.lai[pixel];
            if (entry->surface != SurfaceKind::Vegetation && !validLai(leafArea)) leafArea = 0.0f;  // FIX(2026-11-24 M1)
            const float lat = in.latitude[pixel], lon = in.longitude[pixel];
            if (!validLai(leafArea) || !std::isfinite(lat) || !std::isfinite(lon) ||
                lat < -90 || lat > 90 || lon < -180 || lon > 180) { flags[local] |= kQaInputInvalid; continue; }
            const Sun sun = solarPosition(in.overpass, lat, lon, in.dem[pixel]);
            if (!sun.ok) { flags[local] |= kQaInputInvalid; continue; }
            const int code = static_cast<int>(std::lround(in.type[pixel]));
            // FIX(2026-11-21 S-1): leaf / soil emissivity, Hapke table and hom3 nodes are now set up by setThermalMaterial
            // and thermalSceneWeights (the imaging operator) per observation; the local copies were removed.
            const double moisture = std::isfinite(in.soilMoisture[pixel]) ? static_cast<double>(in.soilMoisture[pixel])
                                                                           : static_cast<double>(in.classes.defaultSoilMoisture);
            // Sky temperature of the reflected term, identical to the forward imaging of this run. In RT "auto" mode
            // without component temperatures (plain LST retrieval) the observed brightness stands in for the air
            // temperature of the Swinbank clear-sky L↓.
            double skyTemperature = in.skyLongwave ? skyTemperatureFromLongwave(in.skyLongwave->at(pixel)) : NAN;
            if (!std::isfinite(skyTemperature) && in.skyFromObservations) {
                double sum = 0.0;
                int count = 0;
                for (size_t j = 0; j < observations; ++j) {
                    const float v = values[j][pixel];
                    if (std::isfinite(v) && v >= kMinTemperature && v <= kMaxTemperature) { sum += v; ++count; }
                }
                if (count) skyTemperature = skyTemperatureFromLongwave(swinbankLongwave(sum / count));
            }
            int valid = 0;
            for (size_t j = 0; j < observations; ++j) {
                Sample& sample = samples[local * observations + j];
                const auto& item = inv.observations[j];
                const float value = values[j][pixel];
                const float zenith = obsVza[j].empty() ? in.vza[pixel] : obsVza[j][pixel];
                const float azimuth = obsVaa[j].empty() ? in.vaa[pixel] : obsVaa[j][pixel];
                if (!std::isfinite(value)) continue;
                if (value < kMinTemperature || value > kMaxTemperature) { ++outOfRangeValues; continue; }
                if (!std::isfinite(zenith) || zenith < 0 || zenith >= 90 || !std::isfinite(azimuth)) continue;
                // FIX(2026-11-21 S-1): weights of the imaging operator (thermal_view.cpp) for this view, terrain included
                // (single slope / facet: local zenith and azimuth; hill / ridge: facet composite). They used to be the
                // horizontal thermalWeights, so retrieval and imaging disagreed in terrain mode.
                ThermalScene scene;
                const TerrainFields flat;
                if (!thermalSceneAt(in.terrain ? *in.terrain : flat, pixel, sun.zenith, sun.azimuth, zenith, azimuth, scene)) continue;
                setThermalMaterial(scene, config, *entry, in.soilEmissivity, code);
                std::array<double, kComponents> weights{}, fixed{};
                double reflectivity = 0.0;
                if (!thermalSceneWeights(scene, leafArea, moisture, weights, fixed, reflectivity)) continue;
                double fixedSum = 0.0;
                for (int k = 0; k < kComponents; ++k) {
                    sample.weights[k] = static_cast<float>(weights[k]);
                    fixedSum += fixed[k];
                }
                sample.fixedEmissivity = static_cast<float>(std::clamp(fixedSum, 1e-6, 1.0));
                sample.wavelength = static_cast<float>(item.wavelength);
                sample.sigma = static_cast<float>(item.sigma);
                sample.lst = item.kind == ObservationKind::Lst;
                if (!sample.lst && std::isfinite(skyTemperature)) {
                    sample.sky = static_cast<float>(planck(skyTemperature, item.wavelength));
                    // FIX(2026-11-21 S-1): mean directional reflectivity of the composite (= 1 - sum w when flat).
                    sample.reflected = static_cast<float>(std::clamp(reflectivity, 0.0, 1.0) * sample.sky);
                }
                sample.value = value;
                ++valid;
            }
            if (valid) usable[local] = 1;
            else flags[local] |= kQaInputInvalid;
        }
        } catch (...) { errorsPrepare.capture(); }
    }
    errorsPrepare.rethrow("反演输入准备");
    if (outOfRangeValues) log("警告：" + std::to_string(outOfRangeValues) + " 个观测值不在 150–400 K（单位须为 K），已剔除");

    // 步骤 3：反演。LST 与 LSCT 两个分支各自并行逐行处理；progressMark 仅主线程更新，
    //         用于约每 5% 行数上报一次进度。
    // ---------- retrieval ----------
    const double span = in.progressSpan;
    progress(in.progressStart + 0.1 * span, lsct ? "组分温度反演" : "地表温度反演");
    std::atomic<int> rowsDone{0}; int progressMark = 0;  // FIX(2026-11-24 E2): main thread only; reports whenever it finishes a row after enough rows are done
    long long retrieved = 0, failed = 0, notConverged = 0, lowSupport = 0, outOfRange = 0, illConditioned = 0;
    long long stopConverged = 0, stopStalled = 0, stopNotUpdated = 0, stopStepFailed = 0, stopIterationLimit = 0;  // FIX(2026-11-21 S-5 QA): LSCT stop reasons
    double dfsSum = 0.0, residualSum = 0.0;
    // FIX(2026-11-24 E3): the means are summed per row (one thread per row) and then in row order, so the last digit no
    // longer depends on the OpenMP reduction order.
    std::vector<double> rowResidual(static_cast<size_t>(std::max(grid.height, 0)), 0.0), rowDfs(rowResidual.size(), 0.0);

    // 分支 3a：LST。四组分取同一温度 t，对多个观测做加权 Gauss-Newton（标量问题）；
    // 产品：lst.tif、lst_uncertainty.tif、lst_qa.tif（QA、残差 RMS、使用观测数、有效发射率）。
    if (!lsct) {
        std::vector<float> lst(grid.size(), kNaN), sd(grid.size(), kNaN), qa(grid.size(), kNaN), residual(grid.size(), kNaN),
            used(grid.size(), kNaN), emissivity(grid.size(), kNaN);
        ParallelError errorsLst;
        #pragma omp parallel for schedule(dynamic, 4) reduction(+:retrieved, failed, notConverged, outOfRange)
        for (int row = in.startRow; row < in.endRow; ++row) {
            if (errorsLst.failed()) continue;
            try {
            for (int column = in.startColumn; column < in.endColumn; ++column) {
                const size_t pixel = static_cast<size_t>(row) * grid.width + column;
                const size_t local = static_cast<size_t>(row - in.startRow) * width + (column - in.startColumn);
                const auto* entry = in.classes.find(in.type[pixel]);
                if (!entry || entry->surface == SurfaceKind::Fill) continue;
                unsigned flag = flags[local];
                if (!usable[local]) { qa[pixel] = static_cast<float>(flag); continue; }
                const Sample* own = &samples[local * observations];
                // 初值：该像元所有有效观测值的平均。
                double t = 0.0;
                int count = 0;
                for (size_t j = 0; j < observations; ++j) if (own[j].valid()) { t += own[j].value; ++count; }
                t /= count;
                bool converged = false, ok = true;
                double information = 0.0;
                // 迭代：t ← t + Σ g(y−h)/V ÷ Σ g²/V，其中 g 为模型对 t 的导数、V 为观测方差加发射率误差项；
                // t 限制在搜索域 150–400 K；|步长| < tolerance 视为收敛，模型求值失败则标记失败。
                for (int iteration = 0; iteration < inv.maxIterations && ok; ++iteration) {
                    double numerator = 0.0;
                    information = 0.0;
                    const double temperatures[kComponents] = {t, t, t, t};
                    for (size_t j = 0; j < observations; ++j) {
                        const Sample& sample = own[j];
                        if (!sample.valid()) continue;
                        double value, gradient[kComponents];
                        if (!model(sample, temperatures, value, gradient)) { ok = false; break; }
                        const double slope = gradient[0] + gradient[1] + gradient[2] + gradient[3];
                        // Emissivity error: d(model)/d(epsilon) = (B(T) − B_sky) / (divisor * B'(model)); B_sky = 0 for LST.
                        const double divisor = sample.lst ? sample.fixedEmissivity : 1.0;
                        const double emissivityTerm = inv.emissivitySd * (planck(t, sample.wavelength) - sample.sky) /
                                                      (divisor * planckDerivative(value, sample.wavelength));
                        const double variance = static_cast<double>(sample.sigma) * sample.sigma + emissivityTerm * emissivityTerm;
                        numerator += slope * (sample.value - value) / variance;
                        information += slope * slope / variance;
                    }
                    if (!ok || !(information > 0.0)) { ok = false; break; }
                    const double step = numerator / information;
                    t = std::clamp(t + step, kMinTemperature, kMaxTemperature);
                    if (std::abs(step) < inv.tolerance) { converged = true; break; }
                }
                if (!ok) { qa[pixel] = static_cast<float>(flag | kQaRetrievalFailed); ++failed; continue; }
                if (!converged) { flag |= kQaNotConverged; ++notConverged; }
                // 解处复算：残差 RMS、不确定度所用信息量、有效发射率（取首个有效观测的权重和）。
                // Residual over all observations; effective emissivity of the first valid view.
                // FIX(2026-11-21 S-3): the information (sum slope^2 / variance) is re-evaluated at the returned t, so
                // the reported sigma belongs to the solution (it used to come from the iterate before the last update).
                double squares = 0.0, weightSum = -1.0, finalInformation = 0.0;
                int n = 0;
                const double temperatures[kComponents] = {t, t, t, t};
                for (size_t j = 0; j < observations; ++j) {
                    const Sample& sample = own[j];
                    if (!sample.valid()) continue;
                    if (weightSum < 0.0) { weightSum = 0.0; for (int k = 0; k < kComponents; ++k) weightSum += sample.weights[k]; }
                    double value, gradient[kComponents];
                    if (model(sample, temperatures, value, gradient)) {
                        squares += (sample.value - value) * (sample.value - value);
                        ++n;
                        const double slope = gradient[0] + gradient[1] + gradient[2] + gradient[3];
                        const double divisor = sample.lst ? sample.fixedEmissivity : 1.0;
                        const double emissivityTerm = inv.emissivitySd * (planck(t, sample.wavelength) - sample.sky) /
                                                      (divisor * planckDerivative(value, sample.wavelength));
                        finalInformation += slope * slope /
                                            (static_cast<double>(sample.sigma) * sample.sigma + emissivityTerm * emissivityTerm);
                    }
                }
                if (finalInformation > 0.0) information = finalInformation;
                // 解超出产品有效范围（200–380 K）时不输出 LST，只写 QA 位 2。不确定度 σ = 1/√信息量。
                if (t < kValidLow || t > kValidHigh) { qa[pixel] = static_cast<float>(flag | kQaOutOfRange); ++outOfRange; continue; }
                lst[pixel] = static_cast<float>(t);
                sd[pixel] = static_cast<float>(1.0 / std::sqrt(information));
                residual[pixel] = static_cast<float>(n ? std::sqrt(squares / n) : 0.0);
                used[pixel] = static_cast<float>(count);
                emissivity[pixel] = static_cast<float>(weightSum);
                qa[pixel] = static_cast<float>(flag);
                rowResidual[row] += residual[pixel];
                ++retrieved;
            }
            const int done = ++rowsDone;
            if (isMainThread() && done >= progressMark + std::max(1, height / 20) && (progressMark = done, true))
                progress(in.progressStart + span * (0.1 + 0.85 * done / height), "地表温度反演");
            } catch (...) { errorsLst.capture(); }
        }
        for (size_t row = 0; row < rowResidual.size(); ++row) residualSum += rowResidual[row];
        errorsLst.rethrow("地表温度反演");
        // 输出 LST 产品并登记统计量。
        const fs::path& dir = config.outputDir;
        writeTiff(dir / "lst.tif", grid, {lst}, {"LST (K)"});
        writeTiff(dir / "lst_uncertainty.tif", grid, {sd}, {"LST uncertainty 1-sigma (K)"});
        writeTiff(dir / "lst_qa.tif", grid, {qa, residual, used, emissivity},
                  {"quality flags", "residual RMS (K)", "observations used", "effective emissivity"});
        result.products = {"lst.tif", "lst_uncertainty.tif", "lst_qa.tif"};
        CPLJSONArray stats;
        stats.Add(bandStatistics("LST (K)", lst));
        stats.Add(bandStatistics("LST uncertainty 1-sigma (K)", sd));
        stats.Add(bandStatistics("residual RMS (K)", residual));
        result.summary.Add("statistics", stats);
    } else {
        // 分支 3b：LSCT。以背景组分温度为先验的 MAP Gauss-Newton，每像元最多 4 个控制量（被观测到的组分），
        // 可与同地类的 s×s 窗口内像元共享四个组分温度。
        // 产品：lsct.tif、lsct_uncertainty.tif、lsct_qa.tif（含 MAP 代价）、lsct_fractions.tif、
        // lsct_radiance_fractions.tif。
        // Background covariance B = sd^2 [(1 - r) I + r 1 1^T]; restricted to the observed components per pixel.
        const double variance = inv.priorSd * inv.priorSd;
        std::array<std::vector<float>, 4> posterior, sd;
        for (int k = 0; k < kComponents; ++k) {
            posterior[k].assign(grid.size(), kNaN);
            sd[k].assign(grid.size(), kNaN);
        }
        std::vector<float> qa(grid.size(), kNaN), residual(grid.size(), kNaN), dfs(grid.size(), kNaN), used(grid.size(), kNaN),
            iterations(grid.size(), kNaN);
        // FIX(2026-11-21 S-5 QA): MAP cost at the background and at the analysis (lsct_qa.tif bands 6-7, appended).
        std::array<std::vector<float>, 2> costs{std::vector<float>(grid.size(), kNaN), std::vector<float>(grid.size(), kNaN)};
        std::array<std::vector<float>, 4> shares, radianceShares;
        for (auto& band : shares) band.assign(grid.size(), kNaN);
        for (auto& band : radianceShares) band.assign(grid.size(), kNaN);
        const int half = inv.window / 2;
        ParallelError errorsLsct;
        #pragma omp parallel for schedule(dynamic, 2) reduction(+:retrieved, failed, notConverged, lowSupport, outOfRange, illConditioned, stopConverged, stopStalled, stopNotUpdated, stopStepFailed, stopIterationLimit)
        for (int row = in.startRow; row < in.endRow; ++row) {
            if (errorsLsct.failed()) continue;
            try {
            std::vector<const Sample*> set;
            for (int column = in.startColumn; column < in.endColumn; ++column) {
                const size_t pixel = static_cast<size_t>(row) * grid.width + column;
                const size_t local = static_cast<size_t>(row - in.startRow) * width + (column - in.startColumn);
                const auto* entry = in.classes.find(in.type[pixel]);
                if (!entry || entry->surface == SurfaceKind::Fill) continue;
                unsigned flag = flags[local];
                if (!usable[local]) { qa[pixel] = static_cast<float>(flag); continue; }
                // 步骤 3b-1：收集窗口内同地类的可用观测。
                // Observations of the window: same class, usable, sharing the four component temperatures.
                set.clear();
                const float code = in.type[pixel];
                for (int dr = -half; dr <= half; ++dr) for (int dc = -half; dc <= half; ++dc) {
                    const int r = row + dr, c = column + dc;
                    if (r < in.startRow || r >= in.endRow || c < in.startColumn || c >= in.endColumn) continue;
                    const size_t neighbour = static_cast<size_t>(r - in.startRow) * width + (c - in.startColumn);
                    if (!usable[neighbour] || in.type[static_cast<size_t>(r) * grid.width + c] != code) continue;
                    for (size_t j = 0; j < observations; ++j) {
                        const Sample& sample = samples[neighbour * observations + j];
                        if (sample.valid()) set.push_back(&sample);
                    }
                }
                // 步骤 3b-2：按权重和确定被观测到的组分（index），并取背景状态 xb 作为迭代初值；
                //         被观测组分缺少背景值时无法反演，置位 0 与位 3。
                // Observed components and the background state.
                double weightSum[kComponents]{};
                for (const Sample* sample : set) for (int k = 0; k < kComponents; ++k) weightSum[k] += sample->weights[k];
                int index[kComponents], m = 0;
                double xb[kComponents], x[kComponents];
                bool missingBackground = false;
                for (int k = 0; k < kComponents; ++k) {
                    xb[k] = background[k].empty() ? NAN : background[k][pixel];
                    x[k] = xb[k];
                    if (weightSum[k] > kAbsentWeight) {
                        if (!std::isfinite(xb[k])) missingBackground = true;
                        index[m++] = k;
                    }
                }
                if (m == 0 || missingBackground) { qa[pixel] = static_cast<float>(flag | kQaInputInvalid | kQaRetrievalFailed); ++failed; continue; }
                // 步骤 3b-3：背景协方差 B = σ²[(1−r)I + r11ᵀ] 取被观测组分子矩阵并求逆（此处先填 B 再原地求逆为 B⁻¹）。
                Matrix backgroundInverse{};
                for (int a = 0; a < m; ++a) for (int b = 0; b < m; ++b)
                    backgroundInverse[a][b] = variance * (a == b ? 1.0 : inv.priorCorrelation);
                if (!invert(backgroundInverse, m)) { qa[pixel] = static_cast<float>(flag | kQaIllConditioned | kQaRetrievalFailed); ++failed; ++illConditioned; continue; }

                // Hessian and gradient of J = sum (y - h)^2 / sigma^2 + (x - xb)^T B^-1 (x - xb) at x.
                const auto normal = [&](Matrix& hessian, double gradient[kComponents], double& squares) {
                    hessian = backgroundInverse;
                    for (int a = 0; a < m; ++a) {
                        gradient[a] = 0.0;
                        for (int b = 0; b < m; ++b) gradient[a] -= backgroundInverse[a][b] * (x[index[b]] - xb[index[b]]);
                    }
                    squares = 0.0;
                    for (const Sample* sample : set) {
                        double value, full[kComponents];
                        if (!model(*sample, x, value, full)) return false;
                        const double weight = 1.0 / (static_cast<double>(sample->sigma) * sample->sigma);
                        const double difference = sample->value - value;
                        squares += difference * difference;
                        for (int a = 0; a < m; ++a) {
                            gradient[a] += full[index[a]] * difference * weight;
                            for (int b = 0; b < m; ++b) hessian[a][b] += full[index[a]] * full[index[b]] * weight;
                        }
                    }
                    return true;
                };
                Matrix hessian;
                double gradient[kComponents], squares = 0.0;
                bool ok = true, converged = false;
                int iteration = 0;
                // FIX(2026-11-21 S-5): MAP cost J(x) = sum ((y - h)/sigma)^2 + (x - xb)^T B^-1 (x - xb) for the backtracking
                // line search; GN steps are accepted only when they lower J (fractions 1, 1/2, 1/4, as the StreamEarth
                // native-MAP tool), instead of being applied blindly with bound clipping only.
                const auto cost = [&](const double* state, double& value) {
                    value = 0.0;
                    for (int a = 0; a < m; ++a)
                        for (int b = 0; b < m; ++b)
                            value += (state[index[a]] - xb[index[a]]) * backgroundInverse[a][b] * (state[index[b]] - xb[index[b]]);
                    for (const Sample* sample : set) {
                        double modelValue, full[kComponents];
                        if (!model(*sample, state, modelValue, full)) return false;
                        const double difference = (sample->value - modelValue) / sample->sigma;
                        value += difference * difference;
                    }
                    return std::isfinite(value);
                };
                double currentCost = 0.0, backgroundCost = NAN;
                bool stalled = false, stepFailed = false;  // FIX(2026-11-21 S-5 QA): stop reasons
                // FIX(2026-11-24 I1): which kind of failure ended the retrieval: 1 = the observation operator failed
                // (QA bit 3 only), 2 = singular normal matrix (QA bit 4 with bit 3).
                int failureKind = 0;
                int acceptedSteps = 0;
                if (!cost(x, currentCost)) { ok = false; failureKind = 1; }
                backgroundCost = currentCost;
                // 步骤 3b-4：Gauss-Newton 迭代。步长 = H⁻¹g；最大分量 < tolerance（K）视为收敛；
                //         否则按 1、1/2、1/4 回溯，仅接受使 MAP 代价下降的步长，无下降则按“停滞/未更新”结束。
                for (; ok && iteration < inv.maxIterations; ++iteration) {
                    if (!normal(hessian, gradient, squares)) { ok = false; failureKind = 1; break; }
                    if (!invert(hessian, m)) { ok = false; failureKind = 2; break; }
                    double largest = 0.0, step[kComponents]{};
                    for (int a = 0; a < m; ++a) {
                        for (int b = 0; b < m; ++b) step[a] += hessian[a][b] * gradient[b];
                        largest = std::max(largest, std::abs(step[a]));
                    }
                    if (largest < inv.tolerance) {
                        for (int a = 0; a < m; ++a) x[index[a]] = std::clamp(x[index[a]] + step[a], kMinTemperature, kMaxTemperature);
                        cost(x, currentCost);
                        converged = true; ++iteration; break;
                    }
                    bool accepted = false, evaluated = false;
                    for (const double fraction : {1.0, 0.5, 0.25}) {
                        double candidate[kComponents];
                        for (int k = 0; k < kComponents; ++k) candidate[k] = x[k];
                        for (int a = 0; a < m; ++a)
                            candidate[index[a]] = std::clamp(x[index[a]] + fraction * step[a], kMinTemperature, kMaxTemperature);
                        double candidateCost;
                        if (!cost(candidate, candidateCost)) continue;
                        evaluated = true;
                        if (candidateCost < currentCost - 1e-12 * (1.0 + currentCost)) {
                            for (int k = 0; k < kComponents; ++k) x[k] = candidate[k];
                            currentCost = candidateCost;
                            accepted = true;
                            break;
                        }
                    }
                    // FIX(2026-11-21 S-5 QA): no descent along the GN direction -> stalled (evaluated) or step failure.
                    if (!accepted) { (evaluated ? stalled : stepFailed) = true; ++iteration; break; }
                    ++acceptedSteps;
                }
                // Posterior covariance at the solution.
                if (ok && !normal(hessian, gradient, squares)) { ok = false; failureKind = 1; }
                if (ok && !invert(hessian, m)) { ok = false; failureKind = 2; }
                if (!ok) {
                    if (failureKind == 2) {
                        qa[pixel] = static_cast<float>(flag | kQaIllConditioned | kQaRetrievalFailed);
                        ++failed; ++illConditioned;
                    } else {   // operator failed at the background or at the returned state: RetrievalFailed only
                        qa[pixel] = static_cast<float>(flag | kQaRetrievalFailed);
                        ++failed;
                    }
                    continue;
                }
                // FIX(2026-11-21 S-5 QA): bit 10 (stalled after lowering the cost) and bit 11 (not updated) are not
                // failures; bit 9 only for the iteration limit or an operator failure on every trial step.
                if (converged) ++stopConverged;
                else if (stalled && acceptedSteps > 0) { flag |= kQaLineSearchStalled; ++stopStalled; }
                else if (stalled) { flag |= kQaNotUpdated; ++stopNotUpdated; }
                else { flag |= kQaNotConverged; ++notConverged; if (stepFailed) ++stopStepFailed; else ++stopIterationLimit; }
                costs[0][pixel] = static_cast<float>(backgroundCost);
                costs[1][pixel] = static_cast<float>(currentCost);
                // Degrees of freedom for signal: m - tr(A B^-1).
                double trace = 0.0;
                for (int a = 0; a < m; ++a) for (int b = 0; b < m; ++b) trace += hessian[a][b] * backgroundInverse[b][a];
                const double signal = m - trace;
                if (signal < 1.0) { flag |= kQaLowSupport; ++lowSupport; }
                bool range = true;
                for (int a = 0; a < m; ++a) if (x[index[a]] < kValidLow || x[index[a]] > kValidHigh) range = false;
                if (!range) { qa[pixel] = static_cast<float>(flag | kQaOutOfRange); ++outOfRange; continue; }
                for (int a = 0; a < m; ++a) {
                    posterior[index[a]][pixel] = static_cast<float>(x[index[a]]);
                    sd[index[a]][pixel] = static_cast<float>(std::sqrt(std::max(hessian[a][a], 0.0)));
                }
                // Keep the legacy normalized-weight product; actual radiance
                // contributions also depend on the retrieved component temperatures.
                for (size_t j = 0; j < observations; ++j) {
                    const Sample& sample = samples[local * observations + j];
                    if (!sample.valid()) continue;
                    double total = 0.0;
                    for (int k = 0; k < kComponents; ++k) total += sample.weights[k];
                    if (total > 0.0) for (int k = 0; k < kComponents; ++k) shares[k][pixel] = static_cast<float>(sample.weights[k] / total);
                    double contribution[kComponents]{}, radiance = 0.0;
                    for (int k = 0; k < kComponents; ++k) {
                        if (sample.weights[k] > 0.0f && std::isfinite(x[k]))
                            contribution[k] = sample.weights[k] * planck(x[k], sample.wavelength);
                        radiance += contribution[k];
                    }
                    if (radiance > 0.0) for (int k = 0; k < kComponents; ++k)
                        radianceShares[k][pixel] = static_cast<float>(contribution[k] / radiance);
                    break;
                }
                residual[pixel] = static_cast<float>(std::sqrt(squares / set.size()));
                dfs[pixel] = static_cast<float>(signal);
                used[pixel] = static_cast<float>(set.size());
                iterations[pixel] = static_cast<float>(iteration);
                qa[pixel] = static_cast<float>(flag);
                rowDfs[row] += signal;
                rowResidual[row] += residual[pixel];
                ++retrieved;
            }
            const int done = ++rowsDone;
            if (isMainThread() && done >= progressMark + std::max(1, height / 20) && (progressMark = done, true))
                progress(in.progressStart + span * (0.1 + 0.85 * done / height), "组分温度反演");
            } catch (...) { errorsLsct.capture(); }
        }
        for (size_t row = 0; row < rowResidual.size(); ++row) { dfsSum += rowDfs[row]; residualSum += rowResidual[row]; }
        errorsLsct.rethrow("组分温度反演");
        const fs::path& dir = config.outputDir;
        // 输出 LSCT 产品（波段名即上述组分顺序），并登记统计量。
        const std::vector<std::string> names{"Tss sunlit soil temperature (K)", "Tsh shaded soil temperature (K)",
                                             "Tcs sunlit leaf temperature (K)", "Tch shaded leaf temperature (K)"};
        writeTiff(dir / "lsct.tif", grid, {posterior.begin(), posterior.end()}, names);
        writeTiff(dir / "lsct_uncertainty.tif", grid, {sd.begin(), sd.end()},
                  {"Tss uncertainty 1-sigma (K)", "Tsh uncertainty 1-sigma (K)", "Tcs uncertainty 1-sigma (K)", "Tch uncertainty 1-sigma (K)"});
        writeTiff(dir / "lsct_qa.tif", grid, {qa, residual, dfs, used, iterations, costs[0], costs[1]},
                  {"quality flags", "residual RMS (K)", "degrees of freedom for signal", "observations used", "iterations",
                   "MAP cost at background", "MAP cost at analysis"});  // FIX(2026-11-21 S-5 QA): bands 6-7 appended
        writeTiff(dir / "lsct_fractions.tif", grid, {shares.begin(), shares.end()},
                  {"Tss normalized effective weight", "Tsh normalized effective weight", "Tcs normalized effective weight", "Tch normalized effective weight"});
        writeTiff(dir / "lsct_radiance_fractions.tif", grid, {radianceShares.begin(), radianceShares.end()},
                  {"Tss radiance contribution fraction", "Tsh radiance contribution fraction", "Tcs radiance contribution fraction", "Tch radiance contribution fraction"});
        result.products = {"lsct.tif", "lsct_uncertainty.tif", "lsct_qa.tif", "lsct_fractions.tif", "lsct_radiance_fractions.tif"};
        result.summary.Add("fractionReference", "first valid center-pixel observation; normalized effective weights are not geometric area fractions");
        CPLJSONArray stats;
        for (int k = 0; k < kComponents; ++k) stats.Add(bandStatistics(names[k], posterior[k]));
        stats.Add(bandStatistics("degrees of freedom for signal", dfs));
        stats.Add(bandStatistics("residual RMS (K)", residual));
        result.summary.Add("statistics", stats);
        // Imaging uses the posterior; components without a retrieval keep the background.
        for (int k = 0; k < kComponents; ++k) {
            result.components[k] = posterior[k];
            if (background[k].empty()) continue;
            for (size_t p = 0; p < grid.size(); ++p)
                if (!std::isfinite(result.components[k][p])) result.components[k][p] = background[k][p];
        }
    }

    // 步骤 4：汇总 summary：方法说明、观测列表、先验参数、像元计数与停止原因统计、均值残差/DFS。
    result.summary.Add("target", lsct ? "lsct" : "lst");
    result.summary.Add("method", lsct ? "MAP Gauss-Newton, background = run component temperatures" : "weighted Gauss-Newton, single radiometric temperature");
    result.summary.Add("observationModel", "STREAM forward thermal: TiRT four-component weights + hom3 multiple scattering, top of canopy, no atmosphere; brightness observations include the reflected sky (1 - effective emissivity) B(T_sky)");
    CPLJSONArray list;
    for (const auto& item : inv.observations) {
        CPLJSONObject entry;
        entry.Add("name", item.name);
        entry.Add("kind", item.kind == ObservationKind::Lst ? "lst" : "brightness");
        entry.Add("wavelength", item.wavelength);
        entry.Add("sigma", item.sigma);
        list.Add(entry);
    }
    result.summary.Add("observations", list);
    if (lsct) {
        result.summary.Add("priorSd", inv.priorSd);
        result.summary.Add("priorCorrelation", inv.priorCorrelation);
        result.summary.Add("window", inv.window);
    } else result.summary.Add("emissivitySd", inv.emissivitySd);
    CPLJSONObject counts;
    counts.Add("retrievedPixels", static_cast<GInt64>(retrieved));
    counts.Add("failedPixels", static_cast<GInt64>(failed));
    counts.Add("notConvergedPixels", static_cast<GInt64>(notConverged));
    counts.Add("outOfRangePixels", static_cast<GInt64>(outOfRange));
    if (lsct) {
        counts.Add("lowSupportPixels", static_cast<GInt64>(lowSupport));
        counts.Add("illConditionedPixels", static_cast<GInt64>(illConditioned));
        // FIX(2026-11-21 S-5 QA): stop reasons (bit 10 stalled, bit 11 not updated; neither in the default reject mask)
        counts.Add("convergedPixels", static_cast<GInt64>(stopConverged));
        counts.Add("lineSearchStalledPixels", static_cast<GInt64>(stopStalled));
        counts.Add("notUpdatedPixels", static_cast<GInt64>(stopNotUpdated));
        // FIX(2026-11-24 I2): bit-9 pixels split as in the assimilation (stopReasons.iterationLimit / stepEvaluationFailed).
        counts.Add("iterationLimitPixels", static_cast<GInt64>(stopIterationLimit));
        counts.Add("stepEvaluationFailedPixels", static_cast<GInt64>(stopStepFailed));
    }
    // 云/掩膜与输入无效像元数取自步骤 2 的 flags（二者互斥计数，云优先）。
    long long cloud = 0, invalid = 0;
    for (const unsigned flag : flags) { if (flag & kQaCloud) ++cloud; else if (flag & kQaInputInvalid) ++invalid; }
    counts.Add("cloudPixels", static_cast<GInt64>(cloud));
    counts.Add("inputInvalidPixels", static_cast<GInt64>(invalid));
    counts.Add("rejectedObservationValues", static_cast<GInt64>(outOfRangeValues));
    result.summary.Add("counts", counts);
    if (retrieved) {
        result.summary.Add("meanResidualK", residualSum / retrieved);
        if (lsct) result.summary.Add("meanDegreesOfFreedom", dfsSum / retrieved);
    }
    CPLJSONArray products;
    for (const auto& name : result.products) products.Add(name);
    result.summary.Add("products", products);
    log(std::string(lsct ? "组分温度反演" : "地表温度反演") + "：成功 " + std::to_string(retrieved) + " 个像元" +
        (failed ? "，失败 " + std::to_string(failed) : "") + (notConverged ? "，未收敛 " + std::to_string(notConverged) : "") +
        (lsct && lowSupport ? "，信息量不足（DFS < 1）" + std::to_string(lowSupport) : "") +
        (outOfRange ? "，越界 " + std::to_string(outOfRange) : "") + (cloud ? "，云/掩膜 " + std::to_string(cloud) : "") +
        (retrieved ? "，平均残差 " + format(residualSum / retrieved, 3) + " K" : "") +
        (lsct && retrieved ? "，平均 DFS " + format(dfsSum / retrieved, 2) : ""));
    return result;
}

}  // namespace stream
