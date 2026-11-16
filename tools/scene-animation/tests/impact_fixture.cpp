// Table-based tests for animation::impact (blast, penetration, empirical formulas).
// Prints one PASS/FAIL line per check (plus INFO lines) and returns non-zero if any check fails.
#include "animation/impact/blast.h"
#include "animation/impact/empirical.h"
#include "animation/impact/penetration.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace animation::impact;
using BP = BlastParameters;

namespace {
int checks = 0, failures = 0;
constexpr double kPi = 3.14159265358979323846;
const BurstType kAir = BurstType::FreeAirSpherical, kSurf = BurstType::SurfaceHemispherical;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
}
std::string fmt(const char* f, double a, double b = 0, double c = 0, double d = 0) {
    char s[256];
    std::snprintf(s, sizeof s, f, a, b, c, d);
    return s;
}
void near(const std::string& what, double expected, double actual, double relTol) {
    const double err = actual / expected - 1;
    check(std::abs(err) <= relTol, what + fmt(" expected=%.6g computed=%.6g err=%+.3f%% tol=%.3g%%", expected, actual,
                                              100 * err, 100 * relTol));
}
const char* burstName(BurstType t) { return t == kAir ? "free-air" : "surface"; }

struct Field { double BP::*member; const char* name; double unit; const char* unitName; };
const Field kPs{&BP::incidentPressurePa, "Ps", 1e3, "kPa"}, kPr{&BP::reflectedPressurePa, "Pr", 1e3, "kPa"},
    kIs{&BP::incidentImpulsePaS, "is", 1, "Pa*s"}, kIr{&BP::reflectedImpulsePaS, "ir", 1, "Pa*s"},
    kTa{&BP::arrivalTimeS, "ta", 1e-3, "ms"}, kT0{&BP::positiveDurationS, "t0", 1e-3, "ms"},
    kU{&BP::shockFrontVelocityMps, "U", 1, "m/s"};

// One tabulated point: charge W [kg TNT], range R [m], field, expected value in field units, tolerance.
struct BlastRef { const char* source; BurstType type; double w, r; Field field; double expected, tol; };

void blastTable(const std::vector<BlastRef>& rows) {
    for (const auto& row : rows) {
        const BP b = blast(row.r, row.w, row.type);
        near(std::string("blast ") + row.source + " " + burstName(row.type) +
                 fmt(" W=%g kg R=%.4g m Z=%.4g ", row.w, row.r, b.scaledDistance) + row.field.name + " [" + row.field.unitName + "]",
             row.expected, b.*(row.field.member) / row.field.unit, row.tol);
    }
}

// M.M. Swisdak, "Simplified Kingery Airblast Calculations" (1994, DTIC ADA526744), surface burst, metric:
// Y = exp(A + B lnZ + C lnZ^2 + ...), transcription of kingery-bulmash 1.0.1 (PyPI). Independent fit of the
// K-B surface-burst data; used as a second reference over the whole Z range.
struct SwisdakSeg { double zMax; double a[7]; };
double swisdak(const std::vector<SwisdakSeg>& segs, double z) {
    for (const auto& s : segs)
        if (z <= s.zMax) {
            const double l = std::log(z);
            double y = 0, p = 1;
            for (double c : s.a) { y += c * p; p *= l; }
            return std::exp(y);
        }
    return NAN;
}
const std::vector<SwisdakSeg> kSwTa{{1.5, {-0.7604, 1.8058, 0.1257, -0.0437, -0.0310, -0.00669, 0}},
                                    {40, {-0.7137, 1.5732, 0.5561, -0.4213, 0.1054, -0.00929, 0}}};
const std::vector<SwisdakSeg> kSwPs{{2.9, {7.2106, -2.1069, -0.3229, 0.1117, 0.0685, 0, 0}},
                                    {23.8, {7.5938, -3.0523, 0.40977, 0.0261, -0.01267, 0, 0}},
                                    {198.5, {6.0536, -1.4066, 0, 0, 0, 0, 0}}};
