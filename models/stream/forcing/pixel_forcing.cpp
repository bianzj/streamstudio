#include "pixel_forcing.h"
#include "utc.h"
#include "../physics/constants.h"

#include <algorithm>
#include <cmath>

namespace stream {

double approximateCosZenith(double latitudeDeg, double longitudeDeg, double unixSeconds)
{
    const double wholeDays = std::floor(unixSeconds / 86400.0);
    int year;
    unsigned month, day;
    civilFromDays(static_cast<int64_t>(wholeDays), year, month, day);
    const double dayIndex = wholeDays - static_cast<double>(daysFromCivil(year, 1, 1));
    const double minutes = (unixSeconds - wholeDays * 86400.0) / 60.0;
    const double gamma = 2.0 * kPi / (isLeapYear(year) ? 366.0 : 365.0) * (dayIndex + (minutes / 60.0 - 12.0) / 24.0);
    const double equation = 229.18 * (0.000075 + 0.001868 * std::cos(gamma) - 0.032077 * std::sin(gamma) - 0.014615 * std::cos(2 * gamma) -
                                      0.040849 * std::sin(2 * gamma));
    const double declination = 0.006918 - 0.399912 * std::cos(gamma) + 0.070257 * std::sin(gamma) - 0.006758 * std::cos(2 * gamma) +
                               0.000907 * std::sin(2 * gamma) - 0.002697 * std::cos(3 * gamma) + 0.00148 * std::sin(3 * gamma);
    const double solarMinutes = minutes + equation + 4.0 * longitudeDeg;
    const double hourAngle = (solarMinutes / 4.0 - 180.0) * kPi / 180.0;
    const double latitude = latitudeDeg * kPi / 180.0;
    return std::sin(latitude) * std::sin(declination) + std::cos(latitude) * std::cos(declination) * std::cos(hourAngle);
}

double approximateSunAzimuth(double latitudeDeg, double longitudeDeg, double unixSeconds)
{
    const double wholeDays = std::floor(unixSeconds / 86400.0);
    int year;
    unsigned month, day;
    civilFromDays(static_cast<int64_t>(wholeDays), year, month, day);
    const double dayIndex = wholeDays - static_cast<double>(daysFromCivil(year, 1, 1));
    const double minutes = (unixSeconds - wholeDays * 86400.0) / 60.0;
    const double gamma = 2.0 * kPi / (isLeapYear(year) ? 366.0 : 365.0) * (dayIndex + (minutes / 60.0 - 12.0) / 24.0);
    const double equation = 229.18 * (0.000075 + 0.001868 * std::cos(gamma) - 0.032077 * std::sin(gamma) - 0.014615 * std::cos(2 * gamma) -
                                      0.040849 * std::sin(2 * gamma));
    const double declination = 0.006918 - 0.399912 * std::cos(gamma) + 0.070257 * std::sin(gamma) - 0.006758 * std::cos(2 * gamma) +
                               0.000907 * std::sin(2 * gamma) - 0.002697 * std::cos(3 * gamma) + 0.00148 * std::sin(3 * gamma);
    const double solarMinutes = minutes + equation + 4.0 * longitudeDeg;
    const double hourAngle = (solarMinutes / 4.0 - 180.0) * kPi / 180.0;
    const double latitude = latitudeDeg * kPi / 180.0;
    const double cosZenith = std::sin(latitude) * std::sin(declination) + std::cos(latitude) * std::cos(declination) * std::cos(hourAngle);
    const double sinZenith = std::sqrt(std::max(0.0, 1.0 - cosZenith * cosZenith));
    if (sinZenith < 1e-6) return 0.0;
    const double sinAzimuth = -std::cos(declination) * std::sin(hourAngle) / sinZenith;
    const double cosAzimuth = (std::sin(declination) - std::sin(latitude) * cosZenith) / (std::cos(latitude) * sinZenith);
    return std::atan2(sinAzimuth, cosAzimuth);
}

PixelForcing::PixelForcing(const era5::Pack& pack, double latitude, double longitude, double elevationShift,
                           double slopeDeg, double aspectDeg, double diffuseFraction,
                           double coneSlopeDeg, double coneDensity,
                           double ridgeSlopeDeg, double ridgeStrikeDeg, double ridgeDensity, double ridgeEndFraction)
    : pack_(pack), weights_(pack.weights(latitude, longitude)), latitude_(latitude), longitude_(longitude), shift_(elevationShift),
      slope_(slopeDeg * kPi / 180.0), aspect_(aspectDeg * kPi / 180.0), diffuse_(diffuseFraction),
      coneSlope_(coneSlopeDeg * kPi / 180.0), coneDensity_(coneDensity),
      ridgeSlope_(ridgeSlopeDeg * kPi / 180.0), ridgeStrike_(ridgeStrikeDeg * kPi / 180.0),
      ridgeDensity_(ridgeDensity), ridgeEndFraction_(ridgeEndFraction)
{
}

double PixelForcing::instant(const std::string& variable, double unixSeconds) const
{
    const double hours = unixSeconds / 3600.0;
    const int64_t hour = static_cast<int64_t>(std::floor(hours));
    const double fraction = hours - hour;
    const double a = pack_.at(variable, hour, weights_);
    if (fraction < 1e-9) return a;
    const double b = pack_.at(variable, hour + 1, weights_);
    return (1 - fraction) * a + fraction * b;
}

double PixelForcing::temperature(const std::string& variable, double unixSeconds) const
{
    return instant(variable, unixSeconds) - 0.0065 * shift_;
}

double PixelForcing::state(const std::string& variable, double unixSeconds) const { return instant(variable, unixSeconds); }

// Hourly flux of hour k covers (k-1, k] and is placed at its centre k - 0.5.
double PixelForcing::longwave(double unixSeconds) const
{
    const double centre = unixSeconds / 3600.0 + 0.5;
    const int64_t k = static_cast<int64_t>(std::floor(centre));
    const double f = centre - k;
    return (1 - f) * pack_.hourlyFlux("strd", k, weights_) + f * pack_.hourlyFlux("strd", k + 1, weights_);
}

// Interpolates the clearness F / cos(z) between hour centres so the diurnal cycle follows the sun.
double PixelForcing::shortwave(double unixSeconds) const
{
    const double mu = approximateCosZenith(latitude_, longitude_, unixSeconds);
    if (mu <= 0.0) return 0.0;
    const double centre = unixSeconds / 3600.0 + 0.5;
    const int64_t k = static_cast<int64_t>(std::floor(centre));
    const double f = centre - k;
    const double fluxA = pack_.hourlyFlux("ssrd", k, weights_), fluxB = pack_.hourlyFlux("ssrd", k + 1, weights_);
    const double muA = approximateCosZenith(latitude_, longitude_, (k - 0.5) * 3600.0);
    const double muB = approximateCosZenith(latitude_, longitude_, (k + 0.5) * 3600.0);
    const bool validA = muA > 0.05 && std::isfinite(fluxA), validB = muB > 0.05 && std::isfinite(fluxB);
    double value;
    if (validA && validB) value = ((1 - f) * fluxA / muA + f * fluxB / muB) * mu;
    else if (validA) value = fluxA / muA * mu;
    else if (validB) value = fluxB / muB * mu;
    else value = (1 - f) * fluxA + f * fluxB;
    if (coneDensity_ > 0.0 && mu > 0.0) {
        // Statistical cone: facets of slope beta uniformly distributed in azimuth; density d of the pixel
        // is covered by cones and the rest is flat. The direct term averages max(0, cosI) over the facets.
        const double a = std::cos(coneSlope_) * mu;
        const double b = std::sin(coneSlope_) * std::sqrt(std::max(0.0, 1.0 - mu * mu));
        double average = std::max(0.0, a);
        if (b > 1e-12) {
            const double ratio = -a / b;
            if (ratio <= -1.0) average = a;
            else if (ratio < 1.0) {
                const double phi0 = std::acos(ratio);
                average = (a * phi0 + b * std::sin(phi0)) / kPi;
            } else average = 0.0;
        }
        const double direct = (coneDensity_ * average + (1.0 - coneDensity_) * mu) / mu;
        const double skyView = 0.5 * (1.0 + std::cos(coneSlope_));
        const double diffuse = coneDensity_ * skyView + (1.0 - coneDensity_);
        value *= (1.0 - diffuse_) * direct + diffuse_ * diffuse;
        return std::clamp(value, 0.0, 1400.0);
    }
    if (ridgeDensity_ > 0.0 && mu > 0.0) {
        // Ridge: two flanks (slope beta, facing strike ± 90°) plus two vertical end faces (facing strike
        // and strike + 180°). endFraction e is the end-face share of the ridge surface; density d is
        // the ridge footprint coverage, the rest is flat.
        const double azimuth = approximateSunAzimuth(latitude_, longitude_, unixSeconds);
        const double sinZenith = std::sqrt(std::max(0.0, 1.0 - mu * mu));
        const double endFraction = std::clamp(ridgeEndFraction_, 0.0, 0.5);
        double sum = 0.0;
        for (int side = 0; side < 2; ++side) {
            const double phi = ridgeStrike_ + (side == 0 ? kPi / 2.0 : -kPi / 2.0);
            const double cosIncidence = std::cos(ridgeSlope_) * mu +
                std::sin(ridgeSlope_) * sinZenith * std::cos(phi - azimuth);
            sum += 0.5 * (1.0 - endFraction) * std::max(0.0, cosIncidence);
        }
        for (int end = 0; end < 2; ++end) {
            const double phi = ridgeStrike_ + (end == 0 ? 0.0 : kPi);
            const double cosIncidence = sinZenith * std::cos(phi - azimuth);
            sum += 0.5 * endFraction * std::max(0.0, cosIncidence);
        }
        const double direct = (ridgeDensity_ * sum + (1.0 - ridgeDensity_) * mu) / mu;
        const double skyView = (1.0 - endFraction) * 0.5 * (1.0 + std::cos(ridgeSlope_)) + endFraction * 0.5;
        const double diffuse = ridgeDensity_ * skyView + (1.0 - ridgeDensity_);
        value *= (1.0 - diffuse_) * direct + diffuse_ * diffuse;
        return std::clamp(value, 0.0, 1400.0);
    }
    if (slope_ > 1e-6) {
        // Tilt the beam by the surface normal and scale the diffuse part by the sky-view factor.
        const double azimuth = approximateSunAzimuth(latitude_, longitude_, unixSeconds);
        const double cosIncidence = std::cos(slope_) * mu +
            std::sin(slope_) * std::sqrt(std::max(0.0, 1.0 - mu * mu)) * std::cos(azimuth - aspect_);
        const double directRatio = cosIncidence > 0.0 ? cosIncidence / mu : 0.0;
        const double skyView = 0.5 * (1.0 + std::cos(slope_));
        value *= (1.0 - diffuse_) * directRatio + diffuse_ * skyView;
        return std::clamp(value, 0.0, 1400.0);
    }
    return std::clamp(value, 0.0, 1400.0 * mu);
}

Forcing PixelForcing::at(double unixSeconds) const
{
    Forcing forcing;
    forcing.ta = temperature("t2m", unixSeconds);
    const double dewpoint = instant("d2m", unixSeconds);
    forcing.ea = std::min(saturationVapourPressure(dewpoint - kKelvin), saturationVapourPressure(forcing.ta - kKelvin));
    forcing.p = instant("sp", unixSeconds) / 100.0 * std::exp(-kGravity * shift_ / (kDryAirGasConstant * forcing.ta));
    const double u = instant("u10", unixSeconds), v = instant("v10", unixSeconds);
    forcing.wind = std::sqrt(u * u + v * v);
    forcing.shortwave = shortwave(unixSeconds);
    forcing.longwave = longwave(unixSeconds);
    return forcing;
}

}  // namespace stream
