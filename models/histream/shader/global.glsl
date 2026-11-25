// ==========================================================================
// 文件：global.glsl
// 功能：histream 着色器共用的全局常量、宏、物理常数、64 方向漫射求积方向表，
//       以及少量数值辅助函数。几乎所有 .glsl 都直接或间接包含本文件。
// 主要模块：
//   1. 通用常量：土壤/冠层缺省参数、PI 及其派生量、数值容差 EPSION_*、线程组大小
//   2. 类型标识：TYPE_*（地物类型）、AEROTYPE_*（空气动力学类型）、KIND_*
//   3. 规模常量：TLASTNUM、SOIL_MEMORY_MODES、DIFFUSE_ANGLE_NUM、BANDNUM/NBAND
//   4. 数值辅助：compensatedAdd（Kahan 补偿求和）、finiteNonnegative
//   5. 物理常数：空气密度/比热、Von Karman 常数、重力、Stefan-Boltzmann 常数等
//   6. PAR 光量子通量换算 absorbedParPhotonFlux、短波分组边界 shortwaveGroupEnd
//   7. 温度/辐亮度阈值（RAD_THRESHOLD、TMIN_THRESHOLD、TMAX_THRESHOLD）
//   8. DIFFUSE_DIRECTIONS[64]：等立体角漫射求积方向（32 对精确反向对）
// 与其他文件关系：SOIL_MEMORY_MODES、TLASTNUM 需与 structs.h 保持一致（原注释已说明）；
//         权重常量被辐射传输/冠层散射阶段使用（Canopy.diffuseG 依赖方向表的排列）。
// 约定：温度 K；辐射 W m-2；波长默认 nm（个别函数另有说明）；方向向量为单位向量，
//       y 为竖直向上（见 mwir_solar.glsl 中 sunDirection.y 用作天顶余弦）。
// ==========================================================================

#ifndef GLOBALS_GLSL
#define GLOBALS_GLSL 1

// 模块：全局缺省常量。POROSITY（土壤孔隙度因子）、SSA（单次散射反照率缺省值）、
//       GST、DENSITY 的具体含义与使用处待核对。
// global constants
#define POROSITY 1 //土壤的孔隙度因子
#define SSA 0.25 // single scattering albedo
#define GST 0.35
#define DENSITY 1

// 数学常量与随机数类型。INFINITY 用 1e32 代替 IEEE 无穷（GLSL 中避免真正的 inf）；
// SHORTDISTANCE 为“短距离”阈值（单位同场景长度，待核对用途）。
// 说明（2026-11-25）：π 有两套并存的定义——下面的宏 PI / TWO_PI（以及后面的 PID2 等）与第 55 行起的
//   const float M_PI / M_TWO_PI / M_PI_2 / M_1_OVER_PI / InvPI。数值同为 float 可表示的 π（常量表达式在
//   编译期折叠为 float，两套结果相同），但各着色器已混用多年；为避免合并时意外改变浮点常量的
//   折叠方式（例如 1/PI 与 M_1_OVER_PI 的最后一位），两套保持并存，新代码任选其一并在同文件内保持一致。
#define PI 3.14159265358979323
#define TWO_PI 6.28318530717958648
#define INFINITY 1e32
#define RngStateType uint // Random type
#define SHORTDISTANCE 0.01

// 模块：地物类型标识。TYPE_* 用于体元/面元的材质类别，AEROTYPE_* 用于空气动力学参数选择。
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

// 模块：float 常量版本的数学常数（与上方 PI 宏并存）及 OUTER（场景外部的大距离），InvPI = 1/π。
//       EPSION_1/2/3 与 EPSION 为不同量级的数值容差（原拼写 EPSION，保持不变）。
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
// HSPOT：Kuusk 热点模型的热点参数缺省值（待核对）。下方原注释说明消光所用叶片投影 G 的来源。
// Hotspot_Kuusk takes the leaf projections G of the sun/view directions used
// for extinction (CanopyG); there is no fixed leaf G constant any more.
#define HSPOT 0.5

// 计算着色器线程组尺寸：2D 分组 8×8，1D 分组 64，3D 分组 8³（待核对各自使用处）。
#define GROUP_SIZEXY 8
#define GROUP_SIZEX 64
#define GROUP_SIZEXYZ 8

