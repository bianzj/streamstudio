#pragma once
// Temporal standardisation shared by the gridded and the station reader (docs/forcing-metadata.md 4.5 / 4.6):
// every source ends up on the whole-hour grid, instantaneous values at the hour and fluxes as the mean over the hour
// that ends there. All times are hours since 1970-01-01 UTC (fractions allowed), missing values are NaN.
#include "forcing_metadata.h"
#include "forcing_source.h"

#include <cstdint>
#include <map>
#include <vector>

namespace stream {

struct FluxRecord {
    double start = 0, end = 0;  // interval the mean applies to (hours)
    double mean = 0;            // W m-2
};

enum class FluxShape { Solar, Smooth };

// Linear interpolation between records; an hour is missing when the surrounding records are further apart than maxGapHours
// or one of them is missing. out[h - first] is the value at hour h.
void resampleInstant(const std::vector<double>& times, const std::vector<double>& values, int64_t first, int64_t last,
                     double maxGapHours, std::vector<double>& out);

// Turns the records of one flux channel (values already in SI: W m-2 or J m-2) into interval means. `kind` accumulated divides by the
// interval length, cumulative differences consecutive records and restarts at resetHourUtc or when the value falls.
// Instant kinds are not interval data: the result is empty (the caller resamples them as instants).
std::vector<FluxRecord> fluxRecordsFrom(const ForcingChannel& channel, double stepHours, const std::vector<double>& times,
                                        const std::vector<double>& values);

// Distributes interval means onto the hours: out[h - first] = mean flux over (h - 1, h], NaN where the hour is not covered.
// Solar: proportional to the cosine of the solar zenith angle (clearness index constant within an interval); Smooth: linear between
// interval midpoints with the interval mean kept. Each interval keeps its energy exactly.
void distributeFlux(const std::vector<FluxRecord>& records, FluxShape shape, double latitude, double longitude, int64_t first,
                    int64_t last, std::vector<double>& out);

// Largest gap between consecutive records when the median spacing is `step`: max(6, 2 x step) unless maxGapHours is set.
double forcingMaxGap(double maxGapHours, double stepHours);

// Builds the standard quantities (t2m d2m sp u10 v10 ssrd strd skt swvl1 z) from the hourly channels of any source. `hourly`: channel key
// (ta hum p u v speed dir sw lw skt swvl z) -> [hour][cell] SI values (fluxes in W m-2 of the hour); `constant`: [cell] values of time-invariant
// channels. Humidity, pressure, wind and the Brutsaert longwave fallback follow docs/forcing-metadata.md 4.2-4.6; the wind factor and the air
// temperature shift of the height correction are applied. Output: name -> [hour][cell] floats ("z": [cell]).
void standardiseQuantities(const ForcingMetadata& meta, const std::map<std::string, std::vector<double>>& hourly,
                           const std::map<std::string, std::vector<double>>& constant, size_t hours, size_t cells,
                           const std::vector<std::string>& variables, ForcingReport& report, std::map<std::string, std::vector<float>>& data);
}  // namespace stream
