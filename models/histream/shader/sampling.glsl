// ==========================================================================
// 文件：sampling.glsl
// 功能：蒙特卡洛光线追踪用的采样函数库：由 [0,1)^2 均匀随机数 u 生成矩形/圆盘/三角形/半球上的样本，
//   并给出对应的概率密度（pdf）。部分代码改编自 Quartz（Qt3D 光线追踪），保留其版权说明。
// 主要模块：
//   1. 文件前半部分被注释掉的旧版实现（保留作参考，现用下面同名小写版本）
//   2. 基础采样：sampleRectangle、sampleDisk、sampleDiskConcentric、sampleTriangle 及 pdf
//   3. 半球采样：均匀（sampleHemisphere）与余弦加权（SampleHemisphereCosine 等）及 pdf
//   4. 带随机状态的采样：samplingHemisphere、sampleHemisphereCosine(seed)、stepAndOutputRNGFloat
// 约定：半球采样在局部坐标系中以 +Z 为法线（z 分量为与法线夹角的余弦）；
//   pdf 为对立体角的密度（sr-1），圆盘/三角形 pdf 为对面积的密度。
// 依赖：parameters.h、functions.glsl（M_PI 等常量）、random.glsl（Rnd）。
// 注意：函数名区分大小写，SampleHemisphereCosine(vec2) 与 sampleHemisphereCosine(inout uint) 是两个不同函数。
// ==========================================================================

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

// 模块：基础采样。u 为 [0,1)^2 均匀随机数。
// 作用：在矩形 [rmin, rmax] 内均匀采样。
vec2 sampleRectangle(vec2 u, vec2 rmin, vec2 rmax)
{
    return rmin + u * (rmax - rmin);
}

// 作用：单位圆盘均匀采样（r=√u.x 保证面积均匀）。
vec2 sampleDisk(vec2 u)
{
    float r = sqrt(u.x);
    float theta = M_TWO_PI * u.y;
    return r * vec2(cos(theta), sin(theta));
}

// 作用：Shirley 同心圆映射，把正方形映射到单位圆盘，保持分层采样的邻近关系，且面积均匀。
// 步骤：先把 u 映射到 [-1,1]^2；按 |x|、|y| 大小选分区，角度 θ 取 π/4·(短边/长边)。
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

// 单位圆盘面积密度 1/π。
float pdfDisk()
{
    return M_1_OVER_PI;
}

// 作用：局部坐标系（+Z 为法线）下的均匀半球采样；u.x 直接作为 cosθ；pdf 见 pdfHemisphere。
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

// 均匀半球立体角密度 1/(2π)。
float pdfHemisphere()
{
    return 0.5 * M_1_OVER_PI;
}

// 作用：余弦加权半球采样（Malley 方法）：先在单位圆盘采样，再抬升到半球，z=√(1-x²-y²)。pdf=cosθ/π。
vec3 SampleHemisphereCosine(vec2 u)
{
    vec2 d = sampleDisk(u);
    return vec3(d.x, d.y, sqrt(1.0 - d.x*d.x - d.y*d.y));
}

// 作用：同上，但圆盘用同心圆映射，分层质量更好。
vec3 sampleHemisphereCosineConcentric(vec2 u)
{
    vec2 d = sampleDiskConcentric(u);
    return vec3(d.x, d.y, sqrt(1.0 - d.x*d.x - d.y*d.y));
}

// 余弦加权半球的立体角密度 cosθ/π。
float PdfHemisphereCosine(float cosTheta)
{
    return cosTheta * M_1_OVER_PI;
}

// 作用：在三角形上均匀采样，返回重心坐标中的两个分量（第三个为 1-x-y）。
vec2 sampleTriangle(vec2 u)
{
    float uxsqrt = sqrt(u.x);
    return vec2(1.0 - uxsqrt, u.y * uxsqrt);
}

// 三角形面积密度 1/area（area 为三角形面积）。
float pdfTriangle(float area)
{
    return 1.0 / area;
}


// 作用：推进一步 PCG 状态并返回 [0,1] 内的浮点数（含 1.0，因为除以 2^32-1）。
float stepAndOutputRNGFloat(inout uint rngState)
{
  // Condensed version of pcg_output_rxs_m_xs_32_32, with simple conversion to floating-point [0,1].
  rngState  = rngState * 747796405 + 1;
  uint word = ((rngState >> ((rngState >> 28) + 4)) ^ rngState) * 277803737;
  word      = (word >> 22) ^ word;
  return float(word) / 4294967295.0f;
}

// 作用：以 z 轴为法线（由 x、y、z 三个正交基向量给出的世界系）做余弦加权半球采样。
// 输入：seed（会被 Rnd 更新）和局部坐标基 x,y,z；输出：世界系中的单位方向。
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

// 作用：由随机状态直接生成局部系余弦加权半球方向（同心圆映射，Dave Cline 的少分支版本）。
// 说明：r1=r2=0 时单独处理，避免 r2/r1 除零。
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