const std::vector<SwisdakSeg> kSwPr{{2.0, {9.006, -2.6893, -0.6295, 0.1011, 0.29255, 0.13505, 0.019736}},
                                    {40, {8.8396, -1.733, -2.64, 2.293, -0.8232, 0.14247, -0.0099}}};
const std::vector<SwisdakSeg> kSwT0{{1.02, {0.5426, 3.2299, -1.5931, -5.9667, -4.0815, -0.9149, 0}},
                                    {2.8, {0.5440, 2.7082, -9.7354, 14.3425, -9.7791, 2.8535, 0}},
                                    {40, {-2.4608, 7.1639, -5.6215, 2.2711, -0.44994, 0.03486, 0}}};
const std::vector<SwisdakSeg> kSwIs{{0.96, {5.522, 1.117, 0.6, -0.292, -0.087, 0, 0}},
                                    {2.38, {5.465, -0.308, -1.464, 1.362, -0.432, 0, 0}},
                                    {33.7, {5.2749, -0.4677, -0.2499, 0.0588, -0.00554, 0, 0}},
                                    {158.7, {5.9825, -1.062, 0, 0, 0, 0, 0}}};
const std::vector<SwisdakSeg> kSwIr{{40, {6.7853, -1.3466, 0.101, -0.01123, 0, 0, 0}}};
const std::vector<SwisdakSeg> kSwU{{1.5, {0.1794, -0.956, -0.0866, 0.109, 0.0699, 0.01218, 0}},
                                   {40, {0.2597, -1.326, 0.3767, 0.0396, -0.0351, 0.00432, 0}}};

