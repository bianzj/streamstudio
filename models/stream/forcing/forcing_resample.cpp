#include "forcing_resample.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

}  // namespace stream
