#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace facetphysics {

// Cell-centred finite volumes, metres below the surface. The lower Dirichlet
// boundary stays at the configured initial soil temperature, at 3 m depth.
inline constexpr std::array<double, 13> soilEdges{0, .01, .025, .05, .1, .2, .35, .55, .8, 1.1, 1.5, 2, 3};
inline constexpr size_t soilLayers = soilEdges.size() - 1;
using SoilProfile = std::array<double, soilLayers>;
struct SoilResponse {
    SoilProfile offset{}, slope{}; // T_i(new) = offset_i + slope_i * T_surface(new)
    double conductance{};
    double fluxSlope() const { return conductance * (1 - slope[0]); }
    double flux(double surface) const { return conductance * ((1 - slope[0]) * surface - offset[0]); }
    SoilProfile profile(double surface) const {
        SoilProfile result{};
        for (size_t i = 0; i < soilLayers; ++i) result[i] = offset[i] + slope[i] * surface;
        return result;
    }
};

// Backward Euler: C_i (T_i-new - T_i-old)/dt = q_upper - q_lower.
// Eliminate the tridiagonal interior once per node, not once per EB iteration.
inline SoilResponse soilResponse(const SoilProfile& previous, double bottomTemperature,
                                 double conductivity, double volumetricCapacity, double dt) {
    if (!(conductivity > 0 && volumetricCapacity > 0 && dt > 0) ||
        !std::isfinite(conductivity + volumetricCapacity + dt + bottomTemperature))
        throw std::runtime_error("Invalid FacetEB soil thermal parameters");
    SoilProfile upper{}, diagonal{}, rhs{}, boundary{};
    for (size_t i = 0; i < soilLayers; ++i) {
        if (!std::isfinite(previous[i])) throw std::runtime_error("Nonfinite soil profile");
        const double width = soilEdges[i + 1] - soilEdges[i];
        const double up = conductivity / (i ? .5 * (soilEdges[i + 1] - soilEdges[i - 1]) : .5 * width);
        const double down = conductivity / (i + 1 < soilLayers ? .5 * (soilEdges[i + 2] - soilEdges[i]) : .5 * width);
        const double storage = volumetricCapacity * width / dt;
        diagonal[i] = storage + up + down;
        upper[i] = i + 1 < soilLayers ? -down : 0;
        rhs[i] = storage * previous[i] + (i + 1 == soilLayers ? down * bottomTemperature : 0);
        boundary[i] = i == 0 ? up : 0;
        if (i) {
            const double factor = -up / diagonal[i - 1];
            diagonal[i] -= factor * upper[i - 1];
            rhs[i] -= factor * rhs[i - 1];
            boundary[i] -= factor * boundary[i - 1];
        }
    }
    SoilResponse response;
    response.conductance = conductivity / (.5 * (soilEdges[1] - soilEdges[0]));
    for (size_t i = soilLayers; i-- > 0;) {
        response.offset[i] = (rhs[i] - (i + 1 < soilLayers ? upper[i] * response.offset[i + 1] : 0)) / diagonal[i];
        response.slope[i] = (boundary[i] - (i + 1 < soilLayers ? upper[i] * response.slope[i + 1] : 0)) / diagonal[i];
    }
    return response;
}

