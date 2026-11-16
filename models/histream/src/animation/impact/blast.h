#pragma once
// Free-field airblast parameters of TNT charges from the Kingery-Bulmash (1984) polynomial fits, the
// curves of UFC 3-340-02 Fig. 2-7 (spherical free-air burst) and Fig. 2-15 (hemispherical surface burst).
// SI units throughout: m, kg (TNT equivalent), Pa, Pa*s, s, m/s. Coefficient provenance: blast.cpp.
namespace animation::impact {

constexpr double kTntSpecificEnergyJPerKg = 4.184e6;  // TNT equivalent definition (1 kg TNT := 4.184 MJ)
constexpr double kAmbientPressurePa = 101325.0;       // sea-level ambient used by the shock-speed relation
constexpr double kAmbientSoundSpeedMps = 340.3;       // sea-level sound speed (also used for the far-field ta)

enum class BurstType { FreeAirSpherical, SurfaceHemispherical };

struct BlastParameters {
    double scaledDistance;         // Z = R / W^(1/3) [m/kg^(1/3)] (after clamping to the valid range)
    bool clamped;                  // true when Z was outside the fit range and was clamped
    double incidentPressurePa;     // peak side-on overpressure Ps
    double reflectedPressurePa;    // peak normally reflected overpressure Pr
    double incidentImpulsePaS;     // positive-phase side-on specific impulse is
    double reflectedImpulsePaS;    // positive-phase normally reflected specific impulse ir
    double arrivalTimeS;           // ta (for Z > Zmax: ta(Zmax) + extra distance / kAmbientSoundSpeedMps)
    double positiveDurationS;      // t0 (fit starts at Z = 0.147 / 0.178; smaller Z use the value there)
    double shockFrontVelocityMps;  // U: K-B fit (surface burst); Rankine-Hugoniot from Ps (free-air burst)
};

// Throws std::invalid_argument unless rangeM > 0 and chargeKgTnt > 0 (both finite).
BlastParameters blast(double rangeM, double chargeKgTnt, BurstType type = BurstType::FreeAirSpherical);

// Oblique (regular) reflection at angle of incidence alpha (0 = normal, pi/2 = grazing; |alpha| > pi/2 is
// treated as an unexposed face): X(a) = Xs (1 + cos a - 2 cos^2 a) + Xr0 cos^2 a, i.e. Xr0 at a = 0, Xs at
// grazing, continuous and monotone because K-B always has Xr0 >= 2 Xs. See blast.cpp for source/assumptions.
double reflectedPressureAtIncidence(const BlastParameters& b, double alphaRad);
double reflectedImpulseAtIncidence(const BlastParameters& b, double alphaRad);

double validScaledDistanceMin(BurstType);  // 0.0531 (free air) / 0.0674 (surface) m/kg^(1/3)
double validScaledDistanceMax(BurstType);  // 40 m/kg^(1/3) for both

// Ideal-gas (gamma = 1.4) Rankine-Hugoniot shock speed for a peak overpressure, ambient as above.
double rankineHugoniotShockSpeedMps(double overpressurePa);

}  // namespace animation::impact
