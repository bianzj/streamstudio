#ifndef SURFACE_BRDF_GLSL
#define SURFACE_BRDF_GLSL

#include "background_hapke.glsl"

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