void testBlastTables() {
    const double w750 = std::cbrt(750.0);
    std::vector<BlastRef> rows;
    // UNODA IATG 01.80:2021 (3rd ed.) Table 5 "Examples of calculated blast wave parameters by Kingery-Bulmash
    // formulae" (hemispherical surface burst, bar -> kPa, bar*ms -> Pa*s); values tabulated to 2-3 digits.
    struct T5 { double w, ps, is, dyn, pr, ir, t0, ta, u; };
    for (const T5& t : {T5{1000, 0.43, 5.9, 0.062, 1.01, 12.6, 37.9, 82.4, 398}, T5{10000, 2.02, 25.2, 1.12, 6.8, 65.5, 46.8, 48.1, 559},
                        T5{100000, 11.5, 106, 17.9, 66.5, 372, 93, 24.8, 1114}}) {
        const char* s = "IATG01.80:2021-T5";
        rows.insert(rows.end(), {{s, kSurf, t.w, 50, kPs, 100 * t.ps, 0.02}, {s, kSurf, t.w, 50, kIs, 100 * t.is, 0.02},
                                 {s, kSurf, t.w, 50, kPr, 100 * t.pr, 0.02}, {s, kSurf, t.w, 50, kIr, 100 * t.ir, 0.02},
                                 {s, kSurf, t.w, 50, kT0, t.t0, 0.02}, {s, kSurf, t.w, 50, kTa, t.ta, 0.02},
                                 {s, kSurf, t.w, 50, kU, t.u, 0.02}});
    }
    // IATG 01.80:2021 Table 3 "Example of application of the scaling rules" (surface burst, Z = 22.2).
    for (double k : {1.0, 2.0, 4.0}) {
        const char* s = "IATG01.80:2021-T3";
        rows.insert(rows.end(), {{s, kSurf, 1000 * k * k * k, 222 * k, kPs, 5.4, 0.02},
                                 {s, kSurf, 1000 * k * k * k, 222 * k, kIs, 143 * k, 0.02},
                                 {s, kSurf, 1000 * k * k * k, 222 * k, kT0, 61.1 * k, 0.02}});
    }
    // Karlos & Solomos, "Calculation of blast loads for application to structural components", JRC EUR 26456 EN
    // (2013), section 5.1: 750 kg TNT free-air burst, values read from their K-B diagram (Fig. 13); the fourth
    // row is the fictitious Z = 2.72 used for the Mach stem (is = 636 kPa*ms, Ps = 99.7 kPa).
    struct K { double z, ps, is, t0, ta, u; };
    for (const K& k : {K{1.44, 409.5, 1137.5, 15.3, 9.6, 720}, K{2.54, 115.5, 676.6, 21.7, 27.2, 480},
                       K{2.72, 99.7, 636.0, 22.7, 30.8, 460}, K{4.40, 39.1, 415.2, 28.9, 67.2, 390}}) {
        const char* s = "JRC-EUR26456-5.1";
        const double r = k.z * w750;
        rows.insert(rows.end(), {{s, kAir, 750, r, kPs, k.ps, 0.03}, {s, kAir, 750, r, kIs, k.is, 0.03},
                                 {s, kAir, 750, r, kT0, k.t0, 0.03}, {s, kAir, 750, r, kTa, k.ta, 0.03},
                                 {s, kAir, 750, r, kU, k.u, 0.03}});
    }
    // Same report, section 5.2: 1000 kg C4 = 1302 kg TNT surface burst, read from Fig. 14 at the report's
    // Z = 2.56. The report prints Z = 27.04 / 1302^(1/3) = 2.56, but that ratio is 2.476 (arithmetic slip);
    // its chart readings belong to Z = 2.56, so the check is evaluated at R = 2.56 * 1302^(1/3) = 27.96 m.
    const char* s52 = "JRC-EUR26456-5.2";const double r52 = 2.56 * std::cbrt(1302.0);
    rows.insert(rows.end(), {{s52, kSurf, 1302, r52, kPs, 160, 0.05}, {s52, kSurf, 1302, r52, kIs, 1143.81, 0.05},
                             {s52, kSurf, 1302, r52, kPr, 500, 0.05}, {s52, kSurf, 1302, r52, kIr, 2878.63, 0.05},
                             {s52, kSurf, 1302, r52, kT0, 25.59, 0.05}});
    blastTable(rows);

    // Swisdak (1994) simplified surface-burst fits over the whole range (max deviation per parameter).
    struct SwRow { Field f; const std::vector<SwisdakSeg>* fit; double scale; };
    for (const SwRow& sw : {SwRow{kPs, &kSwPs, 1}, SwRow{kPr, &kSwPr, 1}, SwRow{kIs, &kSwIs, 1}, SwRow{kIr, &kSwIr, 1},
                            SwRow{kTa, &kSwTa, 1}, SwRow{kT0, &kSwT0, 1}, SwRow{kU, &kSwU, 1000}}) {
        double worst = 0, worstZ = 0;
        for (double z : {0.25, 0.4, 0.6, 0.8, 1.0, 1.5, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0, 39.0}) {
            const BP b = blast(z, 1.0, kSurf);
            const double e = b.*(sw.f.member) / sw.f.unit / (swisdak(*sw.fit, z) * sw.scale) - 1;
            if (std::abs(e) > std::abs(worst)) { worst = e; worstZ = z; }
        }
        check(std::abs(worst) <= 0.02, std::string("blast Swisdak1994 surface ") + sw.f.name +
                                           fmt(" max deviation over Z=0.25..39: %+.3f%% at Z=%g tol=2%%", 100 * worst, worstZ));
    }
}

void testBlastPhysics() {
    // Normal reflection factor vs ideal-gas Rankine-Hugoniot (gamma = 1.4): Cr = (8 Ps + 14 P0)/(Ps + 7 P0).
    for (BurstType t : {kAir, kSurf})
        for (double z : {2.0, 3.0, 5.0, 10.0, 20.0, 40.0}) {
            const BP b = blast(z, 1.0, t);
            const double ps = b.incidentPressurePa, p0 = kAmbientPressurePa;
            near(fmt("blast reflection factor Pr/Ps vs ideal-gas R-H, Z=%g ", z) + burstName(t),
                 (8 * ps + 14 * p0) / (ps + 7 * p0), b.reflectedPressurePa / ps, 0.03);
        }
    // K-B surface burst behaves as a free-air burst of 1.8 W: is, ir, t0 (absolute values at the same R).
    for (double z : {0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 39.0}) {
        const BP h = blast(z, 1.0, kSurf), s = blast(z, 1.8, kAir);
        near(fmt("blast surface(W)=free-air(1.8W) is Z=%g", z), s.incidentImpulsePaS, h.incidentImpulsePaS, 0.03);
        near(fmt("blast surface(W)=free-air(1.8W) ir Z=%g", z), s.reflectedImpulsePaS, h.reflectedImpulsePaS, 0.03);
        near(fmt("blast surface(W)=free-air(1.8W) t0 Z=%g", z), s.positiveDurationS, h.positiveDurationS, 0.03);
    }
    // Shock speed: free air uses R-H from Ps; the surface-burst K-B U fit must agree with R-H as well.
    for (double z : {0.5, 1.0, 3.0, 10.0, 40.0}) {
        const BP b = blast(z, 1.0, kSurf);
        near(fmt("blast surface U fit vs R-H(Ps) Z=%g", z), rankineHugoniotShockSpeedMps(b.incidentPressurePa),
             b.shockFrontVelocityMps, 0.03);
    }
}

