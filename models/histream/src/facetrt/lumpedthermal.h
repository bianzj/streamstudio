#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace facetphysics {

// An optional generic internal heat reservoir. Surface conduction is paired
// with the exact opposite power in the surface energy equation.
struct LumpedThermalParameters {
    double capacityJPerK{}, conductanceWPerK{}, airConductanceWPerK{}, powerW{};
    double startSeconds{}, stopSeconds{}, initialK{298.15};
    void validate() const {
        for (double x : {capacityJPerK,conductanceWPerK,airConductanceWPerK,powerW,
                         startSeconds,stopSeconds,initialK})
            if (!std::isfinite(x)) throw std::runtime_error("Nonfinite lumped thermal parameter");
        if (!(capacityJPerK>0 && conductanceWPerK>0 && airConductanceWPerK>=0 && powerW>=0 &&
              startSeconds>=0 && stopSeconds>=startSeconds && initialK>=150 && initialK<=1500))
            throw std::runtime_error("Invalid lumped thermal parameters");
    }
};

struct LumpedThermalStep {
    double temperatureK{},powerW{},conductedW{},airLossW{},storageW{},residualW{};
};

inline double averageLumpedPower(const LumpedThermalParameters& p,double previousSeconds,double dt) {
    p.validate();
    if (!(dt>0 && previousSeconds>=0) || !std::isfinite(dt+previousSeconds))
        throw std::runtime_error("Invalid lumped thermal time interval");
    const double overlap=std::max(0.,std::min(previousSeconds+dt,p.stopSeconds)-
                                     std::max(previousSeconds,p.startSeconds));
    return p.powerW*overlap/dt;
}

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
