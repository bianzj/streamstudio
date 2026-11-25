// ==========================================================================
// 文件：energy.cpp
// 功能：STREAM EB 的四组分（光照/阴影土壤、光照/阴影叶片）能量平衡单节点求解（TIRTEB 移植）
// 主要模块：
//   1. 饱和水汽压、潜热、空气密度与 Monin–Obukhov 相似性函数（Similarity）
//   2. 空气动力学阻抗：裸土整体阻抗 bulkResistance 与 Raupach 冠层阻抗网络 aerodynamicResistances
//   3. 叶片气孔阻抗 stomatalResistanceT（Farquhar + Ball-Berry）与土壤表面阻抗
//   4. balances / fluxes：给定温度与阻抗下的四组分通量与残差 R = Rn - H - LE (- G)
//   5. updateTemperature：带阻尼的牛顿温度更新（单步限幅、温度上下限、冰雪上限）
//   6. solveEnergyNode：温度、对流阵风显热与 1/L 的联合迭代，含过境节点的 1/L 括号法回退
//   7. energyInputs / energyResiduals：把同一套平衡暴露给同化，Ad 版本用于隐式微分
// 数据流：app/image.cpp 逐时次调用 solveEnergyNode（输入气象强迫 Forcing、短波吸收
//         ShortwaveAbsorption、状态 EnergyState）→ 返回诊断量与更新后的温度、热状态；
//         同化用 energyResiduals<Ad> 得到 dR/dT、dR/dx，按 dT/dx = -(dR/dT)^-1 dR/dx 求导
// 关系：声明见 energy.h；调用 heat_conduction（G）、radiosity（长波吸收）、dual.h（自动微分）
// 约定：温度 K，通量 W m-2，阻抗 s m-1，气压/水汽压 hPa，风速 m s-1；
//       四组分温度限制在 [175, 380] K（冰雪类另有 273.15 K 上限，多余能量记为融化热汇）；
//       1/L 限制在 ±5 m-1；收敛判据与最终验收阈值不同（见 PORTING.md“稳定度”）
// ==========================================================================

#include "energy.h"
#include "constants.h"
#include "radiosity.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace stream {

// 作用：Tetens 饱和水汽压 es（hPa），输入摄氏温度（水面公式，冰面未单独处理）。
double saturationVapourPressure(double celsius)
{
    return 6.107 * std::pow(10.0, 7.5 * celsius / (237.3 + celsius));
}

