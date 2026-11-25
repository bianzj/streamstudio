// ==========================================================================
// 文件：canopy.cpp
// 功能：均匀冠层的标准闭合：四组分（光照/阴影土壤与叶片）可见面积比例、一次散射系数
//       与冠层顶方向反射率（一次散射 + hom3 多次散射）
// 主要模块：
//   1. leafCorrelation：双尺度热点的叶片尺度相关（row_closure.leaf_correlation）
//   2. twoLayerSunlitLeaf：可见光照叶片比例的两层近似（hotspot.two_layer_sunlit_leaf，c = 0）
//   3. gauss64 / volumeScattering：高斯积分节点与单叶倾角的体散射函数（rt_transfer.volscatt）
//   4. canopyFractionsT / canopyFractions：四组分比例（模板，可对 LAI 自动微分）
//   5. sphericalCoefficients：球面叶片分布的一次散射系数 ks、ko、sob、sof
//   6. canopyReflectanceT / canopyReflectance：冠层方向反射率（BRF/HDRF）
// 数据流：app/image.cpp 与 energy.cpp 取四组分比例；光学成像取 canopyReflectance；
//         多次散射部分由 radiosity.h 的 hom3 节点辐射度提供
// 约定：角度均为度；方位差取 [0, 180]；叶倾角分布为球面（G = 0.5）；
//       模板版本可同时用 double 与 Ad（dual.h），分支与限幅按数值走，导数取当前分支
// 来源：对应 TiRT 的 canopy_closure、hotspot、row_closure、vnir（见 PORTING.md 模块对照）
// ==========================================================================

#include "canopy.h"
#include "constants.h"
#include "radiosity.h"

#include <algorithm>
#include <cmath>

