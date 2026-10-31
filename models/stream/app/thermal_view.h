#pragma once
// Thermal observation geometry shared by forward imaging (image.cpp) and assimilation (assimilation.cpp).
// Both must see exactly the same component weights, otherwise an analysis of a simulated image would not
// reproduce the state that generated it. All kernels are templates on the scalar type (double or Ad).
#include "config.h"
#include "../scene/classes.h"
#include "../physics/dual.h"
#include "../physics/hapke.h"
#include "../physics/radiosity.h"
#include "../physics/soil_emissivity.h"

#include <algorithm>
#include <array>
#include <map>
#include <vector>

namespace stream {

struct Sun { double zenith = 0, azimuth = 0; bool ok = false; };

// NREL SPA geometric solar position (no refraction), degrees; azimuth clockwise from north.
Sun solarPosition(double unixSeconds, double latitude, double longitude, double elevation);

// Directional soil emissivity at a water content. The Hapke albedo solve is
// anchored to the hemispherical emissivity, so one table is built at the dry
// class value and one at saturation and the two are blended by the same
// moisture weight that scales the level (second-order accurate over the
// <= 0.04 emissivity range between them).
struct MoistureSoilEmissivity {
    SoilEmissivityTable dry, wet;
    void build(double K, double dryEmissivity, double wetEmissivity)
    {
        dry.build(K, std::clamp(dryEmissivity, 1e-6, 1.0));
        wet.build(K, std::clamp(wetEmissivity, 1e-6, 1.0));
    }
    double at(double viewZenithDeg, double moisture) const
    {
        const double weight = soilWetWeight(moisture);
        if (!(weight > 0.0)) return dry.at(viewZenithDeg);
        return dry.at(viewZenithDeg) * (1.0 - weight) + wet.at(viewZenithDeg) * weight;
    }
    template <class T>
    T atT(double viewZenithDeg, const T& moisture) const
    {
        const T weight = soilWetWeightT(moisture);
        if (!(weight > 0.0)) return T(dry.at(viewZenithDeg));
        return dry.at(viewZenithDeg) * (T(1.0) - weight) + wet.at(viewZenithDeg) * weight;
    }
};

// Effective-emissivity weights of [soil sun, soil shade, leaf sun, leaf shade] for one view: TiRT standard
// four-component fractions times the component emissivity, plus the hom3 multiple-scattering contribution.
// The directional radiance of the pixel is sum_k w_k B(T_k); sum_k w_k is its effective emissivity.
std::array<double, 4> thermalWeights(const HomCanopy& nodes, double lai, double hotspot, double sunZenithDeg,
                                     double viewZenithDeg, double relativeAzimuthDeg, double leafEmissivity,
                                     double soilEmissivity);
// Differentiable with respect to LAI and the soil emissivity.
template <class T>
std::array<T, 4> thermalWeightsT(const HomCanopyT<T>& nodes, const T& lai, double hotspot, double sunZenithDeg,
                                 double viewZenithDeg, double relativeAzimuthDeg, double leafEmissivity,
                                 const T& soilEmissivity);

// Thermal view of one pixel: geometry, sub-pixel terrain composite and emissivity settings.
enum class ThermalTerrain { Flat, Hill, Ridge };
struct ThermalScene {
    double sza = 0, saa = 0;                 // horizontal solar geometry, degrees
    double vza = 0, vaa = 0;                 // view geometry, degrees
    double thermalSza = 0, thermalView = 0;  // local geometry (facet slope), = sza / min(vza, 89.9) otherwise
    ThermalTerrain terrain = ThermalTerrain::Flat;
    double slope = 0, coverage = 0, strike = 0, endFraction = 0;  // hill / ridge composite, degrees / 0-1
    double hotspot = 0.2, leafEmissivity = 0.985;
    double soilFixed = 0.955, soilWet = -1.0;  // dry class emissivity, saturated value (-1 = built-in gain)
    bool soil = true;                          // soil ground (moisture-dependent) or flat ground
    const MoistureSoilEmissivity* hapke = nullptr;
    bool reference = true;  // LST product normalises by the fixed class emissivity
};

// Top-of-canopy brightness temperature and surface temperature product for each wavelength. The forward imaging
// runs it with double; the assimilation runs it with Ad (derivatives with respect to the component temperatures,
// LAI and soil moisture). Returns false when the radiance is invalid or a weighted component is missing.
template <class T>
bool thermalPixel(const ThermalScene& scene, const std::array<T, 4>& temperatures, const T& lai, const T& moisture,
                  const std::vector<double>& wavelengths, std::vector<T>& brightness, std::vector<T>& surface);

// Per-pixel terrain fields of the run (null / empty when the mode does not use them).
struct TerrainFields {
    TerrainRadiation mode = TerrainRadiation::None;
    const std::vector<float>* slope = nullptr;  // facet
    const std::vector<float>* aspect = nullptr;
    const std::vector<float>* coneSlope = nullptr;  // hill
    const std::vector<float>* coneDensity = nullptr;
    const std::vector<float>* ridgeSlope = nullptr;  // ridge
    const std::vector<float>* ridgeAspect = nullptr;
    const std::vector<float>* ridgeDensity = nullptr;
    double endFraction = 0.0;
};
// Geometry part of the thermal scene of one pixel and view; false when the facet faces away from the sensor.
bool thermalSceneAt(const TerrainFields& terrain, size_t pixel, double sza, double saa, double vza, double vaa,
                    ThermalScene& scene);
// Material part (hotspot, leaf / soil emissivity, Hapke table, LST convention) from the class and the run config.
void setThermalMaterial(ThermalScene& scene, const ImageConfig& config, const ClassParameters& entry,
                        const std::map<int, MoistureSoilEmissivity>& tables, int code);

}  // namespace stream