namespace {

// 模块：能量平衡数值常数与收敛判据（数值取自 TIRTEB，README/PORTING 的描述需与此保持一致）。
// 说明：kInnerResidual（组分能量残差 W m-2）、kGustTolerance（阵风显热 W m-2）、
//       kStabilityAbsTol/RelTol（1/L 残差）是迭代中的收敛判据；kResidualThreshold
//       等“最终验收”阈值比迭代判据宽（见 PORTING.md“稳定度”）。
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

// 作用：饱和水汽压对温度的导数 des/dT（hPa K-1），对应上面 Tetens 公式的解析导数。
double saturationSlope(double celsius)
{
    const double es = saturationVapourPressure(celsius);
    return es * 2.3026 * 7.5 * 237.3 / std::pow(237.3 + celsius, 2.0);
}

// 作用：水的汽化潜热 L（J kg-1），随温度线性变化（0 °C 时 2.501e6）。
double latentHeat(double kelvin) { return (2.501 - 0.002361 * (kelvin - 273.15)) * 1.0e6; }

// 作用：湿空气密度（kg m-3）：用虚温 Tv = Ta / (1 - 0.378 e/p)，ρ = p / (Rd Tv)。
// 输入：气温 Ta（K）、水汽压 ea（hPa）、气压 p（hPa）。
double airDensity(const Forcing& forcing)
{
    const double pressure = std::max(forcing.p, 1e-6) * 100.0;
    const double vapour = std::max(forcing.ea, 0.0) * 100.0;
    const double virtualTemperature = forcing.ta / (1.0 - 0.378 * vapour / pressure);
    return pressure / (kRd * virtualTemperature);
}

// 模块：Monin–Obukhov 相似性函数（稳定度修正）。
// 说明：inverseObukhov 为 1/L（m-1）；L < 0 不稳定（Paulson 型 psim/psih，x = (1-16 z/L)^(1/4)），
//       L > 0 稳定（-5 z/L），L = 0（中性）全部为 0。稳定侧的 1/L 取 min(1/L, 1/zref)，
//       即 xeff，避免参考高度以上 z/L > 1 时稳定度修正过强。
//       phstar 为粗糙度亚层（roughness sublayer）的修正项（来自 SCOPE resistances）。
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

// 模块：冠层权重 = smoothstep(LAI / 0.5)。LAI 越过 kSparseLai = 0.5 时从裸土阻抗路径
//       平滑过渡到冠层阻抗网络，避免稀疏植被处结果突变。
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

// 模块：裸土/稀疏植被的整体空气动力阻抗 ra（s m-1）。
// 说明：ra = ln(z/z0m)·ln(z/z0h) / (κ² u)（含稳定度修正）；z0h = z0m·exp(-kB^-1)。
//       高度下限 1.01 m、z0 限幅 [1e-4, z/2]、风速下限 0.05 m s-1，保证对数项为正；
//       结果下限 1e-3 s m-1。
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

// 工具：分段阻抗上限 maxResistance（缺省 400 s m-1）；<= 0 表示不设上限。
double resistanceCap(const Aerodynamics& aero) { return aero.maxResistance > 0.0 ? aero.maxResistance : std::numeric_limits<double>::infinity(); }

// 作用：ln((e^a - 1)/(e^a + 1)) = ln tanh(a/2)，用于冠层内指数型扩散廓线的积分；
//       a > 50 时改用 log1p(-2 e^-a) 避免 e^a 溢出，a 下限 1e-8。
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

// 模块：Raupach 冠层空气动力学阻抗网络（SCOPE resistances 的 TIRTEB 版本）。
// 输入：地表参数、气象强迫、1/L、空气密度、场景显热（用于对流阵风）、叶面积 leafArea（可为 Ad）。
// 输出：叶片阻抗 leaf、土壤阻抗 soil（s m-1），以及 u*、位移高度 d、粗糙度 z0、参考高度 z、
//       冠层权重与 limited（稳定度被 1/zref 限幅）标志。
// 步骤 1：有效风速 = sqrt(u² + w*²)，w* 为由场景显热估计的对流速度（待核对：w* 公式中的 1000 为假设的
//         对流边界层高度 m）；各分量下限见 minWindSpeed。
// 步骤 2：参考高度 z = max(referenceHeight, 冠层高 + 0.1 m)；廓线稳定度参考高度随冠层权重
//         在 z 与 2.5 hc 之间过渡；建立 Similarity。
// 步骤 3：LAI = 0 时直接用 z0 = 0.01 m 的裸土整体阻抗并返回。
// 步骤 4：有叶时按 Raupach：位移高度 d、粗糙度 z0（G1 下限 3.3）、摩擦速度 u*，
//         再算冠层上方 rai、粗糙度亚层 rar、冠层内 rac 与叶片边界层、土壤路径阻抗。
// 步骤 5：冠层上方三段共用，分别加叶片边界层/土壤路径阻抗，各段限幅到 cap。
// 步骤 6：LAI < 0.5 时 u*、土壤阻抗、位移与粗糙度按冠层权重与裸土值混合（叶阻抗不混合）。
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
    // （已知近似：气象驱动量被直接当作参考高度 z 处的值，见 README“EB 的参考高度”。）
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

    // 步骤 4：Raupach 位移高度与粗糙度：sq = sqrt(cd1·LAI/2)，d = hc(1 - (1 - e^-sq)/sq)，
    //         z0 = (hc - d)·exp(-κ G1 + psicor)，G1 = max(3.3, (cssoil + cr·LAI/2)^-1/2)；
    //         n 为冠层内扩散系数的衰减指数，zr = 2.5 hc 为粗糙度亚层顶。
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
    // 步骤 5：冠层上方三段（rai + rar + rac）共用，分别加叶片边界层/土壤路径阻抗后限幅。
    const T above = ad::min(rai + rar + rac, cap);
    result.leaf = ad::max(above + ad::min(boundary, cap), 1e-3);
    result.soil = ad::max(above + ad::min(soilProfile + surface.aero.soilBoundary, cap), 1e-3);
    result.ustar = ad::value(ustar);
    result.displacement = ad::value(displacement);
    result.height = z;
    result.roughness = ad::value(roughness);

