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
template <class T>
T twoLayerSunlitLeaf(const T& lai, const T& viewGap, const T& sunGap, const T& sunlitSoil)
{
    const T pv = ad::clamp(viewGap, 1e-12, 1.0);
    const T ps = ad::clamp(sunGap, 1e-12, 1.0);
    const T pss = ad::max(sunlitSoil, 1e-300);
    const T kv = -ad::log(pv) / lai;
    const T ks = -ad::log(ps) / lai;
    const T overlap = ad::clamp(ad::log(pss / (pv * ps)) / lai, T(0.0), ad::min(kv, ks));
    const T index = ad::max(0.82 - 0.08 * lai, 0.02);
    const T upper = ad::sqrt((-ad::log(T(1.0) - (T(1.0) - pv) * index) / ad::max(kv * lai, 1e-12)) *
                             (-ad::log(T(1.0) - (T(1.0) - ps) * index) / ad::max(ks * lai, 1e-12)));
    const T laiUpper = lai * upper;
    const T laiBottom = lai * (T(1.0) - upper);
    const T litBackground = ad::exp(-(ks + kv - overlap) * laiUpper);
    const T litBottom = T(1.0) - ad::exp(-overlap * laiBottom);
    return ad::clamp(T(1.0) - ad::exp(-kv * laiUpper) + litBottom * litBackground, T(0.0), T(1.0) - pv);
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

template <class T>
FourFractionsT<T> canopyFractionsT(const T& lai, double hotspot, double sunZenith, double viewZenith,
                                   double relativeAzimuth)
{
    FourFractionsT<T> result;
    if (!(lai > 0.0)) { result.soilSun = T(1.0); return result; }
    const double muS = std::max(cosine(sunZenith), 1e-6), muV = std::max(cosine(viewZenith), 1e-6);
    const double ks = kG / muS, kv = kG / muV;
    const T gs = ad::exp(-ks * lai), gv = ad::exp(-kv * lai);
    const T ts = ks * lai, tv = kv * lai;
    const double correlation = leafCorrelation(hotspot, sunZenith, viewZenith, relativeAzimuth);
    const T shared = ad::min(ad::min(tv, ts), ad::sqrt(tv * ts) * correlation);
    const T joint = ad::exp(-(ts + tv - shared));  // sunlit soil
    const T leafSun = twoLayerSunlitLeaf<T>(lai, gv, gs, joint);
    result.soilSun = ad::clamp(joint, T(0.0), gv);
    result.soilShade = ad::max(0.0, gv - result.soilSun);
    result.leafSun = ad::clamp(leafSun, T(0.0), ad::max(0.0, T(1.0) - gv));
    result.leafShade = ad::max(0.0, T(1.0) - gv - result.leafSun);
    return result;
}

FourFractions canopyFractions(double lai, double hotspot, double sunZenith, double viewZenith, double relativeAzimuth)
{
    return canopyFractionsT<double>(lai, hotspot, sunZenith, viewZenith, relativeAzimuth);
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

template <class T>
T canopyReflectanceT(const T& lai, double hotspot, double sunZenith, double sunAzimuth, double viewZenith,
                     double viewAzimuth, double leafReflectance, double leafTransmittance, const T& soilReflectance,
                     double diffuseFraction, bool sunlit)
{
    const double direct = sunlit ? 1.0 - diffuseFraction : 0.0;
    if (!(lai > 1e-10)) return direct * soilReflectance + diffuseFraction * soilReflectance;
    const double relative = std::fmod(viewAzimuth - sunAzimuth + 540.0, 360.0) - 180.0;
    FourFractionsT<T> parts = canopyFractionsT<T>(lai, hotspot, sunZenith, viewZenith, std::abs(relative));
    if (!sunlit) {
        parts = FourFractionsT<T>{T(0.0), parts.soilSun + parts.soilShade, T(0.0), parts.leafSun + parts.leafShade};
    }
    const auto coefficients = sphericalCoefficients(std::min(sunZenith, 89.9), std::min(viewZenith, 89.9), std::abs(relative));
    const T single = direct * ((coefficients.sob * leafReflectance + coefficients.sof * leafTransmittance) /
                                   std::max(coefficients.ko, 1e-9) * parts.leafSun +
                               soilReflectance * parts.soilSun);
    const HomCanopyT<T> nodes(lai);
    T multiple(0.0);
    if (direct > 0.0 && sunZenith < 90.0) {
        const auto beam = homBeamMultiple<T>(nodes, soilReflectance, leafReflectance, leafTransmittance,
                                             sunZenith, viewZenith);
        multiple += direct * (beam.canopy + beam.soil);
    }
    if (diffuseFraction > 0.0) {
        const auto sky = homSkyMultiple<T>(nodes, soilReflectance, leafReflectance, leafTransmittance, viewZenith);
        multiple += diffuseFraction * (sky.canopy + sky.soil);
    }
    return single + multiple;
}

double canopyReflectance(double lai, double hotspot, double sunZenith, double sunAzimuth, double viewZenith,
                         double viewAzimuth, double leafReflectance, double leafTransmittance,
                         double soilReflectance, double diffuseFraction, bool sunlit)
{
    return canopyReflectanceT<double>(lai, hotspot, sunZenith, sunAzimuth, viewZenith, viewAzimuth, leafReflectance,
                                      leafTransmittance, soilReflectance, diffuseFraction, sunlit);
}

template FourFractionsT<Ad> canopyFractionsT<Ad>(const Ad&, double, double, double, double);
template Ad canopyReflectanceT<Ad>(const Ad&, double, double, double, double, double, double, double, const Ad&,
                                   double, bool);

}  // namespace stream
