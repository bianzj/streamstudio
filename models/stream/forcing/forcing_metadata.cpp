#include "forcing_metadata.h"

#include "../scene/raster.h"

#include <cpl_json.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace stream {
namespace {

namespace fs = std::filesystem;
constexpr double kNotANumber = std::numeric_limits<double>::quiet_NaN();

std::string number(double value, int digits = 6)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.*g", digits, value);
    return text;
}

// ---------------------------------------------------------------- presets
const char* const kPresetEra5 = R"JSON({
  "kind": "stream-forcing-metadata", "schemaVersion": 1, "name": "era5",
  "description": "ERA5 逐小时 NetCDF（t2m d2m sp u10 v10 ssrd strd，辐射为逐小时累积 J m-2）",
  "form": "gridded",
  "time": {"variable": ["valid_time", "time"], "stepHours": 1},
  "grid": {"latitude": ["latitude", "lat"], "longitude": ["longitude", "lon"]},
  "variables": {
    "airTemperature": {"source": "t2m", "unit": "K", "kind": "instant", "height_m": 2},
    "humidity": {"type": "dewpoint", "source": "d2m", "unit": "K", "height_m": 2},
    "pressure": {"type": "surface", "source": "sp", "unit": "Pa"},
    "wind": {"type": "components", "height_m": 10, "u": {"source": "u10", "unit": "m s-1"}, "v": {"source": "v10", "unit": "m s-1"}},
    "shortwaveDown": {"source": "ssrd", "unit": "J m-2", "kind": "accumulated", "stamp": "interval_end"},
    "longwaveDown": {"source": "strd", "unit": "J m-2", "kind": "accumulated", "stamp": "interval_end"},
    "skinTemperature": {"source": "skt", "unit": "K"},
    "soilMoisture": {"source": "swvl1", "unit": "m3 m-3"},
    "orography": {"source": "z", "unit": "m2 s-2"}
  },
  "heightCorrection": {"wind": {"mode": "log", "targetHeight_m": 10}, "airTemperature": {"mode": "none", "targetHeight_m": 2}},
  "spatial": {"interpolation": "bilinear"}
})JSON";

const char* const kPresetEra5Land = R"JSON({
  "preset": "era5", "name": "era5-land",
  "description": "ERA5-Land 逐小时 NetCDF（ssrd/strd 自 00 UTC 累积，逐时差分）",
  "variables": {
    "shortwaveDown": {"kind": "cumulative", "resetHourUtc": 0},
    "longwaveDown": {"kind": "cumulative", "resetHourUtc": 0}
  }
})JSON";

const char* const kPresetGldas = R"JSON({
  "kind": "stream-forcing-metadata", "schemaVersion": 1, "name": "gldas-noah",
  "description": "GLDAS-2.x Noah 3 小时 NetCDF4（按产品命名与文档推断，未经真实数据验证）",
  "form": "gridded",
  "time": {"variable": ["time"], "stepHours": 3},
  "grid": {"latitude": ["lat", "latitude"], "longitude": ["lon", "longitude"]},
  "variables": {
    "airTemperature": {"source": "Tair_f_inst", "unit": "K", "kind": "instant", "height_m": 2},
    "humidity": {"type": "specific", "source": "Qair_f_inst", "unit": "kg kg-1", "height_m": 2},
    "pressure": {"type": "surface", "source": "Psurf_f_inst", "unit": "Pa"},
    "wind": {"type": "speed", "height_m": 10, "speed": {"source": "Wind_f_inst", "unit": "m s-1"}},
    "shortwaveDown": {"source": "SWdown_f_tavg", "unit": "W m-2", "kind": "interval_mean", "stamp": "interval_end"},
    "longwaveDown": {"source": "LWdown_f_tavg", "unit": "W m-2", "kind": "interval_mean", "stamp": "interval_end"},
    "skinTemperature": {"source": "AvgSurfT_inst", "unit": "K"},
    "soilMoisture": {"source": "SoilMoi0_10cm_inst", "unit": "kg m-2", "thickness_m": 0.1},
    "precipitation": {"source": "Rainf_f_tavg", "unit": "kg m-2 s-1", "kind": "interval_mean", "stamp": "interval_end"}
  },
  "heightCorrection": {"wind": {"mode": "log", "targetHeight_m": 10}, "airTemperature": {"mode": "none", "targetHeight_m": 2}},
  "spatial": {"interpolation": "bilinear"}
})JSON";

const char* const kPresetGldas025 = R"JSON({"preset": "gldas-noah", "name": "gldas-noah-0.25"})JSON";
const char* const kPresetGldas10 = R"JSON({"preset": "gldas-noah", "name": "gldas-noah-1.0"})JSON";

const char* const kPresetStation = R"JSON({
  "kind": "stream-forcing-metadata", "schemaVersion": 1, "name": "station-csv",
  "description": "气象站 CSV（time, station_id, lat, lon, elev, temp °C, rh %, pressure hPa, wind_speed m s-1, swdown, lwdown W m-2）；未写观测高度，请在元数据里补 height_m",
  "form": "station",
  "time": {"column": "time", "format": "iso8601"},
  "station": {"delimiter": ",", "comment": "#", "columns": {"id": "station_id", "latitude": "lat", "longitude": "lon", "elevation": "elev"}},
  "variables": {
    "airTemperature": {"source": "temp", "unit": "degC", "kind": "instant"},
    "humidity": {"type": "relative", "source": "rh", "unit": "%"},
    "pressure": {"type": "surface", "source": "pressure", "unit": "hPa"},
    "wind": {"type": "speed", "speed": {"source": "wind_speed", "unit": "m s-1"}},
    "shortwaveDown": {"source": "swdown", "unit": "W m-2", "kind": "interval_mean", "stamp": "interval_end"},
    "longwaveDown": {"source": "lwdown", "unit": "W m-2", "kind": "interval_mean", "stamp": "interval_end"}
  },
  "spatial": {"interpolation": "idw", "idwPower": 2, "maxStations": 4}
})JSON";

