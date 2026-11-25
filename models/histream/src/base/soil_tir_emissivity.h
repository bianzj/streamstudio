// ==========================================================================
// 文件：soil_tir_emissivity.h
// 功能：由土壤体积含水量（SMC）修正材质的热红外（TIR）反射率，湿土发射率更高（header-only）
// 主要模块：
//   1. 常量（干阈值 0.05、湿度尺度 0.25、干→湿发射率增量 0.03、发射率上限 0.995）
//   2. waterContentFraction：兼容体积分数与旧工程百分数两种 SMC 写法
//   3. wetWeight：BSM 几何的湿度权重 1 - exp(-mu)
//   4. tirReflectance：干/湿反射率按权重线性插值，保证随含水量单调不增
// 数据流：compo.cpp / facetrtcore.cpp 读材质 refl_tir、tau_tir、湿反射率和 SMC
//         → tirReflectance → 写回 TIR 反射率；两个引擎再按 发射率 = 1 - 反射率 - 透射率 推导
// 注意：models/stream/physics/soil_emissivity.h 保留一份等价拷贝，修改时需同步；
//       反射率、透射率均为无量纲 0..1，SMC 单位 m3/m3
// ==========================================================================

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

// 命名空间：土壤光学参数（目前只含 TIR 反射率的含水量修正）。
namespace soiloptics {

constexpr float kDryWaterContent = 0.05f;    // m3/m3, BSM keeps the dry spectrum below this
constexpr float kWaterContentScale = 0.25f;  // m3/m3, BSM SMCp
constexpr float kEmissivityGain = 0.03f;     // dry -> wet emissivity increase
constexpr float kMaxEmissivity = 0.995f;     // an opaque surface cannot emit more

// Accept both volumetric fractions (0.25) and legacy percentages (25).
// 作用：把 SMC 规整为 0..1 的体积分数；大于 1 的值按旧工程的百分数处理（乘 0.01）。
// 非有限值按 0（干土）处理，最后限幅到 [0, 1]。
inline float waterContentFraction(float smc)
{
    const float value = std::isfinite(smc) ? (smc > 1.0f ? smc * 0.01f : smc) : 0.0f;
    return std::clamp(value, 0.0f, 1.0f);
}

// 0 at the dry threshold, approaching 1 for saturated soil.
// 作用：湿度权重 w = 1 - exp(-max(SMC - 0.05, 0) / 0.25)；干阈值以下为 0，趋向饱和时为 1。
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
// 实现：反射率 = dry + (wet - dry) * wetWeight(smc)。
inline float tirReflectance(float dryReflectance, float transmittance, float wetReflectance, float smc)
{
    // 步骤 1：规整干反射率（缺失时取 0.05，限幅到 0..1）与透射率（限幅使 dry + tau <= 1，发射率不为负）。
    const float dry = std::clamp(std::isfinite(dryReflectance) ? dryReflectance : 0.05f, 0.0f, 1.0f);
    const float tau = std::clamp(std::isfinite(transmittance) ? transmittance : 0.0f, 0.0f, 1.0f - dry);
    const float dryEmissivity = 1.0f - dry - tau;
    // 步骤 2：确定饱和（湿）反射率；未给出（非有限或为负）时由“干发射率 + 0.03”
    // （不超过 0.995）反推。再限幅，且不允许湿反射率高于干反射率（水只会降低 TIR 反射率）。
    float wet = wetReflectance;
    if (!std::isfinite(wet) || wet < 0.0f)
        wet = 1.0f - std::min(dryEmissivity + kEmissivityGain, kMaxEmissivity) - tau;
    wet = std::clamp(wet, 0.0f, 1.0f - tau);
    if (wet > dry) wet = dry;
    // 步骤 3：按含水量权重在干、湿反射率之间插值。
    return dry + (wet - dry) * wetWeight(smc);
}

}  // namespace soiloptics
