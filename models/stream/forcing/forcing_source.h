#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace stream {

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

// Standardised forcing of the energy balance (ERA5 naming, SI units):
//   t2m (K, 2 m)  d2m (K, dew point)  sp (Pa)  u10 v10 (m s-1, 10 m)  ssrd strd (downward flux; hourlyFlux gives W m-2)
//   optional: skt (K)  swvl1 (m3 m-3)  z (m2 s-2, source orography)
// Implemented by the gridded pack (era5::Pack) and the station source. All times are whole hours since 1970-01-01 UTC.
class ForcingSource {
public:
    virtual ~ForcingSource() = default;
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