struct PresetEntry { const char* name; const char* text; };
const PresetEntry kPresets[] = {{"era5", kPresetEra5}, {"era5-land", kPresetEra5Land}, {"gldas-noah", kPresetGldas},
                                {"gldas-noah-0.25", kPresetGldas025}, {"gldas-noah-1.0", kPresetGldas10},
                                {"station-csv", kPresetStation}};

std::string presetList()
{
    std::string text;
    for (const auto& name : forcingPresetNames()) text += (text.empty() ? "" : "、") + name;
    return text;
}

// ---------------------------------------------------------------- units
struct UnitDef { const char* quantity; const char* canonical; double factor; double offset; const char* aliases; };
constexpr double kPi = 3.14159265358979323846;
const UnitDef kUnits[] = {
    {"temperature", "K", 1.0, 0.0, "k,kelvin"},
    {"temperature", "degC", 1.0, 273.15, "degc,c,°c,celsius"},
    {"temperature", "degF", 5.0 / 9.0, 273.15 - 32.0 * 5.0 / 9.0, "degf,f,°f,fahrenheit"},
    {"pressure", "Pa", 1.0, 0.0, "pa"},
    {"pressure", "hPa", 100.0, 0.0, "hpa,mbar,mb"},
    {"pressure", "kPa", 1000.0, 0.0, "kpa"},
    {"pressure", "mmHg", 133.322387415, 0.0, "mmhg"},
    {"pressure", "inHg", 3386.389, 0.0, "inhg"},
    {"fraction", "kg kg-1", 1.0, 0.0, "kgkg-1,kg/kg,1"},
    {"fraction", "g kg-1", 0.001, 0.0, "gkg-1,g/kg"},
    {"relativeHumidity", "%", 0.01, 0.0, "%,percent"},
    {"relativeHumidity", "1", 1.0, 0.0, "1,fraction"},
    {"speed", "m s-1", 1.0, 0.0, "ms-1,m/s"},
    {"speed", "km h-1", 1.0 / 3.6, 0.0, "kmh-1,km/h"},
    {"speed", "kt", 1852.0 / 3600.0, 0.0, "kt,kn,knot,knots"},
    {"speed", "mph", 0.44704, 0.0, "mph,mi/h,mih-1"},
    {"direction", "deg", 1.0, 0.0, "deg,degree,degrees,°"},
    {"power", "W m-2", 1.0, 0.0, "wm-2,w/m2"},
    {"power", "kW m-2", 1000.0, 0.0, "kwm-2,kw/m2"},
    {"power", "MJ m-2 h-1", 1.0e6 / 3600.0, 0.0, "mjm-2h-1,mj/m2/h"},
    {"power", "MJ m-2 d-1", 1.0e6 / 86400.0, 0.0, "mjm-2d-1,mj/m2/d"},
    {"power", "cal cm-2 min-1", 4.184e4 / 60.0, 0.0, "calcm-2min-1,cal/cm2/min"},
    {"energy", "J m-2", 1.0, 0.0, "jm-2,j/m2"},
    {"energy", "kJ m-2", 1.0e3, 0.0, "kjm-2,kj/m2"},
    {"energy", "MJ m-2", 1.0e6, 0.0, "mjm-2,mj/m2"},
    {"energy", "Wh m-2", 3600.0, 0.0, "whm-2,wh/m2"},
    {"energy", "kWh m-2", 3.6e6, 0.0, "kwhm-2,kwh/m2"},
    {"energy", "cal cm-2", 4.184e4, 0.0, "calcm-2,cal/cm2"},
    {"precipRate", "kg m-2 s-1", 1.0, 0.0, "kgm-2s-1,kg/m2/s"},
    {"precipRate", "mm s-1", 1.0, 0.0, "mms-1,mm/s"},
    {"precipRate", "mm h-1", 1.0 / 3600.0, 0.0, "mmh-1,mm/h"},
    {"precipRate", "m s-1", 1000.0, 0.0, "ms-1,m/s"},
    {"precipDepth", "mm", 1.0, 0.0, "mm"},
    {"precipDepth", "m", 1000.0, 0.0, "m"},
    {"precipDepth", "kg m-2", 1.0, 0.0, "kgm-2,kg/m2"},
    {"soilMoisture", "m3 m-3", 1.0, 0.0, "m3m-3,m3/m3"},
    {"soilMoisture", "%", 0.01, 0.0, "%,percent"},
    {"soilMoisture", "kg m-2", 1.0, 0.0, "kgm-2,kg/m2"},
    {"orography", "m2 s-2", 1.0, 0.0, "m2s-2,m2/s2"},
    {"orography", "m", kForcingStandardGravity, 0.0, "m,metre,meter"},
};

std::string normaliseUnit(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ' || c == '^' || c == '*' || c == '\t') continue;
        if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xB7) { ++i; continue; }  // middle dot
        if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xB0) { out += "\xC2\xB0"; ++i; continue; }
        out += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return out;
}

bool aliasMatches(const char* aliases, const std::string& normalised)
{
    std::stringstream stream(aliases);
    std::string item;
    while (std::getline(stream, item, ',')) if (normaliseUnit(item) == normalised) return true;
    return false;
}

// ---------------------------------------------------------------- JSON helpers
using Json = CPLJSONObject;

bool isNumber(const Json& o)
{
    const auto t = o.GetType();
    return t == Json::Type::Integer || t == Json::Type::Long || t == Json::Type::Double;
}

std::vector<std::string> stringList(const Json& o)
{
    std::vector<std::string> out;
    if (o.GetType() == Json::Type::String) out.push_back(o.ToString());
    else if (o.GetType() == Json::Type::Array)
        for (const auto& item : o.ToArray()) if (item.GetType() == Json::Type::String) out.push_back(item.ToString());
    return out;
}

void mergeJson(Json& base, const Json& over)
{
    for (const auto& child : over.GetChildren()) {
        const std::string key = child.GetName();
        Json existing = base.GetObj(key);
        if (existing.IsValid() && existing.GetType() == Json::Type::Object && child.GetType() == Json::Type::Object) mergeJson(existing, child);
        else { base.Delete(key); base.Add(key, child); }
    }
}

