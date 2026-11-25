// ==========================================================================
// 文件：brightness.h
// 功能：Planck 辐亮度与由辐亮度反求亮温（按波段中心波长的单色近似）
// 主要模块：
//   1. Planck 常数 kPlanckC1 / kPlanckC2（与 TiRT 的 c1、c2 一致）
//   2. planck / inversePlanck：double 版本
//   3. planckT / inversePlanckT：模板版本，T 可为 double 或自动微分类型 Ad（见 dual.h）
// 数据流：能量平衡、热红外成像（app/thermal_view.cpp）、反演与同化调用，在温度与辐亮度间互转
// 约定：波长单位 μm，温度单位 K，辐亮度单位 W m-2 sr-1 μm-1；
//       辐亮度非正时 inversePlanck 返回 NaN
// ==========================================================================

#pragma once
#include "dual.h"

#include <cmath>

namespace stream {

// 模块：Planck 常数。辐亮度 B(λ,T) = c1 / λ^5 / (exp(c2 / (λ T)) - 1)。
// 说明：c1 = 2hc² 换算到 λ 以 μm 计（W m-2 sr-1 μm-1），c2 = hc/k（μm K）；
//       与 TiRT 取同一数值，保证与 TiRT 对比时逐位一致。
// Planck radiance [W m-2 sr-1 um-1] for wavelength in um and temperature in K (TiRT c1, c2).
constexpr double kPlanckC1 = 11910.439340652 * 1.0e4;
// DEFERRED[低] 物理: c2 用的是 14388.29 μm·K（ITS-68 以前的旧值），CODATA 值为 hc/k = 14387.769 μm·K；c1=1.19104e8 W·m⁻²·sr⁻¹·μm⁴ 本身正确。正反演用同一常数时亮温自洽，但与外部辐亮度（传感器 L1、MODTRAN）互转时，10 μm/300 K 约有 0.01 K 的偏差；建议：改用 CODATA 值，或注明是为与 TiRT 逐位一致而保留
constexpr double kPlanckC2 = 14388.291040407;

// 作用：温度 T（K）与波长 λ（μm）下的黑体辐亮度。用 expm1 保持小指数下的精度。
inline double planck(double temperatureK, double wavelengthUm)
{
    return kPlanckC1 / std::pow(wavelengthUm, 5.0) / std::expm1(kPlanckC2 / (wavelengthUm * temperatureK));
}

// 作用：planck 的逆运算，由辐亮度求亮温（K）；radiance <= 0 或为 NaN 时返回 NaN。
inline double inversePlanck(double radiance, double wavelengthUm)
{
    if (!(radiance > 0.0)) return std::nan("");
    return kPlanckC2 / (wavelengthUm * std::log1p(kPlanckC1 / (radiance * std::pow(wavelengthUm, 5.0))));
}

// 模块：可微版本。同化与反演对温度求导时，T 取 Ad（dual.h 的前向自动微分类型）。
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
