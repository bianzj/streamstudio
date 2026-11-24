#include "era5.h"
#include "forcing_resample.h"
#include "utc.h"
#include "../scene/raster.h"

#include <cpl_json.h>
#include <netcdf.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace stream::era5 {
namespace {

namespace fs = std::filesystem;
constexpr float kMissing = std::numeric_limits<float>::quiet_NaN();

std::string nativePath(const fs::path& path)
{
#ifdef _WIN32
    // netCDF opens files through the ANSI code page; non-ASCII paths go through their 8.3 short name.
    const std::wstring wide = path.wstring();
    if (std::all_of(wide.begin(), wide.end(), [](wchar_t c) { return c < 128; })) return path.string();
    const DWORD length = GetShortPathNameW(wide.c_str(), nullptr, 0);
    if (length > 0) {
        std::wstring shortPath(length, L'\0');
        const DWORD written = GetShortPathNameW(wide.c_str(), shortPath.data(), length);
        shortPath.resize(written);
        if (written > 0 && std::all_of(shortPath.begin(), shortPath.end(), [](wchar_t c) { return c < 128; }))
            return fs::path(shortPath).string();
    }
    throw std::runtime_error("ERA5 文件路径含非 ASCII 字符且系统未提供短文件名，请移到英文路径下：" + utf8FromPath(path));
#else
    return path.string();
#endif
}

class NcFile {
public:
    explicit NcFile(const fs::path& path) : path_(path)
    {
        const int status = nc_open(nativePath(path).c_str(), NC_NOWRITE, &id_);
        if (status != NC_NOERR)
            throw std::runtime_error("无法打开 ERA5 NetCDF（" + std::string(nc_strerror(status)) + "；GRIB 请先转换为 NetCDF）：" + utf8FromPath(path));
    }
    ~NcFile() { if (id_ >= 0) nc_close(id_); }
    NcFile(const NcFile&) = delete;
    NcFile& operator=(const NcFile&) = delete;
    int id() const { return id_; }
    const fs::path& path() const { return path_; }

    int variable(const char* name) const
    {
        int varid = -1;
        return nc_inq_varid(id_, name, &varid) == NC_NOERR ? varid : -1;
    }
    template <size_t N>
    int firstVariable(const char* const (&names)[N]) const
    {
        for (const char* name : names) if (int varid = variable(name); varid >= 0) return varid;
        return -1;
    }
    int firstVariable(const std::vector<std::string>& names) const
    {
        for (const auto& name : names) if (int varid = variable(name.c_str()); varid >= 0) return varid;
        return -1;
    }
    std::vector<std::string> allVariableNames() const
    {
        std::vector<std::string> names;
        int count = 0;
        nc_inq_nvars(id_, &count);
        for (int varid = 0; varid < count; ++varid) {
            char name[NC_MAX_NAME + 1] = {};
            nc_inq_varname(id_, varid, name);
            names.push_back(name);
        }
        return names;
    }
    std::vector<double> readAll(int varid) const
    {
        int dimensions = 0;
        nc_inq_varndims(id_, varid, &dimensions);
        std::vector<int> dims(dimensions);
        nc_inq_vardimid(id_, varid, dims.data());
        size_t total = 1;
        for (int dim : dims) {
            size_t length = 0;
            nc_inq_dimlen(id_, dim, &length);
            total *= length;
        }
        std::vector<double> values(total);
        if (total && nc_get_var_double(id_, varid, values.data()) != NC_NOERR)
            throw std::runtime_error("读取 ERA5 坐标失败：" + utf8FromPath(path_));
        return values;
    }
    std::string textAttribute(int varid, const char* name) const
    {
        size_t length = 0;
        if (nc_inq_attlen(id_, varid, name, &length) != NC_NOERR) return "";
        std::string text(length, '\0');
        if (nc_get_att_text(id_, varid, name, text.data()) != NC_NOERR) return "";
        while (!text.empty() && text.back() == '\0') text.pop_back();
        return text;
    }
    bool numberAttribute(int varid, const char* name, double& value) const
    {
        return nc_get_att_double(id_, varid, name, &value) == NC_NOERR;
    }
    std::string dimensionName(int dim) const
    {
        char name[NC_MAX_NAME + 1] = {};
        nc_inq_dimname(id_, dim, name);
        return name;
    }

private:
    fs::path path_;
    int id_ = -1;
};

// "hours since 1900-01-01 00:00:00.0" -> seconds per unit and Unix epoch offset.
void timeUnits(const std::string& units, double& scale, double& epoch)
{
    const auto since = units.find(" since ");
    if (since == std::string::npos) throw std::runtime_error("无法识别 ERA5 时间单位：" + units);
    const std::string name = units.substr(0, since);
    scale = name.rfind("second", 0) == 0 ? 1.0 : name.rfind("minute", 0) == 0 ? 60.0 : name.rfind("hour", 0) == 0 ? 3600.0
          : name.rfind("day", 0) == 0 ? 86400.0 : 0.0;
    if (!scale) throw std::runtime_error("无法识别 ERA5 时间单位：" + units);
    const char* rest = units.c_str() + since + 7;
    int year = 0, month = 0, day = 0, consumed = 0;
    if (std::sscanf(rest, "%d-%d-%d%n", &year, &month, &day, &consumed) != 3) throw std::runtime_error("无法识别 ERA5 时间单位：" + units);
    rest += consumed;
    double clock = 0;
    if (*rest == ' ' || *rest == 'T') {
        int hour = 0, minute = 0;
        double second = 0;
        const int parts = std::sscanf(rest + 1, "%d:%d:%lf", &hour, &minute, &second);
        if (parts >= 2) clock = hour * 3600.0 + minute * 60.0 + (parts == 3 ? second : 0.0);
    }
    epoch = static_cast<double>(daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day))) * 86400.0 + clock;
}

struct Axis {
    double first = 0, step = 0;
    int count = 0;
};

Axis regularAxis(const std::vector<double>& values, const std::string& label, const fs::path& file)
{
    if (values.size() < 2) throw std::runtime_error("ERA5 " + label + " 至少需要 2 个格点：" + utf8FromPath(file));
    Axis axis{values[0], values[1] - values[0], static_cast<int>(values.size())};
    for (size_t index = 1; index < values.size(); ++index)
        if (std::abs(values[index] - values[index - 1] - axis.step) > 1e-4 * std::max(1.0, std::abs(axis.step)))
            throw std::runtime_error("ERA5 " + label + " 不是等间距网格：" + utf8FromPath(file));
    return axis;
}

bool sameAxis(const Axis& a, const Axis& b) { return a.count == b.count && std::abs(a.first - b.first) < 1e-6 && std::abs(a.step - b.step) < 1e-6; }

struct FileScan {
    Axis latitude, longitude;
    std::vector<int64_t> hours;  // per time index, hours since epoch
    std::vector<std::string> variables;
    std::vector<std::string> allNames;  // every variable of the file (for messages)
    std::set<std::string> invariant;
};

// Coordinate names and the reading rules of one pack: the ERA5 defaults, or what the forcing metadata declares.
struct Names {
    std::vector<std::string> time{"valid_time", "time"}, latitude{"latitude", "lat"}, longitude{"longitude", "lon"};
    std::string timeUnits;       // overrides the units attribute
    bool strictHours = false;    // metadata-driven packs: records must be on whole hours
    std::set<std::string> orography{"z"};
};