    // 步骤 6：稀疏植被（冠层权重 < 1）：u*、土壤阻抗、位移高度按权重向裸土值过渡，
    //         粗糙度在对数空间混合。
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
// 作用：二次方程较小根（a = 0 时退化为线性解）；气孔模型中用它做光合限制的钝化最小值（见 stomatalResistanceT）。
// Smaller root of a x^2 + b x + c.
template <class T>
T smallerRoot(double a, const T& b, const T& c)
{
    if (a == 0.0) return -c / b;
    return (-b - ad::sqrt(ad::max(b * b - 4.0 * a * c, 0.0))) / (2.0 * a);
}

// 作用：Tetens 饱和水汽压的模板版本，可对温度自动微分。
template <class T>
T saturationVapourPressureT(const T& celsius)
{
    return 6.107 * ad::pow(10.0, 7.5 * celsius / (237.3 + celsius));
}

template <class T>
T latentHeatT(const T& kelvin) { return (2.501 - 0.002361 * (kelvin - 273.15)) * 1.0e6; }

// 模块：叶片气孔阻抗（Farquhar 光合 + Ball-Berry 气孔模型，TiRT base/biochemical.py）。
// 输入：叶温、叶片吸收的 PAR、25 °C 的 Vcmax（同化的控制量之一）、气象强迫与空气密度。
// 输出：每单位叶面积的气孔阻抗（s m-1）；非 C3/C4（pathway = 0）时取 625000 s m-1（等效不蒸腾）。
// 步骤 1：设定 C3/C4 参数；温度修正因子 qt、th、tl 仅在 temperatureCorrection 时启用。
// 步骤 2：Rubisco 限制 vc、光限制 ve、输出（sink）限制 vs；C4 用 PEP 羧化 kpep。
// 步骤 3：Ball-Berry 求 Ci = cs(1 - 1.6/(m·rh))，下限 minCi·cs（C3 0.3，C4 0.1）。
// 步骤 4：用 smallerRoot 做钝化最小值（θ = 0.8 合并 vc、ve，θ = 0.98 再合并 vs）得总同化 ag，
//         净同化 = ag - rd；气孔导度 gs = 1.6·A / (cs - ci)，下限 stomatalG0；
//         阻抗 = (空气摩尔密度 ρ/M_air) / gs。
// Farquhar / Ball-Berry leaf resistance [s m-1] per unit leaf area, with the g0 conductance floor.
// Templated on the leaf temperature, PAR and Vcmax25 (the EB control of the assimilation).
template <class T>
T stomatalResistanceT(const EnergySurface& surface, const T& leafTemperature, const T& par, const T& vcmax25,
                      const Forcing& forcing, double density)
{
    if (surface.leaf.pathway != 3 && surface.leaf.pathway != 4) return T(625000.0);
    const bool c4 = surface.leaf.pathway == 4;
    // 步骤 1：C4 / C3 的量子效率相关常数 effcon 与温度响应参数（slti、shti、thl、thh、trdm）。
    const double effcon = c4 ? 1.0 / 6.0 : 1.0 / 5.0;
    const double slti = surface.leaf.temperature[0], shti = surface.leaf.temperature[1];
    const double thl = surface.leaf.temperature[2], thh = surface.leaf.temperature[3], trdm = surface.leaf.temperature[4];
    const double tcor = surface.leaf.temperatureCorrection ? 1.0 : 0.0;
    const double tref = 25.0 + 273.15;
    const double kc25 = 350.0e-6, ko25 = 450.0e-3;
    const double p = std::max(forcing.p, 1e-6);
    const double ca = surface.co2;
    const double oxygen = c4 ? 0.0 : (surface.o2 * 1e-3) * (p * 1e-3);
    // 步骤 2：25 °C 参数换算到叶温（Q10 型 qt = 0.1·(T - Tref)·tcor），Kc、Ko、Kpep、Vcmax、Rd 各取不同底数。
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
    // 步骤 2（续）：光系统量子产率 po0、电子传递限制 je、CO2 补偿点 gamma 与叶面相对湿度 rh。
    const T po0 = 4.0 / (0.05 + kd + 4.0);
    const T je = 0.5 * po0 * par;
    const T gamma = 0.5 * oxygen / specificity;
    const T rh = ad::min(T(1.0), forcing.ea / ad::max(saturationVapourPressureT(leafTemperature - 273.15), 1e-6));
    const double ppm2bar = 1e-6 * (p * 1e-3);
    const double cs = ca * ppm2bar;
    // 步骤 3：Ball-Berry 的 Ci / Cs 关系；cs 为叶表面 CO2 分压（bar）。
    const T ci = ad::max(T(minCi * cs), cs * (1.0 - 1.6 / (surface.leaf.ballBerrySlope * rh)));
    const T vc = c4 ? vcmax : vcmax * (ci - gamma) / (mm + ci);
    const T vs = c4 ? kpep * ci : vsC3;
    const T ve = c4 ? je * effcon : je * (ci - gamma) / (ci + 2.0 * gamma) * effcon;
    // 步骤 4：非直角双曲线的联合限制（SCOPE 做法）：v = min_θ(vc, ve)，ag = min_θ(v, vs)。
    const T v = smallerRoot<T>(0.8, -(vc + ve), vc * ve);
    const T ag = smallerRoot<T>(0.98, -(v + vs), v * vs);
    const T assimilation = ag - rd;
    T gs = 1.6 * assimilation * ppm2bar / ad::max(cs - ci, 1e-12);
    // 下限：净同化 <= 0（夜间等）或非有限时 gs 取 stomatalG0，而不是 625000 s m-1（见 PORTING.md“气孔”）。
    gs = ad::max(ad::isfinite(gs) ? gs : T(0.0), surface.stomatalG0);
    return (density / (kMAir * 1e-3)) / gs;
}

// 作用：土壤表面阻抗 rs（s m-1）。常数模式取 fixedResistance；
//       含水量模式（moistureResistance）取 exp(8.206 - 4.255·相对湿度)（Sellers 型，相对湿度限幅 0-1）。
template <class T>
T soilSurfaceResistanceT(const EnergySurface& surface, const T& moisture)
{
    if (!surface.ground.moistureResistance) return T(std::max(surface.ground.fixedResistance, 0.0));
    const double saturation = std::max(surface.ground.saturatedMoisture, 1e-6);
    const T wetness = ad::clamp(moisture / saturation, 0.0, 1.0);
    return ad::exp(8.206 - 4.255 * wetness);
}

// 结构：单个组分的通量（W m-2）：净辐射、显热、潜热、土壤热通量、残差 R = Rn - H - LE (- G)、
//       融化热汇 melt（仅冰雪温度上限情形）。Fluxes 汇总四个组分；Solution 为一次节点求解结果；
//       BalancesT 为固定阻抗下的四组分平衡（可对 Ad 实例化）。
template <class T> struct ComponentFluxT { T net{}, sensible{}, latent{}, ground{}, residual{}, melt{}; };
using ComponentFlux = ComponentFluxT<double>;
struct Fluxes { ComponentFlux soilSunlit, soilShaded, leafSunlit, leafShaded; };

struct Solution {
    Fluxes fluxes;
    Resistances aero;
    double soilResistance = 0, leafResistanceSun = 625000, leafResistanceShade = 625000;
    double sceneSensible = 0, gustResidual = 1e9, density = 1.2;
    double soilSunlitFraction = 0, laiSunlit = 0;
    double maxResidual = 1e9;
    double sceneMelt = 0;
    bool melting = false;
    bool closed = false;
};

template <class T>
struct BalancesT {
    ComponentFluxT<T> soilSunlit, soilShaded, leafSunlit, leafShaded;
    T soilResistance{}, leafResistanceSun = T(625000.0), leafResistanceShade = T(625000.0);
};

// 模块：四组分能量平衡（光照/阴影土壤、光照/阴影叶片）在给定阻抗和温度下的通量与残差。
// 步骤 1：有叶时用 hom3 全阶长波吸收（homLongwave），无叶时土壤直接吸收下行长波。
// 步骤 2：入射 = 短波（直射 + 漫射）+ 长波。
// 步骤 3：土壤与叶片阻抗（叶片气孔阻抗除以 LAI 折算到冠层）。
// 步骤 4：按组分计算 LE = ρ/(ra + rs)·λ·(q*(T) - qa)、H = ρ cp (T - Ta)/ra、
//         Rn = 入射 - ε σ T⁴ · factor（叶片 factor = 2·LAI，两面发射），R = Rn - LE - H。
// 步骤 5：土壤再扣除 G（direct：Rn 的固定比例；surface：7 h 历史卷积；depth：土柱线性化）。
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

// 模块：按当前状态（组分温度、1/L、阵风显热）一次性计算四组分通量，写入 solution。
// 步骤 1：求阻抗（aerodynamicResistances）与四组分平衡（balances）。
// 步骤 2：冰雪温度上限（temperatureCap > 0）：组分在上限且残差为正时，多余能量记为融化 melt、残差置 0。
// 步骤 3：用光照土壤比例与光照/阴影叶面积比例加权，得到场景显热 sceneH（用于对流阵风的闭合）与场景 melt。
// 步骤 4：记录组分最大残差 maxResidual（有叶 4 个，无叶 2 个）。
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
    // Snow / ice: a component sitting on the temperature cap with a surplus of energy melts it instead of warming
    // (the surplus is the melt flux; the component balance then closes). A component below the cap, or on it with a
    // deficit, is unchanged, so classes without a cap follow the original path bit for bit.
    solution.melting = false;
    if (surface.temperatureCap > 0.0) {
        const auto limit = [&](ComponentFlux& flux, double temperature) {
            if (temperature >= surface.temperatureCap && flux.residual > 0.0) {
                flux.melt = flux.residual;
                flux.residual = 0.0;
                solution.melting = true;
            }
        };
        limit(result.soilSunlit, state.soilSunlit);
        limit(result.soilShaded, state.soilShaded);
        if (leaves) {
            limit(result.leafSunlit, state.leafSunlit);
            limit(result.leafShaded, state.leafShaded);
        }
    }

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
    solution.sceneMelt = soilSunFraction * result.soilSunlit.melt + (1.0 - soilSunFraction) * result.soilShaded.melt +
                         leafWeightSun * result.leafSunlit.melt + leafWeightShade * result.leafShaded.melt;
    solution.fluxes = result;
    double maximum;
    if (leaves) maximum = std::max({std::abs(result.soilSunlit.residual), std::abs(result.soilShaded.residual),
                                    std::abs(result.leafSunlit.residual), std::abs(result.leafShaded.residual)});
    else maximum = std::max(std::abs(result.soilSunlit.residual), std::abs(result.soilShaded.residual));
    solution.maxResidual = maximum;
}

