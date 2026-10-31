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

    const ShortwaveComponentsT<double> parts = shortwaveComponents<double>(
        sun, *spectra.leafReflectance, *spectra.leafTransmittance, *spectra.soilReflectance, lai, globalShortwave,
        result.diffuseFraction, szaDeg, sunlit);
    result.soilDirect = parts.soilDirect;
    result.leafDirect = parts.leafDirect;
    result.soilDiffuseSun = parts.soilDiffuseSun;
    result.soilDiffuseShade = parts.soilDiffuseShade;
    result.leafDiffuseSun = parts.leafDiffuseSun;
    result.leafDiffuseShade = parts.leafDiffuseShade;
    result.soilMean = parts.soilMean;
    result.leafMean = parts.leafMean;
    result.parLeafDirect = parts.parLeafDirect;
    result.parLeafDiffuse = parts.parLeafDiffuse;
    result.parLeafDiffuseSun = parts.parLeafDiffuseSun;
    result.parLeafDiffuseShade = parts.parLeafDiffuseShade;
    result.soilSunlitFraction = parts.soilSunlitFraction;
    result.laiSunlit = parts.laiSunlit;
    result.albedo = parts.incident > 0.0 ? 1.0 - parts.absorbed / parts.incident : 0.0;
    return result;
}

namespace {
template <class S> double soilValue(const S& value) { return static_cast<double>(value); }
template <int N> Dual<N> soilValue(const Dual<N>& value) { return value; }
}  // namespace

