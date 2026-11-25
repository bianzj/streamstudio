// ==========================================================================
// 文件：thermal_view.cpp
// 功能：热红外观测算子的实现（声明见 thermal_view.h）。正向成像、同化和 LST/LSCT 反演共用，
//       保证三者使用相同的四组分权重与反射天空项
// 主要模块：
//   1. solarPosition：NREL SPA 几何太阳位置（不含大气折射）
//   2. thermalWeightsT：四组分有效发射率权重（TiRT 比例 × 发射率 + hom3 多次散射），
//      夜间（太阳在地平线以下）把光照组分并入阴影组分
//   3. thermalSceneAt / setThermalMaterial：按地形模式与地类构造 ThermalScene 的几何与材料部分
//   4. localRelativeAzimuth：斜坡面局地坐标系下的太阳—观测相对方位角
//   5. forEachThermalView：枚举像元内子视角（平地/单坡/面元一个；hill/ridge 为多个面元 + 平地背景）
//   6. thermalPixel：合成亮温与发射率归一化表面温度（double 与 Ad 两种标量）
//   7. thermalSceneWeights：同一算子的线性形式（面积平均权重），供反演使用
//   8. 天空温度换算：skyTemperatureFromLongwave、swinbankLongwave；显式模板实例化
// 数据流：config/类参数 → ThermalScene → 权重 w_k → 辐亮度 Σ w_k B(T_k) + (1 − Σw) B(T_sky)
//         → 逆 Planck 得亮温；表面温度（LST 产品）为发射辐亮度 / 有效发射率，不含反射天空
// 约定：角度为度；温度 K；组分顺序 [土壤光照, 土壤阴影, 叶片光照, 叶片阴影]；
//       方位角自北起顺时针；Σw 为方向有效发射率，1 − Σw 为方向—半球反射率（Kirchhoff）
// ==========================================================================

#include "thermal_view.h"
#include "../forcing/utc.h"
#include "../physics/brightness.h"
#include "../physics/canopy.h"
#include "../physics/constants.h"
#include "../physics/spa.h"

#include <cmath>

