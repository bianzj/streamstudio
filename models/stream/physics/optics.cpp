// ==========================================================================
// 文件：optics.cpp
// 功能：叶片光学 PROSPECT 与土壤光谱 BSM（含湿土 SCOPE soilwat）的 1 nm 光谱计算
// 主要模块：
//   1. readOptCoeff：读取 optipar_new.csv（PROSPECT/BSM 系数，400-2400 nm 共 2001 行 18 列）
//   2. expint / calctav：指数积分与介质界面平均透射率（PROSPECT 基础函数）
//   3. prospect：叶片反射率与透射率（N 层平板模型，Fluspect/PROSPECT 形式）
//   4. bsm：土壤反射率（干土 GSV 三分量 + 含水量的水膜 Poisson 混合）
// 数据流：scene/classes 提供 LeafOptics/SoilOptics 参数；shortwave / image 逐波段取得叶片
//         反射率/透射率与土壤反射率，再交给冠层闭合（canopy.cpp）和节点辐射度（radiosity）
// 约定：光谱网格固定为 400-2400 nm，间隔 1 nm（kSpectralCount = 2001）；
//       全部用 float 计算，与 TiRT/SCOPE 的 fluspect、soilopt 数值逐项保留（见 PORTING.md）
// 来源：base/leafopt.py（fluspect）、base/soilopt.py（BSM）、scifuns.cpp 的 calctav
// ==========================================================================

