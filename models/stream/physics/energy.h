#pragma once
#include "heat_conduction.h"
#include "shortwave.h"

#include <array>

namespace stream {

// Farquhar photosynthesis with Ball-Berry stomatal closure (TiRT base/biochemical.py).
struct Photosynthesis {
    int pathway = 3;               // 3 = C3, 4 = C4, 0 = no photosynthesis
    float vcmax = 50.0f;           // umol m-2 s-1 at 25 C
    float ballBerrySlope = 9.0f;
    float respiration = 0.015f;    // dark respiration / Vcmax25
    float stress = 1.0f;
    bool temperatureCorrection = false;
    float temperature[5] = {0.2f, 0.3f, 281.0f, 308.0f, 328.0f};
};

struct Aerodynamics {
    double cd = 0.2, cd1 = 20.6, cr = 0.35, psicor = 0.2, cssoil = 0.01, soilBoundary = 10.0;
    double minWindSpeed = 0.05;
    double maxResistance = 400.0;
    // kB^-1 = ln(z0/z0h) of the bare-soil path. 2.3026 is the historical z0h = 0.1 z0; arid bare soil is
    // 5-10 (Beljaars & Holtslag), which raises the aerodynamic resistance and the midday surface temperature.
    double heatRoughnessKb = 2.302585093;
};

enum class SoilHeatMethod { Direct, Surface, Depth };

struct Ground {
    bool moistureResistance = false;
    double fixedResistance = 200.0;   // s m-1, TIRTEB template soil_rss
    double saturatedMoisture = 0.45;
    double emissivity = 0.955;
    double conductivity = 1.55;
    double volumetricCapacity = 1.298e6;
    // kB^-1 = ln(z0/z0h) of this class. Arid bare soil / gravel is 5-10 (Beljaars & Holtslag), short vegetation
    // about 2, tall vegetation about 1.5; it sets how strongly the surface is coupled to the air and therefore how
    // wide the diurnal cycle is.
    double heatRoughnessKb = 2.302585093;
    ThermalMaterial material() const { return ThermalMaterial{conductivity, volumetricCapacity}; }
};

struct EnergySurface {
    double lai = 0, height = 1, leafWidth = 0.05, hotspot = 0.2;
    double leafEmissivity = 0.985;
    double soilMoisture = 0.25;
    Ground ground;
    Photosynthesis leaf;
    Aerodynamics aero;
    double referenceHeight = 15.0;  // z, m
    double co2 = 380.0, o2 = 209.0;
    double stomatalG0 = 0.01;       // mol m-2 s-1
    double diffuseFraction = 0.2;   // requested; -1 = Erbs
    bool enforceSolarLimit = true;
    int dayOfYear = 0;
    SoilHeatMethod soilHeatMethod = SoilHeatMethod::Surface;
    bool robustTemperature = true;
    double soilHeatFraction = 0.25;
    int maxIterations = 50;
    int maxStabilityIterations = 60;
};

struct Forcing {
    double ta = 288.15;  // K
    double ea = 10.0;    // hPa
    double p = 1013.0;   // hPa
    double wind = 2.0;   // m s-1
    double shortwave = 0.0, longwave = 300.0;
};

struct EnergyState {
    double soilSunlit = 293.15, soilShaded = 293.15, leafSunlit = 293.15, leafShaded = 293.15;
    double inverseObukhov = 0.0;
    double sceneSensible = 0.0;
    SoilThermalState soilSunState, soilShadeState;
};

struct EnergyDiagnostics {
    double netRadiation = 0, sensible = 0, latent = 0, ground = 0;
    double residual = 0;            // max component EB residual, W m-2
    double stabilityResidual = 0;   // |1/L - target|, m-1
    double gustResidual = 0;
    double soilSunlitFraction = 0, laiSunlit = 0;
    double airDensity = 0, ustar = 0, displacement = 0, roughness = 0, canopyWeight = 0;
    double obukhovLength = 0;
    int iterations = 0, stabilityIterations = 0;
    bool converged = false, stabilityConverged = false, stabilityLimited = false, fallbackUsed = false;
};

EnergyDiagnostics solveEnergyNode(const EnergySurface& surface, const Forcing& forcing,
                                  const ShortwaveAbsorption& shortwave, double szaDeg, bool sunlit,
                                  double dtSeconds, EnergyState& state);

// Inputs of the four component balances at one node on any scalar type (double or Ad). The forward solve uses
// double; the assimilation evaluates the same balance with Ad to get dR/dT and dR/dx at the converged state
// (implicit differentiation: dT/dx = -(dR/dT)^-1 dR/dx).
template <class T>
struct EnergyInputsT {
    T soilSunlit{}, soilShaded{}, leafSunlit{}, leafShaded{};  // component temperatures, K
    T lai{}, soilMoisture{}, vcmax{}, soilEmissivity{}, soilInertia{};
    // absorbed shortwave per unit component area and PAR (ShortwaveAbsorption)
    T soilDirect{}, soilDiffuseSun{}, soilDiffuseShade{}, leafDirect{}, leafDiffuseSun{}, leafDiffuseShade{};
    T parSun{}, parShade{}, soilSunlitFraction{}, laiSunlit{};
};
EnergyInputsT<double> energyInputs(const EnergySurface& surface, const ShortwaveAbsorption& shortwave,
                                   const EnergyState& state);

// Component residuals Rn - H - LE (- G for soil) at a frozen stability / gust state and soil heat history.
template <class T>
std::array<T, 4> energyResiduals(const EnergySurface& surface, const EnergyInputsT<T>& inputs, const Forcing& forcing,
                                 double szaDeg, bool sunlit, double dtSeconds, const EnergyState& history,
                                 double inverseObukhov, double sceneSensible);

// Johansen soil conductivity / volumetric heat capacity from the water content (coarse soil).
template <class T>
void johansenSoilThermal(double saturatedMoisture, const T& moisture, T& conductivity, T& volumetricCapacity);

// Tetens saturation vapour pressure [hPa] at a temperature in Celsius.
double saturationVapourPressure(double celsius);

}  // namespace stream
