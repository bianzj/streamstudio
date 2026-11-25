// ==========================================================================
// 文件：energy.h
// 功能：STREAM EB 四组分能量平衡的数据结构与接口声明（实现见 energy.cpp）
// 主要内容：
//   1. 参数结构：Photosynthesis（光合/气孔）、Aerodynamics（阻抗网络参数）、Ground（土壤）、
//      EnergySurface（单像元地表与求解设置）
//   2. 输入/状态/输出：Forcing（气象强迫）、EnergyState（组分温度与热历史）、EnergyDiagnostics
//   3. solveEnergyNode：单节点求解；energyInputs / energyResiduals：同化用的可微残差接口
//   4. johansenSoilThermal、saturationVapourPressure：土壤热物性与饱和水汽压
// 约定：温度 K；通量 W m-2；气压/水汽压 hPa；风速 m s-1；Vcmax µmol m-2 s-1（25 °C）；
//       四组分 = 光照土壤、阴影土壤、光照叶片、阴影叶片；残差 R = Rn - H - LE (- G)
// ==========================================================================

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
    bool temperatureCorrection = true;  // FIX(2026-11-21 B3): TiRT default tcor = 1 (was false)
    float temperature[5] = {0.2f, 0.3f, 281.0f, 308.0f, 328.0f};
};

// 结构：空气动力学阻抗参数。cd、cd1、cr、psicor、cssoil 为 Raupach 位移高度/粗糙度公式的系数
//       （待核对：各符号与 SCOPE 的对应关系）；soilBoundary 为土壤边界层阻抗（s m-1）；
//       minWindSpeed、maxResistance 为风速下限与各分段阻抗上限。
struct Aerodynamics {
    double cd = 0.2, cd1 = 20.6, cr = 0.35, psicor = 0.2, cssoil = 0.01, soilBoundary = 10.0;
    double minWindSpeed = 0.05;
    double maxResistance = 400.0;
    // kB^-1 = ln(z0/z0h) of the bare-soil path. 2.3026 is the historical z0h = 0.1 z0; arid bare soil is
    // 5-10 (Beljaars & Holtslag), which raises the aerodynamic resistance and the midday surface temperature.
    double heatRoughnessKb = 2.302585093;
};

// 土壤热通量方法：Direct = G 取净辐射的固定比例；Surface = 7 h 表面温度历史卷积；
// Depth = 2 m 土柱（见 heat_conduction.h）。
enum class SoilHeatMethod { Direct, Surface, Depth };

// Hard limits of the component temperatures in the energy-balance solve, K (FIX 2026-11-24 M2: lower limit 240 -> 175).
constexpr double kComponentTemperatureMin = 175.0;
constexpr double kComponentTemperatureMax = 380.0;

// 结构：土壤/地面参数。moistureResistance 为 true 时表面阻抗随含水量变化（Sellers 型），
//       否则取 fixedResistance；emissivity 为干端宽带发射率；conductivity、volumetricCapacity
//       为土壤热参数。
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
    // FIX(2026-11-21 B4): documentation only. Like TiRT aerodynamic.py (_bulk_resistance, z0h = 0.1 z0, used only by
    // the bare-soil and urban bulk paths), kB^-1 enters only the bare-soil bulk resistance: fully for LAI = 0 and with
    // weight 1 - smoothstep(LAI / 0.5) for 0 < LAI < 0.5. For LAI >= 0.5 it has no effect: the canopy network
    // (rai + rar + rac, leaf boundary layer 70/LAI sqrt(w/u), soil path soilProfile + rbs) carries the excess heat
    // resistance. The class values of vegetated classes therefore matter only for sparse (LAI < 0.5) pixels.
    double heatRoughnessKb = 2.302585093;
    ThermalMaterial material() const { return ThermalMaterial{conductivity, volumetricCapacity}; }
};

// 结构：单像元能量平衡所需的全部地表参数与求解设置（LAI、冠层高、叶片发射率、土壤水分、
//       光合、气动、参考高度、CO2/O2、短波直散分配、迭代上限、冰雪温度上限等）。
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
    // FIX(2026-11-24 ice melt): > 0 limits the four component temperatures to this value (273.15 K for snow / ice grounds).
    // Energy that would warm a component above it is not stored: it is reported as a melt flux and the component balance
    // closes with Rn - H - LE - G - melt = 0 (0 = no limit, the previous behaviour).
    double temperatureCap = 0.0;
};

// 结构：气象强迫（标准量）：气温 ta（K）、水汽压 ea、气压 p（hPa）、风速、
//       水平面向下短波与向下长波（W m-2）。
struct Forcing {
    double ta = 288.15;  // K
    double ea = 10.0;    // hPa
    double p = 1013.0;   // hPa
    double wind = 2.0;   // m s-1
    double shortwave = 0.0, longwave = 300.0;
};

// 结构：求解状态，逐节点传递：四组分温度、1/L（m-1）、场景显热（用于对流阵风）、
//       光照/阴影土壤的热历史与土柱。
struct EnergyState {
    double soilSunlit = 293.15, soilShaded = 293.15, leafSunlit = 293.15, leafShaded = 293.15;
    double inverseObukhov = 0.0;
    double sceneSensible = 0.0;
    SoilThermalState soilSunState, soilShadeState;
};

// 结构：节点求解输出的诊断量：场景加权的 Rn、H、LE、G，最大组分残差，稳定度/阵风残差，
//       u*、位移高度、粗糙度、冠层权重、Obukhov 长度，融化热汇，迭代次数与各类收敛/限幅标志。
struct EnergyDiagnostics {
    double netRadiation = 0, sensible = 0, latent = 0, ground = 0;
    double residual = 0;            // max component EB residual, W m-2
    double stabilityResidual = 0;   // |1/L - target|, m-1
    double gustResidual = 0;
    double soilSunlitFraction = 0, laiSunlit = 0;
    double airDensity = 0, ustar = 0, displacement = 0, roughness = 0, canopyWeight = 0;
    double obukhovLength = 0;
    double meltFlux = 0;            // W m-2 scene-weighted melt heat sink of temperature-capped (snow / ice) components
    int iterations = 0, stabilityIterations = 0;
    bool meltLimited = false;       // at least one component sat on the temperature cap with a surplus of energy
    bool converged = false, stabilityConverged = false, stabilityLimited = false, fallbackUsed = false;
};

// 接口：单节点求解，原地更新 state 并返回诊断量；bracketFallback 仅用于过境节点。
EnergyDiagnostics solveEnergyNode(const EnergySurface& surface, const Forcing& forcing,
                                  const ShortwaveAbsorption& shortwave, double szaDeg, bool sunlit,
                                  double dtSeconds, EnergyState& state, bool bracketFallback = false);
// bracketFallback (the overpass node): a node that has not closed after maxIterations continues with a bracketing / bisection
// search of 1/L (FIX 2026-11-24, see energy.cpp); earlier nodes keep the plain iteration (their history is not output).

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
