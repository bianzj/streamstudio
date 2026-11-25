// ==========================================================================
// 文件：station_source.cpp
// 功能：站点 CSV 气象强迫源（forcing 元数据 form = station）：读取、标准化并按站点插值
// 主要模块：
//   1. CSV 与时间工具：trim / splitLine / readLines / parseTime（时间戳 -> 自 1970 起的小时数）
//   2. 几何与统计工具：distanceKm（球面距离）、medianSpacing（记录间隔中位数）
//   3. StationSource 构造：读多个 CSV，按站点 id 分组，应用缺测/范围/比例/单位换算，
//      检查坐标与时间覆盖，建立每站每通道的时间序列（Series）
//   4. covers / available / leadHours：覆盖检查与可用标准量判定
//   5. load：把各站各通道重采样到逐小时（瞬时插值、零阶保持、通量守恒分配），
//      再交 standardiseQuantities 换算为 ERA5 命名的 SI 标准量
//   6. weights / at / hourlyFlux：按最近站或距离反比（IDW，最多 4 站）取值
// 数据流：config 提供元数据与 CSV 路径 → 本类 → pixel_forcing 按像元取 weights() 与 at()/hourlyFlux()
//         → EB 求解；标准量 t2m d2m sp u10 v10 ssrd strd（可选 skt swvl1 z）见 forcing_source.h
// 关系：规格见 docs/forcing-metadata.md 第 3.3、4、6 节；重采样函数在 forcing_resample.*；
//       元数据解析与校验在 forcing_metadata.*
// 约定：时间为自 1970-01-01 UTC 起的整小时（double 小时用于原始记录）；
//       站点内部通道键 ta hum p u v speed dir sw lw skt swvl（见 kKeys）；
//       通量（sw、lw）重采样后不小于 0；缺测一律为 NaN，插值时按剩余有效站点重新归一
// ==========================================================================

#include "station_source.h"

#include "forcing_resample.h"
#include "utc.h"
#include "../scene/raster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace stream {
namespace {

// 常数：kNotANumber 表示缺测。
constexpr double kNotANumber = std::numeric_limits<double>::quiet_NaN();
namespace fs = std::filesystem;

// 模块：CSV 文本工具。trim 去首尾空白；splitLine 按分隔符切分并支持双引号（"" 转义）；
//       readLines 按二进制读入，去 UTF-8 BOM 与行尾 CR。
std::string trim(const std::string& text)
{
    size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r' || text[a] == '\n')) ++a;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r' || text[b - 1] == '\n')) --b;
    return text.substr(a, b - a);
}

std::vector<std::string> splitLine(const std::string& line, char delimiter)
{
    std::vector<std::string> fields;
    std::string current;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') { if (quoted && i + 1 < line.size() && line[i + 1] == '"') { current += '"'; ++i; } else quoted = !quoted; }
        else if (c == delimiter && !quoted) { fields.push_back(trim(current)); current.clear(); }
        else current += c;
    }
    fields.push_back(trim(current));
    return fields;
}

