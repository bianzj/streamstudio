// ==========================================================================
// 文件：surface_brdf.glsl
// 功能：地表反射权重（BRDF 相对朗伯反射率的比值）的统一入口，按材料类型选择模型。
// 主要模块：
//   1. dielectricFresnel：介质表面的非偏振菲涅耳反射率
//   2. coxMunkReflectanceWeight：Cox–Munk 粗糙水面（漫反射 + 太阳耀斑镜面项）
//   3. surfaceReflectanceWeight：选择模型（背景 Hapke → 土壤 Hapke → 水面 Cox–Munk → 朗伯）
// 与其他文件的关系：依赖 background_hapke.glsl（backgroundReflectanceWeight、hapkeReflectanceWeight、
//   surfaceHemisphereDirection）以及 soilSets/waterSets/ubom（bindinglayout）；
//   water_brdf.glsl 含有功能相近的独立实现。
// 约定：返回值为乘在反射率上的无量纲权重（朗伯时等于反射率 R）；方向向量均指向远离表面的一侧
//   （待核对）。
// ==========================================================================

#ifndef SURFACE_BRDF_GLSL
#define SURFACE_BRDF_GLSL

#include "background_hapke.glsl"

// 作用：介质（空气→折射率 n）表面的菲涅耳反射率，取 s、p 偏振平均；全反射时返回 1。
// 输入：入射角余弦、折射率（下限 1.0001）。
float dielectricFresnel(float cosIncident, float refractiveIndex)
{
    float eta = max(refractiveIndex, 1.0001);
    float cosI = clamp(cosIncident, 0.0, 1.0);
    float sinT2 = (1.0 - cosI * cosI) / (eta * eta);
    if (sinT2 >= 1.0)
        return 1.0;
    float cosT = sqrt(max(1.0 - sinT2, 0.0));
    float rs = (cosI - eta * cosT) / max(cosI + eta * cosT, 1e-6);
    float rp = (eta * cosI - cosT) / max(eta * cosI + cosT, 1e-6);
    return 0.5 * (rs * rs + rp * rp);
}

// 作用：水面 BRDF 权重 = 漫反射部分 + π·镜面 BRDF，总权重限幅到 16 防止耀斑奇点。
// 镜面 BRDF = F·D/(4 μ0 μ)，D 为 Cox–Munk 高斯斜率分布，F 为菲涅耳项（半角向量处）。
// 输入：反射率、入/出射方向、法线、折射率、配置的斜率方差、漫反射占比、风速（m s-1）。
float coxMunkReflectanceWeight(float reflectance, vec3 incomingDirection,
                               vec3 outgoingDirection, vec3 surfaceNormal,
                               float refractiveIndex, float configuredVariance,
                               float diffuseFraction, float windSpeed)
{
    vec3 n = normalize(surfaceNormal);
    vec3 wi = surfaceHemisphereDirection(incomingDirection, n);
    vec3 wo = surfaceHemisphereDirection(outgoingDirection, n);
    float mu0 = max(dot(n, wi), 1e-4);
    float mu = max(dot(n, wo), 1e-4);
    vec3 halfVector = normalize(wi + wo);
    float muHalf = max(dot(n, halfVector), 1e-4);

    // 斜率方差缺省值随风速线性增大（0.003 + 0.00512·U，U 限幅到 0~30 m/s）。
    // Cox-Munk isotropic mean-square slope; an explicit XML value overrides it.
    float variance = configuredVariance > 0.0
        ? configuredVariance
        : 0.003 + 0.00512 * clamp(windSpeed, 0.0, 30.0);
    variance = max(variance, 1e-4);
    float tanHalf2 = max(1.0 - muHalf * muHalf, 0.0) / (muHalf * muHalf);
    float slopeDistribution = exp(-tanHalf2 / variance)
        / (PI * variance * pow(muHalf, 4.0));
    float fresnel = dielectricFresnel(max(dot(wi, halfVector), 0.0),
                                      refractiveIndex);
    float specularBrdf = fresnel * slopeDistribution / (4.0 * mu0 * mu);
    float diffuse = clamp(diffuseFraction, 0.0, 1.0) * max(reflectance, 0.0);
    return clamp(diffuse + PI * specularBrdf, 0.0, 16.0);
}

// 作用：地表反射权重入口。优先级：背景结构强度>0 用背景 Hapke；土壤且 brdfModel==1 用 Hapke；
// 水体且 brdfModel==1 用 Cox–Munk（风速取气象强迫 u）；否则为朗伯反射率 max(R,0)。
// 输入：类型 typeId、材料索引 bioId（索引 soilSets/waterSets）、反射率、方向、法线、背景强度。
float surfaceReflectanceWeight(uint typeId, uint bioId, float reflectance,
                               vec3 incomingDirection, vec3 outgoingDirection,
                               vec3 surfaceNormal, float backgroundStrength)
{
    float strength = clamp(backgroundStrength, 0.0, 1.0);
    if (strength > 0.0)
    {
        // The scene exposes one structural control; the internal Hapke shape
        // constants stay fixed so spectral colour remains material-driven.
        return backgroundReflectanceWeight(
            reflectance, incomingDirection, outgoingDirection, surfaceNormal,
            strength);
    }
    if (typeId == TYPE_SOIL && soilSets[bioId].brdfModel == 1)
    {
        return hapkeReflectanceWeight(
            reflectance, incomingDirection, outgoingDirection, surfaceNormal,
            soilSets[bioId].hapkeB0, soilSets[bioId].hapkeH,
            soilSets[bioId].hapkeG);
    }
    if (typeId == TYPE_WATER && waterSets[bioId].brdfModel == 1)
    {
        return coxMunkReflectanceWeight(
            reflectance, incomingDirection, outgoingDirection, surfaceNormal,
            waterSets[bioId].refractiveIndex, waterSets[bioId].slopeVariance,
            waterSets[bioId].diffuseFraction, ubom.meteo.u);
    }
    return max(reflectance, 0.0);
}

#endif
