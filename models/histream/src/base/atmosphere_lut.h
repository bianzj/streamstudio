// ==========================================================================
// 文件：atmosphere_lut.h
// 功能：大气查找表（LUT）读取、插值与传感器处大气校正（透过率 + 程辐射），以及天空像元的
//       经验/LUT 输出值；仅头文件，全部 inline。
// 主要模块：
//   1. AtmosphereCorrection：单个波长上的校正结果（透过率 T、程辐射 L_path）
//   2. atmosphere_lut_detail：LUT 文本解析（load）、按路径缓存（cached）、轴括号查找（bracket）、
//      光谱线性插值（spectralSample）、轴权重（axisWeight）
//   3. sampleAtmosphereLut：对 水汽/能见度/传感器高度/视天顶角 四轴做 16 角点多线性插值
//   4. atmosphereCorrectRadiance / atmosphereCorrectOutputValue：value*T + L_path 校正
//   5. atmospherePlanckRadiance / atmosphereInversePlanckTemperature：普朗克函数及其反函数
//   6. atmosphereSkyPixelZenith / atmosphereSkyOutputValue：传感器视场内天空像元的天顶角与输出值
// 数据流：AtmosphereXml（structs.h，来自工程 XML 的 atmosphere 节点）+ 波长/传感器高度/视天顶角
//       → sampleAtmosphereLut → 乘以 T、加 L_path → 写回输出图像值。
// 约定：波长对外按 nm；传入值 <=50 视为 μm 并乘 1000 转 nm（兼容旧调用）；LUT 有效范围 350–14000 nm。
//       传感器高度 LUT 内为 km（接口 atmosphereCorrectRadiance 等传入 m，内部 /1000）；
//       视天顶角单位度，取绝对值；能见度 km；程辐射单位 W m^-2 sr^-1 um^-1；
//       Planck 辐亮度 c1/c2 常量对应波长 μm，温度 K。
// LUT 文件格式：逗号/空白分隔的文本行；v2 以 atmosphere_model 开头行标识，列为
//       model aerosol waterVapor visibility altitude zenith wavelength transmittance pathRadiance；
//       v1（无该表头）列为 visibility altitude zenith wavelength transmittance pathRadiance，
//       model/aerosol/waterVapor 取缺省 midlatitude-summer / rural / 2.0。
// 注意：LUT 无法打开或为空时一律返回“无校正”（T=1、L_path=0）并保持输出不变。
// ==========================================================================

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

// 模块：单个波长/几何下的大气校正结果。
// transmittance：路径透过率（无量纲，缺省 1 = 不衰减）；pathRadiance：程辐射（缺省 0）。
struct AtmosphereCorrection
{
    float transmittance{1.0f};
    float pathRadiance{0.0f};
};

// 模块：LUT 内部实现细节（解析、缓存、插值辅助），不应被外部直接调用。
namespace atmosphere_lut_detail {

// 一条光谱：同一 (model, aerosol, 水汽, 能见度, 高度, 天顶角) 组合下，
// 按波长（nm）排列的透过率与程辐射三个等长数组。
struct Spectrum
{
    std::vector<float> wavelength;
    std::vector<float> transmittance;
    std::vector<float> pathRadiance;
};

// 整张 LUT：四个插值轴的取值集合（升序去重）+ 以整数键（物理量×1000 取整）索引的光谱表。
// valid=false 表示文件缺失或内容不完整，调用方应跳过校正。
struct Table
{
    bool valid{false};
    std::vector<float> waterVapor;
    std::vector<float> visibility;
    std::vector<float> altitude;
    std::vector<float> zenith;
    std::map<std::tuple<std::string, std::string, int, int, int, int>, Spectrum> spectra;
};

// 作用：把轴取值排序并去重，供 bracket 的 upper_bound 使用。
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

// 作用：读取并解析 LUT 文本文件，返回 Table。
// 输入：LUT 文件路径；输出：Table（打不开时 valid=false 并向 stderr 报警告）。
// 说明：遇到以 atmosphere_model 开头的表头行后按 v2 格式解析，否则按 v1；解析失败的行直接跳过。
//       浮点参数以 ×1000 取整作为 map 键，避免浮点相等比较。
inline Table load(const std::string& path)
{
    Table table;
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
        // v1 格式的列名行（以 visibility_km 开头）直接跳过。
        if (line.rfind("visibility_km", 0) == 0) continue;
        // 统一把逗号分隔视作空白分隔，之后用 istringstream 按列读取。
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
        // 数据清洗：透过率限幅到 [0,1]，程辐射不允许为负。
        spectrum.transmittance.push_back(std::clamp(transmittance, 0.0f, 1.0f));
        spectrum.pathRadiance.push_back(std::max(0.0f, pathRadiance));
    }
    // 读完后：每条光谱按波长排序，各轴取值排序去重，并判定整张表是否可用。
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

// 作用：按文件路径缓存已解析的 LUT（进程内单例，互斥锁保护，线程安全）。
// 说明：缓存永不失效；同一路径文件在运行期被改写不会重新读取。
inline const Table& cached(const std::string& path)
{
    static std::mutex cacheMutex;
    static std::map<std::string, Table> cache;
    const std::lock_guard<std::mutex> lock(cacheMutex);
    auto found = cache.find(path);
    if (found == cache.end()) found = cache.emplace(path, load(path)).first;
    return found->second;
}

// 作用：在升序轴上找到包住 value 的相邻两个格点 {low, high}。
// 边界：value 不超出轴范围内返回相邻格点；越界（<=首/>=尾）时两端取同一格点（等价于最近邻外推）。
// 前提：axis 非空且升序（由 uniqueSort 保证；调用前需 table.valid）。
inline std::pair<float, float> bracket(const std::vector<float>& axis, float value)
{
    if (value <= axis.front()) return {axis.front(), axis.front()};
    if (value >= axis.back()) return {axis.back(), axis.back()};
    const auto upper = std::upper_bound(axis.begin(), axis.end(), value);
    return {*(upper - 1), *upper};
}

// 作用：在一条光谱上按波长（nm）线性插值透过率与程辐射。
// 边界：波长超出光谱范围时取端点值（不外推）；光谱为空返回缺省（T=1, L_path=0）。
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

// 作用：线性插值的单轴权重；upper=true 取上端格点权重 = 位置分数，否则为 1-分数。
// 说明：high<=low（轴越界或只有一个格点）时全部权重给下端格点。
inline float axisWeight(float value, float low, float high, bool upper)
{
    if (high <= low) return upper ? 0.0f : 1.0f;
    const float fraction = std::clamp((value - low) / (high - low), 0.0f, 1.0f);
    return upper ? fraction : 1.0f - fraction;
}

} // namespace atmosphere_lut_detail

