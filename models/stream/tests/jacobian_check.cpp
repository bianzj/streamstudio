// Gradient regression test of the assimilation EB Jacobian (review fix M3/A4, 2026-11-24).
// Replicates image.cpp (ebSolver) with the repo's physics sources on minimal synthetic scenes (crop LAI 3 / 1, forest LAI 4,
// C4 grass) and checks that dR/dLAI of the Ad residual (aerodynamic resistances, displacement, roughness, u*, rac, leaf
// boundary layer and canopy weight are differentiated since M3) equals the central finite difference of the *double*
// residual with the resistances recomputed at each LAI (1/L and the convective gust frozen, as in the engine).
// It also prints, for information, the implicit dT/dLAI and dT/dVcmax25 of the Ad Jacobian against the full nonlinear
// finite difference of the solved march (the remaining difference is the frozen 1/L and gust, review item A5).
// Build/run: cmake --build build/stream --config Release --target stream_jacobian_check ; stream_jacobian_check <stream-defined dir>
// Exit code = number of failed checks.
#include "physics/energy.h"
#include "physics/shortwave.h"
#include "physics/constants.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace stream;

static std::vector<float> readColumn(const std::string& file, size_t n)
{
    std::ifstream in(file);
    std::vector<float> v;
    double x;
    while (in >> x) v.push_back(static_cast<float>(x));
    v.resize(n);
    return v;
}

struct Scenario {
    const char* name;
    double hc, leafWidth, vcmax, lai, moisture, ta, ea, p, wind, shortwave, longwave, szaDeg;
    int pathway;
};

static SolarBins g_bins;
static std::vector<float> g_rho, g_tau, g_soil;

static void buildSpectra(const std::string& dataDir, int binNm)
{
    const auto esun = readColumn(dataDir + "/Esun_.dat", 2001);
    const auto esky = readColumn(dataDir + "/Esky_.dat", 2001);
    g_bins = makeSolarBins(esun, esky, binNm);
    std::vector<float> rho(2001), tau(2001), soil(2001);
    for (int i = 0; i < 2001; ++i) {
        const double wl = 400.0 + i;
        rho[i] = wl < 700 ? 0.09f : wl < 1300 ? 0.45f : 0.30f;
        tau[i] = wl < 700 ? 0.05f : wl < 1300 ? 0.45f : 0.25f;
        soil[i] = static_cast<float>(0.10 + 0.20 * (wl - 400.0) / 2000.0);
    }
    g_rho = binSpectrum(g_bins, rho);
    g_tau = binSpectrum(g_bins, tau);
    g_soil = binSpectrum(g_bins, soil);
}

static EnergySurface makeSurface(const Scenario& s, double lai)
{
    EnergySurface surface;
    surface.lai = lai;
    surface.height = s.hc;
    surface.leafWidth = s.leafWidth;
    surface.leafEmissivity = 0.985;
    surface.soilMoisture = s.moisture;
    surface.leaf.pathway = s.pathway;
    surface.leaf.vcmax = static_cast<float>(s.vcmax);
    surface.referenceHeight = 15.0;
    surface.dayOfYear = 196;
    surface.soilHeatMethod = SoilHeatMethod::Surface;
    return surface;
}

struct March {
    std::array<double, 4> temperature{};
    EnergyState before, after;
    Forcing meteo;
    ShortwaveAbsorption shortwave;
    double sza = 0.0;
    bool lit = false;
    EnergyDiagnostics diag;
};