FileScan scanFile(const NcFile& file, const Names& names, const std::vector<std::string>& wanted)
{
    FileScan scan;
    scan.allNames = file.allVariableNames();
    const int latitude = file.firstVariable(names.latitude);
    const int longitude = file.firstVariable(names.longitude);
    if (latitude < 0 || longitude < 0) throw std::runtime_error("ERA5 文件缺少 latitude/longitude：" + utf8FromPath(file.path()));
    scan.latitude = regularAxis(file.readAll(latitude), "纬度", file.path());
    scan.longitude = regularAxis(file.readAll(longitude), "经度", file.path());
    const int time = file.firstVariable(names.time);
    int timeDimension = -1;
    if (time >= 0) {
        int timeDimensions = 0;
        if (nc_inq_varndims(file.id(), time, &timeDimensions) != NC_NOERR || timeDimensions != 1)
            throw std::runtime_error("ERA5 时间变量必须是一维 time/valid_time（来自 GRIB 的 (time, step) 请先展开 step）：" + utf8FromPath(file.path()));
        if (nc_inq_vardimid(file.id(), time, &timeDimension) != NC_NOERR)
            throw std::runtime_error("无法读取 ERA5 时间变量的维度：" + utf8FromPath(file.path()));
        double scale = 0, epoch = 0;
        timeUnits(names.timeUnits.empty() ? file.textAttribute(time, "units") : names.timeUnits, scale, epoch);
        // DEFERRED[低] 数值: llround 把非整点时刻（如 30 min、三小时步长的半点、带 offset 的累积窗口中点）悄悄吸附到最近整点，可能造成重复小时或错位；calendar 属性也被忽略；建议：|x−round(x)|>1e-3 h 时报错，calendar 非 standard/gregorian/proleptic_gregorian 时报错
        for (const double value : file.readAll(time)) {
            const double hours = (epoch + value * scale) / 3600.0;
            if (names.strictHours && std::abs(hours - std::round(hours)) > 1e-3)
                throw std::runtime_error("气象强迫数据的时刻必须是整点（收到 " + formatUtc(hours * 3600.0) + "）；非整点或亚小时数据请先重采样：" + utf8FromPath(file.path()));
            scan.hours.push_back(static_cast<int64_t>(std::llround(hours)));
        }
    }
    for (const auto& name : wanted) {
        const int varid = file.variable(name.c_str());
        if (varid < 0) continue;
        int dimensions = 0;
        nc_inq_varndims(file.id(), varid, &dimensions);
        std::vector<int> dims(dimensions);
        nc_inq_vardimid(file.id(), varid, dims.data());
        const bool timed = timeDimension >= 0 && std::find(dims.begin(), dims.end(), timeDimension) != dims.end();
        scan.variables.push_back(name);
        if (!timed || (scan.hours.size() == 1 && names.orography.count(name))) scan.invariant.insert(name);
    }
    return scan;
}

struct Layout {
    int varid = -1;
    std::vector<size_t> lengths;
    int timeAxis = -1, latitudeAxis = -1, longitudeAxis = -1;
    double scale = 1, offset = 0, fill = 0, missing = 0;
    bool hasFill = false, hasMissing = false;
};

Layout layoutOf(const NcFile& file, const std::string& name, const Names& names)
{
    Layout layout;
    layout.varid = file.variable(name.c_str());
    if (layout.varid < 0) throw std::runtime_error("ERA5 文件缺少变量 " + name + "：" + utf8FromPath(file.path()));
    int dimensions = 0;
    nc_inq_varndims(file.id(), layout.varid, &dimensions);
    std::vector<int> dims(dimensions);
    nc_inq_vardimid(file.id(), layout.varid, dims.data());
    int timeDimension = -1;
    const int timeVariable = file.firstVariable(names.time);
    if (timeVariable >= 0) nc_inq_vardimid(file.id(), timeVariable, &timeDimension);
    for (int axis = 0; axis < dimensions; ++axis) {
        size_t length = 0;
        nc_inq_dimlen(file.id(), dims[axis], &length);
        layout.lengths.push_back(length);
        const auto dimension = file.dimensionName(dims[axis]);
        if (dims[axis] == timeDimension) layout.timeAxis = axis;
        else if (std::find(names.latitude.begin(), names.latitude.end(), dimension) != names.latitude.end()) layout.latitudeAxis = axis;
        else if (std::find(names.longitude.begin(), names.longitude.end(), dimension) != names.longitude.end()) layout.longitudeAxis = axis;
    }
    if (layout.latitudeAxis < 0 || layout.longitudeAxis < 0)
        throw std::runtime_error("ERA5 变量 " + name + " 缺少经纬度维：" + utf8FromPath(file.path()));
    file.numberAttribute(layout.varid, "scale_factor", layout.scale);
    file.numberAttribute(layout.varid, "add_offset", layout.offset);
    layout.hasFill = file.numberAttribute(layout.varid, "_FillValue", layout.fill);
    layout.hasMissing = file.numberAttribute(layout.varid, "missing_value", layout.missing);
    return layout;
}

// Reads one time step over the window; extra axes (expver, number) keep the first valid member.
void readFrame(const NcFile& file, const Layout& layout, size_t timeIndex, int row0, int rows, int column0, int columns, float* out)
{
    const size_t axes = layout.lengths.size();
    std::vector<size_t> start(axes, 0), count(layout.lengths);
    if (layout.timeAxis >= 0) { start[layout.timeAxis] = timeIndex; count[layout.timeAxis] = 1; }
    start[layout.latitudeAxis] = static_cast<size_t>(row0);
    count[layout.latitudeAxis] = static_cast<size_t>(rows);
    start[layout.longitudeAxis] = static_cast<size_t>(column0);
    count[layout.longitudeAxis] = static_cast<size_t>(columns);
    size_t total = 1;
    for (const size_t length : count) total *= length;
    std::vector<double> raw(total);
    if (nc_get_vara_double(file.id(), layout.varid, start.data(), count.data(), raw.data()) != NC_NOERR)
        throw std::runtime_error("读取 ERA5 数据失败：" + utf8FromPath(file.path()));
    std::fill(out, out + static_cast<size_t>(rows) * columns, kMissing);
    std::vector<size_t> index(axes, 0);
    for (size_t element = 0; element < total; ++element) {
        size_t remainder = element;
        for (size_t axis = axes; axis-- > 0;) {
            index[axis] = remainder % count[axis];
            remainder /= count[axis];
        }
        const size_t cell = index[layout.latitudeAxis] * columns + index[layout.longitudeAxis];
        if (std::isfinite(out[cell])) continue;
        const double value = raw[element];
        if (!std::isfinite(value) || (layout.hasFill && value == layout.fill) || (layout.hasMissing && value == layout.missing) ||
            std::abs(value) > 1e30)
            continue;
        out[cell] = static_cast<float>(value * layout.scale + layout.offset);
    }
}

std::string isoNow()
{
    const auto now = std::chrono::system_clock::now();
    return formatUtc(static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()));
}

}  // namespace

const std::vector<std::string>& requiredVariables()
{
    // FIX(2026-11-24): skt and stl1 are no longer required. The EB initial values come from the 2 m air temperature; skt is
    // read only for water pixels (surface skin temperature), stl1/stl2 are not used by the model any more.
    static const std::vector<std::string> names{"u10", "v10", "t2m", "d2m", "sp", "ssrd", "strd"};
    return names;
}

const std::vector<std::string>& optionalVariables()
{
    static const std::vector<std::string> names{"skt", "stl1", "stl2", "swvl1", "z"};
    return names;
}

