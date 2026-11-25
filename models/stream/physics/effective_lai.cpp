// ==========================================================================
// 文件：effective_lai.cpp
// 功能：垄行/离散树冠冠层的“方向有效 LAI”与半球有效 LAI（等效均匀冠层）
// 状态：已编译但当前引擎任何计算链路都未调用（见 PORTING.md“几何”与 effective_lai.h 的说明）；
//       运行时所有冠层按均质冠层处理，给出树冠/垄行参数时只警告
// 主要模块：
//   1. hasCrownStructure / hasRowStructure：判断是否具备树冠或垄行结构参数
//   2. crownGap：单方向间隙率（均质冠层 Beer 定律，或椭球树冠投影）
//   3. equivalentLai：由半球平均间隙率反求等效均匀 LAI（Newton 迭代）
//   4. directionalGap / directionalLai：垄行或树冠冠层在指定方向的间隙率与有效 LAI
//   5. hemisphericalLai（数值积分）/ hemisphericalLaiAnalytical（4 点 Gauss 积分）
//   6. effectiveLai：一次给出半球、太阳方向、观测方向三个有效 LAI
// 约定：角度为度；天顶角限幅到 [0, 89]°；间隙率下限 1e-12；返回值不会超过冠层自身 LAI；
//       均质冠层（无树冠、垄行结构）在所有方向直接返回自身 LAI
// ==========================================================================

#include "effective_lai.h"
#include "constants.h"

#include <algorithm>
#include <cmath>

