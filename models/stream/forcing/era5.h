#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace stream::era5 {

// Required for every EB run; swvl1 additionally when soil moisture comes from ERA5.
const std::vector<std::string>& requiredVariables();
// Read when present: deep soil temperature, soil moisture and the surface geopotential.
const std::vector<std::string>& optionalVariables();

// Scans ERA5 / ERA5-Land hourly NetCDF files covering any period (one day up to a calendar year) and
// writes the pack index (kind stream-era5-pack). Returns true when every required variable is hourly
// continuous over the common period.
bool registerPack(const std::vector<std::string>& files, const std::filesystem::path& index, const std::string& product);

struct Weights {
    int index[4]{0, 0, 0, 0};
    float weight[4]{0, 0, 0, 0};
};

// One pack, read over a latitude/longitude window and a range of UTC hours.
class Pack {
public:
    void open(const std::filesystem::path& index);
    int year() const { return year_; }
    // Every required variable has all hours of year().
    bool fullYear() const { return fullYear_; }
    // True when every timed variable in `variables` has each hour of [firstHour, lastHour]; otherwise `gap`
    // names the first variable and hour that are missing.
    bool covers(const std::vector<std::string>& variables, int64_t firstHour, int64_t lastHour, std::string& gap) const;
    bool landProduct() const { return land_; }
    const std::string& product() const { return product_; }
    bool available(const std::string& variable) const;
    // Loads [firstHour, lastHour] (hours since 1970-01-01 UTC); hours outside the pack reuse the nearest one.
    void load(const std::vector<std::string>& variables, double latitudeMin, double latitudeMax, double longitudeMin,
              double longitudeMax, int64_t firstHour, int64_t lastHour);
    Weights weights(double latitude, double longitude) const;
    // Value at an hour (clamped to the loaded range), NaN-aware bilinear in space.
    float at(const std::string& variable, int64_t hour, const Weights& weights) const;
    // Mean downward flux (W m-2) over the hour ending at `hour`, de-accumulating ERA5-Land.
    float hourlyFlux(const std::string& variable, int64_t hour, const Weights& weights) const;
    bool loaded(const std::string& variable) const { return data_.count(variable) > 0; }
    int64_t firstHour() const { return firstHour_; }
    int64_t lastHour() const { return lastHour_; }
    const std::vector<std::string>& notes() const { return notes_; }

private:
    struct Source { int file = -1; size_t time = 0; };
    std::vector<std::string> files_;
    std::map<std::string, std::map<int64_t, Source>> sources_;
    std::map<std::string, std::vector<float>> data_;  // [hour][row][column] in the window
    std::map<std::string, std::vector<char>> exact_;  // per loaded hour: read from the pack (not filled in)
    std::vector<std::string> notes_;
    std::string product_;
    bool land_ = false, fullYear_ = false;
    int year_ = 0;
    double latitude0_ = 0, latitudeStep_ = 0, longitude0_ = 0, longitudeStep_ = 0;
    int latitudeCount_ = 0, longitudeCount_ = 0;
    bool globalLongitude_ = false, longitude360_ = false;
    int row0_ = 0, rows_ = 0, column0_ = 0, columns_ = 0;
    int64_t firstHour_ = 0, lastHour_ = -1;
    float sample(const std::vector<float>& values, size_t offset, const Weights& weights) const;
};

}  // namespace stream::era5