bool registerPack(const std::vector<std::string>& files, const fs::path& index, const std::string& requestedProduct)
{
    if (files.empty()) throw std::runtime_error("请提供 ERA5 NetCDF 文件");
    Axis latitude, longitude;
    std::map<std::string, std::set<int64_t>> hours;
    std::set<std::string> invariant;
    std::map<int, size_t> yearCounts;
    CPLJSONArray fileList;
    for (size_t position = 0; position < files.size(); ++position) {
        const fs::path path = pathFromUtf8(files[position]);
        if (!fs::is_regular_file(path)) throw std::runtime_error("ERA5 文件不存在：" + files[position]);
        const NcFile file(path);
        const FileScan scan = scanFile(file, Names{}, [] { auto w = requiredVariables(); w.insert(w.end(), optionalVariables().begin(), optionalVariables().end()); return w; }());
        if (position == 0) { latitude = scan.latitude; longitude = scan.longitude; }
        else if (!sameAxis(latitude, scan.latitude) || !sameAxis(longitude, scan.longitude))
            throw std::runtime_error("ERA5 文件的经纬度网格不一致：" + files[position]);
        for (const auto& name : scan.variables) {
            if (scan.invariant.count(name)) { invariant.insert(name); continue; }
            for (const int64_t hour : scan.hours) {
                hours[name].insert(hour);
                if (name == "t2m") {
                    int year; unsigned month, day;
                    civilFromDays(hour >= 0 ? hour / 24 : (hour - 23) / 24, year, month, day);
                    ++yearCounts[year];
                }
            }
        }
        CPLJSONObject entry;
        const fs::path absolute = fs::absolute(path);
        const fs::path local = absolute.lexically_relative(fs::absolute(index).parent_path());
        const bool inside = !local.empty() && *local.begin() != "..";
        entry.Add("path", utf8FromPath(inside ? local : absolute));
        entry.Add("bytes", static_cast<GInt64>(fs::file_size(path)));
        CPLJSONArray names;
        for (const auto& name : scan.variables) names.Add(name);
        entry.Add("variables", names);
        entry.Add("timeSteps", static_cast<GInt64>(scan.hours.size()));
        fileList.Add(entry);
        std::cout << "STREAM\tprogress\t" << (100.0 * (position + 1) / files.size()) << "\t扫描 ERA5 文件\n";
    }
    if (yearCounts.empty()) throw std::runtime_error("ERA5 文件中没有逐小时的 2 m 气温 t2m");
    const std::string product = !requestedProduct.empty() && requestedProduct != "auto" ? requestedProduct
                              : std::abs(latitude.step) < 0.15 ? "era5-land" : "era5";
    if (product != "era5" && product != "era5-land") throw std::runtime_error("ERA5 产品须为 era5 或 era5-land");

    // Common period: the hours every present required variable spans. A pack may cover one day or a whole year.
    int64_t periodFirst = std::numeric_limits<int64_t>::min(), periodLast = std::numeric_limits<int64_t>::max();
    for (const auto& name : requiredVariables()) {
        const auto found = hours.find(name);
        if (found == hours.end() || found->second.empty()) continue;
        periodFirst = std::max(periodFirst, *found->second.begin());
        periodLast = std::min(periodLast, *found->second.rbegin());
    }
    if (periodFirst > periodLast) throw std::runtime_error("ERA5 必需变量的时间范围没有重叠");
    const int64_t expected = periodLast - periodFirst + 1;
    int year; unsigned month, day;
    civilFromDays(periodFirst >= 0 ? periodFirst / 24 : (periodFirst - 23) / 24, year, month, day);
    const int64_t yearFirst = daysFromCivil(year, 1, 1) * 24;
    const bool fullYear = periodFirst <= yearFirst && periodLast >= yearFirst + (isLeapYear(year) ? 366 : 365) * 24 - 1;

    CPLJSONObject variables;
    CPLJSONArray missing, incomplete;
    bool complete = true;
    std::vector<std::string> names = requiredVariables();
    names.insert(names.end(), optionalVariables().begin(), optionalVariables().end());
    for (const auto& name : names) {
        const bool required = std::find(requiredVariables().begin(), requiredVariables().end(), name) != requiredVariables().end();
        CPLJSONObject item;
        if (invariant.count(name)) {
            item.Add("invariant", true);
            item.Add("complete", true);
            variables.Add(name, item);
            continue;
        }
        const auto found = hours.find(name);
        if (found == hours.end()) {
            if (required) { missing.Add(name); complete = false; }
            continue;
        }
        GInt64 count = 0;
        for (const int64_t hour : found->second) if (hour >= periodFirst && hour <= periodLast) ++count;
        item.Add("hours", count);
        item.Add("expected", static_cast<GInt64>(expected));
        item.Add("complete", count == expected);
        variables.Add(name, item);
        if (count != expected && required) { incomplete.Add(name); complete = false; }
    }
    CPLJSONObject period;
    period.Add("start", formatUtc(static_cast<double>(periodFirst) * 3600.0));
    period.Add("end", formatUtc(static_cast<double>(periodLast) * 3600.0));
    period.Add("hours", static_cast<GInt64>(expected));

    CPLJSONObject grid, latitudeItem, longitudeItem;
    latitudeItem.Add("first", latitude.first); latitudeItem.Add("step", latitude.step); latitudeItem.Add("count", latitude.count);
    longitudeItem.Add("first", longitude.first); longitudeItem.Add("step", longitude.step); longitudeItem.Add("count", longitude.count);
    grid.Add("latitude", latitudeItem);
    grid.Add("longitude", longitudeItem);
    CPLJSONObject root;
    root.Add("kind", "stream-era5-pack");
    root.Add("schemaVersion", 1);
    root.Add("product", product);
    root.Add("year", year);
    root.Add("period", period);
    root.Add("fullYear", fullYear);
    root.Add("createdAt", isoNow());
    root.Add("grid", grid);
    root.Add("variables", variables);
    root.Add("files", fileList);
    root.Add("complete", complete);
    root.Add("missingVariables", missing);
    root.Add("incompleteVariables", incomplete);
    CPLJSONDocument document;
    document.SetRoot(root);
    if (!document.Save(utf8FromPath(index))) throw std::runtime_error("无法写出气象数据包索引：" + utf8FromPath(index));
    std::cout << "[STREAM] 气象数据包：" << product << " " << formatUtc(static_cast<double>(periodFirst) * 3600.0) << " – "
              << formatUtc(static_cast<double>(periodLast) * 3600.0) << "（" << expected << " 个时次" << (fullYear ? "，整年" : "") << "），"
              << files.size() << " 个文件，" << (complete ? "必需变量逐小时连续" : "必需变量缺失或不连续") << "\n";
    return complete;
}

namespace {

std::vector<std::string> legacyWanted()
{
    std::vector<std::string> wanted = requiredVariables();
    wanted.insert(wanted.end(), optionalVariables().begin(), optionalVariables().end());
    return wanted;
}

Names namesOf(const ForcingMetadata& meta)
{
    Names names;
    names.time = meta.timeVariables;
    names.latitude = meta.latitudeNames;
    names.longitude = meta.longitudeNames;
    names.timeUnits = meta.timeUnits;
    names.strictHours = true;
    names.orography.clear();
    for (const auto& name : meta.orography.source) names.orography.insert(name);
    return names;
}

std::vector<std::string> wantedOf(const ForcingMetadata& meta)
{
    std::vector<std::string> wanted;
    for (const ForcingChannel* c : {&meta.airTemperature, &meta.humidity, &meta.pressure, &meta.windU, &meta.windV, &meta.windSpeed,
                                    &meta.windDirection, &meta.shortwaveDown, &meta.longwaveDown, &meta.skinTemperature,
                                    &meta.soilMoisture, &meta.orography})
        for (const auto& name : c->source) wanted.push_back(name);
    return wanted;
}

double medianStep(const std::map<int64_t, int>& hours)
{
    std::vector<double> steps;
    int64_t previous = 0;
    bool first = true;
    for (const auto& item : hours) {
        if (item.first == std::numeric_limits<int64_t>::min()) continue;
        if (!first) steps.push_back(static_cast<double>(item.first - previous));
        previous = item.first;
        first = false;
    }
    if (steps.empty()) return 1.0;
    std::sort(steps.begin(), steps.end());
    return steps[steps.size() / 2];
}

}  // namespace