std::vector<std::string> readLines(const fs::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("无法打开气象站 CSV：" + utf8FromPath(file));
    std::vector<std::string> lines;
    std::string line;
    bool first = true;
    while (std::getline(stream, line)) {
        if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // UTF-8 byte order mark
        first = false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

// 模块：时间戳解析 -> 自 1970-01-01 UTC 起的小时数（double）。
// 输入：text、元数据的 time.format、offsetHours（元数据 utcOffsetHours，本地时间 = UTC + offset）。
// 说明：支持 epoch_seconds / epoch_milliseconds、yyyymmddhh(mm) 与 ISO 风格
//       "YYYY-MM-DD[ T]HH:MM[:SS][Z|±hh[:mm]]"；字符串自带的时区优先于配置的偏移，
//       无时区时按 offsetHours 换算为 UTC；字段超范围或格式不符返回 false。
// Time stamp -> hours since 1970-01-01 UTC.
bool parseTime(const std::string& text, const std::string& format, double offsetHours, double& hours)
{
    const auto seconds = [&](int y, int mo, int d, int h, int mi, double s) {
        if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 24 || mi < 0 || mi > 59 || s < 0 || s >= 61) return false;
        hours = (static_cast<double>(daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d))) * 86400.0 + h * 3600.0 + mi * 60.0 + s) / 3600.0 - offsetHours;
        return true;
    };
    if (format == "epoch_seconds" || format == "epoch_milliseconds") {
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || *end != '\0') return false;
        hours = value / (format == "epoch_seconds" ? 3600.0 : 3600000.0);
        return true;
    }
    if (format == "yyyymmddhh" || format == "yyyymmddhhmm") {
        const size_t digits = format == "yyyymmddhh" ? 10 : 12;
        if (text.size() != digits || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
        const auto part = [&](size_t at, size_t n) { return std::atoi(text.substr(at, n).c_str()); };
        return seconds(part(0, 4), part(4, 2), part(6, 2), part(8, 2), digits == 12 ? part(10, 2) : 0, 0);
    }
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, used = 0;
    double s = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d%n", &y, &mo, &d, &used) != 3) return false;
    size_t pos = static_cast<size_t>(used);
    if (pos >= text.size() || (text[pos] != ' ' && text[pos] != 'T')) return false;
    ++pos;
    if (std::sscanf(text.c_str() + pos, "%d:%d%n", &h, &mi, &used) != 2) return false;
    pos += static_cast<size_t>(used);
    if (pos < text.size() && text[pos] == ':') {
        int n = 0;
        if (std::sscanf(text.c_str() + pos + 1, "%lf%n", &s, &n) != 1) return false;
        pos += 1 + static_cast<size_t>(n);
    }
    double zone = offsetHours;
    if (pos < text.size()) {
        const std::string rest = trim(text.substr(pos));
        if (rest == "Z" || rest == "z") zone = 0;
        else if (rest[0] == '+' || rest[0] == '-') {
            int zh = 0, zm = 0;
            const int parts = std::sscanf(rest.c_str() + 1, "%d:%d", &zh, &zm);
            if (parts < 1) { if (std::sscanf(rest.c_str() + 1, "%2d%2d", &zh, &zm) < 1) return false; }
            zone = (rest[0] == '-' ? -1.0 : 1.0) * (zh + zm / 60.0);
        } else return false;
    }
    if (!seconds(y, mo, d, h, mi, s)) return false;
    hours += offsetHours - zone;  // `seconds` subtracted the configured offset; the string's own zone wins
    return true;
}

// 作用：球面（haversine）距离（km），地球半径 6371.0088 km。
double distanceKm(double lat1, double lon1, double lat2, double lon2)
{
    const double rad = 3.14159265358979323846 / 180.0;
    const double a = std::sin((lat2 - lat1) * rad / 2), b = std::sin((lon2 - lon1) * rad / 2);
    const double h = a * a + std::cos(lat1 * rad) * std::cos(lat2 * rad) * b * b;
    return 2.0 * 6371.0088 * std::asin(std::min(1.0, std::sqrt(h)));
}

// 作用：相邻记录时间差（小时）的中位数；记录少于 2 条时返回 1。用于检测记录步长。
double medianSpacing(const std::vector<double>& times)
{
    std::vector<double> steps;
    for (size_t i = 1; i < times.size(); ++i) steps.push_back(times[i] - times[i - 1]);
    if (steps.empty()) return 1.0;
    std::sort(steps.begin(), steps.end());
    return steps[steps.size() / 2];
}

// 常数：站点通道键，依次为气温、湿度、气压、风 u/v 分量、风速、风向、向下短波、向下长波、
//       地表温度、土壤体积含水量；与元数据中的 ForcingChannel 一一对应（见 channelOf）。
const char* const kKeys[] = {"ta", "hum", "p", "u", "v", "speed", "dir", "sw", "lw", "skt", "swvl"};

}  // namespace

// 作用：由通道键取元数据里的通道描述（列名、单位、kind、缺测标记、范围等）；未知键返回空。
const ForcingChannel* StationSource::channelOf(const std::string& key) const
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
    return nullptr;
}