#include "optics.h"
#include "constants.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace stream {
namespace {

// 工具：把路径转成 UTF-8 字符串用于错误信息（避免本地代码页导致乱码）。
std::string displayPath(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

}  // namespace

// 模块：读取 PROSPECT/BSM 系数表 CSV（首行为表头，跳过）。
// 输出：OptCoeff 的 18 个列向量，列顺序与 columns[] 数组一致（波长、折射率 nr、各吸收系数、
//       水的折射率 nw、GSV 土壤基函数 gsv1-3 等）。
// 说明：列数不足 18 或行数不是 2001（400-2400 nm）都直接报错，避免静默使用残缺光谱。
OptCoeff readOptCoeff(const std::filesystem::path& file)
{
    std::ifstream input(file);
    if (!input) throw std::runtime_error("找不到 PROSPECT/BSM 系数：" + displayPath(file));
    std::string line;
    std::getline(input, line);
    OptCoeff coeff;
    std::vector<float>* columns[] = {&coeff.wl, &coeff.nr, &coeff.kab, &coeff.kca, &coeff.ks, &coeff.kw, &coeff.kdm,
        &coeff.phiI, &coeff.phiII, &coeff.kcaV, &coeff.kcaZ, &coeff.kcant, &coeff.kcaV2, &coeff.phi,
        &coeff.gsv1, &coeff.gsv2, &coeff.gsv3, &coeff.nw};
    while (std::getline(input, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        std::vector<std::string> fields;
        std::stringstream stream(line);
        std::string field;
        while (std::getline(stream, field, ',')) fields.push_back(field);
        if (fields.size() < 18) throw std::runtime_error("PROSPECT/BSM 系数列数不足：" + displayPath(file));
        for (int column = 0; column < 18; ++column) columns[column]->push_back(std::stof(fields[column]));
    }
    if (coeff.wl.size() != kSpectralCount) throw std::runtime_error("PROSPECT/BSM 系数须覆盖 400–2400 nm 共 2001 行");
    return coeff;
}

// 模块：指数积分 E1(x) = ∫_1^∞ exp(-x t)/t dt 的数值近似。
// 说明：对 t ∈ [1, 100) 以 0.001 为步长做矩形求和，上限在 t = 100 截断；
//       x 很小时截断误差不可忽略（待核对：是否与 PROSPECT 原版 expint 取同一做法）。
//       prospect 只在 Kall > 0 时使用该函数。
float expint(float x)
{
    double sum = 0.0;
    for (int i = 1000; i < 100000; i++) {
        const double ii = i / 1000.0;
        sum = sum + std::exp(-x * ii) / ii * 0.001;
    }
    return static_cast<float>(sum);
}

// 模块：折射率 nr 的介质界面对入射角 0..alfa（度）范围内各向同性辐射的平均透射率。
// 说明：PROSPECT/Fluspect 的 calctav，按垂直（ts）与平行（tp）偏振分别积分后合并；
//       alfa = 90 为半球平均，alfa = 59 用于叶片表面入射（与原版一致）。
//       alfa = 90 时 b1 在数学上等于 0，直接置 0 以避免浮点误差使 sqrt 的自变量为负。
float calctav(float alfa, float nr)
{
    const float pi = 3.1415926f;
    const float rd = pi / 180;
    const float n2 = nr * nr;
    const float np = n2 + 1;
    const float nm = n2 - 1;
    const float a = (nr + 1) * (nr + 1) / 2;
    const float k = -(n2 - 1) * (n2 - 1) / 4;
    const float sa = std::sin(alfa * rd);
    float b1 = 0;
    if (alfa != 90) b1 = std::sqrt((sa * sa - np / 2) * (sa * sa - np / 2) + k);
    const float b2 = sa * sa - np / 2;
    const float b = b1 - b2;
    const float b3 = b * b * b;
    const float a3 = a * a * a;
    const float ts = (k * k / (6 * b3) + k / b - b / 2) - (k * k / (6 * a3) + k / a - a / 2);
    const float tp1 = -2 * n2 * (b - a) / (np * np);
    const float tp2 = -2 * n2 * np * std::log(b / a) / (nm * nm);
    const float tp3 = n2 * (1 / b - 1 / a) / 2;
    const float tp4 = 16 * n2 * n2 * (n2 * n2 + 1) * std::log((2 * np * b - nm * nm) / (2 * np * a - nm * nm)) / (np * np * np * nm * nm);
    const float tp5 = 16 * n2 * n2 * n2 * (1 / (2 * np * b - nm * nm) - 1 / (2 * np * a - nm * nm)) / (np * np * np);
    const float tp = tp1 + tp2 + tp3 + tp4 + tp5;
    return (ts + tp) / (2 * sa * sa);
}

// 模块：PROSPECT/Fluspect 叶片反射率与透射率（半球，1 nm 网格，400-2400 nm）。
// 输入：系数表 coeff 与 LeafOptics（叶绿素 cab、水 cw、干物质 cdm、棕色素 cs、结构参数 n）；
//       各含量单位沿用 PROSPECT 约定（待核对具体单位）。
// 输出：reflectance、transmittance，各 kSpectralCount 个值。
void prospect(const OptCoeff& coeff, const LeafOptics& leaf, std::vector<float>& reflectance, std::vector<float>& transmittance)
{
    reflectance.assign(kSpectralCount, 0.0f);
    transmittance.assign(kSpectralCount, 0.0f);
    const float N = leaf.n;
    for (int i = 0; i < kSpectralCount; i++) {
        const float nr = coeff.nr[i];
        // 步骤 1：各成分吸收之和除以层数 N 得单层吸收 Kall，再得单层透过率 tau（含 E1 项）；
        //         Kall = 0 时无吸收，tau = 1。
        const float Kall = (leaf.cab * coeff.kab[i] + leaf.cdm * coeff.kdm[i] + leaf.cw * coeff.kw[i] + leaf.cs * coeff.ks[i]) / N;

        const float t1 = (1 - Kall) * std::exp(-Kall);
        const float t2 = Kall * Kall * expint(Kall);
        float tau = 1;
        if (Kall > 0) tau = t1 + t2;

        // 步骤 2：用 calctav 求界面透射/反射（59° 入射 talf，半球 t12，反向 t21 = t12 / nr²），
        //         得单层平板含表面项的 (Ra, Ta) 与内部多次反射后的 (r, tt)。
        const float talf = calctav(59, nr);
        const float ralf = 1 - talf;
        const float t12 = calctav(90, nr);
        const float r12 = 1 - t12;
        const float t21 = t12 / (nr * nr);
        const float r21 = 1 - t21;

        float denom = 1 - r21 * r21 * tau * tau;
        const float Ta = talf * tau * t21 / denom;
        const float Ra = ralf + r21 * tau * Ta;

        const float tt = t12 * tau * t21 / denom;
        const float r = r12 + r21 * tau * tt;

        // 步骤 3：用 Stokes 公式（a、b、D 与 N-1 次幂）把 N 层平板叠加为 Rsub、Tsub。
        const float D = std::sqrt((1 + r + tt) * (1 + r - tt) * (1 - r + tt) * (1 - r - tt));
        const float rq = r * r;
        const float tq = tt * tt;
        const float a = (1 + rq - tq + D) / (2 * r);
        const float b = (1 - rq + tq + D) / (2 * tt);

        const float bNm1 = std::pow(b, (N - 1));
        const float bN2 = bNm1 * bNm1;
        const float a2 = a * a;
        denom = a2 * bN2 - 1;
        float Rsub = a * (bN2 - 1) / denom;
        float Tsub = bNm1 * (a2 - 1) / denom;

        // 特殊分支：r + tt >= 1 时上面的公式分母退化，改用 tt / (tt + (1-tt)(N-1)) 的极限形式。
        if (r + tt >= 1) {
            Tsub = tt / (tt + (1 - tt) * (N - 1));
            Rsub = 1 - Tsub;
        }

        // 步骤 4：与第一层（含表面）合并，得到叶片总透射率与反射率。
        denom = 1 - Rsub * r;
        transmittance[i] = Ta * Tsub / denom;
        reflectance[i] = Ra + Ta * Rsub * tt / denom;
    }
}

// 模块：BSM（Brightness-Shape-Moisture）土壤反射率（SCOPE soilopt）。
// 输入：SoilOptics（brightness 亮度 B；latitude/longitude 是 BSM 光谱形状参数，
//       不是地理经纬度）与体积含水量 moisture（内部限幅到 0.05-0.55）。
// 输出：reflectance（与系数表等长，即 kSpectralCount 个值）。
void bsm(const OptCoeff& coeff, const SoilOptics& soil, float moisture, std::vector<float>& reflectance)
{
    const float B = soil.brightness;
    const float SMCp = 0.25f;
    const float film = 0.015f;
    const float rd = static_cast<float>(3.1415926 / 180.0);
    const float f1 = B * std::sin(soil.latitude * rd);
    const float f2 = B * std::cos(soil.latitude * rd) * std::sin(soil.longitude * rd);
    const float f3 = B * std::cos(soil.latitude * rd) * std::cos(soil.longitude * rd);
    const size_t count = coeff.gsv1.size();
    reflectance.assign(count, 0.0f);
    for (size_t k = 0; k < count; k++) {
        // 步骤 1：干土反射率 rdry = 三条 GSV 基函数的加权和（权重来自 B 与 latitude/longitude）。
        // 步骤 2：由 rdry 与水的折射率 nw 得到水膜背面反射 rbac、水膜内界面反射 p 与水面反射 Rw。
        const float rdry = coeff.gsv1[k] * f1 + coeff.gsv2[k] * f2 + coeff.gsv3[k] * f3;
        const float rbac = 1 - (1 - rdry) * (rdry * calctav(90, static_cast<float>(2.0 / coeff.nw[k])) / calctav(90, 2.0f) + 1 - rdry);
        const float p = 1 - calctav(90, coeff.nw[k]) / coeff.nw[k] / coeff.nw[k];
        const float Rw = 1 - calctav(40, coeff.nw[k]);
        // 步骤 3：含水量限幅到 [0.05, 0.55]，归一化 mu = (water - 0.05) / SMCp（SMCp = 0.25）。
        const float water = std::clamp(moisture, 0.05f, 0.55f);
        const float mu = static_cast<float>((water - 0.05) / SMCp);
        // SCOPE BSM: soil moisture at or below 5 % leaves the dry spectrum unchanged.
        if (mu <= 0) {
            reflectance[k] = rdry;
            continue;
        }
        // SCOPE soilwat (2026-11-20, as TiRT base/soilopt.py _soilwat): Poisson mixture of k = 0..6 water films,
        // tw = exp(-2 kw film k) is the two-way film transmittance and enters numerator and denominator of
        // Rwet_k. The former closed form (single-pass tw, tw missing in Radd, no 6-film truncation) gave SWIR
        // reflectances 10-20 % too high for wet soil.
        double total = static_cast<double>(rdry) * std::exp(-static_cast<double>(mu));
        for (int films = 1; films <= 6; ++films) {
            const double weight = std::exp(-static_cast<double>(mu) + films * std::log(static_cast<double>(mu)) - std::lgamma(films + 1.0));
            const double tw = std::exp(-2.0 * coeff.kw[k] * film * films);
            const double wet = Rw + (1.0 - Rw) * (1.0 - p) * tw * rbac / (1.0 - p * tw * rbac);
            total += weight * wet;
        }
        reflectance[k] = static_cast<float>(total);
    }
}

}  // namespace stream