void Pack::open(const fs::path& index, const ForcingMetadata* metadata)
{
    CPLJSONDocument document;
    if (!document.Load(utf8FromPath(index))) throw std::runtime_error("无法读取气象数据包索引：" + utf8FromPath(index));
    const auto root = document.GetRoot();
    const std::string kind = root.GetString("kind", "");
    if ((kind != "stream-era5-pack" && kind != "stream-forcing-pack") || root.GetInteger("schemaVersion", 0) != 1)
        throw std::runtime_error("不是气象数据包索引：" + utf8FromPath(index));
    indexProduct_ = root.GetString("product", "era5");
    year_ = root.GetInteger("year", 0);
    if (metadata) meta_ = *metadata;
    else if (const std::string spec = root.GetString("metadata", ""); !spec.empty()) {
        meta_ = resolveForcingMetadata(spec, fs::absolute(index).parent_path());
        meta_.origin = "pack";
    } else meta_ = forcingMetadataForProduct(indexProduct_);
    if (meta_.station()) throw std::runtime_error("气象强迫元数据 \"" + meta_.name + "\" 是站点形态，不能用于网格数据包：" + utf8FromPath(index));
    generic_ = !meta_.era5Layout();
    land_ = !generic_ && meta_.shortwaveDown.kind == "cumulative";
    product_ = generic_ ? meta_.name : indexProduct_;
    for (const auto& text : meta_.warnings) report_.warn(text);
    for (const auto& text : meta_.notes) report_.info(text);
    for (const auto& name : meta_.ignored) report_.ignored.push_back(name);
    const auto grid = root.GetObj("grid");
    latitude0_ = grid.GetObj("latitude").GetDouble("first", 0);
    latitudeStep_ = grid.GetObj("latitude").GetDouble("step", 0);
    latitudeCount_ = grid.GetObj("latitude").GetInteger("count", 0);
    longitude0_ = grid.GetObj("longitude").GetDouble("first", 0);
    longitudeStep_ = grid.GetObj("longitude").GetDouble("step", 0);
    longitudeCount_ = grid.GetObj("longitude").GetInteger("count", 0);
    if (!latitudeStep_ || !longitudeStep_ || latitudeCount_ < 2 || longitudeCount_ < 2) throw std::runtime_error("气象数据包网格无效");
    globalLongitude_ = std::abs(std::abs(longitudeStep_) * longitudeCount_ - 360.0) < std::abs(longitudeStep_) * 0.5;
    const double lastLongitude = longitude0_ + longitudeStep_ * (longitudeCount_ - 1);
    longitude360_ = std::max(longitude0_, lastLongitude) > 180.0 + 1e-6;

    const Names names = generic_ ? namesOf(meta_) : Names{};
    const std::vector<std::string> wanted = generic_ ? wantedOf(meta_) : legacyWanted();
    std::vector<std::string> dataNames;
    const auto list = root.GetArray("files");
    for (int position = 0; position < list.Size(); ++position) {
        const auto item = list[position];
        fs::path path = pathFromUtf8(item.GetString("path", ""));
        // Relative entries belong to the index's folder, so a pack shipped inside a project stays valid when copied.
        if (path.is_relative()) path = fs::absolute(index).parent_path() / path;
        if (!fs::is_regular_file(path)) throw std::runtime_error("气象数据包中的文件不存在：" + utf8FromPath(path));
        if (static_cast<GInt64>(fs::file_size(path)) != item.GetLong("bytes", -1))
            throw std::runtime_error("气象强迫文件在登记后被修改，请重新登记数据包：" + utf8FromPath(path));
        files_.push_back(utf8FromPath(path));
        const NcFile file(path);
        const FileScan scan = scanFile(file, names, wanted);
        for (const auto& name : scan.allNames) if (std::find(dataNames.begin(), dataNames.end(), name) == dataNames.end()) dataNames.push_back(name);
        for (const auto& name : scan.variables) {
            if (scan.invariant.count(name)) { sources_[name][std::numeric_limits<int64_t>::min()] = Source{position, 0}; continue; }
            auto& map = sources_[name];
            for (size_t time = 0; time < scan.hours.size(); ++time) map.emplace(scan.hours[time], Source{position, time});
        }
    }
    if (!generic_) {
        std::string absent;
        for (const auto& name : requiredVariables()) if (!sources_.count(name)) absent += (absent.empty() ? "" : "、") + name;
        if (!absent.empty()) throw std::runtime_error("气象数据包缺少必需变量 " + absent + "：" + utf8FromPath(index));
        const int64_t yearFirst = daysFromCivil(year_, 1, 1) * 24;
        std::string gap;
        fullYear_ = covers(requiredVariables(), yearFirst, yearFirst + (isLeapYear(year_) ? 366 : 365) * 24 - 1, gap);
        return;
    }
    ForcingIssues issues;
    checkForcingAgainstNames(meta_, dataNames, issues);
    if (!issues.ok()) throw std::runtime_error(issues.errorText() + "：" + utf8FromPath(index));
    for (const auto& text : issues.warnings) report_.warn(text);
    // record spacing: declared step against the data
    const std::string swRaw = rawName("sw");
    if (!swRaw.empty() && meta_.stepHours > 0) {
        std::map<int64_t, int> hours;
        for (const auto& item : sources_[swRaw]) hours[item.first] = 0;
        const double detected = medianStep(hours);
        if (hours.size() > 1 && std::abs(detected - meta_.stepHours) > 0.01 * meta_.stepHours)
            report_.warn("time.stepHours = " + std::to_string(meta_.stepHours) + " 与数据检测到的间隔 " + std::to_string(detected) + " h 不一致，按声明处理");
    }
    for (const char* key : {"ta", "hum", "p", "u", "v", "speed", "dir", "sw", "lw", "skt", "swvl", "z"}) {
        if (rawName(key).empty()) continue;
        const double step = stepOf(key);
        if (!(step >= 1.0) || std::abs(step - std::round(step)) > 1e-6)
            throw std::runtime_error("网格数据的记录间隔须为整数小时（" + std::string(key) + " 间隔 " + std::to_string(step) + " h），亚小时数据请先重采样");
    }
    fullYear_ = false;
}

const ForcingChannel* Pack::channelOf(const std::string& key) const
{
    if (key == "ta") return &meta_.airTemperature;
    if (key == "hum") return &meta_.humidity;
    if (key == "p") return &meta_.pressure;
    if (key == "u") return &meta_.windU;
    if (key == "v") return &meta_.windV;
    if (key == "speed") return &meta_.windSpeed;
    if (key == "dir") return &meta_.windDirection;
    if (key == "sw") return &meta_.shortwaveDown;
    if (key == "lw") return &meta_.longwaveDown;
    if (key == "skt") return &meta_.skinTemperature;
    if (key == "swvl") return &meta_.soilMoisture;
    if (key == "z") return &meta_.orography;
    return nullptr;
}

std::string Pack::rawName(const std::string& key) const
{
    const ForcingChannel* channel = channelOf(key);
    if (!channel || !channel->declared) return "";
    for (const auto& name : channel->source) if (sources_.count(name)) return name;
    return "";
}

double Pack::stepOf(const std::string& key) const
{
    const ForcingChannel* channel = channelOf(key);
    if (channel && channel->stepHours > 0) return channel->stepHours;
    if (meta_.stepHours > 0) return meta_.stepHours;
    const auto found = sources_.find(rawName(key));
    if (found == sources_.end()) return 1.0;
    std::map<int64_t, int> hours;
    for (const auto& item : found->second) hours[item.first] = 0;
    return medianStep(hours);
}

