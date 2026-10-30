#pragma once

#include <algorithm>
#include <cmath>

namespace histream::energy_output {

enum class Exchange {
    inactive,
    singleSurface,
    twoSidedLeaf,
    prescribedMedium
};

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

struct Balance {
    double absorbedShortwave{}, absorbedLongwave{}, emittedLongwave{};
    double netRadiation{}, heatFluxSum{}, energyResidual{};
    double netRadiationSunlit{}, netRadiationShaded{};
    double heatFluxSumSunlit{}, heatFluxSumShaded{};
    double energyResidualSunlit{}, energyResidualShaded{};
    double thermalEmissivity{}, sunlitFraction{}, exchangeFaceCount{};
    bool closureApplicable{};
};

// Keep the emission coefficient used by production Stefen_Boltzmann(), rather
// than the slightly different SIGMASB constant used in its Newton derivative.
inline double emittedFlux(double temperatureK, double emissivity)
{
    const double scaled = temperatureK / 100.0;
    return scaled * scaled * scaled * scaled * emissivity * 5.6704;
}

inline Balance calculate(const Input& input)
{
    Balance result;
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
    result.absorbedShortwave = input.diffuseShortwave +
        result.sunlitFraction * input.directShortwave;
    result.absorbedLongwave = input.diffuseLongwave +
        result.sunlitFraction * input.directLongwave;
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
    result.heatFluxSumSunlit = faces * (input.sensibleSunlit + input.latentSunlit) +
        (leaf ? 0.0 : input.storageSunlit);
    result.heatFluxSumShaded = faces * (input.sensibleShaded + input.latentShaded) +
        (leaf ? 0.0 : input.storageShaded);
    result.emittedLongwave = mix(emittedSunlit, emittedShaded);
    result.netRadiation = mix(result.netRadiationSunlit, result.netRadiationShaded);
    result.heatFluxSum = mix(result.heatFluxSumSunlit, result.heatFluxSumShaded);
    result.energyResidualSunlit = result.netRadiationSunlit - result.heatFluxSumSunlit;
    result.energyResidualShaded = result.netRadiationShaded - result.heatFluxSumShaded;
    result.energyResidual = result.netRadiation - result.heatFluxSum;
    return result;
}

} // namespace histream::energy_output
