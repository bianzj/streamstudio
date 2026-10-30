#include "canopy.h"
#include "constants.h"
#include "radiosity.h"

#include <algorithm>
#include <cmath>

namespace stream {
namespace {

constexpr double kG = 0.5;

double zenith(double angle) { return std::min(std::abs(angle), 89.999); }
double cosine(double angle) { return std::cos(zenith(angle) * kPi / 180.0); }

// Leaf-scale hotspot correlation of the dual-scale closure (row_closure.leaf_correlation).
double leafCorrelation(double hotspot, double sunZenith, double viewZenith, double relativeAzimuth)
{
    const double sunTan = std::tan(zenith(sunZenith) * kPi / 180.0);
    const double viewTan = std::tan(zenith(viewZenith) * kPi / 180.0);
    const double separation = std::sqrt(std::max(0.0, (viewTan - sunTan) * (viewTan - sunTan) +
        4.0 * viewTan * sunTan * std::pow(std::sin(relativeAzimuth * kPi / 360.0), 2.0)));
    if (separation == 0.0) return 1.0;
    if (hotspot <= 0.0) return 0.0;
    const double a = separation * 2.0 / (kG / cosine(sunZenith) + kG / cosine(viewZenith)) / hotspot;
    return -std::expm1(-a) / a;
}

// Two-layer approximation of the sunlit visible leaf fraction inside the covered phase
// (hotspot.two_layer_sunlit_leaf, with c = 0 for the homogeneous canopy).
double twoLayerSunlitLeaf(double lai, double viewGap, double sunGap, double sunlitSoil)
{
    const double pv = std::clamp(viewGap, 1e-12, 1.0);
    const double ps = std::clamp(sunGap, 1e-12, 1.0);
    const double pss = std::max(sunlitSoil, 1e-300);
    const double kv = -std::log(pv) / lai;
    const double ks = -std::log(ps) / lai;
    const double overlap = std::clamp(std::log(pss / (pv * ps)) / lai, 0.0, std::min(kv, ks));
    const double index = std::max(0.82 - 0.08 * lai, 0.02);
    const double upper = std::sqrt((-std::log(1.0 - (1.0 - pv) * index) / std::max(kv * lai, 1e-12)) *
                                   (-std::log(1.0 - (1.0 - ps) * index) / std::max(ks * lai, 1e-12)));
    const double laiUpper = lai * upper;
    const double laiBottom = lai * (1.0 - upper);
    const double litBackground = std::exp(-(ks + kv - overlap) * laiUpper);
    const double litBottom = 1.0 - std::exp(-overlap * laiBottom);
    return std::clamp(1.0 - std::exp(-kv * laiUpper) + litBottom * litBackground, 0.0, 1.0 - pv);
}

// Gauss-Legendre nodes and weights on [-1, 1].
struct GaussNodes { double nodes[64]; double weights[64]; };

const GaussNodes& gauss64()
{
    static const GaussNodes table = [] {
        GaussNodes result{};
        constexpr int count = 64;
        for (int i = 0; i < count; ++i) {
            double x = std::cos(kPi * (i + 0.75) / (count + 0.5));
            double derivative = 0.0;
            for (int iteration = 0; iteration < 100; ++iteration) {
                double p0 = 1.0, p1 = x;
                for (int degree = 2; degree <= count; ++degree) {
                    const double current = ((2.0 * degree - 1.0) * x * p1 - (degree - 1.0) * p0) / degree;
                    p0 = p1;
                    p1 = current;
                }
                derivative = count * (x * p1 - p0) / (x * x - 1.0);
                const double step = p1 / derivative;
                x -= step;
                if (std::abs(step) < 1e-15) break;
            }
            result.nodes[i] = x;
            result.weights[i] = 2.0 / ((1.0 - x * x) * derivative * derivative);
        }
        return result;
    }();
    return table;
}

// Volume scattering function integrated over a leaf inclination (TiRT rt_transfer.volscatt).
void volumeScattering(double sunZenith, double viewZenith, double relativeAzimuth, double tilt,
                      double& forward, double& backward)
{
    const double costS = std::cos(sunZenith * kPi / 180.0), costO = std::cos(viewZenith * kPi / 180.0);
    const double sintS = std::sin(sunZenith * kPi / 180.0), sintO = std::sin(viewZenith * kPi / 180.0);
    const double costL = std::cos(tilt * kPi / 180.0), sintL = std::sin(tilt * kPi / 180.0);
    const double cosRaa = std::cos(relativeAzimuth * kPi / 180.0);
    const double cs = costL * costS, co = costL * costO;
    const double ss = sintL * sintS, so = sintL * sintO;
    const double cosBts = std::abs(ss) > 1e-6 ? -cs / ss : 5.0;
    const double cosBto = std::abs(so) > 1e-6 ? -co / so : 5.0;
    double bts, ds, bto, dos;
    if (std::abs(cosBts) < 1.0) { bts = std::acos(cosBts); ds = ss; }
    else { bts = kPi; ds = cs; }
    const double chiS = 2.0 / kPi * ((bts - kPi / 2.0) * cs + std::sin(bts) * ss);
    if (std::abs(cosBto) < 1.0) { bto = std::acos(cosBto); dos = so; }
    else if (viewZenith < 90.0) { bto = kPi; dos = co; }
    else { bto = 0.0; dos = -co; }
    (void)chiS;
    const double chiO = 2.0 / kPi * ((bto - kPi / 2.0) * co + std::sin(bto) * so);
    (void)chiO;
    const double btran1 = std::abs(bts - bto);
    const double btran2 = kPi - std::abs(bts + bto - kPi);
    const double raa = kPi / 180.0 * std::abs(relativeAzimuth);
    double bt1, bt2, bt3;
    if (raa <= btran1) { bt1 = raa; bt2 = btran1; bt3 = btran2; }
    else if (raa <= btran2) { bt1 = btran1; bt2 = raa; bt3 = btran2; }
    else { bt1 = btran1; bt2 = btran2; bt3 = raa; }
    const double t1 = 2.0 * cs * co + ss * so * cosRaa;
    const double t2 = bt2 > 0.0 ? std::sin(bt2) * (2.0 * ds * dos + ss * so * std::cos(bt1) * std::cos(bt3)) : 0.0;
    backward = std::max(((kPi - bt2) * t1 + t2) / (2.0 * kPi * kPi), 0.0);
    forward = std::max((-bt2 * t1 + t2) / (2.0 * kPi * kPi), 0.0);
}

}  // namespace

FourFractions canopyFractions(double lai, double hotspot, double sunZenith, double viewZenith,
                              double relativeAzimuth)
{
    FourFractions result;
    if (!(lai > 0.0)) { result.soilSun = 1.0; return result; }
    const double muS = std::max(cosine(sunZenith), 1e-6), muV = std::max(cosine(viewZenith), 1e-6);
    const double ks = kG / muS, kv = kG / muV;
    const double gs = std::exp(-ks * lai), gv = std::exp(-kv * lai);
    const double ts = ks * lai, tv = kv * lai;
    const double correlation = leafCorrelation(hotspot, sunZenith, viewZenith, relativeAzimuth);
    const double shared = std::min({tv, ts, std::sqrt(tv * ts) * correlation});
    const double joint = std::exp(-(ts + tv - shared));  // sunlit soil
    const double leafSun = twoLayerSunlitLeaf(lai, gv, gs, joint);
    result.soilSun = std::clamp(joint, 0.0, gv);
    result.soilShade = std::max(0.0, gv - result.soilSun);
    result.leafSun = std::clamp(leafSun, 0.0, std::max(0.0, 1.0 - gv));
    result.leafShade = std::max(0.0, 1.0 - gv - result.leafSun);
    return result;
}

SphericalCoefficients sphericalCoefficients(double sunZenith, double viewZenith, double relativeAzimuth)
{
    SphericalCoefficients result;
    const double cs = std::max(std::cos(sunZenith * kPi / 180.0), 1e-6);
    const double cv = std::max(std::cos(viewZenith * kPi / 180.0), 1e-6);
    result.ks = 0.5 / cs;
    result.ko = 0.5 / cv;
    const GaussNodes& gauss = gauss64();
    for (int i = 0; i < 64; ++i) {
        const double mu = 0.5 * (gauss.nodes[i] + 1.0);
        const double weight = 0.5 * gauss.weights[i];
        const double tilt = std::acos(std::min(mu, 1.0)) * 180.0 / kPi;
        double forward = 0.0, backward = 0.0;
        volumeScattering(sunZenith, viewZenith, relativeAzimuth, tilt, forward, backward);
        result.sob += weight * kPi * backward / (cs * cv);
        result.sof += weight * kPi * forward / (cs * cv);
    }
    return result;
}

double canopyReflectance(double lai, double hotspot, double sunZenith, double sunAzimuth, double viewZenith,
                         double viewAzimuth, double leafReflectance, double leafTransmittance,
                         double soilReflectance, double diffuseFraction, bool sunlit)
{
    const double direct = sunlit ? 1.0 - diffuseFraction : 0.0;
    if (!(lai > 1e-10)) return direct * soilReflectance + diffuseFraction * soilReflectance;
    const double relative = std::fmod(viewAzimuth - sunAzimuth + 540.0, 360.0) - 180.0;
    FourFractions parts = canopyFractions(lai, hotspot, sunZenith, viewZenith, std::abs(relative));
    if (!sunlit) {
        parts = FourFractions{0.0, parts.soilSun + parts.soilShade, 0.0, parts.leafSun + parts.leafShade};
    }
    const auto coefficients = sphericalCoefficients(std::min(sunZenith, 89.9), std::min(viewZenith, 89.9), std::abs(relative));
    const double single = direct * ((coefficients.sob * leafReflectance + coefficients.sof * leafTransmittance) /
                                        std::max(coefficients.ko, 1e-9) * parts.leafSun +
                                    soilReflectance * parts.soilSun);
    const HomCanopy nodes(lai);
    double multiple = 0.0;
    if (direct > 0.0 && sunZenith < 90.0) {
        const auto beam = homBeamMultiple(nodes, soilReflectance, leafReflectance, leafTransmittance,
                                          sunZenith, viewZenith);
        multiple += direct * (beam.canopy + beam.soil);
    }
    if (diffuseFraction > 0.0) {
        const auto sky = homSkyMultiple(nodes, soilReflectance, leafReflectance, leafTransmittance, viewZenith);
        multiple += diffuseFraction * (sky.canopy + sky.soil);
    }
    return single + multiple;
}

}  // namespace stream
