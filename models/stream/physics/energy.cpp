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
template <class T>
T smallerRoot(double a, const T& b, const T& c)
{
    if (a == 0.0) return -c / b;
    return (-b - ad::sqrt(ad::max(b * b - 4.0 * a * c, 0.0))) / (2.0 * a);
}

template <class T>
T saturationVapourPressureT(const T& celsius)
{
    return 6.107 * ad::pow(10.0, 7.5 * celsius / (237.3 + celsius));
}

template <class T>
T latentHeatT(const T& kelvin) { return (2.501 - 0.002361 * (kelvin - 273.15)) * 1.0e6; }

// Farquhar / Ball-Berry leaf resistance [s m-1] per unit leaf area, with the g0 conductance floor.
// Templated on the leaf temperature, PAR and Vcmax25 (the EB control of the assimilation).
template <class T>
T stomatalResistanceT(const EnergySurface& surface, const T& leafTemperature, const T& par, const T& vcmax25,
                      const Forcing& forcing, double density)
{
    if (surface.leaf.pathway != 3 && surface.leaf.pathway != 4) return T(625000.0);
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
    const T kpep25 = (vcmax25 / 56.0) * 1e6;
    const T kd = ad::max(0.8738, 0.0301 * (leafTemperature - 273.15) + 0.0773);
    const T qt = 0.1 * (leafTemperature - tref) * tcor;
    const T th = 1.0 + tcor * ad::exp(shti * (leafTemperature - thh));
    const T tl = 1.0 + tcor * ad::exp(slti * (thl - leafTemperature));
    const T kc = kc25 * ad::exp(std::log(2.1) * qt);
    const T ko = ko25 * ad::exp(std::log(1.2) * qt);
    const T kpep = kpep25 * ad::exp(std::log(1.8) * qt);
    const double stress = surface.leaf.stress;
    T vcmax, mm, vsC3;
    double minCi;
    if (!c4) {
        vcmax = vcmax25 * ad::exp(std::log(2.1) * qt) / th * stress;
        mm = kc * (1.0 + oxygen / ko);
        vsC3 = vcmax25 / 2.0 * ad::exp(std::log(1.8) * qt);
        minCi = 0.3;
    } else {
        vcmax = vcmax25 * ad::exp(std::log(2.1) * qt) / (th * tl) * stress;
        mm = T(0.0);
        vsC3 = T(0.0);
        minCi = 0.1;
    }
    const T specificity = 2600.0 * ad::exp(std::log(0.75) * qt);
    const T rd = surface.leaf.respiration * vcmax25 * ad::exp(std::log(1.8) * qt) /
                 (1.0 + ad::exp(1.3 * (leafTemperature - trdm)));
    const T po0 = 4.0 / (0.05 + kd + 4.0);
    const T je = 0.5 * po0 * par;
    const T gamma = 0.5 * oxygen / specificity;
    const T rh = ad::min(T(1.0), forcing.ea / ad::max(saturationVapourPressureT(leafTemperature - 273.15), 1e-6));
    const double ppm2bar = 1e-6 * (p * 1e-3);
    const double cs = ca * ppm2bar;
    const T ci = ad::max(T(minCi * cs), cs * (1.0 - 1.6 / (surface.leaf.ballBerrySlope * rh)));
    const T vc = c4 ? vcmax : vcmax * (ci - gamma) / (mm + ci);
    const T vs = c4 ? kpep * ci : vsC3;
    const T ve = c4 ? je * effcon : je * (ci - gamma) / (ci + 2.0 * gamma) * effcon;
    const T v = smallerRoot<T>(0.8, -(vc + ve), vc * ve);
    const T ag = smallerRoot<T>(0.98, -(v + vs), v * vs);
    const T assimilation = ag - rd;
    T gs = 1.6 * assimilation * ppm2bar / ad::max(cs - ci, 1e-12);
    gs = ad::max(ad::isfinite(gs) ? gs : T(0.0), surface.stomatalG0);
    return (density / (kMAir * 1e-3)) / gs;
}

template <class T>
T soilSurfaceResistanceT(const EnergySurface& surface, const T& moisture)
{
    if (!surface.ground.moistureResistance) return T(std::max(surface.ground.fixedResistance, 0.0));
    const double saturation = std::max(surface.ground.saturatedMoisture, 1e-6);
    const T wetness = ad::clamp(moisture / saturation, 0.0, 1.0);
    return ad::exp(8.206 - 4.255 * wetness);
}

template <class T> struct ComponentFluxT { T net{}, sensible{}, latent{}, ground{}, residual{}; };
using ComponentFlux = ComponentFluxT<double>;
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

template <class T>
struct BalancesT {
    ComponentFluxT<T> soilSunlit, soilShaded, leafSunlit, leafShaded;
    T soilResistance{}, leafResistanceSun = T(625000.0), leafResistanceShade = T(625000.0);
};

