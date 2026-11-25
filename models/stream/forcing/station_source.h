// ==========================================================================
// 文件：station_source.h
// 功能：站点 CSV 气象强迫源 StationSource 的声明（实现见 station_source.cpp）
// 主要内容：
//   1. StationSource：实现 ForcingSource 接口，读入一个或多个站点 CSV，重采样到整点，
//      按最近站或距离反比（IDW，每个位置最多 4 站）做空间插值
//   2. 内部结构 Series / Station：每站每通道的 SI 时间序列与站点坐标、海拔
//   3. readHeader：只读表头，供 forcing-check 校验列名
// 关系：元数据由 forcing_metadata.h 提供；接口见 forcing_source.h；规格见 docs/forcing-metadata.md 3.3
// 约定：standardised() 恒为 true（输出已是 ERA5 命名的 SI 标准量）；fullYear() 恒为 false
// ==========================================================================

#pragma once
// Station CSV forcing (docs/forcing-metadata.md 3.3): one or several stations, resampled to the whole-hour grid and interpolated
// in space by nearest station or inverse-distance weights (at most four stations per location).
#include "forcing_metadata.h"
#include "forcing_source.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace stream {

class StationSource : public ForcingSource {
public:
    // 构造：读入并校验全部 CSV（失败即抛 std::runtime_error）。
    // Reads and validates the CSV files; throws std::runtime_error with the standard messages.
    StationSource(const ForcingMetadata& metadata, const std::vector<std::filesystem::path>& files);

    // 以下为 ForcingSource 接口的实现；loaded/first/lastHour 描述 load() 已加载的窗口。
    const ForcingMetadata& metadata() const override { return meta_; }
    bool standardised() const override { return true; }
    const std::string& product() const override { return meta_.name; }
    int year() const override { return year_; }
    bool fullYear() const override { return false; }
    int64_t leadHours() const override;
    bool available(const std::string& variable) const override;
    bool covers(const std::vector<std::string>& variables, int64_t firstHour, int64_t lastHour, std::string& gap) const override;
    void load(const std::vector<std::string>& variables, double latitudeMin, double latitudeMax, double longitudeMin,
              double longitudeMax, int64_t firstHour, int64_t lastHour) override;
    ForcingWeights weights(double latitude, double longitude) const override;
    float at(const std::string& variable, int64_t hour, const ForcingWeights& weights) const override;
    float hourlyFlux(const std::string& variable, int64_t hour, const ForcingWeights& weights) const override;
    bool loaded(const std::string& variable) const override { return data_.count(variable) > 0; }
    int64_t firstHour() const override { return firstHour_; }
    int64_t lastHour() const override { return lastHour_; }
    const std::vector<std::string>& notes() const override { return notes_; }
    const ForcingReport& report() const override { return report_; }
    double lapseRate() const override { return meta_.lapseRateKPerKm / 1000.0; }
    size_t stationCount() const { return stations_.size(); }

    // 注：返回 CSV 表头的列名列表（不加载数据）。
    // The station a header list of a CSV would need: used by forcing-check to validate files without running.
    static std::vector<std::string> readHeader(const std::filesystem::path& file, const ForcingMetadata& metadata);

private:
    // 内部数据：Series 为按时间排序的原始记录（times 为自 1970 起的小时数，values 为 SI）；
    //       Station 保存站点 id、来源文件、经纬度（度）、海拔（m）与各通道序列；
    //       data_ 为 load() 后的标准量（[小时 × 站点数 + 站点]，float）。
    struct Series { std::vector<double> times, values; };  // values in SI
    struct Station {
        std::string id, file;
        double latitude = 0, longitude = 0, elevation = 0;
        bool hasElevation = false;
        std::map<std::string, Series> channels;  // key: ta hum p u v speed dir sw lw skt swvl
    };
    ForcingMetadata meta_;
    ForcingReport report_;
    std::vector<Station> stations_;
    std::map<std::string, std::vector<float>> data_;
    std::vector<std::string> notes_;
    int year_ = 0;
    int64_t firstHour_ = 0, lastHour_ = -1;
    bool anyElevation_ = false;
    std::vector<std::string> keysFor(const std::vector<std::string>& variables) const;
    bool hasKey(const std::string& key) const;
    const ForcingChannel* channelOf(const std::string& key) const;
    double stepOf(const Station& station, const std::string& key) const;
    // 单站某通道的记录能否覆盖窗口（covers 与 load 的逐站警告共用）。
    bool stationCovers(const Station& station, const std::string& key, int64_t firstHour, int64_t lastHour, std::string& why) const;
};

}  // namespace stream