std::vector<std::string> Pack::channelsFor(const std::vector<std::string>& variables) const
{
    std::set<std::string> keys;
    const auto wind = [&] {
        if (meta_.windType == "components") { keys.insert("u"); keys.insert("v"); }
        else { keys.insert("speed"); if (meta_.windType == "speedDirection") keys.insert("dir"); }
    };
    const auto pressure = [&] {
        if (meta_.pressureType == "surface") keys.insert("p");
        else { keys.insert("z"); if (meta_.pressureType == "seaLevel") { keys.insert("p"); keys.insert("ta"); } }
    };
    const auto dewpoint = [&] {
        keys.insert("hum");
        if (meta_.humidityType != "dewpoint") { keys.insert("ta"); if (meta_.humidityType != "vapourPressure") pressure(); }
    };
    for (const auto& name : variables) {
        if (name == "t2m") keys.insert("ta");
        else if (name == "d2m") dewpoint();
        else if (name == "sp") pressure();
        else if (name == "u10" || name == "v10") wind();
        else if (name == "ssrd") keys.insert("sw");
        else if (name == "strd") {
            if (!rawName("lw").empty()) keys.insert("lw");
            else { keys.insert("ta"); dewpoint(); }
        } else if (name == "skt") keys.insert("skt");
        else if (name == "swvl1") keys.insert("swvl");
        else if (name == "z") keys.insert("z");
    }
    std::vector<std::string> out;
    for (const auto& key : keys) if (!rawName(key).empty()) out.push_back(key);
    return out;
}

bool Pack::channelCovers(const std::string& key, int64_t firstHour, int64_t lastHour, std::string& gap) const
{
    const std::string raw = rawName(key);
    const ForcingChannel* channel = channelOf(key);
    const auto found = sources_.find(raw);
    if (found == sources_.end()) { gap = key + " 不在数据包中"; return false; }
    if (found->second.count(std::numeric_limits<int64_t>::min())) return true;
    const double step = stepOf(key);
    const bool flux = (key == "sw" || key == "lw") && channel->kind != "instant";
    std::vector<double> times;
    for (const auto& item : found->second) times.push_back(static_cast<double>(item.first));
    const auto describe = [&](int64_t hour) { return raw + " 缺少 " + formatUtc(static_cast<double>(hour) * 3600.0) + "（原始记录需包住该时段，记录间隔 " + std::to_string(step) + " h）"; };
    if (flux) {
        for (int64_t hour = firstHour; hour <= lastHour; ++hour) {
            bool covered = false;
            for (const double t : times) {
                const double end = (channel->kind == "previous" || channel->stamp == "interval_start") ? t + step : channel->stamp == "interval_mid" ? t + 0.5 * step : t;
                if (end - step <= hour - 1 + 1e-9 && end >= hour - 1e-9) { covered = true; break; }
            }
            if (!covered) { gap = describe(hour); return false; }
        }
        return true;
    }
    const double maxGap = forcingMaxGap(meta_.maxGapHours, step);
    double before = std::numeric_limits<double>::quiet_NaN(), after = before;
    for (const double t : times) { if (t <= firstHour + 1e-9) before = t; if (t >= lastHour - 1e-9 && !std::isfinite(after)) after = t; }
    if (!std::isfinite(before)) { gap = describe(firstHour); return false; }
    if (!std::isfinite(after)) { gap = describe(lastHour); return false; }
    for (size_t i = 1; i < times.size(); ++i)
        if (times[i] > before + 1e-9 && times[i - 1] < after - 1e-9 && times[i] - times[i - 1] > maxGap + 1e-9) {
            gap = raw + " 在 " + formatUtc(times[i - 1] * 3600.0) + " 到 " + formatUtc(times[i] * 3600.0) + " 之间缺测超过 " + std::to_string(maxGap) + " h";
            return false;
        }
    return true;
}

bool Pack::covers(const std::vector<std::string>& variables, int64_t firstHour, int64_t lastHour, std::string& gap) const
{
    if (generic_) {
        for (const auto& name : variables) {
            if (!available(name)) { gap = name + " 不在数据包中"; return false; }
            for (const auto& key : channelsFor({name}))
                if (!channelCovers(key, firstHour, lastHour, gap)) return false;
        }
        return true;
    }
    for (const auto& name : variables) {
        const auto found = sources_.find(name);
        if (found == sources_.end()) { gap = name + " 不在数据包中"; return false; }
        if (found->second.count(std::numeric_limits<int64_t>::min())) continue;
        for (int64_t hour = firstHour; hour <= lastHour; ++hour)
            if (!found->second.count(hour)) { gap = name + " 缺少 " + formatUtc(static_cast<double>(hour) * 3600.0); return false; }
    }
    return true;
}

int64_t Pack::leadHours() const
{
    if (!generic_) return land_ ? 1 : 0;
    int64_t lead = 0;
    for (const char* key : {"sw", "lw"})
        if (channelOf(key)->kind == "cumulative" && !rawName(key).empty()) lead = std::max<int64_t>(lead, static_cast<int64_t>(std::ceil(stepOf(key))));
    return lead;
}

bool Pack::available(const std::string& variable) const
{
    if (!generic_) return sources_.count(variable) > 0;
    if (variable == "t2m") return !rawName("ta").empty();
    if (variable == "d2m") return !rawName("hum").empty();
    if (variable == "sp") return meta_.pressureType == "none" || !rawName("p").empty();
    if (variable == "u10" || variable == "v10") return meta_.windType == "components" ? !rawName("u").empty() && !rawName("v").empty() : !rawName("speed").empty();
    if (variable == "ssrd") return !rawName("sw").empty();
    if (variable == "strd") return !rawName("lw").empty() || meta_.longwaveFallback == "brutsaert";
    if (variable == "skt") return !rawName("skt").empty();
    if (variable == "swvl1") return !rawName("swvl").empty();
    if (variable == "z") return !rawName("z").empty();
    return false;
}

