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
constexpr double kSigma = kStefanBoltzmann;  // FIX(2026-11-24 T1): was a separate 5.67e-8
constexpr double kResidualThreshold = 5.0;
constexpr double kInnerThreshold = 1e-4;
// Keep README, PORTING and scene_summary stability descriptions consistent with these tolerances.
constexpr double kNewtonRelaxRobust = 0.8;   // damped Newton for the default "robust" temperature method
constexpr double kInnerResidual = 0.5;       // W m-2, component energy-balance residual for convergence
constexpr double kGustTolerance = 1.0;       // W m-2, scene sensible heat used for the convective gust velocity
constexpr double kStabilityAbsTol = 1e-4;    // m-1, |1/L - target|
constexpr double kStabilityRelTol = 0.02;    //      + 2 % of |target|
constexpr double kStabilityRelax = 0.5;      // 1/L relaxation per iteration
// FIX(2026-11-24): fixed relaxation 0.5 cannot converge a strongly unstable canopy (u* ~ 0.05-0.2 m/s, LAI ~ 2-3: the 1/L target map is
// steeper than -3 and the iteration falls into a period-6 limit cycle, 50 iterations without convergence). The relaxation is therefore
// adaptive: halved (down to kStabilityRelaxMin) when the 1/L error changes sign (overshoot), grown back by 10 % per step while the sign is steady.
// Sweeping the first adaptive iteration (0/5/10/15/20/30) on the Beijing example: starting at iteration 0 left the fewest unconverged pixels.
constexpr double kStabilityRelaxMin = 0.02;
constexpr double kStabilityRelaxShrink = 0.5;
constexpr double kStabilityRelaxGrow = 1.1;
constexpr double kMaxInverseObukhov = 5.0;   // m-1
constexpr double kMaxStep = 5.0;             // K per Newton step
// FIX(2026-11-24 M2): component-temperature limits [kComponentTemperatureMin, kComponentTemperatureMax] (energy.h);
// the lower limit was 240 K, which clamped polar / winter-night pixels at 240 K, left them unconverged with H of
// hundreds of W m-2, and was inconsistent with the 180 K clamp of the Newton denominator. 175 K is below any real
// near-surface temperature yet keeps the Newton iteration inside the validity range of the saturation formulas.
constexpr double kTmin = kComponentTemperatureMin;
constexpr double kTmax = kComponentTemperatureMax;
constexpr double kSparseLai = 0.5;
constexpr double kBracketDamping = 0.25;       // under-relaxation of the temperature and gust updates in the bracketing mode
constexpr int kBracketBudget = 4;            // extra iterations of the bracketing mode, in units of maxIterations

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
    // FIX(2026-11-24 M3): templated on the scalar so that displacement, roughness and heights may carry Ad derivatives.
    template <class T>
    T psim(const T& value) const
    {
        if (length < 0.0) {
            const T x = ad::pow(ad::max(1.0 - 16.0 * value / length, 1e-8), 0.25);
            return 2.0 * ad::log((1.0 + x) / 2.0) + ad::log((1.0 + x * x) / 2.0) - 2.0 * ad::atan(x) + kPi / 2.0;
        }
        if (length > 0.0) return -5.0 * ad::max(value, 0.0) * stableInverse;
        return T(0.0);
    }
    template <class T>
    T psih(const T& value) const
    {
        if (length < 0.0) {
            const T x = ad::pow(ad::max(1.0 - 16.0 * value / length, 1e-8), 0.25);
            return 2.0 * ad::log((1.0 + x * x) / 2.0);
        }
        if (length > 0.0) return -5.0 * ad::max(value, 0.0) * stableInverse;
        return T(0.0);
    }
    template <class T>
    T phstar(const T& value, double reference, const T& displacement) const
    {
        if (length < 0.0) {
            const T x = ad::pow(ad::max(1.0 - 16.0 * value / length, 1e-8), 0.25);
            return (value - displacement) / ad::max(reference - displacement, 1e-9) * ((x * x - 1.0) / (x * x + 1.0));
        }
        if (length > 0.0) return -5.0 * ad::max(value, 0.0) * stableInverse;
        return T(0.0);
    }
};

