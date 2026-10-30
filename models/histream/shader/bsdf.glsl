#ifndef BSDF_H
#define BSDF_H

//-----------------------------------------------------------------------------------------------------
// 1. which direction
// 2. how much 
//-----------------------------------------------------------------------------------------------------


#include "functions.glsl"
#include "sampling.glsl"





// obtain the cos(theta) i.e. v.z
float CosThetaTangent(vec3 v)
{
    return abs(v.z);
}

// MIS power heuristic for two samples taken from two different distributions
// pdfA and pdfB. The Beta parameter is assumed to be 2.
float PowerHeuristic(float pdfA, float pdfB)
{
    float f = pdfA * pdfA;
    float g = pdfB * pdfB;
    return f / (f + g);
}

// get the coefficient of BSDF(+1/Pi or -1/Pi)
float CoefficientOfBSDF(uint seed)
{
    float bsdf = 1.0;
    float e  = Rnd(seed);
    if(e > 0.5) {
        bsdf = -InvPI;
    }else{
        bsdf = InvPI;
    }
    return bsdf;
}

float ProbabilityOfBSDF(vec3 wi)
{
    float Pr = 0;
    Pr = PdfHemisphereCosine(CosThetaTangent(wi));
    return Pr;
}

// BSDF sampling, generate a direction, coefficient of BSDF, probability of this direction.
void SampleBSDF(inout uint seed, out vec3 wi, out float bsdf, out float Pr)
{
    float e = Rnd(seed);
    wi = sampleHemisphereCosine(seed);  //   This is for A (0.5)
    //wi = sampleHemisphere(vec2(rnd(seed),rnd(seed)));
    if (e >= 0.5) {
        bsdf = -InvPI;
    }
    else {
        bsdf = InvPI;
    }
    Pr = ProbabilityOfBSDF(wi);
}


float Emission_diff(float wavelength, float temperature)
{
  float radiance;
  radiance = Planck(wavelength,temperature);
  return radiance;
}


#endif