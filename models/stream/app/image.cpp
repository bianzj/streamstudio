#include "image.h"
#include "config.h"
#include "inversion.h"
#include "thermal_view.h"
#include "../forcing/era5.h"
#include "../forcing/pixel_forcing.h"
#include "../forcing/utc.h"
#include "../physics/brightness.h"
#include "../physics/canopy.h"
#include "../physics/constants.h"
#include "../physics/energy.h"
#include "../physics/hapke.h"
#include "../physics/optics.h"
#include "../physics/radiosity.h"
#include "../physics/shortwave.h"
#include "../physics/soil_emissivity.h"
#include "../physics/spa.h"
#include "../scene/classes.h"
#include "../scene/raster.h"

#include <cpl_json.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace stream {
namespace {

namespace fs = std::filesystem;
constexpr int kMoistureSteps = 1000;

void progress(double percent, const std::string& stage)
{
    std::cout << "STREAM\tprogress\t" << std::fixed << std::setprecision(1) << percent << '\t' << stage << '\n';
}

void log(const std::string& text) { std::cout << "[STREAM] " << text << '\n'; }

std::string format(double value, int digits = 2)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(digits) << value;
    return stream.str();
}

bool isMainThread()
{
#ifdef _OPENMP
    return omp_get_thread_num() == 0;
#else
    return true;
#endif
}

std::vector<float> readColumn(const fs::path& file, size_t minimumCount)
{
    std::ifstream input(file);
    if (!input) throw std::runtime_error("找不到太阳光谱：" + utf8FromPath(file));
    std::vector<float> values;
    double value;
    while (input >> value) values.push_back(static_cast<float>(value));
    if (values.size() < minimumCount) throw std::runtime_error("太阳光谱行数不足：" + utf8FromPath(file));
    values.resize(minimumCount);
    return values;
}

// Gaussian spectral response weighted by the reference solar irradiance (direct + diffuse).
std::vector<std::vector<std::pair<int, double>>> opticalWeights(const std::vector<SpectralBand>& bands, const std::vector<float>& irradiance)
{
    std::vector<std::vector<std::pair<int, double>>> result;
    for (const auto& band : bands) {
        std::vector<std::pair<int, double>> weights;
        if (band.fwhm < 1) {
            const int index = std::clamp(static_cast<int>(std::lround(band.center)) - kFirstWavelength, 0, kSpectralCount - 1);
            weights.push_back({index, 1.0});
        } else {
            const double sigma = band.fwhm / (2 * std::sqrt(2 * std::log(2.0)));
            double sum = 0;
            for (int index = 0; index < kSpectralCount; ++index) {
                const double distance = (kFirstWavelength + index - band.center) / sigma;
                if (std::abs(distance) > 3.5) continue;
                const double weight = std::exp(-0.5 * distance * distance) * std::max(0.0f, irradiance[index]);
                if (weight > 0) { weights.push_back({index, weight}); sum += weight; }
            }
            if (!(sum > 0)) throw std::runtime_error("光学波段 " + band.name + " 落在参考太阳光谱为零的区间");
            for (auto& item : weights) item.second /= sum;
        }
        result.push_back(std::move(weights));
    }
    return result;
}

double bandAverage(const std::vector<float>& spectrum, const std::vector<std::pair<int, double>>& weights)
{
    double sum = 0;
    for (const auto& [index, weight] : weights) sum += weight * spectrum[index];
    return sum;
}

int moistureStep(float moisture) { return static_cast<int>(std::lround(std::clamp(moisture, 0.0f, 1.0f) * kMoistureSteps)); }

std::vector<float> componentRaster(const Field& field, const Grid& grid, const std::string& label, double overpass, int& automaticBand)
{
    if (field.fromFile()) {
        auto dataset = openRaster(field.file, label);
        int band = field.band;
        if (band <= 0) {
            if (dataset->GetRasterCount() == 1) band = 1;
            else if (dataset->GetRasterCount() == 24) {
                const UtcTime time = utcFromSeconds(overpass);
                const double hour = time.hour + time.minute / 60.0 + time.second / 3600.0;
                band = std::min(23, static_cast<int>(std::lround(hour))) + 1;
                automaticBand = band;
            } else throw std::runtime_error(label + "为多波段文件，请指定温度波段序号");
        }
        return readBand(*dataset, band, grid, label);
    }
    if (std::isfinite(field.value)) return std::vector<float>(grid.size(), static_cast<float>(field.value));
    throw std::runtime_error(std::string("缺少输入：") + label + "（K）");
}

}  // namespace

