#pragma once
#include "era5.h"
#include "../physics/energy.h"

namespace stream {

// Cosine of the solar zenith angle from the NOAA general solar position formulas; only used to
// interpolate hourly shortwave between hour centres.
double approximateCosZenith(double latitudeDeg, double longitudeDeg, double unixSeconds);

// Solar azimuth (radians clockwise from north) from the same NOAA approximation.
double approximateSunAzimuth(double latitudeDeg, double longitudeDeg, double unixSeconds);

// Surface forcing of one pixel interpolated from an ERA5 pack. elevationShift is the pixel height
// above the ERA5 orography (m); temperatures follow a 6.5 K/km lapse rate and pressure the hypsometric law.
// slopeDeg/aspectDeg describe the receiving surface (0 = horizontal); the shortwave is tilted by the
// direct-beam incidence ratio and the sky-view factor of the diffuse part.
class PixelForcing {
public:
    PixelForcing(const era5::Pack& pack, double latitude, double longitude, double elevationShift,
                 double slopeDeg = 0.0, double aspectDeg = 0.0, double diffuseFraction = 0.2,
                 double coneSlopeDeg = 0.0, double coneDensity = 0.0,
                 double ridgeSlopeDeg = 0.0, double ridgeStrikeDeg = 0.0, double ridgeDensity = 0.0,
                 double ridgeEndFraction = 0.0);
    Forcing at(double unixSeconds) const;
    // Instantaneous temperature-like (K) or other state variable linearly interpolated in time.
    double temperature(const std::string& variable, double unixSeconds) const;
    double state(const std::string& variable, double unixSeconds) const;

private:
    const era5::Pack& pack_;
    era5::Weights weights_;
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
};

}  // namespace stream