// 作用：读取 CSV 表头列名（跳过空行与注释行），供 forcing-check 不加载数据时校验列名。
std::vector<std::string> StationSource::readHeader(const fs::path& file, const ForcingMetadata& metadata)
{
    for (const auto& line : readLines(file)) {
        const std::string text = trim(line);
        if (text.empty() || (!metadata.comment.empty() && text.rfind(metadata.comment, 0) == 0)) continue;
        return splitLine(line, metadata.delimiter.empty() ? ',' : metadata.delimiter[0]);
    }
    return {};
}

// 模块：构造，读入并校验全部站点 CSV；任何问题抛 std::runtime_error（标准提示信息）。
// 步骤 1：检查元数据为站点形态且至少有一个文件，继承元数据的警告/说明/忽略项。
// 步骤 2：逐文件找表头、定位时间/站点/经纬度/海拔列和各通道的源列。
// 步骤 3：逐数据行解析时间与站点 id，登记新站点，记录坐标，并把各通道的值转换后追加到序列：
//         缺测标记与范围外的值丢弃并计数，之后 raw·scale + offset，再乘单位换算到 SI。
// 步骤 4：用元数据校验 CSV 列名（checkForcingAgainstNames），有错误则抛出。
// 步骤 5：检查每个站点经纬度已知、纬度在 ±90°，各通道按时间稳定排序（重复时间戳后者覆盖）。
// 步骤 6：由最早记录确定 year_；没有海拔且气压不是地面类型时警告；
//         声明的 stepHours 与短波检测到的间隔相差超过 1 % 时警告（按声明处理）。
StationSource::StationSource(const ForcingMetadata& metadata, const std::vector<fs::path>& files) : meta_(metadata)
{
    if (!meta_.station()) throw std::runtime_error("气象强迫元数据 \"" + meta_.name + "\" 不是站点形态");
    if (files.empty()) throw std::runtime_error("站点形态的气象强迫需要至少一个 CSV 文件（temperature.forcing.files）");
    for (const auto& text : meta_.warnings) report_.warn(text);
    for (const auto& text : meta_.notes) report_.info(text);
    for (const auto& name : meta_.ignored) report_.ignored.push_back(name);
    const char delimiter = meta_.delimiter.empty() ? ',' : meta_.delimiter[0];
    std::map<std::string, size_t> index;  // station id -> position
    std::vector<std::string> allHeaders;
    std::map<std::string, std::string> idFile;
    std::map<std::string, std::vector<std::pair<double, double>>> located;  // from columns: first valid latitude/longitude
    struct Pending { bool lat = false, lon = false, elev = false; };
    std::map<std::string, Pending> pending;
    // 步骤 2：逐文件处理。站点 id 缺省取文件名（不含扩展名），id 必须在全部文件中唯一。
    for (const auto& path : files) {
        if (!fs::is_regular_file(path)) throw std::runtime_error("气象站 CSV 不存在：" + utf8FromPath(path));
        const std::string fileName = utf8FromPath(path.filename());
        const std::string stem = utf8FromPath(path.stem());
        const auto lines = readLines(path);
        size_t row = 0;
        std::vector<std::string> header;
        for (; row < lines.size(); ++row) {
            const std::string text = trim(lines[row]);
            if (text.empty() || (!meta_.comment.empty() && text.rfind(meta_.comment, 0) == 0)) continue;
            header = splitLine(lines[row], delimiter);
            ++row;
            break;
        }
        if (header.empty()) throw std::runtime_error("气象站 CSV 没有表头行：" + utf8FromPath(path));
        for (const auto& name : header) if (std::find(allHeaders.begin(), allHeaders.end(), name) == allHeaders.end()) allHeaders.push_back(name);
        const auto column = [&](const std::string& name) -> int {
            if (name.empty()) return -1;
            const auto found = std::find(header.begin(), header.end(), name);
            return found == header.end() ? -1 : static_cast<int>(found - header.begin());
        };
        // 步骤 2（续）：定位列；找不到时间列直接报错并列出该文件的全部列名。
        //         每个已声明的通道取第一个在表头中存在的源列名。
        const int timeColumn = column(meta_.timeColumn);
        if (timeColumn < 0) throw std::runtime_error("气象强迫数据找不到时间列 \"" + meta_.timeColumn + "\"（time.column）；" + fileName + " 的列：" + [&] { std::string s; for (const auto& n : header) s += (s.empty() ? "" : "、") + n; return s; }());
        const int idColumn = column(meta_.idColumn), latColumn = column(meta_.latitudeColumn), lonColumn = column(meta_.longitudeColumn), elevColumn = column(meta_.elevationColumn);
        std::map<std::string, int> keyColumn;
        for (const char* key : kKeys) {
            const ForcingChannel* channel = channelOf(key);
            if (!channel->declared) continue;
            for (const auto& name : channel->source) if (column(name) >= 0) { keyColumn[key] = column(name); break; }
        }
        // 步骤 3：逐数据行。removed 统计被 missing/range 剔除的值，最后合并进运行报告。
        std::map<std::string, long long> removed;
        for (; row < lines.size(); ++row) {
            const std::string text = trim(lines[row]);
            if (text.empty() || (!meta_.comment.empty() && text.rfind(meta_.comment, 0) == 0)) continue;
            const auto fields = splitLine(lines[row], delimiter);
            if (static_cast<int>(fields.size()) <= timeColumn) continue;
            double hours = 0;
            if (!parseTime(fields[timeColumn], meta_.timeFormat, meta_.utcOffsetHours, hours))
                throw std::runtime_error("气象强迫数据 " + fileName + " 第 " + std::to_string(row + 1) + " 行时间 \"" + fields[timeColumn] + "\" 无法按 " + meta_.timeFormat + " 解析");
            const std::string id = idColumn >= 0 && static_cast<int>(fields.size()) > idColumn && !fields[idColumn].empty() ? fields[idColumn] : stem;
            const auto known = idFile.find(id);
            if (known != idFile.end() && known->second != fileName) throw std::runtime_error("站点 id \"" + id + "\" 同时出现在 " + known->second + " 和 " + fileName + "，站点 id 在全部文件中必须唯一");
            idFile[id] = fileName;
            // 新站点：先在元数据的站点登记表（stations）里按 id 或文件名匹配取经纬度与海拔；
            //       pending 记录各站点的经度、纬度、海拔是否已知。
            auto position = index.find(id);
            if (position == index.end()) {
                position = index.emplace(id, stations_.size()).first;
                Station station;
                station.id = id;
                station.file = fileName;
                for (const auto& entry : meta_.stations) {
                    if (entry.id == id || (entry.id.empty() && !entry.file.empty() && utf8FromPath(fs::path(entry.file).stem()) == id)) {
                        station.latitude = entry.latitude; station.longitude = entry.longitude;
                        station.elevation = entry.elevation_m; station.hasElevation = entry.hasElevation;
                        pending[id].lat = pending[id].lon = true;
                        pending[id].elev = entry.hasElevation;
                    }
                }
                stations_.push_back(station);
            }
            Station& station = stations_[position->second];
            Pending& known2 = pending[id];
            const auto number = [&](int col, double& out) {
                if (col < 0 || static_cast<int>(fields.size()) <= col || fields[col].empty()) return false;
                char* end = nullptr;
                out = std::strtod(fields[col].c_str(), &end);
                return end != fields[col].c_str() && *end == '\0' && std::isfinite(out);
            };
            // 登记表没有给出时，用 CSV 行内的 lat/lon/elevation 列的第一个有效值（同一站点只取一次）。
            double value = 0;
            if (!known2.lat && number(latColumn, value)) { station.latitude = value; known2.lat = true; }
            if (!known2.lon && number(lonColumn, value)) { station.longitude = value; known2.lon = true; }
            if (!known2.elev && number(elevColumn, value)) { station.elevation = value; station.hasElevation = true; known2.elev = true; }
            // 各通道取值：去掉 missing 标记与 range 之外的值，再按 scale/offset 与单位换算转为 SI 后入序列。
            for (const auto& item : keyColumn) {
                double raw = 0;
                if (!number(item.second, raw)) continue;
                const ForcingChannel& channel = *channelOf(item.first);
                bool drop = false;
                for (const double marker : channel.missing) if (std::abs(raw - marker) <= 1e-9 * std::max(1.0, std::abs(marker))) drop = true;
                double v = raw * channel.scale + channel.offset;
                if (!drop && channel.hasRange && (v < channel.rangeMin || v > channel.rangeMax)) drop = true;
                if (drop) { ++removed[item.first]; continue; }
                Series& series = station.channels[item.first];
                series.times.push_back(hours);
                series.values.push_back(v * channel.unitFactor + channel.unitOffset);
            }
        }
        for (const auto& item : removed) report_.removed[item.first] += item.second;
    }
    // 步骤 4：用元数据对全部表头做一次校验（声明的源列是否存在等）。
    ForcingIssues issues;
    checkForcingAgainstNames(meta_, allHeaders, issues);
    if (!issues.ok()) throw std::runtime_error(issues.errorText());
    for (const auto& text : issues.warnings) report_.warn(text);
    // 步骤 5：站点收尾检查与排序。
    for (auto& station : stations_) {
        const Pending& known = pending[station.id];
        if (!known.lat || !known.lon)
            throw std::runtime_error("站点 \"" + station.id + "\"（" + station.file + "）缺少经纬度：请在 station.stations 登记或在 CSV 里提供 lat/lon 列");
        if (station.latitude < -90 || station.latitude > 90) throw std::runtime_error("站点 \"" + station.id + "\" 的纬度 " + std::to_string(station.latitude) + " 超出范围");
        anyElevation_ = anyElevation_ || station.hasElevation;
        for (auto& item : station.channels) {  // sort by time, later duplicates win
            Series& s = item.second;
            std::vector<size_t> order(s.times.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return s.times[a] < s.times[b]; });
            Series sorted;
            for (const size_t i : order) {
                if (!sorted.times.empty() && std::abs(sorted.times.back() - s.times[i]) < 1e-9) sorted.values.back() = s.values[i];
                else { sorted.times.push_back(s.times[i]); sorted.values.push_back(s.values[i]); }
            }
            s = std::move(sorted);
        }
    }
    if (stations_.empty()) throw std::runtime_error("气象站 CSV 里没有数据行");
    double firstTime = std::numeric_limits<double>::infinity();
    for (const auto& station : stations_) for (const auto& item : station.channels) if (!item.second.times.empty()) firstTime = std::min(firstTime, item.second.times.front());
    if (std::isfinite(firstTime)) { int y; unsigned mo, d; civilFromDays(static_cast<int64_t>(std::floor(firstTime / 24.0)), y, mo, d); year_ = y; }
    if (!anyElevation_ && meta_.pressureType != "surface")
        report_.warn("没有气压且没有站点/格点海拔，地面气压取 1013.25 hPa");
    // declared step against the data
    if (meta_.stepHours > 0)
        for (const auto& station : stations_)
            if (const auto sw = station.channels.find("sw"); sw != station.channels.end() && sw->second.times.size() > 1) {
                const double detected = medianSpacing(sw->second.times);
                if (std::abs(detected - meta_.stepHours) > 0.01 * meta_.stepHours)
                    report_.warn("time.stepHours = " + std::to_string(meta_.stepHours) + " 与数据检测到的间隔 " + std::to_string(detected) + " h 不一致，按声明处理");
                break;
            }
}

