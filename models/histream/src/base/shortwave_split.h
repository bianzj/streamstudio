#pragma once
// Direct/diffuse split of a measured global shortwave Rin per meteorological
// node (light.diffuseFractionMode), shared by VoxelEB and FacetEB.
//
//   fixed : the configured directScatteringRatio for every node (legacy).
//   erbs  : measured horizontal direct/diffuse columns of the meteorology file
//           when present, otherwise the Erbs et al. (1982) diffuse fraction of
//           the clearness index kt = Rin / (Isc * E0 * cos(theta_s)).
//           With the sun below 1 degree elevation, or Rin <= 0, all shortwave
//           is diffuse.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace shortwave_split {

constexpr int kModeFixed = 0;
constexpr int kModeErbs = 1;

// Solar constant used with the Erbs correlation (W m-2).
constexpr double kSolarConstant = 1367.0;
// cos(theta_s) below which (sun elevation < 1 degree) the split is all diffuse.
constexpr double kLowSunCosine = 0.0175;

inline int parseMode(std::string text)
{
    text.erase(std::remove_if(text.begin(), text.end(),
                              [](unsigned char c) { return std::isspace(c) != 0; }),
               text.end());
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text == "erbs" ? kModeErbs : kModeFixed;
}

// Spencer (1971) eccentricity correction (r0/r)^2 for day of year 1..366.
inline double eccentricityFactor(int dayOfYear)
{
    const double gamma = 2.0 * 3.14159265358979323846 * (std::clamp(dayOfYear, 1, 366) - 1) / 365.0;
    return 1.000110 + 0.034221 * std::cos(gamma) + 0.001280 * std::sin(gamma) +
        0.000719 * std::cos(2.0 * gamma) + 0.000077 * std::sin(2.0 * gamma);
}

// Erbs et al. (1982) hourly diffuse fraction kd(kt).
inline double erbsDiffuseFraction(double kt)
{
    if (kt <= 0.22) return 1.0 - 0.09 * std::max(kt, 0.0);
    if (kt <= 0.80)
        return 0.9511 - 0.1604 * kt + 4.388 * kt * kt - 16.638 * kt * kt * kt +
            12.336 * kt * kt * kt * kt;
    return 0.165;
}

struct Split {
    float directFraction{0};
    float clearnessIndex{-1};  // < 0 when not evaluated
    const char* source{"fixed"};
};

// measuredDirect/measuredDiffuse: horizontal W m-2 columns of the meteorology
// row, NaN when the file has none.
inline Split split(int mode, float fixedDirectFraction, float rin, float solarZenithDegrees,
                   int dayOfYear, float measuredDirect, float measuredDiffuse)
{
    Split result;
    if (mode != kModeErbs) {
        result.directFraction = std::clamp(fixedDirectFraction, 0.0f, 1.0f);
        return result;
    }
    const double cosine = std::cos(solarZenithDegrees * 3.14159265358979323846 / 180.0);
    if (std::isfinite(measuredDirect) && std::isfinite(measuredDiffuse) &&
        measuredDirect >= 0.0f && measuredDiffuse >= 0.0f) {
        result.source = "measured";
        const double sum = double(measuredDirect) + double(measuredDiffuse);
        result.directFraction = sum > 0.0 && rin > 0.0f
            ? static_cast<float>(std::clamp(double(measuredDirect) / sum, 0.0, 1.0)) : 0.0f;
        if (cosine > kLowSunCosine)
            result.clearnessIndex = static_cast<float>(
                std::max(0.0, double(rin)) / (kSolarConstant * eccentricityFactor(dayOfYear) * cosine));
        return result;
    }
    result.source = "erbs";
    if (!(rin > 0.0f) || !(cosine > kLowSunCosine)) {
        result.directFraction = 0.0f;
        result.clearnessIndex = 0.0f;
        return result;
    }
    const double kt = double(rin) / (kSolarConstant * eccentricityFactor(dayOfYear) * cosine);
    result.clearnessIndex = static_cast<float>(kt);
    result.directFraction = static_cast<float>(std::clamp(1.0 - erbsDiffuseFraction(kt), 0.0, 1.0));
    return result;
}

}  // namespace shortwave_split
