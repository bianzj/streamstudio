#pragma once
// Closed-form canopy longwave exchange. x is cumulative hemispherical-effective LAI from the top,
// L the total, k = 1 the diffuse extinction, leaf source S(x) = a + b e^{-s x} with
// a = sigma Tlh^4, b = sigma (Tls^4 - Tlh^4) and s the sunlit-leaf extinction:
//   E_down(x) = Esky e^{-kx} + a (1-e^{-kx}) + b k (e^{-sx}-e^{-kx})/(k-s)
//   E_up(x)   = E_up(L) e^{-k(L-x)} + a (1-e^{-k(L-x)}) + b k/(k+s) (e^{-sx} - e^{-sL} e^{-k(L-x)})
//   E_up(L)   = eps_s sigma Tsoil^4 + (1-eps_s) E_down(L)
// Leaves absorb eps_l (E_down + E_up) per unit leaf area (both faces); sunlit and shaded values are
// the e^{-sx} and (1-e^{-sx}) weighted depth means. Soil absorbs eps_s E_down(L).
#include "constants.h"

#include <cmath>

namespace stream {

struct CanopyLongwave { double leafSunlit = 0, leafShaded = 0, soil = 0; };

namespace longwave_detail {
inline double expIntegral(double r, double L) { return std::abs(r * L) < 1e-12 ? L : -std::expm1(-r * L) / r; }
inline double sinhRatio(double d, double L) { return std::abs(d * L) < 1e-12 ? L : std::sinh(d * L) / d; }
inline double xExpIntegral(double r, double L)
{
    const double t = r * L;
    if (std::abs(t) < 1e-3) return L * L * (0.5 - t / 3.0 + t * t / 8.0);
    return (-std::expm1(-t) - t * std::exp(-t)) / (r * r);
}
// int_0^L e^{-q x} (e^{-s x} - e^{-k x})/(k - s) dx, stable for s -> k
inline double differenceIntegral(double q, double s, double k, double L)
{
    const double d = 0.5 * (k - s), m = 0.5 * (k + s);
    if (std::abs(d * L) < 1e-4) return xExpIntegral(q + m, L);
    return (expIntegral(q + s, L) - expIntegral(q + k, L)) / (k - s);
}
}  // namespace longwave_detail

inline CanopyLongwave canopyLongwave(double lai, double leafEmissivity, double soilEmissivity, double sky,
                                     double Tls, double Tlh, double Tss, double Tsh,
                                     double sunExtinction, double soilSunlitFraction)
{
    using namespace longwave_detail;
    const double L = lai > 0.0 ? lai : 0.0, k = 1.0;
    const bool sun = sunExtinction > 0.0;
    const double s = sun ? sunExtinction : 0.0;
    // DEFERRED[中] 物理: 冠层辐射场的叶片源项按黑体 σT⁴ 取（a、b 中没有 ε_l），而叶片只吸收 ε_l 份额、被截获辐射中的 (1−ε_l) 既没有被散射也没有回到辐射场，直接消失；源项偏大 (1−ε_l)，吸收又少计 (1−ε_l)。若 energy.cpp 中叶片发射按 ε_l σT⁴ 计，则 Rn 合计不闭合（ε_l=0.985、LAI 3 时约几 W m⁻²）；建议：源项改为 ε_l σT⁴，并把 (1−ε_l) 散射项计入（或至少与 energy.cpp 的叶片发射项保持同一 ε 约定），再做一次 sky+soil+leaf 的发射−吸收闭合校验
    const double a = kStefanBoltzmann * Tlh * Tlh * Tlh * Tlh;
    const double b = sun ? kStefanBoltzmann * Tls * Tls * Tls * Tls - a : 0.0;
    const double M = std::exp(-k * L), sunGap = std::exp(-s * L);
    const double bottom = k * std::exp(-0.5 * (k + s) * L) * sinhRatio(0.5 * (k - s), L);
    const double down = sky * M + a * (1.0 - M) + b * bottom;
    const double up = soilEmissivity * kStefanBoltzmann *
                          (soilSunlitFraction * Tss * Tss * Tss * Tss + (1.0 - soilSunlitFraction) * Tsh * Tsh * Tsh * Tsh) +
                      (1.0 - soilEmissivity) * down;
    const auto irradiance = [&](double p) {
        const double one = expIntegral(p, L), kd = expIntegral(p + k, L), ks = expIntegral(p + s, L);
        const double fromBottom = std::exp(-0.5 * (k + p) * L) * sinhRatio(0.5 * (k - p), L);
        const double downward = sky * kd + a * (one - kd) + b * k * differenceIntegral(p, s, k, L);
        const double upward = up * fromBottom + a * (one - fromBottom) + b * k / (k + s) * (ks - sunGap * fromBottom);
        return downward + upward;
    };
    CanopyLongwave result;
    result.soil = soilEmissivity * down;
    if (!(L > 0.0)) return result;
    const double total = irradiance(0.0);
    const double sunWeight = sun ? expIntegral(s, L) : 0.0;
    const double shadeWeight = L - sunWeight;
    if (!sun || s * L < 1e-6) {
        result.leafSunlit = result.leafShaded = leafEmissivity * total / L;
        return result;
    }
    const double sunlit = irradiance(s);
    result.leafSunlit = leafEmissivity * sunlit / sunWeight;
    result.leafShaded = shadeWeight > 1e-12 * L ? leafEmissivity * (total - sunlit) / shadeWeight : result.leafSunlit;
    return result;
}

}  // namespace stream
