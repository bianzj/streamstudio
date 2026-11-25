// ==========================================================================
// 文件：specular_brdf.glsl
// 功能：经验的改进 Phong 镜面 BRDF 权重（互易、能量有界），在漫反射反射率预算内划出镜面部分。
// 主要模块：specularReflectanceWeight（唯一函数）。
// 数据流：材料的 SpectralMaterial（reflectance、specularFraction、specularRoughness）与入射/出射方向
//   输入 → 返回 π·f（用于与朗伯 R 权重同量纲地乘到辐射量上）。
// 约定：返回值为相对于漫反射 R 的权重，半球积分不超过 R；透射 T 不受影响。
// ==========================================================================

#ifndef SPECULAR_BRDF_GLSL
#define SPECULAR_BRDF_GLSL

// Empirical reciprocal, energy-bounded modified Phong BRDF, returned as pi*f.
// Split the existing reflectance budget; do not add an independent light source.
// The hemisphere integral is <= R (equal at normal incidence); T stays unchanged.
// 输入：reflectance（R）、镜面占比 fraction（0~1）、粗糙度 roughness、入射方向 incoming（指向光源）、
//   出射方向 outgoing（RayTracing 中为相机到命中点的行进方向）、表面法线。
// 输出：权重；背向半球返回 0。
// 步骤：1) 统一到法线所在半球；2) α=clamp(roughness,0.05,1)，Phong 指数 n=2/α²-2；
//   3) 瓣 lobe=(n+2)/2·cos^n（cos 为镜面反射方向与出射方向夹角），再在 1（朗伯）与 lobe 间按 fraction 线性混合。
float specularReflectanceWeight(float reflectance, float fraction, float roughness,
                               vec3 incoming, vec3 outgoing, vec3 surfaceNormal)
{
    // RayTracing passes outgoing in camera-to-hit (travel) convention.
    vec3 wo = -normalize(outgoing);
    vec3 n = normalize(surfaceNormal);
    if (dot(n, wo) < 0.0) n = -n;
    vec3 wi = normalize(incoming);
    if (dot(n, wi) <= 0.0 || dot(n, wo) <= 0.0) return 0.0;
    float alpha = clamp(roughness, 0.05, 1.0);
    float exponent = max(0.0, 2.0 / (alpha * alpha) - 2.0);
    float cosine = dot(reflect(-wi, n), wo);
    float lobe = cosine > 0.0 ? (exponent + 2.0) * 0.5 * pow(cosine, exponent) : 0.0;
    return max(reflectance, 0.0) * mix(1.0, lobe, clamp(fraction, 0.0, 1.0));
}

#endif
