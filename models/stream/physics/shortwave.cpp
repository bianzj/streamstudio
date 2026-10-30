#include "shortwave.h"
#include "constants.h"
#include "radiosity.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace stream {
namespace {

constexpr double kG = 0.5;
constexpr double kSolarConstant = 1361.0;      // W m-2, TIRTEB clearness index
constexpr double kMeanSolarConstant = 1367.0;  // W m-2, Meinel clear-sky DNI
constexpr double kPhotons = 1e-3 / (6.02214e23 * 6.6262e-34 * 299792458.0);

double orbitalFactor(int dayOfYear)
{
    return dayOfYear > 0 ? 1.0 + 0.033 * std::cos(2.0 * kPi * dayOfYear / 365.0) : 1.033;
}

double solarDirectNormalLimit(double szaDeg, int dayOfYear)
{
    if (szaDeg >= 90.0) return 0.0;
    const double airMass = 1.0 / (std::cos(szaDeg * kPi / 180.0) +
                                  0.50572 * std::pow(96.07995 - szaDeg, -1.6364));
    return kMeanSolarConstant * orbitalFactor(dayOfYear) * std::pow(0.7, std::pow(airMass, 0.678));
}

}  // namespace

SolarBins makeSolarBins(const std::vector<float>& fesun, const std::vector<float>& fesky, int binWidthNm)
{
    SolarBins bins;
    const int width = std::max(1, binWidthNm);
    for (int first = 0; first < kSpectralCount;) {
        int last = first + width;
        if (kSpectralCount - last < width) last = kSpectralCount;
        double direct = 0.0, diffuse = 0.0;
        for (int index = first; index < last; ++index) {
            direct += fesun[index];
            diffuse += fesky[index];
        }
        double weightedWavelength = 0.0;
        for (int index = first; index < last; ++index) {
            weightedWavelength += (fesun[index] + fesky[index]) * (kFirstWavelength + index);
        }
        const bool par = kFirstWavelength + first >= 400 && kFirstWavelength + last - 1 <= 700;
        const double meanWavelength = direct + diffuse > 0.0
            ? weightedWavelength / (direct + diffuse)
            : kFirstWavelength + 0.5 * (first + last - 1);
        bins.range.push_back({first, last});
        bins.direct.push_back(direct);
        bins.diffuse.push_back(diffuse);
        bins.photon.push_back(par ? meanWavelength * kPhotons : 0.0);
        bins.directTotal += direct;
        bins.diffuseTotal += diffuse;
        first = last;
    }
    return bins;
}

std::vector<float> binSpectrum(const SolarBins& bins, const std::vector<float>& spectrum)
{
    std::vector<float> result(bins.size());
    for (size_t bin = 0; bin < bins.size(); ++bin) {
        const auto [first, last] = bins.range[bin];
        double sum = 0.0;
        for (int index = first; index < last; ++index) sum += spectrum[index];
        result[bin] = static_cast<float>(last > first ? sum / (last - first) : 0.0);
    }
    return result;
}

double requestedDiffuseFraction(double globalShortwave, double szaDeg, double configured)
{
    if (configured >= 0.0 && configured <= 1.0) return configured;
    const double cosine = std::max(std::cos(szaDeg * kPi / 180.0), 1e-6);
    const double clearness = std::clamp(globalShortwave / (kSolarConstant * cosine), 0.0, 1.2);
    double fraction;
    if (clearness <= 0.22) fraction = 1.0 - 0.09 * clearness;
    else if (clearness <= 0.80)
        fraction = 0.9511 - 0.1604 * clearness + 4.388 * clearness * clearness -
                   16.638 * clearness * clearness * clearness + 12.336 * std::pow(clearness, 4.0);
    else fraction = 0.165;
    return std::clamp(fraction, 0.0, 1.0);
}

double admissibleDiffuseFraction(double globalShortwave, double szaDeg, int dayOfYear, double configured,
                                 bool enforceLimit, double* limit)
{
    const double requested = requestedDiffuseFraction(globalShortwave, szaDeg, configured);
    const double normalLimit = solarDirectNormalLimit(szaDeg, dayOfYear);
    if (limit) *limit = normalLimit;
    if (!enforceLimit) return requested;
    const double total = std::max(globalShortwave, 0.0);
    if (total <= 0.0) return requested;
    const double horizontalLimit = normalLimit * std::max(std::cos(szaDeg * kPi / 180.0), 0.0);
    return std::max(requested, 1.0 - std::min(horizontalLimit / total, 1.0));
}