void Pack::load(const std::vector<std::string>& variables, double latitudeMin, double latitudeMax, double longitudeMin,
                double longitudeMax, int64_t firstHour, int64_t lastHour)
{
    const auto toPack = [this](double longitude) { return longitude360_ && longitude < 0 ? longitude + 360.0 : longitude; };
    const double r0 = (latitudeMin - latitude0_) / latitudeStep_, r1 = (latitudeMax - latitude0_) / latitudeStep_;
    row0_ = std::max(0, static_cast<int>(std::floor(std::min(r0, r1))) - 2);
    const int rowLast = std::min(latitudeCount_ - 1, static_cast<int>(std::ceil(std::max(r0, r1))) + 2);
    rows_ = rowLast - row0_ + 1;
    const double c0 = (toPack(longitudeMin) - longitude0_) / longitudeStep_, c1 = (toPack(longitudeMax) - longitude0_) / longitudeStep_;
    int columnFirst = static_cast<int>(std::floor(std::min(c0, c1))) - 2;
    int columnLast = static_cast<int>(std::ceil(std::max(c0, c1))) + 2;
    if (globalLongitude_ && (columnFirst < 0 || columnLast >= longitudeCount_ || std::abs(c1 - c0) > longitudeCount_ / 2.0)) {
        columnFirst = 0;
        columnLast = longitudeCount_ - 1;
    }
    column0_ = std::max(0, columnFirst);
    columns_ = std::min(longitudeCount_ - 1, columnLast) - column0_ + 1;
    if (rows_ < 1 || columns_ < 1 || std::min(r0, r1) > latitudeCount_ + 1 || std::max(r0, r1) < -2 ||
        (!globalLongitude_ && (std::min(c0, c1) > longitudeCount_ + 1 || std::max(c0, c1) < -2)))
        throw std::runtime_error("影像范围不在气象数据包的覆盖范围内");
    firstHour_ = firstHour;
    lastHour_ = lastHour;
    if (generic_) { loadGeneric(variables, firstHour, lastHour); return; }

    const size_t frame = static_cast<size_t>(rows_) * columns_;
    const Names names;
    std::map<int, std::unique_ptr<NcFile>> opened;
    const auto file = [&](int position) -> const NcFile& {
        auto& slot = opened[position];
        if (!slot) slot = std::make_unique<NcFile>(pathFromUtf8(files_[position]));
        return *slot;
    };
    for (const auto& name : variables) {
        const auto found = sources_.find(name);
        if (found == sources_.end()) throw std::runtime_error("气象数据包缺少变量 " + name);
        const auto& map = found->second;
        auto invariantSource = map.find(std::numeric_limits<int64_t>::min());
        if (invariantSource != map.end()) {
            auto& values = data_[name];
            values.assign(frame, kMissing);
            const NcFile& nc = file(invariantSource->second.file);
            readFrame(nc, layoutOf(nc, name, names), invariantSource->second.time, row0_, rows_, column0_, columns_, values.data());
            continue;
        }
        auto& values = data_[name];
        values.assign(static_cast<size_t>(lastHour - firstHour + 1) * frame, kMissing);
        auto& exact = exact_[name];
        exact.assign(static_cast<size_t>(lastHour - firstHour + 1), 1);
        std::map<int, Layout> layouts;
        for (int64_t hour = firstHour; hour <= lastHour; ++hour) {
            auto source = map.find(hour);
            if (source == map.end()) {
                exact[static_cast<size_t>(hour - firstHour)] = 0;
                auto next = map.lower_bound(hour);
                auto best = map.end();
                if (next != map.end() && next->first - hour <= 72) best = next;
                if (next != map.begin()) {
                    auto previous = std::prev(next);
                    if (previous->first != std::numeric_limits<int64_t>::min() && hour - previous->first <= 72 &&
                        (best == map.end() || hour - previous->first < best->first - hour))
                        best = previous;
                }
                if (best == map.end()) throw std::runtime_error("气象数据包缺少 " + name + " 在 " + formatUtc(hour * 3600.0) + " 附近的数据");
                notes_.push_back(name + " 在 " + formatUtc(hour * 3600.0) + " 使用相邻时次 " + formatUtc(best->first * 3600.0));
                source = best;
            }
            const NcFile& nc = file(source->second.file);
            auto layout = layouts.find(source->second.file);
            if (layout == layouts.end()) layout = layouts.emplace(source->second.file, layoutOf(nc, name, names)).first;
            readFrame(nc, layout->second, source->second.time, row0_, rows_, column0_, columns_,
                      values.data() + static_cast<size_t>(hour - firstHour) * frame);
        }
    }
}