// PID2 ≈ 89.26°（略小于 π/2 的角度上限，用来避免天顶角余弦为 0；待核对）。
// TLASTNUM：土壤温度“上一时刻”节点数；BANDNUM 的含义待核对。
// （2026-11-25 删除了未被任何编译中着色器引用的 MAXSTEP 宏；步数上限取 setting.maxStep。）
// BANDNUM=2162 与下方 NBAND=2002 两者用途不同（待核对）。
#define PID2 1.557963
#define TLASTNUM 8
// Sum-of-exponentials modes of the soil method-1 history beyond TLASTNUM nodes
// (SoilSet.inertiaHistory = 1); keep equal to structs.h.
#define SOIL_MEMORY_MODES 12
#define DIFFUSE_ANGLE_NUM 64
#define BANDNUM 2162

// 模块：64 方向漫射求积的权重。球面平均权重 1/64；半球平均权重 2/64（只对上半球方向求和时使用）。
// Equal-solid-angle quadrature. The 64 directions form 32 exact antipodal
// pairs, so neither hemisphere nor any Cartesian axis receives a systematic
// bias. For hemisphere-integrated quantities every direction keeps a fixed
// weight; occluded zero-radiance directions therefore remain part of the
// quadrature instead of being removed from the denominator.
// Directions 2k and 2k+1 are antipodal with |y| = (2k+1)/64; Canopy.diffuseG
// (leaf projection per LIDF) and the scattering pass rely on this layout.
const float DIFFUSE_SPHERE_AVERAGE_WEIGHT = 0.015625; // 1 / 64
const float DIFFUSE_HEMISPHERE_AVERAGE_WEIGHT = 0.03125; // 2 / 64

// 作用：Kahan 补偿求和，减小大量小项累加的 float 舍入误差。
// 输入输出：sum（累加和）与 correction（补偿项）均为 inout，调用方需在整个累加过程中保持它们；
//       value 为本次加数。
void compensatedAdd(inout float sum, inout float correction, float value)
{
    float corrected = value - correction;
    float updated = sum + corrected;
    correction = (updated - sum) - corrected;
    sum = updated;
}

// 作用：把 NaN/inf 置 0，并把负值截到 0，用于保护辐射量累加。
float finiteNonnegative(float value)
{
    return (isnan(value) || isinf(value)) ? 0.0 : max(value, 0.0);
}

// 模块：物理常数与经验系数。单位见各行行尾原注释；RHOA 单位 kg m-3，CP 为 J kg-1 K-1，
//       SIGMASB 为 W m-2 K-4，C2K 把 °C 换算成 K，R 为摩尔气体常数 J mol-1 K-1。
//       KV、CD1、CR、PSICOR、CSSOIL、STRESS 为经验/拟合参数（含义待核对）。
#define NBAND 2002  // 2001 vnir+1 TIR 
#define RHOA 1.2047      // specific mass of air
#define CP   1004        // specific heat of dry air
#define KAPPA 0.4        // Von Karman constant
#define GEARTH  9.81     // gravity acceleration
#define AHC 119.7117122  // N_A h c in J nm per micromole photons (legacy constant)

// 模块：吸收的波段辐射（W m-2）→ 光量子通量（µmol m-2 s-1）。
// 输入：absorbedBandWm2（已对波段积分的吸收辐射）、wavelengthNm（波段中心波长）。
// 说明：光子能量 hc/λ，AHC = N_A·h·c（J nm µmol-1），故通量 = E·λ/AHC。
// The radiative solver has already integrated each band over wavelength.
// W m^-2 * wavelength_nm / AHC -> umol photons m^-2 s^-1.
float absorbedParPhotonFlux(float absorbedBandWm2, float wavelengthNm)
{
    return absorbedBandWm2 * wavelengthNm / AHC;
}

// 模块：短波波段分组的终止下标（不含）。
// 输入：firstBand 起始波段、width 期望分组宽度、bandCount 波段总数；输出：分组末端（开区间）。
// 说明：下标 0..300 对应 400–700 nm（含 700 nm 的 PAR 采样），分组不得跨过 301，
//       以免 PAR 与非 PAR 波段混在同一组。
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

// 温度/辐亮度合理性阈值：低于 TMIN 或高于 TMAX 的温度、低于 RAD_THRESHOLD 的辐亮度
// 被判为异常（具体使用处待核对）。TMAX 取 373 K，因为暴晒的深色屋顶可超过 80 °C。
#define RAD_THRESHOLD 2    // radiance threshold
#define TMIN_THRESHOLD 220  // -53 C
#define TMAX_THRESHOLD 373  // +100 C; sunlit dark roofs exceed 80 C


// （以下为被注释掉的旧版 32 方向表，保留作历史参考。）
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

// 模块：64 个漫射求积方向（单位向量，等立体角）。方向 2k 与 2k+1 互为反向，
//       且 |y| = (2k+1)/64，即 y 在 [-1,1] 上等间隔分层；下标顺序被冠层散射和
//       Canopy.diffuseG 依赖，不可重排。
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