void testBlastShape() {
    for (BurstType t : {kAir, kSurf}) {
        const double zMin = validScaledDistanceMin(t), zMax = validScaledDistanceMax(t);
        const int n = 3000;
        std::vector<BP> v;
        std::vector<double> zs;
        for (int i = 0; i <= n; ++i) {
            zs.push_back(zMin * std::pow(zMax / zMin, double(i) / n));
            v.push_back(blast(zs.back(), 1.0, t));
        }
        auto monotone = [&](const Field& f, double fromZ, int sign) {
            bool ok = true;
            for (int i = 1; i <= n; ++i)
                if (zs[i - 1] >= fromZ && sign * (v[i].*(f.member) - v[i - 1].*(f.member)) <= 0) ok = false;
            check(ok, std::string("blast monotone ") + burstName(t) + " " + f.name + (sign < 0 ? " decreasing" : " increasing") +
                          fmt(" for Z=%g..%g (3000 log steps)", fromZ, zMax));
        };
        monotone(kPs, zMin, -1);
        monotone(kPr, zMin, -1);
        monotone(kIr, zMin, -1);
        monotone(kIs, 1.0, -1);  // K-B is has a genuine near-field dip/hump (min ~0.4-0.5, max ~0.8-0.96)
        monotone(kTa, zMin, 1);
        double worst = 0, worstStep = 0;  // continuity across segment joins: second difference of ln(value)
        for (const Field& f : {kPs, kPr, kIs, kIr, kTa, kT0, kU})
            for (int i = 1; i < n; ++i) {
                if (f.member == kT0.member && zs[i - 1] < 0.2) continue;  // t0 is held constant below 0.147/0.178
                const double a = std::log(v[i - 1].*(f.member)), b = std::log(v[i].*(f.member)), c = std::log(v[i + 1].*(f.member));
                worst = std::max(worst, std::abs(c - 2 * b + a));
                worstStep = std::max(worstStep, std::abs(b - a));
            }
        // A value jump of 0.5% gives |d2 lnY| >= 5e-3. The published K-B fits are piecewise in U = K0 + K1 ln Z,
        // so their segment joins are continuous in value but not in slope: a slope kink of 0.8 in ln-ln shows up
        // as ~1.7e-3 at this step. The 4e-3 threshold therefore rejects jumps but accepts the published kinks.
        check(worst < 4e-3, std::string("blast continuity ") + burstName(t) +
                                fmt(" all 7 outputs: max |d2 lnY| %.2g (< 4e-3; a 0.5%% step would give 5e-3), max step %.3f%% per dlnZ=%.4f",
                                    worst, 100 * worstStep, std::log(zMax / zMin) / n));
        // ta keeps increasing beyond Zmax (acoustic extension) and is continuous there.
        const BP at = blast(zMax * 0.999999, 1.0, t), above = blast(zMax * 1.000001, 1.0, t), far = blast(100, 1.0, t);
        check(std::abs(above.arrivalTimeS / at.arrivalTimeS - 1) < 1e-5 && far.arrivalTimeS > above.arrivalTimeS &&
                  std::abs(far.arrivalTimeS - above.arrivalTimeS - (100 - zMax * 1.000001) / kAmbientSoundSpeedMps) < 1e-9,
              std::string("blast arrival time beyond Zmax continuous and acoustic, ") + burstName(t));
    }
}

