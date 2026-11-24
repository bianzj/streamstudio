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
    std::array<T, 4> w;
    if (lai > 0.0) {
        const ThermalScatterT<T> scatter = homThermalScatter<T>(nodes, T(leafEmissivity), soilEmissivity, viewZenithDeg, sunZenithDeg);
        w = {ad::max(0.0, parts.soilSun * soilEmissivity + scatter.soilSun),
             ad::max(0.0, parts.soilShade * soilEmissivity + scatter.soilShade),
             ad::max(0.0, parts.leafSun * leafEmissivity + scatter.leafSun),
             ad::max(0.0, parts.leafShade * leafEmissivity + scatter.leafShade)};
    } else {
        w = {ad::max(0.0, parts.soilSun * soilEmissivity), ad::max(0.0, parts.soilShade * soilEmissivity), T(0.0), T(0.0)};
    }
    // 2026-11-20: with the sun at or below the (local) horizon nothing is sunlit. The canopy kernels clamp the
    // zenith to 89.999°, which left ~15 % "sunlit leaf" weight at night; merge the sunlit shares into the shaded
    // components as the optical path does (canopy.cpp). Σw (the effective emissivity) is unchanged.
    if (!(sunZenithDeg < 90.0)) {
        w[1] = w[1] + w[0];
        w[3] = w[3] + w[2];
        w[0] = T(0.0);
        w[2] = T(0.0);
    }
    return w;
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
    // Single slope and the DEM facet field both give one plane per pixel (2026-11-20: single slope used to be
    // imaged with the horizontal geometry although its shortwave forcing was tilted).
    if ((terrain.mode == TerrainRadiation::Facet || terrain.mode == TerrainRadiation::Single) &&
        terrain.slope && !terrain.slope->empty()) {
        const double beta = (*terrain.slope)[pixel] * kPi / 180.0;
        const double aspect = (*terrain.aspect)[pixel] * kPi / 180.0;
        const double vzaRad = vza * kPi / 180.0, vaaRad = vaa * kPi / 180.0;
        const double cosView = std::cos(beta) * std::cos(vzaRad) + std::sin(beta) * std::sin(vzaRad) * std::cos(vaaRad - aspect);
        if (cosView <= 0.0) return false;
        scene.thermalView = std::acos(std::clamp(cosView, -1.0, 1.0)) * 180.0 / kPi;
        const double szaRad = sza * kPi / 180.0, saaRad = saa * kPi / 180.0;
        const double cosSun = std::cos(beta) * std::cos(szaRad) + std::sin(beta) * std::sin(szaRad) * std::cos(saaRad - aspect);
        scene.thermalSza = std::acos(std::clamp(cosSun, -1.0, 1.0)) * 180.0 / kPi;
        // FIX(2026-11-21 S-2): the hotspot / sunlit-fraction kernels also need the azimuth difference in the slope
        // plane, not the horizontal vaa - saa (only the zenith angles used to be transformed).
        scene.thermalRelative = localRelativeAzimuth(sza, saa, vza, vaa, scene.thermalSza, scene.thermalView);
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

double localRelativeAzimuth(double szaDeg, double saaDeg, double vzaDeg, double vaaDeg, double localSzaDeg,
                            double localVzaDeg)
{
    // FIX(2026-11-21 S-2): azimuth difference in the facet plane from the rotation-invariant scattering angle.
    const double d = kPi / 180.0;
    const double horizontal = std::fmod(vaaDeg - saaDeg, 360.0);
    const double dot = std::cos(szaDeg * d) * std::cos(vzaDeg * d) +
                       std::sin(szaDeg * d) * std::sin(vzaDeg * d) * std::cos((vaaDeg - saaDeg) * d);
    const double denominator = std::sin(localSzaDeg * d) * std::sin(localVzaDeg * d);
    if (!(std::abs(denominator) > 1e-9)) return horizontal;
    const double cosine = (dot - std::cos(localSzaDeg * d) * std::cos(localVzaDeg * d)) / denominator;
    return std::acos(std::clamp(cosine, -1.0, 1.0)) / d;
}

namespace {

// FIX(2026-11-21 S-1/S-2): the sub-pixel views of one thermal scene, enumerated once for the radiance synthesis
// (thermalPixel) and for its linear weights (thermalSceneWeights, used by the LST / LSCT retrieval). visit(localSza,
// localView, localRelative, projection, clampLow) returns false to abort. Flat / single slope / facet: one view with
// projection 1; hill / ridge: the visible facets (coverage x facet weight x cos view) plus the flat background.
// The order and arithmetic are those of the former thermalPixel loops; the facet azimuth is now local (S-2).
template <class Visit>
bool forEachThermalView(const ThermalScene& scene, Visit&& visit)
{
    const double horizontal = std::fmod(scene.vaa - scene.saa, 360.0);
    if (scene.terrain == ThermalTerrain::Flat) {
        const double relative = std::isfinite(scene.thermalRelative) ? scene.thermalRelative : horizontal;
        return visit(scene.thermalSza, scene.thermalView, relative, 1.0, false);
    }
    // Sub-pixel terrain composite (12 cone facets or 2 flanks + 2 end faces) over a flat background.
    const bool hill = scene.terrain == ThermalTerrain::Hill;
    const double beta = scene.slope * kPi / 180.0;
    const double strike = hill ? 0.0 : scene.strike * kPi / 180.0;
    const int facetCount = hill ? 12 : 4;
    const double vzaRad = scene.vza * kPi / 180.0, vaaRad = scene.vaa * kPi / 180.0;
    const double szaRad = scene.sza * kPi / 180.0, saaRad = scene.saa * kPi / 180.0;
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
        // FIX(2026-11-21 S-2): facet-local relative azimuth (was the horizontal vaa - saa for every facet).
        const double localRelative = localRelativeAzimuth(scene.sza, scene.saa, scene.vza, scene.vaa, localSza, localView);
        if (!visit(localSza, localView, localRelative, scene.coverage * facetWeight * cosView, true)) return false;
    }
    return visit(scene.thermalSza, scene.thermalView, horizontal, (1.0 - scene.coverage) * std::cos(scene.thermalView * kPi / 180.0), true);
}

}  // namespace

