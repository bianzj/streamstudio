#pragma once
#include "dual.h"

#include <utility>
#include <vector>

namespace stream {

// Reference solar spectral shapes (fesun / fesky) integrated over contiguous bins of the 1 nm grid.
// Leaf and soil spectra are averaged over the same bins before the node-radiosity solve.
struct SolarBins {
    std::vector<std::pair<int, int>> range;  // [first, last) indices on the 1 nm grid
    std::vector<double> direct, diffuse;     // integrated shapes per bin
    std::vector<double> photon;              // umol J-1 for PAR bins (400-700 nm), 0 elsewhere
    double directTotal = 0.0, diffuseTotal = 0.0;
    size_t size() const { return range.size(); }
};

SolarBins makeSolarBins(const std::vector<float>& fesun, const std::vector<float>& fesky, int binWidthNm);
std::vector<float> binSpectrum(const SolarBins& bins, const std::vector<float>& spectrum);

// Per-bin leaf and soil spectra (same bins as the solar shapes).
struct OpticalSpectra {
    const std::vector<float>* leafReflectance = nullptr;
    const std::vector<float>* leafTransmittance = nullptr;
    const std::vector<float>* soilReflectance = nullptr;
};

// Absorbed shortwave per unit component area (soil per soil area, leaves per leaf area), as TIRTEB.
struct ShortwaveAbsorption {
    double soilDirect = 0, leafDirect = 0;
    double soilDiffuseSun = 0, soilDiffuseShade = 0;
    double leafDiffuseSun = 0, leafDiffuseShade = 0;
    double soilMean = 0, leafMean = 0;
    double parLeafDirect = 0, parLeafDiffuse = 0, parLeafDiffuseSun = 0, parLeafDiffuseShade = 0;
    double soilSunlitFraction = 0, laiSunlit = 0;
    double albedo = 0;
    double diffuseFraction = 0, diffuseFractionRequested = 0;
    double directNormal = 0, directNormalLimit = 0, clearnessIndex = 0;
    bool partitionLimited = false;
};

// FIX(2026-11-21 C7): the Erbs clearness index includes the orbital (Sun-Earth distance) factor E0 of the day, as
// TiRT radiation.py:218-221. dayOfYear <= 0 uses the perihelion value 1.033 (TiRT without a date); the 3-argument
// form is kept for compatibility and is that no-date case.
// FIX(2026-11-21 C10): with the sun at or below the horizon (SZA >= 90°) there is no beam: the Erbs fraction is 1.
double requestedDiffuseFraction(double globalShortwave, double szaDeg, double configured);
double requestedDiffuseFraction(double globalShortwave, double szaDeg, double configured, int dayOfYear);
// FIX(2026-11-21 C10): SZA >= 90° returns 1 (all diffuse) whether or not the DNI limit is enforced.
double admissibleDiffuseFraction(double globalShortwave, double szaDeg, int dayOfYear, double configured,
                                 bool enforceLimit, double* limit = nullptr);

ShortwaveAbsorption absorbShortwave(const SolarBins& sun, const OpticalSpectra& spectra, double lai,
                                    double globalShortwave, double szaDeg, int dayOfYear, double requestedFraction,
                                    bool enforceLimit, bool sunlit);

// Component absorption at a fixed diffuse fraction on any scalar type (double or Ad). absorbShortwave uses the
// double instance; the assimilation differentiates it with respect to LAI and the per-bin soil reflectance.
template <class T>
struct ShortwaveComponentsT {
    T soilDirect{}, leafDirect{}, soilDiffuseSun{}, soilDiffuseShade{}, leafDiffuseSun{}, leafDiffuseShade{};
    T soilMean{}, leafMean{}, parLeafDirect{}, parLeafDiffuse{}, parLeafDiffuseSun{}, parLeafDiffuseShade{};
    T soilSunlitFraction{}, laiSunlit{}, absorbed{}, incident{};
};
// soil: per-bin reflectance (std::vector<float> or std::vector<Ad>).
// FIX(2026-11-21 C10): when the receiving surface is not sunlit (sunlit == false or SZA >= 90°) the whole global
// shortwave is distributed as diffuse sky radiation (the direct part used to be dropped), so incident = global.
template <class T, class SoilSpectrum>
ShortwaveComponentsT<T> shortwaveComponents(const SolarBins& sun, const std::vector<float>& leafReflectance,
                                            const std::vector<float>& leafTransmittance, const SoilSpectrum& soil,
                                            const T& lai, double globalShortwave, double diffuseFraction,
                                            double szaDeg, bool sunlit);

}  // namespace stream
