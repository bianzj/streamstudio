#pragma once

// Soil thermal-infrared emissivity from the volumetric water content.
//
// Wet soil emits more: water fills the pore space, the reflecting surface
// becomes optically smoother and its TIR reflectance falls.  Both engines store
// TIR optics as a reflectance per material and derive
//     emissivity = 1 - reflectance - transmittance
// (FacetEB: makeEnergyLongwaveOptics, VoxelEB: the fixed 10500 nm band), so this
// module only has to turn the material's dry TIR reflectance into the
// reflectance at the material's water content; every emissivity consumer then
// follows without further changes.
//
// The moisture weight deliberately reuses SCOPE's BSM geometry (dry threshold
// 0.05 m3/m3, scale SMCp = 0.25) so that one material drives its visible/SWIR
// reflectance and its TIR emissivity from the same water content with one
// formula:
//     mu  = max(SMC - 0.05, 0) / 0.25
//     wet = 1 - exp(-mu)
// SMC at or below 0.05 keeps the dry value unchanged; 0.25 reaches 55 % of the
// dry-to-wet range; 0.55 (the upper end of the BSM range) reaches about 87 %.
//
// The stream image engine keeps an identical copy in
// models/stream/physics/soil_emissivity.h; keep the two in step.

#include <algorithm>
#include <cmath>

namespace soiloptics {

constexpr float kDryWaterContent = 0.05f;    // m3/m3, BSM keeps the dry spectrum below this
constexpr float kWaterContentScale = 0.25f;  // m3/m3, BSM SMCp
constexpr float kEmissivityGain = 0.03f;     // dry -> wet emissivity increase
constexpr float kMaxEmissivity = 0.995f;     // an opaque surface cannot emit more

// Accept both volumetric fractions (0.25) and legacy percentages (25).
inline float waterContentFraction(float smc)
{
    const float value = std::isfinite(smc) ? (smc > 1.0f ? smc * 0.01f : smc) : 0.0f;
    return std::clamp(value, 0.0f, 1.0f);
}

// 0 at the dry threshold, approaching 1 for saturated soil.
inline float wetWeight(float smc)
{
    const float above = std::max(waterContentFraction(smc) - kDryWaterContent, 0.0f);
    return 1.0f - std::exp(-above / kWaterContentScale);
}

// TIR reflectance of a soil at `smc`.
//   dryReflectance / transmittance: the material's TIR values at SMC <= 0.05
//   wetReflectance: the TIR reflectance at saturation; negative means
//                   "dry emissivity + kEmissivityGain, capped"
// Water can only lower the TIR reflectance, so a wet value above the dry one is
// pulled back to the dry value and the curve stays monotone in water content.
inline float tirReflectance(float dryReflectance, float transmittance, float wetReflectance, float smc)
{
    const float dry = std::clamp(std::isfinite(dryReflectance) ? dryReflectance : 0.05f, 0.0f, 1.0f);
    const float tau = std::clamp(std::isfinite(transmittance) ? transmittance : 0.0f, 0.0f, 1.0f - dry);
    const float dryEmissivity = 1.0f - dry - tau;
    float wet = wetReflectance;
    if (!std::isfinite(wet) || wet < 0.0f)
        wet = 1.0f - std::min(dryEmissivity + kEmissivityGain, kMaxEmissivity) - tau;
    wet = std::clamp(wet, 0.0f, 1.0f - tau);
    if (wet > dry) wet = dry;
    return dry + (wet - dry) * wetWeight(smc);
}

}  // namespace soiloptics
