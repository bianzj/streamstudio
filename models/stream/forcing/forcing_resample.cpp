// ==========================================================================
// 文件：forcing_resample.cpp
// 功能：气象强迫的时间标准化——把任意时间分辨率的记录重采样到逐小时整点，并合成引擎标准量
// 主要模块：
//   1. forcingMaxGap：瞬时量允许线性插值跨越的最大缺口
//   2. resampleInstant：瞬时量在整点的线性插值（缺口过大或端点缺测则该小时缺测）
//   3. fluxRecordsFrom：把通量通道的原始记录还原为“时段平均通量”（W m-2）
//   4. distributeFlux：把时段平均通量分配到逐小时（短波按太阳天顶角余弦，长波按中点线性），
//      保证每个时段的能量守恒
//   5. standardiseQuantities：由各小时通道合成标准量 t2m d2m sp u10 v10 ssrd strd skt swvl1 z
//      （湿度换算、气压估算、风分量、高度订正、Brutsaert 长波回退）
//   6. forcingChannelKeys / recordsCover：标准量所需通道推导与记录覆盖检查（站点源使用）
// 数据流：原始记录（时刻 + 数值）→ fluxRecordsFrom → distributeFlux / resampleInstant → 逐小时通道 →
//   standardiseQuantities → 标准量（float）供 pixel_forcing 使用
// 约定：时间为自 1970-01-01 UTC 起的小时数（可为小数）；缺测为 NaN；
//   逐小时通量 out[h − first] 表示 (h − 1, h] 区间的平均通量（W m-2）
// 注意：各时段的能量严格守恒；夜间且通量近 0 时权重全取 1，避免除零（见 distributeFlux）
// 与其他文件：forcing_metadata.h（通道与元数据）、era5.cpp 与 station_source.cpp（调用方）、
//   utc.h（时间格式）；approximateCosZenith 在别处定义（NOAA 近似式，待核对所在文件）
// ==========================================================================

#include "forcing_resample.h"

#include "forcing_source.h"
#include "utc.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace stream {

// 外部函数声明：近似太阳天顶角余弦（经纬度为度，时间为 Unix 秒）；
// 定义在别的源文件中（待核对），用于把短波按太阳高度分配到逐小时。
double approximateCosZenith(double latitudeDeg, double longitudeDeg, double unixSeconds);

namespace {
constexpr double kNotANumber = std::numeric_limits<double>::quiet_NaN();
// distributeFlux 对时段内部取样的步长（小时）：5 min，与文档 4.5 中“按 5 min 采样平均”一致。
constexpr double kSampleHours = 5.0 / 60.0;
}  // namespace

// 作用：瞬时量线性插值允许跨越的最大缺口（小时）。用户设置了 maxGapHours 就用它，
// 否则取 max(6, 2×记录步长)。
double forcingMaxGap(double maxGapHours, double stepHours) { return maxGapHours > 0 ? maxGapHours : std::max(6.0, 2.0 * stepHours); }

// 作用：把记录（times 升序，单位小时）线性插值到整点 first…last，结果 out[h−first]。
// 规则：恰在记录时刻直接取值；两侧记录相距不超过 maxGapHours 且都有效才插值；
// 否则该小时为 NaN（不外推）。times 为空时全部缺测。
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

// 作用：把通量通道的记录还原为时段平均通量 FluxRecord{start, end, mean(W m-2)}。
// 时段终点 end 由戳语义决定：previous 或 interval_start 为 t+步长，interval_mid 为 t+步长/2，其余为 t；
// start = end − 步长。数值含义由 kind 决定：
//   accumulated：值（J m-2 等）÷ (步长×3600) 得平均通量；
//   cumulative：时段起点恰在重置时刻（resetHourUtc）时，值本身即该时段累积；否则与前一条
//   间隔恰好一个步长的记录差分，差为负（累计量回落，视为重置）时用值本身；
//   其余（interval_mean / previous）：值即平均通量。
// 结果截断为不小于 0（辐射非负），无效为 NaN。kind=instant 不属于时段数据，返回空。
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

// 作用：把时段平均通量分配到逐小时 out[h−first] = (h−1, h] 的平均通量（W m-2），不足覆盖处为 NaN。
// 输入：shape=Solar（短波，权重取太阳天顶角余弦 μ，即晴空指数在时段内不变）或
// Smooth（长波，在相邻时段中点之间线性变化）；latitude/longitude 为度，仅 Solar 使用。
// 步骤 1：对每个有效时段，若恰为对齐整点的 1 h 记录，则直接赋给该小时；
// 步骤 2：否则按 5 min 取样，样本权重 w（Solar：max(μ,0)；Smooth：中点线性插值并截断为 ≥0），
// 按 w/Σw 把时段能量 mean×长度 分给样本所在小时；
// 步骤 3：小时的覆盖时长超过 0.95 h 才输出能量/覆盖时长，由此每个时段的能量严格守恒。
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
        // 权重和为 0（例如整个时段在夜间、μ≤0）时改为均匀分配，避免除以零；这种时段通量本应约为 0。
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

// 作用：由逐小时通道数据合成标准量（见头文件说明），写入 data（name → [hour][cell] float；z 为 [cell]）。
// 输入：hourly（通道键 → [hour][cell]，SI）、constant（不随时间的通道 → [cell]）、
// hours、cells、需要的标准量 variables。
// 要点：t2m 加上气温高度订正平移 temperatureShiftK；气压按 pressureType 取地面气压、由海平面气压
// 折算或由海拔按标准大气估算；d2m 按 humidityType 先换为水汽压再反解露点；
// 风按 components / speed / speedDirection 得到 u、v 再乘 windFactor；
// 无长波数据时按 Brutsaert 晴空参数化并警告；换算过的量写入 report.derived。
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
    // 海拔 = 位势 z ÷ g（m）；气压为 seaLevel/none 时由它推出地面气压。
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
                // 风向换算：气象风向 "from"（风来自的方位，自北顺时针）时分量取负号：u = −speed·sin(dir)，
                // v = −speed·cos(dir)；"to" 为风吹向的方位，取正号。u 向东、v 向北。
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

// 作用：推出标准量 variables 需要读取的通道键（ta hum p u v speed dir sw lw skt swvl z），
// 只保留 present(key) 为真的；映射规则与 era5.cpp 中 Pack::channelsFor 相同（待核对是否应合并）。
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

// 作用：检查一个通道的记录能否覆盖 [first, last]，不能时 why 写出第一个问题。
// 规则：通量类要求每个 (h−1, h] 都落在某条记录代表的时段内；瞬时类需要 first 及之前、
// last 及之后各一条记录，且二者之间相邻记录的间隔不超过 maxGap。
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