bool loadJson(const std::string& text, Json& root, std::string& reason)
{
    CPLJSONDocument document;
    if (!document.LoadMemory(text)) { reason = "不是合法的 JSON"; return false; }
    root = document.GetRoot();
    if (!root.IsValid() || root.GetType() != Json::Type::Object) { reason = "顶层必须是 JSON 对象"; return false; }
    return true;
}

// Preset texts reference their base preset through "preset": merged recursively, the overrides win.
bool resolvePreset(std::string text, Json& out, ForcingIssues& issues, int depth = 0)
{
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);  // UTF-8 byte order mark
    Json root;
    std::string reason;
    if (!loadJson(text, root, reason)) { issues.error("气象强迫元数据无法解析：" + reason); return false; }
    const std::string preset = root.GetString("preset", "");
    if (preset.empty() || depth > 4) { out = root; return true; }
    const PresetEntry* found = nullptr;
    for (const auto& entry : kPresets) if (preset == entry.name) found = &entry;
    if (!found) { issues.error("未知的气象强迫预设 \"" + preset + "\"；可用：" + presetList()); return false; }
    Json base;
    if (!resolvePreset(found->text, base, issues, depth + 1)) return false;
    root.Delete("preset");
    mergeJson(base, root);
    base.Delete("preset");
    base.Add("preset", preset);
    out = base;
    return true;
}

struct Parser {
    ForcingMetadata& m;
    ForcingIssues& issues;
    std::string where(const std::string& path) const { return "气象强迫元数据 \"" + m.name + "\"：" + path; }
    void error(const std::string& path, const std::string& text) { issues.error(where(path) + " " + text); }

    void unknownKeys(const Json& o, const std::string& path, const std::vector<std::string>& known)
    {
        for (const auto& child : o.GetChildren())
            if (std::find(known.begin(), known.end(), child.GetName()) == known.end())
                issues.warning("气象强迫元数据 \"" + m.name + "\"：" + (path.empty() ? "" : path + ".") + child.GetName() + " 不是已知字段，已忽略");
    }

    bool readNumber(const Json& o, const std::string& key, const std::string& path, double& value)
    {
        const Json child = o.GetObj(key);
        if (!child.IsValid()) return false;
        if (!isNumber(child)) { error(path + "." + key, "必须是数字"); return false; }
        value = child.ToDouble();
        return true;
    }

    bool oneOf(const std::string& value, const std::vector<std::string>& options, const std::string& path)
    {
        if (std::find(options.begin(), options.end(), value) != options.end()) return true;
        std::string list;
        for (const auto& item : options) list += (list.empty() ? "" : "、") + item;
        error(path, "\"" + value + "\" 无效；可选：" + list);
        return false;
    }

    // `quantities`: unit classes tried in order (radiation: power then energy).
    void channel(const Json& o, const std::string& path, const std::vector<std::string>& quantities, const std::string& defaultUnit,
                 const std::string& defaultKind, ForcingChannel& c, bool needSource = true)
    {
        c.declared = true;
        unknownKeys(o, path, {"source", "unit", "scale", "offset", "kind", "stamp", "resetHourUtc", "stepHours", "height_m", "missing",
                              "range", "thickness_m", "optional", "type", "u", "v", "speed", "direction", "convention", "fallback"});
        c.source = stringList(o.GetObj("source"));
        if (needSource && c.source.empty()) error(path + ".source", "必填（变量名/列名，或别名列表）");
        readNumber(o, "scale", path, c.scale);
        readNumber(o, "offset", path, c.offset);
        const std::string unit = o.GetString("unit", defaultUnit);
        bool found = false;
        for (const auto& quantity : quantities) {
            std::string canonical;
            if (forcingUnit(quantity, unit, canonical, c.unitFactor, c.unitOffset)) {
                c.unit = canonical;
                c.energyUnit = quantity == "energy" || quantity == "precipDepth";
                found = true;
                break;
            }
        }
        if (!found) {
            std::string list;
            for (const auto& quantity : quantities) list += (list.empty() ? "" : "；") + forcingUnitList(quantity);
            error(path + ".unit", "\"" + unit + "\" 不能识别；该物理量可用单位：" + list);
        }
        c.kind = o.GetString("kind", c.energyUnit && defaultKind == "interval_mean" ? "accumulated" : defaultKind);
        if (o.GetObj("kind").IsValid() == false && !found) c.kind = defaultKind;
        oneOf(c.kind, {"instant", "previous", "interval_mean", "accumulated", "cumulative"}, path + ".kind");
        const bool instantKind = c.kind == "instant" || c.kind == "previous";
        c.stamp = o.GetString("stamp", instantKind ? "instant" : "interval_end");
        oneOf(c.stamp, {"instant", "interval_start", "interval_end", "interval_mid"}, path + ".stamp");
        if (found && (c.kind == "accumulated" || c.kind == "cumulative") && !c.energyUnit)
            error(path, "kind \"" + c.kind + "\" 需要能量（累积量）单位（如 J m-2 / mm），收到 \"" + c.unit + "\"");
        if (found && (c.kind == "instant" || c.kind == "previous" || c.kind == "interval_mean") && c.energyUnit &&
            quantities.size() > 1)
            error(path, "kind \"" + c.kind + "\" 需要功率/速率单位（如 W m-2），收到 \"" + c.unit + "\"");
        double value = 0;
        if (readNumber(o, "resetHourUtc", path, value)) {
            if (value < 0 || value > 23 || value != std::floor(value)) error(path + ".resetHourUtc", "须为 0–23 的整数");
            else c.resetHourUtc = static_cast<int>(value);
        }
        if (readNumber(o, "stepHours", path, value)) {
            if (value <= 0) error(path + ".stepHours", "须大于 0"); else c.stepHours = value;
        }
        if (readNumber(o, "height_m", path, value)) { c.height_m = value; c.heightDeclared = true; }
        readNumber(o, "thickness_m", path, c.thickness_m);
        const Json missing = o.GetObj("missing");
        if (missing.IsValid()) {
            if (missing.GetType() != Json::Type::Array) error(path + ".missing", "必须是数值数组");
            else for (const auto& item : missing.ToArray()) c.missing.push_back(item.ToDouble());
        }
        const Json range = o.GetObj("range");
        if (range.IsValid()) {
            if (range.GetType() != Json::Type::Array || range.ToArray().Size() != 2) error(path + ".range", "必须是 [最小, 最大]");
            else {
                c.hasRange = true;
                c.rangeMin = range.ToArray()[0].ToDouble();
                c.rangeMax = range.ToArray()[1].ToDouble();
                if (c.rangeMin > c.rangeMax) error(path + ".range", "最小值大于最大值");
            }
        }
    }

