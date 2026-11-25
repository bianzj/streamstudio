// ==========================================================================
// 文件：config.cpp
// 功能：解析 STREAM 运行配置 JSON（kind = stream-image-config，schemaVersion = 2），
//   校验并填充 ImageConfig；任何不合法的配置都以 std::runtime_error 报告（中文提示）。
// 主要模块/步骤：
//   1. 匿名辅助函数：numberOr / integerOr（严格数值读取，类型错误时报错而非静默为 0）、
//      readBands（读取光学/热红外波段并校验范围）
//   2. readImageConfig 的解析顺序：
//      基本路径与过境时刻 → surface（类型/LAI/土壤湿度/DEM/地形/土壤亮度/经纬度）→ view（vza/vaa）
//      → region → optical/thermal 波段 → inversion → assimilation → temperature（RT 组分 / EB）
//      → energy / 发射率 / 土壤热参数 / 反射天空 → 其他选项（线程、预热）
// 数据流：配置 JSON 文件 → readImageConfig → ImageConfig（见 config.h）。
// 约定：光学波段中心波长单位 nm（400–2400）；热红外波段单位 μm（3–20）；角度为度；
//   overpass 为 UTC 秒（parseUtc）。许多缺省值用于兼容旧工程，见各处 FIX 注释。
// ==========================================================================

#include "config.h"
#include "../forcing/forcing_metadata.h"
#include "../forcing/utc.h"

#include <cpl_json.h>

#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <set>

namespace {

// 模块：严格读取数值字段的辅助函数（numberOr 读实数，integerOr 读整数）。
// 缺省：键不存在或为 null 时返回 fallback；类型不是数值/不是整数时抛异常，
// path 为用于报错的配置路径。
// FIX(2026-11-24 C2/C3): GDAL's CPLJSONObject::GetDouble/GetInteger turn a string, bool or object into 0 without a word
// (diffuseFraction "abc" became 0, minWindSpeed "x" became 0). Numeric configuration fields must be JSON numbers.
double numberOr(const CPLJSONObject& parent, const std::string& key, double fallback, const std::string& path)
{
    const auto value = parent.GetObj(key);
    if (!value.IsValid() || value.GetType() == CPLJSONObject::Type::Null) return fallback;
    const auto type = value.GetType();
    if (type != CPLJSONObject::Type::Integer && type != CPLJSONObject::Type::Long && type != CPLJSONObject::Type::Double)
        throw std::runtime_error(path + " 须为数值（JSON 数字），当前为 " + value.ToString());
    return value.ToDouble();
}

int integerOr(const CPLJSONObject& parent, const std::string& key, int fallback, const std::string& path)
{
    const double value = numberOr(parent, key, fallback, path);
    if (!std::isfinite(value) || value != std::floor(value) || std::abs(value) > 1.0e9)
        throw std::runtime_error(path + " 须为整数，当前为 " + std::to_string(value));
    return static_cast<int>(value);
}

}  // namespace

namespace stream {
namespace {

// 作用：读取 section.bands 数组为 SpectralBand 列表并校验。
// 输入：中心波长允许范围 [low, high]、单位文字 unit（仅用于报错）、label（光学/热红外）、
//       thermal（热红外附加带宽检查）；最多 64 个波段。
// 说明：波段名中的逗号、花括号、换行替换为空格，避免破坏下游 CSV/JSON 输出。
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
        // 热红外带宽上限 2·(center − 0.1)（单位同波长），保证带宽下沿仍为正波长（待核对）。
        // 热红外按波段中心单色计算，半高宽只记录；仍限制在物理可解释范围内。
        if (thermal && band.fwhm > 2 * (band.center - 0.1))
            throw std::runtime_error(label + "波段 " + band.name + " 的带宽不能超过 " + std::to_string(2 * (band.center - 0.1)) + " " + unit);
        bands.push_back(band);
    }
    if (bands.size() > 64) throw std::runtime_error(label + "波段不能超过 64 个");
    return bands;
}

}  // namespace