// `nodes` hourly nodes with a prescribed diurnal ramp ending at the overpass node (state starts isothermal, as image.cpp).
static March march(const Scenario& s, double lai, int nodes)
{
    March run;
    const EnergySurface surface = makeSurface(s, lai);
    const OpticalSpectra spectra{&g_rho, &g_tau, &g_soil};
    EnergyState state;
    state.soilSunlit = state.soilShaded = state.leafSunlit = state.leafShaded = s.ta - 4.0;
    state.soilSunState.history = {s.ta - 4.0};
    state.soilShadeState.history = {s.ta - 4.0};
    for (int node = 0; node < nodes; ++node) {
        const bool last = node + 1 == nodes;
        const double frac = nodes > 1 ? static_cast<double>(node) / (nodes - 1) : 1.0;
        Forcing meteo;
        meteo.ta = s.ta - 4.0 * (1.0 - frac);
        meteo.ea = s.ea;
        meteo.p = s.p;
        meteo.wind = s.wind;
        meteo.shortwave = s.shortwave * (0.15 + 0.85 * frac);
        meteo.longwave = s.longwave - 25.0 * (1.0 - frac);
        const double sza = s.szaDeg + 40.0 * (1.0 - frac);
        const bool lit = sza < 90.0;
        const ShortwaveAbsorption sw = absorbShortwave(g_bins, spectra, lai, meteo.shortwave, sza, 196, 0.2, true, lit);
        if (last) {
            run.before = state;
            run.meteo = meteo;
            run.shortwave = sw;
            run.sza = sza;
            run.lit = lit;
        }
        run.diag = solveEnergyNode(surface, meteo, sw, sza, lit, 3600.0, state);
    }
    run.after = state;
    run.temperature = {state.soilSunlit, state.soilShaded, state.leafSunlit, state.leafShaded};
    return run;
}

// Solve A dT = -dR/dx for one column, A = dR/dT (4x4, Gauss-Jordan with partial pivoting, as image.cpp:804-825).
static std::array<double, 4> implicitSolve(const double a_in[4][4], const std::array<double, 4>& drdx)
{
    double a[4][4];
    double b[4];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) a[i][j] = a_in[i][j];
        b[i] = -drdx[i];
    }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
        std::swap(b[c], b[pivot]);
        for (int k = 0; k < 4; ++k) std::swap(a[c][k], a[pivot][k]);
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c] / a[c][c];
            for (int k = 0; k < 4; ++k) a[r][k] -= f * a[c][k];
            b[r] -= f * b[c];
        }
    }
    return {b[0] / a[0][0], b[1] / a[1][1], b[2] / a[2][2], b[3] / a[3][3]};
}

struct EngineGradient {
    double a[4][4]{};
    std::array<double, 4> drdLai{}, drdVcmax{}, drdMoisture{};
};

// image.cpp:731-828 for LAI/Vcmax/moisture seeds (soil moisture path simplified: constant emissivity/inertia/spectrum).
static EngineGradient engineAd(const Scenario& s, double lai, const March& run)
{
    const EnergySurface surface = makeSurface(s, lai);
    const EnergyInputsT<double> base = energyInputs(surface, run.shortwave, run.after);
    EnergyInputsT<Ad> x;
    x.soilSunlit = Ad::seed(base.soilSunlit, 0);
    x.soilShaded = Ad::seed(base.soilShaded, 1);
    x.leafSunlit = Ad::seed(base.leafSunlit, 2);
    x.leafShaded = Ad::seed(base.leafShaded, 3);
    x.soilMoisture = Ad::seed(s.moisture, 4);
    x.vcmax = Ad::seed(base.vcmax, 5);
    x.lai = Ad::seed(lai, 6);
    x.soilEmissivity = Ad(surface.ground.emissivity);
    x.soilInertia = Ad(base.soilInertia);
    std::vector<Ad> soilSpectrum(g_soil.size());
    for (size_t bin = 0; bin < g_soil.size(); ++bin) soilSpectrum[bin] = Ad(g_soil[bin]);
    const auto sw = shortwaveComponents<Ad, std::vector<Ad>>(g_bins, g_rho, g_tau, soilSpectrum, x.lai, run.meteo.shortwave,
                                                             run.shortwave.diffuseFraction, run.sza, run.lit);
    x.soilDirect = sw.soilDirect;
    x.soilDiffuseSun = sw.soilDiffuseSun;
    x.soilDiffuseShade = sw.soilDiffuseShade;
    x.leafDirect = sw.leafDirect;
    x.leafDiffuseSun = sw.leafDiffuseSun;
    x.leafDiffuseShade = sw.leafDiffuseShade;
    x.parSun = sw.parLeafDirect + sw.parLeafDiffuseSun;
    x.parShade = sw.parLeafDiffuseShade;
    x.soilSunlitFraction = sw.soilSunlitFraction;
    x.laiSunlit = sw.laiSunlit;
    const auto residual = energyResiduals<Ad>(surface, x, run.meteo, run.sza, run.lit, 3600.0, run.before,
                                              run.after.inverseObukhov, run.after.sceneSensible);
    EngineGradient g;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) g.a[i][j] = residual[i].d[j];
        g.drdLai[i] = residual[i].d[6];
        g.drdVcmax[i] = residual[i].d[5];
        g.drdMoisture[i] = residual[i].d[4];
    }
    return g;
}