    void missingVariable(const std::string& key, const std::string& meaning) { error("variables." + key, "缺少（" + meaning + "）"); }

    void parse(const Json& root)
    {
        m.name = root.GetString("name", "");
        if (m.name.empty()) { m.name = "未命名"; issues.warning("气象强迫元数据缺少 name，显示为“未命名”"); }
        m.description = root.GetString("description", "");
        m.preset = root.GetString("preset", "");
        const std::string kind = root.GetString("kind", "");
        if (kind.empty()) issues.warning("气象强迫元数据 \"" + m.name + "\"：缺少 kind，按 stream-forcing-metadata 处理");
        else if (kind != "stream-forcing-metadata") { issues.error("气象强迫元数据 " + m.name + " 无法解析：kind 应为 stream-forcing-metadata，收到 \"" + kind + "\""); return; }
        const int version = root.GetInteger("schemaVersion", 1);
        if (!root.GetObj("schemaVersion").IsValid()) issues.warning("气象强迫元数据 \"" + m.name + "\"：缺少 schemaVersion，按 1 处理");
        else if (version != 1) { issues.error("气象强迫元数据 " + m.name + " 无法解析：schemaVersion " + std::to_string(version) + " 不受支持（本引擎支持 1）"); return; }
        unknownKeys(root, "", {"kind", "schemaVersion", "name", "preset", "description", "form", "time", "grid", "station", "variables",
                               "heightCorrection", "spatial"});
        m.form = root.GetString("form", "");
        if (!oneOf(m.form, {"gridded", "station"}, "form")) return;
        parseTime(root.GetObj("time"));
        if (m.form == "gridded") {
            const Json grid = root.GetObj("grid");
            if (grid.IsValid()) {
                unknownKeys(grid, "grid", {"latitude", "longitude"});
                if (auto v = stringList(grid.GetObj("latitude")); !v.empty()) m.latitudeNames = v;
                if (auto v = stringList(grid.GetObj("longitude")); !v.empty()) m.longitudeNames = v;
            }
        } else parseStation(root.GetObj("station"));
        parseVariables(root.GetObj("variables"));
        parseHeightCorrection(root.GetObj("heightCorrection"));
        parseSpatial(root.GetObj("spatial"));
    }

    void parseTime(const Json& time)
    {
        if (!time.IsValid()) return;
        unknownKeys(time, "time", {"variable", "units", "column", "format", "stepHours", "utcOffsetHours", "maxGapHours"});
        if (auto v = stringList(time.GetObj("variable")); !v.empty()) m.timeVariables = v;
        m.timeUnits = time.GetString("units", "");
        m.timeColumn = time.GetString("column", m.timeColumn);
        m.timeFormat = time.GetString("format", m.timeFormat);
        if (m.form == "station")
            oneOf(m.timeFormat, {"iso8601", "epoch_seconds", "epoch_milliseconds", "yyyymmddhh", "yyyymmddhhmm"}, "time.format");
        double value = 0;
        if (readNumber(time, "stepHours", "time", value)) { if (value <= 0) error("time.stepHours", "须大于 0"); else m.stepHours = value; }
        readNumber(time, "utcOffsetHours", "time", m.utcOffsetHours);
        if (std::abs(m.utcOffsetHours) > 14) error("time.utcOffsetHours", "须在 −14 到 14 之间");
        if (readNumber(time, "maxGapHours", "time", value)) { if (value <= 0) error("time.maxGapHours", "须大于 0"); else m.maxGapHours = value; }
    }

    void parseStation(const Json& station)
    {
        if (!station.IsValid()) return;
        unknownKeys(station, "station", {"delimiter", "comment", "columns", "stations"});
        m.delimiter = station.GetString("delimiter", m.delimiter);
        if (m.delimiter == "\\t") m.delimiter = "\t";
        if (m.delimiter.size() != 1) error("station.delimiter", "必须是单个字符（, ; 或 \\t）");
        m.comment = station.GetString("comment", m.comment);
        const Json columns = station.GetObj("columns");
        if (columns.IsValid()) {
            unknownKeys(columns, "station.columns", {"id", "latitude", "longitude", "elevation"});
            m.idColumn = columns.GetString("id", "");
            m.latitudeColumn = columns.GetString("latitude", "");
            m.longitudeColumn = columns.GetString("longitude", "");
            m.elevationColumn = columns.GetString("elevation", "");
        }
        const Json list = station.GetObj("stations");
        if (list.IsValid()) {
            if (list.GetType() != Json::Type::Array) { error("station.stations", "必须是数组"); return; }
            int position = 0;
            for (const auto& item : list.ToArray()) {
                const std::string path = "station.stations[" + std::to_string(position++) + "]";
                ForcingStationEntry entry;
                entry.id = item.GetString("id", "");
                entry.file = item.GetString("file", "");
                if (entry.id.empty() && entry.file.empty()) { error(path, "需要 id 或 file"); continue; }
                if (!readNumber(item, "latitude", path, entry.latitude) || !readNumber(item, "longitude", path, entry.longitude))
                    { error(path, "站点 \"" + entry.id + "\" 缺少经纬度（latitude/longitude）"); continue; }
                if (entry.latitude < -90 || entry.latitude > 90 || entry.longitude < -360 || entry.longitude > 360)
                    { error(path, "经纬度超出范围"); continue; }
                entry.hasElevation = readNumber(item, "elevation_m", path, entry.elevation_m);
                m.stations.push_back(entry);
            }
        }
    }