void Pack::loadGeneric(const std::vector<std::string>& variables, int64_t firstHour, int64_t lastHour)
{
    constexpr double kNotANumber = std::numeric_limits<double>::quiet_NaN();
    const Names names = namesOf(meta_);
    const size_t frame = static_cast<size_t>(rows_) * columns_;
    const size_t hours = static_cast<size_t>(lastHour - firstHour + 1);
    std::map<int, std::unique_ptr<NcFile>> opened;
    const auto file = [&](int position) -> const NcFile& {
        auto& slot = opened[position];
        if (!slot) slot = std::make_unique<NcFile>(pathFromUtf8(files_[position]));
        return *slot;
    };
    const auto cellPosition = [&](size_t cell, double& latitude, double& longitude) {
        latitude = latitude0_ + (row0_ + static_cast<double>(cell / columns_)) * latitudeStep_;
        longitude = longitude0_ + (column0_ + static_cast<double>(cell % columns_)) * longitudeStep_;
        while (longitude > 180.0) longitude -= 360.0;
        while (longitude < -180.0) longitude += 360.0;
    };
    std::map<std::string, std::vector<double>> hourly;    // key -> [hour][cell] SI (fluxes: W m-2 of the hour)
    std::map<std::string, std::vector<double>> constant;  // key -> [cell]
    for (const auto& key : channelsFor(variables)) {
        const ForcingChannel& channel = *channelOf(key);
        const std::string raw = rawName(key);
        const auto& map = sources_.at(raw);
        long long removed = 0;
        const auto convert = [&](float input) {
            if (!std::isfinite(input)) return kNotANumber;
            double value = input;
            for (const double marker : channel.missing)
                if (std::abs(value - marker) <= 1e-9 * std::max(1.0, std::abs(marker))) { ++removed; return kNotANumber; }
            value = value * channel.scale + channel.offset;
            if (channel.hasRange && (value < channel.rangeMin || value > channel.rangeMax)) { ++removed; return kNotANumber; }
            return value * channel.unitFactor + channel.unitOffset;
        };
        std::map<int, Layout> layouts;
        const auto read = [&](const Source& source, std::vector<float>& out) {
            const NcFile& nc = file(source.file);
            auto layout = layouts.find(source.file);
            if (layout == layouts.end()) layout = layouts.emplace(source.file, layoutOf(nc, raw, names)).first;
            out.assign(frame, kMissing);
            readFrame(nc, layout->second, source.time, row0_, rows_, column0_, columns_, out.data());
        };
        if (const auto invariant = map.find(std::numeric_limits<int64_t>::min()); invariant != map.end()) {
            std::vector<float> frameValues;
            read(invariant->second, frameValues);
            auto& values = constant[key];
            values.resize(frame);
            for (size_t cell = 0; cell < frame; ++cell) values[cell] = convert(frameValues[cell]);
            if (removed) report_.removed[key] += removed;
            continue;
        }
        const double step = stepOf(key);
        const double maxGap = forcingMaxGap(meta_.maxGapHours, step);
        const double pad = std::max(maxGap, 2.0 * step) + 1.0;
        std::vector<double> times;
        std::vector<std::vector<float>> frames;
        for (const auto& item : map) {
            if (item.first == std::numeric_limits<int64_t>::min()) continue;
            if (item.first < firstHour - pad || item.first > lastHour + pad) continue;
            times.push_back(static_cast<double>(item.first));
            frames.emplace_back();
            read(item.second, frames.back());
        }
        const bool fluxChannel = key == "sw" || key == "lw";
        const bool intervals = fluxChannel && channel.kind != "instant";
        auto& output = hourly[key];
        output.assign(hours * frame, kNotANumber);
        std::vector<double> series(times.size()), shifted(times.size()), result;
        for (size_t cell = 0; cell < frame; ++cell) {
            bool any = false;
            for (size_t r = 0; r < times.size(); ++r) { series[r] = convert(frames[r][cell]); any = any || std::isfinite(series[r]); }
            if (!any) continue;
            if (intervals) {
                double latitude, longitude;
                cellPosition(cell, latitude, longitude);
                distributeFlux(fluxRecordsFrom(channel, step, times, series), key == "sw" ? FluxShape::Solar : FluxShape::Smooth, latitude, longitude,
                               firstHour, lastHour, result);
            } else if (fluxChannel) {  // instantaneous irradiance: the hour (h - 1, h] takes the value at h - 0.5
                for (size_t r = 0; r < times.size(); ++r) shifted[r] = times[r] + 0.5;
                resampleInstant(shifted, series, firstHour, lastHour, maxGap, result);
            } else if (channel.kind == "previous") {  // zero-order hold
                result.assign(hours, kNotANumber);
                for (int64_t hour = firstHour; hour <= lastHour; ++hour) {
                    for (size_t r = times.size(); r-- > 0;)
                        if (times[r] <= hour + 1e-9) { if (hour - times[r] <= maxGap + 1e-9) result[hour - firstHour] = series[r]; break; }
                }
            } else resampleInstant(times, series, firstHour, lastHour, maxGap, result);
            for (size_t h = 0; h < hours; ++h) {
                double value = result[h];
                if (intervals || fluxChannel) value = std::isfinite(value) ? std::max(value, 0.0) : kNotANumber;
                output[h * frame + cell] = value;
            }
        }
        if (removed) report_.removed[key] += removed;
    }
    const auto value = [&](const std::string& key, size_t hour, size_t cell) -> double {
        if (const auto found = hourly.find(key); found != hourly.end()) return found->second[hour * frame + cell];
        if (const auto found = constant.find(key); found != constant.end()) return found->second[cell];
        return kNotANumber;
    };
    const auto altitude = [&](size_t hour, size_t cell) { return value("z", hour, cell) / kForcingStandardGravity; };
    const auto temperature = [&](size_t hour, size_t cell) { return value("ta", hour, cell) + meta_.temperatureShiftK; };
    bool pressureWarned = false;
    const auto pressure = [&](size_t hour, size_t cell) -> double {
        if (meta_.pressureType == "surface") return value("p", hour, cell);
        const double height = altitude(hour, cell);
        if (meta_.pressureType == "seaLevel") return std::isfinite(height) ? forcingPressureFromSeaLevel(value("p", hour, cell), height, temperature(hour, cell)) : kNotANumber;
        if (std::isfinite(height)) return forcingPressureFromAltitude(height);
        if (!pressureWarned) { pressureWarned = true; report_.warn("没有气压且没有站点/格点海拔，地面气压取 1013.25 hPa"); }
        return 101325.0;
    };
    const auto dewpoint = [&](size_t hour, size_t cell) -> double {
        const double humidity = value("hum", hour, cell);
        if (meta_.humidityType == "dewpoint") return humidity;
        double ea = kNotANumber;  // hPa
        if (meta_.humidityType == "vapourPressure") ea = humidity / 100.0;
        else if (meta_.humidityType == "relative") ea = humidity * forcingSaturationVapourPressure(temperature(hour, cell) - 273.15);
        else if (meta_.humidityType == "specific") ea = forcingVapourPressureFromSpecific(humidity, pressure(hour, cell));
        else ea = forcingVapourPressureFromMixingRatio(humidity, pressure(hour, cell));
        return forcingDewPointFromVapourPressure(ea);
    };
    const auto store = [&](const std::string& name, const auto& compute) {
        auto& out = data_[name];
        out.assign(hours * frame, kMissing);
        for (size_t h = 0; h < hours; ++h)
            for (size_t cell = 0; cell < frame; ++cell) {
                const double v = compute(h, cell);
                if (std::isfinite(v)) out[h * frame + cell] = static_cast<float>(v);
            }
        exact_[name].assign(hours, 1);
    };
    const auto wind = [&](size_t hour, size_t cell, bool northward) -> double {
        double u, v;
        if (meta_.windType == "components") { u = value("u", hour, cell); v = value("v", hour, cell); }
        else {
            const double speed = value("speed", hour, cell);
            if (meta_.windType == "speed") { u = speed; v = 0.0; }
            else {
                const double direction = value("dir", hour, cell) * 3.14159265358979323846 / 180.0;
                const double sign = meta_.windConvention == "from" ? -1.0 : 1.0;
                u = sign * speed * std::sin(direction);
                v = sign * speed * std::cos(direction);
            }
        }
        const double result = northward ? v : u;
        return meta_.windFactor != 1.0 ? result * meta_.windFactor : result;
    };
    for (const auto& name : variables) {
        if (name == "t2m") store(name, [&](size_t h, size_t c) { return temperature(h, c); });
        else if (name == "sp") {
            store(name, pressure);
            if (meta_.pressureType != "surface") report_.derived.push_back(meta_.pressureType == "seaLevel" ? "sp 由海平面气压 + 海拔换算" : "sp 由海拔按标准大气估算");
        } else if (name == "d2m") {
            store(name, dewpoint);
            if (meta_.humidityType != "dewpoint") report_.derived.push_back("d2m 由 " + meta_.humidityType + " 湿度换算");
        } else if (name == "u10") store(name, [&](size_t h, size_t c) { return wind(h, c, false); });
        else if (name == "v10") store(name, [&](size_t h, size_t c) { return wind(h, c, true); });
        else if (name == "ssrd") store(name, [&](size_t h, size_t c) { return value("sw", h, c); });
        else if (name == "strd") {
            if (!rawName("lw").empty()) store(name, [&](size_t h, size_t c) { return value("lw", h, c); });
            else {
                report_.warn("没有向下长波，按 Brutsaert 晴空参数化（不含云，阴天偏低）");
                report_.derived.push_back("strd 由 Brutsaert 晴空参数化估算");
                store(name, [&](size_t h, size_t c) {
                    const double ta = temperature(h, c), td = dewpoint(h, c);
                    return std::isfinite(ta) && std::isfinite(td) ? forcingBrutsaertLongwave(ta, forcingSaturationVapourPressure(td - 273.15)) : kNotANumber;
                });
            }
        } else if (name == "skt") store(name, [&](size_t h, size_t c) { return value("skt", h, c); });
        else if (name == "swvl1") store(name, [&](size_t h, size_t c) { return value("swvl", h, c); });
        else if (name == "z") {
            auto& out = data_[name];
            out.assign(frame, kMissing);
            const auto found = constant.find("z");
            if (found != constant.end())
                for (size_t cell = 0; cell < frame; ++cell) if (std::isfinite(found->second[cell])) out[cell] = static_cast<float>(found->second[cell]);
        } else throw std::runtime_error("未知的标准强迫量 " + name);
    }
}

Weights Pack::weights(double latitude, double longitude) const
{
    if (longitude360_ && longitude < 0) longitude += 360.0;
    double fr = (latitude - latitude0_) / latitudeStep_ - row0_;
    double fc = (longitude - longitude0_) / longitudeStep_ - column0_;
    const bool wrap = globalLongitude_ && columns_ == longitudeCount_;
    if (wrap) fc = std::fmod(std::fmod(fc, columns_) + columns_, columns_);
    fr = std::clamp(fr, 0.0, static_cast<double>(rows_ - 1));
    if (!wrap) fc = std::clamp(fc, 0.0, static_cast<double>(columns_ - 1));
    const int r = std::min(static_cast<int>(std::floor(fr)), rows_ - 1), c = std::min(static_cast<int>(std::floor(fc)), columns_ - 1);
    const int r1 = std::min(r + 1, rows_ - 1);
    const int c1 = wrap ? (c + 1) % columns_ : std::min(c + 1, columns_ - 1);
    const float dr = static_cast<float>(fr - r), dc = static_cast<float>(fc - c);
    Weights w;
    w.index[0] = r * columns_ + c;   w.weight[0] = (1 - dr) * (1 - dc);
    w.index[1] = r * columns_ + c1;  w.weight[1] = (1 - dr) * dc;
    w.index[2] = r1 * columns_ + c;  w.weight[2] = dr * (1 - dc);
    w.index[3] = r1 * columns_ + c1; w.weight[3] = dr * dc;
    return w;
}

float Pack::sample(const std::vector<float>& values, size_t offset, const Weights& weights) const
{
    double sum = 0, total = 0;
    for (int k = 0; k < 4; ++k) {
        const float value = values[offset + weights.index[k]];
        if (!std::isfinite(value)) continue;
        sum += weights.weight[k] * value;
        total += weights.weight[k];
    }
    if (total > 1e-6) return static_cast<float>(sum / total);
    for (int k = 0; k < 4; ++k) if (std::isfinite(values[offset + weights.index[k]])) return values[offset + weights.index[k]];
    return kMissing;
}