// 模块：单个组分的温度牛顿更新。输入：残差 R（W m-2）、气动阻抗 ra、表面阻抗 rs、
//       dG/dT、发射率、辐射发射面数 faces（叶片 2·LAI，土壤 1）、cap（冰雪温度上限，0 为不限）。
// 说明：分母非有限或 <= 1e-6 时不更新；更新量 = 阻尼系数 × R / 分母，限幅 ±kMaxStep。
// Newton step on one component balance R(T) = Rn - H - LE - G with the full derivative
// -dR/dT = rho cp / ra + rho lambda dq*/dT / (ra + rs) + 4 n eps sigma T^3 + dG/dT.
// (The former "robust" step dropped the latent and soil-heat terms and was scaled by 0.08, so it needed
// ~170 steps for the inner tolerance while only 50 were allowed: no pixel ever converged.)
// "robust" damps the step (0.8); every step is limited to kMaxStep and to [kTmin, kTmax].
void updateTemperature(double& temperature, double residual, double ra, double rs, double groundDerivative,
                       double emissivity, double faces, bool robust, double density, const Forcing& forcing,
                       double cap = 0.0)
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
    if (cap > 0.0) temperature = std::min(temperature, cap);   // snow / ice cannot be warmer than the melting point
}

}  // namespace