    void parseVariables(const Json& variables)
    {
        if (!variables.IsValid()) { error("variables", "缺少（物理量映射）"); return; }
        unknownKeys(variables, "variables", {"airTemperature", "humidity", "pressure", "wind", "shortwaveDown", "longwaveDown", "precipitation",
                                              "skinTemperature", "soilMoisture", "orography", "directShortwave", "diffuseShortwave"});
        const std::vector<std::string> radiation{"power", "energy"};
        // air temperature
        if (const Json o = variables.GetObj("airTemperature"); o.IsValid()) {
            channel(o, "variables.airTemperature", {"temperature"}, "K", "instant", m.airTemperature);
            if (m.airTemperature.kind != "instant" && m.airTemperature.kind != "previous")
                error("variables.airTemperature.kind", "\"" + m.airTemperature.kind + "\" 无效；气温只能是 instant 或 previous");
        } else missingVariable("airTemperature", "2 m 气温");
        // humidity
        if (const Json o = variables.GetObj("humidity"); o.IsValid()) {
            m.humidityType = o.GetString("type", "");
            if (m.humidityType.empty()) error("variables.humidity.type", "必填；可选：dewpoint、specific、mixingRatio、relative、vapourPressure");
            else if (oneOf(m.humidityType, {"dewpoint", "specific", "mixingRatio", "relative", "vapourPressure"}, "variables.humidity.type")) {
                const std::string quantity = m.humidityType == "dewpoint" ? "temperature" : m.humidityType == "relative" ? "relativeHumidity"
                                           : m.humidityType == "vapourPressure" ? "pressure" : "fraction";
                const std::string unit = m.humidityType == "dewpoint" ? "K" : m.humidityType == "relative" ? "%"
                                       : m.humidityType == "vapourPressure" ? "hPa" : "kg kg-1";
                channel(o, "variables.humidity", {quantity}, unit, "instant", m.humidity);
            }
        } else missingVariable("humidity", "湿度（露点/比湿/相对湿度等）");
        // pressure
        if (const Json o = variables.GetObj("pressure"); o.IsValid()) {
            m.pressureType = o.GetString("type", "surface");
            if (oneOf(m.pressureType, {"surface", "seaLevel", "none"}, "variables.pressure.type")) {
                if (m.pressureType == "none") { m.pressure.declared = true; m.pressure.unit = "Pa"; }
                else channel(o, "variables.pressure", {"pressure"}, "Pa", "instant", m.pressure);
            }
        } else missingVariable("pressure", "气压；没有气压数据时写 {\"type\": \"none\"}");
        parseWind(variables.GetObj("wind"));
        // radiation
        if (const Json o = variables.GetObj("shortwaveDown"); o.IsValid())
            channel(o, "variables.shortwaveDown", radiation, "W m-2", "interval_mean", m.shortwaveDown);
        else missingVariable("shortwaveDown", "向下短波辐射，必需");
        if (const Json o = variables.GetObj("longwaveDown"); o.IsValid()) {
            m.longwaveFallback = o.GetString("fallback", "none");
            oneOf(m.longwaveFallback, {"none", "brutsaert"}, "variables.longwaveDown.fallback");
            if (!o.GetObj("source").IsValid() && m.longwaveFallback == "brutsaert") m.longwaveDown.declared = false;
            else channel(o, "variables.longwaveDown", radiation, "W m-2", "interval_mean", m.longwaveDown);
        } else missingVariable("longwaveDown", "向下长波辐射；没有数据时写 {\"fallback\": \"brutsaert\"}");
        // optional
        if (const Json o = variables.GetObj("skinTemperature"); o.IsValid())
            channel(o, "variables.skinTemperature", {"temperature"}, "K", "instant", m.skinTemperature);
        if (const Json o = variables.GetObj("soilMoisture"); o.IsValid()) {
            channel(o, "variables.soilMoisture", {"soilMoisture"}, "m3 m-3", "instant", m.soilMoisture);
            if (m.soilMoisture.unit == "kg m-2") {
                if (!(m.soilMoisture.thickness_m > 0)) error("variables.soilMoisture.thickness_m", "单位 kg m-2 时必须给出土层厚度（m），如 0.1");
                else m.soilMoisture.unitFactor = 1.0 / (1000.0 * m.soilMoisture.thickness_m);
            }
        }
        if (const Json o = variables.GetObj("orography"); o.IsValid())
            channel(o, "variables.orography", {"orography"}, "m2 s-2", "instant", m.orography);
        if (const Json o = variables.GetObj("precipitation"); o.IsValid()) {
            channel(o, "variables.precipitation", {"precipRate", "precipDepth"}, "kg m-2 s-1", "interval_mean", m.precipitation);
            m.ignored.push_back("precipitation");
        }
        if (const Json o = variables.GetObj("directShortwave"); o.IsValid()) {
            channel(o, "variables.directShortwave", radiation, "W m-2", "interval_mean", m.directShortwave);
            m.ignored.push_back("directShortwave");
        }
        if (const Json o = variables.GetObj("diffuseShortwave"); o.IsValid()) {
            channel(o, "variables.diffuseShortwave", radiation, "W m-2", "interval_mean", m.diffuseShortwave);
            m.ignored.push_back("diffuseShortwave");
        }
        // default heights; missing declarations only warn
        const auto height = [&](ForcingChannel& c, const std::string& label, double fallback) {
            if (!c.declared || c.heightDeclared) return;
            c.height_m = fallback;
            issues.warning("气象强迫元数据 \"" + m.name + "\"：variables." + label + " 未声明 height_m，按默认 " + number(fallback) + " m 处理且不订正（请核对数据来源）");
        };
        height(m.airTemperature, "airTemperature", 2.0);
        height(m.humidity, "humidity", 2.0);
    }

