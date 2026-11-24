#include "forcing_resample.h"

#include "forcing_source.h"
#include "utc.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace stream {

double approximateCosZenith(double latitudeDeg, double longitudeDeg, double unixSeconds);

namespace {
constexpr double kNotANumber = std::numeric_limits<double>::quiet_NaN();
constexpr double kSampleHours = 5.0 / 60.0;
}  // namespace

double forcingMaxGap(double maxGapHours, double stepHours) { return maxGapHours > 0 ? maxGapHours : std::max(6.0, 2.0 * stepHours); }

void resampleInstant(const std::vector<double>& times, const std::vector<double>& values, int64_t first, int64_t last,
                     double maxGapHours, std::vector<double>& out)
{
    out.assign(static_cast<size_t>(last - first + 1), kNotANumber);
    if (times.empty()) return;
    size_t next = 0;
    for (int64_t hour = first; hour <= last; ++hour) {
        const double t = static_cast<double>(hour);
        while (next < times.size() && times[next] < t - 1e-9) ++next;
        double value = kNotANumber;
        if (next < times.size() && std::abs(times[next] - t) <= 1e-9) value = values[next];
        else if (next > 0 && next < times.size()) {
            const size_t a = next - 1, b = next;
            if (times[b] - times[a] <= maxGapHours + 1e-9 && std::isfinite(values[a]) && std::isfinite(values[b])) {
                const double f = (t - times[a]) / (times[b] - times[a]);
                value = (1 - f) * values[a] + f * values[b];
            }
        }
        out[static_cast<size_t>(hour - first)] = value;
    }
}

std::vector<FluxRecord> fluxRecordsFrom(const ForcingChannel& c, double step, const std::vector<double>& times, const std::vector<double>& values)
{
    std::vector<FluxRecord> records;
    if (c.kind == "instant") return records;
    const auto endOf = [&](double t) {
        if (c.kind == "previous" || c.stamp == "interval_start") return t + step;
        return c.stamp == "interval_mid" ? t + 0.5 * step : t;
    };
    const auto hourOfDay = [](double hour) { return std::fmod(std::fmod(hour, 24.0) + 24.0, 24.0); };
    for (size_t i = 0; i < times.size(); ++i) {
        FluxRecord record;
        record.end = endOf(times[i]);
        record.start = record.end - step;
        double mean = kNotANumber;
        if (c.kind == "accumulated") mean = values[i] / (step * 3600.0);
        else if (c.kind == "cumulative") {
            const double startOfDay = hourOfDay(record.start), reset = static_cast<double>(c.resetHourUtc);
            const bool restart = std::abs(startOfDay - reset) < 1e-6 || std::abs(startOfDay - reset - 24.0) < 1e-6;
            if (restart) mean = values[i] / (step * 3600.0);
            else if (i > 0 && std::abs(times[i] - times[i - 1] - step) < 1e-6 && std::isfinite(values[i - 1])) {
                const double difference = values[i] - values[i - 1];
                mean = (difference >= 0 ? difference : values[i]) / (step * 3600.0);
            }
        } else mean = values[i];  // interval_mean / previous
        record.mean = std::isfinite(mean) ? std::max(mean, 0.0) : kNotANumber;
        records.push_back(record);
    }
    return records;
}

