#ifndef GLOBALS_GLSL
#define GLOBALS_GLSL 1

// global constants
#define POROSITY 1 //土壤的孔隙度因子
#define SSA 0.25 // single scattering albedo
#define GST 0.35
#define DENSITY 1

#define PI 3.14159265358979323
#define TWO_PI 6.28318530717958648
#define INFINITY 1e32
#define RngStateType uint // Random type
#define SHORTDISTANCE 0.01

/// type information
#define TYPE_NULL 0
#define TYPE_SOIL 1
#define TYPE_VEGETATION 2
#define TYPE_BUILDING 3
#define TYPE_WATER 4
#define AEROTYPE_VEGETATION 0
#define AEROTYPE_URBAN 1

/// scene object type
#define KIND_CUBE 0

//precision highp int;
precision highp float;

const float M_PI = 3.14159265358979323846;   // pi
const float M_TWO_PI = 6.28318530717958648;      // 2*pi
const float M_PI_2 = 1.57079632679489661923;   // pi/2
const float M_PI_4 = 0.785398163397448309616;  // pi/4
const float M_1_OVER_PI = 0.318309886183790671538;  // 1/pi
const float M_2_OVER_PI = 0.636619772367581343076;  // 2/pi
const float OUTER = 10000.0;
const float InvPI = 0.318309886183790671538;
#define EPSION_1 0.1
#define EPSION_2 0.01
#define EPSION_3 0.001

#define EPSION 0.00000001
#define GLEAF 0.5
#define HSPOT 0.5

#define GROUP_SIZEXY 8
#define GROUP_SIZEX 64
#define GROUP_SIZEXYZ 8

#define PID2 1.557963
#define TLASTNUM 8
#define MAXSTEP 8
#define DIFFUSE_ANGLE_NUM 64
#define BANDNUM 2162

// Equal-solid-angle quadrature. The 64 directions form 32 exact antipodal
// pairs, so neither hemisphere nor any Cartesian axis receives a systematic
// bias. For hemisphere-integrated quantities every direction keeps a fixed
// weight; occluded zero-radiance directions therefore remain part of the
// quadrature instead of being removed from the denominator.
const float DIFFUSE_SPHERE_AVERAGE_WEIGHT = 0.015625; // 1 / 64
const float DIFFUSE_HEMISPHERE_AVERAGE_WEIGHT = 0.03125; // 2 / 64

void compensatedAdd(inout float sum, inout float correction, float value)
{
    float corrected = value - correction;
    float updated = sum + corrected;
    correction = (updated - sum) - corrected;
    sum = updated;
}

float finiteNonnegative(float value)
{
    return (isnan(value) || isinf(value)) ? 0.0 : max(value, 0.0);
}

#define NBAND 2002  // 2001 vnir+1 TIR 
#define RHOA 1.2047      // specific mass of air
#define CP   1004        // specific heat of dry air
#define KAPPA 0.4        // Von Karman constant
#define GEARTH  9.81     // gravity acceleration
#define AHC 119.7117122  // N_A h c in J nm per micromole photons (legacy constant)

// The radiative solver has already integrated each band over wavelength.
// W m^-2 * wavelength_nm / AHC -> umol photons m^-2 s^-1.
float absorbedParPhotonFlux(float absorbedBandWm2, float wavelengthNm)
{
    return absorbedBandWm2 * wavelengthNm / AHC;
}

// Fixed VNIR grid starts at 400 nm with 1 nm spacing. Never group
// the inclusive 700 nm PAR sample with wavelengths outside PAR.
int shortwaveGroupEnd(int firstBand, int width, int bandCount)
{
    int lastBand = min(firstBand + clamp(width, 1, bandCount), bandCount);
    return firstBand < 301 ? min(lastBand, 301) : lastBand;
}
#define MAIR 28.96       // molecular mass of dry air
#define MH20 18          // molecular mass of water
#define MCO2 44          // Molecular mass of carbon dioxide
#define SIGMASB 5.67e-8   // stefan boltzman constant
#define STRESS 1         // 
#define C2K    273.15    // melting point of water
#define KV 0.6396        // 
#define R  8.31           // Molar gas constant
#define LEAFG 0.5
#define CD   0.3         // drag coefficient for the vegetation
#define CR    0.35        // darg for the isolate tree
#define CD1 20.6       //fitting coefficients
#define PSICOR    0.2           // rOUGHNESS LAYER CORRECTION
#define CSSOIL 0.01 // DARY COEFFICIENT FOR SOIL

#define RAD_THRESHOLD 2    // radiance threshold
#define TMIN_THRESHOLD 220  // -53 C
#define TMAX_THRESHOLD 373  // +100 C; sunlit dark roofs exceed 80 C