void testBlastScalingAndClamp() {
    const double k = std::cbrt(2.0);
    for (BurstType t : {kAir, kSurf}) {
        double worst = 0;
        for (double z : {0.06, 0.1, 0.5, 1.0, 3.0, 10.0, 30.0, 60.0}) {
            const BP a = blast(z * std::cbrt(10.0), 10.0, t), b = blast(z * std::cbrt(20.0), 20.0, t);
            for (const Field& f : {kPs, kPr, kU}) worst = std::max(worst, std::abs(b.*(f.member) / a.*(f.member) - 1));
            for (const Field& f : {kIs, kIr, kTa, kT0}) worst = std::max(worst, std::abs(b.*(f.member) / a.*(f.member) / k - 1));
        }
        check(worst < 1e-12, std::string("blast cube-root scaling ") + burstName(t) +
                                 fmt(": W 10->20 kg at fixed Z, pressures/U equal, is/ir/ta/t0 x2^(1/3), max err %.2g", worst));
        const double zMin = validScaledDistanceMin(t);
        const BP lo = blast(0.5 * zMin, 1.0, t), loRef = blast(zMin, 1.0, t), hi = blast(80, 1.0, t), hiRef = blast(40, 1.0, t);
        const BP mid = blast(5, 1.0, t);
        check(lo.clamped && lo.scaledDistance == zMin && lo.incidentPressurePa == loRef.incidentPressurePa && !loRef.clamped,
              std::string("blast clamp below Zmin ") + burstName(t) + fmt(" (Zmin=%g)", zMin));
        check(hi.clamped && hi.scaledDistance == 40 && hi.incidentPressurePa == hiRef.incidentPressurePa &&
                  hi.positiveDurationS == hiRef.positiveDurationS && !hiRef.clamped && !mid.clamped,
              std::string("blast clamp above Zmax=40 ") + burstName(t));
    }
    int rejected = 0;
    for (double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        try { (void)blast(bad, 1); } catch (const std::invalid_argument&) { ++rejected; }
        try { (void)blast(10, bad); } catch (const std::invalid_argument&) { ++rejected; }
    }
    check(rejected == 8, "blast rejects non-positive / non-finite range and charge (8 cases)");
}

void testOblique() {
    for (BurstType t : {kAir, kSurf})
        for (double z : {0.3, 2.0, 20.0}) {
            const BP b = blast(z, 100.0, t);
            const double ps = b.incidentPressurePa, pr = b.reflectedPressurePa, is = b.incidentImpulsePaS, ir = b.reflectedImpulsePaS;
            bool ok = reflectedPressureAtIncidence(b, 0) == pr && reflectedImpulseAtIncidence(b, 0) == ir;
            ok = ok && std::abs(reflectedPressureAtIncidence(b, kPi / 2) / ps - 1) < 1e-12 &&
                 std::abs(reflectedImpulseAtIncidence(b, kPi / 2) / is - 1) < 1e-12;
            ok = ok && reflectedPressureAtIncidence(b, 2.5) == ps && reflectedPressureAtIncidence(b, -0.4) == reflectedPressureAtIncidence(b, 0.4);
            const double c = std::cos(kPi / 4);
            ok = ok && std::abs(reflectedPressureAtIncidence(b, kPi / 4) / (ps * (1 + c - 2 * c * c) + pr * c * c) - 1) < 1e-12;
            double prevP = pr, prevI = ir, maxStep = 0;
            for (int i = 1; i <= 2000; ++i) {
                const double a = kPi / 2 * i / 2000, p = reflectedPressureAtIncidence(b, a), im = reflectedImpulseAtIncidence(b, a);
                ok = ok && p < prevP && im < prevI && p >= ps * (1 - 1e-12) && im >= is * (1 - 1e-12);
                maxStep = std::max(maxStep, (prevP - p) / (pr - ps));
                prevP = p;
                prevI = im;
            }
            check(ok && maxStep < 0.01, std::string("oblique reflection ") + burstName(t) +
                                            fmt(" Z=%g: =Pr0/ir0 at 0, =Ps/is at 90deg, monotone, continuous (max step %.4f of Pr0-Ps)", z, maxStep));
        }
}

