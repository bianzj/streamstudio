// ==========================================================================
// 文件：functions.glsl
// 功能：histream 着色器（RayTracing / VoxelRT / VoxelEB）共用的函数库：冠层几何与光学、
//       射线求交、热辐射/饱和水汽压换算、光合生化辅助、体元寻址、湍流介质（turbid）光学厚度
//       及周期边界遍历、散射相函数、Kuusk 热点、土壤热传导。
// 主要模块（按文件顺序）：
//   1. 冠层叶片投影 G：CanopyProjection / CanopyDiffuseG
//   2. 程序化天空（仅用于传感器影像的背景）：云量、反射率、温度
//   3. 通用几何：OffsetRay（避免自交）、切线空间/世界空间变换、射线-球、射线-AABB
//   4. 气象/热学：饱和水汽压 es_fun/s_fun、热惯量、Planck/InvPlanck、Stefan-Boltzmann、
//      Hapke 方向发射率
//   5. 光合生化辅助：二次方程求根、电子传输、胞间 CO2、气孔阻抗、computeA
//   6. 体元寻址：getPassedVoxelId、getVoxelIdSemi、getFacetIdPos、SurfFlatten 等
//   7. 湍流介质（turbid）：G/聚集指数/HEX 热点密度、层级（mipmap）透过率 DDA、
//      周期边界遍历、光学厚度与逃逸透过率、ResolveSpatialInteraction
//   8. 散射相函数：IsSameSide、scattering_phase_function_direct_*、CanopyPhase、Hotspot_Kuusk
//   9. 土壤：含水量换算、sunlit/shaded 状态转移、有限体积隐式热传导 Soilheatflux、
//      方法 1 的指数和记忆模式 soilMemoryMode
// 数据流：VoxelRT/VoxelEB/RayTracing 的着色器在 DDA 遍历、散射和能量平衡阶段调用本文件，
//         数据来自 bindingLayout.h 中声明的缓冲与 parameters.h 中的结构体。
// 与其他文件关系：包含 global.glsl（常量）、parameters.h（Canopy、Ray 等结构）、
//         lidf_common.h（LIDF）、periodic_walk.glsl（周期边界步进）。
// 约定：温度 K（土壤热传导内部用 °C，见 Soilheatflux）；长度单位为场景单位或“体元格”，
//       注释中会特别说明；方向向量 +y 向上；波长 > 50 视为 nm 并自动换算为 µm。
// ==========================================================================

#ifndef FUNCTIONS_GLSL
#define FUNCTIONS_GLSL

#include "global.glsl"
#include "parameters.h"
#include "lidf_common.h"
#include "periodic_walk.glsl"

// 模块：冠层叶片投影 G。输入：Canopy（含 lidf 频率与常数 G）、方向（无需归一化）。
// 输出：G；lidfValid==0 时回退为用户常数 G，否则用 LIDF 按天顶角余弦 mu=|dir.y| 计算。
// Leaf projection function G(Omega) of a turbid canopy (Ross): from the SAIL
// leaf inclination classes when the host derived them from LIDFa/LIDFb,
// otherwise the constant user G. direction need not be normalized.
float CanopyProjection(Canopy canopy, vec3 direction)
{
    if (canopy.lidfValid == 0) return canopy.G;
    float length2 = dot(direction, direction);
    float mu = length2 > 1.0e-24 ? abs(direction.y) * inversesqrt(length2) : 1.0;
    return lidfProjection(canopy.lidf, mu);
}

// 模块：漫射求积方向的 G。因互为反向的两方向 |y| 相同，宿主端只存 32 个值，下标取 angle>>1。
// G of the diffuse quadrature direction DIFFUSE_DIRECTIONS[angle], also after
// an azimuthal rotation (which keeps y): |y| = (2*(angle>>1)+1)/64, tabulated
// by the host in canopy.diffuseG.
float CanopyDiffuseG(Canopy canopy, uint angle)
{
    return canopy.lidfValid == 0 ? canopy.G : canopy.diffuseG[angle >> 1];
}
//--------------------------------------------------------------------------------------------
// Common functions
//--------------------------------------------------------------------------------------------

// 模块：程序化天空（仅用于传感器影像中射线未命中时的背景，不参与阴影和散射计算）。
// 作用：用几个低频正弦叠加出确定性的“云量”图样（方位角/高度角的函数），地平线以上才有云。
// 输出：云量 0~1。
// Deterministic low-frequency sky used only for sensor-image misses. It is a
// visual background and is intentionally excluded from shadow and scattering
// queries.
float proceduralSkyCloudAmount(vec3 direction)
{
  vec3 d = normalize(direction);
  float azimuth = atan(d.z, d.x);
  float elevation = asin(clamp(d.y, -1.0, 1.0));
  float pattern = 0.5
    + 0.23 * sin(3.1 * azimuth + 1.7 * sin(2.2 * elevation))
    + 0.17 * sin(6.7 * azimuth - 3.4 * elevation + 0.8)
    + 0.10 * sin(13.0 * azimuth + 5.0 * elevation);
  float aboveHorizon = smoothstep(-0.04, 0.18, d.y);
  return smoothstep(0.58, 0.76, pattern) * aboveHorizon;
}

// 作用：程序化天空的“反射率”背景值：晴空随高度降低、在蓝波段（0.47 µm 附近）增强，
//       有云处向 0.82 混合；总输出限幅到 [0, 0.95]。系数为视觉调校值，非物理标定。
float proceduralSkyReflectance(float wavelength, vec3 direction)
{
  float wavelengthUm = wavelength > 50.0 ? wavelength / 1000.0 : wavelength;
  vec3 d = normalize(direction);
  float height = clamp(d.y * 0.5 + 0.5, 0.0, 1.0);
  float blueResponse = exp(-pow((wavelengthUm - 0.47) / 0.19, 2.0));
  float clearSky = mix(0.34, 0.10, height)
    + blueResponse * mix(0.16, 0.38, height);
  float cloud = proceduralSkyCloudAmount(d);
  return clamp(mix(clearSky, 0.82, cloud), 0.0, 0.95);
}

// 作用：程序化天空的热红外亮温（K）：晴空温度 + 地平线增温（最多 8 K）+ 云（+22 K）。
float proceduralSkyTemperature(vec3 direction, float clearSkyTemperature)
{
  vec3 d = normalize(direction);
  float cloud = proceduralSkyCloudAmount(d);
  float horizonWarmth = 8.0 * (1.0 - clamp(d.y, 0.0, 1.0));
  return clearSkyTemperature + horizonWarmth + cloud * 22.0;
}


//-------------------------------------------------------------------------------------------------
// 模块：OffsetRay —— 沿法线 n 偏移射线起点 p，避免射线与出发面自交。
// 说明：采用 Ray Tracing Gems 第 6 章的方法，对 float 位模式做整数偏移（尺度 256 ulp），
//       因此偏移量与坐标量级成比例；|p|<1/32 的靠近原点分量改用固定偏移 1/65536·n。
// Avoiding self intersections (see Ray Tracing Gems, Ch. 6)
//-------------------------------------------------------------------------------------------------
vec3 OffsetRay(in vec3 p, in vec3 n)
{
  const float intScale   = 256.0f;
  const float floatScale = 1.0f / 65536.0f;
  const float origin     = 1.0f / 32.0f;

  ivec3 of_i = ivec3(intScale * n.x, intScale * n.y, intScale * n.z);

  vec3 p_i = vec3(intBitsToFloat(floatBitsToInt(p.x) + ((p.x < 0) ? -of_i.x : of_i.x)),
                  intBitsToFloat(floatBitsToInt(p.y) + ((p.y < 0) ? -of_i.y : of_i.y)),
                  intBitsToFloat(floatBitsToInt(p.z) + ((p.z < 0) ? -of_i.z : of_i.z)));

  return vec3(abs(p.x) < origin ? p.x + floatScale * n.x : p_i.x,  //
              abs(p.y) < origin ? p.y + floatScale * n.y : p_i.y,  //
              abs(p.z) < origin ? p.z + floatScale * n.z : p_i.z);
}

//-------------------------------------------------------------------------------------------------
// 模块：切线空间 ↔ 世界空间的基变换（切线 x、副法线 y、法线 z）。
// From tangent space/local space to world space and from world space to tangent space/ local space
//-------------------------------------------------------------------------------------------------
vec3 World2Tangent(vec3 normal,vec3 tangent,vec3 binormal,vec3 v)
{
    return vec3(dot(tangent,v),dot(binormal,v),dot(normal,v));
}
vec3 Tangent2World(vec3 normal,vec3 tangent,vec3 binormal,vec3 v)
{
    // The basis vectors are expressed in world coordinates.  Reconstruct the
    // world vector from the tangent-space components; dot products perform the
    // opposite (world-to-tangent) transform and rotate sampled light paths.
    return normalize(tangent * v.x + binormal * v.y + normal * v.z);
}


// 模块：射线-球求交。输出：较近交点的射线参数 t（t 以 r.direction 为单位）；无交点返回 -1。
// 说明：不检查 t 是否为负，调用方需自行判断。
// Ray-Sphere intersection
// http://viclw17.github.io/2018/07/16/raytracing-ray-sphere-intersection/
float hitSphere(const Sphere s, const Ray r)
{
  vec3  oc           = r.origin - s.center;
  float a            = dot(r.direction, r.direction);
  float b            = 2.0 * dot(oc, r.direction);
  float c            = dot(oc, oc) - s.radius * s.radius;
  float discriminant = b * b - 4 * a * c;
  if(discriminant < 0)
  {
    return -1.0;
  }
  else
  {
    return (-b - sqrt(discriminant)) / (2.0 * a);
  }
}

// 模块：射线-AABB 求交（slab 法）。输出：射线从外部进入返回进入参数 t0，起点在盒内
//       返回离开参数 t1，未命中返回 -1。依赖 r.direction 各分量取倒数（零分量得到 inf，仍可用）。
// Ray-AABB intersection
float hitAabb(const AllAabb aabb, const Ray r)
{
  vec3  invDir = 1.0 / r.direction;
  vec3  tbot   = invDir * (aabb.minimum - r.origin);
  vec3  ttop   = invDir * (aabb.maximum - r.origin);
  vec3  tmin   = min(ttop, tbot);
  vec3  tmax   = max(ttop, tbot);
  float t0     = max(tmin.x, max(tmin.y, tmin.z));
  float t1     = min(tmax.x, min(tmax.y, tmax.z));
  if(t1 > max(t0, 0.0))
  {
    if(t0<=0) return t1;
    else return t0;
  }
  else return -1.0;
//   return t1 > max(t0, 0.0) ? t0 : -1.0;
}

//--------------------------------------------------------------------------------------------
//  SATE VAPOR USING A TEMPERATURE  T 300K
//--------------------------------------------------------------------------------------------
// 作用：Tetens 公式求饱和水汽压 es = 6.107·10^(7.5T/(237.3+T))，单位 hPa。
// 说明：T 为摄氏温度（上方原注释的“300K”只是示例标题，待核对）。
float es_fun(float T)
{
    float a = 7.5;
    float b = 237.3;
    float temp = a*T/(b+T);
    return 6.107*pow(10,temp);
}
// 作用：饱和水汽压对温度的斜率 des/dT = es·ln10·7.5·237.3/(237.3+T)²（hPa K-1，T 为 °C）。
//slope of the saturated pressure function
float s_fun(float es, float T)
{
    return es*2.3026*7.5*237.3/((237.3+T)*(237.3+T));
}


//-------------------------------------------
// thermal inertial
//-------------------------------------------
// 作用：热惯量（热渗透率）sqrt(c·ρ·λ)；cs 比热、rhos 密度、lambdas 导热系数。
float thermal_inertial(float cs,float rhos,float lambdas)
{
    return sqrt(cs*rhos*lambdas);
}


