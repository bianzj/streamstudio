#ifndef WATER_BRDF_GLSL
#define WATER_BRDF_GLSL

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