// Young (1997) eqs. 3.1/3.2 in the report's own US units (D [ft], W [lb], d [in], V [ft/s]), without K_h.
double youngUsFeet(double wLb, double dIn, double vFps, double s, double n) {
    const double wa = std::pow(wLb / (kPi * dIn * dIn / 4), 0.7);
    return vFps < 200 ? 0.3 * s * n * wa * std::log(1 + 2e-5 * vFps * vFps) : 0.00178 * s * n * wa * (vFps - 100);
}

void testYoung() {
    const double ft = 0.3048, lb = 0.45359237, in = 0.0254;
    const double n6 = ogiveNoseCoefficient(6.0);
    // (1) Report equations in US units vs the SI implementation (heavy penetrators, K_h = 1). SAND97-2426 has no
    //     numerical worked example, so the printed equations themselves are reproduced.
    struct Case { const char* name; double wLb, dIn, vFps, s; };
    for (const Case& c : {Case{"2000lb/18in/1000fps/S0.9 (eq.3.2)", 2000, 18, 1000, 0.9}, Case{"1000lb/10in/150fps/S0.9 (eq.3.1)", 1000, 10, 150, 0.9},
                          Case{"500lb/8in/2500fps/S1.2 (eq.3.2)", 500, 8, 2500, 1.2}, Case{"5000lb/24in/100fps/S0.7 (eq.3.1)", 5000, 24, 100, 0.7}}) {
        const auto r = youngConcrete({c.wLb * lb, c.dIn * in, c.vFps * ft, c.s, n6, 0});
        near(std::string("young US-unit eq. vs SI ") + c.name + " depth [m]", youngUsFeet(c.wLb, c.dIn, c.vFps, c.s, n6) * ft, r.depthM, 1e-9);
    }
    // (2) The report's SI appendix (A-3.2 printed 0.000018 (V-30.5); A-3.1 printed 0.0008).
    const double m = 2000 * lb, d = 18 * in, area = kPi * d * d / 4, ma = std::pow(m / area, 0.7);
    const auto hiV = youngConcrete({m, d, 304.8, 0.9, n6, 0}), loV = youngConcrete({m, d, 45.72, 0.9, n6, 0});
    near("young vs report SI eq. A-3.2 (printed constants) V=304.8 m/s", 0.000018 * 0.9 * n6 * ma * (304.8 - 30.5), hiV.depthM, 0.006);
    const double printedLow = 0.0008 * 0.9 * n6 * ma * std::log(1 + 2.15e-4 * 45.72 * 45.72);
    std::printf("INFO young report SI eq. A-3.1 (printed 0.0008) gives %.4g m vs %.4g m from eq. 3.1: ratio %.4f (exact SI constant 9.29e-4)\n",
                printedLow, loV.depthM, printedLow / loV.depthM);
    near("young documented A-3.1 discrepancy (printed/exact = 0.0008/9.293e-4)", 0.8609, printedLow / loV.depthM, 0.002);
    // (3) Branch continuity at 200 ft/s = 60.96 m/s.
    const double vb = 200 * ft;
    const auto below = youngConcrete({m, d, vb * (1 - 1e-9), 0.9, n6, 0}), above = youngConcrete({m, d, vb * (1 + 1e-9), 0.9, n6, 0});
    near("young branch continuity at 60.96 m/s (eq.3.1 vs 3.2, report gap 0.94%)", below.depthM, above.depthM, 0.015);
    bool rising = true;
    double prev = 0;
    for (int i = 1; i <= 400; ++i) {
        const double dep = youngConcrete({m, d, 3.0 * i, 0.9, n6, 0}).depthM;
        rising = rising && dep > prev;
        prev = dep;
    }
    check(rising, "young depth strictly increasing with speed 3..1200 m/s (both branches)");
    // (4) Perforation, residual speed (eq. 6.7) and constant-deceleration duration.
    const double dd = hiV.depthM, v = 304.8;
    const auto thin = youngConcrete({m, d, v, 0.9, n6, 0.5 * dd}), thick = youngConcrete({m, d, v, 0.9, n6, 1.01 * dd});
    const auto half = youngConcrete({m, d, v, 0.9, n6, -1}), inf = youngConcrete({m, d, v, 0.9, n6, INFINITY});
    check(thin.perforated && std::abs(thin.residualSpeedMps / (v * std::sqrt(0.5)) - 1) < 1e-12 &&
              std::abs(thin.durationS / (2 * 0.5 * dd / (v + thin.residualSpeedMps)) - 1) < 1e-12,
          fmt("young perforation T=0.5D: perforated, Vr=V/sqrt2=%.2f m/s, t=2T/(V+Vr)=%.3f ms", thin.residualSpeedMps, 1e3 * thin.durationS));
    check(!thick.perforated && thick.residualSpeedMps == 0 && std::abs(thick.durationS / (2 * dd / v) - 1) < 1e-12 &&
              !half.perforated && !inf.perforated && half.depthM == dd,
          fmt("young no perforation for T=1.01D, T<=0 and T=inf; t=2D/V=%.3f ms", 1e3 * thick.durationS));
    // (5) Light-penetrator factor K_h (A-3.4, capped at 1) and its effect on depth.
    near("young K_h(10 kg) = 0.46*10^0.15", 0.46 * std::pow(10.0, 0.15), youngConcrete({10, 0.1, 300, 0.9, n6, 0}).massScaleFactorKs, 1e-12);
    bool khOk = true;
    double prevKh = 0;
    for (int i = 1; i <= 400; ++i) {
        const double kh = youngConcrete({double(i), 0.1, 300, 0.9, n6, 0}).massScaleFactorKs;
        khOk = khOk && kh >= prevKh && kh <= 1 && (i < 182 || kh == 1);
        prevKh = kh;
    }
    check(khOk, "young K_h non-decreasing, <= 1, == 1 for m >= 182 kg (1..400 kg)");
    const double mSmall = 20, dSmall = 0.08;
    const auto small = youngConcrete({mSmall, dSmall, 400, 0.9, n6, 0});
    near("young depth includes K_h (20 kg, 80 mm, 400 m/s)",
         youngUsFeet(mSmall / lb, dSmall / in, 400 / ft, 0.9, n6) * ft * 0.46 * std::pow(mSmall, 0.15), small.depthM, 1e-9);
    std::printf("INFO young K_h SI (0.46 m^0.15) / US eq.3.4 (0.4 W^0.15) = %.4f\n", 0.46 * std::pow(mSmall, 0.15) / (0.4 * std::pow(mSmall / lb, 0.15)));
    check(small.outsideValidity == (small.depthM < 3 * dSmall) && youngConcrete({3, 0.05, 300, 0.9, n6, 0}).outsideValidity &&
              !hiV.outsideValidity,
          "young validity flag (m < 5 kg, V > 1219 m/s, D < 3 calibres)");
    // (6) Nose coefficients: eq. 3.8 equals eq. 3.7 with the tangent-ogive nose length; conic eq. 3.9.
    bool noseOk = std::abs(ogiveNoseCoefficient(0.5) - 0.65) < 1e-12 && std::abs(conicNoseCoefficient(2) - 1.06) < 1e-12;
    for (double crh : {0.5, 1.0, 3.0, 6.0, 9.0}) {
        const double s = crh, r = 0.5;                      // ogive radius and body radius in calibres
        const double ln = std::sqrt(s * s - (s - r) * (s - r));  // tangent-ogive nose length [calibres]
        noseOk = noseOk && std::abs(ogiveNoseCoefficient(crh) - (0.18 * ln + 0.56)) < 1e-12;
    }
    check(noseOk, fmt("young nose N: ogive eq.3.8 == eq.3.7 with tangent-ogive Ln/d (CRH 0.5..9), N(6 CRH)=%.4f, cone(2 cal)=1.06", n6));
    // (7) Concrete S-number: SI eq. A-4.2 vs US eq. 4.2 at 5000 psi (35 MPa vs 34.47 MPa rounding: 0.45 %).
    const double sSi = youngConcreteSNumber(5000 * 0.00689475729, 1.5, 1, 6, 40, true);
    near("young concrete S: SI A-4.2 vs US 4.2 (5000 psi, 1.5% rebar, Tc=6, wide)", 0.085 * 9.5 * std::pow(6.0, -0.06), sSi, 0.006);
}