template <class T, class SoilSpectrum>
ShortwaveComponentsT<T> shortwaveComponents(const SolarBins& sun, const std::vector<float>& leafReflectance,
                                            const std::vector<float>& leafTransmittance, const SoilSpectrum& soilSpectrum,
                                            const T& lai, double globalShortwave, double diffuseFraction,
                                            double szaDeg, bool sunlit)
{
    ShortwaveComponentsT<T> out;
    if (!(globalShortwave > 0.0) || sun.size() == 0 || !(sun.directTotal > 0.0) || !(sun.diffuseTotal > 0.0)) return out;
    const bool lit = sunlit && szaDeg < 90.0;
    const double extinction = kG / std::max(std::cos(std::min(szaDeg, 85.0) * kPi / 180.0), 1e-6);
    const double directScale = globalShortwave * (1.0 - diffuseFraction) / sun.directTotal;
    const double diffuseScale = globalShortwave * diffuseFraction / sun.diffuseTotal;

    if (!(lai > 0.0)) {
        T directSoil(0.0), diffuseSoil(0.0), absorbed(0.0);
        double incident = 0.0;
        for (size_t bin = 0; bin < sun.size(); ++bin) {
            const double esun = lit ? directScale * sun.direct[bin] : 0.0;
            const double esky = diffuseScale * sun.diffuse[bin];
            const T soil = ad::clamp(T(soilValue(soilSpectrum[bin])), 0.0, 1.0);
            const T direct = esun * (T(1.0) - soil);
            const T diffuse = esky * (T(1.0) - soil);
            directSoil += direct;
            diffuseSoil += diffuse;
            absorbed += direct + diffuse;
            incident += esun + esky;
        }
        out.soilDirect = directSoil;
        out.soilDiffuseSun = out.soilDiffuseShade = out.soilMean = diffuseSoil;
        out.soilSunlitFraction = T(lit ? 1.0 : 0.0);
        out.absorbed = absorbed;
        out.incident = T(incident);
        return out;
    }

    const HomCanopyT<T> nodes(ad::max(lai, 0.0));
    T beam[kHomNodes]{}, sky[kHomNodes]{};
    if (lit) nodes.split(szaDeg, beam);
    T skySum(0.0);
    for (int node = 0; node < kHomNodes; ++node) {
        sky[node] = nodes.escape[node] * nodes.capacity[node];
        skySum += sky[node];
    }
    T beamArrivals[2 * kHomNodes]{}, skyArrivals[2 * kHomNodes]{};
    for (int node = 0; node < kHomNodes; ++node) {
        beamArrivals[2 * node + 1] = beam[node];
        if (skySum > 0.0) skyArrivals[2 * node + 1] = sky[node] / skySum;
    }
    T soilArea[kHomNodes]{}, leafArea[kHomNodes]{}, sunlitArea[kHomNodes]{};
    for (int node = 0; node < kHomNodes; ++node) {
        if (nodes.soil[node]) {
            soilArea[node] = nodes.capacity[node];
            sunlitArea[node] = lit ? ad::min(beam[node], soilArea[node]) : T(0.0);
        } else {
            leafArea[node] = nodes.capacity[node] / (4.0 * kG);
            sunlitArea[node] = lit ? ad::min(beam[node] / extinction, leafArea[node]) : T(0.0);
        }
    }
    T soilSunlit(0.0), laiSunlit(0.0), soilAreaTotal(0.0), leafAreaTotal(0.0);
    for (int node = 0; node < kHomNodes; ++node) {
        if (nodes.soil[node]) { soilSunlit += sunlitArea[node]; soilAreaTotal += soilArea[node]; }
        else { laiSunlit += sunlitArea[node]; leafAreaTotal += leafArea[node]; }
    }
    out.soilSunlitFraction = soilSunlit;
    out.laiSunlit = laiSunlit;

    const auto mean = [&](const T* values, bool soilMask, bool sunlitPart) {
        const T* area = soilMask ? soilArea : leafArea;
        T weightSum(0.0);
        for (int node = 0; node < kHomNodes; ++node) {
            if (nodes.soil[node] != soilMask || !(area[node] > 0.0)) continue;
            weightSum += sunlitPart ? sunlitArea[node] : (area[node] - sunlitArea[node]);
        }
        const bool fallback = !(weightSum > 1e-12);
        T total(0.0);
        for (int node = 0; node < kHomNodes; ++node) {
            if (nodes.soil[node] != soilMask || !(area[node] > 0.0)) continue;
            const T weight = fallback ? area[node] : (sunlitPart ? sunlitArea[node] : (area[node] - sunlitArea[node]));
            total += values[node] / area[node] * weight;
        }
        const T normalizer = fallback ? (soilMask ? soilAreaTotal : leafAreaTotal) : weightSum;
        return normalizer > 0.0 ? total / normalizer : T(0.0);
    };

    double incident = 0.0;
    for (size_t bin = 0; bin < sun.size(); ++bin) {
        const double esun = lit ? directScale * sun.direct[bin] : 0.0;
        const double esky = diffuseScale * sun.diffuse[bin];
        const double rho = std::clamp(static_cast<double>(leafReflectance[bin]), 0.0, 1.0);
        const double tau = std::min(std::clamp(static_cast<double>(leafTransmittance[bin]), 0.0, 1.0), 1.0 - rho);
        const T soil = ad::clamp(T(soilValue(soilSpectrum[bin])), 0.0, 1.0);
        T beamFirst[kHomNodes], beamTotal[kHomNodes], skyFirst[kHomNodes], skyTotal[kHomNodes];
        homAbsorbedPair<T>(nodes, soil, rho, tau, beamArrivals, skyArrivals, beamFirst, beamTotal, skyFirst, skyTotal);
        T direct[kHomNodes], diffuse[kHomNodes];
        T directSoilBand(0.0), directLeafBand(0.0);
        for (int node = 0; node < kHomNodes; ++node) {
            direct[node] = esun * beamFirst[node];
            diffuse[node] = esun * beamTotal[node] + esky * (skyFirst[node] + skyTotal[node]);
            out.absorbed += direct[node] + diffuse[node];
            if (nodes.soil[node]) directSoilBand += direct[node];
            else directLeafBand += direct[node];
        }
        incident += esun + esky;
        out.soilDirect += soilSunlit > 1e-12 ? directSoilBand / soilSunlit : T(0.0);
        out.leafDirect += laiSunlit > 1e-12 ? directLeafBand / laiSunlit : T(0.0);
        const T soilSunBand = mean(diffuse, true, true);
        const T soilShadeBand = mean(diffuse, true, false);
        const T leafSunBand = mean(diffuse, false, true);
        const T leafShadeBand = mean(diffuse, false, false);
        out.soilDiffuseSun += soilSunBand;
        out.soilDiffuseShade += soilShadeBand;
        out.leafDiffuseSun += leafSunBand;
        out.leafDiffuseShade += leafShadeBand;
        T soilSum(0.0), leafSum(0.0);
        for (int node = 0; node < kHomNodes; ++node) (nodes.soil[node] ? soilSum : leafSum) += diffuse[node];
        const T soilMeanBand = soilAreaTotal > 0.0 ? soilSum / soilAreaTotal : T(0.0);
        const T leafMeanBand = leafAreaTotal > 0.0 ? leafSum / leafAreaTotal : T(0.0);
        out.soilMean += soilMeanBand;
        out.leafMean += leafMeanBand;
        const double photons = sun.photon[bin];
        if (photons > 0.0) {
            out.parLeafDirect += (laiSunlit > 1e-12 ? directLeafBand / laiSunlit : T(0.0)) * photons;
            out.parLeafDiffuse += leafMeanBand * photons;
            out.parLeafDiffuseSun += leafSunBand * photons;
            out.parLeafDiffuseShade += leafShadeBand * photons;
        }
    }
    out.incident = T(incident);
    return out;
}

template ShortwaveComponentsT<double> shortwaveComponents<double, std::vector<float>>(
    const SolarBins&, const std::vector<float>&, const std::vector<float>&, const std::vector<float>&, const double&,
    double, double, double, bool);
template ShortwaveComponentsT<Ad> shortwaveComponents<Ad, std::vector<Ad>>(
    const SolarBins&, const std::vector<float>&, const std::vector<float>&, const std::vector<Ad>&, const Ad&,
    double, double, double, bool);
template ShortwaveComponentsT<Ad> shortwaveComponents<Ad, std::vector<float>>(
    const SolarBins&, const std::vector<float>&, const std::vector<float>&, const std::vector<float>&, const Ad&,
    double, double, double, bool);

}  // namespace stream
