#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace facetbio {

struct LeafBiochemistry {
    float vcmax25{60.0f};
    float ballBerrySlope{8.0f};
    float ballBerry0{0.01f};
    float rdParam{0.015f};
    float kV{0.6396f};
    float stressFactor{1.0f};
    int type{3};
    int temperatureCorrection{0}; // Collatz only; MD12 always corrects temperature.
    std::array<float, 5> tparam{0.2f, 0.3f, 288.0f, 313.0f, 328.0f};
    float annualTemperature{25.0f};
    float beta{0.507f};
    float qLs{1.0f};
    float sustainedNPQ{0.0f};
};

struct FarquharResult {
    double grossAssimilation; // umol CO2 m-2 leaf s-1
    double netAssimilation;
    double intercellularCO2; // ppm
    float resistance; // whole-leaf water-vapour resistance, s m-1
};

// SCOPE 1.73 biochemical_MD12.m / VoxelEB biochemical_m12.comp:
// Farquhar C3 and von Caemmerer C4, with Ball-Berry stomatal coupling.
// Temperature K, absorbed PAR umol photons m-2 s-1, pressure hPa, RH [0,1].
// MD12 has an intrinsic temperature response; Tcor/Tparam control Collatz only.
inline FarquharResult farquhar(const LeafBiochemistry& leaf, double temperature,
    double absorbedPar, double relativeHumidity, double pressure,
    double relativeDepth, double co2Ppm)
{
    const auto nonnegative = [](double x) { return std::isfinite(x) && x >= 0.0; };
    if (!std::isfinite(temperature) || temperature <= 0.0 ||
        !std::isfinite(pressure) || pressure <= 0.0 ||
        !nonnegative(absorbedPar) || !nonnegative(co2Ppm) ||
        !nonnegative(relativeHumidity) || relativeHumidity > 1.0 ||
        !std::isfinite(relativeDepth) || !nonnegative(leaf.vcmax25) ||
        !nonnegative(leaf.ballBerrySlope) || !nonnegative(leaf.ballBerry0) ||
        !nonnegative(leaf.rdParam) || !nonnegative(leaf.kV) ||
        !nonnegative(leaf.stressFactor) || leaf.stressFactor > 1.0 ||
        !nonnegative(leaf.beta) || leaf.beta > 1.0 ||
        !nonnegative(leaf.qLs) || leaf.qLs > 1.0 ||
        !nonnegative(leaf.sustainedNPQ) || !std::isfinite(leaf.annualTemperature) ||
        (leaf.type != 3 && leaf.type != 4))
        throw std::runtime_error("Invalid FacetEB Farquhar leaf/environment parameters");

    constexpr double reference = 298.15, gasConstant = 8.31;
    const bool c4 = leaf.type == 4;
    const double pressurePa = pressure * 100.0;
    const double ppmToBar = pressurePa * 1.0e-11;
    const double cs = co2Ppm * ppmToBar, oxygen = 209.0 * pressurePa * 1.0e-8;
    const double vcmo = leaf.vcmax25 * std::exp(leaf.kV * std::clamp(relativeDepth, -1.0, 0.0));
    const double rt = gasConstant * temperature / 1000.0;
    const double rtRef = gasConstant * reference / 1000.0;
    const auto arrhenius = [&](double activation) {
        return std::exp(activation * (temperature - reference) / (reference * rt));
    };
    const auto peaked = [&](double activation, double deactivation, double entropy) {
        return arrhenius(activation) *
            (1.0 + std::exp((reference * entropy - deactivation) / rtRef)) /
            (1.0 + std::exp((temperature * entropy - deactivation) / rt));
    };
    const double rd = leaf.rdParam * vcmo * arrhenius(46.39);
    const double specificity = 2862.0 / arrhenius(37.83);
    const double vcmax = vcmo * (c4 ? peaked(67.29, 144.57, 0.472)
        : peaked(71.51, 200.0, (-1.07 * leaf.annualTemperature + 668.0) / 1000.0));
    const double jmax = vcmo * (c4 ? (40.0 / 6.0) * peaked(77.9, 191.9, 0.627)
        : 2.68 * peaked(49.88, 200.0, (-0.75 * leaf.annualTemperature + 660.0) / 1000.0));
    const double qt = (temperature - reference) / 10.0;
    const double kc = (c4 ? 944.0 * std::pow(2.1, qt) : 404.9 * arrhenius(79.43)) * ppmToBar;
    const double ko = (c4 ? 633.0 * std::pow(1.2, qt) : 278.4 * arrhenius(36.38)) * pressurePa * 1.0e-8;

    constexpr double kf = 3.0e7, kd = 1.0e8, kPSII = (kd + kf) * 0.88 / 0.12;
    const double kps = kPSII * leaf.qLs;
    const double denominator = kps + kf + kd + leaf.sustainedNPQ * (kf + kd);
    const double theta = (kps - 1.95e8 * leaf.qLs) / denominator;
    const double excitation = leaf.beta * absorbedPar * kps / denominator;
    const double capacity = jmax * leaf.qLs;
    // Rationalized smaller root also handles qLs=0, darkness and theta=0.
    const double sum = excitation + capacity;
    const double electronTransport = sum > 0.0 ? 2.0 * excitation * capacity /
        (sum + std::sqrt(std::max(0.0, sum * sum - 4.0 * theta * excitation * capacity))) : 0.0;
    const double gamma = 0.5 * oxygen / specificity;
    const double lowerCi = c4 ? 9.9 * ppmToBar : gamma;
    const double product = leaf.ballBerrySlope * relativeHumidity * leaf.stressFactor;
    const double ci = product > 0.0 ? std::max(lowerCi, cs * (1.0 - 1.6 / product)) : lowerCi;

    double gross = 0.0;
    if (absorbedPar > 0.0 && electronTransport > 0.0 && product > 0.0 && cs > 0.0) {
        if (!c4) {
            const double rubisco = vcmax * ci / (ci + kc * (1.0 + oxygen / ko));
            const double light = electronTransport * ci / (4.5 * ci + 10.5 * gamma);
            gross = (1.0 - gamma / ci) * std::min(rubisco, light);
        } else {
            const double vpmax = vcmo * 2.33 * peaked(70.37, 117.93, 0.376);
            const double kp = 82.0 * std::pow(2.1, qt) * ppmToBar;
            const double vp = std::min(vpmax * ci / (ci + kp), 80.0);
            const double gbs = (0.0207 * vcmo + 0.4806) * 1000.0;
            const double gam = 0.5 / specificity;
            // Complete C4 equations with MD12's alpha=0 and x=0.4.
            const double supply = vp - 0.5 * rd + gbs * ci;
            const double carboxylation = vcmax - rd;
            const double michaelis = gbs * kc * (1.0 + oxygen / ko);
            const double lightSupply = 0.2 * electronTransport - 0.5 * rd + gbs * ci;
            const double lightCapacity = 0.2 * electronTransport;
            const auto smallerRoot = [](double b, double c) {
                const double disc = b * b - 4.0 * c;
                if (disc < -1.0e-12 * (b * b + std::abs(4.0 * c)))
                    throw std::runtime_error("Invalid FacetEB C4 assimilation discriminant");
                const double root = std::sqrt(std::max(0.0, disc));
                return b < 0.0 ? (b == root ? 0.0 : 2.0 * c / (-b + root)) : (-b - root) / 2.0;
            };
            const double ac = smallerRoot(-(supply + carboxylation + michaelis),
                carboxylation * supply - gam * vcmax * gbs * oxygen + rd * michaelis);
            const double aj = smallerRoot(-(lightSupply + lightCapacity - rd + gbs * gam * oxygen * 7.0 / 3.0),
                lightSupply * (lightCapacity - rd) - gbs * gam * oxygen * (lightCapacity + rd * 7.0 / 3.0));
            gross = std::min(ac, aj) + rd;
        }
    }
    const double net = gross - rd;
    // Retain the configured g0 as residual conductance, as in FacetEB Collatz.
    double conductance = leaf.ballBerry0 * 8.314 * temperature / pressurePa;
    if (net > 0.0 && cs > ci) {
        const double resistance = 0.625 * (cs - ci) / net *
            1.2047 / 28.96 * 1.0e3 * 1.0e6 / pressurePa * 1.0e5;
        conductance += 1.0 / resistance;
    }
    if (!std::isfinite(gross) || !std::isfinite(net) || !std::isfinite(ci) ||
        !nonnegative(conductance))
        throw std::runtime_error("Nonfinite FacetEB Farquhar result");
    const double resistance = conductance > 0.0 ? std::clamp(1.0 / conductance, 1.0, 625000.0) : 625000.0;
    return {std::max(0.0, gross), net, ci / ppmToBar, static_cast<float>(resistance)};
}

} // namespace facetbio
