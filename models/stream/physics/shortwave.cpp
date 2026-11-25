// ==========================================================================
// 文件：shortwave.cpp
// 功能：TIRTEB 风格的短波分配与吸收：把水平面向下短波分成直射/漫射，按光谱分组用 hom3 节点辐射度
//       求土壤与叶片（光照/阴影）的吸收短波、PAR 光合光量子通量、场景反照率。
// 主要模块：
//   1. 轨道因子 E0、晴空直射法向辐照上限（Meinel + Kasten–Young 大气质量）
//   2. makeSolarBins / binSpectrum：1 nm 太阳谱按 binWidthNm 分组、叶片/土壤光谱组内平均
//   3. requestedDiffuseFraction（Erbs 反算）与 admissibleDiffuseFraction（晴空 DNI 上限限幅）
//   4. shortwaveComponents<T>：逐波段 hom3 求解 → 各组分吸收（T = double 或 Ad，用于同化求导）
//   5. absorbShortwave：double 版封装，附带诊断量（clearnessIndex、directNormal、albedo 等）
// 数据流：energy.cpp / 成像代码给出全球短波、太阳天顶角、LAI、光谱 → 本文件 → 各组分吸收（W m-2）。
// 关系：节点辐射度由 radiosity.cpp 提供；光谱网格常数在 constants.h（400–2400 nm，1 nm）。
// 约定：吸收量按“每单位组分面积”给出（土壤按土壤面积、叶片按叶面积）；天顶角为度；kG = 0.5；
//       太阳在地平线以下或受光面不被照亮时，全部短波按漫射处理（FIX 2026-11-21 C10）。
// ==========================================================================

#include "shortwave.h"
#include "constants.h"
#include "radiosity.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace stream {
namespace {

// 常数：叶片投影系数 G（球面叶片分布）；两个太阳常数分别用于 Erbs 晴空指数（1361）与 Meinel DNI（1367）。
// kPhotons：把波长 λ(nm) 乘它即得 μmol J-1（即 1e-3/(N_A·h·c)），用于 PAR 光量子换算。
constexpr double kG = 0.5;
constexpr double kSolarConstant = 1361.0;      // W m-2, TIRTEB clearness index
constexpr double kMeanSolarConstant = 1367.0;  // W m-2, Meinel clear-sky DNI
constexpr double kPhotons = 1e-3 / (6.02214e23 * 6.6262e-34 * 299792458.0);

// 日地距离订正 E0 = 1 + 0.033·cos(2π·doy/365)；dayOfYear <= 0（无日期）取近日点值 1.033。
double orbitalFactor(int dayOfYear)
{
    return dayOfYear > 0 ? 1.0 + 0.033 * std::cos(2.0 * kPi * dayOfYear / 365.0) : 1.033;
}

// 作用：晴空直射法向辐照上限（W m-2）= 1367·E0·0.7^(AM^0.678)（Meinel），AM 用 Kasten–Young 大气质量。
// 输入：太阳天顶角（度）；天顶角 >= 90° 返回 0。用于把超出晴空上限的直射转为漫射（enforceSolarLimit）。
double solarDirectNormalLimit(double szaDeg, int dayOfYear)
{
    if (szaDeg >= 90.0) return 0.0;
    const double airMass = 1.0 / (std::cos(szaDeg * kPi / 180.0) +
                                  0.50572 * std::pow(96.07995 - szaDeg, -1.6364));
    return kMeanSolarConstant * orbitalFactor(dayOfYear) * std::pow(0.7, std::pow(airMass, 0.678));
}

}  // namespace

