#include "animation/impact/empirical.h"

#include <cmath>
#include <stdexcept>

namespace animation::impact {

double recoveryTemperatureK(double ambientK, double speedMps, double recoveryFactor, double gamma, double gasConstant) {
    if (!(ambientK > 0) || !(gamma > 1) || !(gasConstant > 0) || !std::isfinite(speedMps))
        throw std::invalid_argument("recoveryTemperatureK: invalid gas state");
    const double mach2 = speedMps * speedMps / (gamma * gasConstant * ambientK);
    return ambientK * (1 + recoveryFactor * (gamma - 1) / 2 * mach2);
}

// Source: UNODA, IATG 01.80:2021 (3rd ed.) "Formulae for ammunition management", clause 5.7, Table 34
// "Fireball characterization (Baker)": d = 3.86 Q^0.32, t = 0.299 Q^0.32, Q = burning mass [kg]
// (http://data.unsaferguard.org/iatg/en/IATG-01.80-Formulae-ammunition-management-IATG-V.3.pdf), citing
// W.E. Baker et al., Explosion Hazards and Evaluation, Elsevier 1983. The correlation was fitted to propellant
// fireballs (IATG adds a burning-temperature correction (Tx/3600 K)^(1/3) for d, not applied here, i.e. the
// 3600 K reference temperature is assumed). A detonation fireball of condensed HE is of similar size but its
// luminous phase is usually shorter, so durationS should be read as an upper estimate.
Fireball fireball(double chargeKgTnt) {
    if (!(chargeKgTnt > 0) || !std::isfinite(chargeKgTnt)) throw std::invalid_argument("fireball: charge must be > 0");
    const double s = std::pow(chargeKgTnt, 0.32);
    return {3.86 * s, 0.299 * s};
}

double planckSpectralRadiance(double wavelengthM, double temperatureK) {
    if (!(wavelengthM > 0) || !std::isfinite(wavelengthM)) throw std::invalid_argument("planck: wavelength must be > 0");
    if (!(temperatureK > 0)) return 0.0;
    constexpr double h = 6.62607015e-34, c = 299792458.0, k = 1.380649e-23;  // SI 2019 exact constants
    const double x = h * c / (wavelengthM * k * temperatureK);
    const double l5 = wavelengthM * wavelengthM * wavelengthM * wavelengthM * wavelengthM;
    return 2 * h * c * c / l5 / std::expm1(x);  // expm1 -> inf for huge x gives 0
}

}  // namespace animation::impact
