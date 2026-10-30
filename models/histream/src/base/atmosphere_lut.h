#ifndef FIELD_ATMOSPHERE_LUT_H
#define FIELD_ATMOSPHERE_LUT_H

#include "structs.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

struct AtmosphereCorrection
{
    float transmittance{1.0f};
    float pathRadiance{0.0f};
};

namespace atmosphere_lut_detail {

struct Spectrum
{
    std::vector<float> wavelength;
    std::vector<float> transmittance;
    std::vector<float> pathRadiance;
};

struct Table
{
    bool valid{false};
    std::vector<float> waterVapor;
    std::vector<float> visibility;
    std::vector<float> altitude;
    std::vector<float> zenith;
    std::map<std::tuple<std::string, std::string, int, int, int, int>, Spectrum> spectra;
};

inline void uniqueSort(std::vector<float>& values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

// Load 结束后按波长把同一个 Spectrum 的三个数组一起排序，保证 spectralSample 的
// upper_bound 前提成立（波长升序）。
inline void sortSpectrum(Spectrum& spectrum)
{
    const std::size_t count = std::min(spectrum.wavelength.size(),
        std::min(spectrum.transmittance.size(), spectrum.pathRadiance.size()));
    if (count < 2U) return;
    std::vector<std::size_t> order(count);
    for (std::size_t index = 0; index < count; ++index) order[index] = index;
    std::stable_sort(order.begin(), order.end(), [&spectrum](std::size_t lhs, std::size_t rhs) {
        return spectrum.wavelength[lhs] < spectrum.wavelength[rhs];
    });
    std::vector<float> wavelength(count), transmittance(count), pathRadiance(count);
    for (std::size_t index = 0; index < count; ++index) {
        wavelength[index] = spectrum.wavelength[order[index]];
        transmittance[index] = spectrum.transmittance[order[index]];
        pathRadiance[index] = spectrum.pathRadiance[order[index]];
    }
    spectrum.wavelength.swap(wavelength);
    spectrum.transmittance.swap(transmittance);
    spectrum.pathRadiance.swap(pathRadiance);
}

inline Table load(const std::string& path)
{
    Table table;
    // FIXED[中] 安全: 窄字符 std::ifstream(path) 按 ANSI 代码页解析，中文/UTF-8 路径的 LUT 打开失败后只返回无效表，sampleAtmosphereLut 静默返回“无校正”；已修复：保留原回退行为，但打开失败时输出含完整路径的明确告警并说明大气校正被跳过（u8path 全链路属超范围，未做）。
    std::ifstream file(path);
    if (!file) {
        std::cerr << "WARNING: atmosphere LUT could not be opened: \"" << path
                  << "\"; atmosphere correction will be skipped and radiance left uncorrected" << std::endl;
        return table;
    }
    std::string line;
    bool version2 = false;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("atmosphere_model", 0) == 0) {
            version2 = true;
            continue;
        }
        if (line.rfind("visibility_km", 0) == 0) continue;
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream row(line);
        std::string model{"midlatitude-summer"}, aerosol{"rural"};
        float waterVapor = 2.0f, visibility = 0.0f, altitude = 0.0f, zenith = 0.0f;
        float wavelength = 0.0f, transmittance = 1.0f, pathRadiance = 0.0f;
        if (version2) {
            if (!(row >> model >> aerosol >> waterVapor >> visibility >> altitude >> zenith
                >> wavelength >> transmittance >> pathRadiance)) continue;
        } else if (!(row >> visibility >> altitude >> zenith >> wavelength >> transmittance >> pathRadiance)) continue;
        table.waterVapor.push_back(waterVapor);
        // DEFERRED[低] 数值: 各轴取值集合是跨所有 model/aerosol 合并的；若不同模型的网格不同，bracket 选出的格点在当前 model/aerosol 下可能不存在，被跳过后权重重新归一化，插值退化为单侧外推且无提示；请求的 model/aerosol 完全不存在时则静默返回无校正；理由：需按 (model, aerosol) 分别建立轴并改 Table 结构/查找逻辑，属结构性改动，不属本轮允许项。
        table.visibility.push_back(visibility);
        table.altitude.push_back(altitude);
        table.zenith.push_back(zenith);
        auto& spectrum = table.spectra[{
            model,
            aerosol,
            static_cast<int>(std::lround(waterVapor * 1000.0f)),
            static_cast<int>(std::lround(visibility * 1000.0f)),
            static_cast<int>(std::lround(altitude * 1000.0f)),
            static_cast<int>(std::lround(zenith * 1000.0f))}];
        spectrum.wavelength.push_back(wavelength);
        // FIXED[中] Bug: 波长按文件行序追加，未排序也未去重；spectralSample 依赖 wavelength 升序做 upper_bound，LUT 行顺序被打乱时插值结果错误；已修复：load 结束后对每个 Spectrum 调用 sortSpectrum，按波长把 wavelength/transmittance/pathRadiance 三个数组一起稳定排序。
        spectrum.transmittance.push_back(std::clamp(transmittance, 0.0f, 1.0f));
        spectrum.pathRadiance.push_back(std::max(0.0f, pathRadiance));
    }
    for (auto& entry : table.spectra)
        sortSpectrum(entry.second);
    uniqueSort(table.waterVapor);
    uniqueSort(table.visibility);
    uniqueSort(table.altitude);
    uniqueSort(table.zenith);
    table.valid = !table.spectra.empty() && !table.waterVapor.empty() && !table.visibility.empty()
        && !table.altitude.empty() && !table.zenith.empty();
    return table;
}