void distributeFlux(const std::vector<FluxRecord>& records, FluxShape shape, double latitude, double longitude, int64_t first,
                    int64_t last, std::vector<double>& out)
{
    const size_t hours = static_cast<size_t>(last - first + 1);
    std::vector<double> energy(hours, 0.0), covered(hours, 0.0);  // W m-2 h, hours of coverage
    for (size_t index = 0; index < records.size(); ++index) {
        const FluxRecord& record = records[index];
        if (!std::isfinite(record.mean)) continue;
        const double length = record.end - record.start;
        if (!(length > 0)) continue;
        const int64_t firstBin = static_cast<int64_t>(std::floor(record.start)) + 1, lastBin = static_cast<int64_t>(std::ceil(record.end));
        if (lastBin < first || firstBin > last) continue;
        // Aligned hourly record: the hour keeps the record's value.
        if (std::abs(length - 1.0) < 1e-9 && std::abs(record.end - std::round(record.end)) < 1e-9) {
            const int64_t hour = static_cast<int64_t>(std::llround(record.end));
            if (hour >= first && hour <= last) { energy[hour - first] += record.mean; covered[hour - first] += 1.0; }
            continue;
        }
        // Sample the interval; every sample belongs to the hour bin (h - 1, h] that contains it.
        const int samples = std::max(1, static_cast<int>(std::ceil(length / kSampleHours)));
        const double delta = length / samples;
        std::vector<double> weight(samples);
        double total = 0.0;
        for (int s = 0; s < samples; ++s) {
            const double t = record.start + (s + 0.5) * delta;
            double w = 1.0;
            if (shape == FluxShape::Solar) {
                const double mu = approximateCosZenith(latitude, longitude, t * 3600.0);
                w = mu > 0 ? mu : 0.0;
            } else {
                // Linear between the midpoints of the neighbouring intervals (flat beyond the outer ones).
                const double mid = 0.5 * (record.start + record.end);
                const FluxRecord* other = nullptr;
                if (t < mid && index > 0 && std::abs(records[index - 1].end - record.start) < 1e-6) other = &records[index - 1];
                if (t > mid && index + 1 < records.size() && std::abs(records[index + 1].start - record.end) < 1e-6) other = &records[index + 1];
                w = record.mean;
                if (other && std::isfinite(other->mean)) {
                    const double otherMid = 0.5 * (other->start + other->end);
                    w = record.mean + (other->mean - record.mean) * (t - mid) / (otherMid - mid);
                }
                w = std::max(w, 0.0);
            }
            weight[s] = w;
            total += w;
        }
        if (!(total > 1e-12)) { std::fill(weight.begin(), weight.end(), 1.0); total = samples; }
        const double joules = record.mean * length;
        for (int s = 0; s < samples; ++s) {
            const double t = record.start + (s + 0.5) * delta;
            const int64_t hour = static_cast<int64_t>(std::ceil(t));
            if (hour < first || hour > last) continue;
            energy[hour - first] += joules * weight[s] / total;
            covered[hour - first] += delta;
        }
    }
    out.assign(hours, kNotANumber);
    for (size_t i = 0; i < hours; ++i)
        if (covered[i] > 0.95) out[i] = energy[i] / covered[i];
}

void standardiseQuantities(const ForcingMetadata& meta_, const std::map<std::string, std::vector<double>>& hourly,
                           const std::map<std::string, std::vector<double>>& constant, size_t hours, size_t cells,
                           const std::vector<std::string>& variables, ForcingReport& report, std::map<std::string, std::vector<float>>& data)
{
    constexpr double kNotANumber = std::numeric_limits<double>::quiet_NaN();
    constexpr float kMissing = std::numeric_limits<float>::quiet_NaN();
    const auto value = [&](const std::string& key, size_t hour, size_t cell) -> double {
        if (const auto found = hourly.find(key); found != hourly.end()) return found->second[hour * cells + cell];
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
        if (!pressureWarned) { pressureWarned = true; report.warn("没有气压且没有站点/格点海拔，地面气压取 1013.25 hPa"); }
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
        auto& out = data[name];
        out.assign(hours * cells, kMissing);
        for (size_t h = 0; h < hours; ++h)
            for (size_t cell = 0; cell < cells; ++cell) {
                const double v = compute(h, cell);
                if (std::isfinite(v)) out[h * cells + cell] = static_cast<float>(v);
            }
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
            if (meta_.pressureType != "surface") report.derived.push_back(meta_.pressureType == "seaLevel" ? "sp 由海平面气压 + 海拔换算" : "sp 由海拔按标准大气估算");
        } else if (name == "d2m") {
            store(name, dewpoint);
            if (meta_.humidityType != "dewpoint") report.derived.push_back("d2m 由 " + meta_.humidityType + " 湿度换算");
        } else if (name == "u10") store(name, [&](size_t h, size_t c) { return wind(h, c, false); });
        else if (name == "v10") store(name, [&](size_t h, size_t c) { return wind(h, c, true); });
        else if (name == "ssrd") store(name, [&](size_t h, size_t c) { return value("sw", h, c); });
        else if (name == "strd") {
            if (!!hourly.count("lw")) store(name, [&](size_t h, size_t c) { return value("lw", h, c); });
            else {
                report.warn("没有向下长波，按 Brutsaert 晴空参数化（不含云，阴天偏低）");
                report.derived.push_back("strd 由 Brutsaert 晴空参数化估算");
                store(name, [&](size_t h, size_t c) {
                    const double ta = temperature(h, c), td = dewpoint(h, c);
                    return std::isfinite(ta) && std::isfinite(td) ? forcingBrutsaertLongwave(ta, forcingSaturationVapourPressure(td - 273.15)) : kNotANumber;
                });
            }
        } else if (name == "skt") store(name, [&](size_t h, size_t c) { return value("skt", h, c); });
        else if (name == "swvl1") store(name, [&](size_t h, size_t c) { return value("swvl", h, c); });
        else if (name == "z") {
            auto& out = data[name];
            out.assign(cells, kMissing);
            const auto found = constant.find("z");
            if (found != constant.end())
                for (size_t cell = 0; cell < cells; ++cell) if (std::isfinite(found->second[cell])) out[cell] = static_cast<float>(found->second[cell]);
        } else throw std::runtime_error("未知的标准强迫量 " + name);
    }
}

