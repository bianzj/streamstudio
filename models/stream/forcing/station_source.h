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
    // Reads and validates the CSV files; throws std::runtime_error with the standard messages.
    StationSource(const ForcingMetadata& metadata, const std::vector<std::filesystem::path>& files);

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

    // The station a header list of a CSV would need: used by forcing-check to validate files without running.
    static std::vector<std::string> readHeader(const std::filesystem::path& file, const ForcingMetadata& metadata);

private:
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
};

}  // namespace stream
