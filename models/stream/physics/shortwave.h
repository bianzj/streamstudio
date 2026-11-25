// ==========================================================================
// 文件：shortwave.h
// 功能：短波分配与吸收的接口声明：太阳谱分组 SolarBins、漫射份额函数、各组分吸收结果结构。
// 主要模块：
//   1. SolarBins / makeSolarBins / binSpectrum：1 nm 太阳谱分组与光谱组内平均
//   2. OpticalSpectra：每组的叶片反射/透射与土壤反射率（指针，不拥有数据）
//   3. ShortwaveAbsorption 与 absorbShortwave：double 版吸收结果与诊断量
//   4. requestedDiffuseFraction / admissibleDiffuseFraction：Erbs 反算与晴空 DNI 上限限幅
//   5. ShortwaveComponentsT / shortwaveComponents：任意标量类型（double 或 Ad）的组分吸收
// 关系：实现在 shortwave.cpp，节点辐射度来自 radiosity.h；energy.cpp 与成像/同化代码调用。
// 约定：吸收量为每单位组分面积的 W m-2；PAR 量为 μmol m-2 s-1；天顶角为度。
// ==========================================================================

#pragma once
#include "dual.h"

#include <utility>
#include <vector>

namespace stream {

// 结构体：太阳光谱分组。direct/diffuse 为各组直射/天空光谱形状的积分权重，photon 为 PAR 组的
// μmol J-1 换算系数（非 PAR 组为 0），directTotal/diffuseTotal 为全谱权重和（用于归一）。
// Reference solar spectral shapes (fesun / fesky) integrated over contiguous bins of the 1 nm grid.
// Leaf and soil spectra are averaged over the same bins before the node-radiosity solve.
struct SolarBins {
    std::vector<std::pair<int, int>> range;  // [first, last) indices on the 1 nm grid
    std::vector<double> direct, diffuse;     // integrated shapes per bin
    std::vector<double> photon;              // umol J-1 for PAR bins (400-700 nm), 0 elsewhere
    double directTotal = 0.0, diffuseTotal = 0.0;
    size_t size() const { return range.size(); }
};

// 构造分组（binWidthNm = 1 为 1 nm 全谱，缺省配置 50 nm）；binSpectrum 把 1 nm 光谱按同分组取组内平均。
SolarBins makeSolarBins(const std::vector<float>& fesun, const std::vector<float>& fesky, int binWidthNm);
std::vector<float> binSpectrum(const SolarBins& bins, const std::vector<float>& spectrum);

// Per-bin leaf and soil spectra (same bins as the solar shapes).
struct OpticalSpectra {
    const std::vector<float>* leafReflectance = nullptr;
    const std::vector<float>* leafTransmittance = nullptr;
    const std::vector<float>* soilReflectance = nullptr;
};

// 结果结构：字段含义（W m-2，PAR 为 μmol m-2 s-1）：soil/leafDirect 光照部分的直射吸收，
// *DiffuseSun/Shade 光照/阴影组分的漫射吸收，*Mean 面积平均，par* 对应 PAR；
// soilSunlitFraction 光照土壤面积份额，laiSunlit 光照叶面积；albedo 场景反照率；
// diffuseFraction 实际使用的漫射份额（Requested 为请求值）；directNormal(Limit) 法向直射及其晴空上限；
// clearnessIndex 晴空指数；partitionLimited 表示 DNI 上限使直射被转为漫射。
// Absorbed shortwave per unit component area (soil per soil area, leaves per leaf area), as TIRTEB.
struct ShortwaveAbsorption {
    double soilDirect = 0, leafDirect = 0;
    double soilDiffuseSun = 0, soilDiffuseShade = 0;
    double leafDiffuseSun = 0, leafDiffuseShade = 0;
    double soilMean = 0, leafMean = 0;
    double parLeafDirect = 0, parLeafDiffuse = 0, parLeafDiffuseSun = 0, parLeafDiffuseShade = 0;
    double soilSunlitFraction = 0, laiSunlit = 0;
    double albedo = 0;
    double diffuseFraction = 0, diffuseFractionRequested = 0;
    double directNormal = 0, directNormalLimit = 0, clearnessIndex = 0;
    bool partitionLimited = false;
};

// FIX(2026-11-21 C7): the Erbs clearness index includes the orbital (Sun-Earth distance) factor E0 of the day, as
// TiRT radiation.py:218-221. dayOfYear <= 0 uses the perihelion value 1.033 (TiRT without a date); the 3-argument
// form is kept for compatibility and is that no-date case.
// FIX(2026-11-21 C10): with the sun at or below the horizon (SZA >= 90°) there is no beam: the Erbs fraction is 1.
double requestedDiffuseFraction(double globalShortwave, double szaDeg, double configured);
double requestedDiffuseFraction(double globalShortwave, double szaDeg, double configured, int dayOfYear);
// FIX(2026-11-21 C10): SZA >= 90° returns 1 (all diffuse) whether or not the DNI limit is enforced.
double admissibleDiffuseFraction(double globalShortwave, double szaDeg, int dayOfYear, double configured,
                                 bool enforceLimit, double* limit = nullptr);

// 作用：给定全球短波与几何，返回各组分吸收（double 版，实现见 shortwave.cpp）。
ShortwaveAbsorption absorbShortwave(const SolarBins& sun, const OpticalSpectra& spectra, double lai,
                                    double globalShortwave, double szaDeg, int dayOfYear, double requestedFraction,
                                    bool enforceLimit, bool sunlit);

// Component absorption at a fixed diffuse fraction on any scalar type (double or Ad). absorbShortwave uses the
// double instance; the assimilation differentiates it with respect to LAI and the per-bin soil reflectance.
template <class T>
struct ShortwaveComponentsT {
    T soilDirect{}, leafDirect{}, soilDiffuseSun{}, soilDiffuseShade{}, leafDiffuseSun{}, leafDiffuseShade{};
    T soilMean{}, leafMean{}, parLeafDirect{}, parLeafDiffuse{}, parLeafDiffuseSun{}, parLeafDiffuseShade{};
    T soilSunlitFraction{}, laiSunlit{}, absorbed{}, incident{};
};
// soil: per-bin reflectance (std::vector<float> or std::vector<Ad>).
// FIX(2026-11-21 C10): when the receiving surface is not sunlit (sunlit == false or SZA >= 90°) the whole global
// shortwave is distributed as diffuse sky radiation (the direct part used to be dropped), so incident = global.
template <class T, class SoilSpectrum>
ShortwaveComponentsT<T> shortwaveComponents(const SolarBins& sun, const std::vector<float>& leafReflectance,
                                            const std::vector<float>& leafTransmittance, const SoilSpectrum& soil,
                                            const T& lai, double globalShortwave, double diffuseFraction,
                                            double szaDeg, bool sunlit);

}  // namespace stream
