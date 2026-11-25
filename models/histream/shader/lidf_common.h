// ==========================================================================
// 文件：lidf_common.h
// 功能：叶倾角分布（LIDF）的叶片投影函数 G 与双朗伯面积散射相函数 Gamma，
//       GLSL 4.60 / C++17 共用代码（原作者的详细约定见下方原注释）。
// 主要模块：
//   1. 语言适配宏（LIDF_CONST / LIDF_FN / LIDF_ARRAY / LIDF_OUT / LIDF_TABLE）
//   2. 13 个 SAIL 倾角类的中心角 cos/sin 表
//   3. lidfClassProjection / lidfProjection：单类与 LIDF 加权的叶片投影 G(mu)
//   4. lidfVolscatt：SAIL volscatt 核（单倾角类的方位积分，输出 frho、ftau）
//   5. lidfGamma：对 13 类加权求 GammaR、GammaT（面积散射相函数）
//   6. （仅 C++ 宿主端）sailCumulative / sailLeafAngleFrequencies：由 Verhoef 双参数
//      (a, b) 计算 13 类频率
// 数据流：宿主端用 sailLeafAngleFrequencies 填写每个冠层的 13 个频率 → 着色器端
//         用 lidfProjection / lidfGamma 求消光与散射；tools/test-lidf-gamma.cpp 用同一代码做数值校验。
// 与其他文件关系：被 VoxelRT/VoxelEB 着色器包含；Canopy.diffuseG 等由本文件函数计算（待核对）。
// 约定：角度 deg 仅用于宿主端 SAIL 累积函数，其余为 rad / 余弦；方向向量为单位向量，+y 向上；
//       lidf[i] 为第 i 类单面叶面积占比（总和 1）。
// ==========================================================================

// Leaf inclination distribution (LIDF), leaf projection function G and
// bi-Lambertian area scattering phase function Gamma for turbid foliage.
//
// This file is written in the common subset of GLSL 4.60 and C++17 and is
// included by the VoxelRT/VoxelEB shaders, by the host (which fills the 13
// SAIL class frequencies of every canopy) and by tools/test-lidf-gamma.cpp,
// so the numerical check runs exactly the code the GPU executes.
//
// Conventions (Ross 1981; Verhoef SAIL):
//   * 13 SAIL inclination classes with centres 5,15,...,75,81,83,85,87,89 deg
//     (class edges 0,10,...,80,82,84,86,88,90 deg); lidf[i] is the fraction
//     of one-sided leaf area in class i, sum 1.
//   * G(mu) = sum_i lidf[i] * psi(theta_Li, theta), the mean projection of
//     unit leaf area onto a plane normal to a direction with |cos zenith| mu.
//     For every LIDF the mean of G over the sphere is exactly 1/2.
//   * Gamma(s -> o) = rho * GammaR + tau * GammaT with
//       GammaR/T = (1/2pi) int g_L(n) |s.n| |o.n| [reflection/transmission] dn,
//     s pointing towards the source and o towards the observer, so that
//       (1/pi) int_4pi Gamma dOmega_o = (rho + tau) * G(s).
//     A spherical LIDF gives the Ross closed form
//       Gamma = (rho+tau)/(3 pi)(sin a - a cos a) + tau/3 cos a,
//     a being the angle between incident and scattered propagation.
//   * The azimuthal integral of one inclination class is the analytic SAIL
//     "volscatt" kernel (Verhoef 1998, PROSAIL), GammaR/T_i = pi * frho/ftau.
#ifndef LIDF_COMMON_H
#define LIDF_COMMON_H

// SAIL 倾角类个数（中心 5,15,...,75,81,83,85,87,89°）。
#define LIDF_CLASS_COUNT 13

