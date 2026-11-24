#include "assimilation.h"
#include "parallel_error.h"
#include "inversion.h"  // QA bit constants shared with the LST/LSCT retrieval
#include "../forcing/utc.h"  // FIX(2026-11-21 C7): overpass day of year
#include "../physics/canopy.h"
#include "../physics/dual.h"
#include "../physics/shortwave.h"
#include "../physics/observation_errors.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace stream {
namespace {

// ---- built-in defaults (not exposed in the workbench) ----
constexpr double kTemperatureSd = 5.0;      // K, background error of each component
constexpr double kTemperatureCorrelation = 0.5;
constexpr double kMoistureSd = 0.10, kMoistureLow = 0.02, kMoistureHigh = 0.60;  // m3 m-3
constexpr double kVcmaxRelativeSd = 0.3, kVcmaxMinSd = 10.0, kVcmaxLow = 5.0, kVcmaxHigh = 250.0;
constexpr double kLaiRelativeSd = 0.3, kLaiMinSd = 0.5, kLaiHigh = 10.0;
constexpr double kTemperatureLow = 150.0, kTemperatureHigh = 400.0;  // search domain, K
constexpr double kValidLow = 200.0, kValidHigh = 380.0;              // plausible analysis, K
constexpr int kMaxIterationsRt = 20, kMaxIterationsEb = 10;
constexpr double kStepTolerance = 1e-3;  // converged when every step < 1e-3 of its background sd (5e-3 K for temperatures)
constexpr double kLowSupportDfs = 0.2;
constexpr int kMoistureSteps = 1000;
constexpr int kMaxControls = 7;
static_assert(kMaxControls <= kAdSize, "controls exceed the dual-number size");

enum ControlKind { kTss = 0, kTsh = 1, kTcs = 2, kTch = 3, kMoisture = 4, kVcmax = 5, kLai = 6, kKinds = 7 };
const char* kControlNames[kKinds] = {"Tss", "Tsh", "Tcs", "Tch", "soil moisture", "Vcmax25", "LAI"};

void progress(double percent, const std::string& stage)
{
    std::cout << "STREAM\tprogress\t" << std::fixed << std::setprecision(1) << percent << '\t' << stage << '\n';
}
void log(const std::string& text) { std::cout << "[STREAM] " << text << '\n'; }
std::string format(double value, int digits = 2)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(digits) << value;
    return stream.str();
}
bool isMainThread()
{
#ifdef _OPENMP
    return omp_get_thread_num() == 0;
#else
    return true;
#endif
}

using Matrix = std::array<std::array<double, kMaxControls>, kMaxControls>;

// Gauss-Jordan inverse with partial pivoting of the leading n x n block.
bool invert(Matrix& a, int n)
{
    Matrix inverse{};
    for (int i = 0; i < n; ++i) inverse[i][i] = 1.0;
    for (int column = 0; column < n; ++column) {
        int pivot = column;
        for (int row = column + 1; row < n; ++row)
            if (std::abs(a[row][column]) > std::abs(a[pivot][column])) pivot = row;
        if (!(std::abs(a[pivot][column]) > 1e-14)) return false;
        std::swap(a[pivot], a[column]);
        std::swap(inverse[pivot], inverse[column]);
        const double diagonal = a[column][column];
        for (int k = 0; k < n; ++k) { a[column][k] /= diagonal; inverse[column][k] /= diagonal; }
        for (int row = 0; row < n; ++row) {
            if (row == column) continue;
            const double factor = a[row][column];
            if (factor == 0.0) continue;
            for (int k = 0; k < n; ++k) { a[row][k] -= factor * a[column][k]; inverse[row][k] -= factor * inverse[column][k]; }
        }
    }
    a = inverse;
    return true;
}

enum class Source { SceneBrightness, ViewBrightness, Lst, Component, Reflectance };
struct Observation {
    Source source;
    int band = 0, view = -1;
    double y = 0, sigma = 1;
    bool thermal = true;
};

std::vector<std::vector<float>> readBands(const std::string& file, int count, const Grid& grid, const std::string& label)
{
    auto dataset = openRaster(file, label);
    if (dataset->GetRasterCount() < count)
        throw std::runtime_error(label + "须有 " + std::to_string(count) + " 个波段，实际 " + std::to_string(dataset->GetRasterCount()));
    std::vector<std::vector<float>> bands(count);
    for (int band = 0; band < count; ++band) bands[band] = readBand(*dataset, band + 1, grid, label + " 波段 " + std::to_string(band + 1));
    return bands;
}

// One band may be shared by all observations; otherwise the band counts must match.
std::vector<std::vector<float>> optionalBands(const std::string& file, int count, const Grid& grid, const std::string& label)
{
    if (file.empty()) return {};
    auto dataset = openRaster(file, label);
    const int bands = dataset->GetRasterCount();
    if (bands != 1 && bands != count) throw std::runtime_error(label + " must have 1 or " + std::to_string(count) + " bands");
    std::vector<std::vector<float>> result(count);
    for (int b = 0; b < count; ++b) result[b] = readBand(*dataset, bands == 1 ? 1 : b + 1, grid, label);
    return result;
}

bool acceptedQa(const std::vector<std::vector<float>>& qa, int band, size_t pixel, unsigned reject)
{
    if (qa.empty()) return true;
    const double value = qa[band][pixel];
    return std::isfinite(value) && value >= 0 && value <= 65535 && std::floor(value) == value &&
           (static_cast<unsigned>(value) & reject) == 0;
}

double sigmaAt(const std::vector<std::vector<float>>& raster, int band, size_t pixel, double fallback)
{
    return raster.empty() ? fallback : raster[band][pixel];  // invalid supplied errors never use the default
}

}  // namespace

