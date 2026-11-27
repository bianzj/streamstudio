#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace urban {

struct Canyon {
    double buildingHeight{};
    double streetWidth{};

    explicit operator bool() const { return buildingHeight > 0 && streetWidth > 0; }
};

inline Canyon canyonGeometry(double height, double width) {
    if (!std::isfinite(height) || !std::isfinite(width) || height <= 0 || width <= 0)
        throw std::runtime_error("Urban aerodynamics needs positive buildingHeight and streetWidth in metres");
    return {height, width};
}

struct Exchange {
    double roof{};
    double surface{}; // Neutral wall / road coefficient, W m-2 K-1 (no convective velocity).
    double outlet{}; // Per unit street plan area, W m-2 K-1.
    double canyonWind{}; // U_can, m s-1.
    double frictionVelocity{}; // u*, m s-1.
};

// Neutral subset of Masson (2000), sections 2.9.3–2.9.7. Rowley's
// 11.8 + 4.2 U is a heat transfer coefficient in W m-2 K-1.
inline Exchange neutralExchange(Canyon canyon, double referenceHeight, double wind, double rhoCp) {
    constexpr double kappa = 0.4;
    constexpr double roofRoughness = 0.15;
    const double h = canyon.buildingHeight;
    if (!(referenceHeight > h))
        throw std::runtime_error("Urban aerodynamics needs meteo.z above the mean roof height");
    const double zAboveRoof = std::max(referenceHeight - h, 2 * roofRoughness);
    const double displacementToRoof = h / 3;
    const double z0 = std::min(h / 10, 5.0);
    const double logTown = std::log((zAboveRoof + displacementToRoof) / z0);
    const double logRoof = std::log(zAboveRoof / roofRoughness);
    const double windAtReference = std::max(wind, 0.0);
    const double ustar = kappa * windAtReference / logTown;
    const double canyonWind = (2.0 / 3.14159265358979323846) * windAtReference *
        std::exp(-h / (4 * canyon.streetWidth)) * std::log(displacementToRoof / z0) / logTown;
    return {rhoCp * kappa * kappa * windAtReference / (logRoof * logRoof),
            11.8 + 4.2 * std::hypot(canyonWind, ustar),
            rhoCp * kappa * ustar / logTown,
            canyonWind, ustar};
}

// Convective velocity w* = (g / T_can * Q0 * h)^(1/3) of the canyon (Masson 2000; SURFEX scientific
// documentation eq. 3.23). Q0 is the kinematic sensible heat flux of the road and wall surfaces per
// street plan area, K m s-1; only an upward flux (unstable canyon) contributes.
inline double convectiveVelocity(double kinematicHeatFlux, double canyonTemperature, double buildingHeight) {
    constexpr double gravity = 9.80665;
    if (!(kinematicHeatFlux > 0) || !(canyonTemperature > 0)) return 0.0;
    return std::cbrt(gravity / canyonTemperature * kinematicHeatFlux * buildingHeight);
}

// Wall and road exchange coefficient with the convective velocity (SURFEX eq. 3.23):
// 11.8 + 4.2 sqrt(U_can^2 + (u* + w*)^2). wStar = 0 gives Exchange::surface.
inline double surfaceCoefficient(const Exchange& exchange, double convectiveVelocityValue) {
    return 11.8 + 4.2 * std::hypot(exchange.canyonWind, exchange.frictionVelocity + convectiveVelocityValue);
}

// Enforce sum(A_i h_s (T_i - T_can)) = A_road h_top (T_can - T_ref).
inline double canyonAirTemperature(Exchange exchange, double air, double roadArea,
                                   double surfaceArea, double areaTemperature) {
    const double outlet = roadArea * exchange.outlet;
    const double total = outlet + surfaceArea * exchange.surface;
    return total > 0 ? (outlet * air + exchange.surface * areaTemperature) / total : air;
}

} // namespace urban