inline const Table& cached(const std::string& path)
{
    // FIXED[中] 并发: 函数内 static std::map 的查找与插入无锁，并行首次加载会数据竞争；已修复：为静态缓存加 std::mutex（返回的 Table 引用在 map 生命周期内稳定，查找/插入语义不变）。
    static std::mutex cacheMutex;
    static std::map<std::string, Table> cache;
    const std::lock_guard<std::mutex> lock(cacheMutex);
    auto found = cache.find(path);
    if (found == cache.end()) found = cache.emplace(path, load(path)).first;
    return found->second;
}

inline std::pair<float, float> bracket(const std::vector<float>& axis, float value)
{
    if (value <= axis.front()) return {axis.front(), axis.front()};
    if (value >= axis.back()) return {axis.back(), axis.back()};
    const auto upper = std::upper_bound(axis.begin(), axis.end(), value);
    return {*(upper - 1), *upper};
}

inline AtmosphereCorrection spectralSample(const Spectrum& spectrum, float wavelength)
{
    if (spectrum.wavelength.empty()) return {};
    if (wavelength <= spectrum.wavelength.front())
        return {spectrum.transmittance.front(), spectrum.pathRadiance.front()};
    if (wavelength >= spectrum.wavelength.back())
        return {spectrum.transmittance.back(), spectrum.pathRadiance.back()};
    const auto upper = std::upper_bound(spectrum.wavelength.begin(), spectrum.wavelength.end(), wavelength);
    const size_t right = static_cast<size_t>(upper - spectrum.wavelength.begin());
    const size_t left = right - 1;
    const float span = spectrum.wavelength[right] - spectrum.wavelength[left];
    const float weight = span > 0.0f ? (wavelength - spectrum.wavelength[left]) / span : 0.0f;
    return {
        spectrum.transmittance[left] + (spectrum.transmittance[right] - spectrum.transmittance[left]) * weight,
        spectrum.pathRadiance[left] + (spectrum.pathRadiance[right] - spectrum.pathRadiance[left]) * weight
    };
}

inline float axisWeight(float value, float low, float high, bool upper)
{
    if (high <= low) return upper ? 0.0f : 1.0f;
    const float fraction = std::clamp((value - low) / (high - low), 0.0f, 1.0f);
    return upper ? fraction : 1.0f - fraction;
}

} // namespace atmosphere_lut_detail