// 作用：读取并校验运行配置，返回 ImageConfig。
// 输入：配置 JSON 路径 path；输出：填充好的 ImageConfig；失败抛 std::runtime_error。
// 说明：配置按节解析，各节的缺省值保持与旧工程一致，见下面各步骤。
ImageConfig readImageConfig(const std::string& path)
{
    // 步骤 1：加载 JSON，检查 kind / schemaVersion，读取输出与数据目录（必须已存在）和过境时刻（UTC）。
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

    // 步骤 2：读取 surface：类型（IGBP）、LAI、土壤湿度、DEM 等栅格场（readField 支持常数或文件）。
    const auto surface = root.GetObj("surface");
    config.type = readField(surface, "type");
    config.lai = readField(surface, "lai");
    config.soilMoisture = readField(surface, "soilMoisture");
    config.dem = readField(surface, "dem");
    // 步骤 2a：地形辐射模式 none / single / facet(composite) / hill / ridge；
    //         各模式检查必须提供的坡度、坡向、密度、DEM 栅格及取值范围（坡度 0–90°，
    //         坡向 0–360°，山脊端面比例 0–0.5）。
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
            throw std::runtime_error("地形辐射模式须为 none、single、facet、composite、hill 或 ridge");
        }
    }
    // 步骤 2b：BSM 土壤亮度；负值（缺省 -1）表示使用 classes.json；给定则须在 0.05–1.0。
    config.soilBrightness = surface.GetDouble("soilBrightness", -1.0);
    if (!std::isfinite(config.soilBrightness) || config.soilBrightness > 1.0 || (config.soilBrightness > 0.0 && config.soilBrightness < 0.05))
        throw std::runtime_error("地表土壤亮度 soilBrightness 须为 0.05–1.0 或省略（使用 classes.json）");
    config.latitude = readField(surface, "latitude");
    config.longitude = readField(surface, "longitude");
    // 步骤 2c：记录不被模拟的冠层/行结构键（见 config.h ignoredStructureKeys），之后由主程序警告。
    // FIX(2026-11-21 C9): structure inputs that this engine does not simulate are reported instead of ignored silently.
    for (const char* key : {"treeStand", "canopyHeight", "crownShape", "crown", "rows", "cropRows", "canopyStructure"})
        if (surface.GetObj(key).IsValid()) config.ignoredStructureKeys.push_back(std::string("surface.") + key);
    // 步骤 3：观测几何 vza / vaa（常数或栅格）；地表类型必须来自栅格文件。
    const auto view = root.GetObj("view");
    config.vza = readField(view, "vza");
    config.vaa = readField(view, "vaa");
    if (!config.type.fromFile()) throw std::runtime_error("缺少地表类型（IGBP）栅格");

    // 步骤 4：可选的行列子区域（起止行列，缺省为 0；是否包含端点由使用方决定，待核对）。
    const auto region = root.GetObj("region");
    if (region.IsValid() && region.GetType() == CPLJSONObject::Type::Object) {
        config.region = true;
        config.startRow = region.GetInteger("startRow", 0);
        config.endRow = region.GetInteger("endRow", 0);
        config.startColumn = region.GetInteger("startColumn", 0);
        config.endColumn = region.GetInteger("endColumn", 0);
    }

    // 步骤 5：光学（中心波长 400–2400 nm）与热红外（3–20 μm）波段；启用但无波段时报错。
    const auto optical = root.GetObj("optical");
    const auto thermal = root.GetObj("thermal");
    config.opticalEnabled = optical.GetBool("enabled", false);
    config.thermalEnabled = thermal.GetBool("enabled", false);
    if (config.opticalEnabled) config.opticalBands = readBands(optical, 400, 2400, "nm", "光学", false);
    if (config.thermalEnabled) config.thermalBands = readBands(thermal, 3, 20, "μm", "热红外", true);
    if (config.opticalEnabled && config.opticalBands.empty()) throw std::runtime_error("已启用光学模拟但没有波段");
    if (config.thermalEnabled && config.thermalBands.empty()) throw std::runtime_error("已启用热红外模拟但没有波段");

    // 步骤 6：反演设置。target = none / lst / lsct；启用时读取 1–32 个热红外观测
    //         （波长 3–20 μm，σ 0–10 K）、掩膜、先验 sd（0.1–100 K）与相关系数（0–0.95）、
    //         发射率误差（0–0.1）、窗口（1–9 的奇数）、最大迭代（1–100）。
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
    // 步骤 7：同化设置。observation = brightness / lst / components / none；
    //         启用时必须给 data；不确定度（uncertainty）与协方差（covariance）二选一；
    //         各 sigma 须为有限正数；lstWavelengthUm 须在 [3, 25] μm；
    //         rejectQaBits 须为 uint16 位掩码；观测层级只支持 canopy（亮温）/ surface（其他），
    //         不支持 TOA；sourceIds / priorSourceIds 为来源标识字符串数组。
    const auto assimilation = root.GetObj("assimilation");
    if (assimilation.IsValid() && assimilation.GetType() == CPLJSONObject::Type::Object) {
        auto& as = config.assimilation;
        const auto observation = assimilation.GetString("observation", "none");
        if (observation == "brightness") as.observation = AssimilationObservation::Brightness;
        else if (observation == "lst") as.observation = AssimilationObservation::Lst;
        else if (observation == "components") as.observation = AssimilationObservation::Components;
        else if (observation != "none") throw std::runtime_error("assimilation.observation 须为 brightness、lst、components 或 none");
        if (as.enabled()) {
            as.data = assimilation.GetString("data", "");
            if (as.data.empty()) throw std::runtime_error("同化需要观测数据文件");
            as.optical = assimilation.GetString("optical", "");
            as.uncertainty = assimilation.GetString("uncertainty", "");
            as.covariance = assimilation.GetString("covariance", "");
            if (!as.uncertainty.empty() && !as.covariance.empty())
                throw std::runtime_error("supply assimilation uncertainty or covariance, not both");
            as.qa = assimilation.GetString("qa", "");
            as.opticalUncertainty = assimilation.GetString("opticalUncertainty", "");
            as.opticalQa = assimilation.GetString("opticalQa", "");
            // 局部函数：读取必须为有限正数的 assimilation 字段，类型不是数值或值非正则抛异常。
            const auto positive = [&](const char* key, double fallback) {
                const auto field = assimilation.GetObj(key);
                if (field.IsValid() && field.GetType() != CPLJSONObject::Type::Integer &&
                    field.GetType() != CPLJSONObject::Type::Long && field.GetType() != CPLJSONObject::Type::Double)
                    throw std::runtime_error(std::string(key) + " must be numeric");
                const double value = assimilation.GetDouble(key, fallback);
                if (!std::isfinite(value) || value <= 0)
                    throw std::runtime_error(std::string("assimilation.") + key + " must be finite and positive");
                return value;
            };
            as.sigmaBrightnessK = positive("sigmaBrightnessK", as.sigmaBrightnessK);
            as.sigmaLstK = positive("sigmaLstK", as.sigmaLstK);
            as.sigmaComponentK = positive("sigmaComponentK", as.sigmaComponentK);
            as.sigmaReflectance = positive("sigmaReflectance", as.sigmaReflectance);
            as.lstWavelengthUm = positive("lstWavelengthUm", as.lstWavelengthUm);
            if (as.lstWavelengthUm < 3.0 || as.lstWavelengthUm > 25.0)
                throw std::runtime_error("assimilation.lstWavelengthUm must be in [3, 25] um");
            const double rejectBits = assimilation.GetDouble("rejectQaBits", as.rejectQaBits);
            if (!std::isfinite(rejectBits) || rejectBits < 0 || rejectBits > 65535 || std::floor(rejectBits) != rejectBits)
                throw std::runtime_error("assimilation.rejectQaBits must be a uint16 bit mask");
            as.rejectQaBits = static_cast<unsigned>(rejectBits);
            const std::string expectedLevel = as.observation == AssimilationObservation::Brightness ? "canopy" : "surface";
            as.observationLevel = assimilation.GetString("observationLevel", expectedLevel);
            if (as.observationLevel != expectedLevel)
                throw std::runtime_error("assimilation.observationLevel must be " + expectedLevel + "; TOA is unsupported");
            const auto sources = [&](const char* key) {
                std::vector<std::string> result;
                const auto field = assimilation.GetObj(key);
                if (field.IsValid() && field.GetType() != CPLJSONObject::Type::Array)
                    throw std::runtime_error(std::string(key) + " must be an array of source IDs");
                const auto array = assimilation.GetArray(key);
                for (int i = 0; i < array.Size(); ++i) {
                    if (array[i].GetType() != CPLJSONObject::Type::String || array[i].ToString().empty())
                        throw std::runtime_error(std::string(key) + " must contain nonempty source IDs");
                    result.push_back(array[i].ToString());
                }
                return result;
            };
            as.sourceIds = sources("sourceIds");
            as.priorSourceIds = sources("priorSourceIds");
            for (const auto& id : as.sourceIds)
                if (std::find(as.priorSourceIds.begin(), as.priorSourceIds.end(), id) != as.priorSourceIds.end())
                    throw std::runtime_error("assimilation source IDs overlap the prior source IDs");
            // 步骤 7a：附加观测视角（最多 16 个，仅用于亮温同化）；每个视角须给出亮温文件与 VZA/VAA，
            //         且同一文件不得在多个视角中重复计入（以规范化路径和 equivalent 判重）。
            const auto views = assimilation.GetArray("views");
            std::vector<std::filesystem::path> observationFiles{std::filesystem::absolute(pathFromUtf8(as.data)).lexically_normal()};
            if (views.IsValid()) {
                if (views.Size() > 16) throw std::runtime_error("同化的附加观测视角不能超过 16 个");
                if (views.Size() > 0 && as.observation != AssimilationObservation::Brightness)
                    throw std::runtime_error("附加观测视角只用于同化亮温");
                for (int index = 0; index < views.Size(); ++index) {
                    const auto item = views[index];
                    AssimilationView view;
                    view.data = item.GetString("data", "");
                    if (!view.data.empty()) {
                        const auto path = std::filesystem::absolute(pathFromUtf8(view.data)).lexically_normal();
                        for (const auto& previous : observationFiles) {
                            std::error_code error;
                            if (path == previous || std::filesystem::equivalent(path, previous, error))
                                throw std::runtime_error("a brightness file cannot be counted in multiple views");
                        }
                        observationFiles.push_back(path);
                    }
                    view.uncertainty = item.GetString("uncertainty", "");
                    view.qa = item.GetString("qa", "");
                    view.vza = readField(item, "vza");
                    view.vaa = readField(item, "vaa");
                    if (view.data.empty() || !view.vza.present() || !view.vaa.present())
                        throw std::runtime_error("附加观测视角 " + std::to_string(index + 1) + " 需要亮温文件与 VZA/VAA");
                    as.views.push_back(view);
                }
            }
            // 步骤 7b：一致性检查：亮温同化需热红外波段；光学观测需光学波段；同化与反演互斥。
            if (as.observation == AssimilationObservation::Brightness && !config.thermalEnabled)
                throw std::runtime_error("同化亮温需要启用热红外波段（观测文件每个波段对应一个热红外波段）");
            if (!as.optical.empty() && !config.opticalEnabled)
                throw std::runtime_error("光学反射率观测需要启用光学波段（文件每个波段对应一个光学波段）");
            if (config.inversion.enabled()) throw std::runtime_error("同化与单独的温度反演（inversion）不能同时使用");
        }
    }
    // 至少要有一项任务：光学模拟、热红外模拟、反演或同化。
    if (!config.opticalEnabled && !config.thermalEnabled && !config.inversion.enabled() && !config.assimilation.enabled())
        throw std::runtime_error("请至少启用光学、热红外模拟、反演或同化");

    // 步骤 8：温度来源 temperature.mode：eb（能量平衡，读气象强迫与 energy 参数）
    //         或 rt（直接给定四个组分温度）。
    const auto temperature = root.GetObj("temperature");
    const auto mode = temperature.GetString("mode", "rt");
    if (mode == "eb") {
        config.mode = TemperatureMode::Eb;
        // 步骤 8a：EB 气象强迫：网格形态需要数据包（pack 或旧的 era5.pack），站点形态需要站点 CSV 或
        //         元数据中登记的站点文件；元数据可为预设名、文件路径或内联 JSON 对象，
        //         提前解析以便在读栅格之前就报错。
        // Weather forcing (docs/forcing-metadata.md 8): temperature.forcing {pack, files, metadata} or the older era5.pack;
        // temperature.forcingMetadata is shorthand for forcing.metadata.
        const auto forcing = temperature.GetObj("forcing");
        std::string pack = forcing.GetString("pack", "");
        if (pack.empty()) pack = temperature.GetObj("era5").GetString("pack", "");
        config.era5Pack = pathFromUtf8(pack);
        config.forcingBaseDir = std::filesystem::absolute(pathFromUtf8(path)).parent_path();
        auto metadata = forcing.GetObj("metadata");
        if (!metadata.IsValid()) metadata = temperature.GetObj("forcingMetadata");
        if (metadata.IsValid()) {
            if (metadata.GetType() == CPLJSONObject::Type::String) config.forcingMetadata = metadata.ToString();
            else if (metadata.GetType() == CPLJSONObject::Type::Object) config.forcingMetadata = metadata.Format(CPLJSONObject::PrettyFormat::Plain);
            else throw std::runtime_error("temperature.forcing.metadata 须为预设名、元数据文件路径或元数据对象");
        }
        const auto files = forcing.GetArray("files");
        for (int index = 0; index < files.Size(); ++index) config.forcingFiles.push_back(pathFromUtf8(files[index].ToString()));
        bool stationForm = false;
        if (!config.forcingMetadata.empty()) {
            // Validated here so a wrong document fails before any raster is read; the data side is checked when the data open.
            const ForcingMetadata resolved = resolveForcingMetadata(config.forcingMetadata, config.forcingBaseDir);
            stationForm = resolved.station();
            if (stationForm && config.forcingFiles.empty() && resolved.stations.empty())
                throw std::runtime_error("气象强迫元数据 \"" + resolved.name + "\" 是站点形态，需要 temperature.forcing.files（站点 CSV）或元数据里登记的站点文件");
        }
        if (!stationForm && config.era5Pack.empty())
            throw std::runtime_error(config.forcingMetadata.empty() ? "EB 模式需要气象数据包" : "网格形态的气象强迫需要数据包（temperature.forcing.pack 或 temperature.era5.pack）");
        // 土壤水分来源：era5（强迫包提供）或 user（用户给定的 surface.soilMoisture）；
        // elevationCorrection 缺省开启（按 DEM 做高程订正，具体订正见使用方，待核对）。
        const auto source = temperature.GetObj("soilMoisture").GetString("source", "era5");
        if (source != "era5" && source != "user") throw std::runtime_error("土壤水分来源须为 era5 或 user");
        config.soilMoistureFromEra5 = source == "era5";
        config.elevationCorrection = temperature.GetBool("elevationCorrection", true);
        // 步骤 8b：EB 预热（spin-up）时次数 spinupHours：缺省 7，范围 0–240；
        //         非整数向上取整（减 1e-9 以免浮点误差多进一位）。
        // FIX(2026-11-21 B10, 2026-11-24): spin-up length = number of ERA5 hourly nodes before the overpass. Default 7:
        // the 2 m air temperature already constrains the result strongly, and 7 matches the 7 h surface heat history
        // TIRTEB keeps (so the history is full at the overpass). Counted in nodes relative to the overpass, never from a
        // fixed clock time (2026-11-21 used 24 h; the first version used 00 UTC of the overpass day).
        // A non-integer value is rounded up to whole nodes.
        const auto spinup = temperature.GetObj("spinupHours");
        if (spinup.IsValid() && spinup.GetType() != CPLJSONObject::Type::Integer &&
            spinup.GetType() != CPLJSONObject::Type::Long && spinup.GetType() != CPLJSONObject::Type::Double)
            throw std::runtime_error("temperature.spinupHours 须为数值（起转时次个数，缺省 7）");
        const double spinupValue = temperature.GetDouble("spinupHours", 7.0);
        if (!std::isfinite(spinupValue) || spinupValue < 0.0 || spinupValue > 240.0)
            throw std::runtime_error("temperature.spinupHours 须在 0–240 之间（起转时次个数，缺省 7）");
        config.spinupHours = static_cast<int>(std::ceil(spinupValue - 1e-9));
        // 步骤 8c：energy 子节：方案固定为 tirt；土壤热通量方法 surface/depth/direct；
        //         温度求解 robust/direct；漫射比例 diffuseFraction（-1 = Erbs 估算，否则 0–1）；
        //         土壤阻抗（constant 或 sellers 依湿度）；各物理参数缺省值对齐 TIRTEB
        //         （input_tirteb.csv），并做范围检查。
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
        config.diffuseFraction = numberOr(energy, "diffuseFraction", 0.2, "temperature.energy.diffuseFraction");
        if (!std::isfinite(config.diffuseFraction) ||
            !(config.diffuseFraction == -1.0 || (config.diffuseFraction >= 0.0 && config.diffuseFraction <= 1.0)))
            throw std::runtime_error("diffuseFraction 须为 -1（Erbs）或 0 到 1 之间的数");
        config.enforceSolarLimit = energy.GetBool("enforceSolarLimit", true);
        const auto resistance = energy.GetObj("soilResistance");
        if (resistance.IsValid() && resistance.GetType() == CPLJSONObject::Type::Object) {
            const auto method = resistance.GetString("method", "constant");
            if (method != "constant" && method != "sellers") throw std::runtime_error("soilResistance.method 须为 constant 或 sellers");
            config.soilResistanceFromMoisture = method == "sellers";
            // FIX(2026-11-21 B-S1): the value replaces fixedResistance of every soil class (image.cpp buildSurface),
            // so it gets the class-table rule (> 0, classes.cpp) plus a type check and a physical upper bound; 0 or a
            // negative value used to reach the latent-heat resistance unchecked.
            const auto valueField = resistance.GetObj("value");
            if (valueField.IsValid() && valueField.GetType() != CPLJSONObject::Type::Integer &&
                valueField.GetType() != CPLJSONObject::Type::Long && valueField.GetType() != CPLJSONObject::Type::Double)
                throw std::runtime_error("soilResistance.value 须为数值（s m⁻¹）");
            config.soilResistance = numberOr(resistance, "value", 200.0, "soilResistance.value");
            if (!std::isfinite(config.soilResistance) || !(config.soilResistance > 0.0) || config.soilResistance > 1.0e5)
                throw std::runtime_error("soilResistance.value 须在 (0, 100000] s m⁻¹ 之间（土壤表面阻抗，TIRTEB 默认 200），当前为 " +
                                         std::to_string(config.soilResistance));
        }
        // 以下参数缺省值：stomatalG0 0.01、minWindSpeed 0.05 m s-1、maxAerodynamicResistance 400 s m-1
        // （<=0 表示不限幅）、referenceHeight 15 m、heatRoughnessKb -1（<=0 按地类）、
        // 发射率 土壤 0.955 / 叶 0.985、土壤导热率 1.55、体积热容 1.298e6、soilHeatFraction 0.25、
        // 光谱分组宽度 50 nm、最大迭代 50。
        config.stomatalG0 = numberOr(energy, "stomatalG0", 0.01, "temperature.energy.stomatalG0");
        config.minWindSpeed = numberOr(energy, "minWindSpeed", 0.05, "temperature.energy.minWindSpeed");
        config.maxAerodynamicResistance = numberOr(energy, "maxAerodynamicResistance", 400.0, "temperature.energy.maxAerodynamicResistance");
        config.referenceHeight = numberOr(energy, "referenceHeight", 15.0, "temperature.energy.referenceHeight");
        config.heatRoughnessKb = numberOr(energy, "heatRoughnessKb", -1.0, "temperature.energy.heatRoughnessKb");
        if (!std::isfinite(config.heatRoughnessKb) || config.heatRoughnessKb > 20)
            throw std::runtime_error("heatRoughnessKb 须在 0–20 之间（<=0 表示按地类）");
        // maxAerodynamicResistance <= 0 deliberately means "no cap" (energy.cpp resistanceCap).
        if (!std::isfinite(config.maxAerodynamicResistance))
            throw std::runtime_error("maxAerodynamicResistance 须为数值（<=0 表示不限幅）");
        config.soilEmissivity = numberOr(energy, "soilEmissivity", 0.955, "temperature.energy.soilEmissivity");
        config.soilEmissivitySet = energy.GetObj("soilEmissivity").IsValid();
        config.leafEmissivity = numberOr(energy, "leafEmissivity", 0.985, "temperature.energy.leafEmissivity");
        config.soilConductivity = numberOr(energy, "soilConductivity", 1.55, "temperature.energy.soilConductivity");
        config.soilVolumetricCapacity = numberOr(energy, "soilVolumetricCapacity", 1.298e6, "temperature.energy.soilVolumetricCapacity");
        const auto soilThermal = energy.GetString("soilThermal", "constant");
        if (soilThermal != "constant" && soilThermal != "moisture") throw std::runtime_error("soilThermal 须为 constant 或 moisture");
        config.soilThermalFromMoisture = soilThermal == "moisture";
        config.soilHeatFraction = numberOr(energy, "soilHeatFraction", 0.25, "temperature.energy.soilHeatFraction");
        if (!(config.soilHeatFraction >= 0.0 && config.soilHeatFraction <= 1.0))
            throw std::runtime_error("soilHeatFraction 须在 0 与 1 之间");
        config.solarBinWidth = integerOr(energy, "spectralBinNm", 50, "temperature.energy.spectralBinNm");
        if (config.solarBinWidth < 1 || config.solarBinWidth > 100) throw std::runtime_error("spectralBinNm 须在 1–100 nm 之间");
        config.maxIterations = integerOr(energy, "maxIterations", 50, "temperature.energy.maxIterations");
        config.iceMelt = energy.GetBool("iceMelt", true);
        config.maxStabilityIterations = integerOr(energy, "maxStabilityIterations", 60, "temperature.energy.maxStabilityIterations");
        // EB 参数的统一合法性检查（正数/范围）。
        if (!(config.stomatalG0 > 0) || !(config.minWindSpeed >= 0) || !(config.referenceHeight > 0) ||
            !(config.leafEmissivity > 0 && config.leafEmissivity <= 1) ||
            !(config.soilConductivity > 0) || !(config.soilVolumetricCapacity > 0) || config.maxIterations < 1)
            throw std::runtime_error("能量平衡参数超出有效范围");
    // 步骤 8d：RT 模式——四个组分温度：一个多波段文件，或按 soilSunlit、soilShaded、leafSunlit、
    //         leafShaded 四个字段分别给出（依次对应 Tss、Tsh、Tcs、Tch）。
    } else if (mode == "rt") {
        const auto components = temperature.GetObj("components");
        config.componentsFile = components.GetString("file", "");
        const char* keys[] = {"soilSunlit", "soilShaded", "leafSunlit", "leafShaded"};
        for (int index = 0; index < 4; ++index) config.components[index] = readField(components, keys[index]);
    } else {
        throw std::runtime_error("温度模式须为 rt 或 eb");
    }
    // 步骤 9：EB 与 RT 共用的热红外发射设置（均位于 temperature.energy 下）：
    //         土壤发射率（RT 才在此读取干土值及漫射比例；RT 无短波输入，不支持 -1/Erbs）、
    //         饱和土壤发射率、Hapke 方向发射率开关与系数、地表温度归一化方式，并做一致性检查。
    // Thermal emission settings shared by EB and RT (2026-11-20: they used to be read only in EB mode, so RT imaging
    // and RT-background retrievals silently ran with the defaults). They stay under temperature.energy.
    {
        const auto energy = temperature.GetObj("energy");
        if (mode != "eb") {
            config.soilEmissivity = numberOr(energy, "soilEmissivity", 0.955, "temperature.energy.soilEmissivity");
            config.soilEmissivitySet = energy.GetObj("soilEmissivity").IsValid();
            // FIX(2026-11-24 C5): RT imaging (optical reflectance) used a hard-coded 0.2 and ignored energy.diffuseFraction.
            // It is now read here. RT has no shortwave input, so -1 (Erbs) cannot be evaluated and is rejected explicitly.
            config.diffuseFraction = numberOr(energy, "diffuseFraction", 0.2, "temperature.energy.diffuseFraction");
            if (!(config.diffuseFraction >= 0.0 && config.diffuseFraction <= 1.0))
                throw std::runtime_error("RT 模式没有短波辐射输入，diffuseFraction 须为 0 到 1 之间的数（-1/Erbs 仅 EB 模式可用）");
        }
        // Negative selects the built-in dry-to-wet gain; otherwise it is the
        // emissivity at saturation and must be a valid value above the dry one.
        config.soilEmissivityWet = numberOr(energy, "soilEmissivityWet", -1.0, "temperature.energy.soilEmissivityWet");
        const auto emissivityModel = energy.GetString("soilEmissivityModel", "hapke");
        if (emissivityModel != "hapke" && emissivityModel != "constant")
            throw std::runtime_error("soilEmissivityModel 须为 hapke 或 constant");
        config.hapkeSoilEmissivity = emissivityModel == "hapke";
        config.hapkeSoilK = numberOr(energy, "soilEmissivityK", 1.0, "temperature.energy.soilEmissivityK");
        if (!(config.hapkeSoilK > 0)) throw std::runtime_error("soilEmissivityK 须为正数");
        const auto normalisation = energy.GetString("surfaceTemperatureNormalisation", "retrieval");
        if (normalisation != "retrieval" && normalisation != "directional")
            throw std::runtime_error("surfaceTemperatureNormalisation 须为 retrieval 或 directional");
        config.referenceSurfaceTemperature = normalisation == "retrieval";
        if (!(config.soilEmissivity > 0 && config.soilEmissivity <= 1) ||
            (config.soilEmissivityWet >= 0 && !(config.soilEmissivityWet > 0 && config.soilEmissivityWet <= 1)))
            throw std::runtime_error("土壤发射率参数超出有效范围");
        // Wet soil cannot emit less than dry soil.
        if (config.soilEmissivityWet >= 0 && config.soilEmissivityWet < config.soilEmissivity)
            throw std::runtime_error("soilEmissivityWet 须不小于 soilEmissivity（饱和土壤发射率不能低于干土）");
    }
    // 步骤 10：反射天空长波 skyLongwave：{mode: auto|none}、常数（0–700 W m-2）或栅格；
    //          结果存入 skySource / skyLongwave（语义见 config.h）。
    // Reflected sky of the thermal brightness temperature.
    const auto sky = root.GetObj("skyLongwave");
    if (sky.IsValid() && sky.GetType() != CPLJSONObject::Type::Null && sky.GetType() != CPLJSONObject::Type::Object)
        throw std::runtime_error("skyLongwave 须为对象，如 {\"value\": 300}、{\"file\": ...} 或 {\"mode\": \"none\"}，当前为 " + sky.ToString());
    if (sky.IsValid() && sky.GetType() == CPLJSONObject::Type::Object) {
        const auto skyMode = sky.GetString("mode", "auto");
        config.skyLongwave = readField(root, "skyLongwave");
        if (skyMode == "none") config.skySource = ImageConfig::SkySource::None;
        else if (config.skyLongwave.present()) config.skySource = ImageConfig::SkySource::Field;
        else if (skyMode != "auto") throw std::runtime_error("skyLongwave.mode 须为 auto 或 none，或给出常数/栅格（W m⁻²）");
        if (config.skySource == ImageConfig::SkySource::Field && !config.skyLongwave.fromFile() &&
            !(config.skyLongwave.value >= 0.0 && config.skyLongwave.value <= 700.0))
            throw std::runtime_error("skyLongwave 常数须在 0–700 W m⁻²（0 表示不计反射天空）");
    }
    // 步骤 11：常数形式的几何输入范围检查：VZA 0–90°（不含 90）、VAA ±360°、纬度 ±90°、
    //          经度 −180–360°；栅格输入不在此检查。
    // Constant geometry inputs: reject values outside the physical range instead of silently dropping every pixel.
    const auto constantRange = [](const Field& field, double low, double high, bool includeHigh, const char* label) {
        if (field.fromFile() || !std::isfinite(field.value)) return;
        if (!(field.value >= low && (includeHigh ? field.value <= high : field.value < high)))
            throw std::runtime_error(std::string(label) + "常数须在 " + std::to_string(static_cast<int>(low)) + "–" +
                                     std::to_string(static_cast<int>(high)) + " 度之间");
    };
    constantRange(config.vza, 0.0, 90.0, false, "观测天顶角 VZA ");
    constantRange(config.vaa, -360.0, 360.0, true, "观测方位角 VAA ");
    constantRange(config.latitude, -90.0, 90.0, true, "纬度 ");
    constantRange(config.longitude, -180.0, 360.0, true, "经度 ");
    // 线程数；缺省 0（由使用方决定，通常为自动，待核对）。
    config.threads = root.GetInteger("threads", 0);
    return config;
}

}  // namespace stream


