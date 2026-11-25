// ==========================================================================
// 文件：shortwave_split.h
// 功能：把气象节点的水平总短波 Rin 拆分为直射/散射两部分（light.diffuseFractionMode），
//       供 VoxelEB 与 FacetEB 共用（header-only）
// 主要模块：
//   1. 模式常量与 parseMode：解析 fixed / erbs 两种模式
//   2. eccentricityFactor：Spencer (1971) 日地距离修正，用于大气顶水平辐照
//   3. erbsDiffuseFraction：Erbs et al. (1982) 逐时散射比 kd(kt) 分段多项式
//   4. split：按模式返回直射比例、晴空指数 kt 及来源（fixed / measured / erbs）
// 数据流：fileio.cpp / fileio_json.cpp 调 parseMode 得 diffuseFractionMode
//         → voxeleb.cpp、facetrtcore.cpp 逐气象节点调 split → 得到该节点的直射比例
// 约定：Rin、实测直射/散射为水平面 W m-2；太阳天顶角单位为度；directFraction 为 0..1
// 注意：模式 fixed 完全保持旧工程行为（直接用配置的 directScatteringRatio）
// ==========================================================================

#pragma once
// Direct/diffuse split of a measured global shortwave Rin per meteorological
// node (light.diffuseFractionMode), shared by VoxelEB and FacetEB.
//
//   fixed : the configured directScatteringRatio for every node (legacy).
//   erbs  : measured horizontal direct/diffuse columns of the meteorology file
//           when present, otherwise the Erbs et al. (1982) diffuse fraction of
//           the clearness index kt = Rin / (Isc * E0 * cos(theta_s)).
//           With the sun below 1 degree elevation, or Rin <= 0, all shortwave
//           is diffuse.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

// 命名空间：短波直射/散射拆分。
namespace shortwave_split {

// 模式编码：0 = fixed（固定比例，旧工程缺省），1 = erbs。
constexpr int kModeFixed = 0;
constexpr int kModeErbs = 1;

// Solar constant used with the Erbs correlation (W m-2).
constexpr double kSolarConstant = 1367.0;
// cos(theta_s) below which (sun elevation < 1 degree) the split is all diffuse.
constexpr double kLowSunCosine = 0.0175;

// 作用：解析 diffuseFractionMode 文本；去掉空白并转小写后等于 "erbs" 才返回 kModeErbs，
// 其余（含空串、拼写错误）一律退回 kModeFixed，以兼容旧工程。
inline int parseMode(std::string text)
{
    text.erase(std::remove_if(text.begin(), text.end(),
                              [](unsigned char c) { return std::isspace(c) != 0; }),
               text.end());
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text == "erbs" ? kModeErbs : kModeFixed;
}

// Spencer (1971) eccentricity correction (r0/r)^2 for day of year 1..366.
// 实现：gamma = 2π(doy-1)/365 的傅里叶级数；doy 限幅到 1..366。
inline double eccentricityFactor(int dayOfYear)
{
    const double gamma = 2.0 * 3.14159265358979323846 * (std::clamp(dayOfYear, 1, 366) - 1) / 365.0;
    return 1.000110 + 0.034221 * std::cos(gamma) + 0.001280 * std::sin(gamma) +
        0.000719 * std::cos(2.0 * gamma) + 0.000077 * std::sin(2.0 * gamma);
}

// Erbs et al. (1982) hourly diffuse fraction kd(kt).
inline double erbsDiffuseFraction(double kt)
{
    if (kt <= 0.22) return 1.0 - 0.09 * std::max(kt, 0.0);
    if (kt <= 0.80)
        return 0.9511 - 0.1604 * kt + 4.388 * kt * kt - 16.638 * kt * kt * kt +
            12.336 * kt * kt * kt * kt;
    return 0.165;
}

// 结果：directFraction 为直射占总短波的比例（0..1）；clearnessIndex 为晴空指数 kt；
// source 标明来源（"fixed" / "measured" / "erbs"），供日志输出使用。
struct Split {
    float directFraction{0};
    float clearnessIndex{-1};  // < 0 when not evaluated
    const char* source{"fixed"};
};

// measuredDirect/measuredDiffuse: horizontal W m-2 columns of the meteorology
// row, NaN when the file has none.
// 作用：求一个气象节点的直射比例。
// 输入：模式、fixed 模式使用的直射比例、总短波 rin（W m-2）、太阳天顶角（度）、年积日、
//       实测水平直射/散射（W m-2，无实测列时为 NaN）。
// 步骤：fixed → 直接返回限幅后的配置值；erbs → 优先用实测直射/散射之比（measured），
//       否则按 kt 由 Erbs 公式求散射比。
inline Split split(int mode, float fixedDirectFraction, float rin, float solarZenithDegrees,
                   int dayOfYear, float measuredDirect, float measuredDiffuse)
{
    Split result;
    if (mode != kModeErbs) {
        result.directFraction = std::clamp(fixedDirectFraction, 0.0f, 1.0f);
        return result;
    }
    // 步骤 1：太阳天顶角余弦（水平面 cos(theta_s)）。
    const double cosine = std::cos(solarZenithDegrees * 3.14159265358979323846 / 180.0);
    // 步骤 2：气象文件提供了有效的实测直射/散射列时，直接用二者之比；Rin <= 0 或两者之和为 0 时
    // 视为全散射。晴空指数仅在太阳高度角 >= 约 1 度时计算，避免除以接近 0 的余弦。
    if (std::isfinite(measuredDirect) && std::isfinite(measuredDiffuse) &&
        measuredDirect >= 0.0f && measuredDiffuse >= 0.0f) {
        result.source = "measured";
        const double sum = double(measuredDirect) + double(measuredDiffuse);
        result.directFraction = sum > 0.0 && rin > 0.0f
            ? static_cast<float>(std::clamp(double(measuredDirect) / sum, 0.0, 1.0)) : 0.0f;
        if (cosine > kLowSunCosine)
            result.clearnessIndex = static_cast<float>(
                std::max(0.0, double(rin)) / (kSolarConstant * eccentricityFactor(dayOfYear) * cosine));
        return result;
    }
    // 步骤 3：无实测列时用 Erbs 关系；无短波或太阳低于约 1 度时全部视为散射（直射比例 0）。
    result.source = "erbs";
    if (!(rin > 0.0f) || !(cosine > kLowSunCosine)) {
        result.directFraction = 0.0f;
        result.clearnessIndex = 0.0f;
        return result;
    }
    // kt = Rin / (Isc * E0 * cos(theta_s))，即地面总短波与大气顶水平辐照之比；
    // 直射比例 = 1 - kd(kt)。
    const double kt = double(rin) / (kSolarConstant * eccentricityFactor(dayOfYear) * cosine);
    result.clearnessIndex = static_cast<float>(kt);
    result.directFraction = static_cast<float>(std::clamp(1.0 - erbsDiffuseFraction(kt), 0.0, 1.0));
    return result;
}

}  // namespace shortwave_split