// 作用：某站某通道的记录步长（小时）：通道自己的 stepHours > 全局 time.stepHours > 由数据检测的中位间隔。
double StationSource::stepOf(const Station& station, const std::string& key) const
{
    const ForcingChannel* channel = channelOf(key);
    if (channel && channel->stepHours > 0) return channel->stepHours;
    if (meta_.stepHours > 0) return meta_.stepHours;
    const auto found = station.channels.find(key);
    return found == station.channels.end() ? 1.0 : medianSpacing(found->second.times);
}

// 作用：是否有任一站点带有该通道键的数据；键 "z" 表示是否有任一站点海拔。
bool StationSource::hasKey(const std::string& key) const
{
    if (key == "z") return anyElevation_;
    return std::any_of(stations_.begin(), stations_.end(), [&](const Station& s) { return s.channels.count(key) > 0; });
}

// 作用：构成给定标准量所需的站点通道键（例如 u10 需要 u、v 分量或风速），由 forcingChannelKeys 决定。
std::vector<std::string> StationSource::keysFor(const std::vector<std::string>& variables) const
{
    return forcingChannelKeys(meta_, variables, [&](const std::string& key) { return hasKey(key); });
}

// 作用：需要在首个所需小时之前额外覆盖的小时数。累计量（kind = cumulative）的短波/长波
//       要用相邻记录差分，因此提前一个记录步长（向上取整到小时）。
int64_t StationSource::leadHours() const
{
    int64_t lead = 0;
    for (const char* key : {"sw", "lw"})
        if (channelOf(key)->kind == "cumulative" && hasKey(key)) lead = std::max<int64_t>(lead, static_cast<int64_t>(std::ceil(stepOf(stations_.front(), key))));
    return lead;
}