// 作用：Ball-Berry 型胞间 CO2：Ci = max(minCi·Cs, Cs·(1-1.6/(slope·RH)))；
//       返回 vec2(Ci, gs)，gs 恒为 0，A 与 CiInput 未使用（疑似未完成的遗留函数，待核对）。
vec2 BallBerry(float Cs, float RH,float A,float BallBerrySlope, float minCi,float CiInput)
{
    float Ci = max(minCi * Cs, Cs *(1-1.6/(BallBerrySlope * RH)) );
    float gs = 0;
    return vec2(Ci,gs);
}

//----------------------------------------------------------
// 模块：sel_root —— 二次方程 a·x²+b·x+c=0 求根。design>0 取 (-b+√Δ)/(2a)，否则取 (-b-√Δ)/(2a)；
//       a==0 时退化为线性解 -c/b。不检查判别式为负，Δ<0 会产生 NaN。
// quadratic formula, root of least magnitude: AX2 + BX + C = 0
//    for the eqn ax^2 + bx + c, 
//    if dsign is:
//       -1, 0: choose the smaller root
//       +1: choose the larger root
//----------------------------------------------------------

float sel_root(float a,float b,float c,float design)
{
    float x;
    if(a ==0)
    {
        x = -c/b;
    }else
    {
        if(design ==0)
        {
            design = -1;
        }
        x = (-b + design *sqrt(b*b - 4*a*c))/(2*a);
    }
    return x;
}

// 模块：Farquhar/von Caemmerer 光合模型的标量辅助函数。约定：无效方程返回 NaN（biochemicalMissing），
//       不用“看似合理的”碳通量替代，调用方负责判断 biochemicalFinite。
// Scalar helpers for the Farquhar/von Caemmerer solver. Invalid equations
// remain missing values rather than being replaced with plausible carbon flux.
float biochemicalMissing()
{
    return uintBitsToFloat(0x7fc00000u);
}

bool biochemicalFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

// 作用：数值稳健的二次方程求根（语义同 sel_root：design>0 取 +√Δ 根）。
// 步骤：1) 系数按最大绝对值缩放；2) 判别式略负（仅 float 舍入量级，8 倍 epsilon）时当作重根；
//       3) 用 q=-0.5(b+sgn(b)√Δ) 以避免抵消，两根分别为 q/a 与 c/q。
float biochemicalQuadraticRoot(float a, float b, float c, float design)
{
    if(!biochemicalFinite(a) || !biochemicalFinite(b) || !biochemicalFinite(c))
        return biochemicalMissing();
    if(a == 0.0)
        return b != 0.0 ? -c / b : biochemicalMissing();
    float coefficientScale = max(abs(a), max(abs(b), abs(c)));
    a /= coefficientScale;
    b /= coefficientScale;
    c /= coefficientScale;
    float bSquared = b * b;
    float fourAC = 4.0 * a * c;
    float discriminant = bSquared - fourAC;
    // Only roundoff-sized negative discriminants may become a double root.
    float tolerance = 8.0 * 1.192092896e-7 * (bSquared + abs(fourAC));
    if(discriminant < -tolerance) return biochemicalMissing();
    float radical = sqrt(max(discriminant, 0.0));
    if(radical == 0.0) return -b / (2.0 * a);
    float q = -0.5 * (b + (b < 0.0 ? -radical : radical));
    // q/a is the non-cancelling root; c/q is its complementary root.
    bool useQ = (design > 0.0) == (b < 0.0);
    return useQ ? q / a : c / q;
}

// 作用：非直角双曲线的电子传输速率 J（θJ²-(Q2+Jmax)J+Q2·Jmax=0 的较小根）。
// 输入：吸收的激发 absorbedExcitation（Q2）、最大容量 capacity（Jmax）、曲率 theta∈[0,1]。
// 说明：先按最大者缩放，再用“有理化”形式求较小根，θ=0 时精确退化为 Q2·Jmax/(Q2+Jmax)；
//       输入非法返回 NaN，任一为 0 返回 0。
float biochemicalElectronTransport(float absorbedExcitation, float capacity, float theta)
{
    if(!biochemicalFinite(absorbedExcitation) || !biochemicalFinite(capacity) ||
       !biochemicalFinite(theta) || absorbedExcitation < 0.0 || capacity < 0.0 ||
       theta < 0.0 || theta > 1.0) return biochemicalMissing();
    if(absorbedExcitation == 0.0 || capacity == 0.0) return 0.0;
    float scale = max(absorbedExcitation, capacity);
    float excitation = absorbedExcitation / scale;
    float maximum = capacity / scale;
    float sum = excitation + maximum;
    float radical = sqrt(max(sum * sum - 4.0 * theta * excitation * maximum, 0.0));
    // Rationalized smaller root. At theta=0 this is Q2*Jms/(Q2+Jms),
    // the exact linear limit of theta*J^2-(Q2+Jms)*J+Q2*Jms=0.
    return scale * (2.0 * excitation * maximum / (sum + radical));
}

// 作用：由 Ball-Berry 关系求胞间 CO2：Ci = max(下界, Cs·(1-1.6/(slope·RH·stress)))。
//       乘积 ≤ 0（气孔关闭）时取下界。
float biochemicalIntercellularCO2(float surfaceCO2, float lowerBound,
                                 float slope, float relativeHumidity, float stress)
{
    float product = slope * relativeHumidity * stress;
    return product > 0.0 ? max(lowerBound, surfaceCO2 * (1.0 - 1.6 / product)) : lowerBound;
}

// 作用：由净同化 A 与 CO2 差求叶片气孔阻抗。A ≤ 0 时返回上限 625000（气孔关闭/呼吸极限）。
// 说明：公式含 ρ_air/M_air 换算与 1e3·1e6/pressure·1e5 的单位因子（单位待核对）。
float biochemicalResistance(float surfaceCO2, float intercellularCO2,
                             float netAssimilation, float airDensity,
                             float molarAirMass, float pressure)
{
    if(netAssimilation <= 0.0) return 625000.0;
    // The difference is nonnegative for a photosynthesizing leaf. Do not
    // divide by A until after testing the closed-stomata/respiration limit.
    return 0.625 * max(surfaceCO2 - intercellularCO2, 0.0) / netAssimilation *
           airDensity / molarAirMass * 1e3 * 1e6 / pressure * 1e5;
}

// 作用：给定胞间 CO2 求净同化 A = Ag - Rd。C3（Type==3）与 C4（Type==4）分支求 Vc、Ve、Vs，
//       再用两次二次方程（曲率 atheta 与 0.98）取较大/较小根合成 Ag。
// 注意（待核对）：分支内重新声明了 `float Vs/Vc/Ve`，遮蔽了外层同名变量，
//       分支外使用的 Vc、Ve、Vs 未被赋值（疑似遗留代码，未使用时无影响；只报告不修改）。
float computeA(float Ci, int Type, float g_m,float Vs_C3,float MM_consts,float Rd,float Vcmax,
                float Gamma_star, float Je,float effcon, float atheta,float kpepcase)
{
    float Vc,Ve,Vs;
    if(Type == 3)
    {
        float Vs = Vs_C3;
        float Vc = Vcmax *(Ci-Gamma_star)/(MM_consts + Ci);
        float CO2_per_electron =(Ci-Gamma_star)/(Ci+2*Gamma_star) * effcon;
        float Ve = Je * CO2_per_electron;
    }else if(Type ==4)
    {
        float Vc = Vcmax;
        float Vs = kpepcase * Ci;
        float CO2_per_electron = effcon;
        float Ve = Je * CO2_per_electron;
    }

    float V = sel_root(atheta, -(Vc+Ve), Vc*Ve, 1);
    float Ag = sel_root(0.98,-(V+Vs),V*Vs, -1);
    float A = Ag - Rd;

    return A;
}




//--------------------------------------------------------------------------------------------
// 模块：Planck 与反 Planck。波长 > 50 视为 nm 并换算为 µm。
//       Planck：W m-2 sr-1 µm-1（c1=1.191e8）；PlanckT：c1 小 1e4 倍，对应 W cm-2 sr-1 µm-1
//       （由常数推断，待核对）；InvPlanck：由辐亮度（与 Planck 同单位）反求亮温 K。
//       Planck 内部用 double 计算指数，最后转 float，避免溢出/精度损失。
// planck and invplanck functions to convert temperature to radiance
//--------------------------------------------------------------------------------------------
 float Planck(float wavelength, float temperature)
{
    float c1 = 11910.439340652*10000;
    float c2 = 14388.291040407;
    if(wavelength > 50) wavelength = wavelength / 1000.0;
    // double c1 = 11910.439340* 10000;
    // double c2 = 14388.291040;

    //if (temperature < 100) temperature = temperature + 273.15;
    double radiance = c1 / (pow(wavelength, 5) * (exp(c2 / temperature / wavelength) - 1));
    
    return float(radiance);
}

 float PlanckT(float wavelength, float temperature)
{
    float c1 = 11910.439340652;
    float c2 = 14388.291040407;
    if(wavelength > 50) wavelength = wavelength / 1000.0;
    // double c1 = 11910.439340* 10000;
    // double c2 =14388.291040;

    double radiance = c1 / (pow(wavelength, 5) * (exp(c2 / temperature / wavelength) - 1)) ;
    
    return float(radiance);
}

float InvPlanck(float wavelength,float radiance)
{
    // double c1 = 11910.439340 * 10000; 
    // double c2 = 14388.291040;
    float c1 = 11910.439340 * 10000; 
    float c2 = 14388.291040;
    if(wavelength > 50) wavelength = wavelength / 1000.0;
    // double temp = c1 / (radiance * pow((wavelength),5)) + 1;
    // double tempp = c2 / (wavelength * log(temp));
    float temp = c1 / (radiance * pow((wavelength),5)) + 1;
    float tempp = c2 / (wavelength * log(temp));
    return float(tempp);
}

//--------------------------------------------------------------------------------------------
// 作用：Hapke 方向发射率 ε(μ)=√(1-w)·(1+2μ/K)/(1+2μ/K·√(1-w))。
// 输入：cosTheta=μ、单次散射反照率 w、参数 K（适用范围见上行原注释）。
//  hapke directional emissivity. {0 < w < 1; 1 < K < 10}
//--------------------------------------------------------------------------------------------
float Emissivity_Hapke(float cosTheta, float w, float K)
{
    float emissivity;
    emissivity = sqrt(1-w) * (1 + 2 * cosTheta/K)/(1 + 2 * cosTheta/K * sqrt(1-w));
    return emissivity;
}


//--------------------------------------------------------------------------------------------
// 模块：Stefan-Boltzmann 及其反函数。σT⁴ 写成 (T/100)⁴·ε·5.6704，避免 T⁴ 在 float 中过大；
//       输出 W m-2，反函数 Inv_Stefen_Boltzmann 由辐射 rad 与发射率反求温度 K（函数名拼写保持原样）。
//  From temperature to radiance T i.e. 300 K
//--------------------------------------------------------------------------------------------
float Stefen_Boltzmann(float T,float emis)
{
    return T/100*T/100*T/100*T/100*emis*5.6704;
}

float Inv_Stefen_Boltzmann(float rad, float emis)
{
    return pow(rad / emis /5.6704 , 0.25) * 100;
}

//-------------------------------------------------------------------------------------------------
// 作用：由法线 N 构造一组正交的切线 Nt 与副法线 Nb；|N.z| 接近 1 时换一种构造避免退化。返回恒为 1。
//  Return the tangent and binormal from the incoming normal
//-------------------------------------------------------------------------------------------------
int createCoordinateSystem(in vec3 N, out vec3 Nt, out vec3 Nb)
{
  Nt = normalize(((abs(N.z) > 0.99999f) ? vec3(-N.x * N.y, 1.0f - N.y * N.y, -N.y * N.z) :
                                          vec3(-N.x * N.z, -N.y * N.z, 1.0f - N.z * N.z)));
  Nb = cross(Nt, N);
  return 1;
}

//--------------------------------------------------------------------------------------------
// 作用：向量长度（等价于 length()）。
// Vector Distance
//--------------------------------------------------------------------------------------------
float Vector2Distance(vec3 vector)
{
    float pathLength = sqrt(vector.x*vector.x+vector.y*vector.y+vector.z*vector.z);
    return pathLength;
}


