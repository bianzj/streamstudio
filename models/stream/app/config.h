#pragma once
#include "../physics/energy.h"
#include "../scene/raster.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace stream {

struct SpectralBand {
    std::string name;
    double center = 0, fwhm = 0;
};

enum class TemperatureMode { Rt, Eb };

// One thermal observation used by the inversion: a brightness-temperature (or LST-product) band, its wavelength,
// 1-sigma error and view geometry (absent VZA/VAA reuse the scene view geometry).
enum class ObservationKind { Brightness, Lst };
struct InversionObservation {
    std::string name;
    ObservationKind kind = ObservationKind::Brightness;
    Field value;              // K
    double wavelength = 10.5; // um
    double sigma = 0.5;       // K
    Field vza, vaa;           // degrees
};

// Retrieval of LST (one radiometric temperature) or LSCT (four component temperatures, MAP Gauss-Newton with the
// temperature source of the run as background). The posterior replaces the component temperatures used for imaging.
enum class InversionTarget { None, Lst, Lsct };
struct InversionConfig {
    InversionTarget target = InversionTarget::None;
    std::vector<InversionObservation> observations;
    Field mask;                    // optional; non-zero = cloud / unusable
    double priorSd = 5.0;          // K, background error of each component
    double priorCorrelation = 0.0; // between components of one pixel
    double emissivitySd = 0.01;    // LST: emissivity error propagated into the uncertainty
    int window = 1;                // LSCT: odd s x s window of same-class pixels sharing the components
    int maxIterations = 20;
    double tolerance = 1e-3;       // K
    bool enabled() const { return target != InversionTarget::None; }
};
// Surface orientation used by the shortwave forcing: none (horizontal), one plane for the whole scene,
// a per-pixel slope/aspect facet field from the DEM (facet method), or a statistical hill composite
// (sub-pixel cone facets + density).
enum class TerrainRadiation { None, Single, Facet, Hill, Ridge };

struct ImageConfig {
    std::filesystem::path outputDir, dataDir;
    double overpass = 0;  // seconds since 1970-01-01 UTC
    Field type, lai, soilMoisture, dem, latitude, longitude, vza, vaa;
    // BSM soil brightness of this scene; <= 0 keeps classes.json. The table holds one global value (0.25, the
    // TiRT template setting), which is far too dark for bright arid targets such as the Heihe Gobi.
    double soilBrightness = -1.0;
    bool region = false;
    int startRow = 0, endRow = 0, startColumn = 0, endColumn = 0;
    bool opticalEnabled = false, thermalEnabled = false;
    std::vector<SpectralBand> opticalBands, thermalBands;
    TemperatureMode mode = TemperatureMode::Rt;
    // RT: a 4-band components.tif (Tss, Tsh, Tcs, Tch) or one field per component.
    std::string componentsFile;
    std::array<Field, 4> components;
    // EB
    std::filesystem::path era5Pack;
    bool soilMoistureFromEra5 = true;
    bool elevationCorrection = true;
    TerrainRadiation terrainRadiation = TerrainRadiation::None;
    double terrainSlope = 0.0;   // degrees from the horizontal, single-slope mode
    double terrainAspect = 0.0;  // degrees clockwise from north (downslope), single-slope mode
    Field terrainSlopeField, terrainAspectField, terrainDensityField;  // hill/ridge: slope (deg), strike/aspect (deg), density (0-1)
    double terrainEndFraction = 0.0;  // ridge: end-face share; 0 = infinite ridge (no ends)
    // TIRTEB-aligned energy-balance options (defaults follow input_tirteb.csv).
    double diffuseFraction = 0.2;
    bool enforceSolarLimit = true;
    SoilHeatMethod soilHeatMethod = SoilHeatMethod::Surface;
    bool robustTemperature = true;
    bool soilResistanceFromMoisture = false;
    double soilResistance = 200.0;
    double stomatalG0 = 0.01;
    double minWindSpeed = 0.05;
    double maxAerodynamicResistance = 400.0;
    double referenceHeight = 15.0;
    double heatRoughnessKb = -1.0;  // >0 overrides the per-class value in classes.json
    double soilEmissivity = 0.955;
    // Emissivity of saturated soil. Negative keeps the built-in dry-to-wet gain
    // (soilEmissivity + 0.03, capped at 0.995), see physics/soil_emissivity.h.
    double soilEmissivityWet = -1.0;
    double leafEmissivity = 0.985;
    // Thermal imaging: soil directional emissivity from the Hapke rough-surface model, anchored to the class
    // hemispherical value so the broadband balance is unchanged. "constant" keeps the flat class value.
    bool hapkeSoilEmissivity = true;
    double hapkeSoilK = 1.0;
    // "retrieval" (default): directional surface temperature = physical radiance / fixed class emissivity, the
    // convention of MODIS/AHI/SLSTR LST products, so the view-angle response survives. "directional": normalise by
    // the directional emissivity, which removes the angular signal for an isothermal bare soil.
    bool referenceSurfaceTemperature = true;
    double soilConductivity = 1.55;
    double soilVolumetricCapacity = 1.298e6;
    // "moisture": soil conductivity / heat capacity from the pixel soil moisture (Johansen, coarse soil);
    // "constant": soilConductivity / soilVolumetricCapacity for every soil pixel.
    bool soilThermalFromMoisture = false;
    double soilHeatFraction = 0.25;
    int solarBinWidth = 50;  // nm, energy-balance shortwave grouping (1 = full 1 nm integration)
    int maxIterations = 50;
    int maxStabilityIterations = 60;
    InversionConfig inversion;
    int threads = 0;
};

ImageConfig readImageConfig(const std::string& path);

}  // namespace stream