template <class T>
bool thermalPixel(const ThermalScene& scene, const std::array<T, 4>& temperatures, const T& lai, const T& moisture,
                  const std::vector<double>& wavelengths, std::vector<T>& brightness, std::vector<T>& surface)
{
    const size_t bands = wavelengths.size();
    brightness.assign(bands, T(std::nan("")));
    surface.assign(bands, T(std::nan("")));
    // Isotropic sky radiance per band for the reflected term (0 when no sky temperature is set).
    std::vector<double> sky(bands, 0.0);
    if (std::isfinite(scene.skyTemperature) && scene.skyTemperature > 0.0)
        for (size_t b = 0; b < bands; ++b) sky[b] = planck(scene.skyTemperature, wavelengths[b]);
    // FIX(2026-11-21 S-2): the relative azimuth is passed per sub-view (local for slope / facets, horizontal otherwise).
    const double horizontalRelative = std::fmod(scene.vaa - scene.saa, 360.0);
    const HomCanopyT<T> nodes(lai);
    const T soilMoist = scene.soil ? soilEmissivityAtMoistureT<T>(scene.soilFixed, scene.soilWet, moisture) : T(scene.soilFixed);
    const auto soilAt = [&](double view, bool clampLow) -> T {
        if (!scene.hapke) return soilMoist;
        const T value = scene.hapke->atT(view, moisture);
        return clampLow ? ad::clamp(value, 0.01, 1.0) : value;
    };
    const auto weightsAt = [&](double localSza, double localView, double relative, bool clampLow, std::array<T, 4>& w,
                               std::array<T, 4>& wf) {
        w = thermalWeightsT<T>(nodes, lai, scene.hotspot, localSza, localView, relative, scene.leafEmissivity, soilAt(localView, clampLow));
        if (scene.reference)
            wf = thermalWeightsT<T>(nodes, lai, scene.hotspot, localSza, localView, relative, scene.leafEmissivity, T(scene.soilFixed));
        else wf = w;
    };
    // Radiance (per wavelength) and effective emissivity of one set of weights; false if a weighted component is missing.
    // `reflectivity` = 1 − Σw of the physical weights (directional-hemispherical reflectance, Kirchhoff).
    const auto accumulate = [&](const std::array<T, 4>& w, const std::array<T, 4>& wf, std::vector<T>& radiance, T& effective,
                                T& reflectivity) {
        radiance.assign(bands, T(0.0));
        effective = T(0.0);
        T emissivity(0.0);
        for (int k = 0; k < 4; ++k) {
            if (!ad::isfinite(temperatures[k])) {
                if (w[k] > 0.0 || wf[k] > 0.0) return false;
                continue;
            }
            if (w[k] > 0.0)
                for (size_t b = 0; b < bands; ++b) radiance[b] += w[k] * planckT<T>(temperatures[k], wavelengths[b]);
            if (wf[k] > 0.0) effective += wf[k];
            emissivity += w[k];
        }
        reflectivity = ad::clamp(T(1.0) - emissivity, 0.0, 1.0);
        return true;
    };

    if (scene.terrain == ThermalTerrain::Flat) {
        std::array<T, 4> w, wf;
        weightsAt(scene.thermalSza, scene.thermalView,
                  std::isfinite(scene.thermalRelative) ? scene.thermalRelative : horizontalRelative,  // FIX(2026-11-21 S-2)
                  false, w, wf);
        std::vector<T> radiance;
        T effective, reflectivity;
        if (!accumulate(w, wf, radiance, effective, reflectivity)) return false;
        effective = ad::clamp(effective, 1e-6, 1.0);
        for (size_t b = 0; b < bands; ++b) {
            if (!(radiance[b] > 0.0)) return false;
            // Brightness: surface-leaving radiance = emitted + reflected sky; LST product: emitted / ε (unchanged).
            brightness[b] = inversePlanckT<T>(radiance[b] + reflectivity * sky[b], wavelengths[b]);
            surface[b] = inversePlanckT<T>(radiance[b] / effective, wavelengths[b]);
        }
        return true;
    }

    // Sub-pixel terrain composite (12 cone facets or 2 flanks + 2 end faces) over a flat background.
    // FIX(2026-11-21 S-1/S-2): enumerated by forEachThermalView (shared with thermalSceneWeights), facet-local azimuth.
    std::vector<T> radiance(bands, T(0.0)), part;
    T effective(0.0), partEffective, reflected(0.0), partReflectivity;
    double area = 0.0;
    const auto add = [&](double localSza, double localView, double relative, double projection) {
        std::array<T, 4> w, wf;
        weightsAt(localSza, localView, relative, true, w, wf);
        if (!accumulate(w, wf, part, partEffective, partReflectivity)) return false;
        for (size_t b = 0; b < bands; ++b) radiance[b] += projection * part[b];
        effective += projection * partEffective;
        reflected += projection * partReflectivity;
        area += projection;
        return true;
    };
    if (!forEachThermalView(scene, [&](double localSza, double localView, double relative, double projection, bool) {
            return add(localSza, localView, relative, projection);
        })) return false;
    if (!(area > 0.0)) return false;
    const T meanEffective = ad::clamp(effective / area, 1e-6, 1.0);
    const T meanReflectivity = reflected / area;
    for (size_t b = 0; b < bands; ++b) {
        if (!(radiance[b] > 0.0)) return false;
        const T mean = radiance[b] / area;
        brightness[b] = inversePlanckT<T>(mean + meanReflectivity * sky[b], wavelengths[b]);
        surface[b] = inversePlanckT<T>(mean / meanEffective, wavelengths[b]);
    }
    return true;
}

