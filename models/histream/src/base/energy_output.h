// ==========================================================================
// 文件：energy_output.h
// 功能：能量平衡输出（VoxelEB/FacetEB 等）的辐射与热通量汇总：由 GPU 读回的吸收短波/长波、
//       显热/潜热/储热和温度，计算净辐射 Rn、热通量和 Rn - (H+LE+G) 的闭合残差，
//       并给出光照/阴影两部分与按光照比例混合的整体值。
// 主要模块：
//   1. Exchange 枚举：交换面类型（不适用 / 单面 / 双面叶 / 给定介质如火焰、雾）
//   2. Input / Balance 结构体：计算输入与输出
//   3. emittedFlux：Stefan–Boltzmann 出射长波
//   4. calculate：逐项汇总（吸收、发射、Rn、热通量和、残差）
// 数据流：调用方填充 Input → calculate → Balance 写入输出图像/诊断元数据。
// 约定：通量单位 W m-2（对叶片为单位叶面积）；温度 K；符号约定：H、LE、G 取离开表面为正，
//       残差 energyResidual = Rn - (H + LE + G)；directShortwave/diffuse* 均为已吸收量，
//       sunlitFraction 钳制到 [0,1]；单面 faces=1，双面叶 faces=2（发射与 H/LE 随面数缩放）。
// 注意：closureApplicable=false（给定介质/不活跃）时，所有通量输出为 0 占位，仅用于保持输出为有限值，
//       元数据与诊断会标明“不适用”。
// ==========================================================================

#pragma once

#include <algorithm>
#include <cmath>

