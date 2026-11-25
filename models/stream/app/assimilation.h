// ==========================================================================
// 文件：assimilation.h
// 功能：STREAM“同化”模式的接口声明：EB 求解回调、输入/结果结构体与 runAssimilation。
// 主要模块：
//   1. EbControls / EbOutcome / EbSolver：EB 背景下，给定控制量（土壤湿度、Vcmax、LAI）
//      重解单像元能量平衡并返回温度、通量及温度对控制量的梯度
//   2. AssimilationInputs：同化所需的全部只读输入（配置、类表、栅格、地形、光谱表、
//      行列范围、进度区间、可选的反射天空长波）
//   3. AssimilationResult：分析组分温度与控制量、分析标记、产品列表、统计摘要
//   4. runAssimilation：入口函数
// 数据流：主流程组装 AssimilationInputs（及背景组分温度）→ runAssimilation（assimilation.cpp）
//   → AssimilationResult，产品写入 config.outputDir 的 analysis_* / background_* 文件。
// 约定：温度 K，通量 W m-2，土壤湿度 m3 m-3；组分顺序 Tss/Tsh/Tcs/Tch；
//   AssimilationInputs 的各栅格向量与 grid 同大小、行优先；指针成员为空表示该输入缺失。
// ==========================================================================

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

// 模块：EB 背景下的控制量与求解结果。EbOutcome::gradient[k][c] = dT_k / d(控制量 c)，
// c 依次为 土壤湿度、Vcmax、LAI；gradientValid 为 false 表示线性化不可用，分析应拒绝。
struct EbControls { double moisture = 0, vcmax = 0, lai = 0; };
struct EbOutcome {
    bool gradientValid = false;  // false means the analysis must reject the linearization
    std::array<double, 4> temperatures{};                  // Tss, Tsh, Tcs, Tch at the overpass
    std::array<std::array<double, 3>, 4> gradient{};       // dT_k / d[soil moisture, Vcmax, LAI]
    std::array<double, 4> fluxes{};                        // Rn, H, LE, G at the overpass
};
// Re-solves the energy balance of one pixel at the given controls and linearises it; false if it fails.
using EbSolver = std::function<bool(size_t pixel, const EbControls& controls, EbOutcome& outcome)>;

// 模块：同化输入（全部为引用/指针，调用期间须保持有效）。
// 说明：行列范围 [startRow,endRow) × [startColumn,endColumn)（半开区间）；
// progressStart/progressSpan 为本阶段在总进度条中的起点与跨度；
// diffuse/shortwave 仅 EB 提供，用于光学观测算子的漫射比例；
// soilBandReflectance 按土壤湿度步（见 assimilation.cpp 的 kMoistureSteps）和光学波段索引。
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

// 模块：同化输出。未被分析的像元：components 保持背景值，其余分析量为 NaN。
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