namespace stream {

// 模块：太阳位置。输入 Unix 秒（取整到秒）、纬度/经度（度）、海拔（m，非有限时取 0）；
// 输出天顶角与方位角（度，方位自北起顺时针）；SPA 计算失败时 ok=false。
// 说明：气压置 0 表示不做大气折射修正，得到几何位置；delta_t 取固定值 69 s。
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

// 模块：四组分有效发射率权重 w = [土壤光照, 土壤阴影, 叶片光照, 叶片阴影]（模板：double 或 Ad）。
// 有叶（lai > 0）时 = 四组分比例 × 发射率 + hom3 热红外多次散射贡献，无叶时只有土壤两项；
// 各项下限 0；Σw 为方向有效发射率。可对 LAI 与土壤发射率求导（同化用）。
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
    // 步骤：太阳在（局地）地平线以下时无光照组分，把光照权重并入对应阴影组分，Σw 不变。
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

// 模块：构造 ThermalScene 的几何部分。flat：局地几何 = 水平几何（视天顶角上限 89.9°）；
// single/facet：按坡度/坡向求局地太阳与观测天顶角及局地相对方位角；
// hill/ridge：只记录复合参数（坡度、覆盖度、走向、端面比例），由 forEachThermalView 展开。
// 返回 false 表示面元背向传感器（cos 观测角 ≤ 0），该像元不可见。
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
        terrain.slope && !terrain.slope->empty() && terrain.aspect && !terrain.aspect->empty()) {   // 坡度与坡向都要有（缺坡向时按水平面处理，不解引用空指针）
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

// 模块：构造 ThermalScene 的材料部分：热点参数、叶片发射率 = 1 − reflTir − transTir（限幅 0.01–1）、
// 土壤干端发射率（限幅 0.01–1）、饱和端值、是否土壤地面、Hapke 表（仅土壤且配置启用时）、
// LST 归一化约定。叶片发射率取地类值而非 EB 宽带的 leafEmissivity（见 README）。
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

// 模块：斜坡面局地相对方位角（度，0–180）。利用散射角的旋转不变性
// cos(dphi') = (s·v − cos(sza')cos(vza')) / (sin(sza')sin(vza'))；任一局地方向接近面元法线时
// 方位角无定义，退回水平的 vaa − saa（对核函数无影响）。
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

// 匿名命名空间：枚举子视角的内部工具。
namespace {

// 模块：子视角枚举 forEachThermalView（thermalPixel 与 thermalSceneWeights 共用，保证二者一致）。
// 投影权重 projection：平地/单坡/面元 = 1；hill/ridge = 覆盖度 × 面元权重 × cos(局地观测角)，
// 平地背景 = (1 − 覆盖度) × cos(观测角)。hill：12 个锥面方位（每 30°），各 1/12；
// ridge：两侧坡面（走向 ± 90°）合权重 1 − endFraction，两个竖直端面（β = 90°）合权重 endFraction。
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

// 模块：单像元热红外观测算子。输入四组分温度（K）、LAI、土壤含水量、波长（μm）；
// 输出各波长的亮温（含反射天空项）与发射率归一化表面温度（不含反射天空）。
// 返回 false：辐亮度非正，或权重大于 0 的组分温度缺失。
template <class T>
bool thermalPixel(const ThermalScene& scene, const std::array<T, 4>& temperatures, const T& lai, const T& moisture,
                  const std::vector<double>& wavelengths, std::vector<T>& brightness, std::vector<T>& surface)
{
    const size_t bands = wavelengths.size();
    brightness.assign(bands, T(std::nan("")));
    surface.assign(bands, T(std::nan("")));
    // 步骤 1：预先计算各波段的各向同性天空辐亮度，并准备土壤发射率、权重计算的局部函数。
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

    // 步骤 2：平地/单坡/面元只有一个视角：直接合成辐亮度。亮温 = 逆 Planck(发射 + 反射天空)，
    //         表面温度 = 逆 Planck(发射 / 有效发射率)，有效发射率按 LST 约定下限取 1e-6。
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

    // 步骤 3：hill/ridge 复合地形：对各子视角的辐亮度、有效发射率、反射率按投影面积加权平均，
    //         再做逆 Planck。（子视角上 Hapke 土壤发射率下限取 0.01。）
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

// 作用：宽带下行长波 L↓ (W m-2) → 等效黑体天空温度 (L↓/σ)^(1/4) (K)；L↓ 非正或非有限时返回 NaN。
// 说明：宽带折算会略高估大气窗区天空辐亮度（见 README“热红外观测算子”）。
double skyTemperatureFromLongwave(double longwave)
{
    constexpr double kSigma = kStefanBoltzmann;
    return std::isfinite(longwave) && longwave > 0.0 ? std::pow(longwave / kSigma, 0.25) : std::nan("");
}

// 模块：thermalPixel 的线性形式。输出面积平均的四组分权重 w、固定地类土壤发射率下的权重 fixed
// （Σfixed 为 LST 产品归一化因子）和平均方向反射率 reflectivity（反射天空 = reflectivity × B(T_sky)）。
// 没有可见子视角（面积为 0）时返回 false。
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

// 作用：Swinbank 晴空下行长波 5.31e-13 · Ta^6 (W m-2)；Ta ≤ 150 K 或非有限时返回 NaN。
double swinbankLongwave(double airTemperatureK)
{
    return std::isfinite(airTemperatureK) && airTemperatureK > 150.0 ? 5.31e-13 * std::pow(airTemperatureK, 6.0) : std::nan("");
}

// 显式实例化：double（成像、反演）与 Ad（同化自动微分）。
template std::array<double, 4> thermalWeightsT<double>(const HomCanopyT<double>&, const double&, double, double, double,
                                                       double, double, const double&);
template std::array<Ad, 4> thermalWeightsT<Ad>(const HomCanopyT<Ad>&, const Ad&, double, double, double, double, double,
                                               const Ad&);
template bool thermalPixel<double>(const ThermalScene&, const std::array<double, 4>&, const double&, const double&,
                                   const std::vector<double>&, std::vector<double>&, std::vector<double>&);
template bool thermalPixel<Ad>(const ThermalScene&, const std::array<Ad, 4>&, const Ad&, const Ad&,
                               const std::vector<double>&, std::vector<Ad>&, std::vector<Ad>&);

}  // namespace stream
