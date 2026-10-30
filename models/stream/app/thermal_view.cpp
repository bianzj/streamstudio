#include "thermal_view.h"
#include "../forcing/utc.h"
#include "../physics/canopy.h"
#include "../physics/spa.h"

#include <cmath>

namespace stream {

Sun solarPosition(double unixSeconds, double latitude, double longitude, double elevation)
{
    const UtcTime time = utcFromSeconds(std::round(unixSeconds));
    spa_data sun{};
    sun.year = time.year; sun.month = time.month; sun.day = time.day;
    sun.hour = time.hour; sun.minute = time.minute; sun.second = static_cast<int>(std::lround(time.second)) % 60;
    sun.timezone = 0;
    sun.delta_t = 69;
    sun.latitude = latitude; sun.longitude = longitude;
    sun.elevation = std::isfinite(elevation) ? elevation : 0;
    sun.pressure = 0;  // geometric position, no refraction
    sun.temperature = 15;
    sun.function = SPA_ZA;
    Sun result;
    if (SPACalc{}.spa_calculate(&sun) != 0) return result;
    result.zenith = sun.zenith;
    result.azimuth = sun.azimuth;
    result.ok = true;
    return result;
}

std::array<double, 4> thermalWeights(const HomCanopy& nodes, double lai, double hotspot, double sunZenithDeg,
                                     double viewZenithDeg, double relativeAzimuthDeg, double leafEmissivity,
                                     double soilEmissivity)
{
    const FourFractions parts = canopyFractions(lai, hotspot, sunZenithDeg, viewZenithDeg, std::abs(relativeAzimuthDeg));
    if (lai > 0.0) {
        const ThermalScatter scatter = homThermalScatter(nodes, leafEmissivity, soilEmissivity, viewZenithDeg, sunZenithDeg);
        return {std::max(0.0, parts.soilSun * soilEmissivity + scatter.soilSun),
                std::max(0.0, parts.soilShade * soilEmissivity + scatter.soilShade),
                std::max(0.0, parts.leafSun * leafEmissivity + scatter.leafSun),
                std::max(0.0, parts.leafShade * leafEmissivity + scatter.leafShade)};
    }
    return {std::max(0.0, parts.soilSun * soilEmissivity), std::max(0.0, parts.soilShade * soilEmissivity), 0.0, 0.0};
}

}  // namespace stream