inline AtmosphereCorrection sampleAtmosphereLut(const AtmosphereXml& settings,
                                                float wavelengthNanometers,
                                                float sensorAltitudeKilometers,
                                                float viewZenithDegrees)
{
    if (!settings.enabled || settings.lutFile.empty()
        || wavelengthNanometers < 350.0f || wavelengthNanometers > 14000.0f) return {};
    const auto& table = atmosphere_lut_detail::cached(settings.lutFile);
    if (!table.valid) return {};
    const auto waterVapor = atmosphere_lut_detail::bracket(table.waterVapor, settings.waterVapor);
    const auto visibility = atmosphere_lut_detail::bracket(table.visibility, settings.visibility);
    const auto altitude = atmosphere_lut_detail::bracket(table.altitude, sensorAltitudeKilometers);
    const auto zenith = atmosphere_lut_detail::bracket(table.zenith, std::abs(viewZenithDegrees));
    AtmosphereCorrection result{0.0f, 0.0f};
    float accumulatedWeight = 0.0f;
    for (int iw = 0; iw < 2; ++iw) for (int iv = 0; iv < 2; ++iv)
        for (int ia = 0; ia < 2; ++ia) for (int iz = 0; iz < 2; ++iz) {
        const float w = iw ? waterVapor.second : waterVapor.first;
        const float v = iv ? visibility.second : visibility.first;
        const float a = ia ? altitude.second : altitude.first;
        const float z = iz ? zenith.second : zenith.first;
        const float weight = atmosphere_lut_detail::axisWeight(settings.waterVapor, waterVapor.first, waterVapor.second, iw != 0)
            * atmosphere_lut_detail::axisWeight(settings.visibility, visibility.first, visibility.second, iv != 0)
            * atmosphere_lut_detail::axisWeight(sensorAltitudeKilometers, altitude.first, altitude.second, ia != 0)
            * atmosphere_lut_detail::axisWeight(std::abs(viewZenithDegrees), zenith.first, zenith.second, iz != 0);
        if (weight <= 0.0f) continue;
        const auto found = table.spectra.find({
            settings.model,
            settings.aerosol,
            static_cast<int>(std::lround(w * 1000.0f)),
            static_cast<int>(std::lround(v * 1000.0f)),
            static_cast<int>(std::lround(a * 1000.0f)),
            static_cast<int>(std::lround(z * 1000.0f))});
        if (found == table.spectra.end()) continue;
        const auto sample = atmosphere_lut_detail::spectralSample(found->second, wavelengthNanometers);
        result.transmittance += weight * sample.transmittance;
        result.pathRadiance += weight * sample.pathRadiance;
        accumulatedWeight += weight;
    }
    if (accumulatedWeight <= 0.0f) return {};
    result.transmittance /= accumulatedWeight;
    result.pathRadiance /= accumulatedWeight;
    result.transmittance = std::clamp(result.transmittance, 0.0f, 1.0f);
    result.pathRadiance = std::max(0.0f, result.pathRadiance);
    return result;
}

inline float atmosphereCorrectRadiance(float value, const AtmosphereXml& settings,
                                       float wavelengthNanometers,
                                       float sensorAltitudeMeters,
                                       float viewZenithDegrees)
{
    // DEFERRED[低] 物理: value==0 时直接返回 0，未加程辐射；真实的零地表辐亮度在传感器处仍应有路径辐射，导致暗像元与相邻像元不连续；理由：让 0 值也做 value*T+L_path 属物理语义变更，明确禁止本轮修改。
    if (!settings.enabled || value == 0.0f || !std::isfinite(value)) return value;
    const float normalizedWavelength = wavelengthNanometers > 0.0f && wavelengthNanometers <= 50.0f
        ? wavelengthNanometers * 1000.0f : wavelengthNanometers;
    const auto correction = sampleAtmosphereLut(settings, normalizedWavelength,
        std::max(0.0f, sensorAltitudeMeters) / 1000.0f, viewZenithDegrees);
    return value * correction.transmittance + correction.pathRadiance;
}