// 作用：把 1 nm 参考太阳直射谱 fesun 与天空谱 fesky 按 binWidthNm 连续分组，累计每组形状权重。
// 输出：每组的 [first,last) 下标、直射/漫射权重，以及 PAR 组（400–700 nm）的光量子系数 photon。
// 说明：不足一个组宽的尾部并入最后一组；组平均波长用 (fesun+fesky) 加权，权重为 0 时取几何中点；
//       光谱网格为 400–2400 nm、2001 点（constants.h）。
SolarBins makeSolarBins(const std::vector<float>& fesun, const std::vector<float>& fesky, int binWidthNm)
{
    SolarBins bins;
    const int width = std::max(1, binWidthNm);
    for (int first = 0; first < kSpectralCount;) {
        int last = first + width;
        if (kSpectralCount - last < width) last = kSpectralCount;
        double direct = 0.0, diffuse = 0.0;
        for (int index = first; index < last; ++index) {
            direct += fesun[index];
            diffuse += fesky[index];
        }
        double weightedWavelength = 0.0;
        for (int index = first; index < last; ++index) {
            weightedWavelength += (fesun[index] + fesky[index]) * (kFirstWavelength + index);
        }
        const bool par = kFirstWavelength + first >= 400 && kFirstWavelength + last - 1 <= 700;
        const double meanWavelength = direct + diffuse > 0.0
            ? weightedWavelength / (direct + diffuse)
            : kFirstWavelength + 0.5 * (first + last - 1);
        bins.range.push_back({first, last});
        bins.direct.push_back(direct);
        bins.diffuse.push_back(diffuse);
        bins.photon.push_back(par ? meanWavelength * kPhotons : 0.0);
        bins.directTotal += direct;
        bins.diffuseTotal += diffuse;
        first = last;
    }
    return bins;
}

// 作用：把 1 nm 光谱（叶片反射/透射、土壤反射）按同一分组取组内算术平均，供节点辐射度求解。
std::vector<float> binSpectrum(const SolarBins& bins, const std::vector<float>& spectrum)
{
    std::vector<float> result(bins.size());
    for (size_t bin = 0; bin < bins.size(); ++bin) {
        const auto [first, last] = bins.range[bin];
        double sum = 0.0;
        for (int index = first; index < last; ++index) sum += spectrum[index];
        result[bin] = static_cast<float>(last > first ? sum / (last - first) : 0.0);
    }
    return result;
}

// 作用：无日期的三参数版本（兼容旧调用），等价于近日点 E0 = 1.033，见下方四参数版本。
double requestedDiffuseFraction(double globalShortwave, double szaDeg, double configured)
{
    // FIX(2026-11-21 C7): no date -> perihelion orbital factor (TiRT _orbital_factor without solar_day_of_year).
    return requestedDiffuseFraction(globalShortwave, szaDeg, configured, 0);
}

// 作用：请求的漫射比例。configured 在 [0,1] 内直接采用（缺省 0.2）；否则按 Erbs 公式由晴空指数 kt 反算。
// 输入：水平面全球短波（W m-2）、太阳天顶角（度）、日序数；输出：漫射份额 [0,1]。
// 步骤：太阳在地平线以下取 1；kt = 全球短波/(1361·E0·cos θs)，限幅 [0,1.2]；
//       分段 kt<=0.22、0.22–0.80（四次多项式）、>0.80 取 0.165。
double requestedDiffuseFraction(double globalShortwave, double szaDeg, double configured, int dayOfYear)
{
    if (configured >= 0.0 && configured <= 1.0) return configured;
    // FIX(2026-11-21 C10): sun at / below the horizon: no beam. (cos was floored at 1e-6, so kt hit 1.2 and the
    // Erbs fraction became 0.165, i.e. 83 % "direct" with the sun down.)
    if (!(szaDeg < 90.0)) return 1.0;
    const double cosine = std::max(std::cos(szaDeg * kPi / 180.0), 1e-6);
    // FIX(2026-11-21 C7): extraterrestrial horizontal irradiance includes E0 (TiRT radiation.py:218-221; it was
    // already used by the clearnessIndex diagnostic below, so kt and its diagnostic disagreed by up to ±3.3 %).
    const double clearness = std::clamp(globalShortwave / (kSolarConstant * orbitalFactor(dayOfYear) * cosine), 0.0, 1.2);
    double fraction;
    if (clearness <= 0.22) fraction = 1.0 - 0.09 * clearness;
    else if (clearness <= 0.80)
        fraction = 0.9511 - 0.1604 * clearness + 4.388 * clearness * clearness -
                   16.638 * clearness * clearness * clearness + 12.336 * std::pow(clearness, 4.0);
    else fraction = 0.165;
    return std::clamp(fraction, 0.0, 1.0);
}

