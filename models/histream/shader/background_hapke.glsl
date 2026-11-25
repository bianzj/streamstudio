// ==========================================================================
// 文件：background_hapke.glsl
// 功能：背景/地表方向性反射与热红外发射率的 Hapke 近似（着色器端公式库）。
// 主要模块：
//   1. surfaceHemisphereDirection：把方向统一翻到法线所在半球
//   2. Hapke 基本函数：H 函数、相函数（Henyey-Greenstein 形式）、核 hapkeKernel
//   3. hapkeReflectanceWeight / backgroundReflectanceWeight：反射的方向性权重，
//      相对于朗伯反射的比例，并按 backgroundStrength 与朗伯值混合
//   4. surfaceThermalEmissivity / surfaceThermalReflectance：热红外方向发射率及
//      与之配套的（能量守恒的）反射率
// 数据流：调用方给出反照率/反射率、入射与出射方向、表面法线和 backgroundStrength；
//         本文件返回无量纲权重（乘到朗伯 BRDF 上）或发射率/反射率（0~1）。
// 与其他文件关系：Emissivity_Hapke 在 functions.glsl 中定义，PI 在 global.glsl 中定义；
//         使用本文件前需先包含它们（待核对包含顺序）。
// 约定：所有方向向量指向“离开表面”的方向（入射方向亦为指向光源的方向）；
//       角度单位 rad；mu0 / mu 为入射/出射方向与法线夹角的余弦；
//       backgroundStrength=0 时退化为朗伯/基础发射率，=1 时完全采用 Hapke 形式。
// ==========================================================================

#ifndef BACKGROUND_HAPKE_GLSL
#define BACKGROUND_HAPKE_GLSL

// 模块：方向半球化。若 direction 与法线夹角大于 90°则取反，保证结果与 normal 同侧，
//       这样后续 mu = dot(n, d) 恒非负（对朝向表面内侧的输入方向做容错处理）。
// Directions are converted to vectors pointing away from the surface.
vec3 surfaceHemisphereDirection(vec3 direction, vec3 normal)
{
    vec3 d = normalize(direction);
    return dot(d, normal) >= 0.0 ? d : -d;
}

// 模块：Hapke 多次散射 H 函数的近似式 H(x) = (1+2x)/(1+2x·γ)，γ = sqrt(1-w)。
// 输入：mu（方向余弦），singleScatteringAlbedo（单次散射反照率 w）。
// 说明：1-w 下限 1e-6，避免 w→1 时 γ 恰为 0 引起的退化。
float hapkeHFunction(float mu, float singleScatteringAlbedo)
{
    float gamma = sqrt(max(1.0 - singleScatteringAlbedo, 1e-6));
    return (1.0 + 2.0 * mu) / (1.0 + 2.0 * mu * gamma);
}

// 模块：单次散射相函数，Henyey-Greenstein 形式 (1-g²)/(1+2g·cosφ+g²)^1.5。
// 说明：cosPhase 是相角（入射/出射方向夹角，均指向表面外）的余弦；散射角 = π-相角，
//       因此分母中是 +2g·cosφ；g>0 表示前向散射为主。g 限幅到 ±0.95 以避免数值奇异。
float hapkePhaseFunction(float cosPhase, float asymmetry)
{
    float g = clamp(asymmetry, -0.95, 0.95);
    float denominator = max(1.0 + 2.0 * g * cosPhase + g * g, 1e-6);
    return (1.0 - g * g) / pow(denominator, 1.5);
}

// 模块：Hapke 反射核 r = w/(4π) · mu0/(mu0+mu) · [(1+B)·P + H(mu0)·H(mu) - 1]。
// 输入：mu0/mu（入射/出射余弦）、phaseAngle（相角，rad）、w、B0（后向散射/
//       冲击效应振幅）、h（冲击效应宽度）、g（相函数不对称参数）。
// 说明：B = B0/(1+tan(φ/2)/h) 为冲击（opposition）增强项；各分母加下限避免除零。
float hapkeKernel(float mu0, float mu, float phaseAngle,
                  float singleScatteringAlbedo, float B0, float h, float g)
{
    float opposition = max(B0, 0.0)
        / (1.0 + tan(0.5 * phaseAngle) / max(h, 1e-4));
    float phase = hapkePhaseFunction(cos(phaseAngle), g);
    float multiple = hapkeHFunction(mu0, singleScatteringAlbedo)
                   * hapkeHFunction(mu, singleScatteringAlbedo) - 1.0;
    return singleScatteringAlbedo / (4.0 * PI)
         * mu0 / max(mu0 + mu, 1e-5)
         * ((1.0 + opposition) * phase + multiple);
}

