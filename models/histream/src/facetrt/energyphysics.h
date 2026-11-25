// ==========================================================================
// 文件：energyphysics.h
// 功能：FacetRT/FacetEB（面元能量平衡）的物理公式与数值小求解器，纯头文件、无 Vulkan 依赖。
// 主要模块：
//   1. 能量平衡收敛判据（残差阈值 2 W m-2、失败面元比例 < 0.1 %、等效温度残差 ≤ 0.5 K）
//   2. 土壤 12 层有限体积（深 3 m）的后向欧拉响应 T_i = offset_i + slope_i * T_surface
//   3. 阳照/阴影两态之间的面积守恒状态转移（sunlitStateTransfer）
//   4. 建筑墙/屋面多层（最多 6 层 × 10 单元）有限体积及室内 Robin 边界的同类响应
//   5. Monin-Obukhov 相似函数 psiM/psiH/phiH 与冠层空气动力学阻抗 aerodynamics()
//   6. 地面自由对流导度（Kondo & Ishida 1997 / TSEB）与 1/L 估计（inverseObukhov）
//   7. 叶片温度牛顿-二分求根（temperatureRoot）与叶温-气孔阻抗耦合求解（coupledLeaf、stomatalRoot）
// 数据流：facetrtcore.cpp 在每个节点先调用 soilResponse/wallResponse 把内部消元成对表面温度的
//   线性响应，耦合迭代中只更新表面温度；再用 aerodynamics 得到叶/土阻抗、groundExchange 得到
//   地面显热交换，最后用 facetEnergyViolation/facetEnergyConverged 判断是否收敛。
// 约定：温度 K；热通量 W m-2；阻抗 s m-1；导度 m s-1；长度 m；容积热容 J m-3 K-1；
//   土壤/墙体通量 flux 以向下（进入介质）为正；逆 Obukhov 长度 1/L 单位 m-1，ζ = z/L 限于 [-5, 1]。
// 与其他文件：facetrtcore.cpp 是本文件函数的主要调用方（GPU 侧 energy.comp 用同一公式，（待核对））。
// ==========================================================================

#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace facetphysics {

// 模块：能量平衡收敛判据。单个面元侧的残差 R = 净辐射 + 内部/源项 - 显热 - 潜热 - 储热
// （物体面元再减光伏）；|R| ≥ 2 W m-2 或非有限数即为“失败”。
inline constexpr double facetEnergyResidualThreshold = 2.0;
// Cap on the largest temperature-equivalent residual |R| / (dR/dT) of any
// facet side. The 0.1 % count alone let a converged node keep a few facets
// tens of W/m2 (> 1 K) out of balance.
inline constexpr double facetEnergyMaxEquivalentK = 0.5;
inline bool facetEnergyViolation(double residual) {
    return !std::isfinite(residual) || std::abs(residual) >= facetEnergyResidualThreshold;
}
// 收敛条件之一：失败面元数占比严格小于 0.1 %（无面元时视为未收敛）。
inline bool facetEnergyConverged(uint32_t failingFacets, uint32_t totalFacets) {
    // Strictly less than 0.1%; integer arithmetic avoids boundary rounding.
    return totalFacets > 0 && uint64_t(failingFacets) * 1000U < totalFacets;
}
// 完整收敛条件：失败面元比例达标，且最大等效温度残差（|R| / (dR/dT)，K）有限并 ≤ 0.5 K。
inline bool facetEnergyConverged(uint32_t failingFacets, uint32_t totalFacets,
                                 double maximumEquivalentK) {
    return facetEnergyConverged(failingFacets, totalFacets) &&
        std::isfinite(maximumEquivalentK) &&
        maximumEquivalentK <= facetEnergyMaxEquivalentK;
}

// 模块：土壤一维热传导（FacetEB 土壤方法 2 的土柱）。13 个层界给出 12 个单元，
// 单元中心离散；上边界是待求的表面温度，下边界为固定温度（Dirichlet）。
// Cell-centred finite volumes, metres below the surface. The lower Dirichlet
// boundary stays at the configured initial soil temperature, at 3 m depth.
inline constexpr std::array<double, 13> soilEdges{0, .01, .025, .05, .1, .2, .35, .55, .8, 1.1, 1.5, 2, 3};
inline constexpr size_t soilLayers = soilEdges.size() - 1;
using SoilProfile = std::array<double, soilLayers>;
// 土壤对表面温度的线性响应：消去内部单元后，新时刻各层温度 T_i = offset_i + slope_i * Ts，
// conductance 为表面到第 1 层中心的导热导度（W m-2 K-1）。flux() 返回进入土壤的热通量 G（W m-2）。
struct SoilResponse {
    SoilProfile offset{}, slope{}; // T_i(new) = offset_i + slope_i * T_surface(new)
    double conductance{};
    double fluxSlope() const { return conductance * (1 - slope[0]); }
    double flux(double surface) const { return conductance * ((1 - slope[0]) * surface - offset[0]); }
    SoilProfile profile(double surface) const {
        SoilProfile result{};
        for (size_t i = 0; i < soilLayers; ++i) result[i] = offset[i] + slope[i] * surface;
        return result;
    }
};