//--------------------------------------------------------------------------------------------
// （说明：此处的分节标题 “hotspot function proposed by Kuusk” 为历史遗留；其下实际是体元寻址函数，
//   Kuusk 热点函数 Hotspot_Kuusk 在本文件后部。）
// hotspot function proposed by Kuusk 
//--------------------------------------------------------------------------------------------

// 作用：求射线“刚刚穿过”的体元编号：把 position 沿 direction 反向退 0.001 个体元尺寸再取 floor，
//       使恰好落在体元边界上的点归属于来向一侧的体元。
// 输入：position（世界坐标）、scale（体元边长）、direction；输出：整数体元下标 ivec3。
// get passed voxel ID (ivec3)
ivec3 getPassedVoxelId(vec3 position, float scale,vec3 direction)
{
    float minTemp = 0.001;
    int xb = int(floor((position.x-direction.x*minTemp *scale)/scale));
    int yb = int(floor((position.y-direction.y*minTemp *scale)/scale));
    int zb = int(floor((position.z-direction.z*minTemp *scale)/scale)); 

    ivec3 voxelId = ivec3(xb,yb,zb);
    return voxelId;
}

// 模块：带 semiRange 的体元寻址。position 以“体元格”为单位（scale 参数在此重载中未使用），
//       semiRange 是 x、z 方向的半域偏移（网格以原点居中，下标从 0 起算）。
// 输出：返回值 = 沿来向微退 0.05 格所在的体元（“后”体元）；alignVoxelId = 沿前进方向微进
//       0.05 格所在的体元（“前”体元）。y 方向的前进点若低于 0（地面以下）则 y 下标取 -1。
// WITH SEMI
// foward direction 
ivec3 getVoxelIdSemi(vec3 position,vec3 semiRange, float scale,vec3 direction, out ivec3 alignVoxelId)
{
    float minTemp = 0.05;

    // int xf = int((position.x+semiRange.x+direction.x*minTemp*scale)/scale);
    // int yf = int((position.y+direction.y*minTemp*scale)/scale);
    // int zf = int((position.z+semiRange.z+direction.z*minTemp*scale)/scale); 
    // if (position.y+direction.y*minTemp*scale<0) yf = -1;
    // int xb = int((position.x+semiRange.x-direction.x*minTemp *scale)/scale);
    // int yb = int((position.y-direction.y*minTemp*scale)/scale);
    // int zb = int((position.z+semiRange.z-direction.z*minTemp*scale)/scale); 
    int xf = int(floor(position.x+semiRange.x+direction.x*minTemp));
    int yf = int(floor(position.y+direction.y*minTemp));
    int zf = int(floor(position.z+semiRange.z+direction.z*minTemp));
    if (position.y+direction.y*minTemp<0) yf = -1;
    int xb = int(floor(position.x+semiRange.x-direction.x*minTemp));
    int yb = int(floor(position.y-direction.y*minTemp));
    int zb = int(floor(position.z+semiRange.z-direction.z*minTemp));
    ivec3 voxelId = ivec3(xb,yb,zb);
    alignVoxelId = ivec3(xf,yf,zf);
    
    return voxelId;
}

// 作用：同上一重载，但省略 scale 参数，且不对地面以下的 y 做 -1 特例。
ivec3 getVoxelIdSemi(vec3 position,vec3 semiRange,vec3 direction, out ivec3 alignVoxelId)
{
     float minTemp = 0.05;

    int xf = int(floor(position.x+semiRange.x+direction.x*minTemp));
    int yf = int(floor(position.y+direction.y*minTemp));
    int zf = int(floor(position.z+semiRange.z+direction.z*minTemp));
    int xb = int(floor(position.x+semiRange.x-direction.x*minTemp));
    int yb = int(floor(position.y-direction.y*minTemp));
    int zb = int(floor(position.z+semiRange.z-direction.z*minTemp));
    ivec3 voxelId = ivec3(xb,yb,zb);
    alignVoxelId = ivec3(xf,yf,zf);
    
    return voxelId;
}

// 作用：由位置直接求体元下标（int() 向零截断，负坐标不同于 floor，调用方应保证坐标为非负）。
ivec3 getVoxelIdSimple(vec3 position,vec3 semiRange, float scale)
{

    int x = int((position.x+semiRange.x)/scale);
    int y = int(position.y/scale);
    int z = int((position.z+semiRange.z)/scale);
    ivec3 voxelId = ivec3(x,y,z);
    return voxelId;
}

///----------------------------------------
// 作用：判断 position 落在体元 voxelId 的哪个面上。返回值：0=+z 面，1=-z 面，2=-x 面，3=+x 面，
//       4=顶面（y 容差 0.1），7=不在任何面上（其余容差 0.01）。
//       该编号比 voxelLinks 的 faceId（1..5）小 1，见 ResolveVoxelSurfaceState。
// To check the relationship between posistion and semiRange+VoxelId
///---------------------------------------

int getFaceIdSimple(vec3 position,vec3 semiRange,ivec3 voxelId)
{
    vec3 voxelPos = vec3(voxelId.x - semiRange.x, voxelId.y,voxelId.z - semiRange.z);
    if(abs(position.y - voxelPos.y-1)<0.1) return 4;
    if(abs(position.z - voxelPos.z-1)<0.01) return 0;
    else if(abs(position.z - voxelPos.z)<0.01) return 1;
    else if(abs(position.x - voxelPos.x)<0.01) return 2;
    else if(abs(position.x - voxelPos.x-1)<0.01) return 3;
    return 7;
}

// 作用：把体元最小角点 voxelPos0 就地改为 faceid0 所指面的中心点。
//       faceId：0=体元中心，1=+z 面，2=-z 面，3=-x 面，4=+x 面，5=顶面；其他值不改动。返回恒为 1。
int getFacetIdPos(int faceid0, inout vec3 voxelPos0)
{
        if (faceid0 == 1){
        voxelPos0 = vec3(voxelPos0.x + 0.5, voxelPos0.y + 0.5, voxelPos0.z +1.0);
        }
        else if (faceid0 == 2){
        voxelPos0 = vec3(voxelPos0.x + 0.5, voxelPos0.y + 0.5, voxelPos0.z);
        }
        else if (faceid0 == 3){
        voxelPos0 = vec3(voxelPos0.x, voxelPos0.y + 0.5, voxelPos0.z + 0.5);
        }
        else if (faceid0 == 4){
        voxelPos0 = vec3(voxelPos0.x + 1.0, voxelPos0.y + 0.5, voxelPos0.z + 0.5);
        }
        else if (faceid0 == 5){
        voxelPos0 = vec3(voxelPos0.x + 0.5, voxelPos0.y+1, voxelPos0.z + 0.5);
        }
        else if (faceid0 == 0){
            voxelPos0 = vec3(voxelPos0.x + 0.5, voxelPos0.y+0.5, voxelPos0.z + 0.5);
        }

        return 1;
}

// 作用：把水平 (x, z) 体元下标展平为二维地表一维编号 x + z·resX（忽略 y）。
int SurfFlatten(ivec3 voxelId, ivec3 voxelRes)
{
    int surf1DId = voxelId.x + voxelId.z * voxelRes.x;
    return surf1DId;
};

// 模块：以下函数依赖 VoxelRT/VOXELLST 管线才有的缓冲（canopies、voxelLinks、voxelHexs、setting 等），
//       仅在这两类着色器中编译（段落一直到本 #if 对应的 #endif）。
#if defined(VOXELRT) || defined(VOXELLST)
// 模块：CanopyG —— 带单次缓存的冠层投影函数 G（缓存键为 canopyId 与 |cos 天顶角|）。
// 说明：着色器全局变量作为每个调用的私有缓存，射线在相邻体元里重复查询同一方向时避免重算 LIDF 积分。
// G(Omega) of canopy canopyId for a ray direction. Rays query the same canopy
// and direction cell after cell (as the ray direction and as the normalized
// segment direction), so the last value is cached per invocation, keyed by
// |cos zenith| (G depends on nothing else; 1e-6 changes G by < 1e-5).
uint canopyProjectionCacheId = 0xffffffffu;
float canopyProjectionCacheMu = -1.0;
float canopyProjectionCacheValue = 0.5;
float CanopyG(uint canopyId, vec3 direction)
{
    float length2 = dot(direction, direction);
    float mu = length2 > 1.0e-24 ? abs(direction.y) * inversesqrt(length2) : 1.0;
    if (canopyId != canopyProjectionCacheId || abs(mu - canopyProjectionCacheMu) > 1.0e-6) {
        canopyProjectionCacheId = canopyId;
        canopyProjectionCacheMu = mu;
        canopyProjectionCacheValue = canopies[canopyId].lidfValid == 0
            ? canopies[canopyId].G : lidfProjection(canopies[canopyId].lidf, mu);
    }
    return canopyProjectionCacheValue;
}

// 作用：对多状态（建筑五个面）体元，按法线主轴选出对应面状态的体元编号。
// 输入：base（首个状态体元的缓冲下标）、normal；输出：所选状态下标，不满足一致性检查则返回 base。
// 说明：offset 0..4 对应 +z、-z、-x、+x、顶面；检查候选体元的 faceId==offset+1 且 voxelId 相同。
// OBJ cells have one state (faceId=0). Legacy generated buildings have five
// contiguous states (faceId=1..5). Never offset a one-state cell into its neighbour.
int ResolveVoxelSurfaceState(int base, vec3 normal)
{
    if(base < 0 || base >= setting.voxelCount) return base;
    if(voxelLinks[base].faceId == 0) return base;
    vec3 magnitude = abs(normal);
    int offset = magnitude.y >= max(magnitude.x, magnitude.z) ? 4
        : magnitude.x >= magnitude.z ? (normal.x >= 0.0 ? 3 : 2)
        : (normal.z >= 0.0 ? 0 : 1);
    int candidate = base + offset;
    if(candidate >= setting.voxelCount || voxelLinks[candidate].faceId != offset + 1
        || any(notEqual(voxelLinks[candidate].voxelId, voxelLinks[base].voxelId))) return base;
    return candidate;
}

// 作用：固体体元（土壤/建筑/水体）的外法线：faceId 1..5 为轴向单位向量，其余用体元存储的面元法线，
//       法线太短（<0.5）则取水平向上 (0,1,0)。
// Outward normal of a solid (soil/building/water) cell. Legacy generated
// buildings carry it in faceId 1..5; OBJ cells store the area-weighted facet
// normal; cells without one (e.g. DEM ground) are horizontal.
vec3 solidSurfaceNormal(int bufferId)
{
    int face = voxelLinks[bufferId].faceId;
    if(face == 1) return vec3(0.0, 0.0, 1.0);
    if(face == 2) return vec3(0.0, 0.0, -1.0);
    if(face == 3) return vec3(-1.0, 0.0, 0.0);
    if(face == 4) return vec3(1.0, 0.0, 0.0);
    if(face == 5) return vec3(0.0, 1.0, 0.0);
    vec3 n = vec3(voxelLinks[bufferId].normalX, voxelLinks[bufferId].normalY,
                  voxelLinks[bufferId].normalZ);
    float len = length(n);
    return len > 0.5 ? n / len : vec3(0.0, 1.0, 0.0);
}

// 作用：取湍流介质（turbid）体元的体密度 rho。优先级：HEX 介质 > 二维 LAD（islad>=1）>
//       体元自带 leafDensity > 冠层缺省 density；结果非负。
// OBJ 异质性参数优先于二维 LAD 和冠层默认密度；hexId=-1 时保持原算法。
float ResolveTurbidDensity(int bufferId, uint canopyId, ivec3 voxelId, ivec3 voxelRes)
{
    int mediumId = voxelLinks[bufferId].hexId;
    if (mediumId >= 0) return max(voxelHexs[mediumId].rho, 0.0);
    if (setting.islad >= 1) return max(lads[SurfFlatten(voxelId, voxelRes)], 0.0);
    // Objects with geometry-derived leaf density carry it per voxel.
    float leafDensity = voxelLinks[bufferId].leafDensity;
    return leafDensity > 0.0 ? leafDensity : max(canopies[canopyId].density, 0.0);
}