template <class T>
T canopyWeightT(const T& lai)
{
    const T x = ad::clamp(lai / kSparseLai, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}
double canopyWeight(double lai)
{
    const double x = std::clamp(lai / kSparseLai, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

double bulkResistance(double height, double roughness, double wind, const Similarity& similarity, double heatRoughnessKb)
{
    // FIX(2026-11-21 B4): kB^-1 acts only here, i.e. on the bare-soil bulk path (LAI = 0, and the 1 - smoothstep share
    // for LAI < 0.5), as z0h = 0.1 z0 in TiRT _bulk_resistance; the canopy network has no kB^-1 term (see energy.h).
    height = std::max(height, 1.01);
    roughness = std::clamp(roughness, 1e-4, height / 2.0);
    wind = std::max(wind, 0.05);
    const double heatRoughness = std::max(roughness * std::exp(-heatRoughnessKb), 1e-12);
    const double momentumLog = std::log(height / roughness) - similarity.psim(height) + similarity.psim(roughness);
    const double heatLog = std::log(height / heatRoughness) - similarity.psih(height) + similarity.psih(heatRoughness);
    return std::max(momentumLog * heatLog / (kVonKarman * kVonKarman * wind), 1e-3);
}

double resistanceCap(const Aerodynamics& aero) { return aero.maxResistance > 0.0 ? aero.maxResistance : std::numeric_limits<double>::infinity(); }

template <class T>
T logRatio(const T& argument0)
{
    const T argument = ad::max(argument0, 1e-8);
    if (ad::value(argument) > 50.0) return ad::log1p(-2.0 * ad::exp(-argument));
    const T e = ad::exp(argument);
    return ad::log((e - 1.0) / (e + 1.0));
}

// Aerodynamic resistances. FIX(2026-11-24 M3/A4): templated on the scalar of LAI, so that the assimilation's Ad run
// carries dRa/dLAI (displacement, roughness, u*, rac, leaf boundary layer, canopy weight) into the Jacobian; before, the
// resistances were frozen doubles and LAI only acted through the radiation and the leaf-area scaling of the fluxes.
// 1/L and the convective gust (sceneSensible) stay frozen inputs (README, quasi-Newton note). The double
// instantiation is the forward solve and performs the same arithmetic as before.
template <class T>
struct ResistancesT {
    T leaf{0.0}, soil{0.0};
    double ustar = 0.001, displacement = 0, height = 1, roughness = 0.01, weight = 0;
    bool limited = false;
};
using Resistances = ResistancesT<double>;

template <class T>
ResistancesT<T> aerodynamicResistances(const EnergySurface& surface, const Forcing& forcing, double inverseObukhov,
                                       double density, double sceneSensible, const T& leafArea)
{
    ResistancesT<T> result;
    const T lai = ad::max(leafArea, 0.0);
    const double hc = std::max(surface.height, 0.1);
    const double cap = resistanceCap(surface.aero);
    double wind0 = std::max({forcing.wind, surface.aero.minWindSpeed, 0.01});
    const double buoyancy = std::max(sceneSensible, 0.0);
    const double wstar = buoyancy > 0.0
        ? std::cbrt(kGravity * buoyancy * 1000.0 / (density * kCP * forcing.ta))
        : 0.0;
    wind0 = std::hypot(wind0, wstar);
    const T wind(wind0);
    // DEFERRED[中] 物理: 驱动量是 ERA5 的 10 m 风（u10/v10）和 2 m 气温/露点（t2m/d2m），这里却直接当作高度 z=max(15 m, hc+0.1) 处的值。森林（hc 10–30 m）时 z 远高于 10 m 和 2 m，而 ERA5 诊断量是相对于其自身 d+z0 的；风偏小、气温梯度偏大，导致 ra 偏大、H 符号/量级偏差；建议：用 ERA5 自身粗糙度按对数廓线把 u10、t2m 外推到 z（或把 z 设为 10 m/2 m 并相应计算 rai），至少对 hc>2 m 的地类记日志提示
    const double z = std::max(surface.referenceHeight, hc + 0.1);
    const T weight = canopyWeightT<T>(lai);
    const T profileReference = z + weight * (std::max(z, 2.5 * hc) - z);
    const Similarity similarity = Similarity::make(inverseObukhov, ad::value(profileReference));
    result.limited = inverseObukhov > 0.0 && ad::value(profileReference) * inverseObukhov > 1.0;
    result.weight = ad::value(weight);

    if (!(ad::value(lai) > 0.0)) {
        const double roughness = 0.01;
        result.ustar = std::max(0.001, kVonKarman * wind0 /
            std::max(std::log(z / roughness) - similarity.psim(z) + similarity.psim(roughness), 1e-6));
        result.soil = std::min(bulkResistance(z, roughness, wind0, similarity, surface.ground.heatRoughnessKb), cap);
        result.leaf = 0.0;
        result.height = z;
        result.roughness = roughness;
        result.weight = 0.0;
        return result;
    }

    const T sq = ad::sqrt(ad::max(surface.aero.cd1 * lai / 2.0, 1e-8));
    const T g1 = ad::max(ad::pow(ad::max(surface.aero.cssoil + surface.aero.cr * lai / 2.0, 1e-8), -0.5), 3.3);
    const T n = surface.aero.cd * lai / (2.0 * kVonKarman * kVonKarman);
    const double zr = 2.5 * hc;
    const T displacement = hc * (1.0 - (1.0 - ad::exp(-sq)) / sq);
    const T roughness = ad::max((hc - displacement) * ad::exp(-kVonKarman * g1 + surface.aero.psicor), 1e-4);
    // FIX(2026-11-24 L3): the former `if (hc > z)` branch (SCOPE's "wind at a point below the canopy top") was dead code:
    // z = max(referenceHeight, hc + 0.1) is always above hc. Removed; results unchanged.
    const T ustar = ad::max(kVonKarman * wind /
        ad::max(ad::log(ad::max((z - displacement) / roughness, 1.01)) - similarity.psim(z - displacement) +
                similarity.psim(roughness), 1e-6), 0.001);
    T kh = kVonKarman * ustar * ad::max(zr - displacement, 1e-6);
    if (similarity.length < 0.0)
        kh *= ad::sqrt(ad::max(1.0 - 16.0 * (hc - displacement) / similarity.length, 1e-6));
    else if (similarity.length > 0.0)
        kh /= 1.0 + 5.0 * (hc - displacement) * ad::min(1.0 / similarity.length, 1.0 / profileReference);
    kh = ad::max(kh, 1e-6);
    const T canopyWind = ad::max(ustar / kVonKarman *
        (ad::log(ad::max((hc - displacement) / roughness, 1.01)) - similarity.psim(hc - displacement)), 0.01);
    const T rai = z > zr ? 1.0 / (kVonKarman * ustar) *
        (ad::log(ad::max((z - displacement) / ad::max(zr - displacement, 1e-6), 1.01)) -
         similarity.psih(z - displacement) + similarity.psih(zr - displacement)) : T(0.0);
    // 2026-11-20: the stability terms belong inside 1/(κu*) (they were subtracted from the resistance in s m-1, a
    // unit error inherited from SCOPE resistances.m / TIRTEB). phstar = 0 when neutral, so neutral runs are unchanged.
    // FIX(2026-11-21 B9): duplicated copy of the two comment lines above removed.
    const T rar = 1.0 / (kVonKarman * ustar) * ((zr - hc) / ad::max(zr - displacement, 1e-6) -
                  similarity.phstar(T(zr), zr, displacement) + similarity.phstar(T(hc), zr, displacement));
    const bool nPositive = ad::value(n) > 1e-12;
    const T profile = nPositive ? hc * ad::sinh(n) / (n * kh) : hc / kh;
    const T rac = nPositive ? profile * (logRatio<T>(n) - logRatio<T>(n * (roughness + displacement) / hc)) : T(0.0);
    const T windLeaf = ad::max(canopyWind * ad::exp(n * ((roughness + displacement) / hc - 1.0)), 0.01);
    const T boundary = 70.0 / lai * ad::sqrt(ad::max(surface.leafWidth / windLeaf, 1e-8));
    const T soilProfile = nPositive
        ? profile * (logRatio<T>(n * (roughness + displacement) / hc) - logRatio<T>(n * 0.01 / hc))
        : T(0.0);
    const T above = ad::min(rai + rar + rac, cap);
    result.leaf = ad::max(above + ad::min(boundary, cap), 1e-3);
    result.soil = ad::max(above + ad::min(soilProfile + surface.aero.soilBoundary, cap), 1e-3);
    result.ustar = ad::value(ustar);
    result.displacement = ad::value(displacement);
    result.height = z;
    result.roughness = ad::value(roughness);

    if (ad::value(weight) < 1.0) {
        const double bareUstar = std::max(0.001, kVonKarman * wind0 /
            std::max(std::log(std::max(z / 0.01, 1.01)) - similarity.psim(z) + similarity.psim(0.01), 1e-6));
        const double bareSoil = std::min(bulkResistance(z, 0.01, wind0, similarity, surface.ground.heatRoughnessKb), cap);
        result.ustar = ad::value(weight) * result.ustar + (1.0 - ad::value(weight)) * bareUstar;
        result.soil = weight * result.soil + (1.0 - weight) * bareSoil;
        result.displacement *= ad::value(weight);
        result.roughness = std::exp(ad::value(weight) * std::log(result.roughness) + (1.0 - ad::value(weight)) * std::log(0.01));
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
                      const ResistancesT<T>& aero, double density)
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

    const T raLeaf = ad::max(aero.leaf, 1.0);
    const T raSoil = ad::max(aero.soil, 1.0);
    out.soilResistance = ad::max(soilSurfaceResistanceT<T>(surface, in.soilMoisture), 0.0);
    if (leaves) {
        out.leafResistanceSun = stomatalResistanceT<T>(surface, in.leafSunlit, in.parSun, in.vcmax, forcing, density) / lai;
        out.leafResistanceShade = stomatalResistanceT<T>(surface, in.leafShaded, in.parShade, in.vcmax, forcing, density) / lai;
    }

    const auto component = [&](const T& temperature, const T& ra, const T& rs, const T& incoming, const T& factor,
                               const T& emissivity) {
        ComponentFluxT<T> flux;
        const T resistanceAero = ad::max(ra, 1.0);
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
    const Resistances aero = aerodynamicResistances<double>(surface, forcing, inverseObukhov, density, sceneSensible, surface.lai);
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
    const double t = std::clamp(old, kTmin, kTmax);
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
    const ResistancesT<T> aero = aerodynamicResistances<T>(surface, forcing, inverseObukhov, density, sceneSensible, inputs.lai);
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
                                  double dtSeconds, EnergyState& state, bool bracketFallback)
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
    double relax = kStabilityRelax, previousError = 0.0;  // adaptive 1/L relaxation
    // FIX(2026-11-24 convergence): a node that has not closed after maxIterations (limit cycles of 1/L / temperature, mostly
    // strongly unstable cropland and broadleaf forest) continues in a bracketing mode for kBracketBudget x maxIterations
    // more iterations: temperatures and gust are first closed at a frozen 1/L, then 1/L is moved by expansion + bisection on
    // target(1/L) - 1/L. Nodes that close within maxIterations follow the original path bit for bit.
    bool bracketMode = false;
    double bracketLow = std::numeric_limits<double>::quiet_NaN(), bracketHigh = bracketLow;
    int expansions = 0;
    int limit = std::max(1, surface.maxIterations);
    std::array<double, 6> snapshot{};
    const double buoyancyCoefficient = -kVonKarman * kGravity / (density * kCP * forcing.ta);

    // One joint iteration, at most maxIterations (default 50) in total: each pass evaluates the fluxes, takes a
    // damped Newton step on the four component temperatures, updates the convective-gust heat flux and relaxes
    // 1/L towards its Monin-Obukhov target. Converged when energy, gust and stability residuals are all small.
    // maxStabilityIterations is accepted for compatibility only: temperature and 1/L share maxIterations (README).
    for (int iteration = 0; iteration < limit; ++iteration) {
        fluxes(surface, forcing, shortwave, szaDeg, sunlit, inverse, density, dtSeconds, state.sceneSensible,
               state, solution);
        ++iterations;
        const double ustar = std::max(solution.aero.ustar, 1e-8);
        target = buoyancyCoefficient * solution.sceneSensible / std::pow(ustar, 3.0);
        stabilityClosed = std::abs(inverse - target) <= kStabilityAbsTol + kStabilityRelTol * std::abs(target);
        closed = solution.maxResidual <= kInnerResidual && solution.gustResidual <= kGustTolerance && stabilityClosed;
        if (closed) break;
        const auto& f = solution.fluxes;
        const double previousTemperature[4] = {state.leafSunlit, state.leafShaded, state.soilSunlit, state.soilShaded};
        const double previousGust = state.sceneSensible;
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
        if (bracketMode) {   // under-relaxed temperature and gust update: the frozen-1/L iteration itself may cycle (period ~5)
            double* targets[4] = {&state.leafSunlit, &state.leafShaded, &state.soilSunlit, &state.soilShaded};
            for (int k = 0; k < 4; ++k) *targets[k] = previousTemperature[k] + kBracketDamping * (*targets[k] - previousTemperature[k]);
            state.sceneSensible = previousGust + kBracketDamping * (state.sceneSensible - previousGust);
        }
        const double error = target - inverse;
        if (bracketMode) {
            if (!(solution.maxResidual <= kInnerResidual && solution.gustResidual <= kGustTolerance)) continue;  // close T and gust at frozen 1/L first
            if (error > 0.0) bracketLow = inverse; else bracketHigh = inverse;
            double next;
            if (std::isfinite(bracketLow) && std::isfinite(bracketHigh)) next = 0.5 * (bracketLow + bracketHigh);
            else {
                const double step = std::max(std::abs(error), 0.05) * static_cast<double>(1 << std::min(expansions++, 6));
                next = std::clamp(inverse + (error > 0.0 ? step : -step), -kMaxInverseObukhov, kMaxInverseObukhov);
            }
            if (next == inverse) break;  // pinned at the 1/L limit: no root in range
            inverse = next;
            continue;
        }
        relax = previousError * error < 0.0 ? std::max(kStabilityRelaxMin, relax * kStabilityRelaxShrink) : std::min(kStabilityRelax, relax * kStabilityRelaxGrow);
        previousError = error;
        inverse = std::clamp(inverse + relax * error, -kMaxInverseObukhov, kMaxInverseObukhov);
        if (bracketFallback && iteration + 1 == std::max(1, surface.maxIterations)) {  // original budget exhausted without closure
            bracketMode = true;
            limit = iteration + 1 + kBracketBudget * std::max(1, surface.maxIterations);
            snapshot = {state.leafSunlit, state.leafShaded, state.soilSunlit, state.soilShaded, inverse, state.sceneSensible};  // = the original path's result
        }
    }

    // Final re-evaluation at the returned temperatures and stability.
    double finalError = 0.0;
    const auto finalEvaluation = [&]() {
        fluxes(surface, forcing, shortwave, szaDeg, sunlit, inverse, density, dtSeconds, state.sceneSensible, state, solution);
        const double ustar = std::max(solution.aero.ustar, 1e-8);
        const double finalTarget = buoyancyCoefficient * solution.sceneSensible / std::pow(ustar, 3.0);
        finalError = inverse - finalTarget;
        stabilityClosed = std::abs(finalError) <= 2.0 * (kStabilityAbsTol + kStabilityRelTol * std::abs(finalTarget));
        closed = stabilityClosed && solution.maxResidual <= kResidualThreshold && solution.gustResidual <= 2.0 * kGustTolerance;
    };
    finalEvaluation();
    if (bracketMode && !closed) {
        // The bracketing mode must never make a node worse: compare with the state the plain iteration ended in.
        const std::array<double, 6> found{state.leafSunlit, state.leafShaded, state.soilSunlit, state.soilShaded, inverse, state.sceneSensible};
        const double foundResidual = solution.maxResidual;
        const auto load = [&](const std::array<double, 6>& v) {
            state.leafSunlit = v[0]; state.leafShaded = v[1]; state.soilSunlit = v[2]; state.soilShaded = v[3];
            inverse = v[4]; state.sceneSensible = v[5];
        };
        load(snapshot);
        finalEvaluation();
        if (!closed && !(solution.maxResidual < foundResidual)) {
            load(found);
            finalEvaluation();
        }
    }
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


