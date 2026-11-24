#pragma once
// Weather forcing metadata (docs/forcing-metadata.md): maps any source (ERA5, ERA5-Land, GLDAS, station CSV, ...) to the
// standard quantities of the energy balance. This file holds the parsed structure, the presets, the unit tables, the
// humidity / wind-height conversions and the validation messages; the data readers live in era5.cpp / station_source.cpp.
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace stream {

constexpr double kForcingStandardGravity = 9.80665;

// Errors and warnings collected while a metadata document is parsed / checked against data.
struct ForcingIssues {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    void error(const std::string& text);
    void warning(const std::string& text);
    bool ok() const { return errors.empty(); }
    std::string errorText() const;  // all errors, one per line
};

// One physical quantity of the source (a column or a NetCDF variable).
struct ForcingChannel {
    bool declared = false;
    std::vector<std::string> source;  // names / aliases, the first one present in the data is used
    std::string unit;                 // canonical spelling of the declared unit
    double unitFactor = 1.0;          // value in SI = (raw * scale + offset) * unitFactor + unitOffset
    double unitOffset = 0.0;
    bool energyUnit = false;          // radiation: J m-2 like (needs kind accumulated / cumulative)
    double scale = 1.0, offset = 0.0;
    std::string kind;                 // instant | previous | interval_mean | accumulated | cumulative
    std::string stamp;                // instant | interval_start | interval_end | interval_mid
    int resetHourUtc = 0;
    double stepHours = 0.0;           // 0: time.stepHours / detected
    double height_m = 0.0;            // observation height; heightDeclared tells whether it was written
    bool heightDeclared = false;
    std::vector<double> missing;
    bool hasRange = false;
    double rangeMin = 0.0, rangeMax = 0.0;
    double thickness_m = 0.0;         // soilMoisture in kg m-2
    bool identity() const { return scale == 1.0 && offset == 0.0 && unitFactor == 1.0 && unitOffset == 0.0; }
};

struct ForcingStationEntry {
    std::string id, file;
    double latitude = 0.0, longitude = 0.0, elevation_m = 0.0;
    bool hasElevation = false;
};

struct ForcingMetadata {
    std::string name, description, preset, form;  // form: gridded | station
    std::string origin;                           // preset | file | inline | pack
    std::filesystem::path path;                   // metadata file (origin file / pack), else empty
    std::filesystem::path baseDir;                // relative station files are resolved against it
    // time
    std::vector<std::string> timeVariables{"valid_time", "time"};
    std::string timeUnits;                        // overrides the NetCDF units attribute
    std::string timeColumn = "time", timeFormat = "iso8601";
    double stepHours = 0.0;                       // 0: detect
    double utcOffsetHours = 0.0;
    double maxGapHours = 0.0;                     // 0: max(6, 2 x step)
    // grid
    std::vector<std::string> latitudeNames{"latitude", "lat"}, longitudeNames{"longitude", "lon"};
    // station
    std::string delimiter = ",", comment = "#";
    std::string idColumn, latitudeColumn, longitudeColumn, elevationColumn;
    std::vector<ForcingStationEntry> stations;
    // variables (see the document, section 4)
    ForcingChannel airTemperature, pressure, shortwaveDown, longwaveDown, skinTemperature, soilMoisture, orography,
        precipitation, directShortwave, diffuseShortwave;
    ForcingChannel humidity;                      // type below
    ForcingChannel windU, windV, windSpeed, windDirection;
    std::string humidityType = "dewpoint";        // dewpoint | specific | mixingRatio | relative | vapourPressure
    std::string pressureType = "surface";         // surface | seaLevel | none
    std::string windType = "components";          // components | speedDirection | speed
    std::string windConvention = "from";          // from | to
    double windHeight_m = 10.0;
    bool windHeightDeclared = false;
    std::string longwaveFallback = "none";        // none | brutsaert
    // height correction
    std::string windMode = "log", temperatureMode = "none";
    double windTargetHeight_m = 10.0, temperatureTargetHeight_m = 2.0, roughness_m = 0.03, lapseRatePerM = 0.0065;
    double windFactor = 1.0;                      // applied to every wind component (1 = none)
    double temperatureShiftK = 0.0;               // added to the air temperature (0 = none)
    // spatial
    std::string interpolation;                    // station: nearest | idw
    double idwPower = 2.0, maxDistanceKm = 0.0, lapseRateKPerKm = 6.5;
    int maxStations = 4;
    // optional quantities present in the document but not used by the energy balance
    std::vector<std::string> ignored;
    std::vector<std::string> notes;               // informational (height corrections) and warnings of the parse
    std::vector<std::string> warnings;

    bool station() const { return form == "station"; }
    // True for the identity ERA5 / ERA5-Land layout (names t2m ... with SI units): the data go through unchanged.
    bool era5Layout() const;
};

// ---- presets ----
const std::vector<std::string>& forcingPresetNames();
// Full JSON text of a preset; throws std::runtime_error with the standard message for an unknown name.
std::string forcingPresetJson(const std::string& name);

// ---- parsing ----
// Parses JSON text (the preset key and the overrides are merged first). `origin`: preset | file | inline | pack.
ForcingMetadata parseForcingMetadata(const std::string& jsonText, const std::string& origin, const std::filesystem::path& path,
                                     ForcingIssues& issues);
// A preset name, a JSON file path (relative paths resolve against baseDir) or JSON text; throws on any error.
ForcingMetadata resolveForcingMetadata(const std::string& specifier, const std::filesystem::path& baseDir, ForcingIssues* issues = nullptr);
// The metadata a legacy `temperature.era5.pack` implies: the preset for the pack product.
ForcingMetadata forcingMetadataForProduct(const std::string& product);
// Declared quantities that the data does not contain; `available(name)` tells whether a source name exists.
// Adds the errors of the second validation step (data side): missing required variables / columns.
void checkForcingAgainstNames(const ForcingMetadata& metadata, const std::vector<std::string>& dataNames,
                              ForcingIssues& issues);
// Canonical metadata as JSON text (what the run records in scene_summary.json under temperature.forcing.metadata).
std::string forcingMetadataSummaryJson(const ForcingMetadata& metadata);

// ---- conversions (shared by the readers) ----
// Looks up a unit of a quantity class (temperature, pressure, fraction, relativeHumidity, speed, direction, power, energy,
// precipRate, precipDepth, soilMoisture, orography). Returns false for unknown spellings.
bool forcingUnit(const std::string& quantity, const std::string& spelling, std::string& canonical, double& factor, double& offset);
std::string forcingUnitList(const std::string& quantity);
double forcingSaturationVapourPressure(double celsius);  // hPa, the formula of the energy balance
double forcingDewPointFromVapourPressure(double hPa);    // K; NaN for hPa <= 0
double forcingVapourPressureFromSpecific(double q, double pressurePa);   // hPa
double forcingVapourPressureFromMixingRatio(double r, double pressurePa);  // hPa
double forcingPressureFromAltitude(double altitudeM);    // Pa, standard atmosphere
double forcingPressureFromSeaLevel(double seaLevelPa, double altitudeM, double airTemperatureK);  // Pa
double forcingBrutsaertLongwave(double airTemperatureK, double vapourPressureHPa);  // W m-2
// Channel of a standard quantity name (t2m, d2m, sp, u10, v10, ssrd, strd, skt, swvl1, z) as used by readers.
bool forcingStandardQuantity(const std::string& name);

}  // namespace stream