// 作用：某标准量能否由当前数据构成。d2m 由湿度通道换算，sp 在 pressureType = none 时可由海拔估算，
//       strd 在启用 Brutsaert 备选时可由气温等推算，u10/v10 取决于风的类型（分量或风速风向/仅风速）。
bool StationSource::available(const std::string& variable) const
{
    if (variable == "t2m") return hasKey("ta");
    if (variable == "d2m") return hasKey("hum");
    if (variable == "sp") return meta_.pressureType == "none" || hasKey("p");
    if (variable == "u10" || variable == "v10") return meta_.windType == "components" ? hasKey("u") && hasKey("v") : hasKey("speed");
    if (variable == "ssrd") return hasKey("sw");
    if (variable == "strd") return hasKey("lw") || meta_.longwaveFallback == "brutsaert";
    if (variable == "skt") return hasKey("skt");
    if (variable == "swvl1") return hasKey("swvl");
    if (variable == "z") return anyElevation_;
    return false;
}

// 作用：单个站点的某通道记录能否覆盖 [firstHour, lastHour]（站点没有该通道返回 false 且 why 为空）。
// 判定同 recordsCover（forcing_resample.cpp）：通量类要求每个小时都落在记录时段内，瞬时类要求两端有记录且缺口不超过最大缺口。
bool StationSource::stationCovers(const Station& station, const std::string& key, int64_t firstHour, int64_t lastHour, std::string& why) const
{
    const auto found = station.channels.find(key);
    if (found == station.channels.end()) return false;
    const ForcingChannel& channel = *channelOf(key);
    const bool flux = (key == "sw" || key == "lw") && channel.kind != "instant";
    const double step = stepOf(station, key);
    return recordsCover(channel, step, found->second.times, flux, forcingMaxGap(meta_.maxGapHours, step), firstHour, lastHour, why);
}