// 作用：沿 direction 的聚集指数 CI（无 HEX 介质时为 1）。HEX 介质以方向分量 |dx|,|dy|,|dz| 为权重，
//       对 (ax, ay, az) 加权；model==10 用 L1 归一化权重并乘 2，其余模型用 L2 方向归一化后加权平均
//       （两种公式的物理含义待核对，参见 docs 中 HEX 介质的说明）。
float ResolveClumpingIndex(int bufferId, vec3 direction)
{
    int mediumId = voxelLinks[bufferId].hexId;
    if (mediumId < 0) return 1.0;
    float length2 = dot(direction, direction);
    if (length2 <= 1.0e-12) return 1.0;
    VoxelHex medium = voxelHexs[mediumId];
    vec3 weights = abs(direction) * inversesqrt(length2);
    if (medium.model == 10) {
        weights /= max(weights.x + weights.y + weights.z, 1.0e-6);
        return 2.0 * dot(weights, vec3(medium.ax, medium.ay, medium.az));
    }
    return max(dot(weights, vec3(medium.ax, medium.ay, medium.az))
               / max(weights.x + weights.y + weights.z, 1.0e-6), 0.0);
}

// 作用：热点计算用的等效密度。model==10 时直接用 rho·(权重·(ax,ay,az))/G（使消光 = 密度·G 等于该方向系数）；
//       否则 = density·CI(direction)。
float HexHotspotDensity(int bufferId, vec3 direction, float density, float G)
{
    int mediumId = voxelLinks[bufferId].hexId;
    if (mediumId >= 0 && voxelHexs[mediumId].model == 10) {
        vec3 weights = abs(direction) /
            max(dot(abs(direction), vec3(1.0)), 1.0e-12);
        return density * dot(
            weights, vec3(voxelHexs[mediumId].ax,
                          voxelHexs[mediumId].ay,
                          voxelHexs[mediumId].az)) / max(G, 1.0e-6);
    }
    return density * ResolveClumpingIndex(bufferId, direction);
}

// 作用：沿一段路径 pathLength（场景单位，乘 sceneScale 换成米）的透过率与拦截率。
// 输出：vec2(透过率 exp(-τ), 拦截率 1-exp(-τ))；τ = 密度·G·CI·长度；model==10 的 HEX 介质
//       用方向消光系数 rho·(权重·(ax,ay,az)) 代替 density·G·CI。负输入一律截为 0。
// x 为透过率，y 为拦截率，二者使用相同的 rho*G*CI(direction)。
vec2 ResolveTurbidInteraction(int bufferId, float density, float G,
                              vec3 direction, float pathLength, float sceneScale)
{
    int mediumId = voxelLinks[bufferId].hexId;
    if (mediumId >= 0 && voxelHexs[mediumId].model == 10) {
        vec3 weights = abs(direction) /
            max(dot(abs(direction), vec3(1.0)), 1.0e-12);
        float extinction = voxelHexs[mediumId].rho * dot(
            weights, vec3(voxelHexs[mediumId].ax,
                          voxelHexs[mediumId].ay,
                          voxelHexs[mediumId].az));
        float transmission = exp(-max(extinction, 0.0) *
                                  max(pathLength, 0.0) *
                                  max(sceneScale, 0.0));
        return vec2(transmission, 1.0 - transmission);
    }
    float opticalDepth = max(density, 0.0) * max(G, 0.0)
        * ResolveClumpingIndex(bufferId, direction)
        * max(pathLength, 0.0) * max(sceneScale, 0.0);
    float transmission = exp(-opticalDepth);
    return vec2(transmission, 1.0 - transmission);
}

// 模块：HEX 锥体投影/重叠积分的着色器全局状态（每个调用私有）：锥的原点、轴、基径与直径斜率；
//       前一体元的重叠信息（overlapPrevious*）。ResetHexOverlap / ResetHexRay / ConfigureCone
//       在每条射线开始时重置它们。
// Negative aperture values are internal HEX render-mode tags.  They do not
// alter the public camera aperture used by the ordinary perspective path.
vec3 coneOrigin = vec3(0.0);
vec3 coneAxis = vec3(0.0, 1.0, 0.0);
float coneBaseDiameter = 0.0;
float coneDiameterSlope = 0.0;
bool overlapHasPrevious = false;
vec3 overlapPreviousMin = vec3(0.0);
vec3 overlapPreviousEnd = vec3(0.0);
vec3 overlapPreviousSpan = vec3(0.0);

// 模块：湍流介质的“上一个体元”状态，用来在下一次命中时补积分其出射段。
// Turbid cells are represented by interior integration planes, so consecutive
// hits along a ray lie in different cells (or the first one starts at the ray
// origin). The previous turbid cell is kept to integrate its exit segment.
bool turbidHasPrevious = false;
vec3 turbidPreviousMin = vec3(0.0);
float turbidPreviousExtinction = 0.0;

void ResetHexOverlap()
{
    overlapHasPrevious = false;
}

void ResetHexRay()
{
    overlapHasPrevious = false;
    turbidHasPrevious = false;
}

// 作用：是否启用锥体投影模式：第 0 个 HEX 介质 model==5 且传感器 aperture==-4（内部渲染模式标记）。
bool ConeProjectionEnabled()
{
    return voxelHexs.length() > 0 && voxelHexs[0].model == 5 &&
           sensorMatrix.aperture == -4.0;
}

bool ConeLODEnabled()
{
    return ConeProjectionEnabled();
}

// 作用：配置锥体：原点、轴（归一化）、基径、直径随距离增长的斜率 2·tan(halfAngle)，并清空上一体元状态。
void ConfigureCone(vec3 origin, vec3 axis, float diameter, float halfAngle)
{
    overlapHasPrevious = false;
    turbidHasPrevious = false;
    coneOrigin = origin;
    coneAxis = normalize(axis);
    coneBaseDiameter = diameter;
    coneDiameterSlope = 2.0 * tan(halfAngle);
}

// 模块：HierarchyTransmission —— 在 HEX 介质的一层 mipmap（n×n×n 子体元）中沿线段 start→end 做 DDA，
//       返回线段透过率（1=无遮挡）。线段先裁剪到父体元 [parentMin, parentMin+1]，空子体元保持为空。
// 输入：rootId（HEX 根）、level、start/end（体元格单位）、parentMin、sceneScale。
// DDA through one mipmap level.  The segment is clipped to the parent
// voxel, so empty child cells remain empty instead of being filled by an
// enclosing bounding-box approximation.
float HierarchyTransmission(int rootId, int level, vec3 start, vec3 end,
                            vec3 parentMin, float sceneScale)
{
    // 步骤 1：读该层的偏移 offset（子体元数据起点）与每边细分数 n，把线段裁剪到父体元盒内；
    //       与盒平行的轴（|delta|<1e-12）只检查起点是否在盒内。
    vec4 metadata = voxelHexs[rootId].children[level];
    int offset = int(metadata.x);
    int n = int(metadata.y);
    vec3 delta = end - start;
    float enter = 0.0;
    float leave = 1.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(delta[axis]) < 1.0e-12) {
            if (start[axis] < parentMin[axis] ||
                start[axis] >= parentMin[axis] + 1.0) return 1.0;
        } else {
            float a = (parentMin[axis] - start[axis]) / delta[axis];
            float b = (parentMin[axis] + 1.0 - start[axis]) / delta[axis];
            enter = max(enter, min(a, b));
            leave = min(leave, max(a, b));
        }
    }
    if (leave <= enter) return 1.0;

    // 步骤 2：确定起点所在子体元（沿前进方向加 1e-5 防止落在边界上而下标错位），并限幅到 [0, n-1]。
    vec3 directionSign = sign(delta);
    ivec3 cell = clamp(ivec3(floor(
        (start + enter * delta - parentMin) * float(n) +
        directionSign * 1.0e-5)), ivec3(0), ivec3(n - 1));
    // 步骤 3：初始化 DDA：各轴到下一个子体元边界的参数 nextBoundary 与跨一格的参数增量 stride；
    //       与线段平行的轴设为 1e30（永不触发）。
    vec3 nextBoundary = vec3(1.0e30);
    vec3 stride = vec3(1.0e30);
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(delta[axis]) < 1.0e-12) continue;
        float boundary = parentMin[axis] +
            float(cell[axis] + (delta[axis] > 0.0 ? 1 : 0)) / float(n);
        nextBoundary[axis] = (boundary - start[axis]) / delta[axis];
        stride[axis] = 1.0 / (float(n) * abs(delta[axis]));
    }

    vec3 weights = abs(delta) /
        max(dot(abs(delta), vec3(1.0)), 1.0e-12);
    // 步骤 4：逐子体元累加光学厚度 τ = Σ (段长·|delta|·sceneScale · 方向权重·(ax,ay,az))；
    //       每步前进到最近的边界（nextBoundary），循环次数上限 3n+3 作为安全保护；
    //       出父体元或下标越界即结束。最终透过率 = exp(-τ)。
    float opticalDepth = 0.0;
    float t = enter;
    float distanceMetres = length(delta) * max(sceneScale, 0.0);
    for (int visit = 0; visit < 3 * n + 3; ++visit) {
        if (any(lessThan(cell, ivec3(0))) ||
            any(greaterThanEqual(cell, ivec3(n)))) break;
        float nextT = min(leave, min(nextBoundary.x,
                                     min(nextBoundary.y, nextBoundary.z)));
        int index = offset + (cell.x * n + cell.y) * n + cell.z;
        vec3 k = vec3(voxelHexs[index].ax,
                       voxelHexs[index].ay,
                       voxelHexs[index].az);
        opticalDepth += max(nextT - t, 0.0) * distanceMetres * dot(weights, k);
        if (nextT >= leave) break;
        for (int axis = 0; axis < 3; ++axis) {
            if (nextBoundary[axis] <= nextT + 1.0e-7) {
                cell[axis] += int(directionSign[axis]);
                nextBoundary[axis] += stride[axis];
            }
        }
        t = nextT;
    }
    return exp(-max(opticalDepth, 0.0));
}

// 模块：ResolveHierarchyTransmission —— 选择 mipmap 层级并计算线段透过率。
// 说明：锥体 LOD 开启时按线段中点处的锥直径选择细节层 depth = -log2(直径)，限制在 [0, maxDepth]，
//       并在相邻两层之间按小数部分线性混合（连续 LOD）；否则固定用最细层 maxDepth（voxelHexs[root].a，待核对）。
float ResolveHierarchyTransmission(int rootId, vec3 start, vec3 end,
                                   vec3 parentMin, float sceneScale)
{
    int maxDepth = int(voxelHexs[rootId].a);
    vec3 midpoint = 0.5 * (start + end);
    float distance = max(dot(midpoint - coneOrigin, coneAxis), 0.0);
    float diameter = coneBaseDiameter + distance * coneDiameterSlope;
    float depth = ConeLODEnabled()
        ? clamp(-log2(max(diameter, 1.0e-12)), 0.0, float(maxDepth))
        : float(maxDepth);
    int coarse = int(floor(depth));
    int fine = min(coarse + 1, maxDepth);
    float blend = depth - float(coarse);
    float coarseTransmission = HierarchyTransmission(
        rootId, coarse, start, end, parentMin, sceneScale);
    if (fine == coarse || blend < 1.0e-6) return coarseTransmission;
    float fineTransmission = HierarchyTransmission(
        rootId, fine, start, end, parentMin, sceneScale);
    return mix(coarseTransmission, fineTransmission, blend);
}

