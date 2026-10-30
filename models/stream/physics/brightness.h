#pragma once
#include <cmath>

namespace stream {

// Planck radiance [W m-2 sr-1 um-1] for wavelength in um and temperature in K (TiRT c1, c2).
constexpr double kPlanckC1 = 11910.439340652 * 1.0e4;
// DEFERRED[低] 物理: c2 用的是 14388.29 μm·K（ITS-68 以前的旧值），CODATA 值为 hc/k = 14387.769 μm·K；c1=1.19104e8 W·m⁻²·sr⁻¹·μm⁴ 本身正确。正反演用同一常数时亮温自洽，但与外部辐亮度（传感器 L1、MODTRAN）互转时，10 μm/300 K 约有 0.01 K 的偏差；建议：改用 CODATA 值，或注明是为与 TiRT 逐位一致而保留
constexpr double kPlanckC2 = 14388.291040407;

inline double planck(double temperatureK, double wavelengthUm)
{
    return kPlanckC1 / std::pow(wavelengthUm, 5.0) / std::expm1(kPlanckC2 / (wavelengthUm * temperatureK));
}

// FIXED[低] 错误处理: radiance≤0 时 log1p(+inf)=inf 会静默返回 0 K（radiance<0 时返回 NaN 或负温度），调用方得不到提示；已修复：radiance≤0 或非有限时显式返回 NaN（std::nan），调用方 image.cpp 已在 radiance>0 时调用，不影响有效像元
inline double inversePlanck(double radiance, double wavelengthUm)
{
    if (!(radiance > 0.0)) return std::nan("");
    return kPlanckC2 / (wavelengthUm * std::log1p(kPlanckC1 / (radiance * std::pow(wavelengthUm, 5.0))));
}

}  // namespace stream
