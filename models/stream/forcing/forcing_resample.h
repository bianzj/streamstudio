// ==========================================================================
// 文件：forcing_resample.h
// 功能：气象强迫时间标准化的接口（网格读取与站点读取共用）
// 主要模块：
//   1. FluxRecord / FluxShape：时段平均通量记录与分配形状（Solar 短波 / Smooth 长波）
//   2. resampleInstant、fluxRecordsFrom、distributeFlux：瞬时量插值与通量重采样
//   3. standardiseQuantities、forcingChannelKeys：由逐小时通道合成标准量并推导所需通道
//   4. checkRecordsCover、recordsCover、forcingMaxGap：覆盖检查（共用判定）与最大缺口规则
// 数据流：era5.cpp（网格）/ station_source.cpp（站点）读出原始记录 → 本模块 → 逐小时标准量
// 约定：时间为自 1970-01-01 UTC 起的小时数；瞬时量取整点值，通量为截至该整点的前一小时平均（W m-2）；
//   缺测为 NaN
// 与其他文件：实现见 forcing_resample.cpp；规格见 docs/forcing-metadata.md 4.2–4.6
// ==========================================================================

#pragma once
// Temporal standardisation shared by the gridded and the station reader (docs/forcing-metadata.md 4.5 / 4.6):
// every source ends up on the whole-hour grid, instantaneous values at the hour and fluxes as the mean over the hour
// that ends there. All times are hours since 1970-01-01 UTC (fractions allowed), missing values are NaN.
#include "forcing_metadata.h"
#include "forcing_source.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace stream {

// 模块：通量时段记录。mean 为区间 [start, end]（小时）上的平均通量，单位 W m-2。
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
// Which channel keys (ta hum p u v speed dir sw lw skt swvl z) the standard quantities in `variables` need; `present` tells which exist.
std::vector<std::string> forcingChannelKeys(const ForcingMetadata& meta, const std::vector<std::string>& variables,
                                            const std::function<bool(const std::string&)>& present);

// 模块：记录覆盖检查的结构化结果（网格 Pack::channelCovers 与站点 recordsCover 共用同一判定，各自按需要格式化提示文字）。
// kind：Covered 覆盖；FluxHourUncovered 通量类某个小时 (hour − 1, hour] 没有记录时段覆盖（hour 有效）；
// NoRecordBefore / NoRecordAfter 瞬时类在 first 及之前 / last 及之后没有记录；
// GapTooLarge 瞬时类相邻记录 from→to（小时）的间隔超过 maxGapHours。
struct RecordsCoverResult {
    enum class Kind { Covered, FluxHourUncovered, NoRecordBefore, NoRecordAfter, GapTooLarge };
    Kind kind = Kind::Covered;
    int64_t hour = 0;           // FluxHourUncovered：第一个没有覆盖的整点
    double from = 0, to = 0;    // GapTooLarge：缺口两端的记录时刻（小时）
};

// The single decision rule behind recordsCover and Pack::channelCovers (era5.cpp): flux intervals must cover every hour (h - 1, h];
// instants need a record at or before `first`, one at or after `last` and no gap above maxGapHours between them. The first problem is returned.
RecordsCoverResult checkRecordsCover(const ForcingChannel& channel, double stepHours, const std::vector<double>& times, bool flux,
                                     double maxGapHours, int64_t first, int64_t last);

// Do the records of one channel cover [first, last]? Flux intervals must cover every hour (h - 1, h]; instants need a record at or before
// `first`, one at or after `last` and no gap above maxGapHours between them. `why` names the first problem.
bool recordsCover(const ForcingChannel& channel, double stepHours, const std::vector<double>& times, bool flux, double maxGapHours,
                  int64_t first, int64_t last, std::string& why);
}  // namespace stream