// Paulson unstable / linear stable MOST functions, with explicit applicability
// bounds -5 <= z/L <= 1. These are similarity parameterizations, not CFD.
inline double psiM(double zeta) {
    zeta = std::clamp(zeta, -5.0, 1.0);
    if (zeta >= 0) return -5 * zeta;
    const double x = std::pow(1 - 16 * zeta, .25);
    return 2 * std::log((1 + x) / 2) + std::log((1 + x * x) / 2) - 2 * std::atan(x) + 1.5707963267948966;
}
inline double psiH(double zeta) {
    zeta = std::clamp(zeta, -5.0, 1.0);
    return zeta >= 0 ? -5 * zeta : 2 * std::log((1 + std::sqrt(1 - 16 * zeta)) / 2);
}
inline double phiH(double zeta) {
    zeta = std::clamp(zeta, -5.0, 1.0);
    return zeta >= 0 ? 1 + 5 * zeta : 1 / std::sqrt(1 - 16 * zeta);
}
struct Aerodynamics {
    float leafResistance{}, soilResistance{};
    double frictionVelocity{}, effectiveHeight{};
};
inline Aerodynamics aerodynamics(double referenceHeight, double wind, double height,
                                 double lai, double leafWidth, double inverseL = 0) {
    constexpr double kappa = .4, cd = .3;
    const double u = std::max(.05, wind);
    if (height <= .01 || lai <= .01) {
        const double z = std::max(2.0, referenceHeight) - .01, z0 = .01;
        const double momentum = std::log(z / z0) - psiM(z * inverseL) + psiM(z0 * inverseL);
        const double heat = std::log(z / z0) - psiH(z * inverseL) + psiH(z0 * inverseL);
        const double ustar = std::max(.001, kappa * u / std::max(.1, momentum));
        const float raa = static_cast<float>(std::max(1.0, heat / (kappa * ustar)));
        return {raa, raa, ustar, z};
    }
    const double h = height, area = lai;
    const double z = std::max(referenceHeight, h + std::max(2.0, .5 * h));
    const double sq = std::sqrt(20.6 * area / 2);
    const double g1 = std::max(3.3, std::sqrt(.01 + cd * area / 2));
    const double d = h * (1 - (1 - std::exp(-sq)) / sq);
    const double z0 = (h - d) * std::exp(-kappa * g1 + .2);
    const double zr = 2.5 * h, n = cd * area / (2 * kappa * kappa);
    const double ustar = std::max(.001, kappa * u /
        std::max(.1, std::log((z - d) / z0) - psiM((z - d) * inverseL) + psiM(z0 * inverseL)));
    const double rai = z > zr ? (std::log((z - d) / (zr - d)) -
        psiH((z - d) * inverseL) + psiH((zr - d) * inverseL)) / (kappa * ustar) : 0;
    // Integrate the roughness-sublayer heat correction; exact neutral limit.
    double roughnessPhi = 0;
    const double top = std::min(z, zr);
    for (int k = 0; k < 8; ++k)
        roughnessPhi += phiH((h + (top - h) * (k + .5) / 8 - d) * inverseL) / 8;
    const double rar = (top - h) / (zr - d) * roughnessPhi / (kappa * ustar);
    const double kh = kappa * ustar * (zr - d) / phiH((h - d) * inverseL);
    const double uh = std::max(.01, ustar / kappa *
        (std::log((h - d) / z0) - psiM((h - d) * inverseL) + psiM(z0 * inverseL)));
    const double uz0 = uh * std::exp(n * ((z0 + d) / h - 1));
    const auto profile = [&](double y) {
        return std::log(std::tanh(n * y / (2 * h)));
    };
    const double scale = h * std::sinh(n) / (n * kh);
    const double rac = scale * (profile(h) - profile(z0 + d));
    const double rws = scale * (profile(z0 + d) - profile(.01));
    const double rbc = 70 / area * std::sqrt(std::max(.0001, leafWidth) / uz0);
    const double raa = rai + rar + rac;
    const double leaf = std::max(1.0, (area + 1) * (raa + rbc));
    const double soil = std::max(1.0, (area + 1) * (raa + rws + 10));
    if (!std::isfinite(leaf + soil)) throw std::runtime_error("Nonfinite FacetEB aerodynamic resistance");
    return {static_cast<float>(leaf), static_cast<float>(soil), ustar, z - d};
}
inline double inverseObukhov(double sensibleHeat, double rho, double temperature, const Aerodynamics& a) {
    const double raw = -.4 * 9.81 * sensibleHeat /
        (rho * 1005 * temperature * std::pow(a.frictionVelocity, 3));
    return std::clamp(raw * a.effectiveHeight, -5.0, 1.0) / a.effectiveHeight;
}

// Fixed-resistance thermal equation is monotone; safeguard Newton by its bracket.
// DEFERRED[中] 数值: 括号固定为 [220,360] K，若真实根在区间外（强辐射下的深色屋顶/光伏、寒夜）会被静默钳在边界；32 次未收敛也直接返回当前值而不告知调用方，coupledLeaf 随后基于错误温度继续二分；建议：返回是否收敛的标志，或在端点残差同号时抛错/记录诊断。
template<class Energy>
double temperatureRoot(double initial, Energy&& evaluate) {
    double lower = 220, upper = 360, temperature = std::clamp(initial, lower, upper);
    for (int i = 0; i < 32; ++i) {
        const auto value = evaluate(temperature); // residual, minus its temperature derivative
        if (!std::isfinite(value[0] + value[1])) throw std::runtime_error("Nonfinite leaf thermal equation");
        if (std::abs(value[0]) < 0.0005 || upper - lower < 0.00001) break;
        if (value[0] > 0) lower = temperature; else upper = temperature;
        const double trial = temperature + value[0] / std::max(1.0, value[1]);
        temperature = trial > lower && trial < upper ? trial : .5 * (lower + upper);
    }
    return temperature;
}
struct LeafState { double temperature, resistance; };
// Eliminate temperature through energy balance, then solve the stomatal equation.
// Reversing this order avoids branch jumps in humidity-only roots at fixed T.
template<class Thermal, class Physiology>
LeafState coupledLeaf(Thermal&& thermal, Physiology&& physiology) {
    double lower = std::log(1.0), upper = std::log(625000.0);
    for (int i = 0; i < 24; ++i) {
        const double middle = .5 * (lower + upper), resistance = std::exp(middle);
        const double temperature = thermal(resistance), target = physiology(temperature, resistance);
        if (!(target > 0) || !std::isfinite(target)) throw std::runtime_error("Invalid joint stomatal resistance");
        if (middle > std::log(target)) upper = middle; else lower = middle;
    }
    const double resistance = std::exp(.5 * (lower + upper));
    return {thermal(resistance), resistance};
}

// Solve r_s = r_bio(T, e_leaf(r_s)) in log resistance, without iteration-history
// dependence. The caller supplies the selected photosynthesis model.
template<class Model>
double stomatalRoot(Model&& model, double saturation, double ambient, double boundaryResistance) {
    double lower = std::log(1.0), upper = std::log(625000.0);
    for (int i = 0; i < 24; ++i) {
        const double middle = (lower + upper) / 2;
        const double resistance = std::exp(middle);
        const double vapor = ambient + (saturation - ambient) * boundaryResistance / (boundaryResistance + resistance);
        const double target = model(vapor);
        if (!(target > 0) || !std::isfinite(target)) throw std::runtime_error("Invalid coupled stomatal resistance");
        if (middle > std::log(target)) upper = middle; else lower = middle;
    }
    return std::exp((lower + upper) / 2);
}
} // namespace facetphysics