namespace stream {
namespace {

// 常数：叶面积投影函数 G（球面叶倾角分布时与方向无关，取 0.5），消光系数 k = G / cos(天顶角)。
constexpr double kG = 0.5;

// 工具：天顶角限幅到 89.999°，避免 tan/cos 在地平线处发散；cosine 为限幅后的余弦。
double zenith(double angle) { return std::min(std::abs(angle), 89.999); }
double cosine(double angle) { return std::cos(zenith(angle) * kPi / 180.0); }

// 模块：热点相关系数（0-1），描述太阳方向与观测方向的光线路径相关性。
// 输入：hotspot（热点尺度参数，无量纲）、太阳/观测天顶角与相对方位（度）。
// 说明：separation 为两方向在 tan 空间的几何距离，二者重合（separation = 0）时
//       返回 1（完全相关，热点中心）；hotspot <= 0 表示无热点，返回 0；
//       否则取 (1 - exp(-a)) / a，a 随角距离增大而增大，相关性随之衰减。
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

// 模块：观测方向可见的光照叶片比例（两层近似）。
// 输入：lai、观测方向间隙 viewGap、太阳方向间隙 sunGap、可见光照土壤比例 sunlitSoil。
// 说明：步骤 1 由间隙率反求各方向的有效消光 kv、ks，并由联合间隙得到两方向的重叠 overlap；
//       步骤 2 把冠层分为上层（占比 upper，来自 index 加权的间隙对数比）与下层，
//       上层光照叶片按 1 - exp(-kv·LAIupper) 计，下层再加上重叠带来的补充；
//       各处 1e-12 / 1e-300 的下限仅用于避免 log(0) 与除零，结果限幅在 [0, 1 - viewGap]。
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

// 模块：64 点 Gauss-Legendre 求积的节点与权重（首次调用时一次性计算并缓存）。
// 说明：以余弦近似为初值，对 64 阶 Legendre 多项式做 Newton 迭代求根（至多 100 次，
//       步长 < 1e-15 停止），权重 = 2 / ((1 - x²)·P'(x)²)。
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

// 注：这是 SAIL 类模型中 Verhoef 的体散射函数；输出 forward / backward 为该叶倾角
//     tilt（度）下的前向/后向散射因子（未乘叶片反射/透射率，已含 1/(2π²) 归一化）。
//     chiS、chiO 在上游算法中是另外的输出量，此处计算后未使用（(void) 抑制警告）。
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

// 模块：四组分（光照土壤、阴影土壤、光照叶片、阴影叶片）在观测方向上的可见面积比例。
// 输入：lai、hotspot、太阳/观测天顶角与相对方位（度）；输出四个比例，总和为 1（有叶时）。
// 说明：lai <= 0 时全部为光照土壤。对 LAI 可微（T = Ad）。
template <class T>
FourFractionsT<T> canopyFractionsT(const T& lai, double hotspot, double sunZenith, double viewZenith,
                                   double relativeAzimuth)
{
    FourFractionsT<T> result;
    if (!(lai > 0.0)) { result.soilSun = T(1.0); return result; }
    // 步骤 1：各方向消光系数与间隙率（gs：太阳方向、gv：观测方向，Beer 定律）。
    const double muS = std::max(cosine(sunZenith), 1e-6), muV = std::max(cosine(viewZenith), 1e-6);
    const double ks = kG / muS, kv = kG / muV;
    const T gs = ad::exp(-ks * lai), gv = ad::exp(-kv * lai);
    const T ts = ks * lai, tv = kv * lai;
    // 步骤 2：太阳、观测两方向光学厚度的重叠 shared（不超过较小者、也不超过几何平均×热点相关），
    //         联合透过率 joint = exp(-(ts + tv - shared)) 即观测可见的光照土壤比例。
    const double correlation = leafCorrelation(hotspot, sunZenith, viewZenith, relativeAzimuth);
    const T shared = ad::min(ad::min(tv, ts), ad::sqrt(tv * ts) * correlation);
    const T joint = ad::exp(-(ts + tv - shared));  // sunlit soil
    // 步骤 3：由两层近似得到光照叶比例；其余按间隙守恒分配：
    //         可见土壤 = gv（光照 + 阴影），可见叶 = 1 - gv（光照 + 阴影）。
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

// 模块：球面叶倾角分布的一次散射系数（TiRT vnir.spherical_coefficients）。
// 输出：ks、ko 为太阳/观测方向消光系数（= 0.5 / cos）；sob、sof 为后向/前向散射系数。
// 说明：球面分布下叶倾角余弦 mu 均匀分布，故在 mu ∈ [0, 1] 上用 64 点 Gauss-Legendre
//       对 volumeScattering 积分；cos 下限 1e-6 避免掠射方向除零。
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

// 模块：冠层顶方向反射率（BRF；全漫射时即 HDRF），一次散射 + hom3 多次散射。
// 输入：lai、hotspot、几何（度）、叶片反射率/透射率、土壤反射率、diffuseFraction（天空漫射份额）、
//       sunlit（true：直射 + 漫射照明；false：只算直射被遮挡的阴影组分，直射贡献为 0）。
// 说明：对 LAI 与 soilReflectance 可微（土壤水分的导数通过 soilReflectance 传入）。
template <class T>
T canopyReflectanceT(const T& lai, double hotspot, double sunZenith, double sunAzimuth, double viewZenith,
                     double viewAzimuth, double leafReflectance, double leafTransmittance, const T& soilReflectance,
                     double diffuseFraction, bool sunlit)
{
    // 步骤 1：直射权重 direct；LAI 趋近 0 时直接返回土壤反射率（直射+漫射权重之和为 1）。
    const double direct = sunlit ? 1.0 - diffuseFraction : 0.0;
    if (!(lai > 1e-10)) return direct * soilReflectance + diffuseFraction * soilReflectance;
    // 步骤 2：相对方位折到 [-180, 180) 后取绝对值（反射率关于主平面对称）；
    //         阴影组分（sunlit = false）把光照/阴影的土壤与叶片比例合并到阴影项。
    const double relative = std::fmod(viewAzimuth - sunAzimuth + 540.0, 360.0) - 180.0;
    FourFractionsT<T> parts = canopyFractionsT<T>(lai, hotspot, sunZenith, viewZenith, std::abs(relative));
    if (!sunlit) {
        parts = FourFractionsT<T>{T(0.0), parts.soilSun + parts.soilShade, T(0.0), parts.leafSun + parts.leafShade};
    }
    // 步骤 3：一次散射项 = 直射 × （叶片散射 / ko × 光照叶比例 + 土壤反射 × 光照土壤比例）；
    //         天顶角限幅 89.9° 避免掠射奇异。
    const auto coefficients = sphericalCoefficients(std::min(sunZenith, 89.9), std::min(viewZenith, 89.9), std::abs(relative));
    const T single = direct * ((coefficients.sob * leafReflectance + coefficients.sof * leafTransmittance) /
                                   std::max(coefficients.ko, 1e-9) * parts.leafSun +
                               soilReflectance * parts.soilSun);
    // 步骤 4：多次散射项来自 hom3 节点辐射度（radiosity）：直射束（太阳天顶角 < 90°）与天空漫射两部分，
    //         分别按 direct、diffuseFraction 加权。
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

// 显式实例化：把自动微分版本编译进本文件，使 header 中只有声明时其他文件也能链接。
template FourFractionsT<Ad> canopyFractionsT<Ad>(const Ad&, double, double, double, double);
template Ad canopyReflectanceT<Ad>(const Ad&, double, double, double, double, double, double, double, const Ad&,
                                   double, bool);

}  // namespace stream
