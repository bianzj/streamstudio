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

constexpr double kNotANumber = std::numeric_limits<double>::quiet_NaN();
namespace fs = std::filesystem;

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

double distanceKm(double lat1, double lon1, double lat2, double lon2)
{
    const double rad = 3.14159265358979323846 / 180.0;
    const double a = std::sin((lat2 - lat1) * rad / 2), b = std::sin((lon2 - lon1) * rad / 2);
    const double h = a * a + std::cos(lat1 * rad) * std::cos(lat2 * rad) * b * b;
    return 2.0 * 6371.0088 * std::asin(std::min(1.0, std::sqrt(h)));
}

double medianSpacing(const std::vector<double>& times)
{
    std::vector<double> steps;
    for (size_t i = 1; i < times.size(); ++i) steps.push_back(times[i] - times[i - 1]);
    if (steps.empty()) return 1.0;
    std::sort(steps.begin(), steps.end());
    return steps[steps.size() / 2];
}

const char* const kKeys[] = {"ta", "hum", "p", "u", "v", "speed", "dir", "sw", "lw", "skt", "swvl"};

}  // namespace

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

std::vector<std::string> StationSource::readHeader(const fs::path& file, const ForcingMetadata& metadata)
{
    for (const auto& line : readLines(file)) {
        const std::string text = trim(line);
        if (text.empty() || (!metadata.comment.empty() && text.rfind(metadata.comment, 0) == 0)) continue;
        return splitLine(line, metadata.delimiter.empty() ? ',' : metadata.delimiter[0]);
    }
    return {};
}

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
        const int timeColumn = column(meta_.timeColumn);
        if (timeColumn < 0) throw std::runtime_error("气象强迫数据找不到时间列 \"" + meta_.timeColumn + "\"（time.column）；" + fileName + " 的列：" + [&] { std::string s; for (const auto& n : header) s += (s.empty() ? "" : "、") + n; return s; }());
        const int idColumn = column(meta_.idColumn), latColumn = column(meta_.latitudeColumn), lonColumn = column(meta_.longitudeColumn), elevColumn = column(meta_.elevationColumn);
        std::map<std::string, int> keyColumn;
        for (const char* key : kKeys) {
            const ForcingChannel* channel = channelOf(key);
            if (!channel->declared) continue;
            for (const auto& name : channel->source) if (column(name) >= 0) { keyColumn[key] = column(name); break; }
        }
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
            double value = 0;
            if (!known2.lat && number(latColumn, value)) { station.latitude = value; known2.lat = true; }
            if (!known2.lon && number(lonColumn, value)) { station.longitude = value; known2.lon = true; }
            if (!known2.elev && number(elevColumn, value)) { station.elevation = value; station.hasElevation = true; known2.elev = true; }
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
    ForcingIssues issues;
    checkForcingAgainstNames(meta_, allHeaders, issues);
    if (!issues.ok()) throw std::runtime_error(issues.errorText());
    for (const auto& text : issues.warnings) report_.warn(text);
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

double StationSource::stepOf(const Station& station, const std::string& key) const
{
    const ForcingChannel* channel = channelOf(key);
    if (channel && channel->stepHours > 0) return channel->stepHours;
    if (meta_.stepHours > 0) return meta_.stepHours;
    const auto found = station.channels.find(key);
    return found == station.channels.end() ? 1.0 : medianSpacing(found->second.times);
}

bool StationSource::hasKey(const std::string& key) const
{
    if (key == "z") return anyElevation_;
    return std::any_of(stations_.begin(), stations_.end(), [&](const Station& s) { return s.channels.count(key) > 0; });
}

std::vector<std::string> StationSource::keysFor(const std::vector<std::string>& variables) const
{
    return forcingChannelKeys(meta_, variables, [&](const std::string& key) { return hasKey(key); });
}

int64_t StationSource::leadHours() const
{
    int64_t lead = 0;
    for (const char* key : {"sw", "lw"})
        if (channelOf(key)->kind == "cumulative" && hasKey(key)) lead = std::max<int64_t>(lead, static_cast<int64_t>(std::ceil(stepOf(stations_.front(), key))));
    return lead;
}

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

bool StationSource::covers(const std::vector<std::string>& variables, int64_t firstHour, int64_t lastHour, std::string& gap) const
{
    for (const auto& name : variables) {
        if (!available(name)) { gap = name + " 不在数据中"; return false; }
        for (const auto& key : keysFor({name})) {
            if (key == "z") continue;
            bool any = false;
            std::string why;
            for (const auto& station : stations_) {
                const auto found = station.channels.find(key);
                if (found == station.channels.end()) continue;
                const ForcingChannel& channel = *channelOf(key);
                const bool flux = (key == "sw" || key == "lw") && channel.kind != "instant";
                const double step = stepOf(station, key);
                if (recordsCover(channel, step, found->second.times, flux, forcingMaxGap(meta_.maxGapHours, step), firstHour, lastHour, why)) { any = true; break; }
            }
            if (!any) { gap = "没有站点完整覆盖 " + key + "：" + why + "（原始记录需包住该时段）"; return false; }
        }
    }
    return true;
}

void StationSource::load(const std::vector<std::string>& variables, double, double, double, double, int64_t firstHour, int64_t lastHour)
{
    firstHour_ = firstHour;
    lastHour_ = lastHour;
    const size_t hours = static_cast<size_t>(lastHour - firstHour + 1), count = stations_.size();
    std::map<std::string, std::vector<double>> hourly, constant;
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
    const size_t take = meta_.interpolation == "nearest" ? 1 : std::min<size_t>(order.size(), static_cast<size_t>(meta_.maxStations));
    if (order.empty()) return w;  // no station in range: all weights stay 0
    if (take == 1 || order.front().first < 1e-3) { w.index[0] = static_cast<int>(order.front().second); w.weight[0] = 1.0f; return w; }
    double total = 0;
    std::vector<double> raw;
    for (size_t k = 0; k < take; ++k) { raw.push_back(1.0 / std::pow(order[k].first, meta_.idwPower)); total += raw.back(); }
    for (size_t k = 0; k < take && k < 4; ++k) { w.index[k] = static_cast<int>(order[k].second); w.weight[k] = static_cast<float>(raw[k] / total); }
    return w;
}

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

float StationSource::hourlyFlux(const std::string& variable, int64_t hour, const ForcingWeights& weights) const
{
    const float value = at(variable, std::clamp(hour, firstHour_, lastHour_), weights);
    return std::isfinite(value) ? std::max(0.0f, value) : std::numeric_limits<float>::quiet_NaN();
}

}  // namespace stream