// Double residual of the four components at the converged temperatures for a given LAI.
// `aeroLai` < 0: aerodynamic resistances recomputed with `laiIn`; otherwise frozen at that LAI.
static std::array<double, 4> residualAt(const Scenario& s, double laiIn, double aeroLai, const March& run)
{
    EnergySurface surface = makeSurface(s, aeroLai < 0.0 ? laiIn : aeroLai);
    const OpticalSpectra spectra{&g_rho, &g_tau, &g_soil};
    const ShortwaveAbsorption sw = absorbShortwave(g_bins, spectra, laiIn, run.meteo.shortwave, run.sza, 196, 0.2, true, run.lit);
    EnergyInputsT<double> in = energyInputs(surface, sw, run.after);
    in.lai = laiIn;  // surface.lai (aero) may differ from the LAI of the balances
    return energyResiduals<double>(surface, in, run.meteo, run.sza, run.lit, 3600.0, run.before, run.after.inverseObukhov,
                                   run.after.sceneSensible);
}

static int g_failures = 0;

static void report(const Scenario& s, int nodes)
{
    const double lai = s.lai;
    const March base = march(s, lai, nodes);
    const EngineGradient g = engineAd(s, lai, base);

    const double d = 1e-3 * std::max(lai, 1.0);
    const auto up = residualAt(s, lai + d, -1.0, base), dn = residualAt(s, lai - d, -1.0, base);
    const auto upF = residualAt(s, lai + d, lai, base), dnF = residualAt(s, lai - d, lai, base);
    std::array<double, 4> fdFull{}, fdFrozen{};
    for (int i = 0; i < 4; ++i) {
        fdFull[i] = (up[i] - dn[i]) / (2 * d);
        fdFrozen[i] = (upF[i] - dnF[i]) / (2 * d);
    }
    const auto tAd = implicitSolve(g.a, g.drdLai);
    const auto tFrozen = implicitSolve(g.a, fdFrozen);
    const auto tFull = implicitSolve(g.a, fdFull);

    {   // M3 regression: the Ad column equals the finite difference with the resistances recomputed
        for (int i = 0; i < 4; ++i) {
            const double tolerance = 1e-3 + 2e-4 * std::abs(fdFull[i]);
            if (!(std::abs(g.drdLai[i] - fdFull[i]) <= tolerance)) {
                std::printf("FAIL %s nodes %d component %d: Ad dR/dLAI %.6f vs FD %.6f\n", s.name, nodes, i, g.drdLai[i], fdFull[i]);
                ++g_failures;
            }
            const double tolT = 1e-4 + 2e-3 * std::abs(tFull[i]);
            if (!(std::abs(tAd[i] - tFull[i]) <= tolT)) {
                std::printf("FAIL %s nodes %d component %d: implicit dT/dLAI %.6f vs %.6f\n", s.name, nodes, i, tAd[i], tFull[i]);
                ++g_failures;
            }
        }
    }
    std::printf("\n=== %s: LAI %.2f hc %.1f m, %d node(s), converged=%d, ustar %.3f, 1/L %.4f, H %.1f W/m2 ===\n", s.name, lai, s.hc,
                nodes, base.diag.converged ? 1 : 0, base.diag.ustar, base.after.inverseObukhov, base.diag.sensible);
    std::printf("T (soilSun, soilShade, leafSun, leafShade) = %.2f %.2f %.2f %.2f K\n", base.temperature[0], base.temperature[1],
                base.temperature[2], base.temperature[3]);
    std::printf("dR/dLAI [W m-2 per LAI]  Ad     : %9.3f %9.3f %9.3f %9.3f\n", g.drdLai[0], g.drdLai[1], g.drdLai[2], g.drdLai[3]);
    std::printf("dR/dLAI  FD aero frozen         : %9.3f %9.3f %9.3f %9.3f\n", fdFrozen[0], fdFrozen[1], fdFrozen[2], fdFrozen[3]);
    std::printf("dR/dLAI  FD aero recomputed     : %9.3f %9.3f %9.3f %9.3f\n", fdFull[0], fdFull[1], fdFull[2], fdFull[3]);
    std::printf("implicit dT/dLAI [K per LAI] Ad : %9.4f %9.4f %9.4f %9.4f\n", tAd[0], tAd[1], tAd[2], tAd[3]);
    std::printf("implicit dT/dLAI frozen-FD      : %9.4f %9.4f %9.4f %9.4f\n", tFrozen[0], tFrozen[1], tFrozen[2], tFrozen[3]);
    std::printf("implicit dT/dLAI aero-recomputed: %9.4f %9.4f %9.4f %9.4f\n", tFull[0], tFull[1], tFull[2], tFull[3]);
    for (double step : {0.02, 0.05, 0.1, 0.2}) {
        const double h = step * std::max(lai, 1.0);
        const March p = march(s, lai + h, nodes), m = march(s, lai - h, nodes);
        std::printf("full nonlinear FD (h=%.3f)      : %9.4f %9.4f %9.4f %9.4f  (conv +/-: %d %d)\n", h,
                    (p.temperature[0] - m.temperature[0]) / (2 * h), (p.temperature[1] - m.temperature[1]) / (2 * h),
                    (p.temperature[2] - m.temperature[2]) / (2 * h), (p.temperature[3] - m.temperature[3]) / (2 * h),
                    p.diag.converged ? 1 : 0, m.diag.converged ? 1 : 0);
    }
    {   // Vcmax column: implicit Ad gradient (1/L and gust frozen) vs full nonlinear FD
        const auto tV = implicitSolve(g.a, g.drdVcmax);
        std::printf("implicit dT/dVcmax25 [K per umol] Ad: %9.5f %9.5f %9.5f %9.5f\n", tV[0], tV[1], tV[2], tV[3]);
        Scenario sp = s, sm = s;
        const double hv = 5.0;
        sp.vcmax += hv; sm.vcmax -= hv;
        const March p = march(sp, lai, nodes), m = march(sm, lai, nodes);
        std::printf("full nonlinear FD dT/dVcmax25    : %9.5f %9.5f %9.5f %9.5f\n", (p.temperature[0] - m.temperature[0]) / (2 * hv),
                    (p.temperature[1] - m.temperature[1]) / (2 * hv), (p.temperature[2] - m.temperature[2]) / (2 * hv),
                    (p.temperature[3] - m.temperature[3]) / (2 * hv));
    }
}

