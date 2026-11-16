#pragma once
// Penetration of concrete by rigid projectiles: C.W. Young, "Penetration Equations", SAND97-2426, Sandia
// National Laboratories, October 1997 (OSTI 562498, https://www.osti.gov/biblio/562498). SI units.
namespace animation::impact {

struct PenetrationInput {
    double massKg;            // penetrator mass m
    double diameterM;         // body diameter d (A = pi d^2 / 4)
    double speedMps;          // impact speed V (normal incidence)
    double sNumber;           // Young penetrability index S (concrete: youngConcreteSNumber(), default 0.9)
    double noseCoefficientN;  // nose performance coefficient N (ogiveNoseCoefficient / conicNoseCoefficient)
    double targetThicknessM;  // slab thickness T; <= 0 or +inf = semi-infinite target
};

struct PenetrationResult {
    double depthM;             // penetration path length D into a semi-infinite target
    bool perforated;           // D >= T
    double residualSpeedMps;   // exit speed estimate V sqrt(1 - T/D) (Young eq. 6.7), 0 if not perforated
    double durationS;          // constant deceleration: 2D/V, or 2T/(V + Vexit) when perforating
    double massScaleFactorKs;  // Young's light-penetrator factor for hard targets (K_h, eq. 3.4 / A-3.4)
    bool outsideValidity = false;  // m < 5 kg, V > 1219 m/s (4000 fps) or D < 3 calibres (report section 3)
};

PenetrationResult youngConcrete(const PenetrationInput&);  // C.W. Young, SAND97-2426 (OSTI 562498), SI form

// Young's N for a tangent ogive from its calibre-radius-head (eq. 3.8): N = 0.18 sqrt(CRH - 0.25) + 0.56,
// identical to eq. 3.7 N = 0.18 Ln/d + 0.56 because a tangent ogive has Ln/d = sqrt(CRH - 0.25).
double ogiveNoseCoefficient(double caliberRadiusHead);
// Conic nose (eq. 3.9): N = 0.25 Ln/d + 0.56, Ln/d = nose length in calibres. Flat nose: N = 0.56.
double conicNoseCoefficient(double noseLengthCalibers);

constexpr double kYoungDefaultConcreteS = 0.9;  // report section 4.2: default when concrete data are missing
// Concrete S-number, eq. A-4.2/4.3: S = 0.085 Ke (11 - P) (tc Tc)^-0.06 (35/fc)^0.3 with fc [MPa],
// P = volumetric rebar percent, tc = cure time [years] (<= 1), Tc = thickness in calibres (capped at 6),
// Ke = (F/W1)^0.3 (1 if W1 >= F), F = 20 reinforced / 30 plain, halved when 0.5 <= Tc <= 2, W1 = width [calibres].
double youngConcreteSNumber(double compressiveStrengthMPa, double rebarVolumePercent, double cureYears,
                            double thicknessCalibers, double widthCalibers, bool reinforced);

}  // namespace animation::impact