namespace stream {
namespace {

// 常数：kPiLegacy = 3.1415926535 为沿用旧实现的 π 近似值（与 constants.h 的 kPi 相差约 9e-11，相对 3e-11）。
//       本文件的函数目前没有任何调用者（2026-11-25 复查：全引擎无 effectiveLai / directionalLai / hemisphericalLai 的引用），
//       所以该差别不影响任何输出；保留旧值是为了与原实现的数值一致，将来若接入请统一改用 kPi 并重新验证；
//       其余是积分采样数与 Gauss 节点：kGaussMu/Weight 为 4 点 Gauss-Legendre 在 [0,1] 上
//       的 cosθ 节点与权重，kGaussAzimuth/Weight 为相对行向 0-90° 方位角上的 3 点节点与权重。
constexpr double kPiLegacy = 3.1415926535;
constexpr float kMinimumCosine = 0.01f;
constexpr float kMinimumGap = 1.0e-12f;
constexpr int kHemisphereSamples = 90;
constexpr int kRowZenithSamples = 6;
constexpr int kRowAzimuthSamples = 12;

constexpr double kGaussMu[4] = {0.0694318442029737, 0.3300094782075719, 0.6699905217924281, 0.9305681557970263};
constexpr double kGaussMuWeight[4] = {0.1739274225687269, 0.3260725774312731, 0.3260725774312731, 0.1739274225687269};
constexpr double kGaussAzimuth[3] = {10.143149884133247, 45.0, 79.856850115866753};
constexpr double kGaussAzimuthWeight[3] = {5.0 / 18.0, 8.0 / 18.0, 5.0 / 18.0};

// 工具：天顶角限幅到 [0, 89]° 后的余弦，并设下限 0.01，防止掠射时除零。
float safeCosine(float zenithDeg)
{
    const float bounded = std::clamp(zenithDeg, 0.0f, 89.0f);
    return std::max(static_cast<float>(std::cos(bounded * kRad)), kMinimumCosine);
}

float projectionOf(const Canopy& canopy) { return canopy.leafProjection > 0.0f ? canopy.leafProjection : kDefaultLeafProjection; }

// 判定：离散树冠结构有效——IGBP 类 1-5（森林类）、LAI > 0、叶片投影 G > 0，
//       且树密度、树冠高度、冠形参数都 > 0。
bool hasCrownStructure(const Canopy& canopy)
{
    return canopy.type >= 1 && canopy.type <= 5 && canopy.lai > 0.0f && canopy.leafProjection > 0.0f &&
           canopy.treeStand > 0.0f && canopy.canopyHeight > 0.0f && canopy.crownShape > 0.0f;
}

// 判定：垄行结构有效——启用 rows、IGBP 类 12 或 14（农田类）、LAI > 0，
//       行距、行宽（不超过行距）、行高均 > 0。
bool hasRowStructure(const Canopy& canopy, const CropRows& rows)
{
    return rows.enabled && (canopy.type == 12 || canopy.type == 14) && canopy.lai > 0.0f && rows.spacing > 0.0f &&
           rows.width > 0.0f && rows.width <= rows.spacing && rows.height > 0.0f;
}

// 模块：某天顶角方向的冠层间隙率（光线不被冠层截获的概率）。
// 步骤 1：无树冠结构（或树冠面积无效）时按均质冠层 exp(-G·LAI/cosθ)。
// 步骤 2：有树冠结构时，把树冠视作椭球，计算其在该方向的投影面积 crownArea，
//         用椭球内部的精确透过率（tauMax 的解析式；tauMax 很小时取一阶展开避免舍入）
//         得到冠内间隙 gapInside，再按树密度换算成整体间隙率。（公式出处待核对）
float crownGap(const Canopy& canopy, float zenithDeg)
{
    const float lai = std::max(canopy.lai, 0.0f);
    if (lai <= 0.0f) return 1.0f;
    const float projection = projectionOf(canopy);
    const float cosine = safeCosine(zenithDeg);
    if (!hasCrownStructure(canopy)) return std::clamp(std::exp(-projection * lai / cosine), kMinimumGap, 1.0f);

    const double theta = std::clamp(zenithDeg, 0.0f, 89.0f) * kRad;
    const double verticalRadius = canopy.canopyHeight / 2.0;
    const double horizontalRadius = canopy.canopyHeight / (2.0 * canopy.crownShape);
    const double crownArea = std::sqrt(horizontalRadius * horizontalRadius + std::pow(verticalRadius * std::tan(theta), 2.0)) *
                             kPiLegacy * horizontalRadius;
    if (!(crownArea > 0.0) || !std::isfinite(crownArea)) return std::clamp(std::exp(-projection * lai / cosine), kMinimumGap, 1.0f);
    // Exact transmittance through the projected ellipsoid; exp of the mean depth would underestimate it.
    const double tauMax = 1.5 * static_cast<double>(lai) * projection / (crownArea * canopy.treeStand * cosine);
    const double gapInside = tauMax > 1.0e-4 ? 2.0 * (1.0 - (1.0 + tauMax) * std::exp(-tauMax)) / (tauMax * tauMax)
                                             : 1.0 - 2.0 * tauMax / 3.0;
    const double gap = std::exp(-crownArea * (1.0 - gapInside) * canopy.treeStand);
    return std::clamp(static_cast<float>(gap), kMinimumGap, 1.0f);
}

// 模块：由半球平均间隙率 gap 反求等效均匀冠层的 LAI。
// 说明：均匀冠层的半球间隙率为 Σ 2·μ_i·w_i·exp(-G·LAI/μ_i)（4 点 Gauss 积分），
//       对 ln(gap) 做 Newton 迭代（最多 60 次，步长相对 1e-10 收敛）；gap >= 1 时返回 0。
float equivalentLai(double gap, float projection)
{
    if (!(gap < 1.0)) return 0.0f;
    const double target = std::log(std::max(gap, 1.0e-300));
    double x = 0.0;
    for (int iteration = 0; iteration < 60; ++iteration) {
        double value = 0.0, slope = 0.0;
        for (int i = 0; i < 4; ++i) {
            const double term = 2.0 * kGaussMu[i] * kGaussMuWeight[i] * std::exp(-projection * x / kGaussMu[i]);
            value += term;
            slope -= term * projection / kGaussMu[i];
        }
        const double step = (std::log(value) - target) / (-slope / value);
        x += step;
        if (std::abs(step) < 1.0e-10 * std::max(1.0, x)) break;
    }
    return static_cast<float>(x);
}

}  // namespace

// 模块：指定方向（天顶角、方位角）的间隙率。无垄行结构时退回 crownGap。
// 说明：垄行时，沿垂直于行的方向计算行投影宽度 = 行宽 + 行高·tanθ·|sin(方位差)|，
//       覆盖度 cover = 投影宽度/行距（限幅在 [行宽/行距, 1]）；
//       间隙率 = (1 - cover) + cover·exp(-G·LAI/(cover·cosθ))。
float directionalGap(const Canopy& canopy, const CropRows& rows, float zenithDeg, float azimuthDeg)
{
    if (!hasRowStructure(canopy, rows)) return crownGap(canopy, zenithDeg);
    const float bounded = std::clamp(zenithDeg, 0.0f, 89.0f);
    const float cosine = safeCosine(bounded);
    const float crossRow = std::abs(std::sin((azimuthDeg - rows.azimuth) * kRad));
    const float projectedWidth = rows.width + rows.height * std::tan(bounded * kRad) * crossRow;
    const float cover = std::clamp(projectedWidth / rows.spacing, rows.width / rows.spacing, 1.0f);
    const float rowGap = std::exp(-projectionOf(canopy) * std::max(canopy.lai, 0.0f) / (cover * cosine));
    return std::clamp((1.0f - cover) + cover * rowGap, kMinimumGap, 1.0f);
}

// 模块：方向有效 LAI = -ln(间隙率)·cosθ / G，限幅到 [0, 自身 LAI]；均质冠层直接返回 LAI。
float directionalLai(const Canopy& canopy, const CropRows& rows, float zenithDeg, float azimuthDeg)
{
    const float lai = std::max(canopy.lai, 0.0f);
    if (!hasRowStructure(canopy, rows) && !hasCrownStructure(canopy)) return lai;
    const float gap = directionalGap(canopy, rows, zenithDeg, azimuthDeg);
    return std::clamp(-std::log(gap) * safeCosine(zenithDeg) / projectionOf(canopy), 0.0f, lai);
}

// 模块：半球有效 LAI（数值积分）。
// 步骤：在天顶角 [0, 90°] 上等分采样（树冠 90 点；垄行 6 个天顶角 × 12 个方位），
//       以 2·cosθ·sinθ 为权重对方向有效 LAI 加权平均，结果限幅到 [0, 自身 LAI]。
float hemisphericalLai(const Canopy& canopy, const CropRows& rows)
{
    const float lai = std::max(canopy.lai, 0.0f);
    const bool row = hasRowStructure(canopy, rows);
    if (!row && !hasCrownStructure(canopy)) return lai;
    double weighted = 0.0, weightSum = 0.0;
    const int zenithSamples = row ? kRowZenithSamples : kHemisphereSamples;
    for (int sample = 0; sample < zenithSamples; ++sample) {
        const double theta = (sample + 0.5) * (0.5 * kPiLegacy / zenithSamples);
        const double weight = 2.0 * std::cos(theta) * std::sin(theta);
        double value = 0.0;
        if (row) {
            for (int a = 0; a < kRowAzimuthSamples; ++a)
                value += directionalLai(canopy, rows, static_cast<float>(theta / kRad), static_cast<float>((a + 0.5) * (360.0 / kRowAzimuthSamples)));
            value /= kRowAzimuthSamples;
        } else {
            value = directionalLai(canopy, rows, static_cast<float>(theta / kRad), 0.0f);
        }
        weighted += value * weight;
        weightSum += weight;
    }
    return std::clamp(static_cast<float>(weighted / weightSum), 0.0f, lai);
}

// 模块：半球有效 LAI（4 点 Gauss 积分版本）。
// 说明：垄行时在 3 个方位节点（相对行向）上平均间隙率；先积分间隙率，再由 equivalentLai
//       换算为等效 LAI（与数值积分版本相比计算量小）。
float hemisphericalLaiAnalytical(const Canopy& canopy, const CropRows& rows)
{
    const float lai = std::max(canopy.lai, 0.0f);
    const bool row = hasRowStructure(canopy, rows);
    if (!row && !hasCrownStructure(canopy)) return lai;
    const float projection = projectionOf(canopy);
    double gap = 0.0;
    for (int i = 0; i < 4; ++i) {
        const float zenith = static_cast<float>(std::acos(kGaussMu[i]) / kRad);
        double directional = 0.0;
        if (row) {
            for (int j = 0; j < 3; ++j)
                directional += kGaussAzimuthWeight[j] * directionalGap(canopy, rows, zenith, rows.azimuth + static_cast<float>(kGaussAzimuth[j]));
        } else {
            directional = crownGap(canopy, zenith);
        }
        gap += 2.0 * kGaussMu[i] * kGaussMuWeight[i] * directional;
    }
    return std::clamp(equivalentLai(gap, projection), 0.0f, lai);
}

// 入口：一次返回半球、太阳方向与观测方向的有效 LAI；analyticHemisphere 选择半球积分方式。
EffectiveLai effectiveLai(const Canopy& canopy, const CropRows& rows, float szaDeg, float saaDeg, float vzaDeg, float vaaDeg,
                          bool analyticHemisphere)
{
    return EffectiveLai{
        analyticHemisphere ? hemisphericalLaiAnalytical(canopy, rows) : hemisphericalLai(canopy, rows),
        directionalLai(canopy, rows, szaDeg, saaDeg),
        directionalLai(canopy, rows, vzaDeg, vaaDeg)};
}

}  // namespace stream