AssimilationResult runAssimilation(const AssimilationInputs& in, const std::array<std::vector<float>, 4>& background)
{
    const ImageConfig& config = in.config;
    const AssimilationConfig& as = config.assimilation;
    const Grid& grid = in.grid;
    const size_t pixels = grid.size();
    const bool eb = in.eb;
    const AssimilationObservation kind = as.observation;

    progress(in.progressStart, "读取同化观测");
    std::vector<double> brightnessWavelengths;
    for (const auto& band : config.thermalBands) brightnessWavelengths.push_back(band.center);
    const int dataBands = kind == AssimilationObservation::Brightness ? static_cast<int>(brightnessWavelengths.size())
                        : kind == AssimilationObservation::Lst ? 1 : 4;
    const std::string dataLabel = kind == AssimilationObservation::Brightness ? "亮温观测"
                                : kind == AssimilationObservation::Lst ? "地表温度观测" : "组分温度观测";
    const auto data = readBands(as.data, dataBands, grid, dataLabel);
    const auto errors = optionalBands(as.uncertainty, dataBands, grid, "observation uncertainty");
    std::vector<std::vector<float>> covarianceRaster;
    if (!as.covariance.empty()) {
        auto dataset = openRaster(as.covariance, "observation covariance");
        if (dataset->GetRasterCount() != dataBands * dataBands)
            throw std::runtime_error("observation covariance needs N*N row-major bands (K^2)");
        covarianceRaster = readBands(as.covariance, dataBands * dataBands, grid, "observation covariance");
    }
    const auto qa = optionalBands(as.qa, dataBands, grid, "observation QA");
    const double defaultSigma = kind == AssimilationObservation::Brightness ? as.sigmaBrightnessK
                              : kind == AssimilationObservation::Lst ? as.sigmaLstK : as.sigmaComponentK;
    std::vector<std::vector<std::vector<float>>> viewData;
    std::vector<std::vector<std::vector<float>>> viewErrors, viewQa;
    std::vector<std::vector<float>> viewVza, viewVaa;
    for (size_t v = 0; v < as.views.size(); ++v) {
        const std::string label = "附加视角 " + std::to_string(v + 1) + " 亮温";
        viewData.push_back(readBands(as.views[v].data, dataBands, grid, label));
        viewErrors.push_back(optionalBands(as.views[v].uncertainty, dataBands, grid, label + " uncertainty"));
        viewQa.push_back(optionalBands(as.views[v].qa, dataBands, grid, label + " QA"));
        viewVza.push_back(fieldRaster(as.views[v].vza, grid, label + " VZA", 0, true));
        viewVaa.push_back(fieldRaster(as.views[v].vaa, grid, label + " VAA", 0, true));
    }
    const bool withOptical = !as.optical.empty();
    std::vector<std::vector<float>> reflectance;
    if (withOptical) reflectance = readBands(as.optical, static_cast<int>(config.opticalBands.size()), grid, "光学反射率观测");
    const auto opticalErrors = optionalBands(as.opticalUncertainty, static_cast<int>(config.opticalBands.size()), grid, "reflectance uncertainty");
    const auto opticalQa = optionalBands(as.opticalQa, static_cast<int>(config.opticalBands.size()), grid, "reflectance QA");
    const std::vector<double> lstWavelength{as.lstWavelengthUm};
    const int maxIterations = eb ? kMaxIterationsEb : kMaxIterationsRt;
    // FIX(2026-11-21 C7): day of year of the overpass for the orbital factor of the Erbs clearness index.
    const UtcTime overpassTime = utcFromSeconds(in.overpass);
    const int dayOfYear = static_cast<int>(daysFromCivil(overpassTime.year, overpassTime.month, overpassTime.day) -
                                           daysFromCivil(overpassTime.year, 1, 1)) + 1;

    AssimilationResult result;
    for (int k = 0; k < 4; ++k) result.components[k] = background[k];
    result.lai.assign(pixels, kNaN);
    result.soilMoisture.assign(pixels, kNaN);
    result.vcmax.assign(pixels, kNaN);
    result.analysed.assign(pixels, 0);
    std::vector<std::vector<float>> uncertainty(kKinds, std::vector<float>(pixels, kNaN));
    // FIX(2026-11-21 S-5 QA): bands 8-9 (1-based) = MAP cost at the background / at the analysis (appended).
    std::vector<std::vector<float>> qaBands(9, std::vector<float>(pixels, kNaN));  // flags, O-B, O-A, O-A rms, DFS, n, iterations, J_b, J_a
    std::vector<std::vector<float>> backgroundControls(3, std::vector<float>(pixels, kNaN));
    std::vector<std::vector<float>> fluxes(eb ? 4 : 0, std::vector<float>(pixels, kNaN));

    long long analysed = 0, noObservation = 0, failed = 0, notConverged = 0, illConditioned = 0, lowSupport = 0, observationsUsed = 0;
    // FIX(2026-11-21 S-5 QA): stop reasons of the analysed pixels
    long long stopConverged = 0, stopStalled = 0, stopNotUpdated = 0, stopIterationLimit = 0, stopSingular = 0, stopStepFailed = 0;
    double sumOmb = 0, sumOma = 0, sumDfs = 0;
    long long thermalPixels = 0;
    std::atomic<int> rowsDone{0};
    const int rowCount = in.endRow - in.startRow;

    ParallelError errorsAssimilation;
    #pragma omp parallel for schedule(dynamic, 2) reduction(+:analysed, noObservation, failed, notConverged, illConditioned, lowSupport, observationsUsed, sumOmb, sumOma, sumDfs, thermalPixels, stopConverged, stopStalled, stopNotUpdated, stopIterationLimit, stopSingular, stopStepFailed)
    for (int row = in.startRow; row < in.endRow; ++row) {
        if (errorsAssimilation.failed()) continue;
        try {
        for (int column = in.startColumn; column < in.endColumn; ++column) {
            const size_t pixel = static_cast<size_t>(row) * grid.width + column;
            const auto* entry = in.classes.find(in.type[pixel]);
            if (!entry || entry->surface != SurfaceKind::Vegetation) continue;  // land pixels only
            unsigned flags = 0;
            const auto finish = [&](unsigned value) { qaBands[0][pixel] = static_cast<float>(value); };
            const float lat = in.latitude[pixel], lon = in.longitude[pixel];
            const float vz = in.vza[pixel], va = in.vaa[pixel], leafArea = in.lai[pixel];
            if (!(std::isfinite(lat) && std::isfinite(lon) && std::abs(lat) <= 90 && std::abs(lon) <= 180) ||
                !(vz >= 0 && vz < 90) || !(va >= -360 && va <= 360) || !validLai(leafArea)) {  // FIX(2026-11-24 M1): LAI <= kMaxLai (20), nodata invalid
                finish(kQaInputInvalid);
                continue;
            }
            const Sun sun = solarPosition(in.overpass, lat, lon, in.dem[pixel]);
            if (!sun.ok) { finish(kQaInputInvalid); continue; }
            const int code = static_cast<int>(std::lround(in.type[pixel]));
            const double moistureB = std::isfinite(in.soilMoisture[pixel]) ? in.soilMoisture[pixel] : in.classes.defaultSoilMoisture;
            const double vcmaxB = entry->photosynthesis.vcmax;

            // ---- scenes and observations ----
            ThermalScene scene;
            const bool sceneOk = thermalSceneAt(in.terrain, pixel, sun.zenith, sun.azimuth, vz, va, scene);
            setThermalMaterial(scene, config, *entry, in.soilEmissivity, code);
            const double skyTemperature = in.skyLongwave ? skyTemperatureFromLongwave((*in.skyLongwave)[pixel]) : NAN;
            scene.skyTemperature = skyTemperature;
            std::vector<ThermalScene> viewScenes(as.views.size());
            std::vector<char> viewOk(as.views.size(), 0);
            for (size_t v = 0; v < as.views.size(); ++v) {
                const float z = viewVza[v][pixel], a = viewVaa[v][pixel];
                if (!(z >= 0 && z < 90) || !(a >= -360 && a <= 360)) continue;
                viewOk[v] = thermalSceneAt(in.terrain, pixel, sun.zenith, sun.azimuth, z, a, viewScenes[v]) ? 1 : 0;
                setThermalMaterial(viewScenes[v], config, *entry, in.soilEmissivity, code);
                viewScenes[v].skyTemperature = skyTemperature;
            }
            std::vector<Observation> obs;
            const auto temperatureValid = [](float y) { return std::isfinite(y) && y >= kTemperatureLow && y <= kTemperatureHigh; };
            const auto addThermal = [&](Source source, int band, int view, double value,
                                        const auto& errorRaster, const auto& qaRaster) {
                const double sigma = sigmaAt(errorRaster, band, pixel, defaultSigma);
                if (temperatureValid(static_cast<float>(value)) && std::isfinite(sigma) && sigma > 0 &&
                    acceptedQa(qaRaster, band, pixel, as.rejectQaBits))
                    obs.push_back({source, band, view, value, sigma, true});
            };
            if (kind == AssimilationObservation::Brightness) {
                if (sceneOk)
                    for (int b = 0; b < dataBands; ++b)
                        addThermal(Source::SceneBrightness, b, -1, data[b][pixel], errors, qa);
                for (size_t v = 0; v < as.views.size(); ++v) {
                    if (!viewOk[v]) continue;
                    for (int b = 0; b < dataBands; ++b)
                        addThermal(Source::ViewBrightness, b, static_cast<int>(v), viewData[v][b][pixel], viewErrors[v], viewQa[v]);
                }
            } else if (kind == AssimilationObservation::Lst) {
                if (sceneOk) addThermal(Source::Lst, 0, -1, data[0][pixel], errors, qa);
            } else {
                for (int k = 0; k < 4; ++k)
                    addThermal(Source::Component, k, -1, data[k][pixel], errors, qa);
            }
            bool opticalPixel = false;
            if (withOptical && sun.zenith < 90.0 && in.leafBandReflectance.count(code) > 0)
                for (size_t b = 0; b < config.opticalBands.size(); ++b) {
                    const float y = reflectance[b][pixel];
                    const double sigma = sigmaAt(opticalErrors, static_cast<int>(b), pixel, as.sigmaReflectance);
                    if (std::isfinite(y) && y >= 0.0f && y <= 1.0f && std::isfinite(sigma) && sigma > 0 &&
                        acceptedQa(opticalQa, static_cast<int>(b), pixel, as.rejectQaBits)) {
                        obs.push_back({Source::Reflectance, static_cast<int>(b), -1, y, sigma, false});
                        opticalPixel = true;
                    }
                }

            // ---- controls and background covariance ----
            int n = 0;
            std::array<int, kKinds> index;
            index.fill(-1);
            std::array<int, kMaxControls> kindOf{};
            std::array<double, kMaxControls> xb{}, sd{}, low{}, high{};
            const auto add = [&](int which, double value, double sigma, double lo, double hi) {
                index[which] = n;
                kindOf[n] = which;
                xb[n] = value;
                sd[n] = sigma;
                low[n] = std::min(lo, value);
                high[n] = std::max(hi, value);
                ++n;
            };
            if (!eb) {
                for (int k = 0; k < 4; ++k)
                    if (temperatureValid(background[k][pixel])) add(k, background[k][pixel], kTemperatureSd, kTemperatureLow, kTemperatureHigh);
            } else {
                add(kMoisture, moistureB, kMoistureSd, kMoistureLow, kMoistureHigh);
                if (leafArea > 0 && (entry->photosynthesis.pathway == 3 || entry->photosynthesis.pathway == 4))
                    add(kVcmax, vcmaxB, std::max(kVcmaxMinSd, kVcmaxRelativeSd * vcmaxB), kVcmaxLow, kVcmaxHigh);
            }
            if (opticalPixel) add(kLai, leafArea, std::max(kLaiMinSd, kLaiRelativeSd * leafArea), 0.0, kLaiHigh);
            backgroundControls[0][pixel] = static_cast<float>(moistureB);
            backgroundControls[1][pixel] = static_cast<float>(vcmaxB);
            backgroundControls[2][pixel] = leafArea;
            if (obs.empty() || n == 0) { ++noObservation; finish(kQaInputInvalid); qaBands[5][pixel] = 0; continue; }

            // Covariance covers the primary thermal observation. Additional views
            // and optical observations remain independent blocks with their own sigma.
            std::vector<size_t> correlated;
            std::vector<double> precision;
            if (!covarianceRaster.empty()) {
                for (size_t i = 0; i < obs.size(); ++i)
                    if (obs[i].thermal && obs[i].source != Source::ViewBrightness) correlated.push_back(i);
                const int nc = static_cast<int>(correlated.size());
                std::vector<double> covariance(nc * nc);
                for (int i = 0; i < nc; ++i) for (int j = 0; j < nc; ++j)
                    covariance[i * nc + j] = covarianceRaster[obs[correlated[i]].band * dataBands + obs[correlated[j]].band][pixel];
                if (!observationPrecision(covariance, nc, precision)) {
                    ++failed; finish(kQaInputInvalid | kQaIllConditioned); continue;
                }
            }
            const auto applyPrecision = [&](const std::vector<double>& values) {
                std::vector<double> weighted(values.size());
                for (size_t i = 0; i < values.size(); ++i) weighted[i] = values[i] / (obs[i].sigma * obs[i].sigma);
                const size_t nc = correlated.size();
                for (size_t i = 0; i < nc; ++i) {
                    weighted[correlated[i]] = 0.0;
                    for (size_t j = 0; j < nc; ++j) weighted[correlated[i]] += precision[i * nc + j] * values[correlated[j]];
                }
                return weighted;
            };

            Matrix backgroundInverse{};
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    const bool temperatures = kindOf[i] < 4 && kindOf[j] < 4;
                    backgroundInverse[i][j] = i == j ? sd[i] * sd[i]
                                            : temperatures ? kTemperatureCorrelation * sd[i] * sd[j] : 0.0;
                }
            if (!invert(backgroundInverse, n)) { ++failed; finish(kQaIllConditioned); continue; }

            // ---- observation operator with exact derivatives ----
            EbOutcome outcome;
            std::vector<Ad> h(obs.size());
            const auto evaluate = [&](const std::array<double, kMaxControls>& x) {
                const auto value = [&](int which, double fallback) {
                    const int c = index[which];
                    return c >= 0 ? Ad::seed(x[c], c) : Ad(fallback);
                };
                const Ad laiAd = value(kLai, leafArea);
                const Ad moistureAd = eb ? value(kMoisture, moistureB) : Ad(moistureB);
                std::array<Ad, 4> temperatures;
                if (!eb) {
                    for (int k = 0; k < 4; ++k) temperatures[k] = value(k, background[k][pixel]);
                } else {
                    const EbControls controls{moistureAd.v, index[kVcmax] >= 0 ? x[index[kVcmax]] : vcmaxB, laiAd.v};
                    if (!in.ebSolver || !in.ebSolver(pixel, controls, outcome)) return false;
                    if (!outcome.gradientValid) { flags |= kQaIllConditioned; return false; }
                    for (int k = 0; k < 4; ++k) {
                        Ad t(outcome.temperatures[k]);
                        for (int c = 0; c < n; ++c)
                            if (kindOf[c] >= kMoisture) t.d[c] = outcome.gradient[k][kindOf[c] - kMoisture];
                        temperatures[k] = t;
                    }
                }
                std::vector<Ad> sceneBrightness, sceneSurface, viewBrightness, viewSurface;
                bool sceneDone = false;
                std::vector<std::vector<Ad>> views(as.views.size());
                double diffuse = -1.0;
                for (size_t i = 0; i < obs.size(); ++i) {
                    const Observation& o = obs[i];
                    switch (o.source) {
                        case Source::SceneBrightness:
                            if (!sceneDone) {
                                if (!thermalPixel<Ad>(scene, temperatures, laiAd, moistureAd, brightnessWavelengths, sceneBrightness, sceneSurface)) return false;
                                sceneDone = true;
                            }
                            h[i] = sceneBrightness[o.band];
                            break;
                        case Source::ViewBrightness:
                            if (views[o.view].empty() &&
                                !thermalPixel<Ad>(viewScenes[o.view], temperatures, laiAd, moistureAd, brightnessWavelengths, views[o.view], viewSurface))
                                return false;
                            h[i] = views[o.view][o.band];
                            break;
                        case Source::Lst:
                            if (!thermalPixel<Ad>(scene, temperatures, laiAd, moistureAd, lstWavelength, sceneBrightness, sceneSurface)) return false;
                            h[i] = sceneSurface[0];
                            break;
                        case Source::Component:
                            if (!ad::isfinite(temperatures[o.band])) return false;
                            h[i] = temperatures[o.band];
                            break;
                        case Source::Reflectance: {
                            if (diffuse < 0.0) {
                                diffuse = eb && in.diffuse && std::isfinite((*in.diffuse)[pixel]) ? (*in.diffuse)[pixel] : config.diffuseFraction;
                                if (!(diffuse >= 0.0 && diffuse <= 1.0))
                                    diffuse = requestedDiffuseFraction(
                                        eb && in.shortwave && std::isfinite((*in.shortwave)[pixel]) ? (*in.shortwave)[pixel] : 0.0,
                                        sun.zenith, config.diffuseFraction, dayOfYear);  // FIX(2026-11-21 C7): E0 of the overpass day
                            }
                            const auto& leafR = in.leafBandReflectance.at(code);
                            const auto& leafT = in.leafBandTransmittance.at(code);
                            Ad soil(entry->groundReflectance);
                            if (entry->groundKind == GroundKind::Soil) {
                                const double m = std::clamp(moistureAd.v, 0.0, 1.0);
                                const int step = static_cast<int>(std::lround(m * kMoistureSteps));
                                const auto& table = in.soilBandReflectance;
                                if (table[step].empty()) return false;
                                const int lower = std::max(step - 1, 0), upper = std::min(step + 1, kMoistureSteps);
                                double slope = 0.0;
                                if (!table[lower].empty() && !table[upper].empty() && upper > lower)
                                    slope = (table[upper][o.band] - table[lower][o.band]) * kMoistureSteps / (upper - lower);
                                soil = Ad(table[step][o.band]) + slope * (moistureAd - static_cast<double>(step) / kMoistureSteps);
                            }
                            const double lr = leafR[o.band], lt = std::min(leafT[o.band], 1.0 - leafR[o.band]);
                            h[i] = canopyReflectanceT<Ad>(laiAd, entry->canopy.hotspot, sun.zenith, sun.azimuth, std::min<double>(vz, 89.9),
                                                          va, lr, lt, soil, diffuse, sun.zenith < 90.0);
                            break;
                        }
                    }
                    if (!ad::isfinite(h[i])) return false;
                }
                return true;
            };

            // ---- MAP Gauss-Newton (incremental form around the background) ----
            const size_t m = obs.size();
            std::array<double, kMaxControls> x = xb, xGood = xb;
            std::vector<Ad> hGood;
            EbOutcome outcomeGood;
            bool converged = false, ok = true, conditioned = true;
            int iterations = 0;
            double omb = 0.0;
            int thermalCount = 0;
            Matrix posterior{};
            // FIX(2026-11-21 S-4/S-5): (a) every GN step is accepted only after the observation operator evaluated at the
            // candidate and the MAP cost J decreased (backtracking fractions 1, 1/2, 1/4 as the StreamEarth native-MAP
            // tool); a candidate whose evaluation fails is treated as a rejected step, not as a usable analysis.
            // (b) The analysis (x, h, EB outcome) is therefore always one consistently evaluated state; a pixel is
            // flagged RetrievalFailed and NOT analysed only when the operator fails at the background. Previously a
            // failed evaluate() at iteration >= 1 left ok = true, the pixel was counted as analysed and written with
            // the previous iterate while also carrying RetrievalFailed.
            const auto mapCost = [&](const std::array<double, kMaxControls>& state, const std::vector<Ad>& values) {
                std::vector<double> residual(m);
                for (size_t i = 0; i < m; ++i) residual[i] = obs[i].y - values[i].v;
                const auto weighted = applyPrecision(residual);
                double total = 0.0;
                for (size_t i = 0; i < m; ++i) total += residual[i] * weighted[i];
                for (int c = 0; c < n; ++c)
                    for (int d = 0; d < n; ++d) total += (state[c] - xb[c]) * backgroundInverse[c][d] * (state[d] - xb[d]);
                return total;
            };
            double costGood = 0.0, costBackground = std::numeric_limits<double>::quiet_NaN();
            bool stalled = false, stepFailed = false;  // FIX(2026-11-21 S-5 QA): stop reasons
            int acceptedSteps = 0;
            if (!evaluate(x)) ok = false;
            else {
                xGood = x; hGood = h; outcomeGood = outcome;
                for (size_t i = 0; i < m; ++i)
                    if (obs[i].thermal) { omb += obs[i].y - h[i].v; ++thermalCount; }
                costGood = mapCost(xGood, hGood);
                costBackground = costGood;
            }
            for (int iteration = 0; ok && iteration < maxIterations; ++iteration) {
                const std::array<double, kMaxControls>& xs = xGood;
                const std::vector<Ad>& hs = hGood;
                Matrix a = backgroundInverse;
                std::array<double, kMaxControls> g{};
                std::vector<double> innovation(m);
                for (size_t i = 0; i < m; ++i) {
                    innovation[i] = obs[i].y - hs[i].v;
                    for (int c = 0; c < n; ++c) innovation[i] += hs[i].d[c] * (xs[c] - xb[c]);
                }
                const auto weightedInnovation = applyPrecision(innovation);
                for (int c = 0; c < n; ++c) {
                    std::vector<double> column(m);
                    for (size_t i = 0; i < m; ++i) column[i] = hs[i].d[c];
                    const auto weightedColumn = applyPrecision(column);
                    for (size_t i = 0; i < m; ++i) {
                        g[c] += hs[i].d[c] * weightedInnovation[i];
                        for (int d = 0; d < n; ++d) a[d][c] += hs[i].d[d] * weightedColumn[i];
                    }
                }
                if (!invert(a, n)) { conditioned = false; break; }
                double step = 0.0;
                std::array<double, kMaxControls> next{};
                for (int c = 0; c < n; ++c) {
                    double increment = 0.0;
                    for (int d = 0; d < n; ++d) increment += a[c][d] * g[d];
                    next[c] = std::clamp(xb[c] + increment, low[c], high[c]);
                    step = std::max(step, std::abs(next[c] - xs[c]) / sd[c]);
                }
                ++iterations;
                const unsigned flagsBefore = flags;
                if (step < kStepTolerance) {
                    // Converged: take the (tiny) final step when the operator evaluates there.
                    if (evaluate(next)) { xGood = next; hGood = h; outcomeGood = outcome; costGood = mapCost(xGood, hGood); }
                    else flags = flagsBefore;
                    converged = true;
                    break;
                }
                bool accepted = false, evaluated = false;
                unsigned failureFlags = 0;
                for (const double fraction : {1.0, 0.5, 0.25}) {
                    std::array<double, kMaxControls> candidate = xs;
                    for (int c = 0; c < n; ++c) candidate[c] = xs[c] + fraction * (next[c] - xs[c]);
                    if (!evaluate(candidate)) { failureFlags |= flags & ~flagsBefore; flags = flagsBefore; continue; }
                    evaluated = true;
                    const double candidateCost = mapCost(candidate, h);
                    if (std::isfinite(candidateCost) && candidateCost < costGood - 1e-12 * (1.0 + costGood)) {
                        xGood = candidate; hGood = h; outcomeGood = outcome; costGood = candidateCost;
                        accepted = true;
                        break;
                    }
                }
                // FIX(2026-11-21 S-5 QA): classify the stop instead of calling every rejected step "not converged".
                if (!accepted) {
                    if (evaluated) stalled = true;                        // evaluated, but no lower cost along the GN direction
                    else { stepFailed = true; flags |= failureFlags; }    // the operator failed on every trial step
                    break;
                }
                ++acceptedSteps;
            }
            if (!ok || hGood.empty()) { ++failed; finish(flags | kQaRetrievalFailed); continue; }
            if (!conditioned) flags |= kQaIllConditioned;
            // Analysis state: the last accepted (evaluated) iterate = the lowest-cost evaluated state.
            x = xGood;
            h = hGood;
            outcome = outcomeGood;
            // FIX(2026-11-21 S-5 QA): bit 10 / bit 11 for a line-search stop (not failures, outside the default reject
            // mask); bit 9 only for the iteration limit, a singular normal matrix or an operator failure on every trial.
            if (converged) ++stopConverged;
            else if (stalled && acceptedSteps > 0) { flags |= kQaLineSearchStalled; ++stopStalled; }
            else if (stalled) { flags |= kQaNotUpdated; ++stopNotUpdated; }
            else {
                flags |= kQaNotConverged;
                if (stepFailed) ++stopStepFailed;
                else if (!conditioned) ++stopSingular;
                else ++stopIterationLimit;
            }
            qaBands[7][pixel] = static_cast<float>(costBackground);  // FIX(2026-11-21 S-5 QA): MAP cost at the background
            qaBands[8][pixel] = static_cast<float>(costGood);        //                          and at the analysis
            // (Former loop and post-loop re-evaluation replaced by the above — FIX(2026-11-21 S-4/S-5).)

            // Posterior covariance and DFS at the analysis.
            Matrix a = backgroundInverse;
            for (int c = 0; c < n; ++c) {
                std::vector<double> column(m);
                for (size_t i = 0; i < m; ++i) column[i] = h[i].d[c];
                const auto weighted = applyPrecision(column);
                for (size_t i = 0; i < m; ++i)
                    for (int d = 0; d < n; ++d) a[d][c] += h[i].d[d] * weighted[i];
            }
            double dfs = std::numeric_limits<double>::quiet_NaN();
            if (invert(a, n)) {
                posterior = a;
                double trace = 0.0;
                for (int c = 0; c < n; ++c)
                    for (int d = 0; d < n; ++d) trace += posterior[c][d] * backgroundInverse[d][c];
                dfs = n - trace;
            } else flags |= kQaIllConditioned;

            // Analysis components and their uncertainty.
            std::array<double, 4> analysisT{};
            if (!eb) {
                for (int k = 0; k < 4; ++k) analysisT[k] = index[k] >= 0 ? x[index[k]] : background[k][pixel];
            } else analysisT = outcome.temperatures;
            for (int k = 0; k < 4; ++k) {
                result.components[k][pixel] = static_cast<float>(analysisT[k]);
                if (!std::isfinite(analysisT[k])) continue;
                if (analysisT[k] < kValidLow || analysisT[k] > kValidHigh) flags |= kQaOutOfRange;
                if (!std::isfinite(dfs)) continue;
                double variance = 0.0;
                if (!eb) variance = index[k] >= 0 ? posterior[index[k]][index[k]] : kTemperatureSd * kTemperatureSd;
                else {
                    std::array<double, kMaxControls> gk{};
                    for (int c = 0; c < n; ++c)
                        if (kindOf[c] >= kMoisture) gk[c] = outcome.gradient[k][kindOf[c] - kMoisture];
                    for (int c = 0; c < n; ++c)
                        for (int d = 0; d < n; ++d) variance += gk[c] * posterior[c][d] * gk[d];
                }
                uncertainty[k][pixel] = static_cast<float>(std::sqrt(std::max(variance, 0.0)));
            }
            for (int c = 0; c < n; ++c) {
                if (std::isfinite(dfs)) uncertainty[kindOf[c]][pixel] = static_cast<float>(std::sqrt(std::max(posterior[c][c], 0.0)));
                const bool atBound = (x[c] <= low[c] + 1e-9 && xb[c] > low[c] + 1e-9) || (x[c] >= high[c] - 1e-9 && xb[c] < high[c] - 1e-9);
                if (atBound) flags |= kQaOutOfRange;
            }
            if (index[kMoisture] >= 0) result.soilMoisture[pixel] = static_cast<float>(x[index[kMoisture]]);
            if (index[kVcmax] >= 0) result.vcmax[pixel] = static_cast<float>(x[index[kVcmax]]);
            if (index[kLai] >= 0) result.lai[pixel] = static_cast<float>(x[index[kLai]]);
            if (eb) for (int f = 0; f < 4; ++f) fluxes[f][pixel] = static_cast<float>(outcome.fluxes[f]);

            double oma = 0.0, omaSquares = 0.0;
            for (size_t i = 0; i < m; ++i)
                if (obs[i].thermal) { const double r = obs[i].y - h[i].v; oma += r; omaSquares += r * r; }
            if (std::isfinite(dfs) && dfs < kLowSupportDfs) { flags |= kQaLowSupport; ++lowSupport; }
            if (thermalCount > 0) {
                qaBands[1][pixel] = static_cast<float>(omb / thermalCount);
                qaBands[2][pixel] = static_cast<float>(oma / thermalCount);
                qaBands[3][pixel] = static_cast<float>(std::sqrt(omaSquares / thermalCount));
                sumOmb += omb / thermalCount;
                sumOma += oma / thermalCount;
                ++thermalPixels;
            }
            qaBands[4][pixel] = static_cast<float>(dfs);
            qaBands[5][pixel] = static_cast<float>(m);
            qaBands[6][pixel] = static_cast<float>(iterations);
            finish(flags);
            result.analysed[pixel] = 1;
            ++analysed;
            observationsUsed += static_cast<long long>(m);
            if (std::isfinite(dfs)) sumDfs += dfs;
            if (flags & kQaNotConverged) ++notConverged;  // FIX(2026-11-21 S-5 QA): bit 9 only (stalls / not updated counted apart)
            if (flags & kQaIllConditioned) ++illConditioned;
        }
        const int done = ++rowsDone;
        if (isMainThread() && done % std::max(1, rowCount / 40) == 0)
            progress(in.progressStart + in.progressSpan * done / rowCount, "同化分析");
        } catch (...) { errorsAssimilation.capture(); }
    }
    errorsAssimilation.rethrow("同化");

    // ---- outputs ----
    const auto& dir = config.outputDir;
    const std::vector<std::string> componentNames{"Tss sunlit soil temperature (K)", "Tsh shaded soil temperature (K)",
                                                  "Tcs sunlit leaf temperature (K)", "Tch shaded leaf temperature (K)"};
    std::vector<std::string> analysisNames, backgroundNames;
    for (const auto& name : componentNames) { analysisNames.push_back("analysis " + name); backgroundNames.push_back("background " + name); }
    writeTiff(dir / "analysis_components.tif", grid, {result.components.begin(), result.components.end()}, analysisNames);
    writeTiff(dir / "background_components.tif", grid, {background.begin(), background.end()}, backgroundNames);
    writeTiff(dir / "analysis_uncertainty.tif", grid, uncertainty,
              {"Tss analysis 1-sigma (K)", "Tsh analysis 1-sigma (K)", "Tcs analysis 1-sigma (K)", "Tch analysis 1-sigma (K)",
               "soil moisture analysis 1-sigma (m3 m-3)", "Vcmax25 analysis 1-sigma (umol m-2 s-1)", "LAI analysis 1-sigma"});
    writeTiff(dir / "analysis_qa.tif", grid, qaBands,
              {"QA flags", "mean O-B thermal (K)", "mean O-A thermal (K)", "RMS O-A thermal (K)",
               "degrees of freedom for signal", "observations used", "iterations",
               "MAP cost at background", "MAP cost at analysis"});  // FIX(2026-11-21 S-5 QA): bands 8-9 appended
    result.products = {"analysis_components.tif", "background_components.tif", "analysis_uncertainty.tif", "analysis_qa.tif"};
    if (eb || withOptical) {
        std::vector<std::vector<float>> controls{result.soilMoisture, result.vcmax, result.lai};
        for (size_t p = 0; p < pixels; ++p) {
            if (!result.analysed[p]) continue;
            if (!std::isfinite(controls[0][p])) controls[0][p] = backgroundControls[0][p];
            if (!std::isfinite(controls[1][p])) controls[1][p] = backgroundControls[1][p];
            if (!std::isfinite(controls[2][p])) controls[2][p] = backgroundControls[2][p];
        }
        const std::vector<std::string> controlNames{"soil moisture (m3 m-3)", "Vcmax25 (umol m-2 s-1)", "LAI"};
        std::vector<std::string> a, b;
        for (const auto& name : controlNames) { a.push_back("analysis " + name); b.push_back("background " + name); }
        writeTiff(dir / "analysis_controls.tif", grid, controls, a);
        writeTiff(dir / "background_controls.tif", grid, backgroundControls, b);
        result.products.push_back("analysis_controls.tif");
        result.products.push_back("background_controls.tif");
    }
    if (eb) {
        writeTiff(dir / "analysis_fluxes.tif", grid, fluxes,
                  {"analysis net radiation (W m-2)", "analysis sensible heat (W m-2)", "analysis latent heat (W m-2)",
                   "analysis ground heat (W m-2)"});
        result.products.push_back("analysis_fluxes.tif");
    }

    CPLJSONObject summary;
    summary.Add("observation", kind == AssimilationObservation::Brightness ? "brightness" : kind == AssimilationObservation::Lst ? "lst" : "components");
    summary.Add("background", eb ? "eb" : "rt");
    summary.Add("method", "MAP Gauss-Newton per pixel; Dual-number observation Jacobian; tabulated optical moisture slopes are local approximations" +
                std::string(eb ? "; implicit overpass EB derivative conditional on history, 1/L and convective gust (all frozen at the converged state, so quasi-Newton; the aerodynamic resistances ARE differentiated w.r.t. LAI since 2026-11-24 M3; not a full trajectory derivative)" : ""));
    summary.Add("observationLevel", as.observationLevel);
    summary.Add("uncertaintyRaster", as.uncertainty);
    summary.Add("covarianceRaster", as.covariance);
    summary.Add("qaRaster", as.qa);
    summary.Add("rejectQaBits", static_cast<int>(as.rejectQaBits));
    CPLJSONArray sourceIds, priorIds;
    for (const auto& id : as.sourceIds) sourceIds.Add(id);
    for (const auto& id : as.priorSourceIds) priorIds.Add(id);
    summary.Add("sourceIds", sourceIds);
    summary.Add("priorSourceIds", priorIds);
    CPLJSONArray controlList;
    if (eb) { controlList.Add(kControlNames[kMoisture]); controlList.Add(kControlNames[kVcmax]); }
    else for (int k = 0; k < 4; ++k) controlList.Add(kControlNames[k]);
    if (withOptical) controlList.Add(kControlNames[kLai]);
    summary.Add("controls", controlList);
    CPLJSONObject defaults;
    defaults.Add("sigmaBrightnessK", as.sigmaBrightnessK);
    defaults.Add("sigmaLstK", as.sigmaLstK);
    defaults.Add("sigmaComponentK", as.sigmaComponentK);
    defaults.Add("sigmaReflectance", as.sigmaReflectance);
    defaults.Add("backgroundTemperatureSdK", kTemperatureSd);
    defaults.Add("backgroundCorrelation", kTemperatureCorrelation);
    defaults.Add("backgroundMoistureSd", kMoistureSd);
    defaults.Add("backgroundVcmaxRelativeSd", kVcmaxRelativeSd);
    defaults.Add("backgroundLaiRelativeSd", kLaiRelativeSd);
    defaults.Add("lstWavelengthUm", as.lstWavelengthUm);
    defaults.Add("maxIterations", maxIterations);
    summary.Add("defaults", defaults);
    summary.Add("views", static_cast<int>(as.views.size()) + 1);
    summary.Add("optical", withOptical);
    summary.Add("analysedPixels", static_cast<GInt64>(analysed));
    summary.Add("noObservationPixels", static_cast<GInt64>(noObservation));
    summary.Add("failedPixels", static_cast<GInt64>(failed));
    summary.Add("notConvergedPixels", static_cast<GInt64>(notConverged));
    // FIX(2026-11-21 S-5 QA): why the Gauss-Newton iteration stopped (analysed pixels) and the QA bits involved.
    {
        CPLJSONObject stops;
        stops.Add("converged", static_cast<GInt64>(stopConverged));
        stops.Add("lineSearchStalled", static_cast<GInt64>(stopStalled));
        stops.Add("notUpdated", static_cast<GInt64>(stopNotUpdated));
        stops.Add("iterationLimit", static_cast<GInt64>(stopIterationLimit));
        stops.Add("singularNormalMatrix", static_cast<GInt64>(stopSingular));
        stops.Add("stepEvaluationFailed", static_cast<GInt64>(stopStepFailed));
        summary.Add("stopReasons", stops);
        summary.Add("qaBits", "0 input invalid, 1 cloud, 2 out of range, 3 retrieval failed (not analysed), 4 ill-conditioned, "
                              "8 low support, 9 not converged (iteration limit / singular / operator failed on every trial step), "
                              "10 line-search stalled (cost lowered, best state kept), 11 not updated (analysis = background); "
                              "default reject mask 895 = bits 0-6, 8, 9");
    }
    summary.Add("illConditionedPixels", static_cast<GInt64>(illConditioned));
    summary.Add("lowSupportPixels", static_cast<GInt64>(lowSupport));
    summary.Add("observationsUsed", static_cast<GInt64>(observationsUsed));
    if (thermalPixels > 0) {
        summary.Add("meanOmbK", sumOmb / thermalPixels);
        summary.Add("meanOmaK", sumOma / thermalPixels);
    }
    if (analysed > 0) summary.Add("meanDegreesOfFreedom", sumDfs / analysed);
    CPLJSONArray productList;
    for (const auto& name : result.products) productList.Add(name);
    summary.Add("products", productList);
    CPLJSONArray stats;
    for (int k = 0; k < 4; ++k) stats.Add(bandStatistics(analysisNames[k], result.components[k]));
    stats.Add(bandStatistics("degrees of freedom for signal", qaBands[4]));
    stats.Add(bandStatistics("mean O-B thermal (K)", qaBands[1]));
    stats.Add(bandStatistics("mean O-A thermal (K)", qaBands[2]));
    summary.Add("statistics", stats);
    result.summary = summary;

    log("同化（" + std::string(eb ? "EB 背景：土壤水分 + Vcmax" : "RT 背景：四组分温度") + (withOptical ? " + LAI" : "") + "）：分析 " +
        std::to_string(analysed) + " 个像元，无观测 " + std::to_string(noObservation) + "，失败 " + std::to_string(failed) +
        "，未收敛 " + std::to_string(notConverged) +
        // FIX(2026-11-21 S-5 QA): stop reasons
        "（收敛 " + std::to_string(stopConverged) + "，线搜索停滞 " + std::to_string(stopStalled) + "，未更新 " +
        std::to_string(stopNotUpdated) + "，迭代上限 " + std::to_string(stopIterationLimit) + "）" +
        (thermalPixels > 0 ? "，平均 O−B " + format(sumOmb / thermalPixels) + " K，O−A " + format(sumOma / thermalPixels) + " K" : "") +
        (analysed > 0 ? "，平均 DFS " + format(sumDfs / analysed) : ""));
    if (config.terrainRadiation == TerrainRadiation::Hill || config.terrainRadiation == TerrainRadiation::Ridge)
        log("同化：复合地形像元按与成像相同的面元合成计算观测算子");
    return result;
}

}  // namespace stream
