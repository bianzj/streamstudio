#include "energy.h"
#include "constants.h"
#include "radiosity.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace stream {

double saturationVapourPressure(double celsius)
{
    return 6.107 * std::pow(10.0, 7.5 * celsius / (237.3 + celsius));
}

namespace {

constexpr double kCP = 1004.0;
constexpr double kRd = 287.05;
constexpr double kMAir = 28.96;
constexpr double kMH2O = 18.0;
constexpr double kSigma = 5.67e-8;
constexpr double kResidualThreshold = 5.0;
constexpr double kInnerThreshold = 1e-4;
// DEFERRED[低] 可维护性: 实现与文档不一致：PORTING/README 写的是 robust=WC 0.08 残差修正、单步 ±1.5 K、1/L 割线 + 符号区间（容差 1e-7+1e-6|target|），代码实际是 0.8 阻尼牛顿、单步 kMaxStep=5 K、1/L 按 0.5 松弛（容差 1e-4+2%|target|）；建议：统一文档与 scene_summary 元数据，或恢复文档所述算法
constexpr double kNewtonRelaxRobust = 0.8;   // damped Newton for the default "robust" temperature method
constexpr double kInnerResidual = 0.5;       // W m-2, component energy-balance residual for convergence
constexpr double kGustTolerance = 1.0;       // W m-2, scene sensible heat used for the convective gust velocity
constexpr double kStabilityAbsTol = 1e-4;    // m-1, |1/L - target|
constexpr double kStabilityRelTol = 0.02;    //      + 2 % of |target|
constexpr double kStabilityRelax = 0.5;      // 1/L relaxation per iteration
constexpr double kMaxInverseObukhov = 5.0;   // m-1
constexpr double kMaxStep = 5.0;             // K per Newton step
// DEFERRED[中] 物理: 组分温度硬下限 240 K（−33 °C）。高纬/青藏高原冬季夜间、冰雪地表的真实表面温度可低于 240 K，此时 updateTemperature 会被钳在 240 K，残差不收敛，亮温系统性偏高；建议：下限放宽到约 190–200 K（成像侧过滤是 100 K），或按 ERA5 skt 的最小值动态设定
constexpr double kTmin = 240.0;
constexpr double kTmax = 380.0;
constexpr double kSparseLai = 0.5;

double saturationSlope(double celsius)
{
    const double es = saturationVapourPressure(celsius);
    return es * 2.3026 * 7.5 * 237.3 / std::pow(237.3 + celsius, 2.0);
}

double latentHeat(double kelvin) { return (2.501 - 0.002361 * (kelvin - 273.15)) * 1.0e6; }

double airDensity(const Forcing& forcing)
{
    const double pressure = std::max(forcing.p, 1e-6) * 100.0;
    const double vapour = std::max(forcing.ea, 0.0) * 100.0;
    const double virtualTemperature = forcing.ta / (1.0 - 0.378 * vapour / pressure);
    return pressure / (kRd * virtualTemperature);
}

struct Similarity {
    double length = std::numeric_limits<double>::infinity();
    double stableInverse = 0.0;