// Area-conserving transfer between the sunlit (0) and shaded (1) thermal
// states of one surface when its sunlit part changes from pOld to pNew. The
// area that changes state carries its own history: if p falls, the share
// pOld-pNew leaves the sunlit state and is merged into the shaded state
// (area-weighted mean), otherwise pNew-pOld moves from shaded to sunlit. The
// donor state keeps its values, so sum(p * C * T) is unchanged. `target` < 0
// means nothing moves (p unchanged); then no value may be touched.
struct SunlitStateTransfer { int target{-1}; double keep{1}, take{0}; };
inline SunlitStateTransfer sunlitStateTransfer(double pOld, double pNew) {
    SunlitStateTransfer t;
    if (!(pOld != pNew) || !std::isfinite(pOld) || !std::isfinite(pNew)) return t;
    pOld = std::clamp(pOld, 0.0, 1.0); pNew = std::clamp(pNew, 0.0, 1.0);
    if (pOld == pNew) return t;
    const bool toShade = pNew < pOld;
    const double moved = std::abs(pOld - pNew);
    const double resident = toShade ? 1.0 - pOld : pOld;
    t.target = toShade ? 1 : 0;
    t.keep = resident / (resident + moved);
    t.take = moved / (resident + moved);
    return t;
}
// 把转移系数应用到一对晴/阴状态量（温度、土壤/墙体剖面各层等）：只改写接收一侧，供体不变。
template <class T>
inline void applySunlitStateTransfer(const SunlitStateTransfer& t, T& sunlit, T& shaded) {
    if (t.target < 0) return;
    T& target = t.target == 0 ? sunlit : shaded;
    const T& source = t.target == 0 ? shaded : sunlit;
    target = static_cast<T>(t.keep * static_cast<double>(target) + t.take * static_cast<double>(source));
}

// 模块：土壤后向欧拉响应。输入：上一时刻剖面 previous（K）、底边界温度（K）、导热系数
// （W m-1 K-1）、容积热容（J m-3 K-1）、时间步 dt（s）；输出：SoilResponse。
// 用追赶法（Thomas）的前向消元与回代，同时携带“对表面温度的系数”（boundary）以得到 slope。
// Backward Euler: C_i (T_i-new - T_i-old)/dt = q_upper - q_lower.
// Eliminate the tridiagonal interior once per node, not once per EB iteration.
inline SoilResponse soilResponse(const SoilProfile& previous, double bottomTemperature,
                                 double conductivity, double volumetricCapacity, double dt) {
    // 步骤 1：参数必须为正且有限，否则直接抛错，避免静默产生 NaN。
    if (!(conductivity > 0 && volumetricCapacity > 0 && dt > 0) ||
        !std::isfinite(conductivity + volumetricCapacity + dt + bottomTemperature))
        throw std::runtime_error("Invalid FacetEB soil thermal parameters");
    // 步骤 2：逐层组装三对角方程并前向消元；第 0 层的上邻是表面（取半个单元距离），
    // boundary[i] 记录 Ts 对该层方程右端的贡献系数。
    SoilProfile upper{}, diagonal{}, rhs{}, boundary{};
    for (size_t i = 0; i < soilLayers; ++i) {
        if (!std::isfinite(previous[i])) throw std::runtime_error("Nonfinite soil profile");
        const double width = soilEdges[i + 1] - soilEdges[i];
        // up/down：与上/下相邻节点的导度；两单元中心间距为两侧半宽之和，首/末单元到边界取半宽。
        const double up = conductivity / (i ? .5 * (soilEdges[i + 1] - soilEdges[i - 1]) : .5 * width);
        const double down = conductivity / (i + 1 < soilLayers ? .5 * (soilEdges[i + 2] - soilEdges[i]) : .5 * width);
        const double storage = volumetricCapacity * width / dt;
        diagonal[i] = storage + up + down;
        upper[i] = i + 1 < soilLayers ? -down : 0;
        // 最深单元把固定的底边界温度作为已知项移到右端。
        rhs[i] = storage * previous[i] + (i + 1 == soilLayers ? down * bottomTemperature : 0);
        boundary[i] = i == 0 ? up : 0;
        if (i) {
            const double factor = -up / diagonal[i - 1];
            diagonal[i] -= factor * upper[i - 1];
            rhs[i] -= factor * rhs[i - 1];
            boundary[i] -= factor * boundary[i - 1];
        }
    }
    SoilResponse response;
    // 表面到第 1 层中心的导度，用于 flux()/fluxSlope()。
    response.conductance = conductivity / (.5 * (soilEdges[1] - soilEdges[0]));
    // 步骤 3：从最深层向上回代，分别得到 offset 与 slope（二者都是对 Ts 线性展开的系数）。
    for (size_t i = soilLayers; i-- > 0;) {
        response.offset[i] = (rhs[i] - (i + 1 < soilLayers ? upper[i] * response.offset[i + 1] : 0)) / diagonal[i];
        response.slope[i] = (boundary[i] - (i + 1 < soilLayers ? upper[i] * response.slope[i + 1] : 0)) / diagonal[i];
    }
    return response;
}

