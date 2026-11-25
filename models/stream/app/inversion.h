// ==========================================================================
// 文件：inversion.h
// 功能：LST（地表辐射温度）/ LSCT（四组分温度）反演的接口与 QA 质量位定义
// 主要模块：
//   1. QA 质量位常量（位 0–4、8、9 沿用 LS4T base/quality.py；位 10–12 为 STREAM 扩展）
//   2. InversionInputs：反演所需的配置、栅格、地形与反射天空输入（引用，不拥有数据）
//   3. InversionResult：LSCT 后验组分温度、产品文件名与 summary
//   4. runInversion：执行反演并写出产品（实现见 inversion.cpp）
// 数据流：image.cpp 组装 InversionInputs 与背景组分温度 → runInversion → 产品 GeoTIFF 与 summary；
//         同化（assimilation.cpp）也复用这里的 QA 位常量，保证两者质量位定义一致
// 约定：温度 K；亮温/辐亮度的观测算子见 thermal_view.h；组分顺序 [土壤光照, 土壤阴影, 叶片光照, 叶片阴影]
//       QA 位含义与默认拒收掩膜（rejectQaBits）见 README“温度反演”一节
// ==========================================================================

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

// 模块：质量位（按位或写入 QA 波段）。位 5–7（LS4T 专用）STREAM 不设置，故此处没有对应常量。
// Quality bits; 0-8 follow LS4T base/quality.py, bit 9 is a STREAM extension.
constexpr unsigned kQaInputInvalid = 1u << 0;
constexpr unsigned kQaCloud = 1u << 1;
constexpr unsigned kQaOutOfRange = 1u << 2;
constexpr unsigned kQaRetrievalFailed = 1u << 3;
constexpr unsigned kQaIllConditioned = 1u << 4;
constexpr unsigned kQaLowSupport = 1u << 8;
constexpr unsigned kQaNotConverged = 1u << 9;
// FIX(2026-11-21 S-5): MAP stop reasons that are NOT failures (STREAM extensions, outside the default reject mask
// 895 = bits 0-6, 8, 9, so downstream users keep these analyses by default):
//   bit 10 line-search stalled: at least one Gauss-Newton step lowered the MAP cost, then no step along the GN
//          direction (fractions 1, 1/2, 1/4) lowered it further; the analysis is the lowest-cost evaluated state.
//   bit 11 not updated: no step lowered the cost at all; the analysis equals the background (J_a = J_b).
// Bit 9 (not converged, rejected by default) now means: iteration limit reached while the cost was still decreasing,
// the normal matrix was singular, or the operator failed on every trial step.
// FIX(2026-11-24 A2): bit 12 (4096) marks an assimilation pixel with no usable observation or no control variable; it is
// set together with bit 0 so existing "bit 0 = not analysed" masks keep working, while bit 0 alone still means invalid
// inputs (geometry, LAI, solar position).
constexpr unsigned kQaNoObservation = 1u << 12;
constexpr unsigned kQaLineSearchStalled = 1u << 10;
constexpr unsigned kQaNotUpdated = 1u << 11;

// 模块：反演输入。所有 vector 为整景栅格（按 grid 行优先排列），只处理 [startRow,endRow) ×
// [startColumn,endColumn) 范围；progressStart/progressSpan 为本阶段在总进度条中的起点与跨度（百分比）。
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
    // Downwelling longwave of the reflected-sky term (W m-2, NaN = none), same per-pixel field as the imaging;
    // null disables the term. skyFromObservations: RT "auto" fallback (Swinbank from the mean observed value).
    const std::vector<float>* skyLongwave = nullptr;
    bool skyFromObservations = false;
    // FIX(2026-11-21 S-1): terrain fields of the thermal observation operator (same object as the imaging and the
    // assimilation); null = horizontal surface.
    const TerrainFields* terrain = nullptr;
};

// 模块：反演结果。components 供成像使用（LSCT 无反演处保持背景）；summary 并入 scene_summary.json。
struct InversionResult {
    // LSCT: posterior component temperatures (background where no retrieval was possible); LST: empty.
    std::array<std::vector<float>, 4> components;
    std::vector<std::string> products;
    CPLJSONObject summary;
};

// 入口：background 为 LSCT 的背景四组分温度（LST 不使用）。
// Writes the retrieval products into config.outputDir.
InversionResult runInversion(const InversionInputs& inputs, const std::array<std::vector<float>, 4>& background);

}  // namespace stream