// 作用：线段 start + t·delta（t∈[0,1]）落在以 cellMin 为最小角的单位体元内的比例（0~1）。
//       与盒平行的轴只检查起点是否在盒内（含 1e-5 容差）。
// Apply the six-neighbour gap correction to a segment crossing a compact
// current-scale HEX voxel.  Non-HEX media retain the original transport.
// Fraction of the segment start + t*delta, t in [0,1], inside a unit cell.
float SegmentFractionInCell(vec3 start, vec3 delta, vec3 cellMin)
{
    float entry = 0.0;
    float exitPoint = 1.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(delta[axis]) < 1.0e-12) {
            if (start[axis] < cellMin[axis] - 1.0e-5
                || start[axis] > cellMin[axis] + 1.0 + 1.0e-5) return 0.0;
        } else {
            float a = (cellMin[axis] - start[axis]) / delta[axis];
            float b = (cellMin[axis] + 1.0 - start[axis]) / delta[axis];
            entry = max(entry, min(a, b));
            exitPoint = min(exitPoint, max(a, b));
        }
    }
    return max(exitPoint - entry, 0.0);
}

// 模块：基于 NanoVDB 的湍流介质（turbid）体元网格遍历（仅当包含了 nanovdb_info.glsl 时编译）。
//       光学厚度 τ = Σ 消光系数 × 路径长度；位置单位为“场景体元格”。
#ifdef NANOVDB_INFO
#define TURBID_GRID_AVAILABLE 1
// 全局状态：turbidGridLink / turbidGridReady 保存待用的 NanoVDB 句柄，由 ConfigureTurbidGrid 设置。
// VoxelRT: turbid cells are crossed planes, so an oblique ray can clip a cell
// without hitting any of its planes. Integrate segments cell by cell instead.
NanoVDBLink turbidGridLink;
bool turbidGridReady = false;

void ConfigureTurbidGrid(NanoVDBLink link)
{
    turbidGridLink = link;
    turbidGridReady = true;
}

// Top of the occupied grid in cell units (index bbox of the nanovdb root).
// Every interaction needs a grid cell, so nothing above it affects a ray.
float TurbidGridTop()
{
    if (!turbidGridReady) return 1.0e30;
    pnanovdb_coord_t lower = pnanovdb_root_get_bbox_min(turbidGridLink.buf, turbidGridLink.root);
    pnanovdb_coord_t upper = pnanovdb_root_get_bbox_max(turbidGridLink.buf, turbidGridLink.root);
    return upper.y >= lower.y ? float(upper.y + 1) : 1.0e30;
}

// 作用：某体元沿 direction 的消光系数（每体元格长度）= 密度·G·聚集指数·setting.scale。
// 说明：不是植被网格、或冠层按“参与介质”处理（isParticipatingMedium）的体元返回 0；
//       model==10 的 HEX 介质直接用方向加权的 rho·(ax,ay,az)。
float TurbidCellExtinction(int bufferId, ivec3 cell, vec3 direction)
{
    if (bufferId < 0 || bufferId >= setting.voxelCount) return 0.0;
    uint meshId = instanceLinks[voxelLinks[bufferId].instanceId].meshId;
    if (meshLinks[meshId].type != TYPE_VEGETATION) return 0.0;
    uint canopyId = meshLinks[meshId].canopyId;
    if (isParticipatingMedium(canopies[canopyId])) return 0.0;
    int mediumId = voxelLinks[bufferId].hexId;
    if (mediumId >= 0 && voxelHexs[mediumId].model == 10) {
        vec3 weights = abs(direction) / max(dot(abs(direction), vec3(1.0)), 1.0e-12);
        return max(voxelHexs[mediumId].rho * dot(weights, vec3(voxelHexs[mediumId].ax,
            voxelHexs[mediumId].ay, voxelHexs[mediumId].az)), 0.0) * setting.scale;
    }
    float density = ResolveTurbidDensity(bufferId, canopyId, cell, ivec3(setting.voxelSize));
    return density * max(CanopyG(canopyId, direction), 0.0)
        * ResolveClumpingIndex(bufferId, direction) * setting.scale;
}

// 模块：TurbidBoxSegmentOpticalDepth —— 线段在中心网格盒 [0, voxelSize] 内的光学厚度。
// 步骤 1：把坐标平移 semi（x、z 半域偏移）到非负体元下标，并把线段裁剪到盒内（slab 法）。
// 步骤 2：以 Amanatides-Woo 3D DDA 逐体元步进，每格用 nanovdbGet 取体元编号并累加 τ，
//         迭代上限 1024。
// Optical depth of the turbid cells along start..end inside the central grid
// box only (scene cell units).
float TurbidBoxSegmentOpticalDepth(vec3 start, vec3 end)
{
    vec3 semi = vec3(floor(setting.voxelSize.x * 0.5 + 0.5), 0.0,
                     floor(setting.voxelSize.z * 0.5 + 0.5));
    vec3 a = start + semi;
    vec3 delta = (end + semi) - a;
    float len = length(delta);
    if (len < 1.0e-8) return 0.0;
    vec3 dir = delta / len;
    vec3 lo = vec3(0.0);
    vec3 hi = vec3(setting.voxelSize);
    float t0 = 0.0;
    float t1 = len;
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(dir[axis]) < 1.0e-9) {
            if (a[axis] < lo[axis] || a[axis] > hi[axis]) return 0.0;
        } else {
            float ta = (lo[axis] - a[axis]) / dir[axis];
            float tb = (hi[axis] - a[axis]) / dir[axis];
            t0 = max(t0, min(ta, tb));
            t1 = min(t1, max(ta, tb));
        }
    }
    if (t1 <= t0) return 0.0;

    vec3 p = a + dir * t0;
    ivec3 cell = ivec3(floor(p + dir * 1.0e-5));
    ivec3 stepCell = ivec3(sign(dir));
    vec3 tMax = vec3(1.0e30);
    vec3 tDelta = vec3(1.0e30);
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(dir[axis]) >= 1.0e-9) {
            float boundary = float(cell[axis] + (dir[axis] > 0.0 ? 1 : 0));
            tMax[axis] = t0 + (boundary - p[axis]) / dir[axis];
            tDelta[axis] = 1.0 / abs(dir[axis]);
        }
    }
    float t = t0;
    float tau = 0.0;
    for (int i = 0; i < 1024 && t < t1; ++i) {
        float next = min(min(tMax.x, tMax.y), tMax.z);
        float segmentEnd = min(next, t1);
        int bufferId = nanovdbGet(turbidGridLink, cell);
        tau += TurbidCellExtinction(bufferId, cell, dir) * max(segmentEnd - t, 0.0);
        t = segmentEnd;
        if (t >= t1) break;
        if (tMax.x <= tMax.y && tMax.x <= tMax.z) { cell.x += stepCell.x; tMax.x += tDelta.x; }
        else if (tMax.y <= tMax.z) { cell.y += stepCell.y; tMax.y += tDelta.y; }
        else { cell.z += stepCell.z; tMax.z += tDelta.z; }
    }
    return tau;
}

// 模块：周期边界遍历状态 TurbidPeriodicWalk（下标在“展开”的坐标里，查表时取模回中心域）。
//       period 为 x、z 方向的周期（体元数），startTile 为起点所在周期块，limit 为允许跨越的块数，
//       remaining 为剩余步数预算。
// Periodic boundaries (periodicTraversalCount > 0): the grid repeats along x
// and z with the domain period (voxelSize.x, voxelSize.z cells). This DDA
// runs in unwrapped cell indices, looks every cell up modulo the period and
// stops after periodicTraversalCount boundary passages, when it leaves the
// vertical grid range, or after a bounded number of cells.
struct TurbidPeriodicWalk
{
    ivec3 cell;        // unwrapped cell index
    ivec3 stepCell;
    vec3 tMax;
    vec3 tDelta;
    float t;
    float t1;
    ivec2 period;
    ivec2 startTile;
    int limit;
    int remaining;
};

// 作用：由 setting.periodicNeighborCount 得到允许跨越周期边界的次数（>0 表示启用周期边界）。
int TurbidPeriodicLimit()
{
    return periodicCrossingLimit(setting.periodicNeighborCount);
}

// 作用：展开体元下标 (x, z) 所在的周期块编号（中心域为 (0,0)）。
ivec2 TurbidPeriodicTile(ivec3 cell, ivec2 period)
{
    return ivec2(floor(vec2(float(cell.x), float(cell.z)) / vec2(period)));
}

// 作用：TurbidPeriodicBegin —— 初始化周期遍历：把射线裁剪到竖直范围 [0, min(体元高度, 网格顶)]，
//       设置 DDA 初值与步数预算 1024·(1+limit)。无交返回 false。
// start/direction in scene cell units (as TurbidSegmentOpticalDepth).
bool TurbidPeriodicBegin(vec3 start, vec3 direction, out TurbidPeriodicWalk w)
{
    vec3 semi = vec3(floor(setting.voxelSize.x * 0.5 + 0.5), 0.0,
                     floor(setting.voxelSize.z * 0.5 + 0.5));
    vec3 a = start + semi;
    vec3 dir = normalize(direction);
    float hiY = min(float(setting.voxelSize.y), TurbidGridTop());
    float t0 = 0.0;
    float t1 = 1.0e30;
    if (abs(dir.y) < 1.0e-9) {
        if (a.y < 0.0 || a.y > hiY) return false;
    } else {
        float ta = -a.y / dir.y;
        float tb = (hiY - a.y) / dir.y;
        t0 = max(t0, min(ta, tb));
        t1 = min(t1, max(ta, tb));
    }
    if (t1 <= t0) return false;
    vec3 p = a + dir * t0;
    w.cell = ivec3(floor(p + dir * 1.0e-5));
    w.stepCell = ivec3(sign(dir));
    w.tMax = vec3(1.0e30);
    w.tDelta = vec3(1.0e30);
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(dir[axis]) >= 1.0e-9) {
            float boundary = float(w.cell[axis] + (dir[axis] > 0.0 ? 1 : 0));
            w.tMax[axis] = t0 + (boundary - p[axis]) / dir[axis];
            w.tDelta[axis] = 1.0 / abs(dir[axis]);
        }
    }
    w.t = t0;
    w.t1 = t1;
    w.period = max(ivec2(setting.voxelSize.x, setting.voxelSize.z), ivec2(1));
    w.startTile = TurbidPeriodicTile(w.cell, w.period);
    w.limit = TurbidPeriodicLimit();
    w.remaining = 1024 * (1 + w.limit);
    return true;
}

// 作用：TurbidPeriodicNext —— 取遍历的下一个体元：lookup 为回绕到中心域的下标，segmentLength 为
//       射线在该格内的长度；跨越周期块数超过 limit、步数用尽或到线段终点时返回 false。
// Next cell of the walk: its grid index (wrapped into the central domain) and
// the path length inside it. False when the walk has ended.
bool TurbidPeriodicNext(inout TurbidPeriodicWalk w, out ivec3 lookup, out float segmentLength)
{
    lookup = ivec3(0);
    segmentLength = 0.0;
    if (w.remaining <= 0 || w.t >= w.t1) return false;
    ivec2 tile = TurbidPeriodicTile(w.cell, w.period);
    if (abs(tile.x - w.startTile.x) + abs(tile.y - w.startTile.y) > w.limit) return false;
    float segmentEnd = min(min(min(w.tMax.x, w.tMax.y), w.tMax.z), w.t1);
    lookup = ivec3(w.cell.x - tile.x * w.period.x, w.cell.y,
                   w.cell.z - tile.y * w.period.y);
    segmentLength = max(segmentEnd - w.t, 0.0);
    w.t = segmentEnd;
    --w.remaining;
    if (w.tMax.x <= w.tMax.y && w.tMax.x <= w.tMax.z) { w.cell.x += w.stepCell.x; w.tMax.x += w.tDelta.x; }
    else if (w.tMax.y <= w.tMax.z) { w.cell.y += w.stepCell.y; w.tMax.y += w.tDelta.y; }
    else { w.cell.z += w.stepCell.z; w.tMax.z += w.tDelta.z; }
    return true;
}