void testEmpirical() {
    near("recovery temperature V=250 m/s T=288 K (Python reference 315.6831 K)", 315.6830816, recoveryTemperatureK(288, 250), 1e-9);
    near("recovery temperature = T + r V^2/(2 cp), cp = gamma R/(gamma-1)", 288 + 0.89 * 250 * 250 / (2 * 1.4 * 287.05 / 0.4),
         recoveryTemperatureK(288, 250), 1e-12);
    check(recoveryTemperatureK(250, 0) == 250, "recovery temperature at rest equals ambient");
    // Planck: reference from Python decimal (40 digits) with the exact SI constants.
    near("planck B(4 um, 1000 K) [W m^-2 sr^-1 m^-1]", 3.2776635189595523e9, planckSpectralRadiance(4e-6, 1000), 1e-10);
    near("planck B(10 um, 300 K)", 9.924033330070695e6, planckSpectralRadiance(10e-6, 300), 1e-10);
    double integral = 0;  // pi * int B dlambda = sigma T^4 (Stefan-Boltzmann), log-spaced trapezoid 0.1-1000 um
    const int n = 20000;
    for (int i = 0; i < n; ++i) {
        const double a = 1e-7 * std::pow(1e4, double(i) / n), b = 1e-7 * std::pow(1e4, double(i + 1) / n);
        integral += 0.5 * (planckSpectralRadiance(a, 1000) + planckSpectralRadiance(b, 1000)) * (b - a);
    }
    near("planck pi*integral = sigma T^4 at 1000 K", 5.670374419e-8 * 1e12, kPi * integral, 1e-4);
    const double peak = 2.897771955e-3 / 1500;  // Wien displacement
    check(planckSpectralRadiance(peak, 1500) > planckSpectralRadiance(peak * 0.99, 1500) &&
              planckSpectralRadiance(peak, 1500) > planckSpectralRadiance(peak * 1.01, 1500) && planckSpectralRadiance(1e-7, 300) >= 0 &&
              planckSpectralRadiance(4e-6, 0) == 0,
          "planck Wien peak at 1500 K, no overflow at short wavelength, 0 at T=0");
    // Fireball (IATG 01.80:2021 Table 34, Baker): D = 3.86 W^0.32, t = 0.299 W^0.32.
    const Fireball f1 = fireball(1000);
    near("fireball D(1000 kg) = 3.86*1000^0.32 [m]", 3.86 * std::pow(1000.0, 0.32), f1.diameterM, 1e-12);
    near("fireball t(1000 kg) = 0.299*1000^0.32 [s]", 0.299 * std::pow(1000.0, 0.32), f1.durationS, 1e-12);
    bool mono = true;
    Fireball prev = fireball(0.01);
    for (int i = 1; i <= 200; ++i) {
        const Fireball f = fireball(0.01 * std::pow(1e8, i / 200.0));
        mono = mono && f.diameterM > prev.diameterM && f.durationS > prev.durationS;
        prev = f;
    }
    near("fireball doubling W scales D by 2^0.32", std::pow(2.0, 0.32), fireball(20).diameterM / fireball(10).diameterM, 1e-12);
    check(mono, "fireball diameter and duration strictly increasing in W (0.01 kg .. 1e6 kg)");
}
}  // namespace

int main() {
    try {
        testBlastTables();
        testBlastPhysics();
        testBlastShape();
        testBlastScalingAndClamp();
        testOblique();
        testYoung();
        testEmpirical();
    } catch (const std::exception& e) {
        std::printf("FAIL unexpected exception: %s\n", e.what());
        return 2;
    }
    std::printf("%s impact fixture: %d/%d checks passed\n", failures ? "FAIL" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}
