#pragma once
#include "dual.h"

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

inline double inversePlanck(double radiance, double wavelengthUm)
{
    if (!(radiance > 0.0)) return std::nan("");
    return kPlanckC2 / (wavelengthUm * std::log1p(kPlanckC1 / (radiance * std::pow(wavelengthUm, 5.0))));
}

// Differentiable forms (T = double or Ad); same formulas as above.
template <class T>
T planckT(const T& temperatureK, double wavelengthUm)
{
    return kPlanckC1 / std::pow(wavelengthUm, 5.0) / ad::expm1(kPlanckC2 / (wavelengthUm * temperatureK));
}

template <class T>
T inversePlanckT(const T& radiance, double wavelengthUm)
{
    if (!(radiance > 0.0)) return T(std::nan(""));
    return kPlanckC2 / (wavelengthUm * ad::log1p(kPlanckC1 / (radiance * std::pow(wavelengthUm, 5.0))));
}

}  // namespace stream