// 模块：把地表参数、短波吸收与当前状态整理为 double 版 EnergyInputsT（温度、LAI、土壤水分、Vcmax、
//       土壤发射率与热惰性、各短波分量、PAR）。同化把它转为 Ad 并给控制量播种导数。
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

// 模块：固定 1/L 与阵风显热、土壤热历史时四组分的残差 Rn - H - LE (- G)。
//       forward 求解与同化的隐式微分共用 aerodynamicResistances / balances 的同一份实现；
//       无叶时叶片两项返回 0。
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

// 模块：Johansen 土壤导热率与体积热容随含水量的关系（粗质土）。
// 说明：孔隙度由 saturatedMoisture 限幅到 [0.2, 0.7]；干土导热率由容重经验式给出，
//       饱和导热率为 3^(1-n)·0.57^n，按 Kersten 数（0.7·log10(Sr) + 1，Sr <= 0.05 取 0）插值；
//       体积热容 = (1-n)·2e6 + θ·4.18e6 J m-3 K-1。
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

// 模块：单个时间节点的四组分能量平衡求解。
// 输入：地表参数 surface、强迫 forcing、短波吸收、太阳天顶角、是否有直射、时间步 dtSeconds、
//       状态 state（组分温度、1/L、阵风显热、土壤热状态）；bracketFallback 仅过境节点为 true。
// 输出：EnergyDiagnostics（场景加权的 Rn、H、LE、G，残差、阻抗相关量、收敛标志）；
//       副作用：更新 state 的温度、1/L、sceneSensible，并推进土壤热状态（advanceThermalState）。
// 步骤 1：确定 dG/dT 的取法，使牛顿步看到的 G(T) 与残差一致。
// 步骤 2：温度、阵风显热、1/L 的联合迭代（至多 maxIterations）。
// 步骤 3：过境节点未收敛时进入 1/L 括号法回退，必要时恢复到普通迭代的终点。
// 步骤 4：以返回温度重新评估，用较宽的验收阈值判定 converged。
// 步骤 5：提交热状态，汇总场景加权诊断量。
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
    // 步骤 1：surface 方法 dG/dT = 2·I/sqrt(π·dt)（对当前温度的精确导数，只有最新一段历史依赖当前温度）；depth 方法取土柱线性化的斜率；
    //         direct 方法 G = f·Rn，dG/dT = -f·4εσT³。
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

    // 步骤 2：联合迭代。每轮：评估通量 → 检查收敛 → 四组分温度牛顿更新 → 更新阵风显热 →
    //         1/L 松弛更新；括号模式（bracketMode）见上面 FIX 注释。
    // One joint iteration, at most maxIterations (default 50) in total: each pass evaluates the fluxes, takes a
    // damped Newton step on the four component temperatures, updates the convective-gust heat flux and relaxes
    // 1/L towards its Monin-Obukhov target. Converged when energy, gust and stability residuals are all small.
    // maxStabilityIterations is accepted for compatibility only: temperature and 1/L share maxIterations (README).
    for (int iteration = 0; iteration < limit; ++iteration) {
        fluxes(surface, forcing, shortwave, szaDeg, sunlit, inverse, density, dtSeconds, state.sceneSensible,
               state, solution);
        ++iterations;
        const double ustar = std::max(solution.aero.ustar, 1e-8);
        // 步骤 2a：1/L 的 Monin–Obukhov 目标值 -κ g H / (ρ cp Ta u*³)，只用场景显热 H；
        //         收敛 = 组分残差、阵风显热残差与 1/L 残差同时在容差内。
        target = buoyancyCoefficient * solution.sceneSensible / std::pow(ustar, 3.0);
        stabilityClosed = std::abs(inverse - target) <= kStabilityAbsTol + kStabilityRelTol * std::abs(target);
        closed = solution.maxResidual <= kInnerResidual && solution.gustResidual <= kGustTolerance && stabilityClosed;
        if (closed) break;
        // 步骤 2b：保存上一轮温度/阵风（括号模式欠松弛用），再对四个组分做牛顿更新。
        const auto& f = solution.fluxes;
        const double previousTemperature[4] = {state.leafSunlit, state.leafShaded, state.soilSunlit, state.soilShaded};
        const double previousGust = state.sceneSensible;
        if (leaves) {
            updateTemperature(state.leafSunlit, f.leafSunlit.residual, solution.aero.leaf, solution.leafResistanceSun,
                              0.0, surface.leafEmissivity, 2.0 * lai, surface.robustTemperature, density, forcing, surface.temperatureCap);
            updateTemperature(state.leafShaded, f.leafShaded.residual, solution.aero.leaf, solution.leafResistanceShade,
                              0.0, surface.leafEmissivity, 2.0 * lai, surface.robustTemperature, density, forcing, surface.temperatureCap);
        }
        updateTemperature(state.soilSunlit, f.soilSunlit.residual, solution.aero.soil, solution.soilResistance,
                          groundDerivative(state.soilSunlit), surface.ground.emissivity, 1.0, surface.robustTemperature,
                          density, forcing, surface.temperatureCap);
        updateTemperature(state.soilShaded, f.soilShaded.residual, solution.aero.soil, solution.soilResistance,
                          groundDerivative(state.soilShaded), surface.ground.emissivity, 1.0, surface.robustTemperature,
                          density, forcing, surface.temperatureCap);
        // 保护：温度非有限时回退为 Ta + 2 K（光照）/ Ta + 1 K（阴影），冰雪类再受上限约束。
        if (leaves) {
            if (!std::isfinite(state.leafSunlit)) state.leafSunlit = forcing.ta + 2.0;
            if (!std::isfinite(state.leafShaded)) state.leafShaded = forcing.ta + 1.0;
        }
        if (!std::isfinite(state.soilSunlit)) state.soilSunlit = forcing.ta + 2.0;
        if (!std::isfinite(state.soilShaded)) state.soilShaded = forcing.ta + 1.0;
        if (surface.temperatureCap > 0.0) {   // non-finite fallbacks above must respect the cap too
            state.leafSunlit = std::min(state.leafSunlit, surface.temperatureCap);
            state.leafShaded = std::min(state.leafShaded, surface.temperatureCap);
            state.soilSunlit = std::min(state.soilSunlit, surface.temperatureCap);
            state.soilShaded = std::min(state.soilShaded, surface.temperatureCap);
        }
        // 步骤 2c：更新对流阵风用的场景显热。
        state.sceneSensible = solution.sceneSensible;
        if (bracketMode) {   // under-relaxed temperature and gust update: the frozen-1/L iteration itself may cycle (period ~5)
            double* targets[4] = {&state.leafSunlit, &state.leafShaded, &state.soilSunlit, &state.soilShaded};
            for (int k = 0; k < 4; ++k) *targets[k] = previousTemperature[k] + kBracketDamping * (*targets[k] - previousTemperature[k]);
            state.sceneSensible = previousGust + kBracketDamping * (state.sceneSensible - previousGust);
        }
        // 步骤 2d：1/L 更新。普通模式：自适应松弛（误差变号时松弛系数减半、下限 0.02，
        //         同号时每步增长 10%、上限 0.5），1/L 限幅 ±5 m-1；
        //         括号模式：先在冻结的 1/L 下闭合温度与阵风，再用扩展 + 二分法找 target(1/L) = 1/L 的根。
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

    // 步骤 4：用最终温度与 1/L 重新评估并按验收阈值（残差 5 W m-2、阵风 2 W m-2、
    //         稳定度容差 2 倍）判定 converged；迭代内判据更严，二者不同是有意的。
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
    // 步骤 5：提交 1/L 与两套土壤热状态（光照/阴影），随后汇总场景加权诊断量；
    //         stabilityIterations 与 iterations 相同（温度与 1/L 共用迭代，见 README）。
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
    diagnostics.meltFlux = solution.sceneMelt;
    diagnostics.meltLimited = solution.melting;
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
    // 注：fallbackUsed 恒为 false，括号法回退是否启用不在此标记（待核对：该字段是否仍被使用）。
    diagnostics.fallbackUsed = false;
    state.sceneSensible = solution.sceneSensible;
    return diagnostics;
}

}  // namespace stream