// 模块：检查 [firstHour, lastHour] 是否被数据覆盖。每个标准量所需的每个通道，
//       至少要有一个站点的原始记录（含允许的最大缺口）完整包住该时段；
//       否则在 gap 里写出缺失的量与原因并返回 false。
//       只要有一个站点完整覆盖就通过；其余站点在窗口内缺测的情况由 load() 逐站警告（数值上这些站点
//       的缺测小时在 at() 中被剔除并对剩余站点权重重新归一，所以不影响通过与否）。
bool StationSource::covers(const std::vector<std::string>& variables, int64_t firstHour, int64_t lastHour, std::string& gap) const
{
    for (const auto& name : variables) {
        if (!available(name)) { gap = name + " 不在数据中"; return false; }
        for (const auto& key : keysFor({name})) {
            if (key == "z") continue;
            bool any = false;
            std::string why;
            for (const auto& station : stations_)
                if (stationCovers(station, key, firstHour, lastHour, why)) { any = true; break; }
            if (!any) { gap = "没有站点完整覆盖 " + key + "：" + why + "（原始记录需包住该时段）"; return false; }
        }
    }
    return true;
}
// 模块：加载窗口 [firstHour, lastHour]（经纬度窗口对站点无意义，被忽略）。
// 步骤 1：对每个所需通道、每个站点，把原始记录重采样为逐小时序列（hourly[key][小时×站点数 + 站点]）：
//         · 通量（sw、lw）且 kind 不是 instant：按时段平均通量守恒分配（短波按太阳高度，长波平滑），
//         · instant 通量：记录时刻后移 0.5 h 再线性插值（使整点 h 取到 h-0.5 时刻，近似前一小时平均），并警告不守恒，
//         · kind = previous：零阶保持（缺口不超过 maxGap），其余瞬时量：线性插值，
//         · 通量结果小于 0 的置 0，非有限置 NaN。
// 步骤 2：海拔通道 z = 站点海拔 × 标准重力（m2 s-2），无海拔的站为 NaN。
// 步骤 3：交 standardiseQuantities 换算为 ERA5 命名的标准量（高度订正、湿度/风/气压换算等），存入 data_。
void StationSource::load(const std::vector<std::string>& variables, double, double, double, double, int64_t firstHour, int64_t lastHour)
{
    firstHour_ = firstHour;
    lastHour_ = lastHour;
    const size_t hours = static_cast<size_t>(lastHour - firstHour + 1), count = stations_.size();
    std::map<std::string, std::vector<double>> hourly, constant;
    // 步骤 0：逐站检查窗口覆盖。covers() 只要求至少一个站点完整覆盖；其余站点缺测的小时在 at() 中被剔除，
    //         由剩余站点补位（权重重新归一）。这里对“有该通道但窗口内不完整覆盖”的站点给出警告，不改变数值。
    for (const auto& key : keysFor(variables)) {
        if (key == "z") continue;
        for (const auto& station : stations_) {
            std::string why;
            if (station.channels.count(key) && !stationCovers(station, key, firstHour, lastHour, why))
                report_.warn("站点 \"" + station.id + "\" 的 " + key + " 通道在 " + formatUtc(static_cast<double>(firstHour) * 3600.0) + " 至 " +
                             formatUtc(static_cast<double>(lastHour) * 3600.0) + " 内记录不完整（" + why + "），缺测小时由其他站点补位");
        }
    }
    for (const auto& key : keysFor(variables)) {
        if (key == "z") {
            auto& z = constant["z"];
            z.assign(count, kNotANumber);
            for (size_t s = 0; s < count; ++s) if (stations_[s].hasElevation) z[s] = stations_[s].elevation * kForcingStandardGravity;
            continue;
        }
        const ForcingChannel& channel = *channelOf(key);
        auto& output = hourly[key];
        output.assign(hours * count, kNotANumber);
        const bool fluxChannel = key == "sw" || key == "lw";
        for (size_t s = 0; s < count; ++s) {
            const auto found = stations_[s].channels.find(key);
            if (found == stations_[s].channels.end()) continue;
            const Series& series = found->second;
            const double step = stepOf(stations_[s], key), maxGap = forcingMaxGap(meta_.maxGapHours, step);
            std::vector<double> result;
            if (fluxChannel && channel.kind != "instant")
                distributeFlux(fluxRecordsFrom(channel, step, series.times, series.values), key == "sw" ? FluxShape::Solar : FluxShape::Smooth,
                               stations_[s].latitude, stations_[s].longitude, firstHour, lastHour, result);
            else if (fluxChannel) {
                std::vector<double> shifted(series.times);
                for (auto& t : shifted) t += 0.5;
                resampleInstant(shifted, series.values, firstHour, lastHour, maxGap, result);
                report_.warn("瞬时短波/长波的重采样不守恒（variables." + std::string(key == "sw" ? "shortwaveDown" : "longwaveDown") + ".kind = instant）");
            } else if (channel.kind == "previous") {
                result.assign(hours, kNotANumber);
                for (int64_t hour = firstHour; hour <= lastHour; ++hour)
                    for (size_t r = series.times.size(); r-- > 0;)
                        if (series.times[r] <= hour + 1e-9) { if (hour - series.times[r] <= maxGap + 1e-9) result[hour - firstHour] = series.values[r]; break; }
            } else resampleInstant(series.times, series.values, firstHour, lastHour, maxGap, result);
            for (size_t h = 0; h < hours; ++h) {
                double v = result[h];
                if (fluxChannel) v = std::isfinite(v) ? std::max(v, 0.0) : kNotANumber;
                output[h * count + s] = v;
            }
        }
    }
    std::map<std::string, std::vector<float>> standard;
    standardiseQuantities(meta_, hourly, constant, hours, count, variables, report_, standard);
    for (auto& item : standard) data_[item.first] = std::move(item.second);
}