// Building wall/roof construction (FacetEB material wallMode "wall"): up to
// maxWallLayers layers from the outer to the inner face. Every layer is split
// into the same cell-centred finite volumes refined towards both of its faces.
// The outer face carries the solved surface temperature; the inner face
// exchanges with the indoor air (combined convection + radiation h_in) through
// the half-cell conduction: q_in = (T_last - T_in) / (w/2k + 1/h_in). Two
// cells of one layer are coupled by k / ((w_a + w_b)/2); cells on either side
// of a layer interface by the series resistance w_a/2k_a + w_b/2k_b (the
// width-weighted harmonic mean). One layer is the original homogeneous slab,
// operation for operation.
inline constexpr std::array<double, 11> wallEdgeFractions{0, .025, .07, .15, .3, .5, .7, .85, .93, .975, 1};
inline constexpr size_t wallCellsPerLayer = wallEdgeFractions.size() - 1;
inline constexpr size_t maxWallLayers = 6;
inline constexpr size_t maxWallCells = wallCellsPerLayer * maxWallLayers;
using WallProfile = std::array<double, maxWallCells>;
// 墙体单层参数：thickness（m）、conductivity（W m-1 K-1）、volumetricCapacity（J m-3 K-1）；
// 缺省值对应原来的单层均质板。
struct WallLayer {
    double thickness{.2}, conductivity{1.4}, volumetricCapacity{2.024e6};
};
// 多层墙体/屋面：layers[0] 为最外层。每层固定分 wallCellsPerLayer 个单元，
// 单元 i 属于第 i / wallCellsPerLayer 层。indoorK 为室内空气温度（K），
// indoorCoefficient 为室内综合换热系数 h_in（W m-2 K-1，含对流与辐射）。
struct WallSlab {
    std::array<WallLayer, maxWallLayers> layers{};
    size_t layerCount{1};
    double indoorK{295.15}, indoorCoefficient{8};
    // 参数校验：层数 1–6，单层厚度 ≤ 5 m 且总厚 ≤ 5 m，各物性为正有限数，
    // 室内温度在 (200, 400) K 内；不合格抛 std::runtime_error。
    void validate() const {
        double total = 0;
        bool valid = layerCount >= 1 && layerCount <= maxWallLayers && indoorCoefficient > 0 &&
                     indoorK > 200 && indoorK < 400 && std::isfinite(indoorK + indoorCoefficient);
        for (size_t l = 0; valid && l < layerCount; ++l) {
            const auto& layer = layers[l];
            valid = layer.thickness > 0 && layer.thickness <= 5 && layer.conductivity > 0 &&
                    layer.volumetricCapacity > 0 &&
                    std::isfinite(layer.thickness + layer.conductivity + layer.volumetricCapacity);
            total += layer.thickness;
        }
        if (!valid || !(total <= 5))
            throw std::runtime_error("Invalid FacetEB wall parameters (layers/thickness, lambdas, rhos, cs, indoor)");
    }
    size_t cells() const { return layerCount * wallCellsPerLayer; }
    const WallLayer& layerOf(size_t i) const { return layers[i / wallCellsPerLayer]; }
    double thickness() const {
        double sum = 0;
        for (size_t l = 0; l < layerCount; ++l) sum += layers[l].thickness;
        return sum;
    }
    double width(size_t i) const {
        const size_t j = i % wallCellsPerLayer;
        return layerOf(i).thickness * (wallEdgeFractions[j + 1] - wallEdgeFractions[j]);
    }
    double conductivity(size_t i) const { return layerOf(i).conductivity; }
    double capacity(size_t i) const { return layerOf(i).volumetricCapacity; }
    // Conductance between cells i - 1 and i (0 < i < cells()).
    double innerConductance(size_t i) const {
        if (i % wallCellsPerLayer) return conductivity(i) / (.5 * (width(i) + width(i - 1)));
        return 1.0 / (.5 * width(i - 1) / conductivity(i - 1) + .5 * width(i) / conductivity(i));
    }
    // Conductance between the outer surface and the first cell centre.
    double surfaceConductance() const { return conductivity(0) / (.5 * width(0)); }
    double indoorConductance() const {
        const size_t last = cells() - 1;
        return 1.0 / (.5 * width(last) / conductivity(last) + 1.0 / indoorCoefficient);
    }
    // 最内层单元到室内空气的热流（W m-2，指向室内为正）。
    double indoorFlux(const WallProfile& p) const { return indoorConductance() * (p[cells() - 1] - indoorK); }
    // 墙体内表面温度（K）：室内气温加上热流经 h_in 的温降。
    double innerSurfaceK(const WallProfile& p) const { return indoorK + indoorFlux(p) / indoorCoefficient; }
    // 单位墙面积的热含量 Σ C_i * w_i * T_i（J m-2，以 0 K 为基准），用于储热/闭合诊断。
    double heat(const WallProfile& p) const {
        double sum = 0;
        for (size_t i = 0; i < cells(); ++i) sum += capacity(i) * width(i) * p[i];
        return sum;
    }
    // Steady conduction between the surface and the indoor air (series
    // resistances of the layers and 1/h_in).
    WallProfile steadyProfile(double surfaceK) const {
        double resistance = 0;
        for (size_t l = 0; l < layerCount; ++l) resistance += layers[l].thickness / layers[l].conductivity;
        const double total = resistance + 1.0 / indoorCoefficient;
        WallProfile p{};
        double before = 0;
        for (size_t i = 0; i < cells(); ++i) {
            const size_t j = i % wallCellsPerLayer;
            const auto& layer = layerOf(i);
            // before：当前层之前各整层的热阻之和（m2 K W-1）。
            if (i && !j) before += layers[i / wallCellsPerLayer - 1].thickness / layers[i / wallCellsPerLayer - 1].conductivity;
            const double depth = layer.thickness * .5 * (wallEdgeFractions[j] + wallEdgeFractions[j + 1]);
            p[i] = surfaceK + (indoorK - surfaceK) * (before + depth / layer.conductivity) / total;
        }
        return p;
    }
};
// 墙体对表面温度的线性响应，含义同 SoilResponse；cells 为有效单元数。
struct WallResponse {
    WallProfile offset{}, slope{}; // T_i(new) = offset_i + slope_i * T_surface(new)
    double conductance{};
    size_t cells{};
    double fluxSlope() const { return conductance * (1 - slope[0]); }
    double flux(double surface) const { return conductance * ((1 - slope[0]) * surface - offset[0]); }
    WallProfile profile(double surface) const {
        WallProfile result{};
        for (size_t i = 0; i < cells; ++i) result[i] = offset[i] + slope[i] * surface;
        return result;
    }
};
// Backward Euler, as soilResponse, with the indoor Robin boundary.
// innerSource (W m-2 of wall area, default 0): internal heat released in the
// innermost cell (machinery behind a ship plate); 0 leaves the old arithmetic.
inline WallResponse wallResponse(const WallProfile& previous, const WallSlab& slab, double dt, double innerSource = 0) {
    // 步骤 1：先校验墙体与时间步，非法输入直接抛错。
    slab.validate();
    if (!(dt > 0)) throw std::runtime_error("Invalid FacetEB wall time step");
    const size_t n = slab.cells();
    // 步骤 2：组装三对角并前向消元；最内层以室内 Robin 边界（indoorConductance、indoorK）闭合。
    WallProfile upper{}, diagonal{}, rhs{}, boundary{};
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(previous[i])) throw std::runtime_error("Nonfinite wall profile");
        const double width = slab.width(i);
        const double up = i ? slab.innerConductance(i) : slab.surfaceConductance();
        const double down = i + 1 < n ? slab.innerConductance(i + 1) : slab.indoorConductance();
        const double storage = slab.capacity(i) * width / dt;
        diagonal[i] = storage + up + down;
        upper[i] = i + 1 < n ? -down : 0;
        rhs[i] = storage * previous[i] + (i + 1 == n ? down * slab.indoorK : 0);
        // 内热源（如船体钢板后的机械热）加到最内单元；为 0 时算术与旧版完全一致。
        if (i + 1 == n && innerSource != 0) rhs[i] += innerSource;
        boundary[i] = i == 0 ? up : 0;
        if (i) {
            const double factor = -up / diagonal[i - 1];
            diagonal[i] -= factor * upper[i - 1];
            rhs[i] -= factor * rhs[i - 1];
            boundary[i] -= factor * boundary[i - 1];
        }
    }
    WallResponse response;
    response.cells = n;
    response.conductance = slab.surfaceConductance();
    // 步骤 3：回代得到 offset 与 slope。
    for (size_t i = n; i-- > 0;) {
        response.offset[i] = (rhs[i] - (i + 1 < n ? upper[i] * response.offset[i + 1] : 0)) / diagonal[i];
        response.slope[i] = (boundary[i] - (i + 1 < n ? upper[i] * response.slope[i + 1] : 0)) / diagonal[i];
    }
    return response;
}

