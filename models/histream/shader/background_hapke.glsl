#ifndef BACKGROUND_HAPKE_GLSL
#define BACKGROUND_HAPKE_GLSL

// Directions are converted to vectors pointing away from the surface.
vec3 surfaceHemisphereDirection(vec3 direction, vec3 normal)
{
    vec3 d = normalize(direction);
    return dot(d, normal) >= 0.0 ? d : -d;
}

float hapkeHFunction(float mu, float singleScatteringAlbedo)
{
    float gamma = sqrt(max(1.0 - singleScatteringAlbedo, 1e-6));
    return (1.0 + 2.0 * mu) / (1.0 + 2.0 * mu * gamma);
}

float hapkePhaseFunction(float cosPhase, float asymmetry)
{
    float g = clamp(asymmetry, -0.95, 0.95);
    float denominator = max(1.0 + 2.0 * g * cosPhase + g * g, 1e-6);
    return (1.0 - g * g) / pow(denominator, 1.5);
}

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

float hapkeReflectanceWeight(float reflectance, vec3 incomingDirection,
                             vec3 outgoingDirection, vec3 surfaceNormal,
                             float B0, float h, float g)
{
    vec3 n = normalize(surfaceNormal);
    vec3 wi = surfaceHemisphereDirection(incomingDirection, n);
    vec3 wo = surfaceHemisphereDirection(outgoingDirection, n);
    float mu0 = max(dot(n, wi), 1e-4);
    float mu = max(dot(n, wo), 1e-4);
    float phaseAngle = acos(clamp(dot(wi, wo), -1.0, 1.0));
    float r = clamp(reflectance, 0.0, 0.999);
    float singleScatteringAlbedo = clamp(4.0 * r / pow(1.0 + r, 2.0), 1e-5, 0.999);
    float directional = hapkeKernel(mu0, mu, phaseAngle,
                                    singleScatteringAlbedo, B0, h, g);
    float reference = hapkeKernel(1.0, 1.0, 0.0,
                                  singleScatteringAlbedo, 0.0, h, g);
    // Hapke's r is radiance per beam irradiance normal to the beam; callers
    // already project the irradiance onto the surface, so use the BRF r/mu0.
    return clamp(r * directional / mu0 / max(reference, 1e-6), 0.0, 4.0);
}

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
    float singleScatteringAlbedo = clamp(
        1.0 - baseEmissivity * baseEmissivity, 1e-5, 0.999);
    float directional = Emissivity_Hapke(mu, singleScatteringAlbedo, 1.5);
    float reference = Emissivity_Hapke(1.0, singleScatteringAlbedo, 1.5);
    float hapkeEmissivity = clamp(
        baseEmissivity * directional / max(reference, 1e-6), 0.0, 1.0);
    return mix(baseEmissivity, hapkeEmissivity, strength);
}

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