// 模块：把“半球反射率 reflectance”转成 Hapke 方向性权重（相对朗伯的倍数）。
// 输入：reflectance（0~1）、入射/出射方向、表面法线、冲击参数 B0/h 与不对称参数 g。
// 输出：无量纲权重，限幅到 [0, 4]；朗伯表面（权重为 1）的对照在 reference 处完成。
float hapkeReflectanceWeight(float reflectance, vec3 incomingDirection,
                             vec3 outgoingDirection, vec3 surfaceNormal,
                             float B0, float h, float g)
{
    vec3 n = normalize(surfaceNormal);
    vec3 wi = surfaceHemisphereDirection(incomingDirection, n);
    vec3 wo = surfaceHemisphereDirection(outgoingDirection, n);
    // 步骤 1：方向半球化并取余弦（下限 1e-4，防止掠射角除零）、计算相角。
    float mu0 = max(dot(n, wi), 1e-4);
    float mu = max(dot(n, wo), 1e-4);
    float phaseAngle = acos(clamp(dot(wi, wo), -1.0, 1.0));
    // 步骤 2：由反射率估计单次散射反照率 w = 4r/(1+r)²（经验近似，待核对来源），
    //       并限幅到 [1e-5, 0.999] 保证 H 函数与核数值稳定。
    float r = clamp(reflectance, 0.0, 0.999);
    float singleScatteringAlbedo = clamp(4.0 * r / pow(1.0 + r, 2.0), 1e-5, 0.999);
    // 步骤 3：实际几何下的核值，与参考几何（天底入射+天底观测、相角 0、无冲击项）的核值相比，
    //       用比值保持“反射率”标度不变，只引入方向性。
    float directional = hapkeKernel(mu0, mu, phaseAngle,
                                    singleScatteringAlbedo, B0, h, g);
    float reference = hapkeKernel(1.0, 1.0, 0.0,
                                  singleScatteringAlbedo, 0.0, h, g);
    // 注意：此处除以 mu0 是为了与调用方已按表面投影后的辐照度配合（见下方原注释）。
    // Hapke's r is radiance per beam irradiance normal to the beam; callers
    // already project the irradiance onto the surface, so use the BRF r/mu0.
    return clamp(r * directional / mu0 / max(reference, 1e-6), 0.0, 4.0);
}

// 模块：背景反射权重。用固定的 Hapke 参数（B0=1, h=0.1, g=0）计算方向性权重，
//       再按 backgroundStrength（0~1）在朗伯值 reflectance 与 Hapke 值之间线性混合。
float backgroundReflectanceWeight(float reflectance, vec3 incomingDirection,
                                  vec3 outgoingDirection, vec3 surfaceNormal,
                                  float backgroundStrength)
{
    float directional = hapkeReflectanceWeight(
        reflectance, incomingDirection, outgoingDirection, surfaceNormal,
        1.0, 0.1, 0.0);
    return mix(max(reflectance, 0.0), directional,
               clamp(backgroundStrength, 0.0, 1.0));
}

// 模块：热红外方向发射率。基础发射率 = 1 - 反射率 - 透射率（基尔霍夫定律，限幅 0~1）。
// 输出：发射率（0~1）；backgroundStrength 为 0 或基础发射率为 0 时直接返回基础值。
float surfaceThermalEmissivity(float reflectance, float transmittance,
                               vec3 outgoingDirection, vec3 surfaceNormal,
                               float backgroundStrength)
{
    float baseEmissivity = clamp(1.0 - reflectance - transmittance, 0.0, 1.0);
    float strength = clamp(backgroundStrength, 0.0, 1.0);
    if (strength <= 0.0 || baseEmissivity <= 0.0)
        return baseEmissivity;

    vec3 n = normalize(surfaceNormal);
    vec3 wo = surfaceHemisphereDirection(outgoingDirection, n);
    float mu = max(dot(n, wo), 1e-4);
    // 说明：用 w = 1-ε² 由基础发射率反推单次散射反照率（经验近似，待核对），
    //       再用 Emissivity_Hapke（functions.glsl）算方向发射率并以天底值归一，
    //       使天底方向仍保持基础发射率，仅在斜视方向产生变化。
    float singleScatteringAlbedo = clamp(
        1.0 - baseEmissivity * baseEmissivity, 1e-5, 0.999);
    float directional = Emissivity_Hapke(mu, singleScatteringAlbedo, 1.5);
    float reference = Emissivity_Hapke(1.0, singleScatteringAlbedo, 1.5);
    float hapkeEmissivity = clamp(
        baseEmissivity * directional / max(reference, 1e-6), 0.0, 1.0);
    return mix(baseEmissivity, hapkeEmissivity, strength);
}

// 模块：热红外反射率 = 1 - 透射率 - 方向发射率（保持能量守恒，限幅 0~1）。
float surfaceThermalReflectance(float reflectance, float transmittance,
                                vec3 outgoingDirection, vec3 surfaceNormal,
                                float backgroundStrength)
{
    float emissivity = surfaceThermalEmissivity(
        reflectance, transmittance, outgoingDirection, surfaceNormal,
        backgroundStrength);
    return clamp(1.0 - transmittance - emissivity, 0.0, 1.0);
}

#endif
