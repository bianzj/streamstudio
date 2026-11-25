// ==========================================================================
// 文件：terrain_environment.h
// 功能：TerrainEnvironment——地形起伏下背景大气与土壤的“预设经验场”参数及换算函数（仅 VoxelEB）。
// 主要模块：
//   1. 参数结构：空气/土壤开关、参考高度、气温直减率、水汽/土壤/CO2 标高等
//   2. validate / validateAirState：参数与气象输入的合法性检查（失败即抛异常）
//   3. 空气场：temperatureC（直减率）、vaporHpa（指数衰减 + 饱和限幅）、saturationHpa
//   4. 土壤与 CO2：soilMoisture（随高于排水基准的高度指数变干）、co2ExcessPpm
// 数据流：fileio/fileio_json 解析场景中的 TerrainEnvironment 属性 → structs.h 的场景结构保存
//         → voxeleb/buffer.cpp 按体元高度调用本文件函数，生成初始气温、水汽、土壤含水量、CO2。
// 约定：高度为相对场景基准面（DEM 最小值）的米，不是海拔；温度 °C（validateAirState 内换 K）；
//       水汽压 hPa；土壤含水量 m3/m3；CO2 超额 ppm。
// 注意：这些是预设经验场，不是土壤水量平衡或碳输运模型；仅 eVoxelEB 支持（见 engine.cpp）。
// ==========================================================================

#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

// 结构：TerrainEnvironment。默认关闭（airEnabled/soilEnabled 为 false）；
// 参数的物理含义见各字段行注释与下方各函数。
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

    // 作用：检查全部参数为有限值，标高 > 0，CO2 超额 >= 0，且 0 <= soilDry <= soilValley <= 1。
    // 失败时抛 std::runtime_error。
    void validate() const {
        const float values[] = {referenceHeight, temperatureLapseRate, vaporScaleHeight,
            drainageHeight, soilDry, soilValley, soilScaleHeight, co2ValleyExcess, co2ScaleHeight};
        for (float v : values)
            if (!std::isfinite(v)) throw std::runtime_error("TerrainEnvironment requires finite parameters");
        if (vaporScaleHeight <= 0 || soilScaleHeight <= 0 || co2ScaleHeight <= 0 || co2ValleyExcess < 0 ||
            soilDry < 0 || soilValley < soilDry || soilValley > 1)
            throw std::runtime_error("TerrainEnvironment: positive scale heights and 0 <= soilDry <= soilValley <= 1 required");
    }

    // 作用：由参考高度处气温（°C）按直减率外推到 height：T = Tref - Γ·(height - referenceHeight)。
    // 未启用空气场时直接返回参考气温；Γ 为负时表示逆温。
    float temperatureC(float referenceC, float height) const {
        return airEnabled ? referenceC - temperatureLapseRate * (height - referenceHeight) : referenceC;
    }
    // 作用：谷底 CO2 超额随高度指数衰减，超额 = co2ValleyExcess·exp(-max(0,h)/co2ScaleHeight)，
    // 单位 ppm；负高度按 0 处理。
    float co2ExcessPpm(float height) const {
        return co2ValleyExcess * std::exp(-std::max(0.0f, height) / co2ScaleHeight);
    }
    // 作用：饱和水汽压（hPa），6.107·10^(7.5t/(237.3+t))（Magnus/Tetens 型公式，t 为 °C）。
    // 温度先限幅到 [-80, 80] °C，避免分母趋近 0 或指数溢出。
    static float saturationHpa(float temperatureC) {
        const float t = std::clamp(temperatureC, -80.0f, 80.0f);
        return 6.107f * std::pow(10.0f, 7.5f * t / (237.3f + t));
    }
    // 作用：由参考高度水汽压按标高 vaporScaleHeight 指数外推到 height，并限幅到 [0, 饱和水汽压]。
    // 指数参数先限幅到 ±80 以防溢出；未启用空气场时原样返回参考值。
    float vaporHpa(float referenceHpa, float height, float temperatureC) const {
        if (!airEnabled) return referenceHpa;
        return std::clamp(referenceHpa * std::exp(std::clamp(
            -(height - referenceHeight) / vaporScaleHeight, -80.0f, 80.0f)),
            0.0f, saturationHpa(temperatureC));
    }
    // 作用：土壤含水量（m3/m3）= soilDry + (soilValley - soilDry)·exp(-max(0, h - drainageHeight)/标高)，
    // 再限幅到不超过该土壤的饱和含水量 saturation（须在 0..1 内，否则抛异常）。
    // 即越靠近排水基准越湿，越高越趋于 soilDry。
    float soilMoisture(float height, float saturation) const {
        if (!std::isfinite(saturation) || saturation < 0.0f || saturation > 1.0f)
            throw std::runtime_error("TerrainEnvironment requires soil Satwater in m3/m3 within 0..1");
        return std::clamp(soilDry + (soilValley - soilDry) *
            std::exp(-std::max(0.0f, height - drainageHeight) / soilScaleHeight),
            0.0f, std::clamp(saturation, 0.0f, 1.0f));
    }

    // 作用：检查气象输入的水汽压非负有限，并检查场景最低/最高高度处外推气温（K）落在 150..400 K，
    // 用以尽早发现高度基准面或直减率设置错误。未启用空气场时直接返回。
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