// 作用：在请求漫射份额基础上施加晴空 DNI 上限：直射水平分量不得超过 DNI_limit·cos θs，
//       超出部分转为漫射（返回 max(请求值, 1 - 上限/全球短波)），总短波不变。
// 说明：*limit（可选）返回法向上限；太阳在地平线以下时恒返回 1；enforceLimit = false 时只返回请求值。
double admissibleDiffuseFraction(double globalShortwave, double szaDeg, int dayOfYear, double configured,
                                 bool enforceLimit, double* limit)
{
    const double requested = requestedDiffuseFraction(globalShortwave, szaDeg, configured, dayOfYear);  // FIX(2026-11-21 C7): E0 of the day
    const double normalLimit = solarDirectNormalLimit(szaDeg, dayOfYear);
    if (limit) *limit = normalLimit;
    // FIX(2026-11-21 C10): no beam below the horizon, also when the DNI limit is not enforced.
    if (!(szaDeg < 90.0) && globalShortwave > 0.0) return 1.0;
    if (!enforceLimit) return requested;
    const double total = std::max(globalShortwave, 0.0);
    if (total <= 0.0) return requested;
    const double horizontalLimit = normalLimit * std::max(std::cos(szaDeg * kPi / 180.0), 0.0);
    return std::max(requested, 1.0 - std::min(horizontalLimit / total, 1.0));
}

// 作用：double 版短波吸收入口，计算诊断量并调用 shortwaveComponents<double> 取组分吸收。
// 输入：太阳分组 sun、叶片/土壤分组光谱、LAI、全球短波、天顶角、日序数、请求漫射份额、sunlit。
// 输出：ShortwaveAbsorption（吸收按组分面积；albedo = 1 - 吸收/入射；诊断 clearness、DNI 等）。
// 步骤 1：算请求/允许的漫射份额与晴空指数诊断；步骤 2：受光面不被照亮则漫射份额取 1；
// 步骤 3：光谱或太阳谱不全时只返回诊断量；步骤 4：调用 shortwaveComponents 并拷贝字段。
ShortwaveAbsorption absorbShortwave(const SolarBins& sun, const OpticalSpectra& spectra, double lai,
                                    double globalShortwave, double szaDeg, int dayOfYear, double requestedFraction,
                                    bool enforceLimit, bool sunlit)
{
    ShortwaveAbsorption result;
    result.diffuseFractionRequested = requestedDiffuseFraction(globalShortwave, szaDeg, requestedFraction, dayOfYear);  // FIX(2026-11-21 C7)
    result.directNormalLimit = solarDirectNormalLimit(szaDeg, dayOfYear);
    const double cosine = std::cos(szaDeg * kPi / 180.0);
    result.clearnessIndex = cosine > 0.0
        ? globalShortwave / (kSolarConstant * orbitalFactor(dayOfYear) * cosine)
        : std::numeric_limits<double>::quiet_NaN();
    // 无短波或无太阳分组时，吸收量保持 0，只返回已算的诊断量。
    if (globalShortwave <= 0.0 || sun.size() == 0) return result;
    result.diffuseFraction = admissibleDiffuseFraction(globalShortwave, szaDeg, dayOfYear, requestedFraction,
                                                      enforceLimit, &result.directNormalLimit);
    // FIX(2026-11-21 C10): a receiving surface that is not sunlit gets no beam; the whole flux is diffuse (it is
    // also distributed that way in shortwaveComponents), so the diagnostics report fd = 1 / partition limited.
    if (!(sunlit && szaDeg < 90.0)) result.diffuseFraction = 1.0;
    result.partitionLimited = result.diffuseFraction > result.diffuseFractionRequested + 1e-12;
    result.directNormal = (szaDeg < 90.0 && globalShortwave > 0.0)
        ? globalShortwave * (1.0 - result.diffuseFraction) / std::max(cosine, 1e-12)
        : 0.0;
    if (!spectra.leafReflectance || !spectra.leafTransmittance || !spectra.soilReflectance) return result;
    if (!(sun.directTotal > 0.0) || !(sun.diffuseTotal > 0.0)) return result;

    const ShortwaveComponentsT<double> parts = shortwaveComponents<double>(
        sun, *spectra.leafReflectance, *spectra.leafTransmittance, *spectra.soilReflectance, lai, globalShortwave,
        result.diffuseFraction, szaDeg, sunlit);
    result.soilDirect = parts.soilDirect;
    result.leafDirect = parts.leafDirect;
    result.soilDiffuseSun = parts.soilDiffuseSun;
    result.soilDiffuseShade = parts.soilDiffuseShade;
    result.leafDiffuseSun = parts.leafDiffuseSun;
    result.leafDiffuseShade = parts.leafDiffuseShade;
    result.soilMean = parts.soilMean;
    result.leafMean = parts.leafMean;
    result.parLeafDirect = parts.parLeafDirect;
    result.parLeafDiffuse = parts.parLeafDiffuse;
    result.parLeafDiffuseSun = parts.parLeafDiffuseSun;
    result.parLeafDiffuseShade = parts.parLeafDiffuseShade;
    result.soilSunlitFraction = parts.soilSunlitFraction;
    result.laiSunlit = parts.laiSunlit;
    result.albedo = parts.incident > 0.0 ? 1.0 - parts.absorbed / parts.incident : 0.0;
    return result;
}