// 作用：从 start 沿 direction 逃逸（无终点）时的周期边界光学厚度；τ>50（透过率<2e-22）提前结束。
float TurbidPeriodicOpticalDepth(vec3 start, vec3 direction)
{
    TurbidPeriodicWalk w;
    if (!TurbidPeriodicBegin(start, direction, w)) return 0.0;
    vec3 dir = normalize(direction);
    float tau = 0.0;
    ivec3 cell;
    float segmentLength;
    while (TurbidPeriodicNext(w, cell, segmentLength)) {
        int bufferId = nanovdbGet(turbidGridLink, cell);
        tau += TurbidCellExtinction(bufferId, cell, dir) * segmentLength;
        if (tau > 50.0) break;  // transmittance below 2e-22
    }
    return tau;
}

// 模块：TurbidSegmentOpticalDepth —— 线段 start→end 的光学厚度（含周期边界处理）。
// 说明：起点在其他周期块时，分两遍分别积分（命中块 + 出发块），中间块不积分；
//       保持单一调用点是为性能（见下方原注释的测量结果）。
// Optical depth of the turbid cells along start..end, the free flight from
// the previous interaction (start) to the current hit (end, central grid).
// Periodic boundaries: a hit found in a domain copy has start in that copy's
// frame, outside the central grid. The part in the hit copy and the part in
// the copy the flight started from (its origin cell's exit part) are
// integrated; copies crossed in between had no plane hit and are skipped,
// so the cost stays two box DDAs however many copies the flight crossed.
// One call site of the box DDA (a pass loop that folds to a single pass when
// the PERIODIC_BOUNDARY_ENABLED specialization constant is false): a second
// call, even in a branch that is never taken, made the VoxelEB scattering
// pass ~30% slower without periodic boundaries. With periodic boundaries the
// live second pass still costs the scattering pass about that much (driver
// code generation; the extra DDA work itself is ~4%).
float TurbidSegmentOpticalDepth(vec3 start, vec3 end)
{
    vec3 shift = vec3(0.0);
    int passes = 1;
    if (TurbidPeriodicLimit() > 0) {
        vec2 semi = vec2(floor(setting.voxelSize.x * 0.5 + 0.5),
                         floor(setting.voxelSize.z * 0.5 + 0.5));
        vec2 period = max(vec2(setting.voxelSize.x, setting.voxelSize.z), vec2(1.0));
        ivec2 tile = ivec2(floor((vec2(start.x, start.z) + semi) / period));
        if (tile != ivec2(0)) {
            shift = vec3(float(tile.x) * period.x, 0.0, float(tile.y) * period.y);
            passes = 2;
        }
    }
    float tau = 0.0;
    for (int pass = 0; pass < passes; ++pass) {
        vec3 offset = pass == 0 ? vec3(0.0) : shift;
        tau += TurbidBoxSegmentOpticalDepth(start - offset, end - offset);
    }
    return tau;
}
#endif

// 作用：TurbidEscapeTransmittance —— 射线离开场景前穿过剩余冠层网格的透过率。
//       有 NanoVDB 网格时按周期/普通 DDA（普通情形取 4096 格远处为终点）；否则用上一体元的
//       出射段（turbidHasPrevious）估计。
// A ray that leaves the scene after its last turbid hit still crosses the rest
// of the canopy grid; integrate it (positions are in cell units).
float TurbidEscapeTransmittance(vec3 lastPosition, vec3 direction)
{
#ifdef NANOVDB_INFO
    if (turbidGridReady && TurbidPeriodicLimit() > 0)
        return exp(-TurbidPeriodicOpticalDepth(lastPosition, direction));
    if (turbidGridReady)
        return exp(-TurbidSegmentOpticalDepth(lastPosition,
            lastPosition + normalize(direction) * 4096.0));
#endif
    if (!turbidHasPrevious) return 1.0;
    vec3 delta = normalize(direction) * 2.0;
    float exitLength = 2.0 * SegmentFractionInCell(lastPosition, delta, turbidPreviousMin);
    turbidHasPrevious = false;
    return exp(-turbidPreviousExtinction * exitLength);
}

// 模块：ResolveSpatialInteraction —— 一段飞行 start→end 在湍流介质体元中的相互作用。
// 输出：vec2(透过率, 拦截率)；拦截率 = 1 - 透过率。
// 分支：A) model==5 的层级 HEX（mipmap 透过率 + 面邻居 beta 修正）；
//       B) 普通湍流体元（NanoVDB 网格积分，或入口段 + 上一体元出射段近似）；
//       C) model==10 的 HEX 介质（见后部）。
vec2 ResolveSpatialInteraction(int bufferId, float density, float G,
                               vec3 start, vec3 end, float sceneScale)
{
    vec3 delta = end - start;
    float distance = length(delta);
    int mediumId = voxelLinks[bufferId].hexId;

    if (mediumId >= 0 && voxelHexs[mediumId].model == 5) {
        if (distance < 1.0e-10) return vec2(1.0, 0.0);
        vec3 semi = vec3(floor(setting.voxelSize.x * 0.5 + 0.5), 0.0,
                         floor(setting.voxelSize.z * 0.5 + 0.5));
        vec3 parentMin = vec3(voxelLinks[bufferId].voxelId) - semi;
        float transmission = ResolveHierarchyTransmission(
            mediumId, start, end, parentMin, sceneScale);

        // 步骤：若前一次相互作用的体元是面相邻体元且线段首尾衔接（容差 0.002/0.001），
        //       对透过率施加 beta 修正：ln T += beta·exposure（beta 由面号取自 children[6/7]），
        //       exposure = 前后两段在邻接轴方向上的跨度乘积；结果限制 T ≤ 1。
        // Consecutive face neighbours use the same beta correction as the
        // compact HEX path.  The beta values are stored in children[6/7].
        float entry = 0.0;
        float exitPoint = 1.0;
        for (int axis = 0; axis < 3; ++axis) {
            if (abs(delta[axis]) > 1.0e-12) {
                float a = (parentMin[axis] - start[axis]) / delta[axis];
                float b = (parentMin[axis] + 1.0 - start[axis]) / delta[axis];
                entry = max(entry, min(a, b));
                exitPoint = min(exitPoint, max(a, b));
            }
        }
        if (exitPoint > entry) {
            vec3 first = start + entry * delta;
            vec3 last = start + exitPoint * delta;
            vec3 span = abs(last - first);
            vec3 difference = parentMin - overlapPreviousMin;
            vec3 stepCell = round(difference);
            if (overlapHasPrevious && length(first - overlapPreviousEnd) < 0.002 &&
                length(difference - stepCell) < 0.001 &&
                abs(dot(abs(stepCell), vec3(1.0)) - 1.0) < 0.001 &&
                voxelHexs[mediumId].children[6].w > 0.5) {
                int axis = abs(stepCell.x) > 0.5 ? 0 :
                           (abs(stepCell.y) > 0.5 ? 1 : 2);
                int face = axis * 2 + (stepCell[axis] < 0.0 ? 1 : 0);
                float beta = voxelHexs[mediumId].children[6 + face / 3][face % 3];
                float exposure = clamp(overlapPreviousSpan[axis], 0.0, 1.0) *
                                 clamp(span[axis], 0.0, 1.0);
                transmission = exp(min(0.0, log(max(transmission, 1.0e-30)) +
                                             beta * exposure));
            }
            overlapHasPrevious = true;
            overlapPreviousMin = parentMin;
            overlapPreviousEnd = last;
            overlapPreviousSpan = span;
        } else {
            overlapHasPrevious = false;
        }
        return vec2(transmission, 1.0 - transmission);
    }

    if (mediumId < 0 || voxelHexs[mediumId].model != 10) {
        ResetHexOverlap();
        if (distance < 1.0e-10) return vec2(1.0, 0.0);
#ifdef NANOVDB_INFO
        if (turbidGridReady) {
            float gridTransmission = exp(-TurbidSegmentOpticalDepth(start, end));
            return vec2(gridTransmission, 1.0 - gridTransmission);
        }
#endif
        // 说明（分支 B 无网格时）：只有落在湍流体元内的部分才衰减，即本格的入口段与上一湍流体元的出射段。
        // The segment runs from the previous hit (often the ray origin or the
        // far side of an empty gap) to an interior plane of this cell. Only
        // the parts inside turbid cells attenuate: this cell's entry part and
        // the previous turbid cell's exit part.
        vec3 semi = vec3(floor(setting.voxelSize.x * 0.5 + 0.5), 0.0,
                         floor(setting.voxelSize.z * 0.5 + 0.5));
        vec3 cellMin = vec3(voxelLinks[bufferId].voxelId) - semi;
        float extinction = max(density, 0.0) * max(G, 0.0)
            * ResolveClumpingIndex(bufferId, delta) * max(sceneScale, 0.0);
        float opticalDepth = extinction * distance
            * SegmentFractionInCell(start, delta, cellMin);
        if (turbidHasPrevious
            && any(greaterThan(abs(turbidPreviousMin - cellMin), vec3(0.5))))
            opticalDepth += turbidPreviousExtinction * distance
                * SegmentFractionInCell(start, delta, turbidPreviousMin);
        turbidHasPrevious = true;
        turbidPreviousMin = cellMin;
        turbidPreviousExtinction = extinction;
        float transmission = exp(-opticalDepth);
        return vec2(transmission, 1.0 - transmission);
    }
    if (distance < 1.0e-10) return vec2(1.0, 0.0);

    vec3 semi = vec3(floor(setting.voxelSize.x * 0.5 + 0.5), 0.0,
                     floor(setting.voxelSize.z * 0.5 + 0.5));
    // 分支 C：model==10 的 HEX 介质（方向消光系数由 rho·(ax,ay,az) 按方向分量加权）。
    // 步骤 1：把线段裁剪到本体元盒 [lower, lower+1] 内；与盒平行且起点在盒外，或无交，则重置重叠状态并
    //         视为无衰减 vec2(1,0)。
    vec3 lower = vec3(voxelLinks[bufferId].voxelId) - semi;
    float entry = 0.0;
    float exitPoint = 1.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(delta[axis]) < 1.0e-12) {
            if (start[axis] < lower[axis] ||
                start[axis] >= lower[axis] + 1.0) {
                ResetHexOverlap();
                return vec2(1.0, 0.0);
            }
        } else {
            float a = (lower[axis] - start[axis]) / delta[axis];
            float b = (lower[axis] + 1.0 - start[axis]) / delta[axis];
            entry = max(entry, min(a, b));
            exitPoint = min(exitPoint, max(a, b));
        }
    }
    if (exitPoint <= entry) {
        ResetHexOverlap();
        return vec2(1.0, 0.0);
    }

    // 步骤 2：盒内路径 first→last 的光学厚度 τ = max(rho·(权重·(ax,ay,az)),0)·长度·sceneScale。
    vec3 first = start + entry * delta;
    vec3 last = start + exitPoint * delta;
    vec3 span = abs(last - first);
    vec3 weights = abs(delta) /
        max(dot(abs(delta), vec3(1.0)), 1.0e-12);
    float extinction = voxelHexs[mediumId].rho * dot(
        weights, vec3(voxelHexs[mediumId].ax,
                      voxelHexs[mediumId].ay,
                      voxelHexs[mediumId].az));
    float opticalDepth = max(extinction, 0.0) * length(last - first) *
                         max(sceneScale, 0.0);

    // 步骤 3：重叠（间隙）修正：若与上一体元是面相邻且线段首尾衔接，按共享面的 beta（children[face/3][face%3]）
    //         减小光学厚度 τ -= beta·exposure，exposure 为前后两段沿邻接轴方向跨度的乘积；
    //         最终 τ 限制为非负，输出 vec2(exp(-τ), 1-exp(-τ))，并记录本段供下一体元使用。
    vec3 difference = lower - overlapPreviousMin;
    vec3 stepCell = round(difference);
    if (overlapHasPrevious && length(first - overlapPreviousEnd) < 0.002 &&
        length(difference - stepCell) < 0.001 &&
        abs(dot(abs(stepCell), vec3(1.0)) - 1.0) < 0.001) {
        int axis = abs(stepCell.x) > 0.5 ? 0 :
                   (abs(stepCell.y) > 0.5 ? 1 : 2);
        int face = axis * 2 + (stepCell[axis] < 0.0 ? 1 : 0);
        float beta = voxelHexs[mediumId].children[face / 3][face % 3];
        opticalDepth -= beta * clamp(overlapPreviousSpan[axis], 0.0, 1.0) *
                        clamp(span[axis], 0.0, 1.0);
    }

    overlapHasPrevious = true;
    overlapPreviousMin = lower;
    overlapPreviousEnd = last;
    overlapPreviousSpan = span;
    float transmission = exp(-max(opticalDepth, 0.0));
    return vec2(transmission, 1.0 - transmission);
}
#endif