// The four component balances at fixed aerodynamic resistances (one implementation for the forward solve and
// for the implicit derivatives of the assimilation).
template <class T>
BalancesT<T> balances(const EnergySurface& surface, const EnergyInputsT<T>& in, const Forcing& forcing,
                      double szaDeg, bool sunlit, double dtSeconds, const EnergyState& thermal,
                      const Resistances& aero, double density)
{
    BalancesT<T> out;
    const T lai = ad::max(in.lai, 0.0);
    const double eToQ = kMH2O / kMAir / std::max(forcing.p, 1e-6);
    const double qa = forcing.ea * eToQ;
    const double ta = forcing.ta;
    const bool leaves = lai > 0.0;

    LongwaveAbsorptionT<T> longwave;
    if (leaves) {
        const HomCanopyT<T> nodes(lai);
        longwave = homLongwave<T>(nodes, T(surface.leafEmissivity), in.soilEmissivity, forcing.longwave,
                                  in.soilSunlit, in.soilShaded, in.leafSunlit, in.leafShaded, szaDeg, sunlit);
    } else {
        longwave.soilSun = longwave.soilShade = in.soilEmissivity * forcing.longwave;
    }
    const T soilIncomingSun = in.soilDirect + in.soilDiffuseSun + longwave.soilSun;
    const T soilIncomingShade = in.soilDiffuseShade + longwave.soilShade;
    const T leafIncomingSun = in.leafDirect + in.leafDiffuseSun + longwave.leafSun;
    const T leafIncomingShade = in.leafDiffuseShade + longwave.leafShade;

    const double raLeaf = std::max(aero.leaf, 1.0);
    const double raSoil = std::max(aero.soil, 1.0);
    out.soilResistance = ad::max(soilSurfaceResistanceT<T>(surface, in.soilMoisture), 0.0);
    if (leaves) {
        out.leafResistanceSun = stomatalResistanceT<T>(surface, in.leafSunlit, in.parSun, in.vcmax, forcing, density) / lai;
        out.leafResistanceShade = stomatalResistanceT<T>(surface, in.leafShaded, in.parShade, in.vcmax, forcing, density) / lai;
    }

    const auto component = [&](const T& temperature, double ra, const T& rs, const T& incoming, const T& factor,
                               const T& emissivity) {
        ComponentFluxT<T> flux;
        const double resistanceAero = std::max(ra, 1.0);
        const T resistanceSurface = ad::max(rs, 0.0);
        const T qi = saturationVapourPressureT(temperature - 273.15) * eToQ;
        const T lambda = latentHeatT(temperature);
        flux.latent = density / (resistanceAero + resistanceSurface) * lambda * (qi - qa);
        flux.sensible = density * kCP / resistanceAero * (temperature - ta);
        flux.net = incoming - emissivity * kSigma * ad::pow(temperature, 4.0) * factor;
        flux.residual = flux.net - flux.latent - flux.sensible;
        return flux;
    };

    if (leaves) {
        out.leafSunlit = component(in.leafSunlit, raLeaf, out.leafResistanceSun, lai * leafIncomingSun, 2.0 * lai,
                                   T(surface.leafEmissivity));
        out.leafShaded = component(in.leafShaded, raLeaf, out.leafResistanceShade, lai * leafIncomingShade, 2.0 * lai,
                                   T(surface.leafEmissivity));
    }
    out.soilSunlit = component(in.soilSunlit, raSoil, out.soilResistance, soilIncomingSun, T(1.0), in.soilEmissivity);
    out.soilShaded = component(in.soilShaded, raSoil, out.soilResistance, soilIncomingShade, T(1.0), in.soilEmissivity);

    const ThermalMaterial material = surface.ground.material();
    const auto groundFlux = [&](const T& temperature, const T& net, const SoilThermalState& state) -> T {
        switch (surface.soilHeatMethod) {
            case SoilHeatMethod::Direct: return net * surface.soilHeatFraction;
            case SoilHeatMethod::Surface: return surfaceHeatFluxT<T>(temperature, state, dtSeconds, in.soilInertia);
            case SoilHeatMethod::Depth: default:
                // The implicit column is linear in the surface temperature: G = intercept + slope T.
                depthHeatFlux(ad::value(temperature), state, dtSeconds, material);
                if (!state.depthCached) return T(0.0);
                return state.depthIntercept + state.depthSlope * temperature;
        }
    };
    out.soilSunlit.ground = groundFlux(in.soilSunlit, out.soilSunlit.net, thermal.soilSunState);
    out.soilShaded.ground = groundFlux(in.soilShaded, out.soilShaded.net, thermal.soilShadeState);
    out.soilSunlit.residual -= out.soilSunlit.ground;
    out.soilShaded.residual -= out.soilShaded.ground;
    return out;
}

