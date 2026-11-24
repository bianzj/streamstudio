#include "pixel_forcing.h"
#include "utc.h"
#include "../physics/constants.h"
#include "../physics/shortwave.h"

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

PixelForcing::PixelForcing(const ForcingSource& pack, double latitude, double longitude, double elevationShift,
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
    return instant(variable, unixSeconds) - pack_.lapseRate() * shift_;
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
    return shortwave(unixSeconds, approximateCosZenith(latitude_, longitude_, unixSeconds));
}

// FIX(2026-11-21 C10): the instantaneous cos(z) is a parameter so the EB march can use the SPA sun of its nodes.
double PixelForcing::shortwave(double unixSeconds, double mu) const
{
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
    // Horizontal global shortwave. Terrain tilting is done in surfaceShortwave() after the direct/diffuse split.
    return std::clamp(value, 0.0, 1400.0 * mu);
}

SurfaceShortwave PixelForcing::surfaceShortwave(double unixSeconds, double szaDeg, int dayOfYear, bool enforceLimit) const
{
    // FIX(2026-11-21 C10): unchanged NOAA behaviour, now through the shared implementation.
    return surfaceShortwave(unixSeconds, szaDeg, dayOfYear, enforceLimit,
                            approximateCosZenith(latitude_, longitude_, unixSeconds),
                            approximateSunAzimuth(latitude_, longitude_, unixSeconds));
}

SurfaceShortwave PixelForcing::surfaceShortwave(double unixSeconds, double szaDeg, int dayOfYear, bool enforceLimit,
                                                double saaDeg) const
{
    // FIX(2026-11-21 C10): one solar position (the caller's SPA) for the flux, its sign and the tilt.
    if (!std::isfinite(saaDeg) || !std::isfinite(szaDeg)) return surfaceShortwave(unixSeconds, szaDeg, dayOfYear, enforceLimit);
    return surfaceShortwave(unixSeconds, szaDeg, dayOfYear, enforceLimit, std::cos(szaDeg * kPi / 180.0), saaDeg * kPi / 180.0);
}

SurfaceShortwave PixelForcing::surfaceShortwave(double unixSeconds, double szaDeg, int dayOfYear, bool enforceLimit,
                                                double mu, double sunAzimuth) const
{
    SurfaceShortwave out;
    out.horizontal = shortwave(unixSeconds, mu);  // FIX(2026-11-21 C10): cos(z) of the selected solar position
    out.global = out.horizontal;
    out.cosIncidence = std::max(std::cos(szaDeg * kPi / 180.0), 0.0);
    if (!tilted()) return out;  // horizontal surface: absorbShortwave keeps doing the split (unchanged path)
    out.tilted = true;
    const double requested = requestedDiffuseFraction(out.horizontal, szaDeg, diffuse_, dayOfYear);  // FIX(2026-11-21 C7)
    out.horizontalDiffuseFraction = admissibleDiffuseFraction(out.horizontal, szaDeg, dayOfYear, diffuse_, enforceLimit);
    out.limited = out.horizontalDiffuseFraction > requested + 1e-12;
    // FIX(2026-11-21 C10): mu is the parameter (NOAA in the legacy overload, SPA from the EB march).
    if (!(out.horizontal > 0.0) || !(mu > 0.0)) {
        out.global = 0.0;
        out.diffuseShare = 1.0;
        out.cosIncidence = 0.0;
        return out;
    }
    // Area-mean max(0, cos i) of the receiving surface (NOAA sun, as the old tilt) and its sky-view factor.
    double meanCos = 0.0, skyTerm = 1.0;
    const double sinZenith = std::sqrt(std::max(0.0, 1.0 - mu * mu));
    if (coneDensity_ > 0.0) {
        // Statistical cone: facets of slope beta uniformly distributed in azimuth; density d of the pixel
        // is covered by cones and the rest is flat. The direct term averages max(0, cosI) over the facets.
        const double a = std::cos(coneSlope_) * mu;
        const double b = std::sin(coneSlope_) * sinZenith;
        double average = std::max(0.0, a);
        if (b > 1e-12) {
            const double ratio = -a / b;
            if (ratio <= -1.0) average = a;
            else if (ratio < 1.0) {
                const double phi0 = std::acos(ratio);
                average = (a * phi0 + b * std::sin(phi0)) / kPi;
            } else average = 0.0;
        }
        meanCos = coneDensity_ * average + (1.0 - coneDensity_) * mu;
        skyTerm = coneDensity_ * 0.5 * (1.0 + std::cos(coneSlope_)) + (1.0 - coneDensity_);
    } else if (ridgeDensity_ > 0.0) {
        // Ridge: two flanks (slope beta, facing strike ± 90°) plus two vertical end faces (facing strike
        // and strike + 180°). endFraction e is the end-face share of the ridge surface; density d is
        // the ridge footprint coverage, the rest is flat.
        const double azimuth = sunAzimuth;  // FIX(2026-11-21 C10): solar azimuth of the selected solar position
        const double endFraction = std::clamp(ridgeEndFraction_, 0.0, 0.5);
        double sum = 0.0;
        for (int side = 0; side < 2; ++side) {
            const double phi = ridgeStrike_ + (side == 0 ? kPi / 2.0 : -kPi / 2.0);
            sum += 0.5 * (1.0 - endFraction) * std::max(0.0, std::cos(ridgeSlope_) * mu + std::sin(ridgeSlope_) * sinZenith * std::cos(phi - azimuth));
        }
        for (int end = 0; end < 2; ++end) {
            const double phi = ridgeStrike_ + (end == 0 ? 0.0 : kPi);
            sum += 0.5 * endFraction * std::max(0.0, sinZenith * std::cos(phi - azimuth));
        }
        meanCos = ridgeDensity_ * sum + (1.0 - ridgeDensity_) * mu;
        const double skyView = (1.0 - endFraction) * 0.5 * (1.0 + std::cos(ridgeSlope_)) + endFraction * 0.5;
        skyTerm = ridgeDensity_ * skyView + (1.0 - ridgeDensity_);
    } else {
        // One plane: beam by the surface normal, diffuse by the sky-view factor.
        const double azimuth = sunAzimuth;  // FIX(2026-11-21 C10): solar azimuth of the selected solar position
        meanCos = std::max(0.0, std::cos(slope_) * mu + std::sin(slope_) * sinZenith * std::cos(azimuth - aspect_));
        skyTerm = 0.5 * (1.0 + std::cos(slope_));
    }
    const double fd = out.horizontalDiffuseFraction;
    double direct = out.horizontal * (1.0 - fd) * meanCos / mu;  // beam normal flux × mean cos i
    double diffuse = out.horizontal * fd * skyTerm;
    const double total = direct + diffuse;
    if (total > 1400.0) { direct *= 1400.0 / total; diffuse *= 1400.0 / total; }
    out.global = direct + diffuse;
    out.diffuseShare = out.global > 0.0 ? diffuse / out.global : 1.0;
    out.cosIncidence = direct > 0.0 ? std::clamp(meanCos, 0.0, 1.0) : 0.0;
    return out;
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
