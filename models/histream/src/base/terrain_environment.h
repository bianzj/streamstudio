#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

// Heights are metres above the scene datum (the DEM minimum), not sea level.
// These are prescribed empirical fields, not a soil-water balance model.
struct TerrainEnvironment {
    bool airEnabled{false};
    bool soilEnabled{false};
    float referenceHeight{2.0f};
    float temperatureLapseRate{0.0065f}; // K/m; negative values permit inversion
    float vaporScaleHeight{2000.0f};
    float drainageHeight{0.0f}; // prescribed common stream elevation, NOT routed HAND
    float soilDry{0.08f};       // m3/m3
    float soilValley{0.35f};
    float soilScaleHeight{30.0f};
    float co2ValleyExcess{0.0f}; // prescribed excess ppm, no carbon transport
    float co2ScaleHeight{500.0f};

    void validate() const {
        const float values[] = {referenceHeight, temperatureLapseRate, vaporScaleHeight,
            drainageHeight, soilDry, soilValley, soilScaleHeight, co2ValleyExcess, co2ScaleHeight};
        for (float v : values)
            if (!std::isfinite(v)) throw std::runtime_error("TerrainEnvironment requires finite parameters");
        if (vaporScaleHeight <= 0 || soilScaleHeight <= 0 || co2ScaleHeight <= 0 || co2ValleyExcess < 0 ||
            soilDry < 0 || soilValley < soilDry || soilValley > 1)
            throw std::runtime_error("TerrainEnvironment: positive scale heights and 0 <= soilDry <= soilValley <= 1 required");
    }

    float temperatureC(float referenceC, float height) const {
        return airEnabled ? referenceC - temperatureLapseRate * (height - referenceHeight) : referenceC;
    }
    float co2ExcessPpm(float height) const {
        return co2ValleyExcess * std::exp(-std::max(0.0f, height) / co2ScaleHeight);
    }
    static float saturationHpa(float temperatureC) {
        const float t = std::clamp(temperatureC, -80.0f, 80.0f);
        return 6.107f * std::pow(10.0f, 7.5f * t / (237.3f + t));
    }
    float vaporHpa(float referenceHpa, float height, float temperatureC) const {
        if (!airEnabled) return referenceHpa;
        return std::clamp(referenceHpa * std::exp(std::clamp(
            -(height - referenceHeight) / vaporScaleHeight, -80.0f, 80.0f)),
            0.0f, saturationHpa(temperatureC));
    }
    float soilMoisture(float height, float saturation) const {
        if (!std::isfinite(saturation) || saturation < 0.0f || saturation > 1.0f)
            throw std::runtime_error("TerrainEnvironment requires soil Satwater in m3/m3 within 0..1");
        return std::clamp(soilDry + (soilValley - soilDry) *
            std::exp(-std::max(0.0f, height - drainageHeight) / soilScaleHeight),
            0.0f, std::clamp(saturation, 0.0f, 1.0f));
    }

    void validateAirState(float temperature, float vapor, float minHeight, float maxHeight) const {
        if (!airEnabled) return;
        if (!std::isfinite(vapor) || vapor < 0.0f)
            throw std::runtime_error("TerrainEnvironment requires nonnegative finite meteorological vapor pressure");
        for (float height : {minHeight, maxHeight}) {
            const float t = temperatureC(temperature, height) + 273.15f;
            if (!std::isfinite(t) || t < 150.0f || t > 400.0f)
                throw std::runtime_error("TerrainEnvironment: background air temperature outside 150..400 K; check height datum and lapse rate");
        }
    }
};