// Paulson unstable / linear stable MOST functions, with explicit applicability
// bounds -5 <= z/L <= 1. These are similarity parameterizations, not CFD.
inline double psiM(double zeta) {
    zeta = std::clamp(zeta, -5.0, 1.0);
    if (zeta >= 0) return -5 * zeta;
    const double x = std::pow(1 - 16 * zeta, .25);
    return 2 * std::log((1 + x) / 2) + std::log((1 + x * x) / 2) - 2 * std::atan(x) + 1.5707963267948966;
}
inline double psiH(double zeta) {
    zeta = std::clamp(zeta, -5.0, 1.0);
    return zeta >= 0 ? -5 * zeta : 2 * std::log((1 + std::sqrt(1 - 16 * zeta)) / 2);
}
// 热量的无量纲梯度函数 φ_H：稳定 1 + 5ζ，不稳定 (1 - 16ζ)^(-1/2)；用于粗糙子层积分。
inline double phiH(double zeta) {
    zeta = std::clamp(zeta, -5.0, 1.0);
    return zeta >= 0 ? 1 + 5 * zeta : 1 / std::sqrt(1 - 16 * zeta);
}
// 空气动力学结果：叶片/土壤的显热-水汽交换阻抗（s m-1）、摩擦速度 u*（m s-1）、
// 有效高度 effectiveHeight（= 参考高度 - 位移高度 d，m，用于把 1/L 换算成 ζ）。
struct Aerodynamics {
    float leafResistance{}, soilResistance{};
    double frictionVelocity{}, effectiveHeight{};
};
// 模块：单个地面柱的冠层空气动力学阻抗。
// 输入：参考高度（m）、风速（m s-1）、冠层高度 height（m）、LAI、叶宽 leafWidth（m）、
// 1/L（m-1，缺省 0 即中性）；输出：Aerodynamics。
// 说明：无植被/极稀疏时用对数廓线（z0 = 0.01 m）；有植被时分解为冠层上方 rai、粗糙子层 rar、
// 冠层内 rac 与土壤侧 rws，叶/土阻抗再乘（LAI+1）修正（公式来源（待核对））。
inline Aerodynamics aerodynamics(double referenceHeight, double wind, double height,
                                 double lai, double leafWidth, double inverseL = 0) {
    // kappa 为 von Karman 常数；cd 为冠层拖曳系数（取 0.3，来源（待核对））。
    constexpr double kappa = .4, cd = .3;
    // 风速下限 0.05 m s-1，避免静风时除零。
    const double u = std::max(.05, wind);
    Aerodynamics bare{};
    // 步骤 1：裸地或 LAI ≤ 0.01：按对数廓线（含稳定度修正）算 r_aa，叶/土取同一值（下限 1 s m-1）。
    if (height <= .01 || lai <= .01) {
        const double z = std::max(2.0, referenceHeight) - .01, z0 = .01;
        const double momentum = std::log(z / z0) - psiM(z * inverseL) + psiM(z0 * inverseL);
        const double heat = std::log(z / z0) - psiH(z * inverseL) + psiH(z0 * inverseL);
        const double ustar = std::max(.001, kappa * u / std::max(.1, momentum));
        const float raa = static_cast<float>(std::max(1.0, heat / (kappa * ustar)));
        bare = {raa, raa, ustar, z};
        // A positive amount of foliage still needs leaf exchange. The old
        // LAI=0.01 soil cutoff must not switch leaves to bare-ground resistance.
        if (height <= .01 || lai <= 0) return bare;
    }
    // 步骤 2：有植被冠层。参考高度至少高出冠层顶 max(2 m, 0.5h)。
    const double h = height, area = lai;
    const double z = std::max(referenceHeight, h + std::max(2.0, .5 * h));
    // 步骤 3：位移高度 d、粗糙度 z0 与摩擦速度 u*（经验式，常数 20.6、3.3 来源（待核对））。
    const double sq = std::sqrt(20.6 * area / 2);
    const double g1 = std::max(3.3, std::sqrt(.01 + cd * area / 2));
    const double d = h * (1 + std::expm1(-sq) / sq);
    const double z0 = (h - d) * std::exp(-kappa * g1 + .2);
    const double zr = 2.5 * h, n = cd * area / (2 * kappa * kappa);
    const double ustar = std::max(.001, kappa * u /
        std::max(.1, std::log((z - d) / z0) - psiM((z - d) * inverseL) + psiM(z0 * inverseL)));
    // 步骤 4：粗糙子层顶 zr = 2.5h 以上到参考高度的阻抗 rai；参考高度不高于 zr 时为 0。
    const double rai = z > zr ? (std::log((z - d) / (zr - d)) -
        psiH((z - d) * inverseL) + psiH((zr - d) * inverseL)) / (kappa * ustar) : 0;
    // 步骤 5：粗糙子层（冠层顶 h 到 min(z, zr)）的热量修正 φ_H，用 8 点中点法积分得到 rar。
    // Integrate the roughness-sublayer heat correction; exact neutral limit.
    double roughnessPhi = 0;
    const double top = std::min(z, zr);
    for (int k = 0; k < 8; ++k)
        roughnessPhi += phiH((h + (top - h) * (k + .5) / 8 - d) * inverseL) / 8;
    const double rar = (top - h) / (zr - d) * roughnessPhi / (kappa * ustar);
    // 步骤 6：冠层内部涡扩散系数 kh 与冠层顶风速 uh、z0+d 处风速 uz0（指数衰减，系数 n）。
    const double kh = kappa * ustar * (zr - d) / phiH((h - d) * inverseL);
    const double uh = std::max(.01, ustar / kappa *
        (std::log((h - d) / z0) - psiM((h - d) * inverseL) + psiM(z0 * inverseL)));
    const double uz0 = uh * std::exp(n * ((z0 + d) / h - 1));
    // 步骤 7：冠层内 rac（冠层顶到 z0+d）与 rws（z0+d 到土面 0.01 m），按 tanh 型扩散系数
    // 廓线对高度积分的解析形式（公式来源（待核对））。
    const auto profile = [&](double y) {
        return std::log(std::tanh(n * y / (2 * h)));
    };
    const double scale = h * std::sinh(n) / (n * kh);
    const double rac = scale * (profile(h) - profile(z0 + d));
    const double rws = scale * (profile(z0 + d) - profile(.01));
    // 70/LAI*sqrt(w/u) is a canopy boundary resistance per ground area.
    // Facet H/LE are per one-sided leaf area: multiply it by LAI, cancelling
    // the dilution factor. Soil does not share the leaf boundary layer.
    const double leafBoundary = 70 * std::sqrt(std::max(.0001, leafWidth) / uz0);
    // 步骤 8：汇总各段阻抗，叶片与土壤分别加上自己的边界层项。
    const double raa = rai + rar + rac;
    // Retain the existing equivalent common-air-path approximation; apply
    // (LAI+1) only to that common path, not to the leaf boundary branch.
    const double leaf = std::max(1.0, (area + 1) * raa + leafBoundary);
    // 土壤：共同路径 + 冠层内到土面 rws + 土壤边界层阻抗（固定 10 s m-1），同样乘（LAI+1）。
    const double soil = std::max(1.0, (area + 1) * (raa + rws + 10));
    if (!std::isfinite(leaf + soil)) throw std::runtime_error("Nonfinite FacetEB aerodynamic resistance");
    // 步骤 9：0 < LAI ≤ 0.01 的极稀疏植被：只替换叶片阻抗，土壤阻抗与 u*、有效高度沿用裸地结果。
    if (lai <= .01) {
        bare.leafResistance = static_cast<float>(leaf);
        return bare; // Preserve soil resistance and its stability parameters.
    }
    return {static_cast<float>(leaf), static_cast<float>(soil), ustar, z - d};
}
// Free convection of ground facets (soil, roads: every non-object facet), the
// same term as VoxelEB (shader/voxeleb/fluid_coupling.glsl). The bare-ground
// branch above is a log profile from z0 = 0.01 m to the reference height; at
// noon in light wind (u 0.43 m/s at 15 m, Ts - Ta 12 K, bulk Richardson
// number ~30) it gives r_a ~ 770 s/m, i.e. 1.3 W m-2 K-1, outside the range
// of any neutral or MOST (zeta >= -5) formula. When the surface is warmer than
// the air the windless conductance of Kondo & Ishida (1997), as used in TSEB
// (Kustas & Norman 1999), g_free ~ 0.0025 (Ts - Ta)^(1/3) m/s, is added in
// parallel; 0.0025 x (x^2 + 1)^(-1/3) keeps the asymptote with a finite slope
// at x = 0. With the MOST correction the larger of the two conductances is
// used (buoyancy is then already in 1/r_a). Stable/neutral sides keep r_a.
// groundFreeConductance：excess = Ts - Ta（K），限幅到 [0, 60]；返回自由对流导度 g_free（m s-1）。
inline float groundFreeConductance(float excess) {
    const float x = std::clamp(excess, 0.0f, 60.0f);
    return 0.0025f * x * std::pow(x * x + 1.0f, -1.0f / 3.0f);
}
// d(g_free * excess)/d(excess) - g_free: extra sensible-heat slope.
inline float groundFreeConductanceExtraSlope(float excess) {
    if (!(excess > 0.0f) || excess >= 60.0f) return 0.0f;
    const float x = excess, s = x * x + 1.0f;
    const float dg = 0.0025f * (std::pow(s, -1.0f / 3.0f) - (2.0f / 3.0f) * x * x * std::pow(s, -4.0f / 3.0f));
    return x * dg;
}
struct GroundExchange { float resistance, extraSlope; };
// Effective ground resistance [s/m] and the extra slope [m/s] of the free
// convection branch in use. An unchanged resistance is returned bit for bit.
inline GroundExchange groundExchange(float resistance, float excess, bool stabilityCorrection) {
    const float free = groundFreeConductance(excess);
    if (!(free > 0.0f)) return {resistance, 0.0f};
    // 开启 MOST 稳定度修正时浮力已部分计入 1/r_a，故取两种导度中较大者；
    // 未开启时（中性）把自由对流导度与 1/r_a 并联相加。
    if (stabilityCorrection) {
        if (!(free > 1.0f / resistance)) return {resistance, 0.0f};
        return {1.0f / free, groundFreeConductanceExtraSlope(excess)};
    }
    return {1.0f / (1.0f / resistance + free), groundFreeConductanceExtraSlope(excess)};
}

