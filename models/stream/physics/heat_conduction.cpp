// ==========================================================================
// 文件：heat_conduction.cpp
// 功能：土壤（地表）导热通量 G 的两种算法：7 h 表面温度历史卷积、2 m 土柱隐式有限体积
// 主要模块：
//   1. 土柱网格 columnLayers / sharedColumnLayers：1 cm 起按 1.3 倍递增至 2 m
//   2. columnStart / columnProfile：向后欧拉有限体积求解（上边界给定地表温度，下边界零通量）
//   3. initializeColumn：按分层锚点温度初始化土柱
//   4. surfaceHeatFluxT / surfaceHeatFlux：半无限介质对分段线性温度历史的地表热通量
//   5. depthHeatFlux：土柱方法的地表热通量，对地表温度精确线性化并缓存（斜率+截距）
//   6. advanceThermalState：时间步结束后提交节点温度，更新历史与土柱
// 数据流：energy.cpp 的能量平衡迭代反复调用 surfaceHeatFlux(T) 或 depthHeatFlux(T) 得到 G(T)；
//         app/image.cpp 逐时次推进节点后调用 advanceThermalState 保存热状态
// 约定：G 为进入土壤的热通量（W m-2，地表变暖时为正）；温度 K；时间步 dt 至少取 1 s；
//       历史窗口 kHistorySeconds = 7 h（与 TiRT/TIRTEB 一致，见 PORTING.md）
// ==========================================================================

#include "heat_conduction.h"

#include <algorithm>
#include <cmath>

