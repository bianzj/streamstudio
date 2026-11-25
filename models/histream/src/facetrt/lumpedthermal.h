// ==========================================================================
// 文件：lumpedthermal.h（facetrt）
// 功能：FacetEB 的“集总热体”（内部热库）模型：一个有热容、与表皮和空气相连的单节点热库，
//   可在给定时间窗内被加热。表皮传导与热库传导在表面能量方程中成对出现（等值反号）。
// 主要模块：
//   1. LumpedThermalParameters：热库参数与合法性检查（validate）
//   2. LumpedThermalStep：一个时间步的结果（温度与各功率项）
//   3. averageLumpedPower：时间步内按加热窗重叠比例求平均加热功率
//   4. solveLumpedCore：向后欧拉（隐式）求热库温度，并给出能量闭合残差
// 数据流：facetrtcore.cpp 的 runFacetEBCore 对每个集总体取其表皮面元的面积×权重平均温度，
//   调用 solveLumpedCore；返回的核心温度再反馈为面元的内部传热项。
// 注意：单位为 J K-1、W K-1、W、s、K；时间 previousSeconds 为本时间步开始时已过去的秒数。
// ==========================================================================

#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace facetphysics {

// An optional generic internal heat reservoir. Surface conduction is paired
// with the exact opposite power in the surface energy equation.
// 模块：热库参数。capacityJPerK 热容（J K-1，须 > 0）；conductanceWPerK 与表皮的总导热（W K-1，须 > 0）；
// airConductanceWPerK 与空气的导热（W K-1，>= 0）；powerW 加热功率（W，>= 0）；
// startSeconds / stopSeconds 加热窗口（s，0 <= start <= stop）；initialK 初始温度（K，限 150–1500）。
struct LumpedThermalParameters {
    double capacityJPerK{}, conductanceWPerK{}, airConductanceWPerK{}, powerW{};
    double startSeconds{}, stopSeconds{}, initialK{298.15};
    // 作用：检查全部参数有限且在合法范围内，否则抛出 std::runtime_error。
    void validate() const {
        for (double x : {capacityJPerK,conductanceWPerK,airConductanceWPerK,powerW,
                         startSeconds,stopSeconds,initialK})
            if (!std::isfinite(x)) throw std::runtime_error("Nonfinite lumped thermal parameter");
        if (!(capacityJPerK>0 && conductanceWPerK>0 && airConductanceWPerK>=0 && powerW>=0 &&
              startSeconds>=0 && stopSeconds>=startSeconds && initialK>=150 && initialK<=1500))
            throw std::runtime_error("Invalid lumped thermal parameters");
    }
};

// 模块：一步结果。temperatureK 热库温度；powerW 本步平均加热功率；conductedW 传向表皮的功率；
// airLossW 向空气散失的功率；storageW 储热变化率；residualW = power - conducted - airLoss - storage（应为 0）。
struct LumpedThermalStep {
    double temperatureK{},powerW{},conductedW{},airLossW{},storageW{},residualW{};
};

// 作用：时间步 [previousSeconds, previousSeconds+dt] 与加热窗 [start, stop] 的重叠时间占 dt 的比例
//  × powerW，即该步的平均加热功率（W）。dt 须 > 0，previousSeconds 须 >= 0。
inline double averageLumpedPower(const LumpedThermalParameters& p,double previousSeconds,double dt) {
    p.validate();
    if (!(dt>0 && previousSeconds>=0) || !std::isfinite(dt+previousSeconds))
        throw std::runtime_error("Invalid lumped thermal time interval");
    const double overlap=std::max(0.,std::min(previousSeconds+dt,p.stopSeconds)-
                                     std::max(previousSeconds,p.startSeconds));
    return p.powerW*overlap/dt;
}

// 作用：向后欧拉求热库温度。输入：上一步温度 previousK、表皮加权平均温度 weightedSkinK、气温 airK（均为 K，须 > 0）、
// 步长 dt（s）、本步起始时刻 previousSeconds（s）。
// 方程：C/dt·(T - Tprev) = P - G(T - Tskin) - Ga(T - Tair)，解出 T；返回各功率项与闭合残差。
inline LumpedThermalStep solveLumpedCore(const LumpedThermalParameters& p,double previousK,
                                        double weightedSkinK,double airK,double dt,double previousSeconds) {
    const double power=averageLumpedPower(p,previousSeconds,dt);
    if (!std::isfinite(previousK+weightedSkinK+airK) || std::min({previousK,weightedSkinK,airK})<=0)
        throw std::runtime_error("Invalid lumped thermal temperature");
    const double storage=p.capacityJPerK/dt;
    LumpedThermalStep r;
    r.temperatureK=(storage*previousK+power+p.conductanceWPerK*weightedSkinK+
                    p.airConductanceWPerK*airK)/(storage+p.conductanceWPerK+p.airConductanceWPerK);
    r.powerW=power;
    r.conductedW=p.conductanceWPerK*(r.temperatureK-weightedSkinK);
    r.airLossW=p.airConductanceWPerK*(r.temperatureK-airK);
    r.storageW=storage*(r.temperatureK-previousK);
    r.residualW=power-r.conductedW-r.airLossW-r.storageW;
    return r;
}
}
