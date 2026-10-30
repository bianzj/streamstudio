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

inline double hapkeDirectionalEmissivity(double viewZenithDeg, double K, double hemisphericalEmissivity)
{
    return hapkeEmissivity(viewZenithDeg, K, hapkeAlbedoForHemispherical(K, hemisphericalEmissivity));
}

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
        // FIXED[中] 内存: viewZenithDeg 为 NaN 时 std::clamp 原样返回 NaN，static_cast<int>(NaN) 是未定义行为（MSVC 通常得到 INT_MIN），std::min 之后 index 为负，values_[index] 越界读；已修复：先检查 !std::isfinite(viewZenithDeg)，非有限角度直接返回天底值 values_[0]，再做插值
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