// 模块：逆 Obukhov 长度 1/L = -κ g H / (ρ cp T u*^3)（m-1）。
// 输入：柱平均显热 H（W m-2）、空气密度 rho（kg m-3）、气温（K）、aerodynamics() 结果。
// 说明：ζ = (z-d)/L 先限幅到 [-5, 1]（MOST 适用范围）再换算回 1/L；κ=0.4，cp=1005 J kg-1 K-1。
inline double inverseObukhov(double sensibleHeat, double rho, double temperature, const Aerodynamics& a) {
    const double raw = -.4 * 9.81 * sensibleHeat /
        (rho * 1005 * temperature * std::pow(a.frictionVelocity, 3));
    return std::clamp(raw * a.effectiveHeight, -5.0, 1.0) / a.effectiveHeight;
}

// 模块：叶片求根的未收敛诊断计数（进程内全局，原子计数，可在多线程求解中安全累加）。
// 只统计、不改变任何求解结果：temperatureRoot / coupledLeaf 在未满足收敛条件而退出时各加 1，
// 由 facetrtcore.cpp 在每次运行开始时清零、结束时写入日志与 faceteb_run.json 的 solverDiagnostics。
struct SolverDiagnostics {
    std::atomic<std::uint64_t> temperatureRootCalls{0};        // temperatureRoot 调用次数
    std::atomic<std::uint64_t> temperatureRootUnconverged{0};  // 其中未收敛退出的次数（括号收窄到容差仍不满足修正条件，或 32 次用完）
    std::atomic<std::uint64_t> coupledLeafCalls{0};            // coupledLeaf 调用次数
    std::atomic<std::uint64_t> coupledLeafUnconverged{0};      // 其中 24 次 ln r_s 二分用完仍未满足容差的次数
    void reset() {
        temperatureRootCalls = 0; temperatureRootUnconverged = 0;
        coupledLeafCalls = 0; coupledLeafUnconverged = 0;
    }
};
inline SolverDiagnostics& solverDiagnostics() {
    static SolverDiagnostics diagnostics;
    return diagnostics;
}

