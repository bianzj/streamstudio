#pragma once
// LST / LSCT retrieval from thermal observations, using the forward thermal observation model of image.cpp
// (TiRT four-component weights + hom3 multiple scattering + Hapke/moisture soil emissivity, top of canopy).
//
//   LST : one radiometric temperature per pixel (every component at T). Weighted Gauss-Newton over all
//         observations; the emissivity error is propagated into each observation error.
//   LSCT: Tss/Tsh/Tls/Tlh by MAP Gauss-Newton. The background is the component temperature of the run
//         (RT input or EB solution) with error sd and inter-component correlation; optional s x s window of
//         same-class pixels sharing the four temperatures. Posterior covariance (J^T R^-1 J + B^-1)^-1,
//         degrees of freedom for signal 4 - tr(A B^-1).
#include "config.h"
#include "thermal_view.h"
#include "../scene/classes.h"
#include "../scene/raster.h"

#include <cpl_json.h>

#include <array>
#include <map>
#include <string>
#include <vector>

namespace stream {

// Quality bits; 0-8 follow LS4T base/quality.py, bit 9 is a STREAM extension.
constexpr unsigned kQaInputInvalid = 1u << 0;
constexpr unsigned kQaCloud = 1u << 1;
constexpr unsigned kQaOutOfRange = 1u << 2;
constexpr unsigned kQaRetrievalFailed = 1u << 3;
constexpr unsigned kQaIllConditioned = 1u << 4;
constexpr unsigned kQaLowSupport = 1u << 8;
constexpr unsigned kQaNotConverged = 1u << 9;

struct InversionInputs {
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
    const std::map<int, MoistureSoilEmissivity>& soilEmissivity;  // per soil class; empty: constant emissivity
    int startRow, endRow, startColumn, endColumn;
    double overpass;
    double progressStart, progressSpan;
};

struct InversionResult {
    // LSCT: posterior component temperatures (background where no retrieval was possible); LST: empty.
    std::array<std::vector<float>, 4> components;
    std::vector<std::string> products;
    CPLJSONObject summary;
};

// Writes the retrieval products into config.outputDir.
InversionResult runInversion(const InversionInputs& inputs, const std::array<std::vector<float>, 4>& background);

}  // namespace stream
