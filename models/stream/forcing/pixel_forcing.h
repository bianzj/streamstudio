// ==========================================================================
// 文件：pixel_forcing.h
// 功能：单像元气象强迫（PixelForcing）与接收面短波结果（SurfaceShortwave）的声明
// 主要模块：
//   1. approximateCosZenith / approximateSunAzimuth：NOAA 近似太阳位置（实现在 pixel_forcing.cpp）
//   2. SurfaceShortwave：地形下接收面短波、漫射占比与有效入射余弦
//   3. PixelForcing：在 ForcingSource 上取像元插值权重，给出任意时刻的 Forcing 与接收面短波
// 数据流：ForcingSource（标准量，整点）→ PixelForcing.at / surfaceShortwave → physics/energy 的 EB 求解
// 约定：时间为 Unix 秒（UTC）；构造参数中的角度为度、内部成员为弧度；方位角自北顺时针；
//   elevationShift 为像元高程减数据源海拔（m）；辐射 W m-2
// 注意：水平像元保持旧算法逐位不变（tilted=false）；surfaceShortwave 有 NOAA 与 SPA 两个入口
//   （FIX 2026-11-21 C10），后者用调用方的太阳位置
// 与其他文件：实现见 pixel_forcing.cpp；数据接口见 forcing_source.h；
//   漫射份额与晴空 DNI 上限见 physics/shortwave.h
// ==========================================================================

#pragma once
#include "forcing_source.h"
#include "../physics/energy.h"

namespace stream {

// 模块：太阳位置的近似函数（只用于强迫的时间插值与旧路径，不用于 EB 步进的主太阳位置）。
// Cosine of the solar zenith angle from the NOAA general solar position formulas; only used to
// interpolate hourly shortwave between hour centres.
double approximateCosZenith(double latitudeDeg, double longitudeDeg, double unixSeconds);

// Solar azimuth (radians clockwise from north) from the same NOAA approximation.
double approximateSunAzimuth(double latitudeDeg, double longitudeDeg, double unixSeconds);

// Shortwave on the receiving surface (2026-11-20). The horizontal ERA5 flux is split into direct/diffuse first
// (configured or Erbs fraction, clear-sky DNI limit at the true solar zenith), then the direct part is projected
// by the local incidence and the diffuse part by the sky-view factor. The energy balance uses `global` with the
// fixed `diffuseShare` and the incidence angle acos(cosIncidence) for the canopy beam geometry, so the DNI limit
// and the sunlit/shaded split follow the slope. A horizontal surface keeps the old path exactly (tilted = false).
struct SurfaceShortwave {
    double horizontal = 0.0;                 // W m-2, horizontal global (Forcing::shortwave)
    double horizontalDiffuseFraction = 0.0;  // admissible horizontal diffuse fraction
    bool limited = false;                    // clear-sky DNI limit raised the diffuse fraction
    bool tilted = false;                     // a terrain mode is active for this pixel
    double global = 0.0;                     // W m-2 on the surface (per unit surface area)
    double diffuseShare = 0.0;               // diffuse part of `global`
    double cosIncidence = 0.0;               // effective cosine of the beam incidence on the surface (0 = beam-free)
};

// Surface forcing of one pixel interpolated from an ERA5 pack. elevationShift is the pixel height
// above the ERA5 orography (m); temperatures follow a 6.5 K/km lapse rate and pressure the hypsometric law.
// slopeDeg/aspectDeg describe the receiving surface (0 = horizontal); surfaceShortwave() tilts the beam by the
// direct-beam incidence ratio and the diffuse part by the sky-view factor. Forcing::shortwave is horizontal.
class PixelForcing {
public:
    PixelForcing(const ForcingSource& pack, double latitude, double longitude, double elevationShift,
                 double slopeDeg = 0.0, double aspectDeg = 0.0, double diffuseFraction = 0.2,
                 double coneSlopeDeg = 0.0, double coneDensity = 0.0,
                 double ridgeSlopeDeg = 0.0, double ridgeStrikeDeg = 0.0, double ridgeDensity = 0.0,
                 double ridgeEndFraction = 0.0);
    Forcing at(double unixSeconds) const;
    // Instantaneous temperature-like (K) or other state variable linearly interpolated in time.
    double temperature(const std::string& variable, double unixSeconds) const;
    double state(const std::string& variable, double unixSeconds) const;
    bool tilted() const { return coneDensity_ > 0.0 || ridgeDensity_ > 0.0 || slope_ > 1e-6; }
    SurfaceShortwave surfaceShortwave(double unixSeconds, double szaDeg, int dayOfYear, bool enforceLimit) const;
    // FIX(2026-11-21 C10): same, with the solar position of the caller (NREL SPA zenith / azimuth, degrees) instead of
    // the NOAA approximation for the instantaneous cos(z), the tilt and the sign test, so "sun above the horizon"
    // means the same thing for the flux and for the canopy beam geometry. The hour-centre normalisation of the
    // hourly ERA5 means keeps the NOAA cosine.
    SurfaceShortwave surfaceShortwave(double unixSeconds, double szaDeg, int dayOfYear, bool enforceLimit,
                                      double saaDeg) const;

private:
    const ForcingSource& pack_;
    ForcingWeights weights_;
    double latitude_, longitude_, shift_;
    double slope_ = 0.0;   // radians
    double aspect_ = 0.0;  // radians, clockwise from north
    double diffuse_ = 0.2;
    double coneSlope_ = 0.0;   // radians, cone facet slope
    double coneDensity_ = 0.0; // 0-1, fraction covered by cones
    double ridgeSlope_ = 0.0;  // radians, ridge flank slope
    double ridgeStrike_ = 0.0; // radians, ridge strike (flanks face strike ± 90)
    double ridgeDensity_ = 0.0; // 0-1, fraction covered by ridges
    double ridgeEndFraction_ = 0.0; // end-face share; 0 = long ridge
    double instant(const std::string& variable, double unixSeconds) const;
    double longwave(double unixSeconds) const;
    double shortwave(double unixSeconds) const;
    double shortwave(double unixSeconds, double mu) const;  // FIX(2026-11-21 C10): given instantaneous cos(z)
    SurfaceShortwave surfaceShortwave(double unixSeconds, double szaDeg, int dayOfYear, bool enforceLimit, double mu,
                                      double azimuthRad) const;  // FIX(2026-11-21 C10): shared implementation
};

}  // namespace stream