inline float atmospherePlanckRadiance(float wavelengthNanometers, float temperatureKelvin)
{
    constexpr double c1 = 1.1910439340652e8;
    constexpr double c2 = 14388.291040407;
    const double wavelengthMicrometers = wavelengthNanometers > 50.0f
        ? static_cast<double>(wavelengthNanometers) / 1000.0
        : static_cast<double>(wavelengthNanometers);
    const double wavelength = std::max(1.0e-6, wavelengthMicrometers);
    const double temperature = std::max(1.0, static_cast<double>(temperatureKelvin));
    return static_cast<float>(c1 /
        (std::pow(wavelength, 5.0) * std::expm1(c2 / (temperature * wavelength))));
}

inline float atmosphereInversePlanckTemperature(float wavelengthNanometers, float radiance)
{
    if (!std::isfinite(radiance) || radiance <= 0.0f) return 0.0f;
    constexpr double c1 = 1.1910439340652e8;
    constexpr double c2 = 14388.291040407;
    const double wavelengthMicrometers = wavelengthNanometers > 50.0f
        ? static_cast<double>(wavelengthNanometers) / 1000.0
        : static_cast<double>(wavelengthNanometers);
    const double wavelength = std::max(1.0e-6, wavelengthMicrometers);
    const double denominator = std::log1p(c1 /
        (static_cast<double>(radiance) * std::pow(wavelength, 5.0)));
    return denominator > 0.0
        ? static_cast<float>(c2 / (wavelength * denominator)) : 0.0f;
}

inline bool atmosphereSkyPixelZenith(int pixelX, int pixelY,
                                     int width, int height,
                                     float verticalFovDegrees,
                                     float viewZenithDegrees,
                                     float viewAzimuthDegrees,
                                     float& skyZenithDegrees)
{
    if (width <= 0 || height <= 0) return false;
    constexpr float pi = 3.14159265358979323846f;
    const float zenith = viewZenithDegrees * pi / 180.0f;
    const float azimuth = viewAzimuthDegrees * pi / 180.0f;
    glm::vec3 cameraOut{
        std::sin(zenith) * std::cos(azimuth),
        std::cos(zenith),
        std::sin(zenith) * std::sin(azimuth)};
    const glm::vec3 forward = -glm::normalize(cameraOut);
    const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
    glm::vec3 up = worldUp - glm::dot(worldUp, forward) * forward;
    if (glm::dot(up, up) < 1.0e-12f) {
        const glm::vec3 north{1.0f, 0.0f, 0.0f};
        up = north - glm::dot(north, forward) * forward;
    }
    up = glm::normalize(up);
    glm::vec3 right = glm::cross(forward, up);
    right = glm::dot(right, right) < 1.0e-12f
        ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::normalize(right);
    up = glm::normalize(glm::cross(right, forward));

    const float tanHalfVertical = std::tan(std::clamp(
        verticalFovDegrees, 0.1f, 120.0f) * 0.5f * pi / 180.0f);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float screenX = (2.0f * (static_cast<float>(pixelX) + 0.5f) /
                           static_cast<float>(width) - 1.0f) *
                          tanHalfVertical * aspect;
    const float screenY = (1.0f - 2.0f * (static_cast<float>(pixelY) + 0.5f) /
                           static_cast<float>(height)) * tanHalfVertical;
    const glm::vec3 ray = glm::normalize(forward + right * screenX + up * screenY);
    if (!(ray.y > 0.0f)) return false;
    skyZenithDegrees = std::acos(std::clamp(ray.y, 0.0f, 1.0f)) * 180.0f / pi;
    return true;
}

