#include "animation/impact/penetration.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

// C.W. Young, "Penetration Equations", SAND97-2426 (1997), section 3.1 and appendix A.
// The report states the soil/rock/concrete equations in US units (D [ft], W [lb], A [in^2], V [ft/s]):
//   V <  200 ft/s:  D = 0.3     S N (W/A)^0.7 ln(1 + 2e-5 V^2)      (eq. 3.1)
//   V >= 200 ft/s:  D = 0.00178 S N (W/A)^0.7 (V - 100)             (eq. 3.2)
// and multiplies the right side by K_h for light penetrators in rock/concrete (eq. 3.4). They are evaluated
// here with exact unit conversion. Appendix A restates them in SI as
//   D = 0.0008 S N (m/A)^0.7 ln(1 + 2.15e-4 V^2)  (A-3.1),  D = 0.000018 S N (m/A)^0.7 (V - 30.5)  (A-3.2).
// A-3.2 equals the exact conversion of eq. 3.2 (1.809e-5, 30.48) within 0.5 %, but the exact conversion of the
// eq. 3.1 constant is 9.29e-4, not 0.0008: the printed A-3.1 is 13.9 % low and would leave a 14 % step at
// 61 m/s, whereas eqs. 3.1/3.2 meet within 0.9 % at 200 ft/s. Hence the US-unit equations are the reference.
// Light-penetrator factor: K_h = 0.46 m^0.15 for m < 182 kg (A-3.4; the report prints "m >= 182 kg", a typo:
// eq. 3.4 reads K_h = 0.4 W^0.15 for W < 400 lb, i.e. 0.450 m^0.15). It is capped at 1 so that it is continuous
// (0.46 m^0.15 reaches 1 at 177 kg). The soil factor K_s = 0.27 m^0.4 (m < 27 kg, A-3.3) does not apply here.
// Accuracy quoted by the report (section 5): about 10 % for the equations, 15-20 % including the concrete S.
namespace animation::impact {
namespace {
constexpr double kFt = 0.3048, kLb = 0.45359237, kSquareInch = 0.00064516, kPi = 3.14159265358979323846;

double hardTargetMassFactor(double massKg) { return massKg < 182.0 ? std::min(1.0, 0.46 * std::pow(massKg, 0.15)) : 1.0; }

bool positiveFinite(double v) { return v > 0 && std::isfinite(v); }
}  // namespace

PenetrationResult youngConcrete(const PenetrationInput& in) {
    if (!positiveFinite(in.massKg) || !positiveFinite(in.diameterM) || !(in.speedMps >= 0) ||
        !std::isfinite(in.speedMps) || !positiveFinite(in.sNumber) || !positiveFinite(in.noseCoefficientN))
        throw std::invalid_argument("youngConcrete: mass, diameter, S and N must be positive, speed >= 0");
    PenetrationResult r{};
    r.massScaleFactorKs = hardTargetMassFactor(in.massKg);
    const double areaSquareInch = kPi * in.diameterM * in.diameterM / 4 / kSquareInch;
    const double weightOverArea = in.massKg / kLb / areaSquareInch;  // lb/in^2
    const double q = in.sNumber * in.noseCoefficientN * std::pow(weightOverArea, 0.7) * r.massScaleFactorKs;
    const double v = in.speedMps / kFt;  // ft/s
    const double depthFt = v < 200 ? 0.3 * q * std::log(1 + 2e-5 * v * v) : 0.00178 * q * (v - 100);
    r.depthM = depthFt * kFt;
    const double t = in.targetThicknessM;
    r.perforated = t > 0 && std::isfinite(t) && r.depthM >= t;
    if (r.perforated) {
        // Energy estimate with constant resisting force over the slab, eq. 6.7 at normal incidence; Young's
        // thick-concrete form (eq. 6.8) would additionally credit Ln/K3 for cratering/spalling.
        r.residualSpeedMps = in.speedMps * std::sqrt(std::max(0.0, 1 - t / r.depthM));
        r.durationS = 2 * t / (in.speedMps + r.residualSpeedMps);
    } else {
        r.residualSpeedMps = 0;
        r.durationS = in.speedMps > 0 ? 2 * r.depthM / in.speedMps : 0;
    }
    r.outsideValidity = in.massKg < 5 || in.speedMps > 4000 * kFt || r.depthM < 3 * in.diameterM;
    return r;
}

double ogiveNoseCoefficient(double caliberRadiusHead) {
    if (!(caliberRadiusHead >= 0.25) || !std::isfinite(caliberRadiusHead))
        throw std::invalid_argument("ogiveNoseCoefficient: CRH must be >= 0.25");
    return 0.18 * std::sqrt(caliberRadiusHead - 0.25) + 0.56;
}

double conicNoseCoefficient(double noseLengthCalibers) {
    if (!(noseLengthCalibers >= 0) || !std::isfinite(noseLengthCalibers))
        throw std::invalid_argument("conicNoseCoefficient: nose length must be >= 0");
    return 0.25 * noseLengthCalibers + 0.56;
}

double youngConcreteSNumber(double compressiveStrengthMPa, double rebarVolumePercent, double cureYears,
                            double thicknessCalibers, double widthCalibers, bool reinforced) {
    if (!positiveFinite(compressiveStrengthMPa) || !(rebarVolumePercent >= 0) || !(rebarVolumePercent < 11) ||
        !positiveFinite(cureYears) || !positiveFinite(thicknessCalibers) || !positiveFinite(widthCalibers))
        throw std::invalid_argument("youngConcreteSNumber: invalid concrete parameters");
    const double tc = std::min(cureYears, 1.0), tCal = std::min(thicknessCalibers, 6.0);
    double f = reinforced ? 20.0 : 30.0;
    if (tCal >= 0.5 && tCal <= 2.0) f *= 0.5;  // thin targets: F reduced by 50 %
    const double ke = widthCalibers < f ? std::pow(f / widthCalibers, 0.3) : 1.0;
    return 0.085 * ke * (11 - rebarVolumePercent) * std::pow(tc * tCal, -0.06) * std::pow(35.0 / compressiveStrengthMPa, 0.3);
}

}  // namespace animation::impact
