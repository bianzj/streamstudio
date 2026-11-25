// ==========================================================================
// 文件：soil_emissivity.h
// 功能：土壤热红外发射率随体积含水量的变化（含水量越高，发射率越高），header-only。
// 主要模块：
//   1. 常数：干阈值 0.05、尺度 0.25（与 BSM 几何一致）、干→湿增益 0.03、发射率上限 0.995
//   2. soilWetWeight / soilWetWeightT：含水量 → 湿度权重 1 - exp(-mu)，mu = max(SMC-0.05,0)/0.25
//   3. soilSaturatedEmissivity：饱和（湿端）发射率，缺省 = 干端 + 增益并限幅
//   4. soilEmissivityAtMoisture / soilEmissivityAtMoistureT：干端到湿端按权重插值（T 版可求导）
// 数据流：地类表的干端发射率（或 energy.soilEmissivity 覆盖值）+ 像元含水量 → 本文件 → EB 与成像/反演共用。
// 关系：与 optics.cpp 的 BSM 土壤水分几何同参数；三维引擎对应实现为 models/histream 的 soil_tir_emissivity.h。
// 约定：含水量为 m3 m-3（先限幅到 [0,1]）；非有限含水量按干土处理；干端缺省值 0.955。
// ==========================================================================

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

// 常数：与 BSM 的含水量几何共用（干阈值、尺度 SMCp），保证反射率与发射率对含水量的响应一致。
constexpr double kSoilDryWaterContent = 0.05;    // m3/m3, BSM keeps the dry spectrum below this
constexpr double kSoilWaterContentScale = 0.25;  // m3/m3, BSM SMCp
constexpr double kSoilEmissivityGain = 0.03;     // dry -> wet emissivity increase
constexpr double kSoilMaxEmissivity = 0.995;     // an opaque surface cannot emit more

// 作用：含水量 → [0,1) 的湿度权重（double 版）；输入先限幅到 [0,1]，低于干阈值时权重为 0。
// 0 at the dry threshold, approaching 1 for saturated soil. Non-finite water
// content (a pixel without a soil-moisture value) keeps the dry value.
inline double soilWetWeight(double moisture)
{
    if (!std::isfinite(moisture)) return 0.0;
    const double above = std::max(std::clamp(moisture, 0.0, 1.0) - kSoilDryWaterContent, 0.0);
    return 1.0 - std::exp(-above / kSoilWaterContentScale);
}

// 作用：求饱和土壤发射率。wetEmissivity 为负或非有限 → 干端 + 增益，且不超过上限；
//       水只会抬高发射率，湿端低于干端时取干端。干端非有限时按 0.955 兜底。
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

// 作用：按湿度权重在干端与饱和端之间线性插值得到某含水量下的土壤发射率，结果限幅 [0,1]。
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

// T 版本：同上两个函数的可微形式，供同化对土壤含水量求导（非有限含水量同样取干值）。
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