inline float atmosphereSkyOutputValue(const AtmosphereXml& settings,
                                      float wavelengthNanometers,
                                      float sensorAltitudeMeters,
                                      float skyZenithDegrees,
                                      bool temperatureOutput,
                                      float skyTemperatureKelvin = 250.0f)
{
    const float wavelength = wavelengthNanometers > 0.0f && wavelengthNanometers <= 50.0f
        ? wavelengthNanometers * 1000.0f : wavelengthNanometers;
    const auto correction = sampleAtmosphereLut(
        settings, wavelength, std::max(0.0f, sensorAltitudeMeters) / 1000.0f,
        skyZenithDegrees);
    if (wavelength <= 2500.0f) {
        const float clampedWavelength = std::clamp(wavelength, 350.0f, 2500.0f);
        const float zenith = std::clamp(skyZenithDegrees, 0.0f, 89.5f) *
                              3.14159265358979323846f / 180.0f;
        const float cosineZenith = std::max(0.12f, std::cos(zenith));
        const float airMass = 1.0f / cosineZenith;
        const float aerosol = std::clamp(23.0f / std::max(1.0f, settings.visibility),
                                         0.4f, 2.3f);
        const float spectralWeight = std::clamp(
            std::pow(550.0f / clampedWavelength, 1.8f), 0.06f, 2.0f);
        const float empiricalTransmittance = std::exp(
            -(0.10f * spectralWeight + 0.025f * aerosol) * airMass);
        const float transmittance = settings.enabled
            ? correction.transmittance : empiricalTransmittance;
        const float horizonBoost = 1.0f + 0.55f * (1.0f - cosineZenith);
        const float apparentReflectance =
            (1.0f - transmittance) * 0.55f * spectralWeight * horizonBoost +
            0.008f * aerosol * horizonBoost;
        return std::clamp(apparentReflectance, 0.002f, 0.85f);
    }

    const float zenith = std::clamp(skyZenithDegrees, 0.0f, 89.5f) *
                          3.14159265358979323846f / 180.0f;
    const float cosineZenith = std::max(0.05f, std::cos(zenith));
    float radiance = settings.enabled ? correction.pathRadiance : 0.0f;
    if (!(radiance > 0.0f)) {
        // Empirical directional sky: interpolate radiance by cos(theta).
        // Zenith uses the configured zenith emissivity; the longer horizon
        // path approaches black-body radiance at the configured sky temperature.
        // This retains a visible, continuous angular gradient even for the
        // near-horizontal views used by perspective sensors.
        const float zenithEmissivity = settings.enabled &&
                                       correction.transmittance < 0.999f
            ? std::clamp(1.0f - correction.transmittance, 0.08f, 0.995f)
            : 0.72f;
        const float atmosphericFraction = std::clamp(
            1.0f - (1.0f - zenithEmissivity) * cosineZenith,
            zenithEmissivity, 1.0f);
        radiance = atmospherePlanckRadiance(
            wavelength, std::max(120.0f, skyTemperatureKelvin)) * atmosphericFraction;
    }
    return temperatureOutput
        ? atmosphereInversePlanckTemperature(wavelength, radiance) : radiance;
}

inline float atmosphereCorrectOutputValue(float value, const AtmosphereXml& settings,
                                          float wavelengthNanometers,
                                          float sensorAltitudeMeters,
                                          float viewZenithDegrees,
                                          bool temperatureOutput)
{
    if (!settings.enabled || value == 0.0f || !std::isfinite(value)) return value;
    const float normalizedWavelength = wavelengthNanometers > 0.0f && wavelengthNanometers <= 50.0f
        ? wavelengthNanometers * 1000.0f : wavelengthNanometers;
    if (normalizedWavelength <= 2500.0f) {
        // Shortwave outputs are dimensionless reflection factors. The current
        // relative illumination model provides no absolute spectral irradiance
        // with which to normalize LUT path radiance (W m^-2 sr^-1 um^-1).
        // Apply path attenuation only; never add radiance to a reflection factor.
        const auto correction = sampleAtmosphereLut(settings, normalizedWavelength,
            std::max(0.0f, sensorAltitudeMeters) / 1000.0f, viewZenithDegrees);
        return value * correction.transmittance;
    }
    if (!temperatureOutput) {
        return atmosphereCorrectRadiance(value, settings, normalizedWavelength,
            sensorAltitudeMeters, viewZenithDegrees);
    }
    const float radiance = atmospherePlanckRadiance(normalizedWavelength, value);
    const float corrected = atmosphereCorrectRadiance(radiance, settings, normalizedWavelength,
        sensorAltitudeMeters, viewZenithDegrees);
    return atmosphereInversePlanckTemperature(normalizedWavelength, corrected);
}

#endif // FIELD_ATMOSPHERE_LUT_H