    void parseWind(const Json& o)
    {
        if (!o.IsValid()) { missingVariable("wind", "10 m 风"); return; }
        unknownKeys(o, "variables.wind", {"type", "height_m", "u", "v", "speed", "direction", "convention", "unit"});
        m.windType = o.GetString("type", "components");
        if (!oneOf(m.windType, {"components", "speedDirection", "speed"}, "variables.wind.type")) return;
        double height = 10.0;
        m.windHeightDeclared = readNumber(o, "height_m", "variables.wind", height);
        m.windHeight_m = height;
        if (!m.windHeightDeclared)
            issues.warning("气象强迫元数据 \"" + m.name + "\"：variables.wind 未声明 height_m，按默认 10 m 处理且不订正（请核对数据来源）");
        const auto part = [&](const char* key, ForcingChannel& c, const std::string& quantity, const std::string& unit) {
            const Json p = o.GetObj(key);
            if (!p.IsValid()) { error(std::string("variables.wind.") + key, "缺少（风类型 " + m.windType + " 需要）"); return; }
            channel(p, std::string("variables.wind.") + key, {quantity}, unit, "instant", c);
        };
        if (m.windType == "components") { part("u", m.windU, "speed", "m s-1"); part("v", m.windV, "speed", "m s-1"); }
        else {
            part("speed", m.windSpeed, "speed", "m s-1");
            if (m.windType == "speedDirection") {
                part("direction", m.windDirection, "direction", "deg");
                m.windConvention = o.GetString("convention", "from");
                oneOf(m.windConvention, {"from", "to"}, "variables.wind.convention");
            }
        }
    }

    void parseHeightCorrection(const Json& o)
    {
        if (o.IsValid()) {
            unknownKeys(o, "heightCorrection", {"wind", "airTemperature"});
            if (const Json w = o.GetObj("wind"); w.IsValid()) {
                unknownKeys(w, "heightCorrection.wind", {"mode", "targetHeight_m", "roughness_m"});
                m.windMode = w.GetString("mode", "log");
                oneOf(m.windMode, {"log", "none"}, "heightCorrection.wind.mode");
                readNumber(w, "targetHeight_m", "heightCorrection.wind", m.windTargetHeight_m);
                readNumber(w, "roughness_m", "heightCorrection.wind", m.roughness_m);
            }
            if (const Json t = o.GetObj("airTemperature"); t.IsValid()) {
                unknownKeys(t, "heightCorrection.airTemperature", {"mode", "targetHeight_m", "lapseRate_K_per_m"});
                m.temperatureMode = t.GetString("mode", "none");
                oneOf(m.temperatureMode, {"none", "lapse"}, "heightCorrection.airTemperature.mode");
                readNumber(t, "targetHeight_m", "heightCorrection.airTemperature", m.temperatureTargetHeight_m);
                readNumber(t, "lapseRate_K_per_m", "heightCorrection.airTemperature", m.lapseRatePerM);
            }
        }
        // wind
        const double h = m.windHeight_m, target = m.windTargetHeight_m;
        if ((m.windU.declared || m.windSpeed.declared) && std::abs(h - target) > 0.01) {
            if (m.windMode == "log") {
                if (!(h > m.roughness_m) || !(target > m.roughness_m))
                    error("heightCorrection.wind.roughness_m", "= " + number(m.roughness_m) + " m 须小于观测高度 " + number(h) + " m 和目标高度 " + number(target) + " m");
                else {
                    m.windFactor = std::log(target / m.roughness_m) / std::log(h / m.roughness_m);
                    m.notes.push_back("风速由 " + number(h) + " m 订正到 " + number(target) + " m（对数律，z0 = " + number(m.roughness_m) + " m，系数 " + number(m.windFactor, 4) + "）");
                }
            } else
                issues.warning("气象强迫元数据 \"" + m.name + "\"：风速高度 " + number(h) + " m 与引擎约定 " + number(target) + " m 不同，heightCorrection.wind.mode = none，未订正");
        }
        // air temperature / humidity
        if (m.airTemperature.declared && std::abs(m.airTemperature.height_m - m.temperatureTargetHeight_m) > 0.01) {
            if (m.temperatureMode == "lapse") {
                m.temperatureShiftK = -m.lapseRatePerM * (m.temperatureTargetHeight_m - m.airTemperature.height_m);
                m.notes.push_back("气温由 " + number(m.airTemperature.height_m) + " m 订正到 " + number(m.temperatureTargetHeight_m) + " m（递减率 " + number(m.lapseRatePerM) + " K/m，" + number(m.temperatureShiftK, 4) + " K）");
            } else
                issues.warning("气象强迫元数据 \"" + m.name + "\"：气温高度 " + number(m.airTemperature.height_m) + " m 与引擎约定 " + number(m.temperatureTargetHeight_m) + " m 不同，heightCorrection.airTemperature.mode = none，未订正");
        }
        if (m.humidity.declared && std::abs(m.humidity.height_m - m.temperatureTargetHeight_m) > 0.01)
            issues.warning("气象强迫元数据 \"" + m.name + "\"：湿度高度 " + number(m.humidity.height_m) + " m 与引擎约定 " + number(m.temperatureTargetHeight_m) + " m 不同，湿度不做高度订正");
    }

    void parseSpatial(const Json& o)
    {
        if (!o.IsValid()) { m.interpolation = m.form == "station" ? "idw" : "bilinear"; return; }
        unknownKeys(o, "spatial", {"interpolation", "idwPower", "maxStations", "maxDistanceKm", "lapseRateKPerKm"});
        m.interpolation = o.GetString("interpolation", m.form == "station" ? "idw" : "bilinear");
        if (m.form == "station") oneOf(m.interpolation, {"nearest", "idw"}, "spatial.interpolation");
        else if (m.interpolation != "bilinear") oneOf(m.interpolation, {"bilinear"}, "spatial.interpolation");
        readNumber(o, "idwPower", "spatial", m.idwPower);
        double value = 0;
        if (readNumber(o, "maxStations", "spatial", value)) {
            if (value < 1 || value > 4 || value != std::floor(value)) error("spatial.maxStations", "须为 1–4 的整数");
            else m.maxStations = static_cast<int>(value);
        }
        readNumber(o, "maxDistanceKm", "spatial", m.maxDistanceKm);
        if (readNumber(o, "lapseRateKPerKm", "spatial", value)) {
            if (value < 0 || value > 20) error("spatial.lapseRateKPerKm", "须在 0–20 K/km 之间");
            else m.lapseRateKPerKm = value;
        }
    }
};