    static Similarity make(double inverseObukhov, double referenceHeight)
    {
        Similarity result;
        result.length = inverseObukhov != 0.0 ? 1.0 / inverseObukhov : std::numeric_limits<double>::infinity();
        if (result.length > 0.0) result.stableInverse = std::min(1.0 / result.length, 1.0 / std::max(referenceHeight, 1e-6));
        return result;
    }
    double psim(double value) const
    {
        if (length < 0.0) {
            const double x = std::pow(std::max(1.0 - 16.0 * value / length, 1e-8), 0.25);
            return 2.0 * std::log((1.0 + x) / 2.0) + std::log((1.0 + x * x) / 2.0) - 2.0 * std::atan(x) + kPi / 2.0;
        }
        if (length > 0.0) return -5.0 * std::max(value, 0.0) * stableInverse;
        return 0.0;
    }
    double psih(double value) const
    {
        if (length < 0.0) {
            const double x = std::pow(std::max(1.0 - 16.0 * value / length, 1e-8), 0.25);
            return 2.0 * std::log((1.0 + x * x) / 2.0);
        }
        if (length > 0.0) return -5.0 * std::max(value, 0.0) * stableInverse;
        return 0.0;
    }
    double phstar(double value, double reference, double displacement) const
    {
        if (length < 0.0) {
            const double x = std::pow(std::max(1.0 - 16.0 * value / length, 1e-8), 0.25);
            return (value - displacement) / std::max(reference - displacement, 1e-9) * ((x * x - 1.0) / (x * x + 1.0));
        }
        if (length > 0.0) return -5.0 * std::max(value, 0.0) * stableInverse;
        return 0.0;
    }
};

double canopyWeight(double lai)
{
    const double x = std::clamp(lai / kSparseLai, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

double bulkResistance(double height, double roughness, double wind, const Similarity& similarity, double heatRoughnessKb)
{
    height = std::max(height, 1.01);
    roughness = std::clamp(roughness, 1e-4, height / 2.0);
    wind = std::max(wind, 0.05);
    const double heatRoughness = std::max(roughness * std::exp(-heatRoughnessKb), 1e-12);
    const double momentumLog = std::log(height / roughness) - similarity.psim(height) + similarity.psim(roughness);
    const double heatLog = std::log(height / heatRoughness) - similarity.psih(height) + similarity.psih(heatRoughness);
    return std::max(momentumLog * heatLog / (kVonKarman * kVonKarman * wind), 1e-3);
}

double resistanceCap(const Aerodynamics& aero) { return aero.maxResistance > 0.0 ? aero.maxResistance : std::numeric_limits<double>::infinity(); }

double logRatio(double argument)
{
    argument = std::max(argument, 1e-8);
    if (argument > 50.0) return std::log1p(-2.0 * std::exp(-argument));
    const double e = std::exp(argument);
    return std::log((e - 1.0) / (e + 1.0));
}

struct Resistances {
    double leaf = 0, soil = 0, ustar = 0.001, displacement = 0, height = 1, roughness = 0.01, weight = 0;
    bool limited = false;
};

Resistances aerodynamicResistances(const EnergySurface& surface, const Forcing& forcing, double inverseObukhov,
                                   double density, double sceneSensible)
{
    Resistances result;
    const double lai = std::max(surface.lai, 0.0);
    const double hc = std::max(surface.height, 0.1);
    const double cap = resistanceCap(surface.aero);
    double wind = std::max({forcing.wind, surface.aero.minWindSpeed, 0.01});
    const double buoyancy = std::max(sceneSensible, 0.0);
    const double wstar = buoyancy > 0.0
        ? std::cbrt(kGravity * buoyancy * 1000.0 / (density * kCP * forcing.ta))
        : 0.0;
    wind = std::hypot(wind, wstar);
    // DEFERRED[中] 物理: 驱动量是 ERA5 的 10 m 风（u10/v10）和 2 m 气温/露点（t2m/d2m），这里却直接当作高度 z=max(15 m, hc+0.1) 处的值。森林（hc 10–30 m）时 z 远高于 10 m 和 2 m，而 ERA5 诊断量是相对于其自身 d+z0 的；风偏小、气温梯度偏大，导致 ra 偏大、H 符号/量级偏差；建议：用 ERA5 自身粗糙度按对数廓线把 u10、t2m 外推到 z（或把 z 设为 10 m/2 m 并相应计算 rai），至少对 hc>2 m 的地类记日志提示
    double z = std::max(surface.referenceHeight, hc + 0.1);
    result.weight = canopyWeight(lai);
    const double profileReference = z + result.weight * (std::max(z, 2.5 * hc) - z);
    const Similarity similarity = Similarity::make(inverseObukhov, profileReference);
    result.limited = inverseObukhov > 0.0 && profileReference * inverseObukhov > 1.0;

    if (lai <= 0.0) {
        const double roughness = 0.01;
        result.ustar = std::max(0.001, kVonKarman * wind /
            std::max(std::log(z / roughness) - similarity.psim(z) + similarity.psim(roughness), 1e-6));
        result.soil = std::min(bulkResistance(z, roughness, wind, similarity, surface.ground.heatRoughnessKb), cap);
        result.leaf = 0.0;
        result.height = z;
        result.roughness = roughness;
        result.weight = 0.0;
        return result;
    }

    const double sq = std::sqrt(std::max(surface.aero.cd1 * lai / 2.0, 1e-8));
    const double g1 = std::max(3.3, std::pow(std::max(surface.aero.cssoil + surface.aero.cr * lai / 2.0, 1e-8), -0.5));
    const double n = surface.aero.cd * lai / (2.0 * kVonKarman * kVonKarman);
    const double zr = 2.5 * hc;
    double displacement = hc * (1.0 - (1.0 - std::exp(-sq)) / sq);
    double roughness = std::max((hc - displacement) * std::exp(-kVonKarman * g1 + surface.aero.psicor), 1e-4);
    if (hc > z) {
        const double ustarTemp = std::max(0.001, kVonKarman * wind /
            std::max(std::log(std::max((z - displacement) / roughness, 1.01)) - similarity.psim(z - displacement), 1e-6));
        wind = std::max(0.01, ustarTemp / kVonKarman *
            (std::log(std::max((hc - displacement) / roughness, 1.01)) - similarity.psim(hc - displacement)));
    }
    const double ustar = std::max(0.001, kVonKarman * wind /
        std::max(std::log(std::max((z - displacement) / roughness, 1.01)) - similarity.psim(z - displacement) +
                 similarity.psim(roughness), 1e-6));
    double kh = kVonKarman * ustar * std::max(zr - displacement, 1e-6);
    if (similarity.length < 0.0)
        kh *= std::sqrt(std::max(1.0 - 16.0 * (hc - displacement) / similarity.length, 1e-6));
    else if (similarity.length > 0.0)
        kh /= 1.0 + 5.0 * (hc - displacement) * std::min(1.0 / similarity.length, 1.0 / profileReference);
    kh = std::max(kh, 1e-6);
    const double canopyWind = std::max(ustar / kVonKarman *
        (std::log(std::max((hc - displacement) / roughness, 1.01)) - similarity.psim(hc - displacement)), 0.01);
    const double rai = z > zr ? 1.0 / (kVonKarman * ustar) *
        (std::log(std::max((z - displacement) / std::max(zr - displacement, 1e-6), 1.01)) -
         similarity.psih(z - displacement) + similarity.psih(zr - displacement)) : 0.0;
    const double rar = 1.0 / (kVonKarman * ustar) * ((zr - hc) / std::max(zr - displacement, 1e-6)) -
                       similarity.phstar(zr, zr, displacement) + similarity.phstar(hc, zr, displacement);
    const double profile = n > 1e-12 ? hc * std::sinh(n) / (n * kh) : hc / kh;
    const double rac = n > 1e-12 ? profile * (logRatio(n) - logRatio(n * (roughness + displacement) / hc)) : 0.0;
    const double windLeaf = std::max(canopyWind * std::exp(n * ((roughness + displacement) / hc - 1.0)), 0.01);
    const double boundary = 70.0 / lai * std::sqrt(std::max(surface.leafWidth / windLeaf, 1e-8));
    const double soilProfile = n > 1e-12
        ? profile * (logRatio(n * (roughness + displacement) / hc) - logRatio(n * 0.01 / hc))
        : 0.0;
    const double above = std::min(rai + rar + rac, cap);
    result.leaf = std::max(above + std::min(boundary, cap), 1e-3);
    result.soil = std::max(above + std::min(soilProfile + surface.aero.soilBoundary, cap), 1e-3);
    result.ustar = ustar;
    result.displacement = displacement;
    result.height = z;
    result.roughness = roughness;

    if (result.weight < 1.0) {
        const double bareUstar = std::max(0.001, kVonKarman * wind /
            std::max(std::log(std::max(z / 0.01, 1.01)) - similarity.psim(z) + similarity.psim(0.01), 1e-6));
        const double bareSoil = std::min(bulkResistance(z, 0.01, wind, similarity, surface.ground.heatRoughnessKb), cap);
        const double weight = result.weight;
        result.ustar = weight * result.ustar + (1.0 - weight) * bareUstar;
        result.soil = weight * result.soil + (1.0 - weight) * bareSoil;
        result.displacement *= weight;
        result.roughness = std::exp(weight * std::log(result.roughness) + (1.0 - weight) * std::log(0.01));
    }
    return result;
}

// Smaller root of a x^2 + b x + c.
double smallerRoot(double a, double b, double c)
{
    if (a == 0.0) return -c / b;
    return (-b - std::sqrt(std::max(b * b - 4.0 * a * c, 0.0))) / (2.0 * a);
}

// Farquhar / Ball-Berry leaf resistance [s m-1] per unit leaf area, with the g0 conductance floor.
double stomatalResistance(const EnergySurface& surface, double leafTemperature, double par, const Forcing& forcing,
                          double density)
{
    if (surface.leaf.pathway != 3 && surface.leaf.pathway != 4) return 625000.0;
    const bool c4 = surface.leaf.pathway == 4;
    const double effcon = c4 ? 1.0 / 6.0 : 1.0 / 5.0;
    const double slti = surface.leaf.temperature[0], shti = surface.leaf.temperature[1];
    const double thl = surface.leaf.temperature[2], thh = surface.leaf.temperature[3], trdm = surface.leaf.temperature[4];
    const double tcor = surface.leaf.temperatureCorrection ? 1.0 : 0.0;
    const double tref = 25.0 + 273.15;
    const double kc25 = 350.0e-6, ko25 = 450.0e-3;
    const double p = std::max(forcing.p, 1e-6);
    const double ca = surface.co2;
    const double oxygen = c4 ? 0.0 : (surface.o2 * 1e-3) * (p * 1e-3);
    const double kpep25 = (surface.leaf.vcmax / 56.0) * 1e6;
    const double kd = std::max(0.8738, 0.0301 * (leafTemperature - 273.15) + 0.0773);
    const double qt = 0.1 * (leafTemperature - tref) * tcor;
    const double th = 1.0 + tcor * std::exp(shti * (leafTemperature - thh));
    const double tl = 1.0 + tcor * std::exp(slti * (thl - leafTemperature));
    const double kc = kc25 * std::exp(std::log(2.1) * qt);
    const double ko = ko25 * std::exp(std::log(1.2) * qt);
    const double kpep = kpep25 * std::exp(std::log(1.8) * qt);
    const double stress = surface.leaf.stress;
    double vcmax, mm, vsC3;
    double minCi;
    if (!c4) {
        vcmax = surface.leaf.vcmax * std::exp(std::log(2.1) * qt) / th * stress;
        mm = kc * (1.0 + oxygen / ko);
        vsC3 = surface.leaf.vcmax / 2.0 * std::exp(std::log(1.8) * qt);
        minCi = 0.3;
    } else {
        vcmax = surface.leaf.vcmax * std::exp(std::log(2.1) * qt) / (th * tl) * stress;
        mm = 0.0;
        vsC3 = 0.0;
        minCi = 0.1;
    }
    const double specificity = 2600.0 * std::exp(std::log(0.75) * qt);
    const double rd = surface.leaf.respiration * surface.leaf.vcmax * std::exp(std::log(1.8) * qt) /
                      (1.0 + std::exp(1.3 * (leafTemperature - trdm)));
    const double po0 = 4.0 / (0.05 + kd + 4.0);
    const double je = 0.5 * po0 * par;
    const double gamma = 0.5 * oxygen / specificity;
    const double rh = std::min(1.0, forcing.ea / std::max(saturationVapourPressure(leafTemperature - 273.15), 1e-6));
    const double ppm2bar = 1e-6 * (p * 1e-3);
    const double cs = ca * ppm2bar;
    const double ci = std::max(minCi * cs, cs * (1.0 - 1.6 / (surface.leaf.ballBerrySlope * rh)));
    const double vc = c4 ? vcmax : vcmax * (ci - gamma) / (mm + ci);
    const double vs = c4 ? kpep * ci : vsC3;
    const double ve = c4 ? je * effcon : je * (ci - gamma) / (ci + 2.0 * gamma) * effcon;
    const double v = smallerRoot(0.8, -(vc + ve), vc * ve);
    const double ag = smallerRoot(0.98, -(v + vs), v * vs);
    const double assimilation = ag - rd;
    double gs = 1.6 * assimilation * ppm2bar / std::max(cs - ci, 1e-12);
    gs = std::max(std::isfinite(gs) ? gs : 0.0, surface.stomatalG0);
    return (density / (kMAir * 1e-3)) / gs;
}

double soilSurfaceResistance(const EnergySurface& surface)
{
    if (!surface.ground.moistureResistance) return std::max(surface.ground.fixedResistance, 0.0);
    const double saturation = std::max(surface.ground.saturatedMoisture, 1e-6);
    const double wetness = std::clamp(surface.soilMoisture / saturation, 0.0, 1.0);
    return std::exp(8.206 - 4.255 * wetness);
}

struct ComponentFlux { double net = 0, sensible = 0, latent = 0, ground = 0, residual = 0; };
struct Fluxes { ComponentFlux soilSunlit, soilShaded, leafSunlit, leafShaded; };

struct Solution {
    Fluxes fluxes;
    Resistances aero;
    double soilResistance = 0, leafResistanceSun = 625000, leafResistanceShade = 625000;
    double sceneSensible = 0, gustResidual = 1e9, density = 1.2;
    double soilSunlitFraction = 0, laiSunlit = 0;
    double maxResidual = 1e9;
    bool closed = false;
};

void fluxes(const EnergySurface& surface, const Forcing& forcing, const ShortwaveAbsorption& shortwave,
            double szaDeg, bool sunlit, double inverseObukhov, double density, double dtSeconds, double sceneSensible,
            EnergyState& state, Solution& solution)
{
    const double lai = std::max(surface.lai, 0.0);
    const double eToQ = kMH2O / kMAir / std::max(forcing.p, 1e-6);
    const double qa = forcing.ea * eToQ;
    const double ta = forcing.ta;
    const bool leaves = lai > 0.0;

    HomCanopy nodes(lai);
    LongwaveAbsorption longwave;
    if (leaves) {
        longwave = homLongwave(nodes, surface.leafEmissivity, surface.ground.emissivity, forcing.longwave,
                               state.soilSunlit, state.soilShaded, state.leafSunlit, state.leafShaded,
                               szaDeg, sunlit);
    } else {
        longwave.soilSun = longwave.soilShade = surface.ground.emissivity * forcing.longwave;
    }
    const double soilIncomingSun = shortwave.soilDirect + shortwave.soilDiffuseSun + longwave.soilSun;
    const double soilIncomingShade = shortwave.soilDiffuseShade + longwave.soilShade;
    const double leafIncomingSun = shortwave.leafDirect + shortwave.leafDiffuseSun + longwave.leafSun;
    const double leafIncomingShade = shortwave.leafDiffuseShade + longwave.leafShade;

    const Resistances aero = aerodynamicResistances(surface, forcing, inverseObukhov, density, sceneSensible);
    const double raLeaf = std::max(aero.leaf, 1.0);
    const double raSoil = std::max(aero.soil, 1.0);
    const double rss = std::max(soilSurfaceResistance(surface), 0.0);
    solution.aero = aero;
    solution.soilResistance = rss;
    solution.density = density;

    double leafResistanceSun = 625000.0, leafResistanceShade = 625000.0;
    if (leaves) {
        const double parSun = shortwave.parLeafDirect + shortwave.parLeafDiffuseSun;
        const double parShade = shortwave.parLeafDiffuseShade;
        leafResistanceSun = stomatalResistance(surface, state.leafSunlit, parSun, forcing, density) / lai;
        leafResistanceShade = stomatalResistance(surface, state.leafShaded, parShade, forcing, density) / lai;
    }
    solution.leafResistanceSun = leafResistanceSun;
    solution.leafResistanceShade = leafResistanceShade;

    const auto component = [&](double temperature, double ra, double rs, double incoming, double factor, double emissivity) {
        ComponentFlux flux;
        const double resistanceAero = std::max(ra, 1.0);
        const double resistanceSurface = std::max(rs, 0.0);
        const double qi = saturationVapourPressure(temperature - 273.15) * eToQ;
        const double lambda = latentHeat(temperature);
        flux.latent = density / (resistanceAero + resistanceSurface) * lambda * (qi - qa);
        flux.sensible = density * kCP / resistanceAero * (temperature - ta);
        flux.net = incoming - emissivity * kSigma * std::pow(temperature, 4.0) * factor;
        flux.residual = flux.net - flux.latent - flux.sensible;
        return flux;
    };

    Fluxes result;
    if (leaves) {
        result.leafSunlit = component(state.leafSunlit, raLeaf, leafResistanceSun, lai * leafIncomingSun, 2.0 * lai,
                                      surface.leafEmissivity);
        result.leafShaded = component(state.leafShaded, raLeaf, leafResistanceShade, lai * leafIncomingShade, 2.0 * lai,
                                      surface.leafEmissivity);
    }
    result.soilSunlit = component(state.soilSunlit, raSoil, rss, soilIncomingSun, 1.0, surface.ground.emissivity);
    result.soilShaded = component(state.soilShaded, raSoil, rss, soilIncomingShade, 1.0, surface.ground.emissivity);

    const ThermalMaterial material = surface.ground.material();
    const auto groundFlux = [&](double temperature, double net, SoilThermalState& thermal) {
        switch (surface.soilHeatMethod) {
            case SoilHeatMethod::Direct: return net * surface.soilHeatFraction;
            case SoilHeatMethod::Surface: return surfaceHeatFlux(temperature, thermal, dtSeconds, material.inertia());
            case SoilHeatMethod::Depth: default: return depthHeatFlux(temperature, thermal, dtSeconds, material);
        }
    };
    result.soilSunlit.ground = groundFlux(state.soilSunlit, result.soilSunlit.net, state.soilSunState);
    result.soilShaded.ground = groundFlux(state.soilShaded, result.soilShaded.net, state.soilShadeState);
    result.soilSunlit.residual -= result.soilSunlit.ground;
    result.soilShaded.residual -= result.soilShaded.ground;

    const double soilSunFraction = std::clamp(shortwave.soilSunlitFraction, 0.0, 1.0);
    const double laiSunlit = std::clamp(shortwave.laiSunlit, 0.0, lai);
    const double laiShade = lai - laiSunlit;
    solution.soilSunlitFraction = soilSunFraction;
    solution.laiSunlit = laiSunlit;
    const auto leafWeightSun = lai > 0.0 ? laiSunlit / lai : 0.0;
    const auto leafWeightShade = lai > 0.0 ? laiShade / lai : 0.0;
    const double sceneH = soilSunFraction * result.soilSunlit.sensible +
                          (1.0 - soilSunFraction) * result.soilShaded.sensible +
                          leafWeightSun * result.leafSunlit.sensible + leafWeightShade * result.leafShaded.sensible;
    solution.gustResidual = std::abs(sceneH - sceneSensible);
    solution.sceneSensible = sceneH;
    solution.fluxes = result;
    double maximum = std::abs(result.soilSunlit.residual) + std::abs(result.soilShaded.residual);
    if (leaves) maximum = std::max({std::abs(result.soilSunlit.residual), std::abs(result.soilShaded.residual),
                                    std::abs(result.leafSunlit.residual), std::abs(result.leafShaded.residual)});
    else maximum = std::max(std::abs(result.soilSunlit.residual), std::abs(result.soilShaded.residual));
    solution.maxResidual = maximum;
}

// Newton step on one component balance R(T) = Rn - H - LE - G with the full derivative
// -dR/dT = rho cp / ra + rho lambda dq*/dT / (ra + rs) + 4 n eps sigma T^3 + dG/dT.
// (The former "robust" step dropped the latent and soil-heat terms and was scaled by 0.08, so it needed
// ~170 steps for the inner tolerance while only 50 were allowed: no pixel ever converged.)
// "robust" damps the step (0.8); every step is limited to kMaxStep and to [kTmin, kTmax].
void updateTemperature(double& temperature, double residual, double ra, double rs, double groundDerivative,
                       double emissivity, double faces, bool robust, double density, const Forcing& forcing)
{
    const double old = temperature;
    ra = std::max(ra, 1.0);
    rs = std::max(rs, 0.0);
    const double t = std::clamp(old, 180.0, 380.0);
    const double denominator = density * kCP / ra +
                               density * latentHeat(t) * (kMH2O / kMAir / std::max(forcing.p, 1e-6)) *
                                   saturationSlope(t - 273.15) / (ra + rs) +
                               4.0 * faces * emissivity * kSigma * std::pow(t, 3.0) + groundDerivative;
    if (!std::isfinite(residual) || !std::isfinite(denominator) || denominator <= 1e-6) return;
    const double raw = (robust ? kNewtonRelaxRobust : 1.0) * residual / denominator;
    if (!std::isfinite(raw)) return;
    temperature = std::clamp(old + std::clamp(raw, -kMaxStep, kMaxStep), kTmin, kTmax);
}

}  // namespace

EnergyDiagnostics solveEnergyNode(const EnergySurface& surface, const Forcing& forcing,
                                  const ShortwaveAbsorption& shortwave, double szaDeg, bool sunlit,
                                  double dtSeconds, EnergyState& state)
{
    EnergyDiagnostics diagnostics;
    const double lai = std::max(surface.lai, 0.0);
    const bool leaves = lai > 0.0;
    const double density = airDensity(forcing);
    const ThermalMaterial material = surface.ground.material();
    const double dt = std::max(dtSeconds, 1.0);
    // dG/dT of the selected soil heat method, so the Newton step sees the same G(T) as the residual.
    double groundSlope = 2.0 * material.inertia() / std::sqrt(kPi * dt);          // surface (semi-infinite)
    if (surface.soilHeatMethod == SoilHeatMethod::Depth) {
        depthHeatFlux(state.soilSunlit, state.soilSunState, dtSeconds, material);  // fills the linear G(T) cache
        groundSlope = state.soilSunState.depthCached ? state.soilSunState.depthSlope : 0.0;
    }
    const auto groundDerivative = [&](double temperature) {
        if (surface.soilHeatMethod != SoilHeatMethod::Direct) return groundSlope;
        // G = f Rn  ->  dG/dT = -f 4 eps sigma T^3 (G falls as the surface emits more)
        return -surface.soilHeatFraction * 4.0 * surface.ground.emissivity * kSigma * std::pow(temperature, 3.0);
    };
    Solution solution;
    double inverse = std::isfinite(state.inverseObukhov) ? state.inverseObukhov : 0.0;
    double target = 0.0;
    bool closed = false, stabilityClosed = false;
    int iterations = 0;
    const double buoyancyCoefficient = -kVonKarman * kGravity / (density * kCP * forcing.ta);

    // One joint iteration, at most maxIterations (default 50) in total: each pass evaluates the fluxes, takes a
    // damped Newton step on the four component temperatures, updates the convective-gust heat flux and relaxes
    // 1/L towards its Monin-Obukhov target. Converged when energy, gust and stability residuals are all small.
    // DEFERRED[低] 可维护性: surface.maxStabilityIterations（配置 maxStabilityIterations，README 称"外层迭代上限 60"）在本求解器中从未使用，温度与 1/L 共用 maxIterations；用户调这个参数没有任何效果；建议：删除该配置项，或恢复内外两层迭代
    for (int iteration = 0; iteration < std::max(1, surface.maxIterations); ++iteration) {
        fluxes(surface, forcing, shortwave, szaDeg, sunlit, inverse, density, dtSeconds, state.sceneSensible,
               state, solution);
        ++iterations;
        const double ustar = std::max(solution.aero.ustar, 1e-8);
        target = buoyancyCoefficient * solution.sceneSensible / std::pow(ustar, 3.0);
        stabilityClosed = std::abs(inverse - target) <= kStabilityAbsTol + kStabilityRelTol * std::abs(target);
        closed = solution.maxResidual <= kInnerResidual && solution.gustResidual <= kGustTolerance && stabilityClosed;
        if (closed) break;
        const auto& f = solution.fluxes;
        if (leaves) {
            updateTemperature(state.leafSunlit, f.leafSunlit.residual, solution.aero.leaf, solution.leafResistanceSun,
                              0.0, surface.leafEmissivity, 2.0 * lai, surface.robustTemperature, density, forcing);
            updateTemperature(state.leafShaded, f.leafShaded.residual, solution.aero.leaf, solution.leafResistanceShade,
                              0.0, surface.leafEmissivity, 2.0 * lai, surface.robustTemperature, density, forcing);
        }
        updateTemperature(state.soilSunlit, f.soilSunlit.residual, solution.aero.soil, solution.soilResistance,
                          groundDerivative(state.soilSunlit), surface.ground.emissivity, 1.0, surface.robustTemperature,
                          density, forcing);
        updateTemperature(state.soilShaded, f.soilShaded.residual, solution.aero.soil, solution.soilResistance,
                          groundDerivative(state.soilShaded), surface.ground.emissivity, 1.0, surface.robustTemperature,
                          density, forcing);
        if (leaves) {
            if (!std::isfinite(state.leafSunlit)) state.leafSunlit = forcing.ta + 2.0;
            if (!std::isfinite(state.leafShaded)) state.leafShaded = forcing.ta + 1.0;
        }
        if (!std::isfinite(state.soilSunlit)) state.soilSunlit = forcing.ta + 2.0;
        if (!std::isfinite(state.soilShaded)) state.soilShaded = forcing.ta + 1.0;
        state.sceneSensible = solution.sceneSensible;
        inverse = std::clamp(inverse + kStabilityRelax * (target - inverse), -kMaxInverseObukhov, kMaxInverseObukhov);
    }

    // Final re-evaluation at the returned temperatures and stability.
    fluxes(surface, forcing, shortwave, szaDeg, sunlit, inverse, density, dtSeconds, state.sceneSensible, state, solution);
    const double ustar = std::max(solution.aero.ustar, 1e-8);
    const double finalTarget = buoyancyCoefficient * solution.sceneSensible / std::pow(ustar, 3.0);
    const double finalError = inverse - finalTarget;
    stabilityClosed = std::abs(finalError) <= 2.0 * (kStabilityAbsTol + kStabilityRelTol * std::abs(finalTarget));
    closed = stabilityClosed && solution.maxResidual <= kResidualThreshold && solution.gustResidual <= 2.0 * kGustTolerance;
    const int stabilityIterations = iterations;
    state.inverseObukhov = inverse;

    advanceThermalState(state.soilSunlit, state.soilSunState, dtSeconds, surface.ground.material(),
                        surface.soilHeatMethod == SoilHeatMethod::Depth);
    advanceThermalState(state.soilShaded, state.soilShadeState, dtSeconds, surface.ground.material(),
                        surface.soilHeatMethod == SoilHeatMethod::Depth);

    const double soilSunFraction = solution.soilSunlitFraction;
    const double laiSunlit = solution.laiSunlit;
    const double laiShade = lai - laiSunlit;
    const auto leafSunWeight = lai > 0.0 ? laiSunlit / lai : 0.0;
    const auto leafShadeWeight = lai > 0.0 ? laiShade / lai : 0.0;
    const auto& f = solution.fluxes;
    diagnostics.netRadiation = soilSunFraction * f.soilSunlit.net + (1.0 - soilSunFraction) * f.soilShaded.net +
                               leafSunWeight * f.leafSunlit.net + leafShadeWeight * f.leafShaded.net;
    diagnostics.sensible = solution.sceneSensible;
    diagnostics.latent = soilSunFraction * f.soilSunlit.latent + (1.0 - soilSunFraction) * f.soilShaded.latent +
                         leafSunWeight * f.leafSunlit.latent + leafShadeWeight * f.leafShaded.latent;
    diagnostics.ground = soilSunFraction * f.soilSunlit.ground + (1.0 - soilSunFraction) * f.soilShaded.ground;
    diagnostics.residual = solution.maxResidual;
    diagnostics.stabilityResidual = std::abs(finalError);
    diagnostics.gustResidual = solution.gustResidual;
    diagnostics.soilSunlitFraction = soilSunFraction;
    diagnostics.laiSunlit = laiSunlit;
    diagnostics.airDensity = density;
    diagnostics.ustar = solution.aero.ustar;
    diagnostics.displacement = solution.aero.displacement;
    diagnostics.roughness = solution.aero.roughness;
    diagnostics.canopyWeight = solution.aero.weight;
    diagnostics.obukhovLength = inverse != 0.0 ? 1.0 / inverse : std::numeric_limits<double>::infinity();
    diagnostics.iterations = iterations;
    diagnostics.stabilityIterations = stabilityIterations;
    diagnostics.converged = closed;
    diagnostics.stabilityConverged = stabilityClosed;
    diagnostics.stabilityLimited = solution.aero.limited;
    diagnostics.fallbackUsed = false;
    state.sceneSensible = solution.sceneSensible;
    return diagnostics;
}

}  // namespace stream