double skyTemperatureFromLongwave(double longwave)
{
    constexpr double kSigma = kStefanBoltzmann;
    return std::isfinite(longwave) && longwave > 0.0 ? std::pow(longwave / kSigma, 0.25) : std::nan("");
}

bool thermalSceneWeights(const ThermalScene& scene, double lai, double moisture, std::array<double, 4>& w,
                         std::array<double, 4>& fixed, double& reflectivity)
{
    // FIX(2026-11-21 S-1): the per-view weights of thermalPixel, area-averaged with the same projections, so that
    // sum_k w_k B(T_k) + reflectivity B_sky reproduces the brightness radiance of thermalPixel and sum_k fixed_k its
    // LST normaliser (for finite component temperatures).
    const HomCanopy nodes(lai);
    const double soilMoist = scene.soil ? soilEmissivityAtMoisture(scene.soilFixed, scene.soilWet, moisture) : scene.soilFixed;
    w.fill(0.0);
    fixed.fill(0.0);
    double reflected = 0.0, area = 0.0;
    const bool ok = forEachThermalView(scene, [&](double localSza, double localView, double relative, double projection, bool clampLow) {
        double soil = soilMoist;
        if (scene.hapke) {
            soil = scene.hapke->at(localView, moisture);
            if (clampLow) soil = std::clamp(soil, 0.01, 1.0);
        }
        const auto part = thermalWeights(nodes, lai, scene.hotspot, localSza, localView, relative, scene.leafEmissivity, soil);
        const auto partFixed = scene.reference
            ? thermalWeights(nodes, lai, scene.hotspot, localSza, localView, relative, scene.leafEmissivity, scene.soilFixed) : part;
        double emissivity = 0.0;
        for (int k = 0; k < 4; ++k) {
            w[k] += projection * part[k];
            fixed[k] += projection * partFixed[k];
            emissivity += part[k];
        }
        reflected += projection * std::clamp(1.0 - emissivity, 0.0, 1.0);
        area += projection;
        return true;
    });
    if (!ok || !(area > 0.0)) return false;
    for (int k = 0; k < 4; ++k) { w[k] /= area; fixed[k] /= area; }
    reflectivity = reflected / area;
    return true;
}

double swinbankLongwave(double airTemperatureK)
{
    return std::isfinite(airTemperatureK) && airTemperatureK > 150.0 ? 5.31e-13 * std::pow(airTemperatureK, 6.0) : std::nan("");
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