std::vector<std::string> forcingChannelKeys(const ForcingMetadata& meta, const std::vector<std::string>& variables,
                                            const std::function<bool(const std::string&)>& present)
{
    std::set<std::string> keys;
    const auto wind = [&] {
        if (meta.windType == "components") { keys.insert("u"); keys.insert("v"); }
        else { keys.insert("speed"); if (meta.windType == "speedDirection") keys.insert("dir"); }
    };
    const auto pressure = [&] {
        if (meta.pressureType == "surface") keys.insert("p");
        else { keys.insert("z"); if (meta.pressureType == "seaLevel") { keys.insert("p"); keys.insert("ta"); } }
    };
    const auto dewpoint = [&] {
        keys.insert("hum");
        if (meta.humidityType != "dewpoint") { keys.insert("ta"); if (meta.humidityType != "vapourPressure") pressure(); }
    };
    for (const auto& name : variables) {
        if (name == "t2m") keys.insert("ta");
        else if (name == "d2m") dewpoint();
        else if (name == "sp") pressure();
        else if (name == "u10" || name == "v10") wind();
        else if (name == "ssrd") keys.insert("sw");
        else if (name == "strd") {
            if (present("lw")) keys.insert("lw");
            else { keys.insert("ta"); dewpoint(); }
        } else if (name == "skt") keys.insert("skt");
        else if (name == "swvl1") keys.insert("swvl");
        else if (name == "z") keys.insert("z");
    }
    std::vector<std::string> out;
    for (const auto& key : keys) if (present(key)) out.push_back(key);
    return out;
}

bool recordsCover(const ForcingChannel& channel, double step, const std::vector<double>& times, bool flux, double maxGap,
                  int64_t first, int64_t last, std::string& why)
{
    if (flux) {
        for (int64_t hour = first; hour <= last; ++hour) {
            bool covered = false;
            for (const double t : times) {
                const double end = (channel.kind == "previous" || channel.stamp == "interval_start") ? t + step : channel.stamp == "interval_mid" ? t + 0.5 * step : t;
                if (end - step <= hour - 1 + 1e-9 && end >= hour - 1e-9) { covered = true; break; }
            }
            if (!covered) { why = formatUtc(static_cast<double>(hour) * 3600.0) + " 之前的一小时没有记录覆盖"; return false; }
        }
        return true;
    }
    double before = kNotANumber, after = kNotANumber;
    for (const double t : times) { if (t <= first + 1e-9) before = t; if (t >= last - 1e-9 && !std::isfinite(after)) after = t; }
    if (!std::isfinite(before)) { why = "最早的记录晚于 " + formatUtc(static_cast<double>(first) * 3600.0); return false; }
    if (!std::isfinite(after)) { why = "最晚的记录早于 " + formatUtc(static_cast<double>(last) * 3600.0); return false; }
    for (size_t i = 1; i < times.size(); ++i)
        if (times[i] > before + 1e-9 && times[i - 1] < after - 1e-9 && times[i] - times[i - 1] > maxGap + 1e-9) {
            why = formatUtc(times[i - 1] * 3600.0) + " 到 " + formatUtc(times[i] * 3600.0) + " 之间缺测超过 " + std::to_string(maxGap) + " h";
            return false;
        }
    return true;
}
}  // namespace stream
