#pragma once
#include "dual.h"

namespace stream {

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
