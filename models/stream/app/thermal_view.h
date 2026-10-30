#pragma once
// Thermal observation geometry shared by forward imaging (image.cpp) and inversion (inversion.cpp).
// Both must see exactly the same component weights, otherwise a retrieval of a simulated image would not
// reproduce the temperatures that generated it.
#include "../physics/hapke.h"
#include "../physics/radiosity.h"
#include "../physics/soil_emissivity.h"

#include <algorithm>
#include <array>

namespace stream {

struct Sun { double zenith = 0, azimuth = 0; bool ok = false; };

// NREL SPA geometric solar position (no refraction), degrees; azimuth clockwise from north.
Sun solarPosition(double unixSeconds, double latitude, double longitude, double elevation);

// Directional soil emissivity at a water content. The Hapke albedo solve is
// anchored to the hemispherical emissivity, so one table is built at the dry
// class value and one at saturation and the two are blended by the same
// moisture weight that scales the level (second-order accurate over the
// <= 0.04 emissivity range between them).
struct MoistureSoilEmissivity {
    SoilEmissivityTable dry, wet;
    void build(double K, double dryEmissivity, double wetEmissivity)
    {
        dry.build(K, std::clamp(dryEmissivity, 1e-6, 1.0));
        wet.build(K, std::clamp(wetEmissivity, 1e-6, 1.0));
    }
    double at(double viewZenithDeg, double moisture) const
    {
        const double weight = soilWetWeight(moisture);
        if (!(weight > 0.0)) return dry.at(viewZenithDeg);
        return dry.at(viewZenithDeg) * (1.0 - weight) + wet.at(viewZenithDeg) * weight;
    }
};

// Effective-emissivity weights of [soil sun, soil shade, leaf sun, leaf shade] for one view: TiRT standard
// four-component fractions times the component emissivity, plus the hom3 multiple-scattering contribution.
// The directional radiance of the pixel is sum_k w_k B(T_k); sum_k w_k is its effective emissivity.
std::array<double, 4> thermalWeights(const HomCanopy& nodes, double lai, double hotspot, double sunZenithDeg,
                                     double viewZenithDeg, double relativeAzimuthDeg, double leafEmissivity,
                                     double soilEmissivity);

}  // namespace stream