namespace histream::energy_output {

// 能量交换类型：inactive 不参与；singleSurface 单面（土壤、墙体、水体等）；
// twoSidedLeaf 双面叶片（叶两面各自交换）；prescribedMedium 给定的体积发射介质（火焰/雾），
// 不求解表面能量平衡，因此无有效的闭合。
enum class Exchange {
    inactive,
    singleSurface,
    twoSidedLeaf,
    prescribedMedium
};

// 输入（W m-2，K）：直射/漫射短波与长波吸收量、光照/阴影面温度、光照面比例、
// 热红外反射率/透射率（用于求发射率）、光照/阴影的显热 H、潜热 LE、储热 G。
struct Input {
    double directShortwave{}, diffuseShortwave{};
    double directLongwave{}, diffuseLongwave{};
    double sunlitTemperature{}, shadedTemperature{}, sunlitFraction{};
    double thermalReflectance{}, thermalTransmittance{};
    double sensibleSunlit{}, sensibleShaded{};
    double latentSunlit{}, latentShaded{};
    double storageSunlit{}, storageShaded{};
    Exchange exchange{Exchange::inactive};
};

// 输出：吸收短波/长波、发射长波、净辐射、热通量和（H+LE+G）、能量残差，
// 以及光照/阴影分量、热红外发射率、光照比例、交换面数和闭合是否适用的标志。
struct Balance {
    double absorbedShortwave{}, absorbedLongwave{}, emittedLongwave{};
    double netRadiation{}, heatFluxSum{}, energyResidual{};
    double netRadiationSunlit{}, netRadiationShaded{};
    double heatFluxSumSunlit{}, heatFluxSumShaded{};
    double energyResidualSunlit{}, energyResidualShaded{};
    double thermalEmissivity{}, sunlitFraction{}, exchangeFaceCount{};
    bool closureApplicable{};
};

// 作用：灰体出射长波 = ε·σ·T^4，σ=5.6704e-8 W m-2 K-4（写成 (T/100)^4 × 5.6704 的形式）。
// 输入：温度 temperatureK（K）、发射率 emissivity；输出：W m-2。
// Keep the emission coefficient used by production Stefen_Boltzmann(), rather
// than the slightly different SIGMASB constant used in its Newton derivative.
inline double emittedFlux(double temperatureK, double emissivity)
{
    const double scaled = temperatureK / 100.0;
    return scaled * scaled * scaled * scaled * emissivity * 5.6704;
}

// 作用：由 Input 计算 Balance。
// 输入/输出见 Input、Balance；光照和阴影各算一遍，整体值按 sunlitFraction 线性混合（mix）。
inline Balance calculate(const Input& input)
{
    Balance result;
    // 步骤 1：光照比例限幅；热红外发射率 = 1 - 反射率 - 透射率（基尔霍夫定律，限幅到 [0,1]）；
    //         判定是否存在可闭合的表面（单面或双面叶）及交换面数。
    result.sunlitFraction = std::clamp(input.sunlitFraction, 0.0, 1.0);
    result.thermalEmissivity = std::clamp(
        1.0 - input.thermalReflectance - input.thermalTransmittance, 0.0, 1.0);
    result.closureApplicable = input.exchange == Exchange::singleSurface ||
        input.exchange == Exchange::twoSidedLeaf;
    const bool leaf = input.exchange == Exchange::twoSidedLeaf;
    const double faces = leaf ? 2.0 : 1.0;
    result.exchangeFaceCount = result.closureApplicable ? faces : 0.0;
    const auto mix = [&](double sunlit, double shaded) {
        return result.sunlitFraction * sunlit + (1.0 - result.sunlitFraction) * shaded;
    };
    // Diffuse inputs are already the total absorbed by every exchanging face
    // (full-sphere integral for leaves); only emission scales with faces.
    // 步骤 2：吸收的短波/长波 = 漫射 + 光照比例 × 直射。
    result.absorbedShortwave = input.diffuseShortwave +
        result.sunlitFraction * input.directShortwave;
    result.absorbedLongwave = input.diffuseLongwave +
        result.sunlitFraction * input.directLongwave;
    // 步骤 3：不适用情形（给定火/雾介质或未激活）直接返回 0 占位。
    if (!result.closureApplicable) {
        // A prescribed fire/fog source has path-dependent volumetric emission,
        // no solved surface energy balance, and no valid leaf-area Rn or closure.
        // Preserve the finite-float process contract. These zero placeholders
        // are explicitly marked not applicable in metadata and diagnostics.
        const double noData = 0.0;
        result.emittedLongwave = result.netRadiation = result.heatFluxSum =
            result.energyResidual = result.netRadiationSunlit = result.netRadiationShaded =
            result.heatFluxSumSunlit = result.heatFluxSumShaded =
            result.energyResidualSunlit = result.energyResidualShaded = noData;
        return result;
    }
    // 步骤 4：光照/阴影部分的发射长波与净辐射 Rn = 吸收 - 发射（阴影部分无直射项）。
    const double emittedSunlit = faces * emittedFlux(
        input.sunlitTemperature, result.thermalEmissivity);
    const double emittedShaded = faces * emittedFlux(
        input.shadedTemperature, result.thermalEmissivity);
    const double diffuseAbsorbed = input.diffuseShortwave + input.diffuseLongwave;
    result.netRadiationSunlit = diffuseAbsorbed + input.directShortwave +
        input.directLongwave - emittedSunlit;
    result.netRadiationShaded = diffuseAbsorbed - emittedShaded;
    // H and LE GPU fields are per emitting/exchanging face. Leaf G is not part
    // of budget.comp; solid G includes soil conduction or water heat storage.
    // 步骤 5：热通量和：H、LE 为每个交换面的值，乘面数；叶片不计 G，固体面计入土壤导热/水体储热。
    result.heatFluxSumSunlit = faces * (input.sensibleSunlit + input.latentSunlit) +
        (leaf ? 0.0 : input.storageSunlit);
    result.heatFluxSumShaded = faces * (input.sensibleShaded + input.latentShaded) +
        (leaf ? 0.0 : input.storageShaded);
    // 步骤 6：按光照比例混合出整体值，并求光照/阴影/整体的能量残差 Rn - (H+LE+G)。
    result.emittedLongwave = mix(emittedSunlit, emittedShaded);
    result.netRadiation = mix(result.netRadiationSunlit, result.netRadiationShaded);
    result.heatFluxSum = mix(result.heatFluxSumSunlit, result.heatFluxSumShaded);
    result.energyResidualSunlit = result.netRadiationSunlit - result.heatFluxSumSunlit;
    result.energyResidualShaded = result.netRadiationShaded - result.heatFluxSumShaded;
    result.energyResidual = result.netRadiation - result.heatFluxSum;
    return result;
}

} // namespace histream::energy_output