// （以下被注释掉的 SurfFlatten 为旧版实现，使用 x·resZ + z 的排布；现行版本见前面，排布为 x + z·resX。）
// uint SurfFlatten(ivec3 voxelId, ivec3 voxelRes)
// {
//     uint surf1DId = voxelId.x*voxelRes.z + voxelId.z ;
//     return surf1DId;
// };

//--------------------------------------------------------------------------------------------
// 模块：几何判断与散射相函数。
// Geometry function
//--------------------------------------------------------------------------------------------
// wo: from camera to position
// wi: from position to sun
// if negative, the same side; vice versa

// bool isSameSide(vec3 wi,vec3 wo,vec3 normal)
// {
//     float wio = dot(wi,normal)*dot(wi,normal);
//     if(wio <= 0) return false;
//     else return true;
// }
// 作用：判断入射方向 wi（表面指向太阳）与观测方向 wo（相机指向表面）是否位于表面法线
//       的同一侧。dot(wi,n)·dot(wo,n) < 0 时返回 true（因 wo 指向表面，同侧时两点积异号）。
bool IsSameSide(vec3 wi,vec3 wo,vec3 normal)
{
    float wio = dot(wi,normal)*dot(wo,normal);
    if(wio < 0) return true;
    else return false;
}


// 作用：四象限反正切（返回 (-π, π]），按 x、y 符号手工展开；x=y=0 时返回 0。
float atan2(float y,float x)
{
    float phi;
    if(x > 0)
    {
        phi = atan(y/x);
    }else if(x<0 && y>=0)
    {
        phi = atan(y/x) + PI;
    }else if(x<0 && y<0)
    {
        phi = atan(y/x) - PI;
    }else if(x == 0 && y>0)
    {
        phi = PI/2.0;
    }else if( x==0 && y<0)
    {
        phi = -PI/2.0;
    }else
    {
        phi = 0;
    }
    return phi;
}

// // a direction
// vec2 scattering_phase_function_direct_XYZ(vec3 directioni,vec3 directionj)
// {
//     vec2 Gamma;
//     float thetai = acos(directioni.z);
//     float thetaj = acos(directionj.z);
//     float alphai = 0,alphaj = 0;
//     alphai = atan2(directioni.y,directioni.x);
//     alphaj = atan2(directionj.y,directionj.x);
//     float cosa = directioni.z * directionj.z + sin(thetai) * sin(thetaj)*cos(alphai-alphaj);
//     float a = acos(cosa);
//     float sina = sin(a);
//     Gamma.x = abs(sina + (PI - a)*cosa)/(3*PI);
//     Gamma.y = abs(sina + a*cosa)/(3*PI);
    
//     return Gamma;
// }

// 注意：函数名为 cross0，实际返回两向量的点积（待核对命名；只报告不修改）。
float cross0(vec3 A, vec3 B){
    return A.x*B.x+A.y*B.y+A.z*B.z;
}

/////--------------------------------------------
// 模块：面积散射相函数 Gamma（球形叶倾角分布，Kuusk 1995）。
// 输入：directioni、directionj 两个单位方向（y 向上）；输出 vec2：x 为反射项，y 为透射项（abs 取绝对值）。
// 说明：cosa = 两方向夹角余弦（球坐标展开），a 为散射角；
//       Gamma_R = |sin a + (π-a)cos a|/(3π)，Gamma_T = |sin a + a cos a|/(3π)。
//-- A Fast, Invertible Canopy Reflectance Model, Andres Kuusk, RSE, 1995
// Gamma.x for reflectance
// Gamma.y for transmittance
///----------------------------------------------
vec2 scattering_phase_function_direct_XZY(vec3 directioni,vec3 directionj)
{
    // vec2 Gamma;
    // float thetai = acos(directioni.y);
    // float thetaj = acos(directionj.y);
    // float alphai = 0,alphaj = 0;
    // alphai = atan2(directioni.z,directioni.x);
    // alphaj = atan2(directionj.z,directionj.x);
    // //入射方向与出射方向的夹角余弦值; Cosine value of scattering angle between incident direction and exit direction
    // float cosa = directioni.y * directionj.y + sin(thetai) * sin(thetaj)*cos(alphai-alphaj);
    // //scattering angle 
    // float a = acos(cosa);
    // float sina = sin(a);
    // Gamma.x = (sina + (PI - a)*cosa)/(3*PI);
    // Gamma.y = (sina + a*cosa)/(3*PI);
    // return Gamma;

    vec2 Gamma;
    float thetai = acos(directioni.y);
    float thetaj = acos(directionj.y);
    float alphai = 0,alphaj = 0;
    alphai = atan2(directioni.z,directioni.x);
    alphaj = atan2(directionj.z,directionj.x);
    //入射方向与出射方向的夹角余弦值; Cosine value of scattering angle between incident direction and exit direction
    float cosa = cos(thetai) * cos(thetaj) + sin(thetai) * sin(thetaj)*cos(alphai-alphaj);
    //scattering angle 
    float a = acos(cosa);
    float sina = sin(a);
    Gamma.x = abs(sina + (PI - a)*cosa)/(3*PI);
    Gamma.y = abs(sina + a*cosa)/(3*PI);
    return Gamma;
}
/////--------------------------------------------
// 模块：Ross (1981) 双朗伯球形叶散射相函数（论文形式）。输出 x = GammaR/(ρ+τ) 的系数形式
//       (sin a - a cos a)/(3π)，y = cos a/3（带符号，使后向散射只含 ρ/3，见下方原注释）。
//       其中 a 为入射与散射传播方向的夹角（a=π 为后向散射）。
//-- leaf scattering phase function
///----------------------------------------------
vec2 scattering_phase_function_direct_XZY_paper(vec3 directioni,vec3 directionj)
{
    // vec2 Gamma;
    // float thetai = acos(directioni.y);
    // float thetaj = acos(directionj.y);
    // float alphai = 0,alphaj = 0;
    // alphai = atan2(directioni.z,directioni.x);
    // alphaj = atan2(directionj.z,directionj.x);
    // //入射方向与出射方向的夹角余弦值; Cosine value of scattering angle between incident direction and exit direction
    // float cosa = directioni.y * directionj.y + sin(thetai) * sin(thetaj)*cos(alphai-alphaj);
    // //scattering angle 
    // float a = acos(cosa);
    // float sina = sin(a);
    // Gamma.x = (sina - a * cosa) / 3 * PI;
    // Gamma.y = cosa / 3;
    // return Gamma;

     vec2 Gamma;
    float thetai = acos(directioni.y);
    float thetaj = acos(directionj.y);
    float alphai = 0,alphaj = 0;
    alphai = atan2(directioni.z,directioni.x);
    alphaj = atan2(directionj.z,directionj.x);
    //入射方向与出射方向的夹角余弦值; Cosine value of scattering angle between incident direction and exit direction
    float cosa = cos(thetai) * cos(thetaj) + sin(thetai) * sin(thetaj)*cos(alphai-alphaj);
    //scattering angle 
    float a = acos(cosa);
    float sina = sin(a);
    // Ross (1981) bi-Lambertian spherical leaves; a is the angle between the
    // incident and scattered propagation directions (a = pi: backscatter):
    //   Gamma = (rho+tau)/(3 pi) (sin a - a cos a) + tau/3 cos a
    // Gamma.y is signed so that backscatter carries rho/3 only.
    Gamma.x = (sina - a * cosa) / (3 * PI);
    Gamma.y = cosa / 3.0;
    return Gamma;
}

// 模块：CanopyPhase —— 冠层面积散射相函数，输出 vec2(GammaR, GammaT-GammaR)。
//       有 LIDF 时精确积分 SAIL 各类（lidf_common.h）；否则用球形形状乘 G/0.5 缩放，保证能量守恒。
// Area scattering phase function of a canopy with the arguments of
// scattering_phase_function_direct_XZY_paper: towardSource points to the
// source, rayDirection from the observer into the scene (the scattered light
// propagates along -rayDirection). Returns x = GammaR, y = GammaT - GammaR, so
// Gamma = x (rho + tau) + y tau, normalized as (1/pi) int Gamma = (rho+tau) G.
// With a LIDF the SAIL classes are integrated exactly (lidf_common.h);
// otherwise the spherical shape is scaled to the constant G (G/0.5), so that
// Gamma/G stays energy conserving for any G.
vec2 CanopyPhase(Canopy canopy, vec3 towardSource, vec3 rayDirection)
{
    if (canopy.lidfValid == 0)
        return scattering_phase_function_direct_XZY_paper(towardSource, rayDirection)
            * (max(canopy.G, 0.0) / 0.5);
    vec3 s = normalize(towardSource);
    vec3 o = -normalize(rayDirection);
    float gammaR = 0.0;
    float gammaT = 0.0;
    lidfGamma(canopy.lidf, s.x, s.y, s.z, o.x, o.y, o.z, gammaR, gammaT);
    return vec2(gammaR, gammaT - gammaR);
}


// 模块：Hotspot_Kuusk —— 太阳路径与观测路径的双向空隙概率 P（含热点相关项），公式见下方原注释。
// 输入：directioni/directionj（均指向场景外，y 向上）、pl_from/pl_to（两路径长度）、density、
//       hs（叶片尺寸，m）、gFrom/gTo（两方向叶片投影 G）；输出：P（无相关时等于 Ts·Tv）。
// 步骤 1：求太阳/观测天顶角正切 tans/tanv（cos 下限 1e-3 防止掠射发散）与相对方位 cosphi，
//         得到 dso；水平投影过短时视为同方位（cosphi=1）。
// 步骤 2：a = ks = dso·z/hs（z 为竖直深度）；(1-e^-a)/a 在 a≤0.1 时用一阶展开避免舍入。
// 步骤 3：组装指数项 xx，G 相等时用原先的化简式（保持旧结果），输出 exp(xx·density)。
// Kuusk (1991, 1995) bidirectional gap probability of the sun and view paths
// from a scattering point, both directions pointing away from the scene:
//   P = exp(-rho (Gs ls + Gv lv) + rho sqrt(Gs Gv ls lv) (1 - exp(-a)) / a)
// with a = z * dso / hs, z the vertical depth, hs the leaf size in metres and
//   dso = sqrt(tan^2 ts + tan^2 tv - 2 tan ts tan tv cos(phi_s - phi_v)),
// so a vanishes in the exact backscatter direction (full correlation).
// gFrom/gTo are the leaf projections G of the sun and view directions, the
// same ones used for the extinction that defined pl_from/pl_to (SAIL uses
// sqrt(ks ko) for the correlated term); without correlation P = Ts * Tv.
float Hotspot_Kuusk(vec3 directioni,vec3 directionj, float pl_from, float pl_to, float density, float hs,
                    float gFrom, float gTo)
{
    float mus = max(directioni.y, 1.0e-3);
    float muv = max(directionj.y, 1.0e-3);
    float tans = sqrt(max(1.0 - mus * mus, 0.0)) / mus;
    float tanv = sqrt(max(1.0 - muv * muv, 0.0)) / muv;
    vec2 sunAzimuth = directioni.xz;
    vec2 viewAzimuth = directionj.xz;
    float cosphi = (dot(sunAzimuth, sunAzimuth) > 1.0e-12 && dot(viewAzimuth, viewAzimuth) > 1.0e-12)
        ? dot(normalize(sunAzimuth), normalize(viewAzimuth)) : 1.0;
    float dso = sqrt(max(tans * tans + tanv * tanv - 2.0 * tans * tanv * cosphi, 0.0));
    float depth = sqrt(max(pl_from * mus, 0.0) * max(pl_to * muv, 0.0));
    float ks = dso * depth / max(hs, 1.0e-6);
    float xtemp = ks <= 0.1 ? 1.0 - ks * 0.5 : (1.0 - exp(-ks)) / ks;
    float chs = sqrt(max(pl_from * pl_to, 0.0)) * xtemp;
    float gs = max(gFrom, 0.0);
    float gv = max(gTo, 0.0);
    float xx = gs == gv ? (chs - pl_from - pl_to) * gs // constant G: the former expression
        : sqrt(gs * gv) * chs - gs * pl_from - gv * pl_to;
    float hotspot = exp(xx * density);
    return hotspot;
}



