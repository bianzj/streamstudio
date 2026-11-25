// ==========================================================================
// 文件：heat_conduction.h
// 功能：土壤热状态结构与地表热通量接口的声明（实现见 heat_conduction.cpp）
// 主要内容：
//   1. SoilThermalState：7 h 表面温度历史与 2 m 土柱剖面（以及 G(T) 线性化缓存）
//   2. ThermalMaterial：导热率、体积热容与热惰性
//   3. surfaceHeatFlux(T)：历史卷积法（surface）；depthHeatFlux：土柱法（depth）
//   4. initializeColumn / advanceThermalState：初始化与逐步推进
// 约定：G 为进入土壤的热通量（W m-2）；时间步 dtSeconds 内部至少取 1 s
// ==========================================================================

#pragma once
#include "dual.h"

#include <cmath>
#include <vector>

namespace stream {

// Thermal state of one soil surface: 7 h surface-temperature memory and the 2 m soil column.
struct SoilThermalState {
    std::vector<double> history;    // newest first, previous steps only
    std::vector<double> intervals;  // newest first durations between history entries
    std::vector<double> profile;    // depth method: surface + interior nodes
    // depth method, current step: the backward-Euler column is linear in the surface temperature, so
    // G = depthIntercept + depthSlope * T. Computed once per step, reset whenever the profile advances.
    mutable bool depthCached = false;
    mutable double depthDt = 0.0, depthConductivity = 0.0, depthCapacity = 0.0;
    mutable double depthIntercept = 0.0, depthSlope = 0.0;
};

// 结构：土壤热物性，缺省值与 energy 的 soilConductivity / soilVolumetricCapacity 缺省一致。
struct ThermalMaterial {
    double conductivity = 1.55;           // W m-1 K-1
    double volumetricCapacity = 1.298e6;  // J m-3 K-1
    double inertia() const { return std::sqrt(conductivity * volumetricCapacity); }
};

// Semi-infinite surface flux from the piecewise-linear 7 h temperature history [W m-2].
double surfaceHeatFlux(double temperature, const SoilThermalState& state, double dtSeconds, double inertia);
// Same flux on any scalar type (double or Ad): linear in the current temperature and in the thermal inertia.
template <class T>
T surfaceHeatFluxT(const T& temperature, const SoilThermalState& state, double dtSeconds, const T& inertia);

// Finite-volume soil column flux: top-layer conduction plus half-layer storage [W m-2].
double depthHeatFlux(double temperature, const SoilThermalState& state, double dtSeconds, const ThermalMaterial& material);

// Initialise the 2 m column from ERA5 soil temperatures: surface skin, level 1 (0-7 cm) at 3.5 cm and
// level 2 (7-28 cm) at 17.5 cm, held constant below. Linear in depth between those anchors.
void initializeColumn(std::vector<double>& profile, double surface, double level1, double level2);

// Commit the final node temperature: shift the history and advance the column once per time step.
void advanceThermalState(double temperature, SoilThermalState& state, double dtSeconds, const ThermalMaterial& material,
                         bool depthMethod);

}  // namespace stream