std::string sourceList(const ForcingChannel& c)
{
    std::string text;
    for (const auto& name : c.source) text += (text.empty() ? "" : "、") + name;
    return text;
}

}  // namespace

// ---------------------------------------------------------------- issues
void ForcingIssues::error(const std::string& text) { if (std::find(errors.begin(), errors.end(), text) == errors.end()) errors.push_back(text); }
void ForcingIssues::warning(const std::string& text) { if (std::find(warnings.begin(), warnings.end(), text) == warnings.end()) warnings.push_back(text); }
std::string ForcingIssues::errorText() const
{
    std::string text;
    for (const auto& item : errors) text += (text.empty() ? "" : "\n") + item;
    return text;
}

// ---------------------------------------------------------------- presets / units / conversions
const std::vector<std::string>& forcingPresetNames()
{
    static const std::vector<std::string> names{"era5", "era5-land", "gldas-noah-0.25", "gldas-noah-1.0", "station-csv"};
    return names;
}

std::string forcingPresetJson(const std::string& name)
{
    for (const auto& entry : kPresets) {
        if (name != entry.name || name == "gldas-noah") continue;
        ForcingIssues issues;
        Json root;
        if (!resolvePreset(entry.text, root, issues)) throw std::runtime_error(issues.errorText());
        root.Delete("preset");
        return root.Format(Json::PrettyFormat::Pretty);
    }
    throw std::runtime_error("未知的气象强迫预设 \"" + name + "\"；可用：" + presetList());
}

bool forcingUnit(const std::string& quantity, const std::string& spelling, std::string& canonical, double& factor, double& offset)
{
    const std::string key = normaliseUnit(spelling);
    for (const auto& unit : kUnits) {
        if (quantity != unit.quantity || !aliasMatches(unit.aliases, key)) continue;
        canonical = unit.canonical;
        factor = unit.factor;
        offset = unit.offset;
        return true;
    }
    return false;
}

std::string forcingUnitList(const std::string& quantity)
{
    std::string text;
    for (const auto& unit : kUnits) if (quantity == unit.quantity) text += (text.empty() ? "" : "、") + std::string(unit.canonical);
    return text;
}

double forcingSaturationVapourPressure(double celsius) { return 6.107 * std::pow(10.0, 7.5 * celsius / (237.3 + celsius)); }

double forcingDewPointFromVapourPressure(double hPa)
{
    if (!(hPa > 0.0)) return kNotANumber;
    const double l = std::log10(hPa / 6.107);
    return 237.3 * l / (7.5 - l) + 273.15;
}

double forcingVapourPressureFromSpecific(double q, double pressurePa) { return q * (pressurePa / 100.0) / (0.622 + 0.378 * q); }
double forcingVapourPressureFromMixingRatio(double r, double pressurePa) { return r * (pressurePa / 100.0) / (0.622 + r); }
double forcingPressureFromAltitude(double altitudeM) { return 101325.0 * std::pow(1.0 - 2.25577e-5 * altitudeM, 5.25588); }
double forcingPressureFromSeaLevel(double seaLevelPa, double altitudeM, double airTemperatureK)
{
    return seaLevelPa * std::exp(-kForcingStandardGravity * altitudeM / (287.05 * airTemperatureK));
}
double forcingBrutsaertLongwave(double airTemperatureK, double vapourPressureHPa)
{
    const double emissivity = 1.24 * std::pow(std::max(vapourPressureHPa, 0.01) / airTemperatureK, 1.0 / 7.0);
    return emissivity * 5.670374419e-8 * std::pow(airTemperatureK, 4);
}

bool forcingStandardQuantity(const std::string& name)
{
    static const char* const names[] = {"t2m", "d2m", "sp", "u10", "v10", "ssrd", "strd", "skt", "swvl1", "z"};
    return std::any_of(std::begin(names), std::end(names), [&](const char* item) { return name == item; });
}

// ---------------------------------------------------------------- parsing
bool ForcingMetadata::era5Layout() const
{
    if (form != "gridded" || windFactor != 1.0 || temperatureShiftK != 0.0) return false;
    const auto is = [](const ForcingChannel& c, const char* name) {
        return c.declared && !c.source.empty() && c.source.front() == name && c.identity() && c.missing.empty() && !c.hasRange;
    };
    if (!is(airTemperature, "t2m") || !is(humidity, "d2m") || humidityType != "dewpoint" || !is(pressure, "sp") || pressureType != "surface") return false;
    if (windType != "components" || !is(windU, "u10") || !is(windV, "v10")) return false;
    if (stepHours != 1.0 && stepHours != 0.0) return false;
    const auto flux = [](const ForcingChannel& c, const char* name) {
        return c.declared && !c.source.empty() && c.source.front() == name && c.scale == 1.0 && c.offset == 0.0 && c.unit == "J m-2" &&
               c.unitFactor == 1.0 && c.stamp == "interval_end" && c.missing.empty() && !c.hasRange && c.stepHours == 0.0 &&
               (c.kind == "accumulated" || (c.kind == "cumulative" && c.resetHourUtc == 0));
    };
    if (!flux(shortwaveDown, "ssrd") || !flux(longwaveDown, "strd") || shortwaveDown.kind != longwaveDown.kind) return false;
    for (const ForcingChannel* c : {&skinTemperature, &soilMoisture, &orography}) if (c->declared && !c->identity()) return false;
    return true;
}

ForcingMetadata parseForcingMetadata(const std::string& jsonText, const std::string& origin, const fs::path& path, ForcingIssues& issues)
{
    ForcingMetadata metadata;
    metadata.origin = origin;
    metadata.path = path;
    metadata.baseDir = path.empty() ? fs::path() : path.parent_path();
    ForcingIssues local;
    Json root;
    const bool resolved = resolvePreset(jsonText, root, local);
    for (const auto& text : local.errors) issues.error(origin == "file" && !path.empty() ? text + "（" + utf8FromPath(path) + "）" : text);
    if (!resolved) return metadata;
    Parser parser{metadata, local};
    parser.parse(root);
    for (const auto& text : local.errors) issues.error(text);
    for (const auto& text : local.warnings) { issues.warning(text); metadata.warnings.push_back(text); }
    return metadata;
}