int runImage(const std::string& configPath)
{
    const auto started = std::chrono::steady_clock::now();
    const ImageConfig config = readImageConfig(configPath);
#ifdef _OPENMP
    if (config.threads > 0) omp_set_num_threads(config.threads);
#endif
    const bool eb = config.mode == TemperatureMode::Eb;
    ClassTable classes = loadClassTable(config.dataDir / "classes.json");
    if (config.soilBrightness > 0.0) {
        classes.soilOptics.brightness = static_cast<float>(config.soilBrightness);
        log("BSM 土壤亮度按本场景设置：" + format(config.soilBrightness, 3));
    }
    const double overpass = config.overpass;
    // DEFERRED[中] 物理: EB 起算点固定为过境当天 00 UTC，不考虑经度。对东亚/澳洲（UTC+8..+10）约 02:30 UTC 的过境，只积分当地上午 0.5–2.5 h，热历史默认 00 UTC 之前等温（无日变化记忆），G 与 Tss 偏差大；过境恰为 00:00 UTC 时只有 1 个时次，完全没有起转；建议：起转改为过境前固定时长（如 ≥24 h），或从按经度换算的当地午夜开始，并相应扩大 ERA5 读取时段
    const double dayStart = std::floor(overpass / 86400.0) * 86400.0;
    log("过境时刻 " + formatUtc(overpass) + "，温度来源：" + (eb ? "EB（ERA5 驱动能量平衡）" : "RT（给定组分温度）"));

    progress(1, "读取地表输入");
    Grid grid;
    auto typeDataset = openRaster(config.type.file, "地表类型");
    grid.width = typeDataset->GetRasterXSize();
    grid.height = typeDataset->GetRasterYSize();
    // DEFERRED[低] 内存: 上限 4e8 像元太宽：EB 模式会为整幅网格分配 7+18+4+光学/热红外/太阳几何等约 40 个 float 数组（4e8 像元时约 64 GB），即使 region 只是一小块也按全图分配；建议：按 region 尺寸分配输出缓冲（写出时再贴回全图），或者按可用内存估算后再给出明确报错
    if (grid.width < 1 || grid.height < 1 || grid.size() > 400000000ull) throw std::runtime_error("地表类型网格尺寸无效或过大");
    grid.hasTransform = typeDataset->GetGeoTransform(grid.transform) == CE_None;
    if (const char* wkt = typeDataset->GetProjectionRef()) grid.projection = wkt;
    const auto type = readBand(*typeDataset, config.type.band > 0 ? config.type.band : 1, grid, "地表类型");
    typeDataset.reset();
    log("网格 " + std::to_string(grid.width) + " × " + std::to_string(grid.height) + (grid.projection.empty() ? "，无坐标系" : "，已读取坐标系"));

    const auto lai = fieldRaster(config.lai, grid, "叶面积指数 LAI", 0, true);
    const auto vza = fieldRaster(config.vza, grid, "观测天顶角 VZA", 0, true);
    const auto vaa = fieldRaster(config.vaa, grid, "观测方位角 VAA", 0, true);
    const auto dem = fieldRaster(config.dem, grid, "高程 DEM", 0, false);
    std::vector<float> latitude, longitude;
    if (config.latitude.present() || config.longitude.present()) {
        latitude = fieldRaster(config.latitude, grid, "纬度", 0, true);
        longitude = fieldRaster(config.longitude, grid, "经度", 0, true);
        log("经纬度来自输入栅格或常数");
    } else {
        geographicFromGrid(grid, latitude, longitude);
        log("经纬度由地表类型文件的地理参考换算（WGS84）");
    }

    int startRow = 0, endRow = grid.height, startColumn = 0, endColumn = grid.width;
    if (config.region) {
        startRow = config.startRow; endRow = config.endRow; startColumn = config.startColumn; endColumn = config.endColumn;
        if (startRow < 0 || startColumn < 0 || endRow > grid.height || endColumn > grid.width || startRow >= endRow || startColumn >= endColumn)
            throw std::runtime_error("行列范围无效或超出网格（从 0 开始，终点不包含）");
    }
    const auto validLocation = [&](size_t pixel) {
        const float lat = latitude[pixel], lon = longitude[pixel];
        return std::isfinite(lat) && std::isfinite(lon) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
    };

    // ---------------- ERA5 pack (EB) ----------------
    era5::Pack pack;
    std::vector<double> nodes;
    bool elevationCorrection = false;
    if (eb) {
        progress(2, "读取 ERA5 数据包");
        pack.open(config.era5Pack);
        const UtcTime when = utcFromSeconds(overpass);
        double latMin = 90, latMax = -90, lonMin = 180, lonMax = -180;
        for (int row = startRow; row < endRow; ++row)
            for (int column = startColumn; column < endColumn; ++column) {
                const size_t pixel = static_cast<size_t>(row) * grid.width + column;
                if (!validLocation(pixel) || !classes.find(type[pixel])) continue;
                latMin = std::min<double>(latMin, latitude[pixel]); latMax = std::max<double>(latMax, latitude[pixel]);
                lonMin = std::min<double>(lonMin, longitude[pixel]); lonMax = std::max<double>(lonMax, longitude[pixel]);
            }
        if (latMin > latMax) throw std::runtime_error("计算范围内没有可用的像元（地类或经纬度无效）");
        std::vector<std::string> variables = era5::requiredVariables();
        if (config.soilMoistureFromEra5) {
            if (!pack.available("swvl1")) throw std::runtime_error("土壤水分来源为 ERA5，但数据包中没有 swvl1");
            variables.push_back("swvl1");
        }
        if (pack.available("stl2")) variables.push_back("stl2");
        elevationCorrection = config.elevationCorrection && config.dem.present() && pack.available("z");
        if (elevationCorrection) variables.push_back("z");
        else if (config.elevationCorrection && config.dem.present())
            log("警告：已要求按 DEM 订正气温与气压，但 ERA5 数据包没有地表位势 z，本次不做高程订正");
        // Hours the solver reads: 00 UTC of the overpass day through the hour after the overpass (flux interpolation);
        // ERA5-Land de-accumulation also needs the hour before 00 UTC.
        const int64_t firstHour = static_cast<int64_t>(dayStart / 3600.0) - (pack.landProduct() ? 1 : 0);
        int64_t lastHour = static_cast<int64_t>(std::ceil(overpass / 3600.0)) + 1;
        // TIRTEB initialises with the mean air temperature of the whole first simulated day.
        const int64_t dayLastHour = static_cast<int64_t>(std::llround(dayStart / 3600.0)) + 23;
        // FIXED[中] Bug: pack.lastHour() 返回 lastHour_，只在后面的 pack.load() 里赋值，此处恒为默认值 -1，条件永远不成立、lastHour 不会延到当天 23 UTC；已修复：改用已有的 pack.covers(variables, dayStartHour, dayLastHour, gap) 判断数据包是否覆盖当天末小时，覆盖时才延长 lastHour（gap 提前声明，未新增常量）
        const int64_t dayStartHour = static_cast<int64_t>(std::llround(dayStart / 3600.0));
        std::string gap;
        if (pack.covers(variables, dayStartHour, dayLastHour, gap)) lastHour = std::max(lastHour, dayLastHour);
        // A whole-year pack keeps the year-edge rule: hours just outside the year reuse the nearest one.
        if (!pack.covers(variables, firstHour, lastHour, gap) && !(pack.fullYear() && pack.year() == when.year))
            throw std::runtime_error("ERA5 数据包未覆盖本次能量平衡所需时段 " + formatUtc(firstHour * 3600.0) + " – " + formatUtc(lastHour * 3600.0) +
                                     "（" + gap + "）。请补充该时段的逐小时数据后重新登记");
        pack.load(variables, latMin, latMax, lonMin, lonMax, firstHour, lastHour);
        for (const auto& note : pack.notes()) log("ERA5：" + note);
        const double elapsed = overpass - dayStart;
        for (int hour = 0; hour * 3600.0 < elapsed - 300; ++hour) nodes.push_back(dayStart + hour * 3600.0);
        nodes.push_back(overpass);
        log("ERA5 " + pack.product() + (pack.fullYear() ? " " + std::to_string(pack.year()) + " 整年" : " 时段数据") + "：能量平衡从 " + formatUtc(dayStart) + " 推进到过境时刻，共 " +
            std::to_string(nodes.size()) + " 个时次" + (pack.available("stl2") ? "，深层土壤温度取 stl2" : "，深层土壤温度取 stl1") +
            (elevationCorrection ? "，按 DEM 与 ERA5 地形高度差订正气温与气压" : ""));
    }

    // ---------------- soil moisture ----------------
    std::vector<float> soilMoisture;
    long long moistureFallback = 0;
    if (eb && config.soilMoistureFromEra5) {
        soilMoisture.assign(grid.size(), kNaN);
        #pragma omp parallel for schedule(dynamic, 16) reduction(+:moistureFallback)
        for (int row = startRow; row < endRow; ++row)
            for (int column = startColumn; column < endColumn; ++column) {
                const size_t pixel = static_cast<size_t>(row) * grid.width + column;
                if (!validLocation(pixel)) continue;
                const PixelForcing forcing(pack, latitude[pixel], longitude[pixel], 0.0);
                const double value = forcing.state("swvl1", overpass);
                if (std::isfinite(value)) soilMoisture[pixel] = static_cast<float>(std::clamp(value, 0.0, 1.0));
                else { soilMoisture[pixel] = classes.defaultSoilMoisture; ++moistureFallback; }
            }
        log("土壤水分取 ERA5 swvl1（过境时刻）" + (moistureFallback ? "，" + std::to_string(moistureFallback) + " 个像元无 ERA5 陆面值，使用默认值" : std::string()));
    } else {
        soilMoisture = fieldRaster(config.soilMoisture, grid, "土壤水分 SM", classes.defaultSoilMoisture, false);
        // DEFERRED[低] 物理: 用户土壤水分截断到 0.05–0.55，而 ERA5 路径（上面）截断到 0–1，两种来源对同一湿度的处理不一致；这个截断还会传入 EB（sellers 土壤阻抗、Johansen 热参数），并非只影响 BSM；另外"大于 1 视为百分比"的判据会把恰为 1.0（100%）或 0.x% 的输入误判；建议：BSM 的截断只在 bsm() 调用处做，EB 使用原值；百分比换算改为配置里的显式单位字段
        // Accept legacy percent input and keep BSM inside its 0.05-0.55 range.
        for (auto& value : soilMoisture) {
            if (!std::isfinite(value)) continue;
            if (value > 1.0f) value /= 100.0f;
            value = std::clamp(value, 0.05f, 0.55f);
        }
    }

    // ---------------- spectra ----------------
    std::vector<char> typeUsed(256, 0);
    for (int row = startRow; row < endRow; ++row)
        for (int column = startColumn; column < endColumn; ++column) {
            const float value = type[static_cast<size_t>(row) * grid.width + column];
            if (classes.find(value)) typeUsed[static_cast<size_t>(std::lround(value))] = 1;
        }
    const bool needSpectra = config.opticalEnabled || eb;
    std::map<int, std::vector<float>> leafReflectance, leafTransmittance, leafRhoBins, leafTauBins, flatGroundBins;
    std::map<int, std::vector<double>> leafBandReflectance, leafBandTransmittance;
    std::vector<std::vector<double>> soilBandReflectance(kMoistureSteps + 1);
    std::vector<std::vector<float>> soilBins(kMoistureSteps + 1);
    std::vector<std::vector<std::pair<int, double>>> weights;
    SolarBins solarBins;
    if (needSpectra) {
        progress(5, "计算 PROSPECT 叶片光谱");
        const auto coeff = readOptCoeff(config.dataDir / "optipar_new.csv");
        const auto esun = readColumn(config.dataDir / "Esun_.dat", kSpectralCount);
        const auto esky = readColumn(config.dataDir / "Esky_.dat", kSpectralCount);
        std::vector<float> irradiance(kSpectralCount);
        for (int index = 0; index < kSpectralCount; ++index) irradiance[index] = esun[index] + esky[index];
        if (config.opticalEnabled) weights = opticalWeights(config.opticalBands, irradiance);
        solarBins = makeSolarBins(esun, esky, config.solarBinWidth);

        std::vector<int> leafTypes;
        for (int code = 0; code < 256; ++code)
            if (typeUsed[code] && classes.classes[code].surface == SurfaceKind::Vegetation) leafTypes.push_back(code);
        for (const int code : leafTypes) { leafReflectance[code]; leafTransmittance[code]; }
        #pragma omp parallel for schedule(dynamic, 1)
        for (int item = 0; item < static_cast<int>(leafTypes.size()); ++item) {
            const int code = leafTypes[item];
            prospect(coeff, classes.classes[code].leaf, leafReflectance.at(code), leafTransmittance.at(code));
        }
        for (const int code : leafTypes) {
            if (config.opticalEnabled) {
                auto& reflectance = leafBandReflectance[code];
                auto& transmittance = leafBandTransmittance[code];
                for (const auto& bandWeights : weights) {
                    reflectance.push_back(std::clamp(bandAverage(leafReflectance[code], bandWeights), 0.0, 1.0));
                    transmittance.push_back(std::clamp(bandAverage(leafTransmittance[code], bandWeights), 0.0, 1.0));
                }
            }
            if (eb) {
                leafRhoBins[code] = binSpectrum(solarBins, leafReflectance[code]);
                leafTauBins[code] = binSpectrum(solarBins, leafTransmittance[code]);
            }
        }
        log("PROSPECT 叶片光谱：" + std::to_string(leafTypes.size()) + " 个地类");

        progress(eb ? 7 : 20, "计算 BSM 土壤光谱");
        std::vector<char> moistureUsed(kMoistureSteps + 1, 0);
        for (int row = startRow; row < endRow; ++row)
            for (int column = startColumn; column < endColumn; ++column) {
                const size_t pixel = static_cast<size_t>(row) * grid.width + column;
                const auto* entry = classes.find(type[pixel]);
                if (!entry || entry->groundKind != GroundKind::Soil) continue;
                if (std::isfinite(soilMoisture[pixel])) moistureUsed[moistureStep(soilMoisture[pixel])] = 1;
                else if (eb) moistureUsed[moistureStep(classes.defaultSoilMoisture)] = 1;
            }
        #pragma omp parallel for schedule(dynamic, 8)
        for (int step = 0; step <= kMoistureSteps; ++step) {
            if (!moistureUsed[step]) continue;
            std::vector<float> spectrum;
            bsm(coeff, classes.soilOptics, static_cast<float>(step) / kMoistureSteps, spectrum);
            if (config.opticalEnabled) {
                std::vector<double> reflectance;
                for (const auto& bandWeights : weights) reflectance.push_back(std::clamp(bandAverage(spectrum, bandWeights), 0.0, 1.0));
                soilBandReflectance[step] = std::move(reflectance);
            }
            if (eb) soilBins[step] = binSpectrum(solarBins, spectrum);
        }
        for (const int code : leafTypes)
            if (classes.classes[code].groundKind == GroundKind::Flat)
                flatGroundBins[code] = std::vector<float>(solarBins.size(), classes.classes[code].groundReflectance);
    }

    const size_t pixels = grid.size();
    // ---------------- component temperatures ----------------
    std::array<std::vector<float>, 4> components;
    std::vector<std::vector<float>> forcingOut, fluxOut;
    std::vector<float> diffuseFractionOut(pixels, kNaN);
    long long ebPixels = 0, ebConverged = 0, ebStable = 0, ebWaterPixels = 0, ebFailed = 0, ebNoForcing = 0;
    constexpr int kFluxBands = 18;
    // LSCT inversion uses the run's component temperatures (RT input or EB solution) as its background.
    const bool lsctInversion = config.inversion.target == InversionTarget::Lsct;
    const bool needComponents = config.thermalEnabled || eb || lsctInversion;
    const UtcTime overpassTime = utcFromSeconds(overpass);
    const int dayOfYear = static_cast<int>(daysFromCivil(overpassTime.year, overpassTime.month, overpassTime.day) -
                                           daysFromCivil(overpassTime.year, 1, 1)) + 1;
    // Optional receiving-surface orientation for the shortwave forcing: one plane, a per-pixel DEM
    // slope/aspect field, or a statistical cone + density composite. Elevation correction is independent.
    std::vector<float> terrainSlope, terrainAspect, coneSlope, coneDensity, ridgeSlope, ridgeAspect, ridgeDensity;
    if (config.terrainRadiation == TerrainRadiation::Hill) {
        coneSlope = fieldRaster(config.terrainSlopeField, grid, "山包坡度", 0, false);
        coneDensity = fieldRaster(config.terrainDensityField, grid, "山包密度", 0, false);
        long long invalid = 0;
        for (size_t p = 0; p < grid.size(); ++p) {
            if (!std::isfinite(coneSlope[p]) || coneSlope[p] < 0.0f || coneSlope[p] > 90.0f || !std::isfinite(coneDensity[p])) {
                coneSlope[p] = 0.0f;
                coneDensity[p] = 0.0f;
                ++invalid;
                continue;
            }
            coneDensity[p] = std::clamp(coneDensity[p], 0.0f, 1.0f);
        }
        log("地形辐射：山包复合坡（12 方位面元 + 密度），坡度/密度来自栅格" + (invalid ? "，无效像元 " + std::to_string(invalid) : ""));
    } else if (config.terrainRadiation == TerrainRadiation::Ridge) {
        ridgeSlope = fieldRaster(config.terrainSlopeField, grid, "山脊坡度", 0, false);
        ridgeAspect = fieldRaster(config.terrainAspectField, grid, "山脊走向", 0, false);
        ridgeDensity = fieldRaster(config.terrainDensityField, grid, "山脊密度", 0, false);
        long long invalid = 0;
        for (size_t p = 0; p < grid.size(); ++p) {
            if (!std::isfinite(ridgeSlope[p]) || ridgeSlope[p] < 0.0f || ridgeSlope[p] > 90.0f ||
                !std::isfinite(ridgeAspect[p]) || !std::isfinite(ridgeDensity[p])) {
                ridgeSlope[p] = 0.0f;
                ridgeAspect[p] = 0.0f;
                ridgeDensity[p] = 0.0f;
                ++invalid;
                continue;
            }
            while (ridgeAspect[p] < 0.0f) ridgeAspect[p] += 360.0f;
            while (ridgeAspect[p] >= 360.0f) ridgeAspect[p] -= 360.0f;
            ridgeDensity[p] = std::clamp(ridgeDensity[p], 0.0f, 1.0f);
        }
        log("地形辐射：山脊复合坡（两坡面 + 走向 + 密度），来自栅格" + (invalid ? "，无效像元 " + std::to_string(invalid) : ""));
    } else if (config.terrainRadiation != TerrainRadiation::None) {
        terrainSlope.assign(grid.size(), 0.0f);
        terrainAspect.assign(grid.size(), 0.0f);
        if (config.terrainRadiation == TerrainRadiation::Single) {
            terrainSlope.assign(grid.size(), static_cast<float>(config.terrainSlope));
            terrainAspect.assign(grid.size(), static_cast<float>(config.terrainAspect));
            if (config.terrainSlopeField.fromFile())
                terrainSlope = fieldRaster(config.terrainSlopeField, grid, "单一坡坡度", 0, false);
            if (config.terrainAspectField.fromFile())
                terrainAspect = fieldRaster(config.terrainAspectField, grid, "单一坡坡向", 0, false);
            long long invalid = 0;
            for (size_t p = 0; p < grid.size(); ++p) {
                if (!std::isfinite(terrainSlope[p]) || terrainSlope[p] < 0.0f || terrainSlope[p] > 90.0f ||
                    !std::isfinite(terrainAspect[p])) {
                    terrainSlope[p] = 0.0f;
                    terrainAspect[p] = 0.0f;
                    ++invalid;
                    continue;
                }
                while (terrainAspect[p] < 0.0f) terrainAspect[p] += 360.0f;
                while (terrainAspect[p] >= 360.0f) terrainAspect[p] -= 360.0f;
            }
            log("地形辐射：单一坡（逐像元一个坡度/坡向），常数或栅格" + (invalid ? "，无效像元 " + std::to_string(invalid) : ""));
        } else {
            if (!grid.hasTransform) throw std::runtime_error("面元法需要带地理参考的 DEM 与地表类型栅格");
            const double dx = grid.transform[1];
            const double dy = -grid.transform[5];
            if (!(dx > 0.0) || !(dy > 0.0)) throw std::runtime_error("面元法的 DEM 像元尺寸无效");
            long long invalid = 0;
            for (int row = 1; row < grid.height - 1; ++row) {
                for (int column = 1; column < grid.width - 1; ++column) {
                    const size_t p = static_cast<size_t>(row) * grid.width + column;
                    const double z1 = dem[p - grid.width - 1], z2 = dem[p - grid.width], z3 = dem[p - grid.width + 1];
                    const double z4 = dem[p - 1], z6 = dem[p + 1];
                    const double z7 = dem[p + grid.width - 1], z8 = dem[p + grid.width], z9 = dem[p + grid.width + 1];
                    if (!(std::isfinite(z1) && std::isfinite(z2) && std::isfinite(z3) && std::isfinite(z4) &&
                          std::isfinite(z6) && std::isfinite(z7) && std::isfinite(z8) && std::isfinite(z9))) {
                        ++invalid;
                        continue;
                    }
                    const double dzdx = ((z3 + 2 * z6 + z9) - (z1 + 2 * z4 + z7)) / (8.0 * dx);
                    const double dzds = ((z7 + 2 * z8 + z9) - (z1 + 2 * z2 + z3)) / (8.0 * dy);
                    terrainSlope[p] = static_cast<float>(std::atan(std::hypot(dzdx, dzds)) * 180.0 / kPi);
                    double aspect = std::atan2(-dzdx, dzds) * 180.0 / kPi;
                    if (aspect < 0.0) aspect += 360.0;
                    terrainAspect[p] = static_cast<float>(aspect);
                }
            }
            log("地形辐射：面元法，逐像元坡度/坡向（DEM）" + (invalid ? "，无效像元 " + std::to_string(invalid) : ""));
        }
    }
    if (eb) {
        progress(10, "能量平衡求解");
        const std::vector<float> noSpectrum;
        const int64_t dayStartHour = static_cast<int64_t>(std::llround(dayStart / 3600.0));
        const int64_t dayLastHour = std::min<int64_t>(dayStartHour + 23, pack.lastHour());
        for (auto& band : components) band.assign(pixels, kNaN);
        forcingOut.assign(7, std::vector<float>(pixels, kNaN));
        fluxOut.assign(kFluxBands, std::vector<float>(pixels, kNaN));
        std::atomic<int> rowsDone{0};
        const int rowCount = endRow - startRow;
        const auto validForcing = [](const Forcing& f) {
            return std::isfinite(f.ta) && std::isfinite(f.ea) && std::isfinite(f.p) && std::isfinite(f.wind) &&
                   std::isfinite(f.shortwave) && std::isfinite(f.longwave);
        };
        #pragma omp parallel for schedule(dynamic, 2) reduction(+:ebPixels, ebConverged, ebStable, ebWaterPixels, ebFailed, ebNoForcing)
        for (int row = startRow; row < endRow; ++row) {
            for (int column = startColumn; column < endColumn; ++column) {
                const size_t pixel = static_cast<size_t>(row) * grid.width + column;
                const auto* entry = classes.find(type[pixel]);
                if (!entry || entry->surface == SurfaceKind::Fill || !validLocation(pixel)) continue;
                const float leafArea = lai[pixel];
                // DEFERRED[中] 错误处理: LAI 上限 100 太宽。MODIS LAI（×0.1）的填充码 249–254（水体、裸地、冰雪、城市等）若未声明为 nodata，经 scale 后变成 24.9–25.4，会被当成有效 LAI 参与 EB 与成像；另外这条检查放在水体分支前面，水体像元的 LAI 一旦是 nodata，就连 skt 也拿不到；建议：把上限收紧到约 10–15，超限时记日志计数；水体判断移到 LAI 检查之前
                if (!std::isfinite(leafArea) || leafArea < 0 || leafArea > 100) continue;
                double shift = 0.0;
                if (elevationCorrection && std::isfinite(dem[pixel])) {
                    const double orography = pack.at("z", 0, pack.weights(latitude[pixel], longitude[pixel])) / kGravity;
                    if (std::isfinite(orography)) shift = dem[pixel] - orography;
                }
                const PixelForcing forcing(pack, latitude[pixel], longitude[pixel], shift,
                                           terrainSlope.empty() ? 0.0 : terrainSlope[pixel],
                                           terrainAspect.empty() ? 0.0 : terrainAspect[pixel],
                                           config.diffuseFraction,
                                           coneSlope.empty() ? 0.0 : coneSlope[pixel],
                                           coneDensity.empty() ? 0.0 : coneDensity[pixel],
                                           ridgeSlope.empty() ? 0.0 : ridgeSlope[pixel],
                                           ridgeAspect.empty() ? 0.0 : ridgeAspect[pixel],
                                           ridgeDensity.empty() ? 0.0 : ridgeDensity[pixel],
                                           config.terrainEndFraction);
                const Forcing atOverpass = forcing.at(overpass);
                // FIXED[中] 错误处理: 原来只检查 ta 和 longwave，d2m/sp/u10/v10/ssrd 缺测时 ea/p/wind/shortwave 为 NaN，残差变 NaN、组分温度停在初值却看似有效；已修复：用 validForcing 对 ta/ea/p/wind/shortwave/longwave 全部做 isfinite 检查，并在逐时次循环里同样检查 meteo，无效像元计入 ebNoForcing/ebFailed
                if (!validForcing(atOverpass)) { ++ebNoForcing; continue; }
                const double values[7] = {atOverpass.ta, atOverpass.ea, atOverpass.p, atOverpass.wind, atOverpass.shortwave, atOverpass.longwave, soilMoisture[pixel]};
                for (int k = 0; k < 7; ++k) forcingOut[k][pixel] = static_cast<float>(values[k]);
                if (entry->surface == SurfaceKind::Water) {
                    const float skin = static_cast<float>(forcing.temperature("skt", overpass));
                    for (auto& band : components) band[pixel] = skin;
                    ++ebWaterPixels;
                    continue;
                }
                const int code = static_cast<int>(std::lround(type[pixel]));
                EnergySurface surface;
                surface.lai = leafArea;
                surface.height = entry->canopy.height;
                surface.leafWidth = entry->canopy.leafWidth;
                surface.hotspot = entry->canopy.hotspot;
                surface.leafEmissivity = config.leafEmissivity;
                surface.soilMoisture = std::isfinite(soilMoisture[pixel]) ? soilMoisture[pixel] : classes.defaultSoilMoisture;
                // DEFERRED[低] 物理: 冰雪（15）按普通平面地面处理：没有 273.15 K 上限，没有融化潜热，饱和水汽压也用的是水面 Tetens；中午强辐射下雪面温度可能算到 0 °C 以上，这在物理上不可能；建议：冰雪地类把 T 限制在 ≤273.15 K（超出部分能量计入融化），饱和水汽压改用冰面公式
                surface.ground = entry->ground;
                if (entry->groundKind == GroundKind::Soil) {
                    // Wet soil emits more: the broadband emissivity follows the
                    // pixel water content between the dry value and saturation.
                    surface.ground.emissivity = soilEmissivityAtMoisture(
                        config.soilEmissivity, config.soilEmissivityWet, surface.soilMoisture);
                    surface.ground.conductivity = config.soilConductivity;
                    surface.ground.volumetricCapacity = config.soilVolumetricCapacity;
                    surface.ground.moistureResistance = config.soilResistanceFromMoisture;
                    surface.ground.fixedResistance = config.soilResistance;
                    if (config.soilThermalFromMoisture) {
                        // Johansen (coarse soil): dry sand ~0.2 W m-1 K-1 / inertia ~600, wet soil ~1.4 / ~1900.
                        const double porosity = std::clamp(surface.ground.saturatedMoisture, 0.2, 0.7);
                        const double theta = std::clamp(surface.soilMoisture, 0.0, porosity);
                        const double bulk = (1.0 - porosity) * 2700.0;
                        const double dry = (0.135 * bulk + 64.7) / (2700.0 - 0.947 * bulk);
                        const double saturated = std::pow(3.0, 1.0 - porosity) * std::pow(0.57, porosity);
                        const double saturation = theta / porosity;
                        const double kersten = saturation > 0.05 ? std::clamp(0.7 * std::log10(saturation) + 1.0, 0.0, 1.0) : 0.0;
                        surface.ground.conductivity = dry + kersten * (saturated - dry);
                        surface.ground.volumetricCapacity = (1.0 - porosity) * 2.0e6 + theta * 4.18e6;
                    }
                }
                surface.leaf = entry->photosynthesis;
                surface.aero = classes.aerodynamics;
                surface.aero.minWindSpeed = config.minWindSpeed;
                surface.aero.maxResistance = config.maxAerodynamicResistance;
                if (config.heatRoughnessKb > 0.0) surface.ground.heatRoughnessKb = config.heatRoughnessKb;
                surface.referenceHeight = config.referenceHeight;
                surface.co2 = classes.co2;
                surface.o2 = classes.o2;
                surface.stomatalG0 = config.stomatalG0;
                surface.diffuseFraction = config.diffuseFraction;
                surface.enforceSolarLimit = config.enforceSolarLimit;
                surface.dayOfYear = dayOfYear;
                surface.soilHeatMethod = config.soilHeatMethod;
                surface.robustTemperature = config.robustTemperature;
                surface.soilHeatFraction = config.soilHeatFraction;
                surface.maxIterations = config.maxIterations;
                surface.maxStabilityIterations = config.maxStabilityIterations;
                const std::vector<float>& ground = entry->groundKind == GroundKind::Flat ? flatGroundBins.at(code) : soilBins[moistureStep(surface.soilMoisture)];
                const auto leafRho = leafRhoBins.find(code), leafTau = leafTauBins.find(code);
                const std::vector<float>& rho = leafRho != leafRhoBins.end() ? leafRho->second : noSpectrum;
                const std::vector<float>& tau = leafTau != leafTauBins.end() ? leafTau->second : noSpectrum;
                if ((leafArea > 0 && rho.empty()) || ground.empty()) { ++ebFailed; continue; }
                const OpticalSpectra spectra{&rho, &tau, &ground};

                // TIRTEB initialises soil and leaf temperatures with the first simulated day's mean air temperature.
                double initialSum = 0.0;
                int initialCount = 0;
                for (int64_t hour = dayStartHour; hour <= dayLastHour; ++hour) {
                    const double value = forcing.temperature("t2m", static_cast<double>(hour) * 3600.0);
                    if (std::isfinite(value)) { initialSum += value; ++initialCount; }
                }
                const double initial = initialCount > 0 ? initialSum / initialCount : forcing.temperature("t2m", dayStart);
                // Soil starts from the ERA5 skin temperature at 00 UTC (the pack already carries skt, stl1 and stl2,
                // which the run log promises); the 2 m column is anchored to the ERA5 soil levels. Leaves hold no heat
                // and converge in one step, so they keep the day-mean air temperature. With the day-mean air start the
                // soil was up to 6 K too warm at 06-08 local time and the first step had G = 0 (empty history).
                const double skin = forcing.temperature("skt", dayStart);
                const double level1 = forcing.temperature("stl1", dayStart);
                const double level2 = pack.available("stl2") ? forcing.temperature("stl2", dayStart) : level1;
                const bool soilFromEra5 = std::isfinite(skin) && skin > 180.0 && skin < 340.0;
                const double soilInitial = soilFromEra5 ? skin : initial;
                EnergyState state;
                state.soilSunlit = state.soilShaded = soilInitial;
                state.leafSunlit = state.leafShaded = initial;
                state.soilSunState.history = {soilInitial};
                state.soilShadeState.history = {soilInitial};
                if (soilFromEra5 && config.soilHeatMethod == SoilHeatMethod::Depth) {
                    initializeColumn(state.soilSunState.profile, soilInitial,
                                     std::isfinite(level1) ? level1 : soilInitial, std::isfinite(level2) ? level2 : soilInitial);
                    state.soilShadeState.profile = state.soilSunState.profile;
                }

                EnergyDiagnostics result;
                bool solved = true;
                double partitionLimited = 0.0;
                for (size_t node = 0; node < nodes.size(); ++node) {
                    const double time = nodes[node];
                    const Sun sun = solarPosition(time, latitude[pixel], longitude[pixel], dem[pixel]);
                    if (!sun.ok) { solved = false; break; }
                    const Forcing meteo = forcing.at(time);
                    if (!validForcing(meteo)) { solved = false; break; }
                    // DEFERRED[低] 物理: meteo.shortwave 由 NOAA 近似 cos(SZA) 决定是否大于 0，而这里的 sunlit 用的是 SPA 天顶角。日出日落附近可能出现 SPA SZA≥90 而短波>0 的情况；enforceSolarLimit=true 时直射上限为 0，会全部转成漫射，没有问题；但 enforceSolarLimit=false 时 absorbShortwave 在 !lit 下令 esun=0，直射部分 (1−fd)·SW 被直接丢掉，能量不守恒；建议：两处用同一套太阳位置，或者 !lit 时把直射全部计入漫射
                    const ShortwaveAbsorption shortwave = absorbShortwave(
                        solarBins, spectra, leafArea, meteo.shortwave, sun.zenith, dayOfYear, config.diffuseFraction,
                        config.enforceSolarLimit, sun.zenith < 90.0);
                    partitionLimited = shortwave.partitionLimited ? 1.0 : 0.0;
                    diffuseFractionOut[pixel] = static_cast<float>(shortwave.diffuseFraction);
                    result = solveEnergyNode(surface, meteo, shortwave, sun.zenith, sun.zenith < 90.0,
                                             node ? time - nodes[node - 1] : 3600.0, state);
                }
                if (!solved) { ++ebFailed; continue; }
                components[0][pixel] = static_cast<float>(state.soilSunlit);
                components[1][pixel] = static_cast<float>(state.soilShaded);
                components[2][pixel] = static_cast<float>(state.leafSunlit);
                components[3][pixel] = static_cast<float>(state.leafShaded);
                if (!(leafArea > 0.0f)) { components[2][pixel] = kNaN; components[3][pixel] = kNaN; }
                const double obukhov = std::isfinite(result.obukhovLength) ? result.obukhovLength : std::numeric_limits<double>::quiet_NaN();
                const double fluxes[kFluxBands] = {
                    result.netRadiation, result.sensible, result.latent, result.ground,
                    static_cast<double>(result.iterations), result.converged ? 1.0 : 0.0,
                    obukhov, result.ustar, result.stabilityConverged ? 1.0 : 0.0,
                    result.residual, result.stabilityResidual, result.stabilityLimited ? 1.0 : 0.0,
                    result.gustResidual, result.airDensity, result.roughness, result.displacement,
                    result.canopyWeight, partitionLimited};
                for (int k = 0; k < kFluxBands; ++k) fluxOut[k][pixel] = static_cast<float>(fluxes[k]);
                ++ebPixels;
                if (result.converged) ++ebConverged;
                if (result.stabilityConverged) ++ebStable;
            }
            const int done = ++rowsDone;
            if (isMainThread() && done % std::max(1, rowCount / 40) == 0) progress(10 + 60.0 * done / rowCount, "能量平衡求解");
        }
        log("能量平衡：" + std::to_string(ebPixels) + " 个像元，过境时刻收敛 " + std::to_string(ebConverged) + " 个（稳定度迭代收敛 " +
            std::to_string(ebStable) + " 个），水体取 ERA5 表皮温度 " +
            std::to_string(ebWaterPixels) + " 个" + (ebFailed ? "，未能求解 " + std::to_string(ebFailed) + " 个" : "") +
            (ebNoForcing ? "，ERA5 无有效值（如 ERA5-Land 海面）跳过 " + std::to_string(ebNoForcing) + " 个" : ""));
    } else if (config.thermalEnabled || lsctInversion) {
        const char* labels[] = {"光照土壤温度", "阴影土壤温度", "光照叶片温度", "阴影叶片温度"};
        if (!config.componentsFile.empty()) {
            auto dataset = openRaster(config.componentsFile, "组分温度文件");
            if (dataset->GetRasterCount() < 4) throw std::runtime_error("组分温度文件须有 4 个波段（Tss、Tsh、Tcs、Tch）");
            for (int index = 0; index < 4; ++index) components[index] = readBand(*dataset, index + 1, grid, labels[index]);
            log("组分温度来自 " + config.componentsFile);
        } else {
            int automaticBand = 0;
            for (int index = 0; index < 4; ++index) components[index] = componentRaster(config.components[index], grid, labels[index], overpass, automaticBand);
            if (automaticBand) log("24 波段组分温度按 UTC 小时自动选用第 " + std::to_string(automaticBand) + " 波段");
        }
    }
    if (needComponents)
        for (auto& band : components)
            // DEFERRED[低] 错误处理: RT 模式下若组分温度误给成 °C（如 25），会在这里静默变成 NaN，最后只体现为 thermalFailed 计数，没有任何单位提示；建议：统计被剔除的像元，大多数值落在 −50..60 时报错或警告"组分温度须为 K"
            for (auto& value : band) if (!(value >= 100 && value <= 400)) value = kNaN;

    // ---------------- imaging ----------------
    std::vector<std::vector<float>> optical(config.opticalBands.size(), std::vector<float>(config.opticalEnabled ? pixels : 0, kNaN));
    std::vector<std::vector<float>> brightness(config.thermalBands.size(), std::vector<float>(config.thermalEnabled ? pixels : 0, kNaN));
    std::vector<std::vector<float>> surfaceTemperature(config.thermalBands.size(), std::vector<float>(config.thermalEnabled ? pixels : 0, kNaN));
    std::vector<std::vector<float>> solar(2, std::vector<float>(pixels, kNaN));
    // One Hapke directional-emissivity table pair per soil class: dry anchor and saturation anchor.
    std::map<int, MoistureSoilEmissivity> soilEmissivityTables;
    if ((config.thermalEnabled || config.inversion.enabled()) && config.hapkeSoilEmissivity) {
        for (int code = 0; code < 256; ++code) {
            if (!typeUsed[code]) continue;
            const auto& entry = classes.classes[code];
            if (entry.surface == SurfaceKind::Fill || entry.groundKind != GroundKind::Soil) continue;
            const double dry = std::clamp(static_cast<double>(entry.ground.emissivity), 1e-6, 1.0);
            soilEmissivityTables[code].build(config.hapkeSoilK, dry,
                soilSaturatedEmissivity(dry, config.soilEmissivityWet));
        }
        if (!soilEmissivityTables.empty()) {
            const auto& first = *soilEmissivityTables.begin();
            const auto& sample = first.second;
            log("土壤方向发射率：Hapke（K = " + format(config.hapkeSoilK, 2) + "），天底 " + format(sample.dry.at(0.0), 4) +
                "，视角 55° " + format(sample.dry.at(55.0), 4) + "，半球均值等于地类值");
            log("调试：地类 " + std::to_string(first.first) + " 半球发射率 " +
                format(std::clamp(static_cast<double>(classes.classes[first.first].ground.emissivity), 1e-6, 1.0), 4) +
                "，hemi(0) " + format(hapkeHemisphericalEmissivity(config.hapkeSoilK, 0.0), 4) +
                "，hemi(0.2) " + format(hapkeHemisphericalEmissivity(config.hapkeSoilK, 0.2), 4));
            log("土壤发射率随含水量变化：干土（SMC ≤ " + format(kSoilDryWaterContent, 2) + "）" +
                format(sample.dry.at(0.0), 4) + "，饱和 " + format(sample.wet.at(0.0), 4) +
                "（天底；半球值同比例变化，可用 energy.soilEmissivityWet 指定饱和值）");
        }
    }
    // ---------------- inversion ----------------
    // LST / LSCT retrieval from the configured thermal observations. For LSCT the posterior component temperatures
    // replace the background for imaging, so the simulated bands are the forward image of the retrieved state.
    const double inversionStart = eb ? 70 : 20, inversionSpan = config.inversion.enabled() ? (eb ? 10 : 35) : 0;
    InversionResult inversionResult;
    std::array<std::vector<float>, 4> ebComponents;  // EB solution kept for components.tif when LSCT replaces it
    if (config.inversion.enabled()) {
        const InversionInputs inputs{config, classes, grid, type, lai, vza, vaa, latitude, longitude, dem, soilMoisture,
                                     soilEmissivityTables, startRow, endRow, startColumn, endColumn, overpass,
                                     inversionStart, inversionSpan};
        inversionResult = runInversion(inputs, components);
        if (lsctInversion) {
            if (eb) ebComponents = components;
            components = inversionResult.components;
        }
    }

    long long invalidInput = 0, invalidType = 0, waterPixels = 0, nightPixels = 0, opticalPixels = 0, thermalPixels = 0, thermalFailed = 0, solarFailed = 0;
    std::atomic<int> rowsDone{0};
    const int rowCount = endRow - startRow;
    const double imageStart = inversionSpan > 0 ? inversionStart + inversionSpan : (eb ? 70 : 25);
    const double imageSpan = 90 - imageStart;
    progress(imageStart, "逐像元成像");
    #pragma omp parallel for schedule(dynamic, 4) reduction(+:invalidInput, invalidType, waterPixels, nightPixels, opticalPixels, thermalPixels, thermalFailed, solarFailed)
    for (int row = startRow; row < endRow; ++row) {
        for (int column = startColumn; column < endColumn; ++column) {
            const size_t pixel = static_cast<size_t>(row) * grid.width + column;
            const auto* entry = classes.find(type[pixel]);
            if (!entry) { ++invalidType; continue; }
            const float viewZenith = vza[pixel], viewAzimuth = vaa[pixel];
            const float leafArea = lai[pixel];
            if (!validLocation(pixel) || !std::isfinite(viewZenith) || viewZenith < 0 || viewZenith >= 90 || !std::isfinite(viewAzimuth) ||
                viewAzimuth < -360 || viewAzimuth > 360 || !std::isfinite(leafArea) || leafArea < 0 || leafArea > 100) { ++invalidInput; continue; }
            const Sun sun = solarPosition(overpass, latitude[pixel], longitude[pixel], dem[pixel]);
            if (!sun.ok) { ++solarFailed; continue; }
            const double sza = sun.zenith, saa = sun.azimuth;
            solar[0][pixel] = static_cast<float>(sza);
            solar[1][pixel] = static_cast<float>(saa);
            const double view = std::min<double>(viewZenith, 89.9);
            // Thermal view/sun relative to the local terrain normal (composite terrain). The optical
            // reflectance keeps the horizontal geometry; the directional LST must see the slope.
            double thermalView = view;
            double thermalSza = sza;
            bool backface = false;
            if (config.terrainRadiation == TerrainRadiation::Facet && !terrainSlope.empty()) {
                const double beta = terrainSlope[pixel] * kPi / 180.0;
                const double aspect = terrainAspect[pixel] * kPi / 180.0;
                const double vzaRad = viewZenith * kPi / 180.0;
                const double vaaRad = viewAzimuth * kPi / 180.0;
                const double cosView = std::cos(beta) * std::cos(vzaRad) +
                                       std::sin(beta) * std::sin(vzaRad) * std::cos(vaaRad - aspect);
                backface = cosView <= 0.0;
                if (!backface) thermalView = std::acos(std::clamp(cosView, -1.0, 1.0)) * 180.0 / kPi;
                const double szaRad = sza * kPi / 180.0;
                const double saaRad = saa * kPi / 180.0;
                const double cosSun = std::cos(beta) * std::cos(szaRad) +
                                      std::sin(beta) * std::sin(szaRad) * std::cos(saaRad - aspect);
                thermalSza = std::acos(std::clamp(cosSun, -1.0, 1.0)) * 180.0 / kPi;
            }
            // DEFERRED[低] 可维护性: 热点判据 VAA==SAA 隐含 VAA 是"像元指向传感器"的方位角（北起顺时针，与 SPA 太阳方位同一约定）。若输入用"传感器看向像元"的约定（相差 180°），热点会落到前向散射方向，且配置和 README 都没有说明；建议：在配置/README 中写明约定，或提供 viewAzimuthConvention 选项
            const double relative = std::fmod(viewAzimuth - saa, 360.0);
            const int code = static_cast<int>(std::lround(type[pixel]));
            const bool waterOrFill = entry->surface != SurfaceKind::Vegetation;

            if (config.opticalEnabled) {
                const float moisture = soilMoisture[pixel];
                if (waterOrFill) ++waterPixels;
                else if (sza >= 90) ++nightPixels;
                else if (entry->groundKind == GroundKind::Flat || std::isfinite(moisture)) {
                    const auto& leafR = leafBandReflectance.at(code);
                    const auto& leafT = leafBandTransmittance.at(code);
                    try {
                        for (size_t band = 0; band < config.opticalBands.size(); ++band) {
                            const double ground = entry->groundKind == GroundKind::Flat ? entry->groundReflectance : soilBandReflectance[moistureStep(moisture)][band];
                            const double lr = leafR[band], lt = std::min(leafT[band], 1.0 - leafR[band]);
                            // FIXED[中] Bug: EB 模式下若该像元没有算出 diffuseFractionOut（ebNoForcing、SPA 失败等）或处于 RT 模式，会回退到 config.diffuseFraction，该值可为 −1（Erbs），使 canopyReflectance 里 direct=2、diffuse=−1；已修复：回退值不在 [0,1] 时用 requestedDiffuseFraction 现算（EB 有短波值时用该值，否则按 0 短波），不再把 −1 传进反射率
                            double diffuse = eb && std::isfinite(diffuseFractionOut[pixel])
                                ? diffuseFractionOut[pixel] : config.diffuseFraction;
                            if (!(diffuse >= 0.0 && diffuse <= 1.0))
                                diffuse = requestedDiffuseFraction(
                                    eb && !forcingOut.empty() && std::isfinite(forcingOut[4][pixel]) ? forcingOut[4][pixel] : 0.0,
                                    sza, config.diffuseFraction);
                            optical[band][pixel] = static_cast<float>(canopyReflectance(
                                leafArea, entry->canopy.hotspot, sza, saa, view, viewAzimuth, lr, lt, ground,
                                diffuse, sza < 90.0));
                        }
                        ++opticalPixels;
                    } catch (const std::exception&) { for (auto& band : optical) band[pixel] = kNaN; ++invalidInput; }
                } else ++invalidInput;
            }

            if (config.thermalEnabled && entry->surface != SurfaceKind::Fill) {
                if (backface) { ++thermalFailed; continue; }
                const double leafEmissivity = std::clamp(1.0 - entry->leaf.reflTir - entry->leaf.transTir, 0.01, 1.0);
                // Dry (class) value: the retrieval convention normalises by it.
                const double soilEmissivityFixed = std::clamp(static_cast<double>(entry->ground.emissivity), 0.01, 1.0);
                const bool hapke = config.hapkeSoilEmissivity && entry->groundKind == GroundKind::Soil;
                // Physical value: wetter soil emits more, so the radiance synthesis uses the water-content value.
                const double pixelMoisture = std::isfinite(soilMoisture[pixel])
                    ? static_cast<double>(soilMoisture[pixel])
                    : static_cast<double>(classes.defaultSoilMoisture);
                const double soilEmissivityMoist = entry->groundKind == GroundKind::Soil
                    ? soilEmissivityAtMoisture(soilEmissivityFixed, config.soilEmissivityWet, pixelMoisture)
                    : soilEmissivityFixed;
                const HomCanopy nodes(leafArea);
                const double relativeLocal = std::fmod(viewAzimuth - saa, 360.0);
                const auto weightsAt = [&](double localSza, double localView, double soilEmissivityValue, std::array<double, 4>& target) {
                    target = thermalWeights(nodes, leafArea, entry->canopy.hotspot, localSza, localView, relativeLocal,
                                            leafEmissivity, soilEmissivityValue);
                };
                std::array<double, 4> weights{}, weightsFixed{};
                weightsAt(thermalSza, thermalView, hapke ? soilEmissivityTables.at(code).at(thermalView, pixelMoisture) : soilEmissivityMoist, weights);
                // Retrieval convention: the reported LST divides by the fixed dry class emissivity, so the
                // moisture / view-angle signal stays in the radiance instead of being normalised away.
                if (config.referenceSurfaceTemperature)
                    weightsAt(thermalSza, thermalView, soilEmissivityFixed, weightsFixed);
                else weightsFixed = weights;
                try {
                    for (size_t band = 0; band < config.thermalBands.size(); ++band) {
                        const double wavelength = config.thermalBands[band].center;
                        const bool hillMode = config.terrainRadiation == TerrainRadiation::Hill && !coneSlope.empty();
                        const bool ridgeMode = config.terrainRadiation == TerrainRadiation::Ridge && !ridgeSlope.empty();
                        if (hillMode || ridgeMode) {
                            const double beta = (hillMode ? coneSlope[pixel] : ridgeSlope[pixel]) * kPi / 180.0;
                            const double coverage = std::clamp(static_cast<double>(hillMode ? coneDensity[pixel] : ridgeDensity[pixel]), 0.0, 1.0);
                            const double strike = ridgeMode ? ridgeAspect[pixel] * kPi / 180.0 : 0.0;
                            const int facetCount = hillMode ? 12 : 4;
                            const double endFraction = std::clamp(config.terrainEndFraction, 0.0, 0.5);
                            const double vzaRad = viewZenith * kPi / 180.0;
                            const double vaaRad = viewAzimuth * kPi / 180.0;
                            const double szaRad = sza * kPi / 180.0;
                            const double saaRad = saa * kPi / 180.0;
                            double radiance = 0.0, effective = 0.0, area = 0.0;
                            const auto addFacet = [&](double localSza, double localView, double facetArea, double cosView) {
                                std::array<double, 4> w{}, wf{};
                                const double soilE = hapke ? std::clamp(soilEmissivityTables.at(code).at(localView, pixelMoisture), 0.01, 1.0) : soilEmissivityMoist;
                                weightsAt(localSza, localView, soilE, w);
                                if (config.referenceSurfaceTemperature) weightsAt(localSza, localView, soilEmissivityFixed, wf); else wf = w;
                                double L = 0.0, E = 0.0;
                                for (int component = 0; component < 4; ++component) {
                                    const double temperature = components[component][pixel];
                                    if (!std::isfinite(temperature)) {
                                        if (w[component] > 0.0 || wf[component] > 0.0) throw std::runtime_error("组分温度无效");
                                        continue;
                                    }
                                    if (w[component] > 0.0) L += w[component] * planck(temperature, wavelength);
                                    if (wf[component] > 0.0) E += wf[component];
                                }
                                const double projection = facetArea * cosView;
                                radiance += projection * L;
                                effective += projection * E;
                                area += projection;
                            };
                            for (int facet = 0; facet < facetCount; ++facet) {
                                double facetBeta = beta;
                                double facetPhi = 0.0;
                                double facetWeight = 0.0;
                                if (hillMode) {
                                    facetPhi = facet * 30.0 * kPi / 180.0;
                                    facetWeight = 1.0 / 12.0;
                                } else if (facet < 2) {
                                    facetPhi = strike + (facet == 0 ? 0.5 * kPi : -0.5 * kPi);
                                    facetWeight = 0.5 * (1.0 - endFraction);
                                } else {
                                    facetBeta = 0.5 * kPi;
                                    facetPhi = strike + (facet == 2 ? 0.0 : kPi);
                                    facetWeight = 0.5 * endFraction;
                                }
                                const double cosView = std::cos(facetBeta) * std::cos(vzaRad) +
                                                       std::sin(facetBeta) * std::sin(vzaRad) * std::cos(vaaRad - facetPhi);
                                if (cosView <= 0.0) continue;
                                const double localView = std::acos(std::clamp(cosView, -1.0, 1.0)) * 180.0 / kPi;
                                const double cosSun = std::cos(facetBeta) * std::cos(szaRad) +
                                                      std::sin(facetBeta) * std::sin(szaRad) * std::cos(saaRad - facetPhi);
                                const double localSza = std::acos(std::clamp(cosSun, -1.0, 1.0)) * 180.0 / kPi;
                                addFacet(localSza, localView, coverage * facetWeight, cosView);
                            }
                            {
                                std::array<double, 4> w{}, wf{};
                                const double soilE = hapke ? std::clamp(soilEmissivityTables.at(code).at(thermalView, pixelMoisture), 0.01, 1.0) : soilEmissivityMoist;
                                weightsAt(thermalSza, thermalView, soilE, w);
                                if (config.referenceSurfaceTemperature) weightsAt(thermalSza, thermalView, soilEmissivityFixed, wf); else wf = w;
                                double L = 0.0, E = 0.0;
                                for (int component = 0; component < 4; ++component) {
                                    const double temperature = components[component][pixel];
                                    if (!std::isfinite(temperature)) {
                                        if (w[component] > 0.0 || wf[component] > 0.0) throw std::runtime_error("组分温度无效");
                                        continue;
                                    }
                                    if (w[component] > 0.0) L += w[component] * planck(temperature, wavelength);
                                    if (wf[component] > 0.0) E += wf[component];
                                }
                                const double cosView = std::cos(thermalView * kPi / 180.0);
                                const double projection = (1.0 - coverage) * cosView;
                                radiance += projection * L;
                                effective += projection * E;
                                area += projection;
                            }
                            if (!(radiance > 0.0) || !(area > 0.0)) throw std::runtime_error("无效辐亮度");
                            const double meanRadiance = radiance / area;
                            const double meanEffective = std::clamp(effective / area, 1e-6, 1.0);
                            brightness[band][pixel] = static_cast<float>(inversePlanck(meanRadiance, wavelength));
                            surfaceTemperature[band][pixel] = static_cast<float>(inversePlanck(meanRadiance / meanEffective, wavelength));
                        } else {
                            double radiance = 0.0, effective = 0.0;
                            for (int component = 0; component < 4; ++component) {
                                const double temperature = components[component][pixel];
                                if (!std::isfinite(temperature)) {
                                    if (weights[component] > 0.0 || weightsFixed[component] > 0.0) throw std::runtime_error("组分温度无效");
                                    continue;
                                }
                                if (weights[component] > 0.0) radiance += weights[component] * planck(temperature, wavelength);
                                if (weightsFixed[component] > 0.0) effective += weightsFixed[component];
                            }
                            if (!(radiance > 0.0)) throw std::runtime_error("无效辐亮度");
                            effective = std::clamp(effective, 1e-6, 1.0);
                            brightness[band][pixel] = static_cast<float>(inversePlanck(radiance, wavelength));
                            surfaceTemperature[band][pixel] = static_cast<float>(inversePlanck(radiance / effective, wavelength));
                        }
                    }
                    ++thermalPixels;
                } catch (const std::exception&) {
                    for (size_t band = 0; band < config.thermalBands.size(); ++band) brightness[band][pixel] = surfaceTemperature[band][pixel] = kNaN;
                    ++thermalFailed;
                }
            }
        }
        const int done = ++rowsDone;
        if (isMainThread() && done % std::max(1, rowCount / 40) == 0) progress(imageStart + imageSpan * done / rowCount, "逐像元成像");
    }

    // ---------------- outputs ----------------
    progress(92, "写出结果影像");
    const fs::path& outputDir = config.outputDir;
    CPLJSONObject summary;
    summary.Add("kind", "stream-image-result");
    summary.Add("schemaVersion", 2);
    summary.Add("model", "STREAM image simulation: PROSPECT + BSM; TiRT standard four-component closure (hom3 node radiosity, hotspot); TIRTEB energy balance; NREL SPA");
    summary.Add("closure", "tirt-standard");
    summary.Add("atmosphere", "none (top-of-canopy)");
    summary.Add("overpass", formatUtc(overpass));
    CPLJSONObject gridInfo;
    gridInfo.Add("width", grid.width); gridInfo.Add("height", grid.height);
    gridInfo.Add("startRow", startRow); gridInfo.Add("endRow", endRow);
    gridInfo.Add("startColumn", startColumn); gridInfo.Add("endColumn", endColumn);
    summary.Add("grid", gridInfo);
    CPLJSONObject counts;
    counts.Add("opticalPixels", static_cast<GInt64>(opticalPixels));
    counts.Add("thermalPixels", static_cast<GInt64>(thermalPixels));
    counts.Add("invalidInputPixels", static_cast<GInt64>(invalidInput));
    counts.Add("unknownTypePixels", static_cast<GInt64>(invalidType));
    counts.Add("waterOrFillPixels", static_cast<GInt64>(waterPixels));
    counts.Add("nightPixels", static_cast<GInt64>(nightPixels));
    counts.Add("solarFailedPixels", static_cast<GInt64>(solarFailed));
    counts.Add("thermalFailedPixels", static_cast<GInt64>(thermalFailed));
    summary.Add("counts", counts);
    CPLJSONObject temperatureInfo;
    temperatureInfo.Add("mode", eb ? "eb" : "rt");
    CPLJSONArray products;

    const std::vector<std::string> geometryNames{"solar zenith angle (deg)", "solar azimuth angle (deg)"};
    writeTiff(outputDir / "solar_geometry.tif", grid, solar, geometryNames);
    products.Add("solar_geometry.tif");
    CPLJSONArray solarStats;
    for (size_t index = 0; index < solar.size(); ++index) solarStats.Add(bandStatistics(geometryNames[index], solar[index]));
    summary.Add("solarGeometry", solarStats);
    if (config.inversion.enabled()) {
        for (const auto& name : inversionResult.products) products.Add(name);
        summary.Add("inversion", inversionResult.summary);
        if (lsctInversion) temperatureInfo.Add("posterior", "lsct");
    }

    if (config.opticalEnabled) {
        std::vector<std::string> names;
        CPLJSONArray stats;
        for (size_t index = 0; index < config.opticalBands.size(); ++index) {
            names.push_back(config.opticalBands[index].name + " reflectance @ " + format(config.opticalBands[index].center, 0) + " nm");
            stats.Add(bandStatistics(names.back(), optical[index]));
        }
        writeTiff(outputDir / "optical_reflectance.tif", grid, optical, names);
        products.Add("optical_reflectance.tif");
        summary.Add("optical", stats);
    }
    if (config.thermalEnabled) {
        std::vector<std::string> brightnessNames, surfaceNames;
        CPLJSONArray stats;
        for (size_t index = 0; index < config.thermalBands.size(); ++index) {
            const auto suffix = " @ " + format(config.thermalBands[index].center * 1000, 0) + " nm";
            brightnessNames.push_back(config.thermalBands[index].name + " brightness temperature (K)" + suffix);
            surfaceNames.push_back(config.thermalBands[index].name + " surface temperature (K)" + suffix);
            stats.Add(bandStatistics(brightnessNames.back(), brightness[index]));
            stats.Add(bandStatistics(surfaceNames.back(), surfaceTemperature[index]));
        }
        writeTiff(outputDir / "tir_brightness_temperature.tif", grid, brightness, brightnessNames);
        writeTiff(outputDir / "tir_surface_temperature.tif", grid, surfaceTemperature, surfaceNames);
        products.Add("tir_brightness_temperature.tif");
        products.Add("tir_surface_temperature.tif");
        summary.Add("thermal", stats);
    }
    if (eb) {
        const std::vector<std::string> componentNames{"Tss sunlit soil temperature (K)", "Tsh shaded soil temperature (K)",
                                                      "Tcs sunlit leaf temperature (K)", "Tch shaded leaf temperature (K)"};
        const auto& solved = ebComponents[0].empty() ? components : ebComponents;
        std::vector<std::vector<float>> componentBands(solved.begin(), solved.end());
        writeTiff(outputDir / "components.tif", grid, componentBands, componentNames);
        products.Add("components.tif");
        CPLJSONArray componentStats;
        for (size_t index = 0; index < 4; ++index) componentStats.Add(bandStatistics(componentNames[index], components[index]));
        summary.Add("components", componentStats);
        const std::vector<std::string> forcingNames{"air temperature (K)", "vapour pressure (hPa)", "surface pressure (hPa)", "wind speed (m s-1)",
                                                    "downward shortwave (W m-2)", "downward longwave (W m-2)", "soil moisture (m3 m-3)"};
        writeTiff(outputDir / "eb_forcing.tif", grid, forcingOut, forcingNames);
        products.Add("eb_forcing.tif");
        const std::vector<std::string> fluxNames{
            "net radiation (W m-2)", "sensible heat (W m-2)", "latent heat (W m-2)", "ground heat (W m-2)",
            "iterations", "converged", "Obukhov length (m)", "friction velocity (m s-1)", "stability converged",
            "energy balance residual (W m-2)", "stability residual (m-1)", "stability limited",
            "gustiness residual (W m-2)", "air density (kg m-3)", "aerodynamic roughness length (m)",
            "aerodynamic displacement (m)", "aerodynamic canopy weight", "shortwave partition limited"};
        writeTiff(outputDir / "eb_fluxes.tif", grid, fluxOut, fluxNames);
        products.Add("eb_fluxes.tif");
        CPLJSONArray fluxStats;
        for (const size_t index : {0, 1, 2, 3, 7, 9}) fluxStats.Add(bandStatistics(fluxNames[index], fluxOut[index]));
        summary.Add("fluxes", fluxStats);

        CPLJSONObject componentsInfo;
        componentsInfo.Add("kind", "stream-components");
        componentsInfo.Add("schemaVersion", 1);
        componentsInfo.Add("overpass", formatUtc(overpass));
        componentsInfo.Add("source", "eb");
        componentsInfo.Add("file", "components.tif");
        CPLJSONArray bandList;
        for (const auto& name : componentNames) bandList.Add(name);
        componentsInfo.Add("bands", bandList);
        CPLJSONDocument componentsDocument;
        componentsDocument.SetRoot(componentsInfo);
        // FIXED[低] 错误处理: components.json 的 Save 返回值原来没有检查（scene_summary.json 有检查），写失败时后续 RT 复用会找不到元数据；已修复：与 scene_summary.json 一致，Save 返回 false 时抛异常
        if (!componentsDocument.Save(utf8FromPath(outputDir / "components.json"))) throw std::runtime_error("无法写出 components.json");

        CPLJSONObject era5Info;
        era5Info.Add("product", pack.product());
        era5Info.Add("year", pack.year());
        era5Info.Add("fullYear", pack.fullYear());
        era5Info.Add("hours", formatUtc(pack.firstHour() * 3600.0) + "/" + formatUtc(pack.lastHour() * 3600.0));
        era5Info.Add("pack", utf8FromPath(config.era5Pack));
        era5Info.Add("elevationCorrection", elevationCorrection);
        era5Info.Add("soilMoisture", config.soilMoistureFromEra5 ? "era5" : "user");
        temperatureInfo.Add("era5", era5Info);
        CPLJSONArray nodeList;
        for (const double node : nodes) nodeList.Add(formatUtc(node));
        temperatureInfo.Add("nodes", nodeList);
        temperatureInfo.Add("solvedPixels", static_cast<GInt64>(ebPixels));
        temperatureInfo.Add("convergedPixels", static_cast<GInt64>(ebConverged));
        temperatureInfo.Add("stabilityConvergedPixels", static_cast<GInt64>(ebStable));
        // DEFERRED[低] 可维护性: 这段元数据写的是"1/L 割线 + 符号区间，容差 1e-7+1e-6|target|"，但 energy.cpp 实际是 0.5 松弛迭代，容差 1e-4+2%|target|，结果文件描述与实现不符；建议：按实现更新文字（README/PORTING 同理）
        temperatureInfo.Add("stability", "Monin-Obukhov coupled solve: 1/L secant with sign bracket, H-only buoyancy, tolerance 1e-7 + 1e-6|target|");
        temperatureInfo.Add("energyScheme", "tirt");
        temperatureInfo.Add("soilHeatMethod", config.soilHeatMethod == SoilHeatMethod::Depth ? "depth" :
                                               config.soilHeatMethod == SoilHeatMethod::Surface ? "surface" : "direct");
        temperatureInfo.Add("temperatureMethod", config.robustTemperature ? "robust" : "direct");
        temperatureInfo.Add("waterPixels", static_cast<GInt64>(ebWaterPixels));
        temperatureInfo.Add("failedPixels", static_cast<GInt64>(ebFailed));
        temperatureInfo.Add("noForcingPixels", static_cast<GInt64>(ebNoForcing));
        temperatureInfo.Add("moistureFallbackPixels", static_cast<GInt64>(moistureFallback));
        CPLJSONArray notes;
        for (const auto& note : pack.notes()) notes.Add(note);
        temperatureInfo.Add("notes", notes);
    }
    summary.Add("temperature", temperatureInfo);
    summary.Add("products", products);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    summary.Add("elapsedSeconds", elapsed);
    CPLJSONDocument output;
    output.SetRoot(summary);
    if (!output.Save(utf8FromPath(outputDir / "scene_summary.json"))) throw std::runtime_error("无法写出 scene_summary.json");

    log("光学有效像元 " + std::to_string(opticalPixels) + "，热红外有效像元 " + std::to_string(thermalPixels) + "，输入无效 " +
        std::to_string(invalidInput) + "，未知地类 " + std::to_string(invalidType) +
        (config.opticalEnabled ? "，水体/填充值 " + std::to_string(waterPixels) + "，夜间 " + std::to_string(nightPixels) : ""));
    if (config.opticalEnabled && nightPixels) log("警告：部分像元太阳在地平线以下，光学反射率记为空值");
    log("完成，耗时 " + format(elapsed, 1) + " 秒，输出 " + utf8FromPath(outputDir));
    progress(100, "完成");
    return 0;
}

}  // namespace stream