// 模块：语言适配。C++ 下进入 namespace lidf 并把数组/输出参数/常量表映射为 C++ 写法；
//       GLSL 下用 out 参数与 float[] 构造。这样同一份源码在两种语言中编译。
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
namespace lidf {
using std::abs;
using std::acos;
using std::cos;
using std::max;
using std::min;
using std::sin;
using std::sqrt;
#define LIDF_CONST static const
#define LIDF_FN inline
#define LIDF_ARRAY(name) const float name[LIDF_CLASS_COUNT]
#define LIDF_OUT(type) type&
#define LIDF_TABLE(name, values) static const float name[LIDF_CLASS_COUNT] = {values}
#else
#define LIDF_CONST const
#define LIDF_FN
#define LIDF_ARRAY(name) float name[LIDF_CLASS_COUNT]
#define LIDF_OUT(type) out type
#define LIDF_TABLE(name, values) const float name[LIDF_CLASS_COUNT] = float[LIDF_CLASS_COUNT](values)
#endif

LIDF_CONST float LIDF_PI = 3.14159265358979f;

// 模块：13 个倾角类中心角的 cos/sin 查表（常量，避免着色器里重复调用三角函数）。
// cos/sin of the SAIL class centres 5,15,...,75,81,83,85,87,89 deg.
#define LIDF_COS_VALUES 0.99619470f, 0.96592583f, 0.90630779f, 0.81915204f, \
    0.70710678f, 0.57357644f, 0.42261826f, 0.25881905f, 0.15643447f, \
    0.12186934f, 0.08715574f, 0.05233596f, 0.01745241f
#define LIDF_SIN_VALUES 0.08715574f, 0.25881905f, 0.42261826f, 0.57357644f, \
    0.70710678f, 0.81915204f, 0.90630779f, 0.96592583f, 0.98768834f, \
    0.99254615f, 0.99619470f, 0.99862953f, 0.99984770f
LIDF_TABLE(LIDF_CLASS_COS, LIDF_COS_VALUES);
LIDF_TABLE(LIDF_CLASS_SIN, LIDF_SIN_VALUES);

// 作用：单个倾角类对天顶余弦 mu 方向的平均投影（SAIL 的 chi_s）。
// 说明：叶面不会翻到与该方向侧向（cs >= ss）时就是 cosL·mu；否则用 Ross 闭式。
// Projection of one inclination class (cosL, sinL) for a direction with
// |cos zenith| mu (SAIL chi_s). Equals cosL*mu when the leaf never turns
// edge-on to the direction, otherwise the Ross closed form.
LIDF_FN float lidfClassProjection(float cosL, float sinL, float mu)
{
    float sinT = sqrt(max(1.0f - mu * mu, 0.0f));
    float cs = cosL * mu;
    float ss = sinL * sinT;
    if (ss <= 1.0e-6f || cs >= ss) return cs;
    float beta = acos(-cs / ss);
    return 2.0f / LIDF_PI * ((beta - 0.5f * LIDF_PI) * cs + sin(beta) * ss);
}

// 作用：LIDF 加权的叶片投影 G(mu) = Σ frequency[i]·psi_i；mu 取绝对值并限到 [0,1]。
LIDF_FN float lidfProjection(LIDF_ARRAY(frequency), float mu)
{
    float m = min(abs(mu), 1.0f);
    float g = 0.0f;
    for (int i = 0; i < LIDF_CLASS_COUNT; ++i)
        g += frequency[i] * lidfClassProjection(LIDF_CLASS_COS[i], LIDF_CLASS_SIN[i], m);
    return g;
}

// 作用：SAIL volscatt（Verhoef 1998）：一个倾角类在给定太阳/观测几何下，
//       对方位角积分后的反射（frho）与透射（ftau）权重。
// 输入：cts/sts、cto/sto（源/观测方向天顶角的 cos/sin）、cospsi/psi（相对方位）、
//       costl/sintl（叶倾角 cos/sin）。
// SAIL volscatt for one class: bidirectional reflection (frho) and
// transmission (ftau) weights. cts >= 0 (source above the horizon), cto is
// signed (cto < 0: observer below the horizon), psi = relative azimuth in
// [0, pi] between the source and observer directions.
LIDF_FN void lidfVolscatt(float cts, float sts, float cto, float sto, float cospsi, float psi,
                  float costl, float sintl, LIDF_OUT(float) frho, LIDF_OUT(float) ftau)
{
    float cs = costl * cts;
    float co = costl * cto;
    float ss = sintl * sts;
    float so = sintl * sto;
    // 步骤 1：求叶面“翻转角” bts/bto（acos(-cs/ss)）；|cosbt|>=1（叶不会翻转）时用哨兵值 5，
    //       并取 bts/bto 为 π 或 0 的极限形式（对应 ds/doo 的不同取值）。
    float cosbts = 5.0f;
    if (abs(ss) > 1.0e-6f) cosbts = -cs / ss;
    float cosbto = 5.0f;
    if (abs(so) > 1.0e-6f) cosbto = -co / so;
    float bts = LIDF_PI;
    float ds = cs;
    if (abs(cosbts) < 1.0f) {
        bts = acos(cosbts);
        ds = ss;
    }
    float bto = LIDF_PI;
    float doo = co;
    if (abs(cosbto) < 1.0f) {
        bto = acos(cosbto);
        doo = so;
    } else if (cto <= 0.0f) {
        bto = 0.0f;
        doo = -co;
    }
    // 步骤 2：把方位角 psi 与过渡角 btran1/btran2 比较并排序成 bt1<=bt2<=bt3。
    float btran1 = abs(bts - bto);
    float btran2 = LIDF_PI - abs(bts + bto - LIDF_PI);
    float bt1 = btran1;
    float bt2 = psi;
    float bt3 = btran2;
    if (psi <= btran1) {
        bt1 = psi;
        bt2 = btran1;
    } else if (psi > btran2) {
        bt2 = btran2;
        bt3 = psi;
    }
    // 步骤 3：按 SAIL 公式合成 t1、t2，frho/ftau 除以 2π²，并截断为非负（数值上的微小负值置 0）。
    float t1 = 2.0f * cs * co + ss * so * cospsi;
    float t2 = 0.0f;
    if (bt2 > 0.0f) t2 = sin(bt2) * (2.0f * ds * doo + ss * so * cos(bt1) * cos(bt3));
    float denom = 2.0f * LIDF_PI * LIDF_PI;
    frho = max(((LIDF_PI - bt2) * t1 + t2) / denom, 0.0f);
    ftau = max((-bt2 * t1 + t2) / denom, 0.0f);
}

// 作用：LIDF 冠层的面积散射相函数。输入：LIDF 频率、指向光源的单位向量 s、指向观测者的单位向量 o；
// 输出：gammaR、gammaT（Gamma = rho·GammaR + tau·GammaT）。
// Area scattering phase function of a LIDF canopy. towardSource (s) and
// towardObserver (o) are unit vectors, +y is up. Returns GammaR and GammaT
// (Gamma = rho * GammaR + tau * GammaT) in the Ross normalization above.
LIDF_FN void lidfGamma(LIDF_ARRAY(frequency), float sx, float sy, float sz,
               float ox, float oy, float oz,
               LIDF_OUT(float) gammaR, LIDF_OUT(float) gammaT)
{
    // 步骤 1：利用对水平面镜像的对称性，保证光源在上半球（sy>=0）。
    // A two-sided, azimuthally uniform foliage is symmetric under a mirror
    // in the horizontal plane; keep the source in the upper hemisphere.
    if (sy < 0.0f) {
        sy = -sy;
        oy = -oy;
    }
    // 步骤 2：求源/观测的天顶 cos/sin 与相对方位角 psi（水平投影过短时视为同方位，cospsi=1）。
    float cts = min(sy, 1.0f);
    float cto = max(min(oy, 1.0f), -1.0f);
    float sts = sqrt(max(1.0f - cts * cts, 0.0f));
    float sto = sqrt(max(1.0f - cto * cto, 0.0f));
    float hs = sqrt(sx * sx + sz * sz);
    float ho = sqrt(ox * ox + oz * oz);
    float cospsi = 1.0f;
    if (hs > 1.0e-6f && ho > 1.0e-6f)
        cospsi = max(min((sx * ox + sz * oz) / (hs * ho), 1.0f), -1.0f);
    float psi = acos(cospsi);
    float r = 0.0f;
    float t = 0.0f;
    // 步骤 3：对 13 个倾角类按频率加权累加 frho/ftau，跳过频率为 0 的类。
    for (int i = 0; i < LIDF_CLASS_COUNT; ++i) {
        if (frequency[i] <= 0.0f) continue;
        float frho = 0.0f;
        float ftau = 0.0f;
        lidfVolscatt(cts, sts, cto, sto, cospsi, psi,
                     LIDF_CLASS_COS[i], LIDF_CLASS_SIN[i], frho, ftau);
        r += frequency[i] * frho;
        t += frequency[i] * ftau;
    }
    // 步骤 4：乘 π（volscatt 输出为 GammaR/T_i / π，见头部约定）。
    gammaR = LIDF_PI * r;
    gammaT = LIDF_PI * t;
}

#ifdef __cplusplus
// 模块（仅宿主端 C++）：Verhoef 双参数 LIDF 的累积分布函数。
// 作用：返回倾角 thetaDeg（度）以内叶面积累计占比（SCOPE dcum）。
// 说明：a>1 取球形分布 F = 1 - cos(θ)；否则对 x 做不动点迭代（收敛判据 |dx|<1e-10，
//       最多 100000 次）后用 (2y+θ2)/π。
// Host side: Verhoef's two-parameter LIDF integrated over the SAIL classes
// (SCOPE leafangles/dcum). a > 1 selects the spherical distribution
// F = 1 - cos(theta) (SCOPE convention). Returns false for parameters
// outside the SAIL domain |a| + |b| <= 1 or non-finite input.
inline double sailCumulative(double a, double b, double thetaDeg)
{
    const double rd = 3.14159265358979323846 / 180.0;
    if (a > 1.0) return 1.0 - std::cos(thetaDeg * rd);
    const double theta2 = 2.0 * rd * thetaDeg;
    double x = theta2;
    double y = 0.0;
    for (int iteration = 0; iteration < 100000; ++iteration) {
        y = a * std::sin(x) + 0.5 * b * std::sin(2.0 * x);
        const double dx = 0.5 * (y - x + theta2);
        x += dx;
        if (std::abs(dx) < 1.0e-10) break;
    }
    return (2.0 * y + theta2) / 3.14159265358979323846;
}

// 作用：由 (a, b) 计算 13 类的叶面积频率（对类边界累积函数做差并归一化）。
// 输出：frequency；参数不在 SAIL 域（|a|+|b|<=1）、含非有限数或归一化和过小时返回 false。
// 说明：类边界为 10..80° 每 10°，再 82..90° 每 2°；末类累积直接取 1。
inline bool sailLeafAngleFrequencies(double a, double b, float frequency[LIDF_CLASS_COUNT])
{
    for (int i = 0; i < LIDF_CLASS_COUNT; ++i) frequency[i] = 0.0f;
    if (!std::isfinite(a) || !std::isfinite(b)) return false;
    if (a <= 1.0 && std::abs(a) + std::abs(b) > 1.0 + 1.0e-6) return false;
    static const double edges[LIDF_CLASS_COUNT] = {
        10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 82.0, 84.0, 86.0, 88.0, 90.0};
    double previous = 0.0;
    double sum = 0.0;
    double values[LIDF_CLASS_COUNT];
    for (int i = 0; i < LIDF_CLASS_COUNT; ++i) {
        const double cumulative = i == LIDF_CLASS_COUNT - 1 ? 1.0 : sailCumulative(a, b, edges[i]);
        values[i] = cumulative - previous;
        if (!(values[i] > -1.0e-6)) return false;
        values[i] = std::max(values[i], 0.0);
        sum += values[i];
        previous = cumulative;
    }
    if (!(sum > 0.5)) return false;
    for (int i = 0; i < LIDF_CLASS_COUNT; ++i)
        frequency[i] = static_cast<float>(values[i] / sum);
    return true;
}
} // namespace lidf
#endif

#endif
