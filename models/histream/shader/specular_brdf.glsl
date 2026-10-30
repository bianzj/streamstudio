#ifndef SPECULAR_BRDF_GLSL
#define SPECULAR_BRDF_GLSL

// Empirical reciprocal, energy-bounded modified Phong BRDF, returned as pi*f.
// Split the existing reflectance budget; do not add an independent light source.
// The hemisphere integral is <= R (equal at normal incidence); T stays unchanged.
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