// const vec3 DIFFUSE_DIRECTIONS[32] = {
//     vec3(0.898904, 0.435512, 0.0479745),
//     vec3(0.898904, -0.435512, -0.0479745),
//     vec3(0.898904, 0.0479745, -0.435512),
//     vec3(0.898904, -0.0479745, 0.435512),
//     vec3(-0.898904, 0.435512, -0.0479745),
//     vec3(-0.898904, -0.435512, 0.0479745),
//     vec3(-0.898904, 0.0479745, 0.435512),
//     vec3(-0.898904, -0.0479745, -0.435512),
//     vec3(0.0479745, 0.898904, 0.435512),
//     vec3(-0.0479745, 0.898904, -0.435512),
//     vec3(-0.435512, 0.898904, 0.0479745),
//     vec3(0.435512, 0.898904, -0.0479745),
//     vec3(-0.0479745, -0.898904, 0.435512),
//     vec3(0.0479745, -0.898904, -0.435512),
//     vec3(0.435512, -0.898904, 0.0479745),
//     vec3(-0.435512, -0.898904, -0.0479745),
//     vec3(0.435512, 0.0479745, 0.898904),
//     vec3(-0.435512, -0.0479745, 0.898904),
//     vec3(0.0479745, -0.435512, 0.898904),
//     vec3(-0.0479745, 0.435512, 0.898904),
//     vec3(0.435512, -0.0479745, -0.898904),
//     vec3(-0.435512, 0.0479745, -0.898904),
//     vec3(0.0479745, 0.435512, -0.898904),
//     vec3(-0.0479745, -0.435512, -0.898904),
//     vec3(0.57735, 0.57735, 0.57735),
//     vec3(0.57735, 0.57735, -0.57735),
//     vec3(0.57735, -0.57735, 0.57735),
//     vec3(0.57735, -0.57735, -0.57735),
//     vec3(-0.57735, 0.57735, 0.57735),
//     vec3(-0.57735, 0.57735, -0.57735),
//     vec3(-0.57735, -0.57735, 0.57735),
//     vec3(-0.57735, -0.57735, -0.57735)
// };

const vec3 DIFFUSE_DIRECTIONS[64] = {
    vec3(0.999877922, 0.015625000, 0.000000000),
    vec3(-0.999877922, -0.015625000, 0.000000000),
    vec3(-0.736558335, 0.046875000, 0.674747770),
    vec3(0.736558335, -0.046875000, -0.674747770),
    vec3(0.087158514, 0.078125000, -0.993126315),
    vec3(-0.087158514, -0.078125000, 0.993126315),
    vec3(0.604788567, 0.109375000, 0.788839590),
    vec3(-0.604788567, -0.109375000, -0.788839590),
    vec3(-0.974928320, 0.140625000, -0.172451092),
    vec3(0.974928320, -0.140625000, 0.172451092),
    vec3(0.831199175, 0.171875000, -0.528740877),
    vec3(-0.831199175, -0.171875000, 0.528740877),
    vec3(-0.254192286, 0.203125000, 0.945582633),
    vec3(0.254192286, -0.203125000, -0.945582633),
    vec3(-0.448069042, 0.234375000, -0.862729675),
    vec3(0.448069042, -0.234375000, 0.862729675),
    vec3(0.905577520, 0.265625000, 0.330715458),
    vec3(-0.905577520, -0.265625000, -0.330715458),
    vec3(-0.882672684, 0.296875000, 0.364354453),
    vec3(0.882672684, -0.296875000, -0.364354453),
    vec3(0.400379470, 0.328125000, -0.855587672),
    vec3(-0.400379470, -0.328125000, 0.855587672),
    vec3(0.279289677, 0.359375000, 0.890419500),
    vec3(-0.279289677, -0.359375000, -0.890419500),
    vec3(-0.796470088, 0.390625000, -0.461570696),
    vec3(0.796470088, -0.390625000, 0.461570696),
    vec3(0.885507002, 0.421875000, -0.194676230),
    vec3(-0.885507002, -0.421875000, 0.194676230),
    vec3(-0.512697398, 0.453125000, 0.729259290),
    vec3(0.512697398, -0.453125000, -0.729259290),
    vec3(-0.112428924, 0.484375000, -0.867606245),
    vec3(0.112428924, -0.484375000, 0.867606245),
    vec3(0.655162245, 0.515625000, 0.552171434),
    vec3(-0.655162245, -0.515625000, -0.552171434),
    vec3(-0.836499335, 0.546875000, 0.034591874),
    vec3(0.836499335, -0.546875000, -0.034591874),
    vec3(0.578368093, 0.578125000, -0.575553502),
    vec3(-0.578368093, -0.578125000, 0.575553502),
    vec3(-0.036624373, 0.609375000, 0.792035835),
    vec3(0.036624373, -0.609375000, -0.792035835),
    vec3(-0.491971015, 0.640625000, -0.589545698),
    vec3(0.491971015, -0.640625000, 0.589545698),
    vec3(0.734049986, 0.671875000, 0.098765387),
    vec3(-0.734049986, -0.671875000, -0.098765387),
    vec3(-0.583684672, 0.703125000, 0.406112592),
    vec3(0.583684672, -0.703125000, -0.406112592),
    vec3(0.148971653, 0.734375000, -0.662193934),
    vec3(-0.148971653, -0.734375000, 0.662193934),
    vec3(0.319830069, 0.765625000, 0.558146116),
    vec3(-0.319830069, -0.765625000, -0.558146116),
    vec3(-0.575563831, 0.796875000, -0.183620561),
    vec3(0.575563831, -0.796875000, 0.183620561),
    vec3(0.508856394, 0.828125000, -0.235104561),
    vec3(-0.508856394, -0.828125000, 0.235104561),
    vec3(-0.197410744, 0.859375000, 0.471702881),
    vec3(0.197410744, -0.859375000, -0.471702881),
    vec3(-0.153907284, 0.890625000, -0.427901457),
    vec3(0.153907284, -0.890625000, 0.427901457),
    vec3(0.342999753, 0.921875000, 0.180271057),
    vec3(-0.342999753, -0.921875000, -0.180271057),
    vec3(-0.292582713, 0.953125000, 0.077123864),
    vec3(0.292582713, -0.953125000, -0.077123864),
    vec3(0.095233309, 0.984375000, -0.148109676),
    vec3(-0.095233309, -0.984375000, 0.148109676)
};

#endif
