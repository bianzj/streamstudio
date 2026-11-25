// ==========================================================================
// 文件：water_brdf.glsl
// 功能：水面 BRDF：Cox–Munk 粗糙面权重与由菲涅耳反射率导出的方向反射率。
// 主要模块：
//   1. waterDielectricFresnel：非偏振菲涅耳反射率（与 surface_brdf.glsl 的 dielectricFresnel 相同）
//   2. waterCoxMunkWeight：漫反射 + 太阳耀斑镜面项的权重，限幅到 16
//   3. waterDirectionalReflectance：按出射方向的方向反射率（菲涅耳与漫反射率混合）
// 数据流：WaterSet（refractiveIndex、slopeVariance、diffuseFraction、brdfModel）+ 入/出射方向 + 法线
//   → 无量纲权重/反射率。brdfModel!=1 时退化为朗伯反射率。
// 注意：依赖 surfaceHemisphereDirection、waterSets、PI（由包含本文件前的头文件提供）；
//   waterCoxMunkWeight 在斜率方差为 0 时用固定中等风速值 0.01324（不随气象风速变化），
//   与 surface_brdf.glsl 的风速公式不同。
// ==========================================================================

#ifndef WATER_BRDF_GLSL
#define WATER_BRDF_GLSL

// 作用：空气→水的非偏振菲涅耳反射率（s、p 偏振平均），全反射时为 1。
float waterDielectricFresnel(float cosIncident, float refractiveIndex)
{
    float eta = max(refractiveIndex, 1.0001);
    float cosI = clamp(cosIncident, 0.0, 1.0);
    float sinT2 = (1.0 - cosI * cosI) / (eta * eta);
    if (sinT2 >= 1.0) return 1.0;
    float cosT = sqrt(max(1.0 - sinT2, 0.0));
    float rs = (cosI - eta * cosT) / max(cosI + eta * cosT, 1e-6);
    float rp = (eta * cosI - cosT) / max(eta * cosI + cosT, 1e-6);
    return 0.5 * (rs * rs + rp * rp);
}

// 作用：Cox–Munk 水面 BRDF 权重：diffuseFraction·R + π·F·D/(4 μ0 μ)，总量限幅到 16。
// 输入：水体设置索引 bioId、漫反射率、入/出射方向、法线。D 为以半角向量为基准的高斯斜率分布。
float waterCoxMunkWeight(uint bioId, float reflectance,
                         vec3 incomingDirection, vec3 outgoingDirection,
                         vec3 surfaceNormal)
{
    WaterSet water = waterSets[bioId];
    if (water.brdfModel != 1) return max(reflectance, 0.0);
    vec3 n = normalize(surfaceNormal);
    vec3 wi = surfaceHemisphereDirection(incomingDirection, n);
    vec3 wo = surfaceHemisphereDirection(outgoingDirection, n);
    float mu0 = max(dot(n, wi), 1e-4);
    float mu = max(dot(n, wo), 1e-4);
    vec3 halfVector = normalize(wi + wo);
    float muHalf = max(dot(n, halfVector), 1e-4);
    // A zero value selects a stable moderate-wind surface for RT-only runs.
    float variance = water.slopeVariance > 0.0 ? water.slopeVariance : 0.01324;
    variance = max(variance, 1e-4);
    float tanHalf2 = max(1.0 - muHalf * muHalf, 0.0) / (muHalf * muHalf);
    float distribution = exp(-tanHalf2 / variance)
        / (PI * variance * pow(muHalf, 4.0));
    float fresnel = waterDielectricFresnel(
        max(dot(wi, halfVector), 0.0), water.refractiveIndex);
    float specular = fresnel * distribution / (4.0 * mu0 * mu);
    float diffuse = clamp(water.diffuseFraction, 0.0, 1.0)
        * max(reflectance, 0.0);
    return clamp(diffuse + PI * specular, 0.0, 16.0);
}

// 作用：给定出射方向的水面方向反射率：在菲涅耳反射率与漫反射率之间按 diffuseFraction 混合，结果限幅到 0~1。
float waterDirectionalReflectance(uint bioId, float reflectance,
                                  vec3 outgoingDirection, vec3 surfaceNormal)
{
    WaterSet water = waterSets[bioId];
    if (water.brdfModel != 1) return clamp(reflectance, 0.0, 1.0);
    float cosine = abs(dot(normalize(surfaceNormal), normalize(outgoingDirection)));
    float fresnel = waterDielectricFresnel(cosine, water.refractiveIndex);
    return clamp(mix(fresnel, reflectance,
        clamp(water.diffuseFraction, 0.0, 1.0)), 0.0, 1.0);
}

#endif
