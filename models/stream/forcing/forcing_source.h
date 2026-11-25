// ==========================================================================
// 文件：forcing_source.h
// 功能：STREAM 能量平衡（EB）气象强迫源的抽象接口与运行报告结构
// 主要模块：
//   1. ForcingWeights：单个位置的插值权重（最多 4 个格点/站点）
//   2. ForcingReport：标准化层的警告/信息/换算/忽略项/剔除计数记录
//   3. ForcingSource：强迫源纯虚接口（覆盖检查、按窗口加载、取值、小时通量）
// 数据流：era5::Pack（网格）与 StationSource（站点 CSV）实现本接口 →
//   pixel_forcing 按像元取 weights 与 at/hourlyFlux → EB 求解
// 约定：标准量沿用 ERA5 命名、SI 单位；时间一律为自 1970-01-01 UTC 起的整小时数
// 与其他文件：元数据见 forcing_metadata.h；报告实现见 forcing_source.cpp
// ==========================================================================

#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace stream {

struct ForcingMetadata;

// 模块：插值权重。网格源的 index 是已加载窗口内“行×列数+列”的一维偏移（双线性四角），
// 站点源的 index 是站点序号（距离反比加权，最多 4 站）；权重已归一，缺测处理见 at()。
// Interpolation weights of one location: up to four cells (grid corners) or stations. Unused slots have weight 0.
struct ForcingWeights {
    int index[4]{0, 0, 0, 0};
    float weight[4]{0, 0, 0, 0};
};

// What the standardisation layer did with the data of one run (written to the log and scene_summary.json).
struct ForcingReport {
    std::vector<std::string> warnings;         // user-visible warnings (also logged)
    std::vector<std::string> infos;            // e.g. the applied height corrections
    std::vector<std::string> derived;          // standard quantities that were computed from other variables
    std::vector<std::string> ignored;          // declared quantities the energy balance does not use
    std::map<std::string, long long> removed;  // values turned into missing by `missing` / `range`, per quantity
    void warn(const std::string& text);        // adds once
    void info(const std::string& text);        // adds once
};

// 模块：强迫源接口。EB 只通过它读取气象量，不关心数据来自 ERA5 网格还是站点 CSV；
// 调用顺序一般为 covers() 检查 → load() 加载窗口 → weights() 取像元权重 → at()/hourlyFlux() 取值。
// Standardised forcing of the energy balance (ERA5 naming, SI units):
//   t2m (K, 2 m)  d2m (K, dew point)  sp (Pa)  u10 v10 (m s-1, 10 m)  ssrd strd (downward flux; hourlyFlux gives W m-2)
//   optional: skt (K)  swvl1 (m3 m-3)  z (m2 s-2, source orography)
// Implemented by the gridded pack (era5::Pack) and the station source. All times are whole hours since 1970-01-01 UTC.
class ForcingSource {
public:
    virtual ~ForcingSource() = default;
    virtual const ForcingMetadata& metadata() const = 0;  // the metadata the data were read with
    virtual bool standardised() const = 0;                // false: the ERA5 layout, read without any conversion
    virtual const std::string& product() const = 0;       // preset / metadata name
    virtual int year() const = 0;
    virtual bool fullYear() const = 0;                    // every required quantity has all hours of year()
    // Hours before the first needed hour that must also be covered (accumulated fluxes are de-accumulated).
    virtual int64_t leadHours() const = 0;
    virtual bool available(const std::string& variable) const = 0;
    // True when every standard quantity in `variables` can be built for [firstHour, lastHour]; otherwise `gap`
    // names the first missing quantity and hour.
    virtual bool covers(const std::vector<std::string>& variables, int64_t firstHour, int64_t lastHour, std::string& gap) const = 0;
    // Loads [firstHour, lastHour] over the latitude/longitude window (ignored by point sources).
    virtual void load(const std::vector<std::string>& variables, double latitudeMin, double latitudeMax, double longitudeMin,
                      double longitudeMax, int64_t firstHour, int64_t lastHour) = 0;
    virtual ForcingWeights weights(double latitude, double longitude) const = 0;
    // Value at an hour (clamped to the loaded range), NaN-aware weighted mean over the weights.
    virtual float at(const std::string& variable, int64_t hour, const ForcingWeights& weights) const = 0;
    // Mean downward flux (W m-2) over the hour ending at `hour` (variable ssrd or strd).
    virtual float hourlyFlux(const std::string& variable, int64_t hour, const ForcingWeights& weights) const = 0;
    virtual bool loaded(const std::string& variable) const = 0;
    virtual int64_t firstHour() const = 0;
    virtual int64_t lastHour() const = 0;
    virtual const std::vector<std::string>& notes() const = 0;
    virtual const ForcingReport& report() const = 0;
    // Air temperature lapse rate (K m-1) of the DEM elevation correction.
    virtual double lapseRate() const { return 0.0065; }
};

}  // namespace stream