ForcingMetadata resolveForcingMetadata(const std::string& specifier, const fs::path& baseDir, ForcingIssues* outer)
{
    ForcingIssues local;
    ForcingIssues& issues = outer ? *outer : local;
    ForcingMetadata metadata;
    const std::string trimmed = [&] { auto s = specifier; s.erase(0, s.find_first_not_of(" \t\r\n")); return s; }();
    if (!trimmed.empty() && trimmed[0] == '{') metadata = parseForcingMetadata(trimmed, "inline", {}, issues);
    else {
        bool preset = false;
        for (const auto& name : forcingPresetNames()) preset = preset || name == specifier;
        if (preset) {
            metadata = parseForcingMetadata("{\"preset\": \"" + specifier + "\"}", "preset", {}, issues);
        } else {
            fs::path file = pathFromUtf8(specifier);
            if (file.is_relative() && !baseDir.empty()) file = baseDir / file;
            std::ifstream stream(file, std::ios::binary);
            if (!stream) issues.error("未知的气象强迫预设或找不到元数据文件 \"" + specifier + "\"；预设可用：" + presetList());
            else {
                std::stringstream buffer;
                buffer << stream.rdbuf();
                metadata = parseForcingMetadata(buffer.str(), "file", fs::absolute(file), issues);
            }
        }
    }
    if (!outer && !issues.ok()) throw std::runtime_error(issues.errorText());
    return metadata;
}

ForcingMetadata forcingMetadataForProduct(const std::string& product)
{
    ForcingIssues issues;
    ForcingMetadata metadata = parseForcingMetadata(std::string("{\"preset\": \"") + (product == "era5-land" ? "era5-land" : "era5") + "\"}", "preset", {}, issues);
    if (!issues.ok()) throw std::runtime_error(issues.errorText());
    return metadata;
}

void checkForcingAgainstNames(const ForcingMetadata& m, const std::vector<std::string>& dataNames, ForcingIssues& issues)
{
    const auto has = [&](const std::string& name) { return std::find(dataNames.begin(), dataNames.end(), name) != dataNames.end(); };
    const auto first = [&](const ForcingChannel& c) { for (const auto& name : c.source) if (has(name)) return true; return false; };
    std::string listed;
    for (size_t i = 0; i < dataNames.size() && i < 20; ++i) listed += (i ? "、" : "") + dataNames[i];
    const auto need = [&](const ForcingChannel& c, const std::string& path, bool required) {
        if (!c.declared || c.source.empty() || first(c)) return;
        const std::string text = "气象强迫数据缺少变量 \"" + c.source.front() + "\"（" + path + ".source，已尝试别名：" + sourceList(c) + "）；数据里有：" + listed + "。请在元数据中改 source 或加别名";
        if (required) issues.error(text); else issues.warning("可选量 " + path + "：" + text);
    };
    need(m.airTemperature, "variables.airTemperature", true);
    need(m.humidity, "variables.humidity", true);
    if (m.pressureType != "none") need(m.pressure, "variables.pressure", true);
    need(m.windU, "variables.wind.u", true);
    need(m.windV, "variables.wind.v", true);
    need(m.windSpeed, "variables.wind.speed", true);
    need(m.windDirection, "variables.wind.direction", true);
    need(m.shortwaveDown, "variables.shortwaveDown", true);
    if (m.longwaveDown.declared) need(m.longwaveDown, "variables.longwaveDown", m.longwaveFallback != "brutsaert");
    for (const auto& pair : std::vector<std::pair<const ForcingChannel*, const char*>>{{&m.skinTemperature, "variables.skinTemperature"},
             {&m.soilMoisture, "variables.soilMoisture"}, {&m.orography, "variables.orography"}})
        need(*pair.first, pair.second, false);
}

std::string forcingMetadataSummaryJson(const ForcingMetadata& m)
{
    Json root;
    Json meta;
    meta.Add("name", m.name);
    meta.Add("origin", m.origin);
    if (!m.path.empty()) meta.Add("path", utf8FromPath(m.path));
    if (!m.preset.empty()) meta.Add("preset", m.preset);
    meta.Add("form", m.form);
    meta.Add("stepHours", m.stepHours);
    root.Add("metadata", meta);
    Json variables;
    const auto add = [&](const char* key, const ForcingChannel& c, const std::string& type = "") {
        if (!c.declared) return;
        Json o;
        if (!type.empty()) o.Add("type", type);
        o.Add("source", sourceList(c));
        o.Add("unit", c.unit);
        if (!c.kind.empty()) o.Add("kind", c.kind);
        if (c.heightDeclared || key == std::string("airTemperature")) o.Add("height_m", c.height_m);
        variables.Add(key, o);
    };
    add("airTemperature", m.airTemperature);
    add("humidity", m.humidity, m.humidityType);
    add("pressure", m.pressure, m.pressureType);
    if (m.windType == "components") { add("windU", m.windU, "components"); add("windV", m.windV); }
    else { add("windSpeed", m.windSpeed, m.windType); add("windDirection", m.windDirection); }
    add("shortwaveDown", m.shortwaveDown);
    add("longwaveDown", m.longwaveDown, m.longwaveFallback == "brutsaert" ? "brutsaert-fallback" : "");
    add("skinTemperature", m.skinTemperature);
    add("soilMoisture", m.soilMoisture);
    add("orography", m.orography);
    root.Add("variables", variables);
    Json height;
    Json wind;
    wind.Add("from_m", m.windHeight_m);
    wind.Add("to_m", m.windTargetHeight_m);
    wind.Add("mode", m.windMode);
    wind.Add("factor", m.windFactor);
    height.Add("wind", wind);
    Json temperature;
    temperature.Add("applied", m.temperatureShiftK != 0.0);
    temperature.Add("shiftK", m.temperatureShiftK);
    height.Add("airTemperature", temperature);
    root.Add("heightCorrection", height);
    CPLJSONArray ignored;
    for (const auto& name : m.ignored) ignored.Add(name);
    root.Add("ignored", ignored);
    return root.Format(Json::PrettyFormat::Pretty);
}

}  // namespace stream
