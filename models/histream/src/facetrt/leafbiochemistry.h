// ==========================================================================
// 文件：leafbiochemistry.h（facetrt）
// 功能：FacetEB 叶片光合–气孔模型的 Farquhar（C3）/ von Caemmerer（C4）版本，并与 Ball-Berry 气孔导度耦合，
//   对应 SCOPE 1.73 的 biochemical_MD12.m 与 VoxelEB 的 biochemical_m12.comp。
// 主要模块：
//   1. LeafBiochemistry：叶片生理参数（vcmax25、Ball-Berry 斜率/截距、C3/C4 类型、荧光参数等）
//   2. FarquharResult：结果（总/净同化、胞间 CO2、整叶水汽阻抗）
//   3. farquhar：输入检查 → 温度响应 → 电子传输 → Ci 估计 → 同化 → 导度/阻抗
// 数据流：facetrtcore.cpp 在 vegetationTemperatureMethod 不为 0 时，用叶温、吸收的 PAR、水汽压
//   算出的相对湿度、气压和 CO2 调用 farquhar，取其 resistance 作为叶片气孔阻抗。
// 注意：温度 K；吸收 PAR umol photons m-2 s-1；气压 hPa；相对湿度 [0,1]；相对深度 <= 0
//   （冠顶为 0，向下为负，限幅在 [-1, 0]）；MD12 自带温度响应，Tcor/Tparam 只作用于 Collatz。
// ==========================================================================

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace facetbio {

// 模块：叶片生理参数（缺省值见下）。vcmax25：25 °C 最大羧化速率（umol m-2 s-1）；
// ballBerrySlope / ballBerry0：Ball-Berry 斜率 m 与残余导度 g0；rdParam：暗呼吸占 vcmax 的比例；
// kV：vcmax 随冠层相对深度指数衰减的系数；stressFactor：水分胁迫因子 [0,1]；type：3 = C3，4 = C4；
// temperatureCorrection、tparam：仅 Collatz 用的温度修正与温度响应参数；annualTemperature：年均温，
//  用于 C3 的 Vcmax/Jmax 熵项（单位待核对）；beta：吸收光中分配给 PSII 的份额；
// qLs：PSII 活性/开放份额 [0,1]；sustainedNPQ：持续非光化学淬灭。
struct LeafBiochemistry {
    float vcmax25{60.0f};
    float ballBerrySlope{8.0f};
    float ballBerry0{0.01f};
    float rdParam{0.015f};
    float kV{0.6396f};
    float stressFactor{1.0f};
    int type{3};
    int temperatureCorrection{0}; // Collatz only; MD12 always corrects temperature.
    std::array<float, 5> tparam{0.2f, 0.3f, 288.0f, 313.0f, 328.0f};
    float annualTemperature{25.0f};
    float beta{0.507f};
    float qLs{1.0f};
    float sustainedNPQ{0.0f};
};

// 模块：farquhar 的结果。grossAssimilation 总同化、netAssimilation 净同化（umol CO2 m-2 s-1）；
// intercellularCO2 胞间 CO2（ppm）；resistance 整叶水汽阻抗（s m-1），范围 [1, 625000]。
struct FarquharResult {
    double grossAssimilation; // umol CO2 m-2 leaf s-1
    double netAssimilation;
    double intercellularCO2; // ppm
    float resistance; // whole-leaf water-vapour resistance, s m-1
};

// 作用：给定叶温、吸收 PAR、相对湿度、气压、冠层相对深度与 CO2，求同化速率与气孔阻抗。
// 步骤：1 参数合法性检查；2 单位换算与温度响应；3 电子传输速率；4 由 Ball-Berry 估计 Ci；
// 5 C3 / C4 的总同化；6 净同化与导度、阻抗。输入非法时抛 std::runtime_error。
// SCOPE 1.73 biochemical_MD12.m / VoxelEB biochemical_m12.comp:
// Farquhar C3 and von Caemmerer C4, with Ball-Berry stomatal coupling.
// Temperature K, absorbed PAR umol photons m-2 s-1, pressure hPa, RH [0,1].
// MD12 has an intrinsic temperature response; Tcor/Tparam control Collatz only.
inline FarquharResult farquhar(const LeafBiochemistry& leaf, double temperature,
    double absorbedPar, double relativeHumidity, double pressure,
    double relativeDepth, double co2Ppm)
{
    // 步骤 1：检查输入与参数合法（有限、非负，且 RH、胁迫、beta、qLs 不超过 1，类型只能是 3 或 4）。
    const auto nonnegative = [](double x) { return std::isfinite(x) && x >= 0.0; };
    if (!std::isfinite(temperature) || temperature <= 0.0 ||
        !std::isfinite(pressure) || pressure <= 0.0 ||
        !nonnegative(absorbedPar) || !nonnegative(co2Ppm) ||
        !nonnegative(relativeHumidity) || relativeHumidity > 1.0 ||
        !std::isfinite(relativeDepth) || !nonnegative(leaf.vcmax25) ||
        !nonnegative(leaf.ballBerrySlope) || !nonnegative(leaf.ballBerry0) ||
        !nonnegative(leaf.rdParam) || !nonnegative(leaf.kV) ||
        !nonnegative(leaf.stressFactor) || leaf.stressFactor > 1.0 ||
        !nonnegative(leaf.beta) || leaf.beta > 1.0 ||
        !nonnegative(leaf.qLs) || leaf.qLs > 1.0 ||
        !nonnegative(leaf.sustainedNPQ) || !std::isfinite(leaf.annualTemperature) ||
        (leaf.type != 3 && leaf.type != 4))
        throw std::runtime_error("Invalid FacetEB Farquhar leaf/environment parameters");

    // 步骤 2：单位换算与温度响应。ppmToBar：ppm 乘总压（Pa）换成分压（bar）；氧分压按 20.9%；
    // vcmo 为按冠层深度衰减后的 vcmax25；arrhenius 为 Arrhenius 型因子，peaked 为带高温失活的峰形因子；
    // 据此得 rd（暗呼吸）、vcmax、jmax、Rubisco 常数 kc/ko 与特异性 specificity。
    constexpr double reference = 298.15, gasConstant = 8.31;
    const bool c4 = leaf.type == 4;
    const double pressurePa = pressure * 100.0;
    const double ppmToBar = pressurePa * 1.0e-11;
    const double cs = co2Ppm * ppmToBar, oxygen = 209.0 * pressurePa * 1.0e-8;
    const double vcmo = leaf.vcmax25 * std::exp(leaf.kV * std::clamp(relativeDepth, -1.0, 0.0));
    const double rt = gasConstant * temperature / 1000.0;
    const double rtRef = gasConstant * reference / 1000.0;
    const auto arrhenius = [&](double activation) {
        return std::exp(activation * (temperature - reference) / (reference * rt));
    };
    const auto peaked = [&](double activation, double deactivation, double entropy) {
        return arrhenius(activation) *
            (1.0 + std::exp((reference * entropy - deactivation) / rtRef)) /
            (1.0 + std::exp((temperature * entropy - deactivation) / rt));
    };
    const double rd = leaf.rdParam * vcmo * arrhenius(46.39);
    const double specificity = 2862.0 / arrhenius(37.83);
    const double vcmax = vcmo * (c4 ? peaked(67.29, 144.57, 0.472)
        : peaked(71.51, 200.0, (-1.07 * leaf.annualTemperature + 668.0) / 1000.0));
    const double jmax = vcmo * (c4 ? (40.0 / 6.0) * peaked(77.9, 191.9, 0.627)
        : 2.68 * peaked(49.88, 200.0, (-0.75 * leaf.annualTemperature + 660.0) / 1000.0));
    const double qt = (temperature - reference) / 10.0;
    const double kc = (c4 ? 944.0 * std::pow(2.1, qt) : 404.9 * arrhenius(79.43)) * ppmToBar;
    const double ko = (c4 ? 633.0 * std::pow(1.2, qt) : 278.4 * arrhenius(36.38)) * pressurePa * 1.0e-8;

    // 步骤 3：电子传输速率（含叶绿素荧光相关速率常数 kf、kd、kPSII 和 NPQ）：
    // 用非直角双曲线（曲率 theta）取较小根，由激发速率 excitation 与容量 jmax·qLs 得 electronTransport；
    // 写成“有理化的较小根”形式以覆盖 qLs = 0、黑暗和 theta = 0 的情形。
    constexpr double kf = 3.0e7, kd = 1.0e8, kPSII = (kd + kf) * 0.88 / 0.12;
    const double kps = kPSII * leaf.qLs;
    const double denominator = kps + kf + kd + leaf.sustainedNPQ * (kf + kd);
    const double theta = (kps - 1.95e8 * leaf.qLs) / denominator;
    const double excitation = leaf.beta * absorbedPar * kps / denominator;
    const double capacity = jmax * leaf.qLs;
    // Rationalized smaller root also handles qLs=0, darkness and theta=0.
    const double sum = excitation + capacity;
    const double electronTransport = sum > 0.0 ? 2.0 * excitation * capacity /
        (sum + std::sqrt(std::max(0.0, sum * sum - 4.0 * theta * excitation * capacity))) : 0.0;
    // 步骤 4：Ci 估计。由 Ball-Berry 与扩散比 1.6 得 Ci = Cs·(1 - 1.6 / (m·RH·胁迫))，
    // 下限为 C3 的补偿点 gamma 或 C4 的 9.9 ppm（对应 bar）。
    const double gamma = 0.5 * oxygen / specificity;
    const double lowerCi = c4 ? 9.9 * ppmToBar : gamma;
    const double product = leaf.ballBerrySlope * relativeHumidity * leaf.stressFactor;
    const double ci = product > 0.0 ? std::max(lowerCi, cs * (1.0 - 1.6 / product)) : lowerCi;

    // 步骤 5：总同化。C3：取 Rubisco 限制与光（电子传输）限制中的较小者再乘 (1 - gamma/Ci)；
    // C4：用完整 C4 方程（MD12，alpha = 0、x = 0.4），Ac、Aj 各为一个二次方程的较小根，取二者较小者再加 rd。
    // 无光、无电子传输、无 Ball-Berry 驱动或 Cs <= 0 时总同化为 0。
    double gross = 0.0;
    if (absorbedPar > 0.0 && electronTransport > 0.0 && product > 0.0 && cs > 0.0) {
        if (!c4) {
            const double rubisco = vcmax * ci / (ci + kc * (1.0 + oxygen / ko));
            const double light = electronTransport * ci / (4.5 * ci + 10.5 * gamma);
            gross = (1.0 - gamma / ci) * std::min(rubisco, light);
        } else {
            const double vpmax = vcmo * 2.33 * peaked(70.37, 117.93, 0.376);
            const double kp = 82.0 * std::pow(2.1, qt) * ppmToBar;
            const double vp = std::min(vpmax * ci / (ci + kp), 80.0);
            const double gbs = (0.0207 * vcmo + 0.4806) * 1000.0;
            const double gam = 0.5 / specificity;
            // Complete C4 equations with MD12's alpha=0 and x=0.4.
            const double supply = vp - 0.5 * rd + gbs * ci;
            const double carboxylation = vcmax - rd;
            const double michaelis = gbs * kc * (1.0 + oxygen / ko);
            const double lightSupply = 0.2 * electronTransport - 0.5 * rd + gbs * ci;
            const double lightCapacity = 0.2 * electronTransport;
            const auto smallerRoot = [](double b, double c) {
                const double disc = b * b - 4.0 * c;
                if (disc < -1.0e-12 * (b * b + std::abs(4.0 * c)))
                    throw std::runtime_error("Invalid FacetEB C4 assimilation discriminant");
                const double root = std::sqrt(std::max(0.0, disc));
                return b < 0.0 ? (b == root ? 0.0 : 2.0 * c / (-b + root)) : (-b - root) / 2.0;
            };
            const double ac = smallerRoot(-(supply + carboxylation + michaelis),
                carboxylation * supply - gam * vcmax * gbs * oxygen + rd * michaelis);
            const double aj = smallerRoot(-(lightSupply + lightCapacity - rd + gbs * gam * oxygen * 7.0 / 3.0),
                lightSupply * (lightCapacity - rd) - gbs * gam * oxygen * (lightCapacity + rd * 7.0 / 3.0));
            gross = std::min(ac, aj) + rd;
        }
    }
    // 步骤 6：净同化 = 总同化 - rd。导度 = 残余导度 g0（换算为速度单位）+ 由同化驱动的部分：
    // CO2 阻抗 = (Cs - Ci)/net，乘 0.625（= 1/1.6，水汽/CO2 扩散比）后经单位换算得水汽阻抗；
    // 总阻抗 = 1/导度，限幅在 [1, 625000] s m-1（导度为 0 时取上限）。
    const double net = gross - rd;
    // Retain the configured g0 as residual conductance, as in FacetEB Collatz.
    double conductance = leaf.ballBerry0 * 8.314 * temperature / pressurePa;
    if (net > 0.0 && cs > ci) {
        const double resistance = 0.625 * (cs - ci) / net *
            1.2047 / 28.96 * 1.0e3 * 1.0e6 / pressurePa * 1.0e5;
        conductance += 1.0 / resistance;
    }
    if (!std::isfinite(gross) || !std::isfinite(net) || !std::isfinite(ci) ||
        !nonnegative(conductance))
        throw std::runtime_error("Nonfinite FacetEB Farquhar result");
    const double resistance = conductance > 0.0 ? std::clamp(1.0 / conductance, 1.0, 625000.0) : 625000.0;
    return {std::max(0.0, gross), net, ci / ppmToBar, static_cast<float>(resistance)};
}

} // namespace facetbio