ShortwaveAbsorption absorbShortwave(const SolarBins& sun, const OpticalSpectra& spectra, double lai,
                                    double globalShortwave, double szaDeg, int dayOfYear, double requestedFraction,
                                    bool enforceLimit, bool sunlit)
{
    ShortwaveAbsorption result;
    result.diffuseFractionRequested = requestedDiffuseFraction(globalShortwave, szaDeg, requestedFraction);
    result.directNormalLimit = solarDirectNormalLimit(szaDeg, dayOfYear);
    const double cosine = std::cos(szaDeg * kPi / 180.0);
    result.clearnessIndex = cosine > 0.0
        ? globalShortwave / (kSolarConstant * orbitalFactor(dayOfYear) * cosine)
        : std::numeric_limits<double>::quiet_NaN();
    if (globalShortwave <= 0.0 || sun.size() == 0) return result;
    result.diffuseFraction = admissibleDiffuseFraction(globalShortwave, szaDeg, dayOfYear, requestedFraction,
                                                      enforceLimit, &result.directNormalLimit);
    result.partitionLimited = result.diffuseFraction > result.diffuseFractionRequested + 1e-12;
    result.directNormal = (szaDeg < 90.0 && globalShortwave > 0.0)
        ? globalShortwave * (1.0 - result.diffuseFraction) / std::max(cosine, 1e-12)
        : 0.0;
    if (!spectra.leafReflectance || !spectra.leafTransmittance || !spectra.soilReflectance) return result;
    if (!(sun.directTotal > 0.0) || !(sun.diffuseTotal > 0.0)) return result;

    const bool lit = sunlit && szaDeg < 90.0;
    const double extinction = kG / std::max(std::cos(std::min(szaDeg, 85.0) * kPi / 180.0), 1e-6);
    const double directScale = globalShortwave * (1.0 - result.diffuseFraction) / sun.directTotal;
    const double diffuseScale = globalShortwave * result.diffuseFraction / sun.diffuseTotal;

    HomCanopy nodes(std::max(lai, 0.0));
    double beam[kHomNodes]{}, sky[kHomNodes]{};
    if (lit && lai > 0.0) nodes.split(szaDeg, beam);
    double skySum = 0.0;
    for (int node = 0; node < kHomNodes; ++node) {
        sky[node] = nodes.escape[node] * nodes.capacity[node];
        skySum += sky[node];
    }
    double beamArrivals[2 * kHomNodes]{}, skyArrivals[2 * kHomNodes]{};
    for (int node = 0; node < kHomNodes; ++node) {
        beamArrivals[2 * node + 1] = beam[node];
        if (skySum > 0.0) skyArrivals[2 * node + 1] = sky[node] / skySum;
    }
    double soilArea[kHomNodes]{}, leafArea[kHomNodes]{}, sunlitArea[kHomNodes]{};
    for (int node = 0; node < kHomNodes; ++node) {
        if (nodes.soil[node]) {
            soilArea[node] = nodes.capacity[node];
            sunlitArea[node] = lit ? std::min(beam[node], soilArea[node]) : 0.0;
        } else {
            leafArea[node] = nodes.capacity[node] / (4.0 * kG);
            sunlitArea[node] = lit ? std::min(beam[node] / extinction, leafArea[node]) : 0.0;
        }
    }
    double soilSunlit = 0.0, laiSunlit = 0.0, soilAreaTotal = 0.0, leafAreaTotal = 0.0;
    for (int node = 0; node < kHomNodes; ++node) {
        if (nodes.soil[node]) { soilSunlit += sunlitArea[node]; soilAreaTotal += soilArea[node]; }
        else { laiSunlit += sunlitArea[node]; leafAreaTotal += leafArea[node]; }
    }
    result.soilSunlitFraction = soilSunlit;
    result.laiSunlit = laiSunlit;

    const auto mean = [&](const double* values, bool soilMask, bool sunlitPart) {
        const double* area = soilMask ? soilArea : leafArea;
        double weightSum = 0.0;
        for (int node = 0; node < kHomNodes; ++node) {
            if (nodes.soil[node] != soilMask || !(area[node] > 0.0)) continue;
            weightSum += sunlitPart ? sunlitArea[node] : (area[node] - sunlitArea[node]);
        }
        const bool fallback = !(weightSum > 1e-12);
        double total = 0.0;
        for (int node = 0; node < kHomNodes; ++node) {
            if (nodes.soil[node] != soilMask || !(area[node] > 0.0)) continue;
            const double weight = fallback ? area[node] : (sunlitPart ? sunlitArea[node] : (area[node] - sunlitArea[node]));
            total += values[node] / area[node] * weight;
        }
        const double normalizer = fallback ? (soilMask ? soilAreaTotal : leafAreaTotal) : weightSum;
        return normalizer > 0.0 ? total / normalizer : 0.0;
    };

    double directSoil = 0.0, directLeaf = 0.0, diffuseSoilSun = 0.0, diffuseSoilShade = 0.0;
    double diffuseLeafSun = 0.0, diffuseLeafShade = 0.0, soilMean = 0.0, leafMean = 0.0;
    double absorbed = 0.0, incident = 0.0;
    double parDirect = 0.0, parDiffuse = 0.0, parDiffuseSun = 0.0, parDiffuseShade = 0.0;

    if (!(lai > 0.0)) {
        for (size_t bin = 0; bin < sun.size(); ++bin) {
            const double esun = lit ? directScale * sun.direct[bin] : 0.0;
            const double esky = diffuseScale * sun.diffuse[bin];
            const double soil = std::clamp(static_cast<double>((*spectra.soilReflectance)[bin]), 0.0, 1.0);
            const double direct = esun * (1.0 - soil);
            const double diffuse = esky * (1.0 - soil);
            directSoil += direct;
            diffuseSoilSun += diffuse;
            diffuseSoilShade += diffuse;
            absorbed += direct + diffuse;
            incident += esun + esky;
        }
        result.soilDirect = directSoil;
        result.soilDiffuseSun = result.soilDiffuseShade = result.soilMean = diffuseSoilSun;
        result.soilSunlitFraction = lit ? 1.0 : 0.0;
        result.albedo = incident > 0.0 ? 1.0 - absorbed / incident : 0.0;
        return result;
    }

    for (size_t bin = 0; bin < sun.size(); ++bin) {
        const double esun = lit ? directScale * sun.direct[bin] : 0.0;
        const double esky = diffuseScale * sun.diffuse[bin];
        const double rho = std::clamp(static_cast<double>((*spectra.leafReflectance)[bin]), 0.0, 1.0);
        const double tau = std::min(std::clamp(static_cast<double>((*spectra.leafTransmittance)[bin]), 0.0, 1.0), 1.0 - rho);
        const double soil = std::clamp(static_cast<double>((*spectra.soilReflectance)[bin]), 0.0, 1.0);
        double beamFirst[kHomNodes], beamTotal[kHomNodes], skyFirst[kHomNodes], skyTotal[kHomNodes];
        homAbsorbedPair(nodes, soil, rho, tau, beamArrivals, skyArrivals,
                        beamFirst, beamTotal, skyFirst, skyTotal);
        double direct[kHomNodes], diffuse[kHomNodes];
        double directSoilBand = 0.0, directLeafBand = 0.0;
        for (int node = 0; node < kHomNodes; ++node) {
            direct[node] = esun * beamFirst[node];
            diffuse[node] = esun * beamTotal[node] + esky * (skyFirst[node] + skyTotal[node]);
            absorbed += direct[node] + diffuse[node];
            if (nodes.soil[node]) directSoilBand += direct[node];
            else directLeafBand += direct[node];
        }
        incident += esun + esky;
        directSoil += soilSunlit > 1e-12 ? directSoilBand / soilSunlit : 0.0;
        directLeaf += laiSunlit > 1e-12 ? directLeafBand / laiSunlit : 0.0;
        const double soilSunBand = mean(diffuse, true, true);
        const double soilShadeBand = mean(diffuse, true, false);
        const double leafSunBand = mean(diffuse, false, true);
        const double leafShadeBand = mean(diffuse, false, false);
        diffuseSoilSun += soilSunBand;
        diffuseSoilShade += soilShadeBand;
        diffuseLeafSun += leafSunBand;
        diffuseLeafShade += leafShadeBand;
        double soilSum = 0.0, leafSum = 0.0;
        for (int node = 0; node < kHomNodes; ++node)
            (nodes.soil[node] ? soilSum : leafSum) += diffuse[node];
        const double soilMeanBand = soilAreaTotal > 0.0 ? soilSum / soilAreaTotal : 0.0;
        const double leafMeanBand = leafAreaTotal > 0.0 ? leafSum / leafAreaTotal : 0.0;
        soilMean += soilMeanBand;
        leafMean += leafMeanBand;
        const double photons = sun.photon[bin];
        if (photons > 0.0) {
            parDirect += (laiSunlit > 1e-12 ? directLeafBand / laiSunlit : 0.0) * photons;
            parDiffuse += leafMeanBand * photons;
            parDiffuseSun += leafSunBand * photons;
            parDiffuseShade += leafShadeBand * photons;
        }
    }
    result.soilDirect = directSoil;
    result.leafDirect = directLeaf;
    result.soilDiffuseSun = diffuseSoilSun;
    result.soilDiffuseShade = diffuseSoilShade;
    result.leafDiffuseSun = diffuseLeafSun;
    result.leafDiffuseShade = diffuseLeafShade;
    result.soilMean = soilMean;
    result.leafMean = leafMean;
    result.parLeafDirect = parDirect;
    result.parLeafDiffuse = parDiffuse;
    result.parLeafDiffuseSun = parDiffuseSun;
    result.parLeafDiffuseShade = parDiffuseShade;
    result.albedo = incident > 0.0 ? 1.0 - absorbed / incident : 0.0;
    return result;
}

}  // namespace stream
