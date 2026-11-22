// Numerical check of the shared LIDF / G / Gamma code used by VoxelRT and
// VoxelEB (models/histream/shader/lidf_common.h, compiled here as C++).
//
// Build and run, either through CMake (target excluded from the default build):
//   cmake --build build\histream --config Release --target histream_lidf_gamma_tests
//   build\histream\Release\histream_lidf_gamma_tests.exe
// or directly (Developer PowerShell / vcvars64):
//   cl /nologo /std:c++17 /O2 /EHsc tools\test-lidf-gamma.cpp /Fe:%TEMP%\test-lidf-gamma.exe
//   %TEMP%\test-lidf-gamma.exe
// Exit code 0 means every check passed.
//
// Checks
//   1. SAIL class frequencies: sum 1, invalid parameters rejected.
//   2. G(mu) of the closed-form class kernel against brute-force azimuthal
//      integration of |Omega . n| for the same 13 classes, and against an
//      independent double-precision PROSAIL transcription (dcum + volscatt
//      chi_s, G = k_s cos theta_s).
//   3. G(mu) of the 13-class SAIL discretization against the continuous
//      Verhoef distribution (planophile a=1,b=0; erectophile a=-1,b=0; ...).
//   4. Mean of G over the sphere = 1/2 for every LIDF.
//   5. Gamma (SAIL volscatt kernel) against brute-force integration over leaf
//      normals, including observers and sources below the horizon.
//   6. Normalization (1/pi) int_4pi GammaR dOmega = (1/pi) int_4pi GammaT dOmega
//      = G(source), i.e. (1/pi) int Gamma dOmega = (rho + tau) G(source).
//   7. Spherical limit: a continuous spherical LIDF reproduces the Ross
//      closed form used before this change; the 13-class spherical and the
//      default SAIL "spherical" (-0.35,-0.15) LIDF stay close to it.
#include "../models/histream/shader/lidf_common.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;
int failures = 0;

void check(bool ok, const std::string& what, double value, double limit)
{
    std::printf("  [%s] %-58s %.3e (limit %.1e)\n", ok ? "ok" : "FAIL", what.c_str(), value, limit);
    if (!ok) ++failures;
}

struct Dir { double x, y, z; };
Dir fromAngles(double zenithDeg, double azimuthDeg)
{
    const double t = zenithDeg * kPi / 180.0, p = azimuthDeg * kPi / 180.0;
    return {std::sin(t) * std::cos(p), std::cos(t), std::sin(t) * std::sin(p)};
}

// (1/2pi) int_0^2pi |d . n(thetaL, phi)| dphi
double bruteClassProjection(double thetaL, double mu)
{
    const int n = 20000;
    const double st = std::sqrt(std::max(0.0, 1.0 - mu * mu));
    double sum = 0.0;
    for (int k = 0; k < n; ++k) {
        const double phi = (k + 0.5) * 2.0 * kPi / n;
        sum += std::abs(std::cos(thetaL) * mu + std::sin(thetaL) * st * std::cos(phi));
    }
    return sum / n;
}

// Gamma_R/T of one inclination class by brute force, s towards the source,
// o towards the observer: reflection when both lie on the same leaf side.
void bruteClassGamma(double thetaL, Dir s, Dir o, double& r, double& t)
{
    const int n = 20000;
    r = t = 0.0;
    for (int k = 0; k < n; ++k) {
        const double phi = (k + 0.5) * 2.0 * kPi / n;
        const Dir nrm{std::sin(thetaL) * std::cos(phi), std::cos(thetaL), std::sin(thetaL) * std::sin(phi)};
        const double a = s.x * nrm.x + s.y * nrm.y + s.z * nrm.z;
        const double b = o.x * nrm.x + o.y * nrm.y + o.z * nrm.z;
        if (a * b > 0.0) r += std::abs(a * b); else t += std::abs(a * b);
    }
    r /= n;
    t /= n;
}

double classCenterRad(int i)
{
    return (i < 8 ? 5.0 + 10.0 * i : 81.0 + 2.0 * (i - 8)) * kPi / 180.0;
}

double rossSphericalGamma(Dir s, Dir o, double rho, double tau)
{
    // a = angle between incident (-s) and scattered (o) propagation.
    const double cosa = std::max(-1.0, std::min(1.0, -(s.x * o.x + s.y * o.y + s.z * o.z)));
    const double a = std::acos(cosa);
    return (rho + tau) / (3.0 * kPi) * (std::sin(a) - a * cosa) + tau / 3.0 * cosa;
}

