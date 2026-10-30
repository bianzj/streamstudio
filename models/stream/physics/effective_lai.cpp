#include "effective_lai.h"
#include "constants.h"

#include <algorithm>
#include <cmath>

namespace stream {
namespace {

constexpr double kPiLegacy = 3.1415926535;
constexpr float kMinimumCosine = 0.01f;
constexpr float kMinimumGap = 1.0e-12f;
constexpr int kHemisphereSamples = 90;
constexpr int kRowZenithSamples = 6;
constexpr int kRowAzimuthSamples = 12;

constexpr double kGaussMu[4] = {0.0694318442029737, 0.3300094782075719, 0.6699905217924281, 0.9305681557970263};
constexpr double kGaussMuWeight[4] = {0.1739274225687269, 0.3260725774312731, 0.3260725774312731, 0.1739274225687269};
constexpr double kGaussAzimuth[3] = {10.143149884133247, 45.0, 79.856850115866753};
constexpr double kGaussAzimuthWeight[3] = {5.0 / 18.0, 8.0 / 18.0, 5.0 / 18.0};

float safeCosine(float zenithDeg)
{
    const float bounded = std::clamp(zenithDeg, 0.0f, 89.0f);
    return std::max(static_cast<float>(std::cos(bounded * kRad)), kMinimumCosine);
}

float projectionOf(const Canopy& canopy) { return canopy.leafProjection > 0.0f ? canopy.leafProjection : kDefaultLeafProjection; }

bool hasCrownStructure(const Canopy& canopy)
{
    return canopy.type >= 1 && canopy.type <= 5 && canopy.lai > 0.0f && canopy.leafProjection > 0.0f &&
           canopy.treeStand > 0.0f && canopy.canopyHeight > 0.0f && canopy.crownShape > 0.0f;
}

bool hasRowStructure(const Canopy& canopy, const CropRows& rows)
{
    return rows.enabled && (canopy.type == 12 || canopy.type == 14) && canopy.lai > 0.0f && rows.spacing > 0.0f &&
           rows.width > 0.0f && rows.width <= rows.spacing && rows.height > 0.0f;
}

float crownGap(const Canopy& canopy, float zenithDeg)
{
    const float lai = std::max(canopy.lai, 0.0f);
    if (lai <= 0.0f) return 1.0f;
    const float projection = projectionOf(canopy);
    const float cosine = safeCosine(zenithDeg);
    if (!hasCrownStructure(canopy)) return std::clamp(std::exp(-projection * lai / cosine), kMinimumGap, 1.0f);

    const double theta = std::clamp(zenithDeg, 0.0f, 89.0f) * kRad;
    const double verticalRadius = canopy.canopyHeight / 2.0;
    const double horizontalRadius = canopy.canopyHeight / (2.0 * canopy.crownShape);
    const double crownArea = std::sqrt(horizontalRadius * horizontalRadius + std::pow(verticalRadius * std::tan(theta), 2.0)) *
                             kPiLegacy * horizontalRadius;
    if (!(crownArea > 0.0) || !std::isfinite(crownArea)) return std::clamp(std::exp(-projection * lai / cosine), kMinimumGap, 1.0f);
    // Exact transmittance through the projected ellipsoid; exp of the mean depth would underestimate it.
    const double tauMax = 1.5 * static_cast<double>(lai) * projection / (crownArea * canopy.treeStand * cosine);
    const double gapInside = tauMax > 1.0e-4 ? 2.0 * (1.0 - (1.0 + tauMax) * std::exp(-tauMax)) / (tauMax * tauMax)
                                             : 1.0 - 2.0 * tauMax / 3.0;
    const double gap = std::exp(-crownArea * (1.0 - gapInside) * canopy.treeStand);
    return std::clamp(static_cast<float>(gap), kMinimumGap, 1.0f);
}

float equivalentLai(double gap, float projection)
{
    if (!(gap < 1.0)) return 0.0f;
    const double target = std::log(std::max(gap, 1.0e-300));
    double x = 0.0;
    for (int iteration = 0; iteration < 60; ++iteration) {
        double value = 0.0, slope = 0.0;
        for (int i = 0; i < 4; ++i) {
            const double term = 2.0 * kGaussMu[i] * kGaussMuWeight[i] * std::exp(-projection * x / kGaussMu[i]);
            value += term;
            slope -= term * projection / kGaussMu[i];
        }
        const double step = (std::log(value) - target) / (-slope / value);
        x += step;
        if (std::abs(step) < 1.0e-10 * std::max(1.0, x)) break;
    }
    return static_cast<float>(x);
}

}  // namespace

float directionalGap(const Canopy& canopy, const CropRows& rows, float zenithDeg, float azimuthDeg)
{
    if (!hasRowStructure(canopy, rows)) return crownGap(canopy, zenithDeg);
    const float bounded = std::clamp(zenithDeg, 0.0f, 89.0f);
    const float cosine = safeCosine(bounded);
    const float crossRow = std::abs(std::sin((azimuthDeg - rows.azimuth) * kRad));
    const float projectedWidth = rows.width + rows.height * std::tan(bounded * kRad) * crossRow;
    const float cover = std::clamp(projectedWidth / rows.spacing, rows.width / rows.spacing, 1.0f);
    const float rowGap = std::exp(-projectionOf(canopy) * std::max(canopy.lai, 0.0f) / (cover * cosine));
    return std::clamp((1.0f - cover) + cover * rowGap, kMinimumGap, 1.0f);
}

float directionalLai(const Canopy& canopy, const CropRows& rows, float zenithDeg, float azimuthDeg)
{
    const float lai = std::max(canopy.lai, 0.0f);
    if (!hasRowStructure(canopy, rows) && !hasCrownStructure(canopy)) return lai;
    const float gap = directionalGap(canopy, rows, zenithDeg, azimuthDeg);
    return std::clamp(-std::log(gap) * safeCosine(zenithDeg) / projectionOf(canopy), 0.0f, lai);
}

float hemisphericalLai(const Canopy& canopy, const CropRows& rows)
{
    const float lai = std::max(canopy.lai, 0.0f);
    const bool row = hasRowStructure(canopy, rows);
    if (!row && !hasCrownStructure(canopy)) return lai;
    double weighted = 0.0, weightSum = 0.0;
    const int zenithSamples = row ? kRowZenithSamples : kHemisphereSamples;
    for (int sample = 0; sample < zenithSamples; ++sample) {
        const double theta = (sample + 0.5) * (0.5 * kPiLegacy / zenithSamples);
        const double weight = 2.0 * std::cos(theta) * std::sin(theta);
        double value = 0.0;
        if (row) {
            for (int a = 0; a < kRowAzimuthSamples; ++a)
                value += directionalLai(canopy, rows, static_cast<float>(theta / kRad), static_cast<float>((a + 0.5) * (360.0 / kRowAzimuthSamples)));
            value /= kRowAzimuthSamples;
        } else {
            value = directionalLai(canopy, rows, static_cast<float>(theta / kRad), 0.0f);
        }
        weighted += value * weight;
        weightSum += weight;
    }
    return std::clamp(static_cast<float>(weighted / weightSum), 0.0f, lai);
}

float hemisphericalLaiAnalytical(const Canopy& canopy, const CropRows& rows)
{
    const float lai = std::max(canopy.lai, 0.0f);
    const bool row = hasRowStructure(canopy, rows);
    if (!row && !hasCrownStructure(canopy)) return lai;
    const float projection = projectionOf(canopy);
    double gap = 0.0;
    for (int i = 0; i < 4; ++i) {
        const float zenith = static_cast<float>(std::acos(kGaussMu[i]) / kRad);
        double directional = 0.0;
        if (row) {
            for (int j = 0; j < 3; ++j)
                directional += kGaussAzimuthWeight[j] * directionalGap(canopy, rows, zenith, rows.azimuth + static_cast<float>(kGaussAzimuth[j]));
        } else {
            directional = crownGap(canopy, zenith);
        }
        gap += 2.0 * kGaussMu[i] * kGaussMuWeight[i] * directional;
    }
    return std::clamp(equivalentLai(gap, projection), 0.0f, lai);
}

EffectiveLai effectiveLai(const Canopy& canopy, const CropRows& rows, float szaDeg, float saaDeg, float vzaDeg, float vaaDeg,
                          bool analyticHemisphere)
{
    return EffectiveLai{
        analyticHemisphere ? hemisphericalLaiAnalytical(canopy, rows) : hemisphericalLai(canopy, rows),
        directionalLai(canopy, rows, szaDeg, saaDeg),
        directionalLai(canopy, rows, vzaDeg, vaaDeg)};
}

}  // namespace stream
