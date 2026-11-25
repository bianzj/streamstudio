// ==========================================================================
// 文件：canopy.h
// 功能：均匀冠层闭合的声明：四组分可见比例、球面叶片一次散射系数、冠层方向反射率
// 主要接口：
//   1. FourFractions(T)：光照/阴影土壤与叶片的可见面积比例
//   2. canopyFractions(T)：由 LAI、热点、太阳/观测几何计算四组分比例
//   3. sphericalCoefficients：球面叶片一次散射系数
//   4. canopyReflectance(T)：冠层顶方向反射率（一次散射 + hom3 多次散射）
// 关系：实现见 canopy.cpp；模板版本在 canopy.cpp 末尾显式实例化为 double 与 Ad
// 约定：角度为度；相对方位 0° 为热点方向（观测与太阳同方位）
// ==========================================================================

#pragma once
#include "dual.h"

namespace stream {

// 结构：四组分比例，有叶时 soilSun + soilShade + leafSun + leafShade = 1。
// Sunlit/shaded soil and leaf fractions of one view direction (TiRT standard homogeneous closure).
template <class T> struct FourFractionsT { T soilSun{}, soilShade{}, leafSun{}, leafShade{}; };
using FourFractions = FourFractionsT<double>;

FourFractions canopyFractions(double lai, double hotspot, double sunZenithDeg, double viewZenithDeg,
                              double relativeAzimuthDeg);
// Same closure on any scalar type (double or Ad): differentiable with respect to LAI.
template <class T>
FourFractionsT<T> canopyFractionsT(const T& lai, double hotspot, double sunZenithDeg, double viewZenithDeg,
                                   double relativeAzimuthDeg);

// Single-scattering coefficients of the spherical-leaf closure (TiRT vnir.spherical_coefficients).
struct SphericalCoefficients { double ks = 0, ko = 0, sob = 0, sof = 0; };
SphericalCoefficients sphericalCoefficients(double sunZenithDeg, double viewZenithDeg, double relativeAzimuthDeg);

// BRF / HDRF of a plane homogeneous canopy: direct single scattering plus all scattering orders.
double canopyReflectance(double lai, double hotspot, double sunZenithDeg, double sunAzimuthDeg,
                         double viewZenithDeg, double viewAzimuthDeg, double leafReflectance,
                         double leafTransmittance, double soilReflectance, double diffuseFraction, bool sunlit);
// Differentiable with respect to LAI and the soil reflectance (which carries the soil-moisture derivative).
template <class T>
T canopyReflectanceT(const T& lai, double hotspot, double sunZenithDeg, double sunAzimuthDeg, double viewZenithDeg,
                     double viewAzimuthDeg, double leafReflectance, double leafTransmittance, const T& soilReflectance,
                     double diffuseFraction, bool sunlit);

}  // namespace stream