namespace {
template <class S> double soilValue(const S& value) { return static_cast<double>(value); }
template <int N> Dual<N> soilValue(const Dual<N>& value) { return value; }
}  // namespace

// 作用：给定漫射份额，逐波段用 hom3 节点辐射度求各组分吸收（同化时 T = Ad 对 LAI 与土壤光谱求导）。
// 输入：太阳分组、叶片 ρ/τ 与土壤反射率（按组）、LAI、全球短波（W m-2）、diffuseFraction、天顶角、sunlit。
// 输出：ShortwaveComponentsT：直射/漫射分量对光照/阴影组分的吸收（每单位组分面积）、PAR（μmol m-2 s-1）、
//       光照土壤面积份额、光照叶面积（LAI 中受光部分）、总吸收 absorbed 与入射 incident。
// 步骤：1) 判受光并决定直射/漫射标度；2) LAI = 0 走裸土；3) 构造节点与到达向量；
//       4) 求各节点光照面积；5) 逐波段 homAbsorbedPair 求解并累加；6) 汇总平均吸收与 PAR。
template <class T, class SoilSpectrum>
ShortwaveComponentsT<T> shortwaveComponents(const SolarBins& sun, const std::vector<float>& leafReflectance,
                                            const std::vector<float>& leafTransmittance, const SoilSpectrum& soilSpectrum,
                                            const T& lai, double globalShortwave, double diffuseFraction,
                                            double szaDeg, bool sunlit)
{
    ShortwaveComponentsT<T> out;
    if (!(globalShortwave > 0.0) || sun.size() == 0 || !(sun.directTotal > 0.0) || !(sun.diffuseTotal > 0.0)) return out;
    // 步骤 1：受光判定（太阳在地平线以上且受光面朝向太阳）；不受光时全部短波转为漫射。
    //         extinction = G/cos θs（天顶角限幅 85°，cos 下限 1e-6，避免低太阳高度时消光发散）。
    //         directScale/diffuseScale：把太阳分组权重（直射/漫射谱形状）归一到当前全球短波。
    const bool lit = sunlit && szaDeg < 90.0;
    // FIX(2026-11-21 C10): without a sunlit surface the direct part (1 - fd) SW was dropped (esun = 0 below), losing
    // up to 83 % of the shortwave near sunrise/sunset; route it to the diffuse sky term so absorbed + reflected = SW.
    if (!lit) diffuseFraction = 1.0;
    const double extinction = kG / std::max(std::cos(std::min(szaDeg, 85.0) * kPi / 180.0), 1e-6);
    const double directScale = globalShortwave * (1.0 - diffuseFraction) / sun.directTotal;
    const double diffuseScale = globalShortwave * diffuseFraction / sun.diffuseTotal;

    // 步骤 2：LAI = 0（裸地）：只有土壤吸收，逐波段 (1 - 土壤反射率) × 入射；土壤反射率限幅 [0,1]。
    if (!(lai > 0.0)) {
        T directSoil(0.0), diffuseSoil(0.0), absorbed(0.0);
        double incident = 0.0;
        for (size_t bin = 0; bin < sun.size(); ++bin) {
            const double esun = lit ? directScale * sun.direct[bin] : 0.0;
            const double esky = diffuseScale * sun.diffuse[bin];
            const T soil = ad::clamp(T(soilValue(soilSpectrum[bin])), 0.0, 1.0);
            const T direct = esun * (T(1.0) - soil);
            const T diffuse = esky * (T(1.0) - soil);
            directSoil += direct;
            diffuseSoil += diffuse;
            absorbed += direct + diffuse;
            incident += esun + esky;
        }
        out.soilDirect = directSoil;
        out.soilDiffuseSun = out.soilDiffuseShade = out.soilMean = diffuseSoil;
        out.soilSunlitFraction = T(lit ? 1.0 : 0.0);
        out.absorbed = absorbed;
        out.incident = T(incident);
        return out;
    }

    // 步骤 3：构造 hom3 节点（土壤 + 上下两层冠层）；beam = 光束到达各节点的份额，
    //         天空辐照按各节点 escape × capacity 权重分配（归一后作为“从上方到达”）。
    const HomCanopyT<T> nodes(ad::max(lai, 0.0));
    T beam[kHomNodes]{}, sky[kHomNodes]{};
    if (lit) nodes.split(szaDeg, beam);
    T skySum(0.0);
    for (int node = 0; node < kHomNodes; ++node) {
        sky[node] = nodes.escape[node] * nodes.capacity[node];
        skySum += sky[node];
    }
    T beamArrivals[2 * kHomNodes]{}, skyArrivals[2 * kHomNodes]{};
    for (int node = 0; node < kHomNodes; ++node) {
        beamArrivals[2 * node + 1] = beam[node];
        if (skySum > 0.0) skyArrivals[2 * node + 1] = sky[node] / skySum;
    }
    // 步骤 4：各节点面积与光照面积：土壤取 capacity（=1），叶片每层 LAI/2（capacity/(4G)）；
    //         光照叶面积 = 截获的光束份额 / 消光系数，光照土壤 = 穿透冠层的光束份额，均不超过该节点面积。
    T soilArea[kHomNodes]{}, leafArea[kHomNodes]{}, sunlitArea[kHomNodes]{};
    for (int node = 0; node < kHomNodes; ++node) {
        if (nodes.soil[node]) {
            soilArea[node] = nodes.capacity[node];
            sunlitArea[node] = lit ? ad::min(beam[node], soilArea[node]) : T(0.0);
        } else {
            leafArea[node] = nodes.capacity[node] / (4.0 * kG);
            sunlitArea[node] = lit ? ad::min(beam[node] / extinction, leafArea[node]) : T(0.0);
        }
    }
    T soilSunlit(0.0), laiSunlit(0.0), soilAreaTotal(0.0), leafAreaTotal(0.0);
    for (int node = 0; node < kHomNodes; ++node) {
        if (nodes.soil[node]) { soilSunlit += sunlitArea[node]; soilAreaTotal += soilArea[node]; }
        else { laiSunlit += sunlitArea[node]; leafAreaTotal += leafArea[node]; }
    }
    out.soilSunlitFraction = soilSunlit;
    out.laiSunlit = laiSunlit;

    // 辅助：把各节点的吸收功率除以节点面积后，按光照面积（或阴影面积）加权，得到“光照/阴影”组分的平均吸收；
    //       光照或阴影权重总和过小（< 1e-12）时退化为按节点总面积加权。
    const auto mean = [&](const T* values, bool soilMask, bool sunlitPart) {
        const T* area = soilMask ? soilArea : leafArea;
        T weightSum(0.0);
        for (int node = 0; node < kHomNodes; ++node) {
            if (nodes.soil[node] != soilMask || !(area[node] > 0.0)) continue;
            weightSum += sunlitPart ? sunlitArea[node] : (area[node] - sunlitArea[node]);
        }
        const bool fallback = !(weightSum > 1e-12);
        T total(0.0);
        for (int node = 0; node < kHomNodes; ++node) {
            if (nodes.soil[node] != soilMask || !(area[node] > 0.0)) continue;
            const T weight = fallback ? area[node] : (sunlitPart ? sunlitArea[node] : (area[node] - sunlitArea[node]));
            total += values[node] / area[node] * weight;
        }
        const T normalizer = fallback ? (soilMask ? soilAreaTotal : leafAreaTotal) : weightSum;
        return normalizer > 0.0 ? total / normalizer : T(0.0);
    };

    // 步骤 5：逐光谱组循环：叶片 ρ/τ 限幅到 [0,1] 且 ρ+τ <= 1；解“光束+天空”两组到达；
    //         直射项 = 光束未散射吸收（按光照面积平均），漫射项 = 光束散射部分 + 天空全阶吸收；
    //         PAR 组再乘 photon（μmol J-1）累加到各 PAR 量。
    double incident = 0.0;
    for (size_t bin = 0; bin < sun.size(); ++bin) {
        const double esun = lit ? directScale * sun.direct[bin] : 0.0;
        const double esky = diffuseScale * sun.diffuse[bin];
        const double rho = std::clamp(static_cast<double>(leafReflectance[bin]), 0.0, 1.0);
        const double tau = std::min(std::clamp(static_cast<double>(leafTransmittance[bin]), 0.0, 1.0), 1.0 - rho);
        const T soil = ad::clamp(T(soilValue(soilSpectrum[bin])), 0.0, 1.0);
        T beamFirst[kHomNodes], beamTotal[kHomNodes], skyFirst[kHomNodes], skyTotal[kHomNodes];
        homAbsorbedPair<T>(nodes, soil, rho, tau, beamArrivals, skyArrivals, beamFirst, beamTotal, skyFirst, skyTotal);
        T direct[kHomNodes], diffuse[kHomNodes];
        T directSoilBand(0.0), directLeafBand(0.0);
        for (int node = 0; node < kHomNodes; ++node) {
            direct[node] = esun * beamFirst[node];
            diffuse[node] = esun * beamTotal[node] + esky * (skyFirst[node] + skyTotal[node]);
            out.absorbed += direct[node] + diffuse[node];
            if (nodes.soil[node]) directSoilBand += direct[node];
            else directLeafBand += direct[node];
        }
        incident += esun + esky;
        out.soilDirect += soilSunlit > 1e-12 ? directSoilBand / soilSunlit : T(0.0);
        out.leafDirect += laiSunlit > 1e-12 ? directLeafBand / laiSunlit : T(0.0);
        const T soilSunBand = mean(diffuse, true, true);
        const T soilShadeBand = mean(diffuse, true, false);
        const T leafSunBand = mean(diffuse, false, true);
        const T leafShadeBand = mean(diffuse, false, false);
        out.soilDiffuseSun += soilSunBand;
        out.soilDiffuseShade += soilShadeBand;
        out.leafDiffuseSun += leafSunBand;
        out.leafDiffuseShade += leafShadeBand;
        T soilSum(0.0), leafSum(0.0);
        for (int node = 0; node < kHomNodes; ++node) (nodes.soil[node] ? soilSum : leafSum) += diffuse[node];
        const T soilMeanBand = soilAreaTotal > 0.0 ? soilSum / soilAreaTotal : T(0.0);
        const T leafMeanBand = leafAreaTotal > 0.0 ? leafSum / leafAreaTotal : T(0.0);
        out.soilMean += soilMeanBand;
        out.leafMean += leafMeanBand;
        const double photons = sun.photon[bin];
        if (photons > 0.0) {
            out.parLeafDirect += (laiSunlit > 1e-12 ? directLeafBand / laiSunlit : T(0.0)) * photons;
            out.parLeafDiffuse += leafMeanBand * photons;
            out.parLeafDiffuseSun += leafSunBand * photons;
            out.parLeafDiffuseShade += leafShadeBand * photons;
        }
    }
    out.incident = T(incident);
    return out;
}

// 显式实例化：double（正向）、Ad + Ad 土壤谱（对 LAI 与土壤光谱求导）、Ad + float 土壤谱（只对 LAI 求导）。
template ShortwaveComponentsT<double> shortwaveComponents<double, std::vector<float>>(
    const SolarBins&, const std::vector<float>&, const std::vector<float>&, const std::vector<float>&, const double&,
    double, double, double, bool);
template ShortwaveComponentsT<Ad> shortwaveComponents<Ad, std::vector<Ad>>(
    const SolarBins&, const std::vector<float>&, const std::vector<float>&, const std::vector<Ad>&, const Ad&,
    double, double, double, bool);
template ShortwaveComponentsT<Ad> shortwaveComponents<Ad, std::vector<float>>(
    const SolarBins&, const std::vector<float>&, const std::vector<float>&, const std::vector<float>&, const Ad&,
    double, double, double, bool);

}  // namespace stream
