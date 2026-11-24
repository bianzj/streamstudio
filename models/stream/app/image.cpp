#include "image.h"
#include "assimilation.h"
#include "config.h"
#include "inversion.h"
#include "thermal_view.h"
#include "../forcing/era5.h"
#include "../forcing/forcing_metadata.h"
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
    // One soil emissivity for the EB balance and the imaging / retrieval (2026-11-20): an explicit
    // energy.soilEmissivity replaces the class value of every soil-ground class; otherwise both use classes.json.
    if (config.soilEmissivitySet) {
        for (auto& entry : classes.classes)
            if (entry.surface != SurfaceKind::Missing && entry.surface != SurfaceKind::Fill && entry.groundKind == GroundKind::Soil)
                entry.ground.emissivity = static_cast<float>(config.soilEmissivity);
        log("土壤（干端）发射率按配置 " + format(config.soilEmissivity, 3) + "，能量平衡与成像共用");
    }
    const double overpass = config.overpass;
    // FIX(2026-11-21 B10, 2026-11-24): the march start is resolved from temperature.spinupHours = number of whole-hour
    // ERA5 nodes before the overpass (default 7, see config.h); counted backwards from the overpass, never from a fixed
    // clock time. marchStart (below) replaces the former dayStart (00 UTC of the overpass day). Original note kept:
    // DEFERRED[中] 物理: EB 起算点固定为过境当天 00 UTC，不考虑经度。对东亚/澳洲（UTC+8..+10）约 02:30 UTC 的过境，只积分当地上午 0.5–2.5 h，热历史默认 00 UTC 之前等温（无日变化记忆），G 与 Tss 偏差大；过境恰为 00:00 UTC 时只有 1 个时次，完全没有起转；建议：起转改为过境前固定时长（如 ≥24 h），或从按经度换算的当地午夜开始，并相应扩大 ERA5 读取时段
    const double dayStart = std::floor(overpass / 86400.0) * 86400.0;
    double marchStart = dayStart;        // first EB node (set from spinupHours and the pack coverage)
    double spinupUsedHours = 0.0;        // overpass - marchStart, hours
    int64_t marchStartHour = static_cast<int64_t>(std::llround(dayStart / 3600.0));  // whole hour of the first node
    int spinupNodesUsed = 0;             // whole-hour nodes actually integrated before the overpass (excludes the overpass node)
    std::vector<std::string> spinupNotes; // shortfall notice for scene_summary.json temperature.notes
    log("过境时刻 " + formatUtc(overpass) + "，温度来源：" + (eb ? "EB（气象强迫驱动能量平衡）" : "RT（给定组分温度）"));

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
    // Accept 0–360 longitudes: fold everything into [-180, 180) so such pixels are not silently dropped.
    long long longitudeWrapped = 0;
    for (auto& lon : longitude)
        if (std::isfinite(lon) && (lon >= 180.0f || lon < -180.0f) && lon <= 360.0f && lon >= -360.0f) {
            lon = static_cast<float>(std::fmod(static_cast<double>(lon) + 540.0, 360.0) - 180.0);
            ++longitudeWrapped;
        }
    if (longitudeWrapped) log("经度已从 0–360 制换算到 ±180，共 " + std::to_string(longitudeWrapped) + " 个像元");

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
    bool waterSkinMissing = false;  // water pixels present but the pack has no skt: they take the 2 m air temperature
    if (eb) {
        progress(2, "读取气象数据包");
        ForcingMetadata forcingMetadata;
        if (!config.forcingMetadata.empty()) forcingMetadata = resolveForcingMetadata(config.forcingMetadata, config.forcingBaseDir);
        pack.open(config.era5Pack, config.forcingMetadata.empty() ? nullptr : &forcingMetadata);
        {
            const ForcingMetadata& used = pack.metadata();
            log("气象强迫元数据：" + used.name + "（" + (used.origin == "preset" ? "预设" : used.origin == "file" ? "文件 " + utf8FromPath(used.path) : used.origin == "pack" ? "数据包索引" : "内联") +
                "，" + (pack.standardised() ? "经元数据标准化" : "ERA5 原布局，无换算") + "）");
            for (const auto& text : pack.report().infos) log("气象强迫：" + text);
            for (const auto& text : pack.report().warnings) log("警告：" + text);
        }        const UtcTime when = utcFromSeconds(overpass);
        double latMin = 90, latMax = -90, lonMin = 180, lonMax = -180;
        bool waterPresent = false;
        for (int row = startRow; row < endRow; ++row)
            for (int column = startColumn; column < endColumn; ++column) {
                const size_t pixel = static_cast<size_t>(row) * grid.width + column;
                if (!validLocation(pixel)) continue;
                const auto* entry = classes.find(type[pixel]);
                if (!entry) continue;
                if (entry->surface == SurfaceKind::Water) waterPresent = true;
                latMin = std::min<double>(latMin, latitude[pixel]); latMax = std::max<double>(latMax, latitude[pixel]);
                lonMin = std::min<double>(lonMin, longitude[pixel]); lonMax = std::max<double>(lonMax, longitude[pixel]);
            }
        if (latMin > latMax) throw std::runtime_error("计算范围内没有可用的像元（地类或经纬度无效）");
        std::vector<std::string> variables = era5::requiredVariables();
        if (config.soilMoistureFromEra5) {
            if (!pack.available("swvl1")) throw std::runtime_error("土壤水分来源为气象强迫，但数据包中没有 swvl1");
            variables.push_back("swvl1");
        }
        // Water pixels take the ERA5 skin temperature skt (the only use left of skt). Without skt in the pack they degrade to the
        // 2 m air temperature at the overpass (warning + scene_summary note) instead of failing the run.
        if (waterPresent) {
            if (pack.available("skt")) variables.push_back("skt");
            else {
                waterSkinMissing = true;
                const std::string text = "数据包没有 skt，计算范围内的水体像元改取过境时刻 2 m 气温作为水面温度（水体通常与气温相差数 K，精度较低）";
                log("警告：" + text);
                spinupNotes.push_back("水体降级：" + text);
            }
        }
        elevationCorrection = config.elevationCorrection && config.dem.present() && pack.available("z");
        if (elevationCorrection) variables.push_back("z");
        else if (config.elevationCorrection && config.dem.present())
            log("警告：已要求按 DEM 订正气温与气压，但气象数据包没有地表位势 z，本次不做高程订正");
        // Hours the solver reads: the march start through the hour after the overpass (flux interpolation);
        // ERA5-Land de-accumulation also needs the hour before the start.
        int64_t lastHour = static_cast<int64_t>(std::ceil(overpass / 3600.0)) + 1;
        // FIX(2026-11-21 B10, 2026-11-24): march start = the spinupHours-th whole-hour ERA5 node counted backwards from
        // the last whole hour before the overpass (a whole hour within 5 min before the overpass is replaced by the
        // overpass node, the same tolerance as the node list below), i.e. the floor of the overpass time on the hourly
        // grid, not a fixed clock time. 0 nodes: only the overpass node. If the pack does not cover the start, the
        // march starts at the earliest hour the pack covers continuously up to lastHour (warning + scene_summary note).
        // (Was: 00 UTC of the overpass day; 2026-11-21: floor(overpass - 24 h).)
        const int64_t overpassHour = static_cast<int64_t>(std::floor(overpass / 3600.0));
        const int64_t lastNodeHour = static_cast<int64_t>(std::ceil((overpass - 300.0) / 3600.0)) - 1;
        const int requestedNodes = config.spinupHours;
        const int64_t desiredStartHour = requestedNodes > 0 ? lastNodeHour - (requestedNodes - 1) : overpassHour;
        int64_t startHour = std::min(desiredStartHour, overpassHour);
        const bool yearEdgePack = pack.fullYear() && pack.year() == when.year;
        {
            std::string coverageGap;
            // ERA5-Land de-accumulation also reads the hour before the start, so that hour must be covered too.
            const int64_t landLead = pack.leadHours();
            while (startHour < overpassHour && !pack.covers(variables, startHour - landLead, lastHour, coverageGap)) ++startHour;
            const int64_t usableNodes = requestedNodes > 0 ? std::max<int64_t>(0, lastNodeHour - startHour + 1) : 0;
            if (requestedNodes > 0 && usableNodes == 0 && !yearEdgePack)
                throw std::runtime_error("气象数据包在过境时刻 " + formatUtc(overpass) + " 之前没有可用的整点时次，无法起转（temperature.spinupHours = " +
                                         std::to_string(requestedNodes) + "）。请补充过境前的逐小时数据，或把起转节点数设为 0");
            if (requestedNodes > 0 && usableNodes < requestedNodes) {
                const std::string text = "气象数据包不覆盖要求的起转起点 " + formatUtc(static_cast<double>(desiredStartHour) * 3600.0) +
                    "（temperature.spinupHours = " + std::to_string(requestedNodes) + " 个时次），能量平衡改从数据包可用的最早时次 " +
                    formatUtc(static_cast<double>(startHour) * 3600.0) + " 起转，实际 " + std::to_string(usableNodes) + " 个时次；结果的土壤热历史偏短，请补充更早的逐小时数据";
                log("警告：" + text);
                spinupNotes.push_back("起转不足：" + text);
            }
            spinupNodesUsed = static_cast<int>(usableNodes);
        }
        marchStartHour = startHour;
        marchStart = requestedNodes > 0 ? static_cast<double>(startHour) * 3600.0 : overpass;
        spinupUsedHours = (overpass - marchStart) / 3600.0;
        const int64_t firstHour = startHour - pack.leadHours();
        std::string gap;
        // A whole-year pack keeps the year-edge rule: hours just outside the year reuse the nearest one.
        if (!pack.covers(variables, firstHour, lastHour, gap) && !(pack.fullYear() && pack.year() == when.year))
            throw std::runtime_error("气象数据包未覆盖本次能量平衡所需时段 " + formatUtc(firstHour * 3600.0) + " – " + formatUtc(lastHour * 3600.0) +
                                     "（" + gap + "）。请补充该时段的逐小时数据后重新登记");
        pack.load(variables, latMin, latMax, lonMin, lonMax, firstHour, lastHour);
        for (const auto& note : pack.notes()) log("气象强迫：" + note);
        // Whole-hour nodes from the march start, stopping 5 min before the overpass, then the overpass node itself.
        if (requestedNodes > 0)
            for (double node = marchStart; node < overpass - 300.0; node += 3600.0) nodes.push_back(node);
        nodes.push_back(overpass);
        spinupNodesUsed = static_cast<int>(nodes.size()) - 1;
        log("气象强迫（" + pack.product() + "）" + (pack.fullYear() ? std::to_string(pack.year()) + " 整年" : "时段数据") + "：能量平衡从 " + formatUtc(marchStart) + " 推进到过境时刻（起转 " +
            std::to_string(spinupNodesUsed) + " 个整点时次 / 请求 " + std::to_string(requestedNodes) + "，" + format(spinupUsedHours, 2) + " h），共 " +
            std::to_string(nodes.size()) + " 个时次" + (elevationCorrection ? "，按 DEM 与气象强迫网格的地形高度差订正气温与气压" : ""));
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
        log("土壤水分取气象强迫 swvl1（过境时刻）" + (moistureFallback ? "，" + std::to_string(moistureFallback) + " 个像元无强迫陆面值，使用默认值" : std::string()));
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
    // FIX(2026-11-21 C9): crown / row structure parameters are not simulated; say so instead of ignoring them silently.
    std::vector<std::pair<int, std::string>> ignoredStructureClasses;
    for (int code = 0; code < 256; ++code)
        if (typeUsed[code] && !classes.classes[code].ignoredStructure.empty()) {
            ignoredStructureClasses.push_back({code, classes.classes[code].ignoredStructure});
            log("警告：地类 " + std::to_string(code) + "（" + classes.classes[code].name + "）配置了树冠/垄行结构参数 " +
                classes.classes[code].ignoredStructure + "，本版本未接入（physics/effective_lai.cpp 未调用），按均质冠层处理");
        }
    if (!config.ignoredStructureKeys.empty()) {
        std::string keys;
        for (const auto& key : config.ignoredStructureKeys) keys += (keys.empty() ? "" : "、") + key;
        log("警告：配置中的冠层结构参数 " + keys + " 本版本未接入，按均质冠层处理");
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
        // The analysis can move soil moisture anywhere and needs neighbouring steps for dρ/dθ: build every step.
        if (config.assimilation.enabled()) std::fill(moistureUsed.begin(), moistureUsed.end(), 1);
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
    const bool assimilate = config.assimilation.enabled();
    const bool needComponents = config.thermalEnabled || eb || lsctInversion || assimilate;
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
        // FIX(2026-11-21 B-S4): the shortwave forcing integrates max(0, cos i) analytically over the cone azimuths
        // (pixel_forcing.cpp); only the thermal view composite uses 12 azimuth facets (thermal_view.cpp).
        log("地形辐射：山包复合坡（短波按锥面方位解析积分 + 密度，热红外按 12 方位面元合成），坡度/密度来自栅格" + (invalid ? "，无效像元 " + std::to_string(invalid) : ""));
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
            // Geographic grids store pixel sizes in degrees; convert to metres with the local latitude so
            // dz/dx is not inflated by ~1e5 (which would make every slope ~90 degrees).
            const bool geographicGrid = gridIsGeographic(grid);
            if (geographicGrid) log("地形辐射：DEM 为经纬度坐标，像元尺寸按纬度换算为米");
            long long invalid = 0;
            for (int row = 1; row < grid.height - 1; ++row) {
                for (int column = 1; column < grid.width - 1; ++column) {
                    const size_t p = static_cast<size_t>(row) * grid.width + column;
                    double dxMetres = dx, dyMetres = dy;
                    if (geographicGrid) {
                        const double lat = latitude[p];
                        if (!std::isfinite(lat) || std::abs(lat) >= 89.9) { ++invalid; continue; }
                        dxMetres = dx * 111320.0 * std::cos(lat * kPi / 180.0);
                        dyMetres = dy * 110540.0;
                    }
                    const double z1 = dem[p - grid.width - 1], z2 = dem[p - grid.width], z3 = dem[p - grid.width + 1];
                    const double z4 = dem[p - 1], z6 = dem[p + 1];
                    const double z7 = dem[p + grid.width - 1], z8 = dem[p + grid.width], z9 = dem[p + grid.width + 1];
                    if (!(std::isfinite(z1) && std::isfinite(z2) && std::isfinite(z3) && std::isfinite(z4) &&
                          std::isfinite(z6) && std::isfinite(z7) && std::isfinite(z8) && std::isfinite(z9))) {
                        ++invalid;
                        continue;
                    }
                    const double dzdx = ((z3 + 2 * z6 + z9) - (z1 + 2 * z4 + z7)) / (8.0 * dxMetres);
                    const double dzds = ((z7 + 2 * z8 + z9) - (z1 + 2 * z2 + z3)) / (8.0 * dyMetres);
                    terrainSlope[p] = static_cast<float>(std::atan(std::hypot(dzdx, dzds)) * 180.0 / kPi);
                    double aspect = std::atan2(-dzdx, dzds) * 180.0 / kPi;
                    if (aspect < 0.0) aspect += 360.0;
                    terrainAspect[p] = static_cast<float>(aspect);
                }
            }
            log("地形辐射：面元法，逐像元坡度/坡向（DEM）" + (invalid ? "，无效像元 " + std::to_string(invalid) : ""));
        }
    }
    // ---- EB pixel machinery: one implementation for the forward solve and the assimilation re-solve ----
    const std::vector<float> noSpectrum;
    // Fallback initial temperature (only when the 2 m air temperature at the march start is invalid): mean 2 m air temperature
    // of the first 24 h from the march start, but only of the hours loaded for the run (never later than one hour after the
    // overpass; with the 7-node default the window is the spin-up itself).
    const int64_t ebDayStartHour = marchStartHour;
    const int64_t ebDayLastHour = std::min<int64_t>(ebDayStartHour + 23, pack.lastHour());
    const auto validForcing = [](const Forcing& f) {
        return std::isfinite(f.ta) && std::isfinite(f.ea) && std::isfinite(f.p) && std::isfinite(f.wind) &&
               std::isfinite(f.shortwave) && std::isfinite(f.longwave);
    };
    const auto pixelForcing = [&](size_t pixel) {
        double shift = 0.0;
        if (elevationCorrection && std::isfinite(dem[pixel])) {
            const double orography = pack.at("z", 0, pack.weights(latitude[pixel], longitude[pixel])) / kGravity;
            if (std::isfinite(orography)) shift = dem[pixel] - orography;
        }
        return PixelForcing(pack, latitude[pixel], longitude[pixel], shift,
                            terrainSlope.empty() ? 0.0 : terrainSlope[pixel],
                            terrainAspect.empty() ? 0.0 : terrainAspect[pixel],
                            config.diffuseFraction,
                            coneSlope.empty() ? 0.0 : coneSlope[pixel],
                            coneDensity.empty() ? 0.0 : coneDensity[pixel],
                            ridgeSlope.empty() ? 0.0 : ridgeSlope[pixel],
                            ridgeAspect.empty() ? 0.0 : ridgeAspect[pixel],
                            ridgeDensity.empty() ? 0.0 : ridgeDensity[pixel],
                            config.terrainEndFraction);
    };
    const auto buildSurface = [&](const ClassParameters& entry, double leafArea, double moisture, double vcmax) {
        EnergySurface surface;
        surface.lai = leafArea;
        surface.height = entry.canopy.height;
        surface.leafWidth = entry.canopy.leafWidth;
        surface.hotspot = entry.canopy.hotspot;
        surface.leafEmissivity = config.leafEmissivity;
        surface.soilMoisture = moisture;
        // DEFERRED[低] 物理: 冰雪（15）按普通平面地面处理：没有 273.15 K 上限，没有融化潜热，饱和水汽压也用的是水面 Tetens；中午强辐射下雪面温度可能算到 0 °C 以上，这在物理上不可能；建议：冰雪地类把 T 限制在 ≤273.15 K（超出部分能量计入融化），饱和水汽压改用冰面公式
        surface.ground = entry.ground;
        if (entry.groundKind == GroundKind::Soil) {
            // Wet soil emits more: the broadband emissivity follows the
            // pixel water content between the dry value and saturation. The dry value is the class
            // ground.emissivity (shared with the imaging; overridden by energy.soilEmissivity when given).
            surface.ground.emissivity = soilEmissivityAtMoisture(entry.ground.emissivity, config.soilEmissivityWet, surface.soilMoisture);
            surface.ground.conductivity = config.soilConductivity;
            surface.ground.volumetricCapacity = config.soilVolumetricCapacity;
            surface.ground.moistureResistance = config.soilResistanceFromMoisture;
            surface.ground.fixedResistance = config.soilResistance;  // FIX(2026-11-21 B-S1): range-checked in config.cpp (0, 1e5]
            if (config.soilThermalFromMoisture)
                johansenSoilThermal<double>(surface.ground.saturatedMoisture, surface.soilMoisture,
                                            surface.ground.conductivity, surface.ground.volumetricCapacity);
        }
        surface.leaf = entry.photosynthesis;
        surface.leaf.vcmax = static_cast<float>(vcmax);
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
        return surface;
    };
    const auto groundBins = [&](const ClassParameters& entry, int code, double moisture) -> const std::vector<float>& {
        return entry.groundKind == GroundKind::Flat ? flatGroundBins.at(code) : soilBins[moistureStep(static_cast<float>(moisture))];
    };
    const auto leafBins = [&](int code, const std::vector<float>*& rho, const std::vector<float>*& tau) {
        const auto leafRho = leafRhoBins.find(code), leafTau = leafTauBins.find(code);
        rho = leafRho != leafRhoBins.end() ? &leafRho->second : &noSpectrum;
        tau = leafTau != leafTauBins.end() ? &leafTau->second : &noSpectrum;
    };
    std::atomic<long long> initialLaterHour{0}, initialDayMean{0};  // pixels whose t2m at the march start was invalid
    const auto initialState = [&](const PixelForcing& forcing) {
        // FIX(2026-11-24): every initial temperature of the EB march (soil sunlit/shaded, leaf sunlit/shaded, the soil heat
        // history of the semi-infinite method and every layer of the `depth` column) is the 2 m air temperature t2m at the
        // march start. The soil temperature is not known with certainty, so ERA5 skt/stl1/stl2 are deliberately not used
        // (the user's decision: all initial values from air temperature). With only a few spin-up nodes (default 7) the
        // air temperature at the start is closer to the state at the start than the mean air temperature of the whole day
        // (24 h from the start, mostly after the overpass), which was the former leaf start (the soil started from skt).
        // Fallbacks: the first valid t2m after the start, then the mean over the first simulated day (counted, logged once).
        const auto plausible = [](double value) { return std::isfinite(value) && value > 180.0 && value < 340.0; };
        double initial = forcing.temperature("t2m", marchStart);
        if (!plausible(initial)) {
            initial = std::numeric_limits<double>::quiet_NaN();
            for (int64_t hour = ebDayStartHour + 1; hour <= ebDayLastHour && !plausible(initial); ++hour) {
                const double value = forcing.temperature("t2m", static_cast<double>(hour) * 3600.0);
                if (plausible(value)) { initial = value; ++initialLaterHour; }
            }
            if (!plausible(initial)) {
                double initialSum = 0.0;
                int initialCount = 0;
                for (int64_t hour = ebDayStartHour; hour <= ebDayLastHour; ++hour) {
                    const double value = forcing.temperature("t2m", static_cast<double>(hour) * 3600.0);
                    if (std::isfinite(value)) { initialSum += value; ++initialCount; }
                }
                initial = initialCount > 0 ? initialSum / initialCount : forcing.temperature("t2m", marchStart);
                ++initialDayMean;
            }
        }
        EnergyState state;
        state.soilSunlit = state.soilShaded = initial;
        state.leafSunlit = state.leafShaded = initial;
        state.soilSunState.history = {initial};
        state.soilShadeState.history = {initial};
        if (config.soilHeatMethod == SoilHeatMethod::Depth) {
            initializeColumn(state.soilSunState.profile, initial, initial, initial);  // uniform column
            state.soilShadeState.profile = state.soilSunState.profile;
        }
        return state;
    };
    struct EbRun {
        EnergyDiagnostics result;
        double partitionLimited = 0.0, diffuse = std::numeric_limits<double>::quiet_NaN();
        EnergyState before;  // state entering the overpass node (soil heat history of the implicit derivative)
        Forcing meteo;
        ShortwaveAbsorption shortwave;
        double sza = 0.0, dt = 3600.0;  // sza: beam incidence angle of the receiving surface (= SZA when flat)
        bool lit = false;
    };
    const auto marchNodes = [&](size_t pixel, const EnergySurface& surface, const OpticalSpectra& spectra, double leafArea,
                                const PixelForcing& forcing, EnergyState& state, EbRun& run) {
        for (size_t node = 0; node < nodes.size(); ++node) {
            const double time = nodes[node];
            const Sun sun = solarPosition(time, latitude[pixel], longitude[pixel], dem[pixel]);
            if (!sun.ok) return false;
            Forcing meteo = forcing.at(time);
            if (!validForcing(meteo)) return false;
            // Receiving-surface shortwave (2026-11-20). Terrain: split the horizontal flux (DNI limit at the true
            // SZA), tilt direct by cos i and diffuse by the sky view, then solve with that fixed share and the local
            // beam incidence angle (canopy extinction, sunlit fractions, longwave split). Flat: unchanged.
            const SurfaceShortwave surfaceFlux = forcing.surfaceShortwave(time, sun.zenith, dayOfYear, config.enforceSolarLimit, sun.azimuth);
            // FIX(2026-11-21 C10): the horizontal flux of the node uses the same SPA sun as the beam geometry below
            // (Forcing::shortwave decided "sun up" with the NOAA approximation).
            meteo.shortwave = surfaceFlux.horizontal;
            double beamZenith = sun.zenith;
            bool beamLit = sun.zenith < 90.0;
            ShortwaveAbsorption shortwave;
            if (surfaceFlux.tilted) {
                meteo.shortwave = surfaceFlux.global;
                beamLit = beamLit && surfaceFlux.cosIncidence > 1e-6;
                beamZenith = beamLit ? std::acos(std::clamp(surfaceFlux.cosIncidence, 0.0, 1.0)) * 180.0 / kPi : sun.zenith;
                shortwave = absorbShortwave(solarBins, spectra, leafArea, meteo.shortwave, beamZenith, dayOfYear,
                                            std::clamp(surfaceFlux.diffuseShare, 0.0, 1.0), false, beamLit);
                shortwave.partitionLimited = surfaceFlux.limited;
                run.diffuse = surfaceFlux.horizontalDiffuseFraction;  // horizontal partition for the optical image
            } else {
                // FIX(2026-11-21 C10): resolved. meteo.shortwave now follows the SPA sun (above) and absorbShortwave /
                // shortwaveComponents put the whole flux into the diffuse term when the surface is not sunlit, so the
                // direct part is no longer dropped. (Original note kept for reference:)
                // DEFERRED[低] 物理: meteo.shortwave 由 NOAA 近似 cos(SZA) 决定是否大于 0，而这里的 sunlit 用的是 SPA 天顶角。日出日落附近可能出现 SPA SZA≥90 而短波>0 的情况；enforceSolarLimit=true 时直射上限为 0，会全部转成漫射，没有问题；但 enforceSolarLimit=false 时 absorbShortwave 在 !lit 下令 esun=0，直射部分 (1−fd)·SW 被直接丢掉，能量不守恒；建议：两处用同一套太阳位置，或者 !lit 时把直射全部计入漫射
                shortwave = absorbShortwave(
                    solarBins, spectra, leafArea, meteo.shortwave, sun.zenith, dayOfYear, config.diffuseFraction,
                    config.enforceSolarLimit, sun.zenith < 90.0);
                run.diffuse = shortwave.diffuseFraction;
            }
            run.partitionLimited = shortwave.partitionLimited ? 1.0 : 0.0;
            const double dt = node ? time - nodes[node - 1] : 3600.0;
            if (node + 1 == nodes.size()) {
                run.before = state;
                run.meteo = meteo;
                run.shortwave = shortwave;
                run.sza = beamZenith;
                run.lit = beamLit;
                run.dt = dt;
            }
            run.result = solveEnergyNode(surface, meteo, shortwave, beamZenith, beamLit, dt, state);
        }
        return true;
    };
    // Assimilation: re-solve one pixel at the analysed controls and linearise the overpass energy balance.
    // R(T, x) = 0 at the converged node, so dT/dx = -(dR/dT)^-1 dR/dx; R, dR/dT and dR/dx come from one Dual
    // evaluation of the same component balances (soil moisture: soil emissivity, soil resistance, Johansen inertia,
    // BSM soil albedo of the absorbed shortwave; Vcmax: stomatal resistance; LAI: hom3 radiation and fractions).
    // The soil heat history before the overpass node is held fixed (derivative of the last node only).
    EbSolver ebSolver = [&](size_t pixel, const EbControls& controls, EbOutcome& outcome) -> bool {
        outcome.gradientValid = false;
        const auto* entry = classes.find(type[pixel]);
        if (!entry || entry->surface != SurfaceKind::Vegetation || !validLocation(pixel)) return false;
        const double leafArea = std::clamp(controls.lai, 0.0, 100.0);
        const double moisture = std::clamp(controls.moisture, 0.0, 1.0);
        const PixelForcing forcing = pixelForcing(pixel);
        if (!validForcing(forcing.at(overpass))) return false;
        const int code = static_cast<int>(std::lround(type[pixel]));
        const EnergySurface surface = buildSurface(*entry, leafArea, moisture, controls.vcmax);
        const std::vector<float>& ground = groundBins(*entry, code, moisture);
        const std::vector<float>* rho = nullptr;
        const std::vector<float>* tau = nullptr;
        leafBins(code, rho, tau);
        if ((leafArea > 0 && rho->empty()) || ground.empty()) return false;
        const OpticalSpectra spectra{rho, tau, &ground};
        EnergyState state = initialState(forcing);
        EbRun run;
        if (!marchNodes(pixel, surface, spectra, leafArea, forcing, state, run)) return false;
        const bool leaves = leafArea > 0.0;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        outcome.temperatures = {state.soilSunlit, state.soilShaded, leaves ? state.leafSunlit : nan, leaves ? state.leafShaded : nan};
        outcome.fluxes = {run.result.netRadiation, run.result.sensible, run.result.latent, run.result.ground};
        for (auto& row : outcome.gradient) row.fill(0.0);

        const EnergyInputsT<double> base = energyInputs(surface, run.shortwave, state);
        EnergyInputsT<Ad> x;
        x.soilSunlit = Ad::seed(base.soilSunlit, 0);
        x.soilShaded = Ad::seed(base.soilShaded, 1);
        x.leafSunlit = Ad::seed(base.leafSunlit, 2);
        x.leafShaded = Ad::seed(base.leafShaded, 3);
        x.soilMoisture = Ad::seed(moisture, 4);
        x.vcmax = Ad::seed(base.vcmax, 5);
        x.lai = Ad::seed(leafArea, 6);
        const bool soil = entry->groundKind == GroundKind::Soil;
        x.soilEmissivity = soil ? soilEmissivityAtMoistureT<Ad>(entry->ground.emissivity, config.soilEmissivityWet, x.soilMoisture)
                                : Ad(surface.ground.emissivity);
        if (soil && config.soilThermalFromMoisture) {
            Ad conductivity, capacity;
            johansenSoilThermal<Ad>(surface.ground.saturatedMoisture, x.soilMoisture, conductivity, capacity);
            x.soilInertia = ad::sqrt(conductivity * capacity);
        } else x.soilInertia = Ad(base.soilInertia);
        if (run.meteo.shortwave > 0.0) {
            // BSM soil albedo per solar bin, with its moisture slope from the neighbouring tabulated steps.
            std::vector<Ad> soilSpectrum(ground.size());
            const int step = moistureStep(static_cast<float>(moisture));
            const int lower = std::max(step - 1, 0), upper = std::min(step + 1, kMoistureSteps);
            const bool slope = soil && upper > lower && soilBins[lower].size() == ground.size() && soilBins[upper].size() == ground.size();
            for (size_t bin = 0; bin < ground.size(); ++bin) {
                const double gradient = slope ? (soilBins[upper][bin] - soilBins[lower][bin]) * kMoistureSteps / (upper - lower) : 0.0;
                soilSpectrum[bin] = Ad(ground[bin]) + gradient * (x.soilMoisture - moisture);
            }
            const auto sw = shortwaveComponents<Ad, std::vector<Ad>>(solarBins, *rho, *tau, soilSpectrum, x.lai, run.meteo.shortwave,
                                                                     run.shortwave.diffuseFraction, run.sza, run.lit);
            x.soilDirect = sw.soilDirect;
            x.soilDiffuseSun = sw.soilDiffuseSun;
            x.soilDiffuseShade = sw.soilDiffuseShade;
            x.leafDirect = sw.leafDirect;
            x.leafDiffuseSun = sw.leafDiffuseSun;
            x.leafDiffuseShade = sw.leafDiffuseShade;
            x.parSun = sw.parLeafDirect + sw.parLeafDiffuseSun;
            x.parShade = sw.parLeafDiffuseShade;
            x.soilSunlitFraction = sw.soilSunlitFraction;
            x.laiSunlit = sw.laiSunlit;
        }
        const auto residual = energyResiduals<Ad>(surface, x, run.meteo, run.sza, run.lit, run.dt, run.before,
                                                  state.inverseObukhov, state.sceneSensible);
        const int size = leaves ? 4 : 2;
        double a[4][4]{}, b[4][3]{};
        for (int i = 0; i < size; ++i) {
            for (int j = 0; j < size; ++j) a[i][j] = residual[i].d[j];
            for (int c = 0; c < 3; ++c) b[i][c] = -residual[i].d[4 + c];
        }
        for (int column = 0; column < size; ++column) {
            int pivot = column;
            for (int row = column + 1; row < size; ++row)
                if (std::abs(a[row][column]) > std::abs(a[pivot][column])) pivot = row;
            if (!(std::abs(a[pivot][column]) > 1e-9) || !std::isfinite(a[pivot][column])) return true;  // temperatures valid, derivative rejected
            if (pivot != column) {
                for (int k = 0; k < size; ++k) std::swap(a[column][k], a[pivot][k]);
                for (int k = 0; k < 3; ++k) std::swap(b[column][k], b[pivot][k]);
            }
            for (int row = 0; row < size; ++row) {
                if (row == column) continue;
                const double factor = a[row][column] / a[column][column];
                for (int k = 0; k < size; ++k) a[row][k] -= factor * a[column][k];
                for (int k = 0; k < 3; ++k) b[row][k] -= factor * b[column][k];
            }
        }
        for (int i = 0; i < size; ++i)
            for (int c = 0; c < 3; ++c) {
                const double value = b[i][c] / a[i][i];
                if (!std::isfinite(value)) return true;
                outcome.gradient[i][c] = value;
            }
        outcome.gradientValid = true;
        return true;
    };

    if (eb) {
        progress(10, "能量平衡求解");
        for (auto& band : components) band.assign(pixels, kNaN);
        forcingOut.assign(7, std::vector<float>(pixels, kNaN));
        fluxOut.assign(kFluxBands, std::vector<float>(pixels, kNaN));
        std::atomic<int> rowsDone{0};
        const int rowCount = endRow - startRow;
        #pragma omp parallel for schedule(dynamic, 2) reduction(+:ebPixels, ebConverged, ebStable, ebWaterPixels, ebFailed, ebNoForcing)
        for (int row = startRow; row < endRow; ++row) {
            for (int column = startColumn; column < endColumn; ++column) {
                const size_t pixel = static_cast<size_t>(row) * grid.width + column;
                const auto* entry = classes.find(type[pixel]);
                if (!entry || entry->surface == SurfaceKind::Fill || !validLocation(pixel)) continue;
                const float leafArea = lai[pixel];
                // DEFERRED[中] 错误处理: LAI 上限 100 太宽。MODIS LAI（×0.1）的填充码 249–254（水体、裸地、冰雪、城市等）若未声明为 nodata，经 scale 后变成 24.9–25.4，会被当成有效 LAI 参与 EB 与成像；另外这条检查放在水体分支前面，水体像元的 LAI 一旦是 nodata，就连 skt 也拿不到；建议：把上限收紧到约 10–15，超限时记日志计数；水体判断移到 LAI 检查之前
                if (!std::isfinite(leafArea) || leafArea < 0 || leafArea > 100) continue;
                const PixelForcing forcing = pixelForcing(pixel);
                const Forcing atOverpass = forcing.at(overpass);
                if (!validForcing(atOverpass)) { ++ebNoForcing; continue; }
                // FIX(2026-11-21 C10): the reported horizontal shortwave at the overpass uses the SPA sun, as the solve.
                double overpassShortwave = atOverpass.shortwave;
                if (const Sun overpassSun = solarPosition(overpass, latitude[pixel], longitude[pixel], dem[pixel]); overpassSun.ok)
                    overpassShortwave = forcing.surfaceShortwave(overpass, overpassSun.zenith, dayOfYear, config.enforceSolarLimit,
                                                                 overpassSun.azimuth).horizontal;
                const double values[7] = {atOverpass.ta, atOverpass.ea, atOverpass.p, atOverpass.wind, overpassShortwave, atOverpass.longwave, soilMoisture[pixel]};
                for (int k = 0; k < 7; ++k) forcingOut[k][pixel] = static_cast<float>(values[k]);
                if (entry->surface == SurfaceKind::Water) {
                    const float skin = static_cast<float>(forcing.temperature(waterSkinMissing ? "t2m" : "skt", overpass));
                    for (auto& band : components) band[pixel] = skin;
                    ++ebWaterPixels;
                    continue;
                }
                const int code = static_cast<int>(std::lround(type[pixel]));
                const double moisture = std::isfinite(soilMoisture[pixel]) ? soilMoisture[pixel] : classes.defaultSoilMoisture;
                const EnergySurface surface = buildSurface(*entry, leafArea, moisture, entry->photosynthesis.vcmax);
                const std::vector<float>& ground = groundBins(*entry, code, surface.soilMoisture);
                const std::vector<float>* rho = nullptr;
                const std::vector<float>* tau = nullptr;
                leafBins(code, rho, tau);
                if ((leafArea > 0 && rho->empty()) || ground.empty()) { ++ebFailed; continue; }
                const OpticalSpectra spectra{rho, tau, &ground};
                EnergyState state = initialState(forcing);
                EbRun run;
                const bool solved = marchNodes(pixel, surface, spectra, leafArea, forcing, state, run);
                if (std::isfinite(run.diffuse)) diffuseFractionOut[pixel] = static_cast<float>(run.diffuse);
                if (!solved) { ++ebFailed; continue; }
                const EnergyDiagnostics& result = run.result;
                const double partitionLimited = run.partitionLimited;
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
            std::to_string(ebStable) + " 个），水体取" + (waterSkinMissing ? " 2 m 气温（数据包无 skt）" : " 气象强迫表皮温度") + " " +
            std::to_string(ebWaterPixels) + " 个" + (ebFailed ? "，未能求解 " + std::to_string(ebFailed) + " 个" : "") +
            (ebNoForcing ? "，气象强迫无有效值（如陆面产品的海面）跳过 " + std::to_string(ebNoForcing) + " 个" : ""));
        log("EB 初值：土壤、叶片与土壤热历史全部取起转起点 " + formatUtc(marchStart) + " 的 2 m 气温（不使用 skt/stl1/stl2）");
        if (initialLaterHour > 0 || initialDayMean > 0)
            log("警告：起转起点的 2 m 气温无效，初值改取起点后第一个有效时次 " + std::to_string(initialLaterHour.load()) + " 个像元、改取首个模拟日平均气温 " +
                std::to_string(initialDayMean.load()) + " 个像元");
    } else if (config.thermalEnabled || lsctInversion || assimilate) {
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

    // ---------------- reflected sky ----------------
    // Downwelling longwave of the reflected-sky term of the thermal brightness (2026-11-20). One per-pixel field
    // shared by imaging, inversion and assimilation; it is fixed from the background state (not re-derived from
    // a posterior), so the retrieval and the image of its result use the same operator.
    std::vector<float> skyLongwave;
    bool skyFromObservations = false;
    std::string skySource = "none";
    if ((config.thermalEnabled || config.inversion.enabled() || assimilate) && config.skySource != ImageConfig::SkySource::None) {
        if (config.skySource == ImageConfig::SkySource::Field) {
            skyLongwave = fieldRaster(config.skyLongwave, grid, "下行长波 skyLongwave", 0, true);
            skySource = "configured";
        } else if (eb) {
            skyLongwave = forcingOut[5];
            skySource = "era5-strd";
        } else {
            // RT: Swinbank clear-sky L↓ with the shaded leaf (else shaded / sunlit soil) temperature as air proxy.
            skyLongwave.assign(pixels, kNaN);
            const auto at = [&](int k, size_t p) { return components[k].size() == pixels ? components[k][p] : kNaN; };
            for (size_t p = 0; p < pixels; ++p) {
                float proxy = at(3, p);
                if (!std::isfinite(proxy)) proxy = at(1, p);
                if (!std::isfinite(proxy)) proxy = at(0, p);
                skyLongwave[p] = static_cast<float>(swinbankLongwave(proxy));
            }
            skyFromObservations = true;  // plain LST retrieval without components: proxy = observed brightness
            skySource = "swinbank-shaded-component";
        }
        double sum = 0.0;
        long long count = 0;
        for (auto& value : skyLongwave) {
            if (!(std::isfinite(value) && value > 0.0f)) { value = kNaN; continue; }
            sum += value;
            ++count;
        }
        log("热红外反射天空项：下行长波来源 " + skySource + (count ? "，均值 " + format(sum / count, 1) + " W m⁻²" : "，无有效值"));
    }

    // ---------------- imaging ----------------
    std::vector<std::vector<float>> optical(config.opticalBands.size(), std::vector<float>(config.opticalEnabled ? pixels : 0, kNaN));
    std::vector<std::vector<float>> brightness(config.thermalBands.size(), std::vector<float>(config.thermalEnabled ? pixels : 0, kNaN));
    std::vector<std::vector<float>> surfaceTemperature(config.thermalBands.size(), std::vector<float>(config.thermalEnabled ? pixels : 0, kNaN));
    std::vector<std::vector<float>> solar(2, std::vector<float>(pixels, kNaN));
    // One Hapke directional-emissivity table pair per soil class: dry anchor and saturation anchor.
    std::map<int, MoistureSoilEmissivity> soilEmissivityTables;
    if ((config.thermalEnabled || config.inversion.enabled() || assimilate) && config.hapkeSoilEmissivity) {
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
    // Terrain of the thermal observation operator, shared by the imaging loop, the inversion and the assimilation.
    // FIX(2026-11-21 S-1): built before the inversion (was after it) so the retrieval uses the same terrain operator.
    TerrainFields terrainFields;
    terrainFields.mode = config.terrainRadiation;
    terrainFields.slope = &terrainSlope;
    terrainFields.aspect = &terrainAspect;
    terrainFields.coneSlope = &coneSlope;
    terrainFields.coneDensity = &coneDensity;
    terrainFields.ridgeSlope = &ridgeSlope;
    terrainFields.ridgeAspect = &ridgeAspect;
    terrainFields.ridgeDensity = &ridgeDensity;
    terrainFields.endFraction = config.terrainEndFraction;
    if (config.inversion.enabled()) {
        const InversionInputs inputs{config, classes, grid, type, lai, vza, vaa, latitude, longitude, dem, soilMoisture,
                                     soilEmissivityTables, startRow, endRow, startColumn, endColumn, overpass,
                                     inversionStart, inversionSpan, skyLongwave.empty() ? nullptr : &skyLongwave,
                                     skyFromObservations && config.skySource == ImageConfig::SkySource::Auto,
                                     &terrainFields};  // FIX(2026-11-21 S-1)
        inversionResult = runInversion(inputs, components);
        if (lsctInversion) {
            if (eb) ebComponents = components;
            components = inversionResult.components;
        }
    }

    // ---------------- assimilation ----------------
    // The analysis replaces the component temperatures (and LAI / soil moisture when they are controls), so every
    // simulated product of this run is the forward image of the analysed state; the background is kept separately.
    const double assimilationStart = eb ? 70 : 20, assimilationSpan = assimilate ? (eb ? 15 : 35) : 0;
    AssimilationResult assimilationResult;
    std::vector<float> imagingLai = lai;
    // (Terrain fields of the thermal operator: built above, before the inversion — FIX(2026-11-21 S-1).)
    std::vector<double> thermalWavelengths;
    for (const auto& band : config.thermalBands) thermalWavelengths.push_back(band.center);
    if (assimilate) {
        const AssimilationInputs inputs{config, classes, grid, type, lai, vza, vaa, latitude, longitude, dem, soilMoisture,
                                        eb ? &diffuseFractionOut : nullptr, eb && !forcingOut.empty() ? &forcingOut[4] : nullptr,
                                        soilEmissivityTables, terrainFields, leafBandReflectance, leafBandTransmittance,
                                        soilBandReflectance, startRow, endRow, startColumn, endColumn, overpass, eb,
                                        eb ? ebSolver : EbSolver{}, assimilationStart, assimilationSpan,
                                        skyLongwave.empty() ? nullptr : &skyLongwave};
        assimilationResult = runAssimilation(inputs, components);
        if (eb) ebComponents = components;
        components = assimilationResult.components;
        for (size_t p = 0; p < pixels; ++p) {
            if (!assimilationResult.analysed[p]) continue;
            if (std::isfinite(assimilationResult.lai[p])) imagingLai[p] = assimilationResult.lai[p];
            if (std::isfinite(assimilationResult.soilMoisture[p])) soilMoisture[p] = assimilationResult.soilMoisture[p];
        }
    }

    long long invalidInput = 0, invalidType = 0, waterPixels = 0, nightPixels = 0, opticalPixels = 0, thermalPixels = 0, thermalFailed = 0, solarFailed = 0;
    std::atomic<int> rowsDone{0};
    const int rowCount = endRow - startRow;
    const double imageStart = assimilationSpan > 0 ? assimilationStart + assimilationSpan
                            : inversionSpan > 0 ? inversionStart + inversionSpan : (eb ? 70 : 25);
    const double imageSpan = 90 - imageStart;
    progress(imageStart, "逐像元成像");
    #pragma omp parallel for schedule(dynamic, 4) reduction(+:invalidInput, invalidType, waterPixels, nightPixels, opticalPixels, thermalPixels, thermalFailed, solarFailed)
    for (int row = startRow; row < endRow; ++row) {
        for (int column = startColumn; column < endColumn; ++column) {
            const size_t pixel = static_cast<size_t>(row) * grid.width + column;
            const auto* entry = classes.find(type[pixel]);
            if (!entry) { ++invalidType; continue; }
            const float viewZenith = vza[pixel], viewAzimuth = vaa[pixel];
            const float leafArea = imagingLai[pixel];  // analysed LAI when the assimilation controls it
            if (!validLocation(pixel) || !std::isfinite(viewZenith) || viewZenith < 0 || viewZenith >= 90 || !std::isfinite(viewAzimuth) ||
                viewAzimuth < -360 || viewAzimuth > 360 || !std::isfinite(leafArea) || leafArea < 0 || leafArea > 100) { ++invalidInput; continue; }
            const Sun sun = solarPosition(overpass, latitude[pixel], longitude[pixel], dem[pixel]);
            if (!sun.ok) { ++solarFailed; continue; }
            const double sza = sun.zenith, saa = sun.azimuth;
            solar[0][pixel] = static_cast<float>(sza);
            solar[1][pixel] = static_cast<float>(saa);
            const double view = std::min<double>(viewZenith, 89.9);
            // Terrain-relative thermal geometry is handled by thermalSceneAt (thermal_view.cpp); the optical
            // reflectance keeps the horizontal geometry. (The former duplicate facet block here was unused.)
            // VAA convention (README): azimuth of the direction from the pixel to the sensor, degrees clockwise
            // from north, the same convention as the SPA solar azimuth; VAA = SAA is the hotspot.
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
                            double diffuse = eb && std::isfinite(diffuseFractionOut[pixel])
                                ? diffuseFractionOut[pixel] : config.diffuseFraction;
                            if (!(diffuse >= 0.0 && diffuse <= 1.0))
                                diffuse = requestedDiffuseFraction(
                                    eb && !forcingOut.empty() && std::isfinite(forcingOut[4][pixel]) ? forcingOut[4][pixel] : 0.0,
                                    sza, config.diffuseFraction, dayOfYear);  // FIX(2026-11-21 C7): E0 of the overpass day
                            optical[band][pixel] = static_cast<float>(canopyReflectance(
                                leafArea, entry->canopy.hotspot, sza, saa, view, viewAzimuth, lr, lt, ground,
                                diffuse, sza < 90.0));
                        }
                        ++opticalPixels;
                    } catch (const std::exception&) { for (auto& band : optical) band[pixel] = kNaN; ++invalidInput; }
                } else ++invalidInput;
            }

            if (config.thermalEnabled && entry->surface != SurfaceKind::Fill) {
                // Shared observation operator (app/thermal_view.cpp): the assimilation evaluates exactly this code on
                // dual numbers, so a simulated image and the analysis operator can never drift apart. Facet slope
                // geometry and hill / ridge sub-pixel composites are handled inside thermalSceneAt / thermalPixel.
                ThermalScene scene;
                if (!thermalSceneAt(terrainFields, pixel, sza, saa, viewZenith, viewAzimuth, scene)) { ++thermalFailed; continue; }
                setThermalMaterial(scene, config, *entry, soilEmissivityTables, code);
                scene.skyTemperature = skyLongwave.empty() ? std::numeric_limits<double>::quiet_NaN()
                                                           : skyTemperatureFromLongwave(skyLongwave[pixel]);
                // Physical value: wetter soil emits more, so the radiance synthesis uses the water-content value.
                const double pixelMoisture = std::isfinite(soilMoisture[pixel])
                    ? static_cast<double>(soilMoisture[pixel])
                    : static_cast<double>(classes.defaultSoilMoisture);
                const std::array<double, 4> temperatures{components[0][pixel], components[1][pixel],
                                                         components[2][pixel], components[3][pixel]};
                std::vector<double> bandBrightness, bandSurface;
                if (thermalPixel<double>(scene, temperatures, static_cast<double>(leafArea), pixelMoisture, thermalWavelengths,
                                         bandBrightness, bandSurface)) {
                    for (size_t band = 0; band < config.thermalBands.size(); ++band) {
                        brightness[band][pixel] = static_cast<float>(bandBrightness[band]);
                        surfaceTemperature[band][pixel] = static_cast<float>(bandSurface[band]);
                    }
                    ++thermalPixels;
                } else {
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
    // Reflected sky term of the thermal brightness temperature: source of L↓ ("none" = not applied).
    summary.Add("reflectedSky", skySource);
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
    if (assimilate) {
        for (const auto& name : assimilationResult.products) products.Add(name);
        summary.Add("assimilation", assimilationResult.summary);
        temperatureInfo.Add("posterior", "assimilation");
        summary.Add("runMode", "assimilation");
    } else summary.Add("runMode", "simulation");

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
        // Statistics describe exactly what components.tif holds (the EB solution); a posterior that replaced it
        // for imaging (LSCT / assimilation) is summarised separately (2026-11-20: the two used to be mixed).
        for (size_t index = 0; index < 4; ++index) componentStats.Add(bandStatistics(componentNames[index], solved[index]));
        summary.Add("components", componentStats);
        if (!ebComponents[0].empty()) {
            CPLJSONArray posteriorStats;
            for (size_t index = 0; index < 4; ++index) posteriorStats.Add(bandStatistics(componentNames[index], components[index]));
            summary.Add("posteriorComponents", posteriorStats);
        }
        const std::vector<std::string> forcingNames{"air temperature (K)", "vapour pressure (hPa)", "surface pressure (hPa)", "wind speed (m s-1)",
                                                    "downward shortwave, horizontal (W m-2)", "downward longwave (W m-2)", "soil moisture (m3 m-3)"};
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
        {
            // temperature.forcing: the metadata that was used and what the standardisation layer did (docs/forcing-metadata.md 9).
            CPLJSONObject forcingInfo;
            CPLJSONDocument summary;
            if (summary.LoadMemory(forcingMetadataSummaryJson(pack.metadata()))) forcingInfo = summary.GetRoot();
            const ForcingReport& report = pack.report();
            const auto list = [](const std::vector<std::string>& values) {
                CPLJSONArray array;
                for (const auto& value : values) array.Add(value);
                return array;
            };
            forcingInfo.Add("standardised", pack.standardised());
            forcingInfo.Add("derived", list(report.derived));
            forcingInfo.Add("ignored", list(report.ignored));
            forcingInfo.Add("infos", list(report.infos));
            forcingInfo.Add("warnings", list(report.warnings));
            CPLJSONObject removed;
            for (const auto& item : report.removed) removed.Add(item.first, static_cast<GInt64>(item.second));
            forcingInfo.Add("valuesRemoved", removed);
            temperatureInfo.Add("forcing", forcingInfo);
        }
        CPLJSONArray nodeList;
        for (const double node : nodes) nodeList.Add(formatUtc(node));
        temperatureInfo.Add("nodes", nodeList);
        // FIX(2026-11-21 B10): spin-up actually integrated before the overpass.
        temperatureInfo.Add("spinupNodesRequested", config.spinupHours);
        temperatureInfo.Add("spinupNodesUsed", spinupNodesUsed);
        temperatureInfo.Add("spinupHoursUsed", spinupUsedHours);
        temperatureInfo.Add("marchStart", formatUtc(marchStart));
        temperatureInfo.Add("ebNodeCount", static_cast<GInt64>(nodes.size()));
        temperatureInfo.Add("solvedPixels", static_cast<GInt64>(ebPixels));
        temperatureInfo.Add("convergedPixels", static_cast<GInt64>(ebConverged));
        temperatureInfo.Add("stabilityConvergedPixels", static_cast<GInt64>(ebStable));
        temperatureInfo.Add("stability", "Monin-Obukhov coupled solve: 1/L fixed-point relaxation 0.5, H-only buoyancy; iteration tolerance 1e-4 + 0.02|target| (m-1), final acceptance 2x iteration tolerance");
        temperatureInfo.Add("energyScheme", "tirt");
        temperatureInfo.Add("soilHeatMethod", config.soilHeatMethod == SoilHeatMethod::Depth ? "depth" :
                                               config.soilHeatMethod == SoilHeatMethod::Surface ? "surface" : "direct");
        temperatureInfo.Add("temperatureMethod", config.robustTemperature ? "robust" : "direct");
        temperatureInfo.Add("waterPixels", static_cast<GInt64>(ebWaterPixels));
        temperatureInfo.Add("failedPixels", static_cast<GInt64>(ebFailed));
        temperatureInfo.Add("noForcingPixels", static_cast<GInt64>(ebNoForcing));
        temperatureInfo.Add("moistureFallbackPixels", static_cast<GInt64>(moistureFallback));
        CPLJSONArray notes;
        for (const auto& note : spinupNotes) notes.Add(note);
        for (const auto& note : pack.notes()) notes.Add(note);
        temperatureInfo.Add("notes", notes);
    }
    summary.Add("temperature", temperatureInfo);
    // FIX(2026-11-21 C9): the canopy structure actually simulated, and any crown / row inputs that were not used.
    {
        CPLJSONObject structure;
        structure.Add("model", "homogeneous");
        structure.Add("note", "crown / row structure (physics/effective_lai.cpp) is not connected in this version; every canopy is a homogeneous turbid medium");
        CPLJSONArray ignoredClasses, ignoredKeys;
        for (const auto& [code, keys] : ignoredStructureClasses) {
            CPLJSONObject item;
            item.Add("code", code);
            item.Add("keys", keys);
            ignoredClasses.Add(item);
        }
        for (const auto& key : config.ignoredStructureKeys) ignoredKeys.Add(key);
        structure.Add("ignoredClassParameters", ignoredClasses);
        structure.Add("ignoredConfigKeys", ignoredKeys);
        summary.Add("canopyStructure", structure);
    }
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


