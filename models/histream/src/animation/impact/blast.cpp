#include "animation/impact/blast.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

// Kingery-Bulmash airblast fits (metric).
// Primary source: C.N. Kingery, G. Bulmash, "Airblast Parameters from TNT Spherical Air Burst and Hemispherical
//   Surface Burst", ARBRL-TR-02555, Ballistic Research Laboratory, April 1984 (DTIC ADA148978); the same curves
//   are UFC 3-340-02 (2008) Fig. 2-7 (free air) and Fig. 2-15 (surface burst).
// Form:  U = K0 + K1*log10(Z),  log10(Y) = sum_i C_i U^i,  Z in m/kg^(1/3);
//   Y in kPa (Ps, Pr), kPa*ms/kg^(1/3) (is, ir), ms/kg^(1/3) (ta, t0), km/s (U).
// The original report was not reachable, so the coefficients below were transcribed from three published
// copies and every disagreement between them was resolved numerically:
//  [IATG15] UNODA, IATG 01.80:2015 "Formulae for ammunition management", 2nd ed. Amd 1, Tables 9-13A
//           https://s3.amazonaws.com/unoda-web/wp-content/uploads/2019/05/IATG-01.80-Formulae-for-Ammunition-Management-V.2.01.pdf
//  [LIRA]   LIRA 10.16 user manual, ch. 14 "Dynamics+", sections 14.1.2.2 (surface) and 14.1.2.3 (free air):
//           complete sets incl. ta and t0 (http://lira10.com/Content/Instructions/10.16/, chapter 14 PDF)
//  [UNSG]   UN SaferGuard Kingery-Bulmash calculator (surface burst incl. shock velocity), page source of
//           https://unsaferguard.org/un-saferguard/kingery-bulmash
// Resolved transcription errors (wrong value -> value used, and the evidence):
//  * free-air Pr C2: [LIRA] 0.35119031446 -> 0.035119031446 [IATG15] (Pr/Ps then equals the ideal-gas
//    Rankine-Hugoniot reflection factor within 1% for Z >= 2; the [LIRA] value gives Pr/Ps = 32 at Z = 40).
//  * free-air is (Z > 0.792): [IATG15] K1 = 0.306.., 8 terms, C6 < 0 -> [LIRA] 9-term set (continuous with the
//    near-field segment within 0.4%, matches the JRC example below within 0.2%).
//  * free-air ir K0: [IATG15] +0.2040.. -> -0.204004553231 [LIRA] (only this sign satisfies the surface-burst =
//    1.8 x free-air charge relation that holds within 2% for is, ir and t0).
//  * surface t0 (1.01 < Z < 2.78) K0: [UNSG] +2.1249.. -> -2.12492525216 [LIRA]; with it the segment is the
//    free-air segment shifted by exactly 1.8^(1/3) in Z and time, and IATG 01.80:2021 Table 5 is reproduced.
//  * surface Ps / Pr: [IATG15] sign errors and missing U^9..U^11 terms -> [UNSG] = [LIRA] sets.
// Validation (see tools/scene-animation/tests/impact_fixture.cpp): IATG 01.80:2021 Tables 3 and 5 (surface,
// <= 0.7%), M.M. Swisdak, "Simplified Kingery Airblast Calculations", 26th DoD Explosives Safety Seminar 1994
// (DTIC ADA526744) independent surface-burst fits (<= 1%), Karlos & Solomos, JRC EUR 26456 EN (2013) worked
// examples read from K-B curves (free air and surface, <= 1%).
namespace animation::impact {
namespace {
struct Fit { double zMax, k0, k1; std::vector<double> c; };  // one segment, valid up to zMax
using Curve = std::vector<Fit>;
struct BurstFits { double zMin, t0ZMin; Curve ps, is, pr, ir, ta, t0, u; };

const BurstFits kFreeAir{0.0531, 0.147,
    {{40, -0.214362789151, 1.35034249993, {2.611368669, -1.69012801396, 0.00804973591951, 0.336743114941,
        -0.00516226351334, -0.0809228619888, -0.00478507266747, 0.00793030472242, 0.0007684469735}}},
    {{0.792, 2.34723921354, 3.24299066475, {2.38830516757, -0.443749377691, 0.168825414684, 0.0348138030308,
        -0.010435192824}},
     {40, -1.75305660315, 2.30629231803, {1.55197227115, -0.40463292088, -0.0142721946082, 0.00912366316617,
        -0.0006750681404, -0.00800863718901, 0.00314819515931, 0.00152044783382, -0.0007470265899}}},
    {{40, -0.214362789151, 1.35034249993, {3.22958031387, -2.21400538997, 0.035119031446, 0.657599992109,
        0.0141818951887, -0.243076636231, -0.0158699803158, 0.0492741184234, 0.00227639644004,
        -0.00397126276058}}},
    {{40, -0.204004553231, 1.37882996018, {2.55875660396, -0.903118886091, 0.101771877942, -0.0242139751146}}},
    {{40, -0.253273111999, 1.37407043777, {0.0720707787637, 1.36456871214, -0.0570035692784, -0.182832224796,
        0.0118851436014, 0.0432648687627, -0.0007997367834, -0.00436073555033}}},
    {{0.888, 2.26367268496, 5.11588554305, {-0.686608550419, 0.164953518069, 0.127788499497, 0.00291430135946,
        0.00187957449227, 0.0173413962543, 0.00269739758043, -0.00361976502798, -0.00100926577934}},
     {2.28, -1.33361206714, 9.2996288611, {0.23031841078, -0.0297944268969, 0.0306329542941, 0.0183405574074,
        -0.0173964666286, -0.00106321963576, 0.0056206003128, 0.0001618217499, -0.0006860188944}},
     {40, -3.13005805346, 3.1524725364, {0.621036276475, 0.0967031995552, -0.00801302059667, 0.00482705779732,
        0.00187587272287, -0.00246738509321, -0.000841116665, 0.0006193291052}}},
    {}};  // no K-B shock-velocity fit transcribed for free air: U from Rankine-Hugoniot

const BurstFits kSurface{0.0674, 0.178,
    {{40, -0.214362789151, 1.35034249993, {2.78076916577, -1.6958988741, -0.154159376846, 0.514060730593,
        0.0988534365274, -0.293912623038, -0.0268112345019, 0.109097496421, 0.00162846756311, -0.0214631030242,
        0.0001456723382, 0.00167847752266}}},
    {{0.955, 2.06761908721, 3.0760329666, {2.52455620925, -0.502992763686, 0.171335645235, 0.0450176963051,
        -0.0118964626402}},
     {40, -1.94708846747, 2.40697745406, {1.67281645863, -0.384519026965, -0.0260816706301, 0.00595798753822,
        0.014544526107, -0.00663289334734, -0.00284189327204, 0.0013644816227}}},
    {{40, -0.240657322658, 1.36637719229, {3.40283217581, -2.21030870597, -0.218536586295, 0.895319589372,
        0.24989009775, -0.569249436807, -0.11791682383, 0.224131161411, 0.0245620259375, -0.0455116002694,
        -0.00190930738887, 0.00361471193389}}},
    {{40, -0.246208804814, 1.33422049854, {2.70588058103, -0.949516092853, 0.112136118689, -0.0250659183287}}},
    {{40, -0.202425716178, 1.37784223635, {-0.0591634288046, 1.35706496258, 0.052492798645, -0.196563954086,
        -0.0601770052288, 0.0696360270891, 0.0215297490092, -0.0161658930785, -0.00232531970294,
        0.00147752067524}}},
    {{1.01, 1.92946154068, 5.25099193925, {-0.614227603559, 0.130143717675, 0.134872511954, 0.0391574276906,
        -0.00475933664702, -0.00428144598008}},
     {2.78, -2.12492525216, 9.2996288611, {0.315409245784, -0.0297944268976, 0.030632954288, 0.0183405574086,
        -0.0173964666211, -0.00106321963633, 0.00562060030977, 0.0001618217499, -0.0006860188944}},
     {40, -3.53626218091, 3.46349745571, {0.686906642409, 0.0933035304009, -0.0005849420883, -0.00226884995013,
        -0.00295908591505, 0.00148029868929}}},
    {{40, -0.202425716178, 1.37784223635, {-0.06621072854, -0.698029762594, 0.158916781906, 0.443812098136,
        -0.113402023921, -0.369887075049, 0.129230567449, 0.19857981197, -0.0867636217397, -0.0620391900135,
        0.0307482926566, 0.0102657234407, -0.00546533250772, -0.000693180974, 0.0003847494916}}}};

constexpr double kZMax = 40.0;
// Adjacent K-B segments differ by up to 1.8% at their nominal joins (surface t0 at Z = 1.01); log10(Y) is
// blended linearly over +-4% in ln Z around every join so all outputs are continuous in Z.
constexpr double kJoinBlend = 0.04;

double log10Value(const Fit& f, double z) {
    const double u = f.k0 + f.k1 * std::log10(z);
    double y = 0;
    for (auto c = f.c.rbegin(); c != f.c.rend(); ++c) y = y * u + *c;  // Horner
    return y;
}

double evaluate(const Curve& curve, double z) {
    for (size_t i = 0; i + 1 < curve.size(); ++i) {
        const double s = std::log(z / curve[i].zMax);
        if (s < -kJoinBlend) return std::pow(10.0, log10Value(curve[i], z));
        if (s <= kJoinBlend) {
            const double w = (s + kJoinBlend) / (2 * kJoinBlend);
            return std::pow(10.0, (1 - w) * log10Value(curve[i], z) + w * log10Value(curve[i + 1], z));
        }
    }
    return std::pow(10.0, log10Value(curve.back(), z));
}

const BurstFits& fits(BurstType type) { return type == BurstType::FreeAirSpherical ? kFreeAir : kSurface; }

// Normal/oblique blend, after the ConWep-style interpolation used for engineering blast loading:
//   X(a) = Xs (1 + cos a - 2 cos^2 a) + Xr0 cos^2 a  (the same expression is given in the LIRA 10.16 manual,
// section 14.1.2.1, and is the angle blend of LS-DYNA *LOAD_BLAST after Randers-Pehrson & Bannister 1997,
// ARL-TR-1310). Assumption: it ignores the Mach-reflection enhancement near the critical angle (~40 deg,
// UFC 3-340-02 Fig. 2-193) and is applied unchanged to the impulse.
double obliqueBlend(double incident, double normal, double alphaRad) {
    const double a = std::abs(alphaRad);
    const double c = a >= 1.5707963267948966 ? 0.0 : std::cos(a);
    return incident * (1 + c - 2 * c * c) + normal * c * c;
}
}  // namespace

double validScaledDistanceMin(BurstType type) { return fits(type).zMin; }
double validScaledDistanceMax(BurstType) { return kZMax; }

double rankineHugoniotShockSpeedMps(double overpressurePa) {
    // U = c0 sqrt(1 + (gamma+1)/(2 gamma) * Ps/P0), gamma = 1.4 (normal-shock relations, e.g. Kinney & Graham).
    return kAmbientSoundSpeedMps * std::sqrt(1.0 + 6.0 / 7.0 * std::max(0.0, overpressurePa) / kAmbientPressurePa);
}

BlastParameters blast(double rangeM, double chargeKgTnt, BurstType type) {
    if (!(rangeM > 0) || !(chargeKgTnt > 0) || !std::isfinite(rangeM) || !std::isfinite(chargeKgTnt))
        throw std::invalid_argument("blast: range and charge mass must be positive and finite");
    const BurstFits& f = fits(type);
    const double cube = std::cbrt(chargeKgTnt);  // W^(1/3): scaled impulse/time -> absolute
    const double zRaw = rangeM / cube;
    const double z = std::clamp(zRaw, f.zMin, kZMax);
    BlastParameters b{};
    b.scaledDistance = z;
    b.clamped = z != zRaw;
    b.incidentPressurePa = 1e3 * evaluate(f.ps, z);      // kPa -> Pa
    b.reflectedPressurePa = 1e3 * evaluate(f.pr, z);
    b.incidentImpulsePaS = evaluate(f.is, z) * cube;    // kPa*ms == Pa*s
    b.reflectedImpulsePaS = evaluate(f.ir, z) * cube;
    b.arrivalTimeS = 1e-3 * evaluate(f.ta, z) * cube;   // ms -> s
    if (zRaw > kZMax) b.arrivalTimeS += (zRaw - kZMax) * cube / kAmbientSoundSpeedMps;  // acoustic far field
    b.positiveDurationS = 1e-3 * evaluate(f.t0, std::max(z, f.t0ZMin)) * cube;
    b.shockFrontVelocityMps = f.u.empty() ? rankineHugoniotShockSpeedMps(b.incidentPressurePa)
                                          : 1e3 * evaluate(f.u, z);  // km/s -> m/s
    return b;
}

double reflectedPressureAtIncidence(const BlastParameters& b, double alphaRad) {
    return obliqueBlend(b.incidentPressurePa, b.reflectedPressurePa, alphaRad);
}

double reflectedImpulseAtIncidence(const BlastParameters& b, double alphaRad) {
    return obliqueBlend(b.incidentImpulsePaS, b.reflectedImpulsePaS, alphaRad);
}

}  // namespace animation::impact
