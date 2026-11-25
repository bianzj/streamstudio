// ==========================================================================
// 文件：hapke.h
// 功能：粗糙土壤的方向发射率（Hapke 形式，TiRT soilopt.hapke_emissivity）及其查表
// 主要模块：
//   1. hapkeEmissivity：给定观测天顶角、粗糙参数 K 与单次散射反照率 w 的方向发射率
//   2. hapkeHemisphericalEmissivity：余弦加权半球平均（中点法 180 段）
//   3. hapkeAlbedoForHemispherical：二分法反求使半球发射率等于地类给定值的 w
//   4. hapkeDirectionalEmissivity：由半球发射率直接得到方向发射率
//   5. SoilEmissivityTable：按观测天顶角（0.1° 间隔）的发射率查找表
// 数据流：地类/配置给出土壤半球发射率（含水量插值后，见 soil_emissivity.h）→ 反求 w →
//         热红外成像按观测方向取方向发射率；宽带能量平衡仍使用半球发射率
// 约定：角度为度；发射率 0-1；K 为 Hapke 粗糙/几何参数（下限 1e-8 防除零）；
//       w 限制在 [0, 0.999999]
// ==========================================================================

#pragma once
#include "constants.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace stream {

// Directional emissivity of a rough soil, TiRT base/soilopt.py hapke_emissivity:
//   b    = sqrt(1 - w)                 (w = single-scattering albedo)
//   e(z) = b (1 + 2 cos z / K) / (1 + 2 cos z b / K)
// The cosine-weighted hemispherical mean of e(z) decreases with w, so the single-scattering albedo is solved until
// the hemispherical mean equals the class value from classes.json. The broadband energy balance keeps using the
// hemispherical emissivity, so only the observed directional radiance changes (it falls towards grazing view).
inline double hapkeEmissivity(double viewZenithDeg, double K, double albedo)
{
    const double k = std::max(K, 1e-8);
    const double w = std::clamp(albedo, 0.0, 0.999999);
    const double base = std::sqrt(1.0 - w);
    const double term = 2.0 * std::cos(std::clamp(viewZenithDeg, 0.0, 89.999) * kPi / 180.0) / k;
    return std::clamp(base * (1.0 + term) / (1.0 + term * base), 0.0, 1.0);
}

// 说明：半球平均 = ∫ e(z)·2μ dμ（μ = cos z），中点法，各段权重 2μ/段数。
// Cosine-weighted hemispherical mean by a midpoint rule in mu = cos(zenith) (180 bands, error < 1e-5).
inline double hapkeHemisphericalEmissivity(double K, double albedo)
{
    constexpr int kBands = 180;
    double total = 0.0;
    for (int band = 0; band < kBands; ++band) {
        const double mu = (band + 0.5) / kBands;
        const double zenith = std::acos(std::min(mu, 1.0)) * 180.0 / kPi;
        total += 2.0 * mu * hapkeEmissivity(zenith, K, albedo) / kBands;
    }
    return std::clamp(total, 0.0, 1.0);
}

// 说明：target 不小于 w = 0 时的半球发射率（即无法达到）时返回 0；否则二分 60 次，
//       利用半球发射率随 w 单调下降的性质。
// Single-scattering albedo whose hemispherical emissivity equals `target` (bisection; monotone decreasing).
inline double hapkeAlbedoForHemispherical(double K, double target)
{
    if (!(target < hapkeHemisphericalEmissivity(K, 0.0))) return 0.0;
    double low = 0.0, high = 0.999999;
    for (int iteration = 0; iteration < 60; ++iteration) {
        const double mid = 0.5 * (low + high);
        if (hapkeHemisphericalEmissivity(K, mid) > target) low = mid;
        else high = mid;
    }
    return 0.5 * (low + high);
}

// 作用：由半球发射率与 K 直接求某观测天顶角的方向发射率（每次都重新二分，批量调用请用下面的查表）。
inline double hapkeDirectionalEmissivity(double viewZenithDeg, double K, double hemisphericalEmissivity)
{
    return hapkeEmissivity(viewZenithDeg, K, hapkeAlbedoForHemispherical(K, hemisphericalEmissivity));
}

// 模块：方向发射率查找表。build 一次反求 w 并填表（0-90°、0.1° 步长）；
//       at 按观测天顶角线性插值，输入非有限值时取天顶（0°）值。
// View-zenith lookup: the albedo is solved once, then the table is filled with e(z) for that albedo.
class SoilEmissivityTable {
public:
    void build(double K, double hemisphericalEmissivity)
    {
        const double target = std::clamp(hemisphericalEmissivity, 1e-6, 1.0);
        albedo_ = hapkeAlbedoForHemispherical(K, target);
        k_ = std::max(K, 1e-8);
        for (int step = 0; step <= kSteps; ++step) values_[step] = hapkeEmissivity(90.0 * step / kSteps, k_, albedo_);
    }
    double at(double viewZenithDeg) const
    {
        if (!std::isfinite(viewZenithDeg)) return values_[0];
        const double position = std::clamp(viewZenithDeg, 0.0, 90.0) / 90.0 * kSteps;
        const int index = std::min(static_cast<int>(position), kSteps - 1);
        const double fraction = position - index;
        return values_[index] * (1.0 - fraction) + values_[index + 1] * fraction;
    }
    double albedo() const { return albedo_; }

private:
    static constexpr int kSteps = 900;  // 0.1 deg
    double k_ = 1.0, albedo_ = 0.0;
    std::array<double, kSteps + 1> values_{};
};

}  // namespace stream