void fluxes(const EnergySurface& surface, const Forcing& forcing, const ShortwaveAbsorption& shortwave,
            double szaDeg, bool sunlit, double inverseObukhov, double density, double dtSeconds, double sceneSensible,
            EnergyState& state, Solution& solution)
{
    const double lai = std::max(surface.lai, 0.0);
    const bool leaves = lai > 0.0;
    const EnergyInputsT<double> inputs = energyInputs(surface, shortwave, state);
    const Resistances aero = aerodynamicResistances(surface, forcing, inverseObukhov, density, sceneSensible);
    solution.aero = aero;
    solution.density = density;
    const BalancesT<double> b = balances<double>(surface, inputs, forcing, szaDeg, sunlit, dtSeconds, state, aero, density);
    solution.soilResistance = b.soilResistance;
    solution.leafResistanceSun = b.leafResistanceSun;
    solution.leafResistanceShade = b.leafResistanceShade;
    Fluxes result{b.soilSunlit, b.soilShaded, b.leafSunlit, b.leafShaded};

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
    double maximum;
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

EnergyInputsT<double> energyInputs(const EnergySurface& surface, const ShortwaveAbsorption& shortwave,
                                   const EnergyState& state)
{
    EnergyInputsT<double> in;
    in.soilSunlit = state.soilSunlit;
    in.soilShaded = state.soilShaded;
    in.leafSunlit = state.leafSunlit;
    in.leafShaded = state.leafShaded;
    in.lai = surface.lai;
    in.soilMoisture = surface.soilMoisture;
    in.vcmax = surface.leaf.vcmax;
    in.soilEmissivity = surface.ground.emissivity;
    in.soilInertia = surface.ground.material().inertia();
    in.soilDirect = shortwave.soilDirect;
    in.soilDiffuseSun = shortwave.soilDiffuseSun;
    in.soilDiffuseShade = shortwave.soilDiffuseShade;
    in.leafDirect = shortwave.leafDirect;
    in.leafDiffuseSun = shortwave.leafDiffuseSun;
    in.leafDiffuseShade = shortwave.leafDiffuseShade;
    in.parSun = shortwave.parLeafDirect + shortwave.parLeafDiffuseSun;
    in.parShade = shortwave.parLeafDiffuseShade;
    in.soilSunlitFraction = shortwave.soilSunlitFraction;
    in.laiSunlit = shortwave.laiSunlit;
    return in;
}

template <class T>
std::array<T, 4> energyResiduals(const EnergySurface& surface, const EnergyInputsT<T>& inputs, const Forcing& forcing,
                                 double szaDeg, bool sunlit, double dtSeconds, const EnergyState& history,
                                 double inverseObukhov, double sceneSensible)
{
    const double density = airDensity(forcing);
    const Resistances aero = aerodynamicResistances(surface, forcing, inverseObukhov, density, sceneSensible);
    const BalancesT<T> b = balances<T>(surface, inputs, forcing, szaDeg, sunlit, dtSeconds, history, aero, density);
    const bool leaves = ad::value(inputs.lai) > 0.0;
    return {b.soilSunlit.residual, b.soilShaded.residual,
            leaves ? b.leafSunlit.residual : T(0.0), leaves ? b.leafShaded.residual : T(0.0)};
}

template <class T>
void johansenSoilThermal(double saturatedMoisture, const T& moisture, T& conductivity, T& volumetricCapacity)
{
    // Johansen (coarse soil): dry sand ~0.2 W m-1 K-1 / inertia ~600, wet soil ~1.4 / ~1900.
    const double porosity = std::clamp(saturatedMoisture, 0.2, 0.7);
    const T theta = ad::clamp(moisture, 0.0, porosity);
    const double bulk = (1.0 - porosity) * 2700.0;
    const double dry = (0.135 * bulk + 64.7) / (2700.0 - 0.947 * bulk);
    const double saturated = std::pow(3.0, 1.0 - porosity) * std::pow(0.57, porosity);
    const T saturation = theta / porosity;
    const T kersten = saturation > 0.05 ? ad::clamp(0.7 * ad::log10(saturation) + 1.0, 0.0, 1.0) : T(0.0);
    conductivity = dry + kersten * (saturated - dry);
    volumetricCapacity = (1.0 - porosity) * 2.0e6 + theta * 4.18e6;
}

template std::array<double, 4> energyResiduals<double>(const EnergySurface&, const EnergyInputsT<double>&,
                                                       const Forcing&, double, bool, double, const EnergyState&,
                                                       double, double);
template std::array<Ad, 4> energyResiduals<Ad>(const EnergySurface&, const EnergyInputsT<Ad>&, const Forcing&, double,
                                               bool, double, const EnergyState&, double, double);
template void johansenSoilThermal<double>(double, const double&, double&, double&);
template void johansenSoilThermal<Ad>(double, const Ad&, Ad&, Ad&);

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