namespace stream {
namespace {

// 常数：历史窗口与土柱几何（深度 2 m；最上层 1 cm，每层比上一层厚 1.3 倍）。
// FIX(2026-11-21 B9): TiRT/TIRTEB keep the same 7 h window (heat_conduction.py HISTORY_SECONDS; TIRTEB theory §G
// "保存实际经过的 7 小时时间窗"), so this is not a STREAM-only shortening. (Former, wrong comment: "TiRT/TIRTEB
// integrate 48 h of surface temperature; STREAM only needs the last 7 h (project decision).")
constexpr double kHistorySeconds = 7.0 * 3600.0;
constexpr double kColumnDepth = 2.0;      // m
constexpr double kColumnTopLayer = 0.01;  // m
constexpr double kColumnGrowth = 1.3;

// 模块：生成土柱各层厚度（m）。
// 说明：从 1 cm 开始逐层 × 1.3 直到接近 2 m；最后剩余厚度小于最后一层时并入最后一层，
//       否则单独成层，保证总厚度恰为 2 m。
std::vector<double> columnLayers()
{
    std::vector<double> layers;
    double total = 0.0, thickness = kColumnTopLayer;
    while (total + thickness < kColumnDepth - 1.0e-9) {
        layers.push_back(thickness);
        total += thickness;
        thickness *= kColumnGrowth;
    }
    const double remainder = kColumnDepth - total;
    if (!layers.empty() && remainder < layers.back()) layers.back() += remainder;
    else layers.push_back(remainder);
    return layers;
}

// The layer geometry is fixed; build it once (thread-safe static initialisation).
const std::vector<double>& sharedColumnLayers()
{
    static const std::vector<double> layers = columnLayers();
    return layers;
}

// 工具：把已有土柱剖面补齐/截断到 nodes 个节点（缺失或首次调用时用 temperature 或最底层温度填充）。
std::vector<double> columnStart(const std::vector<double>& profile, double temperature, size_t nodes)
{
    std::vector<double> old = profile;
    if (old.empty()) old.push_back(temperature);
    while (old.size() < nodes) old.push_back(old.back());
    old.resize(nodes);
    return old;
}

// 模块：一个时间步 dt 的土柱温度剖面。节点 0 为地表（给定温度 surface），节点 1..N 为各层底部。
// 步骤 1：层间导热系数 conductance = λ / 层厚；每个未知节点的储热项取上下半层厚度之和的一半 × C / dt。
// 步骤 2：组装三对角方程（向后欧拉，隐式；最底层零通量），
//         上边界通过 rhs[0] 与对角项同时引入（见下面注释）。
// 步骤 3：Thomas 算法前消去 + 回代；返回 [surface, 各层节点温度]。
// Backward-Euler finite volumes: Dirichlet surface, zero-flux bottom.
std::vector<double> columnProfile(const std::vector<double>& old, double surface, double conductivity,
                                  double capacity, const std::vector<double>& layers, double dt)
{
    const size_t count = layers.size();
    if (count == 0) return {surface};
    std::vector<double> conductance(count), storage(count);
    for (size_t index = 0; index < count; ++index) conductance[index] = conductivity / layers[index];
    for (size_t index = 0; index < count; ++index) {
        const double above = layers[index];
        const double below = index + 1 < count ? layers[index + 1] : 0.0;
        storage[index] = capacity * 0.5 * (above + below) / dt;
    }
    std::vector<double> lower(count), upper(count), diagonal(count), rhs(count);
    // Unknown i is the node at the bottom of layer i: linked upward through layer i (to the surface when i == 0)
    // and downward through layer i + 1 (none at the zero-flux bottom). The surface link belongs on the diagonal
    // too, otherwise a uniform column gains heat in proportion to its absolute temperature.
    for (size_t index = 0; index < count; ++index) {
        const double above = conductance[index];
        const double below = index + 1 < count ? conductance[index + 1] : 0.0;
        lower[index] = index > 0 ? -above : 0.0;
        upper[index] = -below;
        diagonal[index] = storage[index] + above + below;
        rhs[index] = storage[index] * old[index + 1];
    }
    rhs[0] += conductance[0] * surface;
    for (size_t index = 1; index < count; ++index) {
        const double multiplier = lower[index] / diagonal[index - 1];
        diagonal[index] -= multiplier * upper[index - 1];
        rhs[index] -= multiplier * rhs[index - 1];
    }
    std::vector<double> result(count);
    result.back() = rhs.back() / diagonal.back();
    for (size_t index = count - 1; index-- > 0;)
        result[index] = (rhs[index] - upper[index] * result[index + 1]) / diagonal[index];
    result.insert(result.begin(), surface);
    return result;
}

}  // namespace

// 模块：用三个锚点温度初始化土柱剖面：地表 surface、ERA5 第 1 层（0-7 cm，锚在 3.5 cm）
//       level1、第 2 层（7-28 cm，锚在 17.5 cm）level2；锚点之间线性插值，
//       17.5 cm 以下保持 level2。当前 app/image.cpp 以同一数值调用（均匀土柱）。
void initializeColumn(std::vector<double>& profile, double surface, double level1, double level2)
{
    const std::vector<double>& layers = sharedColumnLayers();
    constexpr double kLevel1Depth = 0.035;   // ERA5 soil temperature level 1: 0-7 cm
    constexpr double kLevel2Depth = 0.175;   // ERA5 soil temperature level 2: 7-28 cm
    const auto at = [&](double depth) {
        if (!(depth > kLevel1Depth)) return surface + (level1 - surface) * (depth / kLevel1Depth);
        if (!(depth > kLevel2Depth)) return level1 + (level2 - level1) * ((depth - kLevel1Depth) / (kLevel2Depth - kLevel1Depth));
        return level2;
    };
    profile.assign(layers.size() + 1, surface);
    double depth = 0.0;
    for (size_t index = 0; index < layers.size(); ++index) {
        depth += layers[index];
        profile[index + 1] = at(depth);
    }
}

// 模块：半无限介质的地表热通量（7 h 温度历史卷积）。
// 输入：当前温度 temperature、历史（最新在前，仅含此前时刻）、各历史间隔 intervals、
//       热惰性 inertia = sqrt(λ·C)。
// 说明：地表温度按分段线性处理，每段贡献 (ΔT / Δt)·(sqrt(t_end) - sqrt(t_start))，
//       t 从当前时刻向前累计，超过 7 h 的部分截断；总通量 = 2·I/sqrt(π) × 累加和。
//       对 temperature 与 inertia 都是线性的，所以对 Ad 版本可精确求导。
template <class T>
T surfaceHeatFluxT(const T& temperature, const SoilThermalState& state, double dtSeconds, const T& inertia)
{
    const double dt = std::max(dtSeconds, 1.0);
    double elapsed = 0.0;
    T weighted(0.0);
    for (size_t index = 0; index < state.history.size(); ++index) {
        const double duration = index == 0 ? dt : (index - 1 < state.intervals.size() ? state.intervals[index - 1] : dt);
        const double interval = std::max(duration, 1.0);
        const double end = std::min(elapsed + interval, kHistorySeconds);
        const T newer = index == 0 ? temperature : T(state.history[index - 1]);
        weighted += (newer - state.history[index]) / interval * (std::sqrt(end) - std::sqrt(elapsed));
        elapsed += interval;
        if (elapsed >= kHistorySeconds) break;
    }
    return 2.0 * inertia / std::sqrt(3.14159265358979323846) * weighted;
}

double surfaceHeatFlux(double temperature, const SoilThermalState& state, double dtSeconds, double inertia)
{
    return surfaceHeatFluxT<double>(temperature, state, dtSeconds, inertia);
}

// 显式实例化：double 与自动微分类型 Ad。
template double surfaceHeatFluxT<double>(const double&, const SoilThermalState&, double, const double&);
template Ad surfaceHeatFluxT<Ad>(const Ad&, const SoilThermalState&, double, const Ad&);

// 模块：土柱方法的地表热通量 G = 上层导热 + 半层储热（进入土壤为正）。
// 说明：隐式土柱的解对地表温度是线性的，故每个时间步只解两次（地表 0 与 1）
//       得到斜率 depthSlope 与截距 depthIntercept 并缓存，之后能量平衡迭代中
//       G(T) = depthIntercept + depthSlope·T 直接取值；时间步、导热率、热容变化时重算。
//       土柱为空（冷启动）时返回 0。
double depthHeatFlux(double temperature, const SoilThermalState& state, double dtSeconds, const ThermalMaterial& material)
{
    // Cold start: the column is taken at the surface temperature itself, so no heat flows (unchanged behaviour).
    if (state.profile.empty()) return 0.0;
    const double dt = std::max(dtSeconds, 1.0);
    if (!state.depthCached || state.depthDt != dt || state.depthConductivity != material.conductivity ||
        state.depthCapacity != material.volumetricCapacity) {
        // The energy-balance iteration calls this thousands of times per step with only the surface temperature
        // changing. The implicit column is linear in that boundary value, so two solves give G(T) exactly.
        const std::vector<double>& layers = sharedColumnLayers();
        const std::vector<double> old = columnStart(state.profile, state.profile.front(), layers.size() + 1);
        const std::vector<double> zero = columnProfile(old, 0.0, material.conductivity, material.volumetricCapacity, layers, dt);
        const std::vector<double> unit = columnProfile(old, 1.0, material.conductivity, material.volumetricCapacity, layers, dt);
        const double response = unit[1] - zero[1];   // d(node 1)/d(surface)
        const double conduction = material.conductivity / layers[0];
        const double storage = material.volumetricCapacity * 0.5 * layers[0] / dt;
        state.depthSlope = conduction * (1.0 - response) + storage;
        state.depthIntercept = -conduction * zero[1] - storage * old[0];
        state.depthDt = dt;
        state.depthConductivity = material.conductivity;
        state.depthCapacity = material.volumetricCapacity;
        state.depthCached = true;
    }
    return state.depthIntercept + state.depthSlope * temperature;
}

// 模块：节点收敛后提交热状态。
// 步骤 1：把当前温度与本步 dt 插到历史/间隔的最前面，累计到 7 h 即停止保留更早的项。
// 步骤 2：depth 方法时同时推进土柱剖面一步，并使缓存的 G(T) 线性化失效。
void advanceThermalState(double temperature, SoilThermalState& state, double dtSeconds, const ThermalMaterial& material,
                         bool depthMethod)
{
    const double dt = std::max(dtSeconds, 1.0);
    std::vector<double> intervals;
    intervals.push_back(dt);
    for (const double value : state.intervals) intervals.push_back(value);
    double elapsed = 0.0;
    size_t retained = 0;
    for (const double interval : intervals) {
        elapsed += interval;
        ++retained;
        if (elapsed >= kHistorySeconds) break;
    }
    std::vector<double> history;
    history.push_back(temperature);
    for (size_t index = 0; index < state.history.size() && history.size() < retained + 1; ++index)
        history.push_back(state.history[index]);
    state.history = std::move(history);
    state.intervals.assign(intervals.begin(), intervals.begin() + static_cast<long>(std::min(retained, intervals.size())));
    if (depthMethod) {
        const std::vector<double>& layers = sharedColumnLayers();
        const size_t nodes = layers.size() + 1;
        const std::vector<double> old = columnStart(state.profile, temperature, nodes);
        state.profile = columnProfile(old, temperature, material.conductivity, material.volumetricCapacity, layers, dt);
        state.depthCached = false;
    }
}

}  // namespace stream