float Pack::at(const std::string& variable, int64_t hour, const Weights& weights) const
{
    const auto found = data_.find(variable);
    if (found == data_.end()) return kMissing;
    const size_t frame = static_cast<size_t>(rows_) * columns_;
    if (found->second.size() == frame) return sample(found->second, 0, weights);
    const int64_t clamped = std::clamp(hour, firstHour_, lastHour_);
    return sample(found->second, static_cast<size_t>(clamped - firstHour_) * frame, weights);
}

float Pack::hourlyFlux(const std::string& variable, int64_t hour, const Weights& weights) const
{
    hour = std::clamp(hour, firstHour_, lastHour_);
    if (generic_) {
        const float value = at(variable, hour, weights);
        return std::isfinite(value) ? std::max(0.0f, value) : kMissing;
    }
    if (!land_) {
        const float current = at(variable, hour, weights);
        if (!std::isfinite(current)) return kMissing;
        return std::max(0.0f, current / 3600.0f);
    }
    // ERA5-Land accumulates from 00 UTC: the hour ending at 01 UTC is the accumulation itself, every other
    // hour the difference to the previous one. Differences need two hours read from the pack, so hours
    // next to filled-in ones (outside the pack's period) use the nearest hour that can be de-accumulated.
    const auto found = exact_.find(variable);
    const auto hourOfDay = [](int64_t h) { return ((h % 24) + 24) % 24; };
    const auto usable = [&](int64_t h) {
        if (h < firstHour_ || h > lastHour_) return false;
        if (found == exact_.end()) return true;
        if (!found->second[static_cast<size_t>(h - firstHour_)]) return false;
        return hourOfDay(h) == 1 || (h - 1 >= firstHour_ && found->second[static_cast<size_t>(h - 1 - firstHour_)]);
    };
    int64_t chosen = hour;
    for (int64_t distance = 1; !usable(chosen) && distance <= lastHour_ - firstHour_; ++distance) {
        if (usable(hour - distance)) chosen = hour - distance;
        else if (usable(hour + distance)) chosen = hour + distance;
    }
    const float current = at(variable, chosen, weights);
    if (!std::isfinite(current)) return kMissing;
    if (hourOfDay(chosen) == 1) return std::max(0.0f, current / 3600.0f);
    const float previous = at(variable, chosen - 1, weights);
    if (!std::isfinite(previous)) return kMissing;
    return std::max(0.0f, (current - previous) / 3600.0f);
}

bool registerForcingPack(const std::vector<std::string>& files, const fs::path& index, const ForcingMetadata& meta, const std::string& metadataSpec)
{
    if (files.empty()) throw std::runtime_error("请提供气象强迫 NetCDF 文件");
    if (meta.station()) throw std::runtime_error("气象强迫元数据 \"" + meta.name + "\" 是站点形态，站点 CSV 无需登记，直接写进 temperature.forcing.files");
    const Names names = namesOf(meta);
    const std::vector<std::string> wanted = wantedOf(meta);
    Axis latitude, longitude;
    std::map<std::string, std::set<int64_t>> hours;
    std::vector<std::string> dataNames;
    CPLJSONArray fileList;
    for (size_t position = 0; position < files.size(); ++position) {
        const fs::path path = pathFromUtf8(files[position]);
        if (!fs::is_regular_file(path)) throw std::runtime_error("气象强迫文件不存在：" + files[position]);
        const NcFile file(path);
        const FileScan scan = scanFile(file, names, wanted);
        if (position == 0) { latitude = scan.latitude; longitude = scan.longitude; }
        else if (!sameAxis(latitude, scan.latitude) || !sameAxis(longitude, scan.longitude))
            throw std::runtime_error("气象强迫文件的经纬度网格不一致：" + files[position]);
        for (const auto& name : scan.allNames) if (std::find(dataNames.begin(), dataNames.end(), name) == dataNames.end()) dataNames.push_back(name);
        for (const auto& name : scan.variables) if (!scan.invariant.count(name)) for (const int64_t hour : scan.hours) hours[name].insert(hour);
        CPLJSONObject entry;
        const fs::path absolute = fs::absolute(path);
        const fs::path local = absolute.lexically_relative(fs::absolute(index).parent_path());
        const bool inside = !local.empty() && *local.begin() != "..";
        entry.Add("path", utf8FromPath(inside ? local : absolute));
        entry.Add("bytes", static_cast<GInt64>(fs::file_size(path)));
        CPLJSONArray variableNames;
        for (const auto& name : scan.variables) variableNames.Add(name);
        entry.Add("variables", variableNames);
        entry.Add("timeSteps", static_cast<GInt64>(scan.hours.size()));
        fileList.Add(entry);
        std::cout << "STREAM\tprogress\t" << (100.0 * (position + 1) / files.size()) << "\t扫描气象强迫文件\n";
    }
    ForcingIssues issues;
    checkForcingAgainstNames(meta, dataNames, issues);
    if (!issues.ok()) throw std::runtime_error(issues.errorText());
    for (const auto& text : issues.warnings) std::cout << "[STREAM] 警告：" << text << "\n";
    int64_t periodFirst = std::numeric_limits<int64_t>::min(), periodLast = std::numeric_limits<int64_t>::max();
    for (const ForcingChannel* c : {&meta.airTemperature, &meta.humidity, &meta.shortwaveDown}) {
        for (const auto& name : c->source) {
            const auto found = hours.find(name);
            if (found == hours.end() || found->second.empty()) continue;
            periodFirst = std::max(periodFirst, *found->second.begin());
            periodLast = std::min(periodLast, *found->second.rbegin());
            break;
        }
    }
    if (periodFirst > periodLast || periodFirst == std::numeric_limits<int64_t>::min()) throw std::runtime_error("气象强迫必需变量的时间范围没有重叠");
    int year; unsigned month, day;
    civilFromDays(periodFirst >= 0 ? periodFirst / 24 : (periodFirst - 23) / 24, year, month, day);
    CPLJSONObject root, grid, latitudeItem, longitudeItem, period;
    root.Add("kind", "stream-forcing-pack");
    root.Add("schemaVersion", 1);
    root.Add("product", meta.name);
    root.Add("metadata", metadataSpec);
    root.Add("stepHours", meta.stepHours);
    root.Add("year", year);
    root.Add("created", isoNow());
    latitudeItem.Add("first", latitude.first); latitudeItem.Add("step", latitude.step); latitudeItem.Add("count", latitude.count);
    longitudeItem.Add("first", longitude.first); longitudeItem.Add("step", longitude.step); longitudeItem.Add("count", longitude.count);
    grid.Add("latitude", latitudeItem);
    grid.Add("longitude", longitudeItem);
    root.Add("grid", grid);
    period.Add("start", formatUtc(static_cast<double>(periodFirst) * 3600.0));
    period.Add("end", formatUtc(static_cast<double>(periodLast) * 3600.0));
    period.Add("hours", static_cast<GInt64>(periodLast - periodFirst + 1));
    root.Add("period", period);
    root.Add("complete", true);
    root.Add("files", fileList);
    CPLJSONDocument document;
    document.SetRoot(root);
    if (!document.Save(utf8FromPath(index))) throw std::runtime_error("无法写出气象数据包索引：" + utf8FromPath(index));
    std::cout << "[STREAM] 气象数据包：" << meta.name << " " << formatUtc(static_cast<double>(periodFirst) * 3600.0) << " – "
              << formatUtc(static_cast<double>(periodLast) * 3600.0) << "（原始记录），" << files.size() << " 个文件\n";
    return true;
}

}  // namespace stream::era5