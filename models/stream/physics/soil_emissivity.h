#pragma once

// Soil thermal emissivity from the volumetric water content.
//
// Wet soil emits more: water fills the pore space, the reflecting surface
// becomes optically smoother and its TIR reflectance falls, so emissivity rises.
// The image engine keeps the soil emissivity as a class constant and the BSM
// soil reflectance as a function of the pixel water content; this module links
// the two so one moisture field drives both.
//
// The weight deliberately reuses the BSM geometry (dry threshold 0.05 m3/m3,
// scale SMCp = 0.25, the same constants as optics.cpp) so reflectance and
// emissivity respond to water content consistently:
//     mu  = max(SMC - 0.05, 0) / 0.25
//     wet = 1 - exp(-mu)
// SMC at or below 0.05 equals the dry class value; 0.25 reaches 55 % of the
// dry-to-wet range; 0.55 reaches about 87 %.
//
// models/histream/src/base/soil_tir_emissivity.h holds the three-dimensional
// counterpart; keep the two in step.

#include "dual.h"

#include <algorithm>
#include <cmath>

namespace stream {

constexpr double kSoilDryWaterContent = 0.05;    // m3/m3, BSM keeps the dry spectrum below this
constexpr double kSoilWaterContentScale = 0.25;  // m3/m3, BSM SMCp
constexpr double kSoilEmissivityGain = 0.03;     // dry -> wet emissivity increase
constexpr double kSoilMaxEmissivity = 0.995;     // an opaque surface cannot emit more

// 0 at the dry threshold, approaching 1 for saturated soil. Non-finite water
// content (a pixel without a soil-moisture value) keeps the dry value.
inline double soilWetWeight(double moisture)
{
    if (!std::isfinite(moisture)) return 0.0;
    const double above = std::max(std::clamp(moisture, 0.0, 1.0) - kSoilDryWaterContent, 0.0);
    return 1.0 - std::exp(-above / kSoilWaterContentScale);
}

// Emissivity of a saturated soil. `wetEmissivity` negative means
// "dry emissivity + gain, capped"; water can only raise emissivity, so a wet
// value below the dry one is pulled back to the dry value.
inline double soilSaturatedEmissivity(double dryEmissivity, double wetEmissivity)
{
    const double dry = std::clamp(std::isfinite(dryEmissivity) ? dryEmissivity : 0.955, 0.0, 1.0);
    double wet = wetEmissivity;
    if (!std::isfinite(wet) || wet < 0.0)
        wet = std::min(dry + kSoilEmissivityGain, kSoilMaxEmissivity);
    wet = std::clamp(wet, 0.0, 1.0);
    return wet < dry ? dry : wet;
}

// Emissivity of a soil class at a water content.
//   dryEmissivity: the class value from classes.json (or the energy override)
//   wetEmissivity: the saturated limit; negative means
//                  "dry emissivity + gain, capped"
inline double soilEmissivityAtMoisture(double dryEmissivity, double wetEmissivity, double moisture)
{
    const double dry = std::clamp(std::isfinite(dryEmissivity) ? dryEmissivity : 0.955, 0.0, 1.0);
    const double wet = soilSaturatedEmissivity(dry, wetEmissivity);
    return std::clamp(dry + (wet - dry) * soilWetWeight(moisture), 0.0, 1.0);
}

// Differentiable forms (T = double or Ad) of the two functions above.
template <class T>
T soilWetWeightT(const T& moisture)
{
    if (!ad::isfinite(moisture)) return T(0.0);
    const T above = ad::max(ad::clamp(moisture, 0.0, 1.0) - kSoilDryWaterContent, 0.0);
    return T(1.0) - ad::exp(-above / kSoilWaterContentScale);
}

template <class T>
T soilEmissivityAtMoistureT(double dryEmissivity, double wetEmissivity, const T& moisture)
{
    const double dry = std::clamp(std::isfinite(dryEmissivity) ? dryEmissivity : 0.955, 0.0, 1.0);
    const double wet = soilSaturatedEmissivity(dry, wetEmissivity);
    return ad::clamp(dry + (wet - dry) * soilWetWeightT(moisture), 0.0, 1.0);
}

}  // namespace stream