// 模块：叶片温度与气孔阻抗的耦合求解。leafThermalToleranceK 为叶温内层求根容差（K）。
inline constexpr double leafThermalToleranceK = 0.01;
// Fixed-resistance thermal equation is monotone; safeguard Newton by its bracket.
// 说明：括号固定为 [220,360] K。真实根在括号外（强辐射下的深色屋顶/光伏、寒夜）时会被钳在边界；
// 32 次未收敛或括号收窄到容差仍不满足修正条件时返回当前值（求解结果保持不变）。这些情形现在会计入
// solverDiagnostics().temperatureRootUnconverged，由调用方在日志与 faceteb_run.json 中报告。
template<class Energy>
double temperatureRoot(double initial, Energy&& evaluate) {
    solverDiagnostics().temperatureRootCalls.fetch_add(1, std::memory_order_relaxed);
    // 步骤 1：在固定括号 [220, 360] K 内从 initial 起步。
    double lower = 220, upper = 360, temperature = std::clamp(initial, lower, upper);
    for (int i = 0; i < 32; ++i) {
        const auto value = evaluate(temperature); // residual, minus its temperature derivative
        if (!std::isfinite(value[0] + value[1])) throw std::runtime_error("Nonfinite leaf thermal equation");
        // 步骤 2：牛顿修正量；斜率下限取 1 W m-2 K-1，防止导数接近 0 时步长过大。
        const double correction = value[0] / std::max(1.0, value[1]);
        // Apply the already computed sub-tolerance correction before exit.
        // Otherwise roundoff across the exit boundary causes a ~0.01 K jump.
        // No additional evaluation or iteration is needed.
        if (std::abs(correction) <= leafThermalToleranceK)
            return std::clamp(temperature + correction, lower, upper);
        // 步骤 3：括号已窄于容差仍未满足修正条件时停止（按未收敛计数，见上方说明）。
        if (upper - lower <= leafThermalToleranceK) break;
        // 步骤 4：残差为正说明根在更高温度，据此收紧括号；牛顿试探点落在括号外则改用二分。
        if (value[0] > 0) lower = temperature; else upper = temperature;
        const double trial = temperature + value[0] / std::max(1.0, value[1]);
        temperature = trial > lower && trial < upper ? trial : .5 * (lower + upper);
    }
    // 只有 break 或 32 次用完才会走到这里（正常收敛已在循环内返回）：计为未收敛。
    solverDiagnostics().temperatureRootUnconverged.fetch_add(1, std::memory_order_relaxed);
    return temperature;
}
// 叶片状态：叶温（K）与气孔阻抗 r_s（s m-1）。
struct LeafState { double temperature, resistance; };
inline constexpr int leafStomatalMaxIterations = 24;
inline constexpr double leafStomatalLogTolerance = 0.01;
// 模块：叶温-气孔阻抗耦合。外层在 ln r_s 上二分（范围 1–625000 s m-1，最多 24 次）；
// 每个试探 r_s 先由 thermal(r) 解能量平衡得叶温 T，再由 physiology(T, r) 给出目标阻抗，
// 比较 ln r_s 与 ln target 决定缩小哪一侧。
// Eliminate temperature through energy balance, then solve the stomatal equation.
// Reversing this order avoids branch jumps in humidity-only roots at fixed T.
template<class Thermal, class Physiology>
LeafState coupledLeaf(Thermal&& thermal, Physiology&& physiology) {
    solverDiagnostics().coupledLeafCalls.fetch_add(1, std::memory_order_relaxed);
    // 步骤 1：在 ln r_s 空间设括号。
    double lower = std::log(1.0), upper = std::log(625000.0);
    for (int i = 0; i < leafStomatalMaxIterations; ++i) {
        const double middle = .5 * (lower + upper), resistance = std::exp(middle);
        // 步骤 2：同一 r_s 下先解叶温，再求生理模型的目标阻抗。
        const double temperature = thermal(resistance), target = physiology(temperature, resistance);
        if (!(target > 0) || !std::isfinite(target)) throw std::runtime_error("Invalid joint stomatal resistance");
        // This trial already solves the thermal equation at the same resistance.
        if (std::abs(middle - std::log(target)) <= leafStomatalLogTolerance)
            return {temperature, resistance};
        // 步骤 3：试探值大于目标则缩小上界，否则抬高下界。
        if (middle > std::log(target)) upper = middle; else lower = middle;
    }
    // 步骤 4：达最大迭代次数仍未满足容差时，取括号中点并重新解叶温（结果不变，仅计入未收敛诊断计数）。
    solverDiagnostics().coupledLeafUnconverged.fetch_add(1, std::memory_order_relaxed);
    const double resistance = std::exp(.5 * (lower + upper));
    return {thermal(resistance), resistance};
}

