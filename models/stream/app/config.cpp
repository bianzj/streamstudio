#include "config.h"
#include "../forcing/utc.h"

#include <cpl_json.h>

#include <cmath>
#include <stdexcept>

namespace stream {
namespace {

std::vector<SpectralBand> readBands(const CPLJSONObject& section, double low, double high, const std::string& unit, const std::string& label, bool thermal)
{
    std::vector<SpectralBand> bands;
    const auto array = section.GetArray("bands");
    for (int index = 0; index < array.Size(); ++index) {
        const auto item = array[index];
        std::string name = item.GetString("name", "B" + std::to_string(index + 1));
        for (auto& c : name) if (c == ',' || c == '{' || c == '}' || c == '\r' || c == '\n') c = ' ';
        SpectralBand band{name, item.GetDouble("center", NAN), item.GetDouble("fwhm", 0)};
        if (!std::isfinite(band.center) || band.center < low || band.center > high)
            throw std::runtime_error(label + "波段 " + band.name + " 的中心波长须在 " + std::to_string(low) + "–" + std::to_string(high) + " " + unit);
        if (!std::isfinite(band.fwhm) || band.fwhm < 0) throw std::runtime_error(label + "波段 " + band.name + " 的带宽无效");
        // 热红外按波段中心单色计算，半高宽只记录；仍限制在物理可解释范围内。
        if (thermal && band.fwhm > 2 * (band.center - 0.1))
            throw std::runtime_error(label + "波段 " + band.name + " 的带宽不能超过 " + std::to_string(2 * (band.center - 0.1)) + " " + unit);
        bands.push_back(band);
    }
    if (bands.size() > 64) throw std::runtime_error(label + "波段不能超过 64 个");
    return bands;
}

}  // namespace

ImageConfig readImageConfig(const std::string& path)
{
    CPLJSONDocument document;
    if (!document.Load(path)) throw std::runtime_error("无法读取配置 JSON：" + path);
    const auto root = document.GetRoot();
    if (root.GetString("kind", "") != "stream-image-config" || root.GetInteger("schemaVersion", 0) != 2)
        throw std::runtime_error("配置须为 stream-image-config，schemaVersion 2");
    ImageConfig config;
    config.outputDir = pathFromUtf8(root.GetString("outputDir", ""));
    config.dataDir = pathFromUtf8(root.GetString("dataDir", ""));
    if (config.outputDir.empty() || !std::filesystem::is_directory(config.outputDir)) throw std::runtime_error("输出目录不存在");
    if (config.dataDir.empty() || !std::filesystem::is_directory(config.dataDir)) throw std::runtime_error("STREAM 数据目录不存在");
    if (!parseUtc(root.GetString("overpass", ""), config.overpass)) throw std::runtime_error("过境时刻须为 UTC，格式 YYYY-MM-DDTHH:MM:SSZ");

    const auto surface = root.GetObj("surface");
    config.type = readField(surface, "type");
    config.lai = readField(surface, "lai");
    config.soilMoisture = readField(surface, "soilMoisture");
    config.dem = readField(surface, "dem");
    const auto terrain = surface.GetObj("terrain");
    if (terrain.IsValid() && terrain.GetType() == CPLJSONObject::Type::Object) {
        const auto terrainMode = terrain.GetString("mode", "none");
        if (terrainMode == "single") {
            config.terrainRadiation = TerrainRadiation::Single;
            config.terrainSlopeField = readField(terrain, "slope");
            config.terrainAspectField = readField(terrain, "aspect");
            if (!config.terrainSlopeField.present()) throw std::runtime_error("单一坡需要坡度（常数或栅格）");
            if (!config.terrainAspectField.present()) throw std::runtime_error("单一坡需要坡向（常数或栅格）");
            if (!config.terrainSlopeField.fromFile()) {
                config.terrainSlope = config.terrainSlopeField.value;
                if (!(config.terrainSlope >= 0.0 && config.terrainSlope < 90.0))
                    throw std::runtime_error("单一坡坡度须在 0–90 度之间");
            }
            if (!config.terrainAspectField.fromFile()) {
                config.terrainAspect = config.terrainAspectField.value;
                if (!(config.terrainAspect >= 0.0 && config.terrainAspect < 360.0))
                    throw std::runtime_error("单一坡坡向须在 0–360 度之间");
            }
        } else if (terrainMode == "facet" || terrainMode == "composite") {
            if (!config.dem.fromFile()) throw std::runtime_error("面元法需要 DEM 栅格");
            config.terrainRadiation = TerrainRadiation::Facet;
        } else if (terrainMode == "hill") {
            config.terrainRadiation = TerrainRadiation::Hill;
            config.terrainSlopeField = readField(terrain, "slope");
            config.terrainDensityField = readField(terrain, "density");
            if (!config.terrainSlopeField.present()) throw std::runtime_error("山包复合坡需要山包坡度栅格");
            if (!config.terrainDensityField.present()) throw std::runtime_error("山包复合坡需要密度栅格");
        } else if (terrainMode == "ridge") {
            config.terrainRadiation = TerrainRadiation::Ridge;
            config.terrainSlopeField = readField(terrain, "slope");
            config.terrainAspectField = readField(terrain, "aspect");
            config.terrainDensityField = readField(terrain, "density");
            if (!config.terrainSlopeField.present()) throw std::runtime_error("山脊复合坡需要山脊坡度栅格");
            if (!config.terrainAspectField.present()) throw std::runtime_error("山脊复合坡需要山脊走向栅格");
            if (!config.terrainDensityField.present()) throw std::runtime_error("山脊复合坡需要密度栅格");
            config.terrainEndFraction = terrain.GetDouble("endFraction", 0.0);
            if (!(config.terrainEndFraction >= 0.0 && config.terrainEndFraction <= 0.5))
                throw std::runtime_error("山脊端面比例 endFraction 须在 0–0.5 之间");
        } else if (terrainMode != "none") {
            throw std::runtime_error("地形辐射模式须为 none、single 或 composite");
        }
    }
    config.soilBrightness = surface.GetDouble("soilBrightness", -1.0);
    if (!std::isfinite(config.soilBrightness) || config.soilBrightness > 1.0 || (config.soilBrightness > 0.0 && config.soilBrightness < 0.05))
        throw std::runtime_error("地表土壤亮度 soilBrightness 须为 0.05–1.0 或省略（使用 classes.json）");
    config.latitude = readField(surface, "latitude");
    config.longitude = readField(surface, "longitude");
    const auto view = root.GetObj("view");
    config.vza = readField(view, "vza");
    config.vaa = readField(view, "vaa");
    if (!config.type.fromFile()) throw std::runtime_error("缺少地表类型（IGBP）栅格");

    const auto region = root.GetObj("region");
    if (region.IsValid() && region.GetType() == CPLJSONObject::Type::Object) {
        config.region = true;
        config.startRow = region.GetInteger("startRow", 0);
        config.endRow = region.GetInteger("endRow", 0);
        config.startColumn = region.GetInteger("startColumn", 0);
        config.endColumn = region.GetInteger("endColumn", 0);
    }

    const auto optical = root.GetObj("optical");
    const auto thermal = root.GetObj("thermal");
    config.opticalEnabled = optical.GetBool("enabled", false);
    config.thermalEnabled = thermal.GetBool("enabled", false);
    if (config.opticalEnabled) config.opticalBands = readBands(optical, 400, 2400, "nm", "光学", false);
    if (config.thermalEnabled) config.thermalBands = readBands(thermal, 3, 20, "μm", "热红外", true);
    if (config.opticalEnabled && config.opticalBands.empty()) throw std::runtime_error("已启用光学模拟但没有波段");
    if (config.thermalEnabled && config.thermalBands.empty()) throw std::runtime_error("已启用热红外模拟但没有波段");

    const auto inversion = root.GetObj("inversion");
    if (inversion.IsValid() && inversion.GetType() == CPLJSONObject::Type::Object) {
        const auto target = inversion.GetString("target", "none");
        if (target == "lst") config.inversion.target = InversionTarget::Lst;
        else if (target == "lsct") config.inversion.target = InversionTarget::Lsct;
        else if (target != "none") throw std::runtime_error("inversion.target 须为 lst、lsct 或 none");
    }
    if (config.inversion.enabled()) {
        auto& inv = config.inversion;
        const auto list = inversion.GetArray("observations");
        if (list.Size() < 1) throw std::runtime_error("反演至少需要一个热红外观测");
        if (list.Size() > 32) throw std::runtime_error("反演观测不能超过 32 个");
        for (int index = 0; index < list.Size(); ++index) {
            const auto item = list[index];
            InversionObservation observation;
            observation.name = item.GetString("name", "obs" + std::to_string(index + 1));
            const auto kind = item.GetString("kind", "brightness");
            if (kind == "lst") observation.kind = ObservationKind::Lst;
            else if (kind != "brightness") throw std::runtime_error("观测 " + observation.name + " 的类型须为 brightness 或 lst");
            observation.value = readField(item, "value");
            if (!observation.value.present()) throw std::runtime_error("观测 " + observation.name + " 缺少亮温栅格或常数");
            observation.wavelength = item.GetDouble("wavelength", NAN);
            if (!(observation.wavelength >= 3 && observation.wavelength <= 20))
                throw std::runtime_error("观测 " + observation.name + " 的波长须在 3–20 μm");
            observation.sigma = item.GetDouble("sigma", 0.5);
            if (!(observation.sigma > 0 && observation.sigma <= 10))
                throw std::runtime_error("观测 " + observation.name + " 的误差 σ 须在 0–10 K");
            observation.vza = readField(item, "vza");
            observation.vaa = readField(item, "vaa");
            inv.observations.push_back(observation);
        }
        inv.mask = readField(inversion, "mask");
        const auto prior = inversion.GetObj("prior");
        if (prior.IsValid() && prior.GetType() == CPLJSONObject::Type::Object) {
            inv.priorSd = prior.GetDouble("sd", 5.0);
            inv.priorCorrelation = prior.GetDouble("correlation", 0.0);
        }
        inv.emissivitySd = inversion.GetDouble("emissivitySd", 0.01);
        inv.window = inversion.GetInteger("window", 1);
        inv.maxIterations = inversion.GetInteger("maxIterations", 20);
        if (!(inv.priorSd >= 0.1 && inv.priorSd <= 100)) throw std::runtime_error("背景场误差 prior.sd 须在 0.1–100 K");
        if (!(inv.priorCorrelation >= 0 && inv.priorCorrelation <= 0.95)) throw std::runtime_error("背景场相关系数须在 0–0.95");
        if (!(inv.emissivitySd >= 0 && inv.emissivitySd <= 0.1)) throw std::runtime_error("发射率误差须在 0–0.1");
        if (inv.window < 1 || inv.window > 9 || inv.window % 2 == 0) throw std::runtime_error("反演窗口须为 1–9 的奇数");
        if (inv.maxIterations < 1 || inv.maxIterations > 100) throw std::runtime_error("反演迭代次数须在 1–100");
    }
    if (!config.opticalEnabled && !config.thermalEnabled && !config.inversion.enabled())
        throw std::runtime_error("请至少启用光学、热红外模拟或反演");

    const auto temperature = root.GetObj("temperature");
    const auto mode = temperature.GetString("mode", "rt");
    if (mode == "eb") {
        config.mode = TemperatureMode::Eb;
        config.era5Pack = pathFromUtf8(temperature.GetObj("era5").GetString("pack", ""));
        if (config.era5Pack.empty()) throw std::runtime_error("EB 模式需要 ERA5 年度包");
        const auto source = temperature.GetObj("soilMoisture").GetString("source", "era5");
        if (source != "era5" && source != "user") throw std::runtime_error("土壤水分来源须为 era5 或 user");
        config.soilMoistureFromEra5 = source == "era5";
        config.elevationCorrection = temperature.GetBool("elevationCorrection", true);
        const auto energy = temperature.GetObj("energy");
        const auto scheme = energy.GetString("scheme", "tirt");
        if (scheme != "tirt" && scheme != "tirt-analytic") throw std::runtime_error("能量平衡方案须为 tirt");
        const auto heat = energy.GetString("soilHeatMethod", "surface");
        if (heat == "surface") config.soilHeatMethod = SoilHeatMethod::Surface;
        else if (heat == "depth") config.soilHeatMethod = SoilHeatMethod::Depth;
        else if (heat == "direct") config.soilHeatMethod = SoilHeatMethod::Direct;
        else throw std::runtime_error("soilHeatMethod 须为 surface、depth 或 direct");
        const auto temperatureMethod = energy.GetString("temperatureMethod", "robust");
        if (temperatureMethod != "robust" && temperatureMethod != "direct")
            throw std::runtime_error("temperatureMethod 须为 robust 或 direct");
        config.robustTemperature = temperatureMethod == "robust";
        config.diffuseFraction = energy.GetDouble("diffuseFraction", 0.2);
        if (!std::isfinite(config.diffuseFraction) || config.diffuseFraction < -1.0 || config.diffuseFraction > 1.0)
            throw std::runtime_error("diffuseFraction 须在 -1 与 1 之间（-1 为 Erbs）");
        config.enforceSolarLimit = energy.GetBool("enforceSolarLimit", true);
        const auto resistance = energy.GetObj("soilResistance");
        if (resistance.IsValid() && resistance.GetType() == CPLJSONObject::Type::Object) {
            const auto method = resistance.GetString("method", "constant");
            if (method != "constant" && method != "sellers") throw std::runtime_error("soilResistance.method 须为 constant 或 sellers");
            config.soilResistanceFromMoisture = method == "sellers";
            config.soilResistance = resistance.GetDouble("value", 200.0);
        }
        config.stomatalG0 = energy.GetDouble("stomatalG0", 0.01);
        config.minWindSpeed = energy.GetDouble("minWindSpeed", 0.05);
        config.maxAerodynamicResistance = energy.GetDouble("maxAerodynamicResistance", 400.0);
        config.referenceHeight = energy.GetDouble("referenceHeight", 15.0);
        config.heatRoughnessKb = energy.GetDouble("heatRoughnessKb", -1.0);
        if (config.heatRoughnessKb > 20) throw std::runtime_error("heatRoughnessKb 须在 0–20 之间（<=0 表示按地类）");;
        config.soilEmissivity = energy.GetDouble("soilEmissivity", 0.955);
        // Negative selects the built-in dry-to-wet gain; otherwise it is the
        // emissivity at saturation and must be a valid value above the dry one.
        config.soilEmissivityWet = energy.GetDouble("soilEmissivityWet", -1.0);
        config.leafEmissivity = energy.GetDouble("leafEmissivity", 0.985);
        const auto emissivityModel = energy.GetString("soilEmissivityModel", "hapke");
        if (emissivityModel != "hapke" && emissivityModel != "constant")
            throw std::runtime_error("soilEmissivityModel 须为 hapke 或 constant");
        config.hapkeSoilEmissivity = emissivityModel == "hapke";
        config.hapkeSoilK = energy.GetDouble("soilEmissivityK", 1.0);
        if (!(config.hapkeSoilK > 0)) throw std::runtime_error("soilEmissivityK 须为正数");
        const auto normalisation = energy.GetString("surfaceTemperatureNormalisation", "retrieval");
        if (normalisation != "retrieval" && normalisation != "directional")
            throw std::runtime_error("surfaceTemperatureNormalisation 须为 retrieval 或 directional");
        config.referenceSurfaceTemperature = normalisation == "retrieval";
        config.soilConductivity = energy.GetDouble("soilConductivity", 1.55);
        config.soilVolumetricCapacity = energy.GetDouble("soilVolumetricCapacity", 1.298e6);
        const auto soilThermal = energy.GetString("soilThermal", "constant");
        if (soilThermal != "constant" && soilThermal != "moisture") throw std::runtime_error("soilThermal 须为 constant 或 moisture");
        config.soilThermalFromMoisture = soilThermal == "moisture";
        config.soilHeatFraction = energy.GetDouble("soilHeatFraction", 0.25);
        config.solarBinWidth = energy.GetInteger("spectralBinNm", 50);
        if (config.solarBinWidth < 1 || config.solarBinWidth > 100) throw std::runtime_error("spectralBinNm 须在 1–100 nm 之间");
        config.maxIterations = energy.GetInteger("maxIterations", 50);
        config.maxStabilityIterations = energy.GetInteger("maxStabilityIterations", 60);
        if (!(config.stomatalG0 > 0) || !(config.minWindSpeed >= 0) || !(config.referenceHeight > 0) ||
            !(config.soilEmissivity > 0 && config.soilEmissivity <= 1) ||
            (config.soilEmissivityWet >= 0 && !(config.soilEmissivityWet > 0 && config.soilEmissivityWet <= 1)) ||
            !(config.leafEmissivity > 0 && config.leafEmissivity <= 1) ||
            !(config.soilConductivity > 0) || !(config.soilVolumetricCapacity > 0) || config.maxIterations < 1 || config.maxStabilityIterations < 1)
            throw std::runtime_error("能量平衡参数超出有效范围");
        // Wet soil cannot emit less than dry soil.
        if (config.soilEmissivityWet >= 0 && config.soilEmissivityWet < config.soilEmissivity)
            throw std::runtime_error("soilEmissivityWet 须不小于 soilEmissivity（饱和土壤发射率不能低于干土）");
    } else if (mode == "rt") {
        const auto components = temperature.GetObj("components");
        config.componentsFile = components.GetString("file", "");
        const char* keys[] = {"soilSunlit", "soilShaded", "leafSunlit", "leafShaded"};
        for (int index = 0; index < 4; ++index) config.components[index] = readField(components, keys[index]);
    } else {
        throw std::runtime_error("温度模式须为 rt 或 eb");
    }
    config.threads = root.GetInteger("threads", 0);
    return config;
}

}  // namespace stream