// 作用：两方向各分量差均小于 EPSION_2（0.01）时视为同一方向（非归一化的逐分量比较）。
bool checkSameDirection(vec3 directioni, vec3 directionj)
{
    bool a = false;
    
    if (abs(directioni.x - directionj.x) < EPSION_2)
    {
        if (abs(directioni.y - directionj.y) < EPSION_2)
        {
            if (abs(directioni.z - directionj.z) < EPSION_2)
            {
                a = true;
                return a;
            }
        }
    }
    return a;
}


// 作用：经验关系 0.88·(1-exp(-0.7·LAI^0.75))，由叶面积指数 lai 得到一个 0~0.88 的修正系数
//       （名称提示为“再校正”，具体含义与来源待核对）。
float calReCor(float lai)
{
    float reCor = 0.88 * (1 - exp(-0.7 * pow(lai, 0.75)));
    return reCor;
}


// 作用：把土壤含水量统一成体积分数：>1 视为百分数（×0.01），并限制到 [0, 饱和含水量]。
// Material SMC accepts a volumetric fraction or a percentage.  The pore-space
// limit belongs to this material, rather than the global meteorological state.
float soilMoistureFraction(float moisture, float saturatedWater)
{
    float fraction = moisture > 1.0 ? moisture * 0.01 : moisture;
    return clamp(fraction, 0.0, clamp(saturatedWater, 0.0, 1.0));
}

// 模块：sunlitStateTransfer —— 受光比例 p 变化时在 sunlit(x)/shaded(y) 两个状态间转移历史温度。
// 步骤：p 减小则把 pOld-pNew 的面积份额从 sunlit 并入 shaded（按面积加权平均），p 增大则反之；
//       供体保持原值，从而 p·T_sunlit+(1-p)·T_shaded 守恒。pOld<0 或 p 不变时原样返回。
// Area-conserving sunlit/shaded transfer of one stored history value
// (x = sunlit, y = shaded) when the sunlit fraction changes from pOld (the
// node the values were stored at; < 0 = none) to pNew. If p falls, the share
// pOld-pNew leaves the sunlit state and is merged into the shaded state, and
// vice versa; the donor keeps its value, so p*T_sunlit + (1-p)*T_shaded is
// conserved. Keep in step with facetrt/energyphysics.h sunlitStateTransfer.
// Unchanged p returns the values untouched (bitwise).
vec2 sunlitStateTransfer(vec2 values, float pOld, float pNew)
{
    if (!(pOld >= 0.0) || pOld == pNew) return values;
    float oldPart = clamp(pOld, 0.0, 1.0);
    float newPart = clamp(pNew, 0.0, 1.0);
    if (oldPart == newPart) return values;
    float moved = abs(oldPart - newPart);
    if (newPart < oldPart) {
        float resident = 1.0 - oldPart;
        values.y = (resident * values.y + moved * values.x) / (resident + moved);
    } else {
        float resident = oldPart;
        values.x = (resident * values.x + moved * values.y) / (resident + moved);
    }
    return values;
}

// 模块：Soilheatflux —— 土壤一维隐式热传导（有限体积，8 个节点深度 0–1 m），输出地表土壤热通量 G。
// 输入：上一时刻温度 Tprofile、含水量 Mprofile、地表温度 Tsi、下边界温度、步长 dtime、土壤热参数；
// 输出：G（向下为正，W m-2）与新时刻温度 T[]。温度用摄氏度。
// Implicit, conservative finite-volume conduction on a nonuniform nodal grid.
// Temperatures are Celsius; G is positive downward [W m-2].  cs [J kg-1 K-1]
// and rhos [kg m-3] give the bulk volumetric capacity of the soil at the
// material's own moisture SMC (referenceMoisture), as in FacetEB, VoxelEB
// method 1 and SCOPE (GAM = sqrt(cs rhos lambdas)); only a local moisture that
// differs from SMC (terrain soil water) adds/removes pore-water capacity, the
// same reference convention as the local rss scaling.  Until 2026-11-23 the
// whole SMC was added on top of cs*rhos, double counting the water (built-in
// loam: C 2.12e6 -> 3.17e6 J m-3 K-1).
void Soilheatflux(in float Tprofile[TLASTNUM], in float Mprofile[TLASTNUM], float Tsi,
                  float lowerBoundaryTemperature, inout float G,
                  inout float T[TLASTNUM], float dtime,
                  float cs, float rhos, float lambdas, float saturatedWater,
                  float referenceMoisture)
{
    // 步骤 1：节点深度（m，共 TLASTNUM=8 个，需与 TLASTNUM 一致）；时间步至少 1 s；
    //         各节点容量 = 干土体积热容 + 4.195e6·(局部含水量 - 参考含水量)，下限 1，
    //         只有偏离材料自身 SMC 的那部分水会增减热容（避免重复计水，见上方原注释）。
    float depth[] = {0.0, 0.02, 0.04, 0.10, 0.20, 0.40, 0.60, 1.0};
    float timestep = max(dtime, 1.0);
    float conductivity = max(lambdas, 0.0);
    float bulkCapacity = max(cs, 0.0) * max(rhos, 0.0);
    float referenceWater = soilMoistureFraction(referenceMoisture, saturatedWater);
    float capacity[TLASTNUM];
    float TA[TLASTNUM], TB[TLASTNUM], TC[TLASTNUM], TD[TLASTNUM];
    for(int i = 0; i < TLASTNUM; ++i)
    {
        capacity[i] = max(bulkCapacity + 4.195e6 *
            (soilMoistureFraction(Mprofile[i], saturatedWater) - referenceWater), 1.0);
    }
    // 步骤 2：组装三对角方程：首节点（地表）与末节点（1 m）为 Dirichlet 边界（Tsi、下边界温度），
    //         内部节点为后向欧拉：storage·(T-Told) = TB·(T+ - T) - TC·(T - T-)。
    for(int k = 0; k < TLASTNUM; ++k)
    {
        if(k == 0 || k == TLASTNUM - 1)
        {
            TA[k] = 1.0;
            TB[k] = 0.0;
            TC[k] = 0.0;
            TD[k] = k == 0 ? Tsi : lowerBoundaryTemperature;
        }
        else
        {
            float volumeWidth = 0.5 * (depth[k + 1] - depth[k - 1]);
            float storageRate = capacity[k] * volumeWidth / timestep;
            TB[k] = conductivity / (depth[k + 1] - depth[k]);
            TC[k] = conductivity / (depth[k] - depth[k - 1]);
            TA[k] = storageRate + TB[k] + TC[k];
            TD[k] = storageRate * Tprofile[k];
        }
    }

    // 步骤 3：Thomas 算法（追赶法）：向前消元得 P、Q，再向后回代求各节点新温度。
    float P[TLASTNUM], Q[TLASTNUM];
    P[0] = TB[0] / TA[0];
    Q[0] = TD[0] / TA[0];
    for(int i = 1; i < TLASTNUM; ++i)
    {
        float denominator = TA[i] - TC[i] * P[i - 1];
        P[i] = TB[i] / denominator;
        Q[i] = (TD[i] + TC[i] * Q[i - 1]) / denominator;
    }
    T[TLASTNUM - 1] = Q[TLASTNUM - 1];
    for(int i = TLASTNUM - 2; i >= 0; --i)
    {
        T[i] = P[i] * T[i + 1] + Q[i];
    }

    // 步骤 4：G = 表层半格的储热变化率 + 向第一内部节点的传导通量（向下为正）。
    // Surface half-cell storage plus conduction into the first interior cell.
    // Telescoping the interior balances gives the identical quantity as total
    // column storage plus conduction through the prescribed 1 m boundary.
    float surfaceStorage = capacity[0] * 0.5 * (depth[1] - depth[0]) *
        (T[0] - Tprofile[0]) / timestep;
    G = surfaceStorage + conductivity * (T[0] - T[1]) /
        (depth[1] - depth[0]);
}

// 作用：dG/dTsurf —— 因系统对地表温度线性，用“上一时刻温度为 0、地表温度为 1”的单位响应得到，
//       与 Soilheatflux 使用完全相同的容量、网格与时间步（供牛顿迭代的斜率）。
// The conduction system is linear in surface temperature.  A unit response
// uses exactly the same capacities, grid and timestep as the flux evaluation.
float soilHeatFluxSurfaceDerivative(in float Mprofile[TLASTNUM], float dtime,
                                   float cs, float rhos, float lambdas,
                                   float saturatedWater, float referenceMoisture)
{
    float previous[TLASTNUM], response[TLASTNUM];
    for(int i = 0; i < TLASTNUM; ++i) previous[i] = 0.0;
    float derivative = 0.0;
    Soilheatflux(previous, Mprofile, 1.0, 0.0, derivative, response, dtime,
                 cs, rhos, lambdas, saturatedWater, referenceMoisture);
    return derivative;
}

// 模块：soilMemoryMode —— 土壤方法 1 的“指数和”记忆核：用 SOIL_MEMORY_MODES 个指数项近似
//       τ^-1/2 核，计算第 j 个模态的权重 weight 与每步衰减因子 decay（原理见下方原注释）。
// 输入：j（模态号）、dtime；输出：weight、decay。
// Soil method 1 (semi-infinite conduction, SCOPE discretisation) beyond the
// TLASTNUM explicit nodes.  The kernel tau^-1/2 = pi^-1/2 int s^-1/2 e^(-s tau) ds
// is a trapezoid sum over SOIL_MEMORY_MODES rates s_j, geometric from
// 1/(10 years) to 30/(TLASTNUM dt), with half weights at both ends and the
// part below s_min lumped into the slowest mode.  Mode j keeps
// S_j = sum_{k > TLASTNUM} dT_k exp(-s_j (k - TLASTNUM - 1) dt), dT_k the
// change over the k-th interval back, so S_j <- decay_j S_j + (T_{n-7} - T_{n-8})
// after each node; a constant temperature leaves every S_j at zero.  weight_j
// times S_j adds to sum_k dT_k (sqrt(k dt) - sqrt((k-1) dt)) (the exact
// interval integrals of 1/2 tau^-1/2 dtau).  Relative error of the interval
// weights <= 0.5 % up to 30 days back, 1 % up to 120 days (FacetEB
// soilMemoryModes uses the same formula in double precision).
void soilMemoryMode(int j, float dtime, out float weight, out float decay)
{
    float dt = max(dtime, 1.0);
    // 说明：速率 s 在 1/(10 年)（3.15e8 s）到 30/(TLASTNUM·dt) 之间按对数等间隔取 SOIL_MEMORY_MODES 个；
    //       梯形求积端点半权重，j=0 额外加 2.0 以并入低于 s_min 的尾部；x 很小时 1-e^-x 用泰勒展开避免舍入。
    float logMin = -log(3.15e8);
    float logMax = log(30.0 / (float(TLASTNUM) * dt));
    float h = (logMax - logMin) / float(SOIL_MEMORY_MODES - 1);
    float s = exp(logMin + h * float(j));
    float coefficient = j == 0 ? 0.5 * h + 2.0
                      : (j == SOIL_MEMORY_MODES - 1 ? 0.5 * h : h);
    float x = s * dt;
    float oneMinusDecay = x < 1.0e-3 ? x * (1.0 - x * (0.5 - x / 6.0)) : 1.0 - exp(-x);
    decay = exp(-x);
    weight = coefficient * 0.5 / sqrt(PI * s) * oneMinusDecay * exp(-float(TLASTNUM) * x);
}




#endif