// 模块：某位置的站点插值权重。
// 步骤：按球面距离排序，剔除超过 maxDistanceKm（> 0 时）的站点；nearest 取 1 站，
//       idw 取最近 maxStations 站按 1/d^power 归一化加权；距离 < 1 m 视为同址，直接取该站；
//       权重槽最多 4 个，取站数被限制在 4（配置校验已限 maxStations 为 1–4，这里再保险），所以存入的权重和恒为 1；
//       范围内没有站点时权重全为 0。
ForcingWeights StationSource::weights(double latitude, double longitude) const
{
    ForcingWeights w;
    std::vector<std::pair<double, size_t>> order;
    for (size_t s = 0; s < stations_.size(); ++s) {
        const double d = distanceKm(latitude, longitude, stations_[s].latitude, stations_[s].longitude);
        if (meta_.maxDistanceKm > 0 && d > meta_.maxDistanceKm) continue;
        order.emplace_back(d, s);
    }
    std::sort(order.begin(), order.end());
    const size_t take = meta_.interpolation == "nearest" ? 1 : std::min<size_t>(order.size(), std::min<size_t>(static_cast<size_t>(meta_.maxStations), 4));  // 权重槽只有 4 个：限制取站数，保证存入的权重和为 1
    if (order.empty()) return w;  // no station in range: all weights stay 0
    if (take == 1 || order.front().first < 1e-3) { w.index[0] = static_cast<int>(order.front().second); w.weight[0] = 1.0f; return w; }
    double total = 0;
    std::vector<double> raw;
    for (size_t k = 0; k < take; ++k) { raw.push_back(1.0 / std::pow(order[k].first, meta_.idwPower)); total += raw.back(); }
    for (size_t k = 0; k < take && k < 4; ++k) { w.index[k] = static_cast<int>(order[k].second); w.weight[k] = static_cast<float>(raw[k] / total); }
    return w;
}

