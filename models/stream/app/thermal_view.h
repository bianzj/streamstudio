// ==========================================================================
// 文件：thermal_view.h
// 功能：热红外观测几何与观测算子的声明，供正向成像（image.cpp）、同化（assimilation.cpp）
//       和反演（inversion.cpp）共用，保证三者使用完全相同的四组分权重
// 主要模块：
//   1. 太阳位置（solarPosition，NREL SPA）
//   2. 随含水量变化的土壤方向发射率（MoistureSoilEmissivity，Hapke 表干/湿两端混合）
//   3. 四组分有效发射率权重（thermalWeights / thermalWeightsT，后者可对 LAI、土壤发射率求导）
//   4. ThermalScene：单像元的热红外视角（几何、地形复合、发射率设置、反射天空）
//   5. 天空等效温度（skyTemperatureFromLongwave、swinbankLongwave）
//   6. thermalPixel：亮温与发射率归一化表面温度；thermalSceneWeights：线性形式的权重
//   7. 地形：局地相对方位角、TerrainFields、thermalSceneAt、setThermalMaterial
// 数据流：config + 类参数 → setThermalMaterial / thermalSceneAt 填 ThermalScene
//         → thermalPixel（成像、同化）或 thermalSceneWeights（反演）→ 亮温/表面温度/权重
// 约定：角度单位为度；温度 K；长波 W m-2；组分顺序 [土壤光照, 土壤阴影, 叶片光照, 叶片阴影]
//       方位角自北起顺时针；实现在 thermal_view.cpp，模板核按标量类型（double 或 Ad）实例化
// ==========================================================================

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
#include <limits>
#include <map>
#include <vector>

namespace stream {

// 太阳位置：zenith/azimuth 为度，ok 表示计算有效。
struct Sun { double zenith = 0, azimuth = 0; bool ok = false; };

// NREL SPA geometric solar position (no refraction), degrees; azimuth clockwise from north.
Sun solarPosition(double unixSeconds, double latitude, double longitude, double elevation);

// 模块：随含水量变化的土壤方向发射率（干端与饱和端两张 Hapke 表按含水量权重混合）。
// atT 为模板版本，可对含水量求导（同化用）。
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

// 模块：四组分有效发射率权重 w_k（TiRT 比例 × 发射率 + hom3 多次散射）。
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

// 模块：ThermalScene——单像元、单视角的热红外场景参数。
// 字段单位：角度为度；slope/strike 为度，coverage/endFraction 为 0-1；skyTemperature 为 K。
// Thermal view of one pixel: geometry, sub-pixel terrain composite and emissivity settings.
enum class ThermalTerrain { Flat, Hill, Ridge };
struct ThermalScene {
    double sza = 0, saa = 0;                 // horizontal solar geometry, degrees
    double vza = 0, vaa = 0;                 // view geometry, degrees
    double thermalSza = 0, thermalView = 0;  // local geometry (facet slope), = sza / min(vza, 89.9) otherwise
    // FIX(2026-11-21 S-2): sun-view relative azimuth in the local (slope-plane) frame of a single-slope / facet
    // pixel, degrees in [0, 180]; NaN = horizontal vaa - saa (flat pixels and the flat part of a composite).
    double thermalRelative = std::numeric_limits<double>::quiet_NaN();
    ThermalTerrain terrain = ThermalTerrain::Flat;
    double slope = 0, coverage = 0, strike = 0, endFraction = 0;  // hill / ridge composite, degrees / 0-1
    double hotspot = 0.2, leafEmissivity = 0.985;
    double soilFixed = 0.955, soilWet = -1.0;  // dry class emissivity, saturated value (-1 = built-in gain)
    bool soil = true;                          // soil ground (moisture-dependent) or flat ground
    const MoistureSoilEmissivity* hapke = nullptr;
    bool reference = true;  // LST product normalises by the fixed class emissivity
    // Reflected sky (2026-11-20): equivalent blackbody temperature of the isotropic downwelling sky,
    // T_sky = (L↓/σ)^(1/4), K. The brightness temperature adds (1 − Σw)·B(T_sky, λ), Σw = directional effective
    // emissivity (Kirchhoff). NaN = no reflected sky (legacy). The LST product stays emitted radiance / ε.
    double skyTemperature = std::numeric_limits<double>::quiet_NaN();
};

// 模块：反射天空项的来源换算（宽带下行长波 → 等效黑体天空温度；RT 缺省的 Swinbank 估计）。
// Equivalent sky temperature of a broadband downwelling longwave flux [W m-2]; NaN when the flux is not positive.
double skyTemperatureFromLongwave(double longwave);
// Clear-sky downwelling longwave from the air temperature (Swinbank 1963, L↓ = 5.31e-13 Ta^6 W m-2); the RT
// default when no L↓ is configured (the shaded component temperature stands in for the air temperature).
double swinbankLongwave(double airTemperatureK);

// 模块：单像元热红外观测算子（正向成像用 double，同化用 Ad 自动微分）。
// Top-of-canopy brightness temperature and surface temperature product for each wavelength. The forward imaging
// runs it with double; the assimilation runs it with Ad (derivatives with respect to the component temperatures,
// LAI and soil moisture). Returns false when the radiance is invalid or a weighted component is missing.
template <class T>
bool thermalPixel(const ThermalScene& scene, const std::array<T, 4>& temperatures, const T& lai, const T& moisture,
                  const std::vector<double>& wavelengths, std::vector<T>& brightness, std::vector<T>& surface);

// FIX(2026-11-21 S-1): the linear form of thermalPixel for one scene: area-mean effective weights `w` (radiance =
// sum_k w_k B(T_k)), the weights `fixed` with the fixed class soil emissivity (LST-product normaliser = sum fixed)
// and the mean directional reflectivity (reflected sky = reflectivity * B(T_sky)). Same sub-pixel enumeration
// (flat / single slope / facet / hill / ridge) and the same local geometry as thermalPixel, so the LST / LSCT
// retrieval uses the imaging operator also with terrain. False when no sub-view is visible.
bool thermalSceneWeights(const ThermalScene& scene, double lai, double moisture, std::array<double, 4>& w,
                         std::array<double, 4>& fixed, double& reflectivity);

// FIX(2026-11-21 S-2): relative azimuth (degrees, [0, 180]) between the sun and the view direction measured in the
// plane of a tilted facet whose local zenith angles are localSza / localVza (TiRT theory §5.2: global directions are
// transformed to the slope-local frame first). Uses the rotation invariance of the scattering angle:
// cos(dphi') = (s.v - cos(sza') cos(vza')) / (sin(sza') sin(vza')). Falls back to the horizontal value when either
// local direction is (near) the facet normal, where the azimuth is undefined and does not affect the kernels.
double localRelativeAzimuth(double szaDeg, double saaDeg, double vzaDeg, double vaaDeg, double localSzaDeg,
                            double localVzaDeg);

// 模块：地形字段与场景构造（面元 facet 用 slope/aspect，hill 用 coneSlope/coneDensity，
// ridge 用 ridgeSlope/ridgeAspect/ridgeDensity；不使用的模式为空指针）。
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
