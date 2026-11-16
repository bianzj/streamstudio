#pragma once
// Miscellaneous public empirical/physical formulas for the impact model. SI units.
namespace animation::impact {

// Adiabatic-wall (recovery) temperature of a body moving at speedMps through still air:
//   Tr = T_inf (1 + r (gamma - 1)/2 M^2),  M = V / sqrt(gamma R T_inf).
// r = 0.89 ~ Pr^(1/3) is the usual turbulent-boundary-layer recovery factor (laminar: Pr^(1/2) ~ 0.85).
double recoveryTemperatureK(double ambientK, double speedMps, double recoveryFactor = 0.89, double gamma = 1.4,
                            double gasConstant = 287.05);

struct Fireball { double diameterM, durationS; };
// D = 3.86 W^0.32 [m], t = 0.299 W^0.32 [s] (W in kg): "Fireball characterization (Baker)", IATG 01.80:2021
// Table 34, from Baker et al., Explosion Hazards and Evaluation (1983). Derived for propellant fireballs; used
// here for TNT-equivalent mass because no accessible high-explosive-specific relation was found (see .cpp).
Fireball fireball(double chargeKgTnt);

// Planck black-body spectral radiance B_lambda(lambda, T) [W m^-2 sr^-1 m^-1]; 0 for T <= 0.
double planckSpectralRadiance(double wavelengthM, double temperatureK);

}  // namespace animation::impact