// Continuous LIDF density on a fine inclination grid (by differencing the
// SAIL cumulative), used for discretization and spherical-limit checks.
std::vector<double> continuousDensity(double a, double b, int n)
{
    std::vector<double> f(n);
    double previous = 0.0;
    for (int k = 0; k < n; ++k) {
        const double upper = 90.0 * (k + 1) / n;
        const double cumulative = k == n - 1 ? 1.0 : lidf::sailCumulative(a, b, upper);
        f[k] = cumulative - previous;
        previous = cumulative;
    }
    return f;
}

double continuousG(const std::vector<double>& f, double mu)
{
    const int n = static_cast<int>(f.size());
    double g = 0.0;
    for (int k = 0; k < n; ++k) {
        const double theta = (k + 0.5) * 0.5 * kPi / n;
        g += f[k] * lidf::lidfClassProjection(
            static_cast<float>(std::cos(theta)), static_cast<float>(std::sin(theta)), static_cast<float>(mu));
    }
    return g;
}

// Independent double-precision transcription of PROSAIL (Verhoef SAIL) for the
// reference G: dcum/dladgen class frequencies and volscatt chi_s, so that
// G_SAIL(theta_s) = sum_i lidf_i chi_s(theta_Li) = k_s cos(theta_s).
double prosailDcum(double a, double b, double t)
{
    const double rd = kPi / 180.0;
    if (a > 1.0) return 1.0 - std::cos(rd * t);
    const double p = 2.0 * rd * t;
    double x = p, y = 0.0, delx = 1.0;
    while (delx > 1e-12) {
        y = a * std::sin(x) + 0.5 * b * std::sin(2.0 * x);
        const double dx = 0.5 * (y - x + p);
        x += dx;
        delx = std::abs(dx);
    }
    return (2.0 * y + p) / kPi;
}

double prosailG(double a, double b, double thetaSDeg)
{
    const double rd = kPi / 180.0;
    double freq[13];
    for (int i = 0; i < 8; ++i) freq[i] = prosailDcum(a, b, 10.0 * (i + 1));
    for (int i = 8; i < 12; ++i) freq[i] = prosailDcum(a, b, 80.0 + 2.0 * (i - 7));
    freq[12] = 1.0;
    for (int i = 12; i > 0; --i) freq[i] -= freq[i - 1];
    const double litab[13] = {5, 15, 25, 35, 45, 55, 65, 75, 81, 83, 85, 87, 89};
    const double cts = std::cos(rd * thetaSDeg), sts = std::sin(rd * thetaSDeg);
    double g = 0.0;
    for (int i = 0; i < 13; ++i) {
        const double cs = std::cos(rd * litab[i]) * cts;
        const double ss = std::sin(rd * litab[i]) * sts;
        const double cosbts = std::abs(ss) > 1e-6 ? -cs / ss : 5.0;
        double bts = kPi, ds = cs;
        if (std::abs(cosbts) < 1.0) { bts = std::acos(cosbts); ds = ss; }
        (void)ds;
        const double chis = 2.0 / kPi * ((bts - kPi * 0.5) * cs + std::sin(bts) * ss);
        g += freq[i] * chis;
    }
    return g;
}

double sphereMean(const float* frequency, bool gamma, Dir s, double& reflectionIntegral, double& transmissionIntegral)
{
    // Midpoint rule in (mu, phi), equal solid angle cells.
    const int nm = 400, np = 400;
    double gMean = 0.0;
    reflectionIntegral = transmissionIntegral = 0.0;
    for (int i = 0; i < nm; ++i) {
        const double mu = -1.0 + (i + 0.5) * 2.0 / nm;
        const double st = std::sqrt(std::max(0.0, 1.0 - mu * mu));
        const double gm = lidf::lidfProjection(frequency, static_cast<float>(mu));
        gMean += gm / nm;
        if (!gamma) continue;
        for (int j = 0; j < np; ++j) {
            const double phi = (j + 0.5) * 2.0 * kPi / np;
            float r = 0.0f, t = 0.0f;
            lidf::lidfGamma(frequency, float(s.x), float(s.y), float(s.z),
                float(st * std::cos(phi)), float(mu), float(st * std::sin(phi)), r, t);
            reflectionIntegral += r * (2.0 / nm) * (2.0 * kPi / np);
            transmissionIntegral += t * (2.0 / nm) * (2.0 * kPi / np);
        }
    }
    reflectionIntegral /= kPi;
    transmissionIntegral /= kPi;
    return gMean;
}
} // namespace