// 模块：固定叶温下求气孔阻抗 r_s。输入：光合模型 model（给出目标阻抗）、叶内饱和水汽压 saturation、
// 环境水汽压 ambient（与 saturation 同单位，（待核对））、叶边界层阻抗（s m-1）。
// 做 24 次 ln r_s 二分，不早停，结果与迭代历史无关。
// Solve r_s = r_bio(T, e_leaf(r_s)) in log resistance, without iteration-history
// dependence. The caller supplies the selected photosynthesis model.
template<class Model>
double stomatalRoot(Model&& model, double saturation, double ambient, double boundaryResistance) {
    double lower = std::log(1.0), upper = std::log(625000.0);
    for (int i = 0; i < 24; ++i) {
        const double middle = (lower + upper) / 2;
        const double resistance = std::exp(middle);
        // 叶表面水汽压：叶内饱和值经气孔与边界层串联向环境扩散后在叶表面处的值。
        const double vapor = ambient + (saturation - ambient) * boundaryResistance / (boundaryResistance + resistance);
        const double target = model(vapor);
        if (!(target > 0) || !std::isfinite(target)) throw std::runtime_error("Invalid coupled stomatal resistance");
        // 试探 r_s 大于目标则缩小上界，否则抬高下界。
        if (middle > std::log(target)) upper = middle; else lower = middle;
    }
    return std::exp((lower + upper) / 2);
}
} // namespace facetphysics
