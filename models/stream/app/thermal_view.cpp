#include "thermal_view.h"
#include "../forcing/utc.h"
#include "../physics/brightness.h"
#include "../physics/canopy.h"
#include "../physics/constants.h"
#include "../physics/spa.h"

#include <cmath>

namespace stream {

Sun solarPosition(double unixSeconds, double latitude, double longitude, double elevation)
{
    const UtcTime time = utcFromSeconds(std::round(unixSeconds));
    spa_data sun{};
    sun.year = time.year; sun.month = time.month; sun.day = time.day;
    sun.hour = time.hour; sun.minute = time.minute; sun.second = static_cast<int>(std::lround(time.second)) % 60;
    sun.timezone = 0;
    sun.delta_t = 69;
    sun.latitude = latitude; sun.longitude = longitude;
    sun.elevation = std::isfinite(elevation) ? elevation : 0;
    sun.pressure = 0;  // geometric position, no refraction
    sun.temperature = 15;
    sun.function = SPA_ZA;
    Sun result;
    if (SPACalc{}.spa_calculate(&sun) != 0) return result;
    result.zenith = sun.zenith;
    result.azimuth = sun.azimuth;
    result.ok = true;
    return result;
}

template <class T>
std::array<T, 4> thermalWeightsT(const HomCanopyT<T>& nodes, const T& lai, double hotspot, double sunZenithDeg,
                                 double viewZenithDeg, double relativeAzimuthDeg, double leafEmissivity,
                                 const T& soilEmissivity)
{
    const FourFractionsT<T> parts = canopyFractionsT<T>(lai, hotspot, sunZenithDeg, viewZenithDeg, std::abs(relativeAzimuthDeg));
    if (lai > 0.0) {
        const ThermalScatterT<T> scatter = homThermalScatter<T>(nodes, T(leafEmissivity), soilEmissivity, viewZenithDeg, sunZenithDeg);
        return {ad::max(0.0, parts.soilSun * soilEmissivity + scatter.soilSun),
                ad::max(0.0, parts.soilShade * soilEmissivity + scatter.soilShade),
                ad::max(0.0, parts.leafSun * leafEmissivity + scatter.leafSun),
                ad::max(0.0, parts.leafShade * leafEmissivity + scatter.leafShade)};
    }
    return {ad::max(0.0, parts.soilSun * soilEmissivity), ad::max(0.0, parts.soilShade * soilEmissivity), T(0.0), T(0.0)};
}

std::array<double, 4> thermalWeights(const HomCanopy& nodes, double lai, double hotspot, double sunZenithDeg,
                                     double viewZenithDeg, double relativeAzimuthDeg, double leafEmissivity,
                                     double soilEmissivity)
{
    return thermalWeightsT<double>(nodes, lai, hotspot, sunZenithDeg, viewZenithDeg, relativeAzimuthDeg,
                                   leafEmissivity, soilEmissivity);
}

bool thermalSceneAt(const TerrainFields& terrain, size_t pixel, double sza, double saa, double vza, double vaa,
                    ThermalScene& scene)
{
    scene.sza = sza;
    scene.saa = saa;
    scene.vza = vza;
    scene.vaa = vaa;
    scene.thermalView = std::min(vza, 89.9);
    scene.thermalSza = sza;
    scene.terrain = ThermalTerrain::Flat;
    if (terrain.mode == TerrainRadiation::Facet && terrain.slope && !terrain.slope->empty()) {
        const double beta = (*terrain.slope)[pixel] * kPi / 180.0;
        const double aspect = (*terrain.aspect)[pixel] * kPi / 180.0;
        const double vzaRad = vza * kPi / 180.0, vaaRad = vaa * kPi / 180.0;
        const double cosView = std::cos(beta) * std::cos(vzaRad) + std::sin(beta) * std::sin(vzaRad) * std::cos(vaaRad - aspect);
        if (cosView <= 0.0) return false;
        scene.thermalView = std::acos(std::clamp(cosView, -1.0, 1.0)) * 180.0 / kPi;
        const double szaRad = sza * kPi / 180.0, saaRad = saa * kPi / 180.0;
        const double cosSun = std::cos(beta) * std::cos(szaRad) + std::sin(beta) * std::sin(szaRad) * std::cos(saaRad - aspect);
        scene.thermalSza = std::acos(std::clamp(cosSun, -1.0, 1.0)) * 180.0 / kPi;
    } else if (terrain.mode == TerrainRadiation::Hill && terrain.coneSlope && !terrain.coneSlope->empty()) {
        scene.terrain = ThermalTerrain::Hill;
        scene.slope = (*terrain.coneSlope)[pixel];
        scene.coverage = std::clamp(static_cast<double>((*terrain.coneDensity)[pixel]), 0.0, 1.0);
    } else if (terrain.mode == TerrainRadiation::Ridge && terrain.ridgeSlope && !terrain.ridgeSlope->empty()) {
        scene.terrain = ThermalTerrain::Ridge;
        scene.slope = (*terrain.ridgeSlope)[pixel];
        scene.coverage = std::clamp(static_cast<double>((*terrain.ridgeDensity)[pixel]), 0.0, 1.0);
        scene.strike = (*terrain.ridgeAspect)[pixel];
        scene.endFraction = std::clamp(terrain.endFraction, 0.0, 0.5);
    }
    return true;
}

void setThermalMaterial(ThermalScene& scene, const ImageConfig& config, const ClassParameters& entry,
                        const std::map<int, MoistureSoilEmissivity>& tables, int code)
{
    scene.hotspot = entry.canopy.hotspot;
    scene.leafEmissivity = std::clamp(1.0 - entry.leaf.reflTir - entry.leaf.transTir, 0.01, 1.0);
    // Dry (class) value: the retrieval convention normalises by it; the radiance uses the water-content value.
    scene.soilFixed = std::clamp(static_cast<double>(entry.ground.emissivity), 0.01, 1.0);
    scene.soilWet = config.soilEmissivityWet;
    scene.soil = entry.groundKind == GroundKind::Soil;
    const auto table = tables.find(code);
    scene.hapke = config.hapkeSoilEmissivity && scene.soil && table != tables.end() ? &table->second : nullptr;
    scene.reference = config.referenceSurfaceTemperature;
}

template <class T>
bool thermalPixel(const ThermalScene& scene, const std::array<T, 4>& temperatures, const T& lai, const T& moisture,
                  const std::vector<double>& wavelengths, std::vector<T>& brightness, std::vector<T>& surface)
{
    const size_t bands = wavelengths.size();
    brightness.assign(bands, T(std::nan("")));
    surface.assign(bands, T(std::nan("")));
    const double relative = std::fmod(scene.vaa - scene.saa, 360.0);
    const HomCanopyT<T> nodes(lai);
    const T soilMoist = scene.soil ? soilEmissivityAtMoistureT<T>(scene.soilFixed, scene.soilWet, moisture) : T(scene.soilFixed);
    const auto soilAt = [&](double view, bool clampLow) -> T {
        if (!scene.hapke) return soilMoist;
        const T value = scene.hapke->atT(view, moisture);
        return clampLow ? ad::clamp(value, 0.01, 1.0) : value;
    };
    const auto weightsAt = [&](double localSza, double localView, bool clampLow, std::array<T, 4>& w, std::array<T, 4>& wf) {
        w = thermalWeightsT<T>(nodes, lai, scene.hotspot, localSza, localView, relative, scene.leafEmissivity, soilAt(localView, clampLow));
        if (scene.reference)
            wf = thermalWeightsT<T>(nodes, lai, scene.hotspot, localSza, localView, relative, scene.leafEmissivity, T(scene.soilFixed));
        else wf = w;
    };
    // Radiance (per wavelength) and effective emissivity of one set of weights; false if a weighted component is missing.
    const auto accumulate = [&](const std::array<T, 4>& w, const std::array<T, 4>& wf, std::vector<T>& radiance, T& effective) {
        radiance.assign(bands, T(0.0));
        effective = T(0.0);
        for (int k = 0; k < 4; ++k) {
            if (!ad::isfinite(temperatures[k])) {
                if (w[k] > 0.0 || wf[k] > 0.0) return false;
                continue;
            }
            if (w[k] > 0.0)
                for (size_t b = 0; b < bands; ++b) radiance[b] += w[k] * planckT<T>(temperatures[k], wavelengths[b]);
            if (wf[k] > 0.0) effective += wf[k];
        }
        return true;
    };

    if (scene.terrain == ThermalTerrain::Flat) {
        std::array<T, 4> w, wf;
        weightsAt(scene.thermalSza, scene.thermalView, false, w, wf);
        std::vector<T> radiance;
        T effective;
        if (!accumulate(w, wf, radiance, effective)) return false;
        effective = ad::clamp(effective, 1e-6, 1.0);
        for (size_t b = 0; b < bands; ++b) {
            if (!(radiance[b] > 0.0)) return false;
            brightness[b] = inversePlanckT<T>(radiance[b], wavelengths[b]);
            surface[b] = inversePlanckT<T>(radiance[b] / effective, wavelengths[b]);
        }
        return true;
    }

    // Sub-pixel terrain composite (12 cone facets or 2 flanks + 2 end faces) over a flat background.
    const bool hill = scene.terrain == ThermalTerrain::Hill;
    const double beta = scene.slope * kPi / 180.0;
    const double strike = hill ? 0.0 : scene.strike * kPi / 180.0;
    const int facetCount = hill ? 12 : 4;
    const double vzaRad = scene.vza * kPi / 180.0, vaaRad = scene.vaa * kPi / 180.0;
    const double szaRad = scene.sza * kPi / 180.0, saaRad = scene.saa * kPi / 180.0;
    std::vector<T> radiance(bands, T(0.0)), part;
    T effective(0.0), partEffective;
    double area = 0.0;
    const auto add = [&](double localSza, double localView, double projection) {
        std::array<T, 4> w, wf;
        weightsAt(localSza, localView, true, w, wf);
        if (!accumulate(w, wf, part, partEffective)) return false;
        for (size_t b = 0; b < bands; ++b) radiance[b] += projection * part[b];
        effective += projection * partEffective;
        area += projection;
        return true;
    };
    for (int facet = 0; facet < facetCount; ++facet) {
        double facetBeta = beta, facetPhi = 0.0, facetWeight = 0.0;
        if (hill) {
            facetPhi = facet * 30.0 * kPi / 180.0;
            facetWeight = 1.0 / 12.0;
        } else if (facet < 2) {
            facetPhi = strike + (facet == 0 ? 0.5 * kPi : -0.5 * kPi);
            facetWeight = 0.5 * (1.0 - scene.endFraction);
        } else {
            facetBeta = 0.5 * kPi;
            facetPhi = strike + (facet == 2 ? 0.0 : kPi);
            facetWeight = 0.5 * scene.endFraction;
        }
        const double cosView = std::cos(facetBeta) * std::cos(vzaRad) + std::sin(facetBeta) * std::sin(vzaRad) * std::cos(vaaRad - facetPhi);
        if (cosView <= 0.0) continue;
        const double localView = std::acos(std::clamp(cosView, -1.0, 1.0)) * 180.0 / kPi;
        const double cosSun = std::cos(facetBeta) * std::cos(szaRad) + std::sin(facetBeta) * std::sin(szaRad) * std::cos(saaRad - facetPhi);
        const double localSza = std::acos(std::clamp(cosSun, -1.0, 1.0)) * 180.0 / kPi;
        if (!add(localSza, localView, scene.coverage * facetWeight * cosView)) return false;
    }
    if (!add(scene.thermalSza, scene.thermalView, (1.0 - scene.coverage) * std::cos(scene.thermalView * kPi / 180.0))) return false;
    if (!(area > 0.0)) return false;
    const T meanEffective = ad::clamp(effective / area, 1e-6, 1.0);
    for (size_t b = 0; b < bands; ++b) {
        if (!(radiance[b] > 0.0)) return false;
        const T mean = radiance[b] / area;
        brightness[b] = inversePlanckT<T>(mean, wavelengths[b]);
        surface[b] = inversePlanckT<T>(mean / meanEffective, wavelengths[b]);
    }
    return true;
}

template std::array<double, 4> thermalWeightsT<double>(const HomCanopyT<double>&, const double&, double, double, double,
                                                       double, double, const double&);
template std::array<Ad, 4> thermalWeightsT<Ad>(const HomCanopyT<Ad>&, const Ad&, double, double, double, double, double,
                                               const Ad&);
template bool thermalPixel<double>(const ThermalScene&, const std::array<double, 4>&, const double&, const double&,
                                   const std::vector<double>&, std::vector<double>&, std::vector<double>&);
template bool thermalPixel<Ad>(const ThermalScene&, const std::array<Ad, 4>&, const Ad&, const Ad&,
                               const std::vector<double>&, std::vector<Ad>&, std::vector<Ad>&);

}  // namespace stream
