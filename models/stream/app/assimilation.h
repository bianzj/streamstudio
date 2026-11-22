#pragma once
// Run mode "同化": MAP Gauss-Newton analysis of one pixel at a time with the built-in differentiable operator.
//
// Observation operator h(x): shared with imaging and differentiated with Dual numbers.
// Tabulated moisture optical slopes are local approximations; EB derivatives are
// conditional on the overpass history/stability, not the full trajectory derivative.
//   RT background: x = four component temperatures (+ LAI when reflectances are given).
//   EB background: x = soil moisture + Vcmax (+ LAI). The component temperatures follow from the energy balance;
//                  dT/dx comes from implicit differentiation of the four component balances at the converged
//                  overpass state (dT/dx = -(dR/dT)^-1 dR/dx), so H = dh/dT dT/dx + dh/dx.
// Errors, iteration and convergence are built-in defaults (assimilation.cpp); emissivity is not a control.
#include "config.h"
#include "thermal_view.h"
#include "../scene/classes.h"
#include "../scene/raster.h"

#include <cpl_json.h>

#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace stream {

struct EbControls { double moisture = 0, vcmax = 0, lai = 0; };
struct EbOutcome {
    bool gradientValid = false;  // false means the analysis must reject the linearization
    std::array<double, 4> temperatures{};                  // Tss, Tsh, Tcs, Tch at the overpass
    std::array<std::array<double, 3>, 4> gradient{};       // dT_k / d[soil moisture, Vcmax, LAI]
    std::array<double, 4> fluxes{};                        // Rn, H, LE, G at the overpass
};
// Re-solves the energy balance of one pixel at the given controls and linearises it; false if it fails.
using EbSolver = std::function<bool(size_t pixel, const EbControls& controls, EbOutcome& outcome)>;

struct AssimilationInputs {
    const ImageConfig& config;
    const ClassTable& classes;
    const Grid& grid;
    const std::vector<float>& type;
    const std::vector<float>& lai;
    const std::vector<float>& vza;
    const std::vector<float>& vaa;
    const std::vector<float>& latitude;
    const std::vector<float>& longitude;
    const std::vector<float>& dem;
    const std::vector<float>& soilMoisture;
    const std::vector<float>* diffuse;    // per-pixel diffuse fraction (EB), may be null
    const std::vector<float>* shortwave;  // per-pixel downward shortwave (EB), may be null
    const std::map<int, MoistureSoilEmissivity>& soilEmissivity;
    const TerrainFields& terrain;
    const std::map<int, std::vector<double>>& leafBandReflectance;
    const std::map<int, std::vector<double>>& leafBandTransmittance;
    const std::vector<std::vector<double>>& soilBandReflectance;  // per soil-moisture step, per optical band
    int startRow, endRow, startColumn, endColumn;
    double overpass;
    bool eb;
    EbSolver ebSolver;
    double progressStart, progressSpan;
    // Downwelling longwave of the reflected-sky term (W m-2 per pixel, NaN = none), the same field the forward
    // imaging uses, so h(x) of brightness observations includes (1 − Σw)·B(T_sky). Null disables the term.
    const std::vector<float>* skyLongwave = nullptr;
};

struct AssimilationResult {
    std::array<std::vector<float>, 4> components;  // analysis (background where not analysed)
    std::vector<float> lai, soilMoisture, vcmax;   // analysis controls (vcmax: EB only, NaN elsewhere)
    std::vector<char> analysed;
    std::vector<std::string> products;
    CPLJSONObject summary;
};

// Writes analysis_* / background_* products into config.outputDir.
AssimilationResult runAssimilation(const AssimilationInputs& inputs, const std::array<std::vector<float>, 4>& background);

}  // namespace stream