int main(int argc, char** argv)
{
    const std::string dataDir = argc > 1 ? argv[1] : "stream-defined";
    buildSpectra(dataDir, 50);
    // name, hc, leafWidth, vcmax, lai, moisture, ta, ea, p, wind, shortwave, longwave, sza, pathway
    const Scenario crop{"crop C3 midday", 1.0, 0.05, 60, 3.0, 0.2, 300.0, 15.0, 1000.0, 2.5, 800.0, 350.0, 30.0, 3};
    const Scenario cropLow{"crop C3 midday sparse", 1.0, 0.05, 60, 1.0, 0.2, 300.0, 15.0, 1000.0, 2.5, 800.0, 350.0, 30.0, 3};
    const Scenario forest{"forest C3 midday", 20.0, 0.05, 50, 4.0, 0.25, 298.0, 18.0, 1000.0, 3.0, 750.0, 360.0, 30.0, 3};
    const Scenario grass{"grass C4 windy", 0.5, 0.01, 40, 2.0, 0.2, 303.0, 20.0, 1000.0, 1.0, 850.0, 380.0, 25.0, 4};
    for (int nodes : {1, 7}) {
        report(crop, nodes);
        report(cropLow, nodes);
        report(forest, nodes);
        report(grass, nodes);
    }
    std::printf("\n%d failed gradient checks\n", g_failures);
    return g_failures;
}