int main()
{
    struct Case { const char* name; double a, b; };
    const Case cases[] = {
        {"planophile (1, 0)", 1.0, 0.0},
        {"erectophile (-1, 0)", -1.0, 0.0},
        {"plagiophile (0, -1)", 0.0, -1.0},
        {"extremophile (0, 1)", 0.0, 1.0},
        {"uniform (0, 0)", 0.0, 0.0},
        {"SAIL spherical (-0.35, -0.15)", -0.35, -0.15},
        {"exact spherical (a > 1)", 2.0, 0.0},
    };

    std::printf("1. SAIL class frequencies\n");
    float tmp[LIDF_CLASS_COUNT];
    check(!lidf::sailLeafAngleFrequencies(0.8, 0.5, tmp), "|a|+|b| > 1 rejected", 0.0, 0.0);
    check(!lidf::sailLeafAngleFrequencies(std::nan(""), 0.0, tmp), "NaN rejected", 0.0, 0.0);
    for (int i = 0; i < LIDF_CLASS_COUNT; ++i) {
        const double c = classCenterRad(i);
        check(std::abs(std::cos(c) - lidf::LIDF_CLASS_COS[i]) < 1e-7
              && std::abs(std::sin(c) - lidf::LIDF_CLASS_SIN[i]) < 1e-7,
              "class centre table " + std::to_string(i), std::abs(std::cos(c) - lidf::LIDF_CLASS_COS[i]), 1e-7);
    }

    std::mt19937 rng(20261122);
    std::uniform_real_distribution<double> uz(0.0, 180.0), ua(0.0, 360.0);

    for (const Case& c : cases) {
        float f[LIDF_CLASS_COUNT];
        const bool ok = lidf::sailLeafAngleFrequencies(c.a, c.b, f);
        std::printf("\n== %s\n  lidf:", c.name);
        double sum = 0.0;
        for (float v : f) { std::printf(" %.4f", v); sum += v; }
        std::printf("\n");
        check(ok && std::abs(sum - 1.0) < 1e-6, "frequencies valid, sum = 1", std::abs(sum - 1.0), 1e-6);

        // 2./3. G(mu)
        const auto density = continuousDensity(c.a, c.b, 9000);
        double errBrute = 0.0, errCont = 0.0;
        std::printf("  theta  G(13 classes)  G(continuous)\n");
        for (int deg = 0; deg <= 90; deg += 5) {
            const double mu = std::cos(deg * kPi / 180.0);
            double brute = 0.0;
            for (int i = 0; i < LIDF_CLASS_COUNT; ++i) brute += f[i] * bruteClassProjection(classCenterRad(i), mu);
            const double g = lidf::lidfProjection(f, static_cast<float>(mu));
            const double gc = continuousG(density, mu);
            errBrute = std::max(errBrute, std::abs(g - brute));
            errCont = std::max(errCont, std::abs(g - gc));
            if (deg % 15 == 0) std::printf("  %5d  %.5f        %.5f\n", deg, g, gc);
        }
        check(errBrute < 2e-5, "G closed form vs brute force (max abs)", errBrute, 2e-5);
        double errSail = 0.0;
        for (int deg = 0; deg <= 90; deg += 5)
            errSail = std::max(errSail, std::abs(lidf::lidfProjection(f, static_cast<float>(std::cos(deg * kPi / 180.0)))
                                                 - prosailG(c.a, c.b, deg)));
        check(errSail < 1e-5, "G vs PROSAIL reference k_s cos(theta_s) (max abs)", errSail, 1e-5);
        // SAIL lumps 88..90 deg into one 89 deg class; for planophile/extremophile this is the largest term.
        check(errCont < 3e-2, "G 13 classes vs continuous LIDF (max abs)", errCont, 3e-2);

        // 4./6. sphere mean of G and Gamma normalization
        double integralR = 0.0, integralT = 0.0;
        const double gMean = sphereMean(f, false, {0, 1, 0}, integralR, integralT);
        check(std::abs(gMean - 0.5) < 1e-4, "sphere mean of G = 1/2", std::abs(gMean - 0.5), 1e-4);
        for (int k = 0; k < 4; ++k) {
            const Dir s = fromAngles(k == 3 ? 120.0 : 10.0 + 25.0 * k, 30.0 * k);
            sphereMean(f, true, s, integralR, integralT);
            const double gs = lidf::lidfProjection(f, static_cast<float>(s.y));
            // Each face part integrates to G(s): (1/pi) int (rho GammaR + tau GammaT) = (rho + tau) G(s).
            const double rel = std::max(std::abs(integralR - gs), std::abs(integralT - gs)) / gs;
            check(rel < 2e-3,
                  "(1/pi) int GammaR,T dOmega = G(s), s zenith " + std::to_string(int(std::round(std::acos(s.y) * 180 / kPi))),
                  rel, 2e-3);
        }

        // 5. Gamma vs brute force, random directions over the whole sphere
        double errGamma = 0.0;
        for (int k = 0; k < 40; ++k) {
            const Dir s = fromAngles(uz(rng), ua(rng));
            const Dir o = fromAngles(uz(rng), ua(rng));
            double br = 0.0, bt = 0.0;
            for (int i = 0; i < LIDF_CLASS_COUNT; ++i) {
                double r = 0.0, t = 0.0;
                bruteClassGamma(classCenterRad(i), s, o, r, t);
                br += f[i] * r;
                bt += f[i] * t;
            }
            float r = 0.0f, t = 0.0f;
            lidf::lidfGamma(f, float(s.x), float(s.y), float(s.z), float(o.x), float(o.y), float(o.z), r, t);
            errGamma = std::max({errGamma, std::abs(r - br), std::abs(t - bt)});
        }
        check(errGamma < 2e-5, "Gamma volscatt vs brute force (max abs, 40 pairs)", errGamma, 2e-5);
    }

    // 7. Spherical limit.
    std::printf("\n7. Spherical limit (rho = 0.45, tau = 0.40)\n");
    {
        const auto density = continuousDensity(2.0, 0.0, 400);
        double errContinuous = 0.0, errExact13 = 0.0, errSail = 0.0;
        float exact13[LIDF_CLASS_COUNT], sail[LIDF_CLASS_COUNT];
        lidf::sailLeafAngleFrequencies(2.0, 0.0, exact13);
        lidf::sailLeafAngleFrequencies(-0.35, -0.15, sail);
        for (int k = 0; k < 30; ++k) {
            const Dir s = fromAngles(uz(rng) * 0.5, ua(rng));
            const Dir o = fromAngles(uz(rng), ua(rng));
            const double ross = rossSphericalGamma(s, o, 0.45, 0.40);
            double r = 0.0, t = 0.0;
            for (size_t i = 0; i < density.size(); ++i) {
                const double theta = (i + 0.5) * 0.5 * kPi / density.size();
                float fr = 0.0f, ft = 0.0f;
                lidf::lidfVolscatt(float(s.y), float(std::sqrt(1 - s.y * s.y)), float(o.y),
                    float(std::sqrt(std::max(0.0, 1 - o.y * o.y))),
                    float((s.x * o.x + s.z * o.z) / std::max(1e-12, std::sqrt((s.x * s.x + s.z * s.z) * (o.x * o.x + o.z * o.z)))),
                    float(std::acos(std::max(-1.0, std::min(1.0, (s.x * o.x + s.z * o.z)
                        / std::max(1e-12, std::sqrt((s.x * s.x + s.z * s.z) * (o.x * o.x + o.z * o.z))))))),
                    float(std::cos(theta)), float(std::sin(theta)), fr, ft);
                r += density[i] * kPi * fr;
                t += density[i] * kPi * ft;
            }
            errContinuous = std::max(errContinuous, std::abs(0.45 * r + 0.40 * t - ross));
            float er = 0, et = 0, sr = 0, st = 0;
            lidf::lidfGamma(exact13, float(s.x), float(s.y), float(s.z), float(o.x), float(o.y), float(o.z), er, et);
            lidf::lidfGamma(sail, float(s.x), float(s.y), float(s.z), float(o.x), float(o.y), float(o.z), sr, st);
            errExact13 = std::max(errExact13, std::abs(0.45 * er + 0.40 * et - ross));
            errSail = std::max(errSail, std::abs(0.45 * sr + 0.40 * st - ross));
        }
        check(errContinuous < 5e-4, "continuous spherical LIDF vs Ross closed form", errContinuous, 5e-4);
        check(errExact13 < 1e-2, "13-class spherical vs Ross closed form", errExact13, 1e-2);
        check(errSail < 3e-2, "SAIL (-0.35,-0.15) vs Ross closed form", errSail, 3e-2);
        double gMin = 1.0, gMax = 0.0;
        for (int deg = 0; deg <= 90; ++deg) {
            const double g = lidf::lidfProjection(sail, float(std::cos(deg * kPi / 180.0)));
            gMin = std::min(gMin, g);
            gMax = std::max(gMax, g);
        }
        std::printf("  default SAIL LIDF (-0.35,-0.15): G ranges %.4f .. %.4f over 0..90 deg\n", gMin, gMax);
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
