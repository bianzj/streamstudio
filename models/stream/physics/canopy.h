#pragma once

namespace stream {

// Sunlit/shaded soil and leaf fractions of one view direction (TiRT standard homogeneous closure).
struct FourFractions {
    double soilSun = 0, soilShade = 0, leafSun = 0, leafShade = 0;
};

FourFractions canopyFractions(double lai, double hotspot, double sunZenithDeg, double viewZenithDeg,
                              double relativeAzimuthDeg);

// Single-scattering coefficients of the spherical-leaf closure (TiRT vnir.spherical_coefficients).
struct SphericalCoefficients { double ks = 0, ko = 0, sob = 0, sof = 0; };
SphericalCoefficients sphericalCoefficients(double sunZenithDeg, double viewZenithDeg, double relativeAzimuthDeg);

// BRF / HDRF of a plane homogeneous canopy: direct single scattering plus all scattering orders.
double canopyReflectance(double lai, double hotspot, double sunZenithDeg, double sunAzimuthDeg,
                         double viewZenithDeg, double viewAzimuthDeg, double leafReflectance,
                         double leafTransmittance, double soilReflectance, double diffuseFraction, bool sunlit);

}  // namespace stream
