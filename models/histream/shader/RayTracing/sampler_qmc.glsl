// ==========================================================================
// 文件：RayTracing/sampler_qmc.glsl
// 功能：RayTracing 模式的可选低差异采样器：Owen 置乱 Sobol（Burley 2020，
//   "Practical Hash-based Owen Scrambling"，JCGT 9(4)），以二维点对为单位提供随机数。
// 开关：特化常量 RT_SAMPLER_SOBOL（主机端环境变量 HISTREAM_RT_SAMPLER=sobol 或工程 control.sampler="sobol"）；
//   为 0 时 offline.rgen 完全走原来的 PCG 路径，结果逐位不变。
// 维度分配（pairId，每个点对 = 一个独立置乱的 (0,2)-序列二维点）：
//   0                像素内抖动（相机射线）
//   1 + 3d           散射顶点 d 的天空 NEE 方向
//   2 + 3d           散射顶点 d 的下一次散射（BSDF）方向
//   3 + 3d           散射顶点 d 的 (俄罗斯轮盘赌, 反射/透射选择)
//   d = 0 .. QMC_VERTEX_DEPTH-1；更深的顶点回退到逐像素 PCG（kDepth 本身从不超过 maxDepth）。
// 做法：每个点对用 hash(像素种子, pairId) 得到独立种子，先对样本序号做嵌套均匀置乱（Laine–Karras），
//   再取 Sobol 第 0/1 维并各自做 Owen 置乱；不同点对之间互相独立，同一点对内前 2^k 个样本构成 (0,k,2)-网。
// 依赖：GLSL 4.00 bitfieldReverse。全局状态由 offline.rgen 的 main 在每个像素开始时设置。
// ==========================================================================

#ifndef RT_SAMPLER_QMC_GLSL
#define RT_SAMPLER_QMC_GLSL 1

// 使用 Sobol 维度的散射顶点数（kDepth < QMC_VERTEX_DEPTH）；可用 glslc -DQMC_VERTEX_DEPTH=n 做对照实验。
#ifndef QMC_VERTEX_DEPTH
#define QMC_VERTEX_DEPTH 2
#endif

// 采样器选择：特化常量 8（主机端 pipeline.cpp 按 HISTREAM_RT_SAMPLER / control.sampler 设置）。
// 取 0 时驱动在建管线阶段把所有 Sobol 分支折叠掉，生成的代码与旧版 PCG 路径相同（结果逐位不变）。
layout(constant_id = 8) const int RT_SAMPLER_SOBOL = 0;
const bool RT_QMC_ENABLED = RT_SAMPLER_SOBOL != 0;

// 全局状态：当前像素的种子、当前样本序号。
uint gQmcSeed = 0u;
uint gQmcIndex = 0u;

// lowbias32 整数哈希（Chris Wellons）。
uint qmcHash(uint x)
{
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

// Laine–Karras 置乱：高位只受低位影响（对位反转后的数即为“嵌套”置乱）。
uint qmcLaineKarras(uint x, uint seed)
{
  x += seed;
  x ^= x * 0x6c50b47cU;
  x ^= x * 0xb82f1e52U;
  x ^= x * 0xc7afe638U;
  x ^= x * 0x8d22f6e6U;
  return x;
}

// 嵌套均匀（Owen）置乱：位反转 → Laine–Karras → 位反转，作用于“最高位在前”的小数表示。
uint qmcNestedScramble(uint x, uint seed)
{
  x = bitfieldReverse(x);
  x = qmcLaineKarras(x, seed);
  return bitfieldReverse(x);
}

// Sobol 第 1 维（Joe–Kuo 方向数 v_k = v_{k-1} ^ (v_{k-1} >> 1)，第 0 维为位反转）。
uint qmcSobolDim1(uint index)
{
  uint result = 0u;
  uint v = 1u << 31;
  for(; index != 0u; index >>= 1)
  {
    if((index & 1u) != 0u) result ^= v;
    v ^= v >> 1;
  }
  return result;
}

// 32 位小数 → [0,1) 浮点（取高 24 位，严格小于 1）。
float qmcToFloat(uint x)
{
  return float(x >> 8) * (1.0 / 16777216.0);
}

// 当前样本在点对 pairId 上的二维点，范围 [0,1)^2。
vec2 qmcPair(uint pairId)
{
  uint seed = qmcHash(gQmcSeed ^ (pairId * 0x9e3779b9U + 0x7f4a7c15U));
  uint index = qmcNestedScramble(gQmcIndex, seed);
  uint x = bitfieldReverse(index);
  uint y = qmcSobolDim1(index);
  x = qmcNestedScramble(x, qmcHash(seed ^ 0x68bc21ebU));
  y = qmcNestedScramble(y, qmcHash(seed ^ 0x02e5be93U));
  return vec2(qmcToFloat(x), qmcToFloat(y));
}

// 某散射顶点是否使用 Sobol 维度。
bool qmcActive(int vertexDepth)
{
  return RT_QMC_ENABLED && vertexDepth < QMC_VERTEX_DEPTH;
}

// 与 sampling.glsl 的 sampleHemisphereCosine(inout seed) 相同的同心圆映射，但输入为给定的 u（[0,1)^2）。
vec3 qmcCosineHemisphere(vec2 u)
{
  float r1 = 2.0 * u.x - 1.0;
  float r2 = 2.0 * u.y - 1.0;
  float phi, r;
  if(r1 == 0.0 && r2 == 0.0) {
    r = phi = 0.0;
  } else if(r1 * r1 > r2 * r2) {
    r = r1;
    phi = (M_PI / 4.0) * (r2 / r1);
  } else {
    r = r2;
    phi = (M_PI / 2.0) - (r1 / r2) * (M_PI / 4.0);
  }
  float cosPhi = cos(phi);
  float sinPhi = sin(phi);
  return vec3(r * cosPhi, r * sinPhi,
              sqrt(max(0.0, 1.0 - r * r * cosPhi * cosPhi - r * r * sinPhi * sinPhi)));
}

#endif  // RT_SAMPLER_QMC_GLSL