// 作用：某标准量在整点 hour（限制在已加载范围内）的值：对有效（有限）站点值做权重平均，
//       权重按剩余有效站点重新归一；全部缺测返回 NaN。海拔 z 为常量，不随小时变化。
float StationSource::at(const std::string& variable, int64_t hour, const ForcingWeights& weights) const
{
    const auto found = data_.find(variable);
    if (found == data_.end()) return std::numeric_limits<float>::quiet_NaN();
    const size_t count = stations_.size();
    const size_t offset = variable == "z" ? 0 : static_cast<size_t>(std::clamp(hour, firstHour_, lastHour_) - firstHour_) * count;
    double sum = 0, total = 0;
    for (int k = 0; k < 4; ++k) {
        if (!(weights.weight[k] > 0)) continue;
        const float value = found->second[offset + static_cast<size_t>(weights.index[k])];
        if (!std::isfinite(value)) continue;
        sum += weights.weight[k] * value;
        total += weights.weight[k];
    }
    return total > 1e-9 ? static_cast<float>(sum / total) : std::numeric_limits<float>::quiet_NaN();
}

// 作用：截至整点 hour 的前一小时平均向下通量（W m-2）；负值取 0，缺测返回 NaN。
float StationSource::hourlyFlux(const std::string& variable, int64_t hour, const ForcingWeights& weights) const
{
    const float value = at(variable, std::clamp(hour, firstHour_, lastHour_), weights);
    return std::isfinite(value) ? std::max(0.0f, value) : std::numeric_limits<float>::quiet_NaN();
}

}  // namespace stream