// 作用：从 LUT 取指定波长/传感器高度/视天顶角下的大气透过率与程辐射。
// 输入：settings（启用开关、model、aerosol、水汽、能见度、LUT 路径）；波长 nm；
//       传感器高度 km；视天顶角度（取绝对值）。
// 输出：T 限幅到 [0,1]、L_path>=0；未启用/波长不在 350–14000 nm/LUT 不可用/无有效权重时
//       返回缺省（T=1, L_path=0）。
// 方法：水汽、能见度、高度、天顶角四轴各取两个包围格点，共 16 个角点做多线性插值；
//       角点在当前 model/aerosol 下查不到光谱则跳过，其余权重按累计权重重新归一化。
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
    // 步骤 1：对 16 个角点累加加权光谱；累加初值为 0（不是缺省 T=1），最后需除以累计权重。
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
    // 步骤 2：归一化并限幅；一个角点都没命中则回退为“无校正”。
    if (accumulatedWeight <= 0.0f) return {};
    result.transmittance /= accumulatedWeight;
    result.pathRadiance /= accumulatedWeight;
    result.transmittance = std::clamp(result.transmittance, 0.0f, 1.0f);
    result.pathRadiance = std::max(0.0f, result.pathRadiance);
    return result;
}

// 作用：对一个辐亮度值做大气校正：value * T + L_path。
// 输入：value（与 LUT 程辐射同单位的辐亮度）、波长（nm；<=50 视为 μm）、传感器高度（m）、
//       视天顶角（度）。value 为 0 或非有限值时原样返回。
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

// 作用：普朗克黑体光谱辐亮度，单位 W m^-2 sr^-1 um^-1。
// 说明：波长 >50 视为 nm 转 μm，<=50 视为已是 μm；c1=2hc^2 (W μm^4 m^-2 sr^-1)，
//       c2=hc/k (μm K)；波长下限 1e-6 μm，温度下限 1 K 防止除零；用 expm1 提高小指数时精度。
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

// 作用：Planck 反函数，由光谱辐亮度求亮温（K）。
// 说明：radiance 非正或非有限时返回 0；波长单位约定同 atmospherePlanckRadiance。
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

// 作用：计算透视相机像素对应射线的天顶角，判断该像素是否看到天空。
// 输入：像素坐标（pixelY 向下）、图像宽高、垂直视场角（度，限幅到 [0.1,120]）、
//       相机视天顶角/方位角（度）。
// 输出：返回 true 且写 skyZenithDegrees（射线与 +Y 的夹角，度）；射线不朝上（ray.y<=0）时返回 false。
// 坐标：世界 +Y 向上；cameraOut 为从目标指向相机的单位向量，相机朝向 forward = -cameraOut。
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
    // 步骤 1：构造相机基（forward/right/up）；up 取世界 +Y 在垂直于 forward 平面上的投影，
    //         相机正对天顶/天底（投影退化）时改用 +X 方向代替。
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

    // 步骤 2：像素中心 → 归一化屏幕坐标（NDC×tan(fov/2)，水平方向乘宽高比），
    //         合成世界空间射线并取其 y 分量得到天顶角。
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

// 作用：天空像元（射线不与地物相交）的输出值。
// 输入：settings、波长（nm）、传感器高度（m）、天空天顶角（度）、是否输出亮温、天空温度（K，缺省 250）。
// 输出：短波（<=2500 nm）返回表观反射率（限幅 [0.002, 0.85]）；
//       热红外返回天空辐亮度或亮温。
// 说明：短波/热红外两段均为经验形式（待核对物理依据），仅用于使天空像元有连续的角度渐变。
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
        // 未启用 LUT 时用经验透过率：波长项 (550/λ)^1.8 与气溶胶项（随能见度），乘大气质量（1/cosθ，cosθ 下限 0.12）。
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
    // 热红外：优先使用 LUT 程辐射；LUT 不可用或为 0 时走下面的经验方向天空模型。
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

// 作用：对输出图像值做大气校正（按输出类型分三种情形）。
//   - 短波（<=2500 nm）：反射因子只乘 T，不加程辐射（见下方说明）；
//   - 热红外辐亮度：value*T + L_path；
//   - 热红外亮温：先 Planck 转辐亮度，校正后再反演回亮温。
// 未启用或 value 为 0/非有限值时原样返回。
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
