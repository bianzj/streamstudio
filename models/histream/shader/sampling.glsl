/*
 * Copyright (C) 2018-2019 Michał Siejak
 * This file is part of Quartz - a raytracing aspect for Qt3D.
 * See LICENSE file for licensing information.
 */

#ifndef SAMPLING_H
#define SAMPLING_H 1


#include "parameters.h"
#include "functions.glsl"
#include "random.glsl"


// // SampleTriangle(): sampling for triangle.
// vec2 SampleTriangle(vec2 u)
// {
//     float uxsqrt = sqrt(u.x);
//     return vec2(1.0 - uxsqrt, u.y * uxsqrt);
// }

// // PdfTriangle(): probability distribution function for triangle.
// float PdfTriangle(float area)
// {
//     return 1.0 / area;
// }

// // SampleRectangle(): sampling for rectangle.
// vec2 SampleRectangle(vec2 u, vec2 rmin, vec2 rmax)
// {
//     return rmin + u * (rmax - rmin);
// }

// // SampleDisk(): sampling for disk.
// vec2 SampleDisk(vec2 u)
// {
//     float r = sqrt(u.x);
//     float theta = M_TWO_PI * u.y;
//     return r * vec2(cos(theta), sin(theta));
// }

// // PdfDisk(): probability distribution function for disk.
// // this is only for uint circle.
// float PdfDisk()
// {
//     return M_1_OVER_PI;
// }

// // SampleDiskConcentric: sampling for concentric ring.
// vec2 SampleDiskConcentric(vec2 u)
// {
//     vec2 up = 2.0 * u - vec2(1.0);
//     if (up == vec2(0.0)) {
//         return vec2(0.0);
//     }
//     else {
//         float r, theta;
//         if (abs(up.x) > abs(up.y)) {
//             r = up.x;
//             theta = 0.25 * M_PI * (up.y / up.x);
//         }
//         else {
//             r = up.y;
//             theta = 0.5 * M_PI - 0.25 * M_PI * (up.x / up.y);
//         }
//         return r * vec2(cos(theta), sin(theta));
//     }
// }

// // SampleHemisphere: hemispherical sampling.
// vec3 SampleHemisphere(vec2 u)
// {
//     float phi = M_TWO_PI * u.y;
//     float sinTheta = sqrt(1.0 - u.x * u.x);
//     return vec3(
//         cos(phi) * sinTheta,
//         sin(phi) * sinTheta,
//         u.x
//     );
// }

// // PdfHemisphere: probability distribution function for hemispherical.
// float PdfHemisphere()
// {
//     return 0.5 * M_1_OVER_PI;
// }

// // SampleHemisphereCosine: hemispherical sampling using cosine weighted method.
// vec3 SampleHemisphereCosine(vec2 u)
// {
//     vec2 d = SampleDisk(u);
//     return vec3(d.x, d.y, sqrt(1.0 - d.x * d.x - d.y * d.y));
// }

// // PdfHemisphereCosine: probability distribution function for hemispherical cosine weighted sampling.
// float PdfHemisphereCosine(float cosTheta)
// {
//     return cosTheta * M_1_OVER_PI;
// }

//----------------------------------------------------------------------
// 1. how to sample 
// 2. Its distibution (pdf)
//------------------------------------------------------------------------

// // Generate a random float in [0, 1) given the previous RNG state
// float rnd(inout uint prev)
// {
//   prev = pcg(prev);
//   return (float(prev) * (1.0 / float(0xffffffffu)));
//   //return (float(lcg(prev)) / float(0x01000000));
// }

vec2 sampleRectangle(vec2 u, vec2 rmin, vec2 rmax)
{
    return rmin + u * (rmax - rmin);
}

vec2 sampleDisk(vec2 u)
{
    float r = sqrt(u.x);
    float theta = M_TWO_PI * u.y;
    return r * vec2(cos(theta), sin(theta));
}

vec2 sampleDiskConcentric(vec2 u)
{
    vec2 up = 2.0 * u - vec2(1.0);
    if(up == vec2(0.0)) {
        return vec2(0.0);
    }
    else {
        float r, theta;
        if(abs(up.x) > abs(up.y)) {
            r = up.x;
            theta = 0.25 * M_PI * (up.y / up.x);
        }
        else {
            r = up.y;
            theta = 0.5 * M_PI - 0.25 * M_PI * (up.x / up.y);
        }
        return r * vec2(cos(theta), sin(theta));
    }
}

float pdfDisk()
{
    return M_1_OVER_PI;
}

vec3 sampleHemisphere(vec2 u)
{
    float phi = M_TWO_PI * u.y;
    float sinTheta = sqrt(1.0 - u.x*u.x);
    return vec3(
        cos(phi) * sinTheta,
        sin(phi) * sinTheta,
        u.x
    );
}

float pdfHemisphere()
{
    return 0.5 * M_1_OVER_PI;
}

vec3 SampleHemisphereCosine(vec2 u)
{
    vec2 d = sampleDisk(u);
    return vec3(d.x, d.y, sqrt(1.0 - d.x*d.x - d.y*d.y));
}

vec3 sampleHemisphereCosineConcentric(vec2 u)
{
    vec2 d = sampleDiskConcentric(u);
    return vec3(d.x, d.y, sqrt(1.0 - d.x*d.x - d.y*d.y));
}

float PdfHemisphereCosine(float cosTheta)
{
    return cosTheta * M_1_OVER_PI;
}

vec2 sampleTriangle(vec2 u)
{
    float uxsqrt = sqrt(u.x);
    return vec2(1.0 - uxsqrt, u.y * uxsqrt);
}

float pdfTriangle(float area)
{
    return 1.0 / area;
}


float stepAndOutputRNGFloat(inout uint rngState)
{
  // Condensed version of pcg_output_rxs_m_xs_32_32, with simple conversion to floating-point [0,1].
  rngState  = rngState * 747796405 + 1;
  uint word = ((rngState >> ((rngState >> 28) + 4)) ^ rngState) * 277803737;
  word      = (word >> 22) ^ word;
  return float(word) / 4294967295.0f;
}

// Randomly sampling around +Z
vec3 samplingHemisphere(inout uint seed, in vec3 x, in vec3 y, in vec3 z)
{
  float r1 = Rnd(seed);
  float r2 = Rnd(seed);
  float sq = sqrt(r2);

  vec3 direction = vec3(cos(2 * M_PI * r1) * sq, sin(2 * M_PI * r1) * sq, sqrt(1-r2));
  direction      = direction.x * x + direction.y * y + direction.z * z;

  return direction;
}

vec3 sampleHemisphereCosine(inout uint seed)
{

  float r1 = 2.0f*Rnd(seed) - 1.0f;
  float r2 = 2.0f*Rnd(seed) - 1.0f;

	/* Modified concencric map code with less branching (by Dave Cline), see
	   http://psgraphics.blogspot.ch/2011/01/improved-code-for-concentric-map.html */
	float phi, r;
	if (r1 == 0 && r2 == 0) {
		r = phi = 0;
	} else if (r1*r1 > r2*r2) {
		r   = r1;
		phi = (M_PI/4.0f) * (r2/r1);
	} else {
		r   = r2;
		phi = (M_PI/2.0f) - (r1/r2) * (M_PI/4.0f);
	}
  float cosPhi, sinPhi;
  cosPhi = cos(phi);
  sinPhi = sin(phi);
	
  vec3 direction = vec3(r*cosPhi, r*sinPhi, sqrt(1-r*r*cosPhi*cosPhi-r*r*sinPhi*sinPhi));
  return direction;
}




#endif // QUARTZ_SHADERS_SAMPLING_H