// ==========================================================================
// 文件：bsdf.glsl
// 功能：朗伯型 BSDF 的方向采样、符号系数与概率密度，以及热发射辐亮度的封装。
// 主要模块：
//   1. CosThetaTangent：切线空间余弦
//   2. PowerHeuristic：多重重要性采样（MIS）的幂启发式权重（β=2）
//   3. CoefficientOfBSDF / ProbabilityOfBSDF / SampleBSDF：采样方向、系数 ±1/π 与 pdf
//   4. Emission_diff：漫发射辐亮度（Planck）
// 数据流：调用方提供随机种子 seed → 本文件产生切线空间方向 wi、系数 bsdf、概率 Pr。
// 与其他文件关系：依赖 functions.glsl（Rnd、Planck、InvPI 等）与 sampling.glsl
//         （sampleHemisphereCosine、PdfHemisphereCosine）。
// 约定：切线空间中法线为 +z（v.z = cosθ）；bsdf 系数的符号 ±1/π 是体元/面元算法中
//       “正负采样”的约定（概率 0.5 取负），不是物理上的负反射（待核对与调用方的对应关系）。
// ==========================================================================

#ifndef BSDF_H
#define BSDF_H

//-----------------------------------------------------------------------------------------------------
// 1. which direction
// 2. how much 
//-----------------------------------------------------------------------------------------------------


#include "functions.glsl"
#include "sampling.glsl"





// 模块：切线空间余弦。取 |v.z|，即不区分上/下半球。
// obtain the cos(theta) i.e. v.z
float CosThetaTangent(vec3 v)
{
    return abs(v.z);
}

// 模块：MIS 幂启发式权重 w_A = pdfA² / (pdfA² + pdfB²)。
// MIS power heuristic for two samples taken from two different distributions
// pdfA and pdfB. The Beta parameter is assumed to be 2.
float PowerHeuristic(float pdfA, float pdfB)
{
    float f = pdfA * pdfA;
    float g = pdfB * pdfB;
    return f / (f + g);
}

// 模块：BSDF 系数。以 0.5 概率取 +1/π 或 -1/π（朗伯 BRDF 常数 1/π，符号随机）。
// 输入：seed（随机数种子）；输出：±InvPI。
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

// 模块：方向 wi 的采样概率密度，即余弦加权半球分布的 pdf（cosθ/π，由 PdfHemisphereCosine 给出）。
float ProbabilityOfBSDF(vec3 wi)
{
    float Pr = 0;
    Pr = PdfHemisphereCosine(CosThetaTangent(wi));
    return Pr;
}

// 模块：BSDF 采样。余弦加权采样半球方向 wi，同时随机取符号系数 bsdf=±1/π，
//       Pr 为该方向的 pdf。注意：Pr 不含符号选择的 0.5 概率（待核对调用方是否另行处理）。
// 副作用：推进 seed。
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


// 模块：漫发射辐亮度。直接返回黑体 Planck(wavelength, temperature)，未乘发射率。
// 输入：波长（单位与 Planck 实现一致，见 functions.glsl）、温度 K。
float Emission_diff(float wavelength, float temperature)
{
  float radiance;
  radiance = Planck(wavelength,temperature);
  return radiance;
}


#endif