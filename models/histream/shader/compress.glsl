// ==========================================================================
// 文件：compress.glsl（含 NVIDIA 原版 Apache-2.0 许可头，见下方）
// 功能：把单位向量（法线/切线）用八面体映射（octahedral）压缩到一个 32 位 uint，并解压。
//       同一份代码既可在 GLSL 着色器中使用，也可在宿主端 C++（__cplusplus）中编译使用。
// 主要模块：
//   1. __cplusplus 兼容层：uintBitsToFloat / floatBitsToUint / packUnorm4x8 / roundEven 的
//      C++ 等价实现，使 compress_unit_vec 在 CPU 端同样可用
//   2. compress_unit_vec：单位向量 → uint（x、y 各 16 位，z 的符号由八面体折叠隐含）
//   3. short_to_floatm11 / decompress_unit_vec：uint → 单位向量
// 数据流：宿主端或着色器调用 compress_unit_vec 写入顶点/材质缓冲；着色器端
//         decompress_unit_vec 读取后得到归一化向量。
// 约定：~0u 作为“无效向量”哨兵（输入含 inf/NaN 时返回，解压得到全 C_Stack_Max 向量）；
//       有效向量恰好编码为 ~0u 时改写为 ~0x1u 以避免与哨兵冲突（精度损失可忽略）。
//       方法来源：JCGT《A Survey of Efficient Representations for Independent Unit Vectors》。
// ==========================================================================

/*
 * Copyright (c) 2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2021 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */

//-------------------------------------------------------------------------------------------------
// This file can compress normal or tangent to a single uint
// all oct functions derived from:
// "A Survey of Efficient Representations for Independent Unit Vectors"
// http://jcgt.org/published/0003/02/01/paper.pdf


#ifndef COMPRESS_GLSL
#define COMPRESS_GLSL


// 模块：C++ 兼容层。在宿主端编译时补齐 GLSL 内建函数（位转换、packUnorm4x8、roundEven），
//       并把 INLINE 定义为 inline；在 GLSL 中 INLINE 为空（见 #else 分支）。
// 说明：uintBitsToFloat/floatBitsToUint 用 union 做类型双关（type punning）。
#ifdef __cplusplus
#define INLINE inline

INLINE float uintBitsToFloat(uint32_t const& v)
{
  union
  {
    uint  in;
    float out;
  } u;

  u.in = v;

  return u.out;
};

INLINE uint32_t floatBitsToUint(float v)
{
  union
  {
    float in;
    uint  out;
  } u;

  u.in = v;

  return u.out;
};

// 模块：packUnorm4x8 的 C++ 版：每个分量限幅到 [0,1] 后量化为 8 位并打包（x 在最低字节）。
INLINE uint packUnorm4x8(vec4 const& v)
{
  union
  {
    unsigned char in[4];
    uint          out;
  } u;

  u.in[0] = (unsigned char)std::round(std::min(std::max(v.x, 0.0f), 1.0f) * 255.f);
  u.in[1] = (unsigned char)std::round(std::min(std::max(v.y, 0.0f), 1.0f) * 255.f);
  u.in[2] = (unsigned char)std::round(std::min(std::max(v.z, 0.0f), 1.0f) * 255.f);
  u.in[3] = (unsigned char)std::round(std::min(std::max(v.w, 0.0f), 1.0f) * 255.f);

  return u.out;
}

// 模块：roundEven 的 C++ 版（四舍五入，恰为 .5 时取偶数）。
// 说明：非 .5 的情况直接用 std::round；恰为 .5 时按整数部分奇偶决定，负数分支为原作者的 workaround。
INLINE float roundEven(float x)
{
  int   Integer        = static_cast<int>(x);
  float IntegerPart    = static_cast<float>(Integer);
  float FractionalPart = (x - floor(x));

  if(FractionalPart > 0.5f || FractionalPart < 0.5f)
  {
    return std::round(x);
  }
  else if((Integer % 2) == 0)
  {
    return IntegerPart;
  }
  else if(x <= 0)  // Work around...
  {
    return IntegerPart - 1;
  }
  else
  {
    return IntegerPart + 1;
  }
}

#else
#define INLINE
#endif


//-----------------------------------------------------------------------
// 模块：压缩（可在宿主端或设备端执行）。
// Compression - can be done on host or device
//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// C_Stack_Max = float 最大值（FLT_MAX），既用作有效性判据，也用作解压失败时的返回值。
#define C_Stack_Max 3.402823466e+38f
// 作用：把单位向量 nv 压缩为 uint。输入：nv（应已归一化）；输出：压缩值，无效输入返回 ~0u。
// 说明：八面体映射 —— 先按 L1 范数缩放到 ±32767 的整数格点，z<0 时把下半八面体折叠到
//       上半（符号翻转交换），最后 y、x 各偏移 32767 后分别放在高/低 16 位。
INLINE uint compress_unit_vec(vec3 nv)
{
  // map to octahedron and then flatten to 2D (see 'Octahedron Environment Maps' by Engelhardt & Dachsbacher)
  if((nv.x < C_Stack_Max) && !isinf(nv.x))
  {
    // 步骤 1：按 L1 范数把向量投影到八面体表面，并缩放到整数范围 ±32767。
    const float d = 32767.0f / (abs(nv.x) + abs(nv.y) + abs(nv.z));
    int         x = int(roundEven(nv.x * d));
    int         y = int(roundEven(nv.y * d));

    // 步骤 2：z<0 的下半八面体折叠到平面上（x、y 互换并取符号），用位掩码避免分支。
    if(nv.z < 0.0f)
    {
      const int maskx = x >> 31;
      const int masky = y >> 31;
      const int tmp   = 32767 + maskx + masky;
      const int tmpx  = x;
      x               = (tmp - (y ^ masky)) ^ maskx;
      y               = (tmp - (tmpx ^ maskx)) ^ masky;
    }

    // 步骤 3：偏移成非负数后打包；若恰等于哨兵 ~0u，则改为 ~0x1u 以保持哨兵唯一。
    uint packed = (uint(y + 32767) << 16) | uint(x + 32767);
    if(packed == ~0u)
      return ~0x1u;
    return packed;
  }
  else
  {
    return ~0u;
  }
}


///
// 作用：把 16 位整数 v（约 ±32767）线性映射到 [-1, 1] 的 float。
// 说明：借助 IEEE 754 尾数位构造 1.xxx 再减 1，避免整数→浮点除法；符号位单独处理。
float short_to_floatm11(const int v)  // linearly maps a short 32767-32768 to a float -1-+1 //!! opt.?
{
  return (v >= 0) ? (uintBitsToFloat(0x3F800000u | (uint(v) << 8)) - 1.0f) :
                    (uintBitsToFloat((0x80000000u | 0x3F800000u) | (uint(-v) << 8)) + 1.0f);
}

// 作用：compress_unit_vec 的逆过程。输入：压缩值；输出：归一化向量，
//       输入为哨兵 ~0u 时返回 vec3(C_Stack_Max)（调用方应先判断有效性）。
// 说明：先还原 x、y，再由 L1 范数为常数的关系求 z；z<0 时反折叠 x、y。
vec3 decompress_unit_vec(uint packed)
{
  if(packed != ~0u)  // sanity check, not needed as isvalid_unit_vec is called earlier
  {
    // 步骤 1：拆出 x（低 16 位）与 y（高 16 位）并去掉 32767 偏移。
    int x = int(packed & 0xFFFFu) - 32767;
    int y = int(packed >> 16) - 32767;

    const int maskx = x >> 31;
    const int masky = y >> 31;
    const int tmp0  = 32767 + maskx + masky;
    const int ymask = y ^ masky;
    const int tmp1  = tmp0 - (x ^ maskx);
    // 步骤 2：由 |x|+|y|+|z| = 32767 求 z（带符号掩码的整数运算）。
    const int z     = tmp1 - ymask;
    float     zf;
    if(z < 0)
    {
      x  = (tmp0 - ymask) ^ maskx;
      y  = tmp1 ^ masky;
      zf = uintBitsToFloat((0x80000000u | 0x3F800000u) | (uint(-z) << 8)) + 1.0f;
    }
    else
    {
      zf = uintBitsToFloat(0x3F800000u | (uint(z) << 8)) - 1.0f;
    }

    // 步骤 3：转成 float 后重新归一化，抵消量化误差。
    return normalize(vec3(short_to_floatm11(x), short_to_floatm11(y), zf));
  }
  else
  {
    return vec3(C_Stack_Max);
  }
}


#endif  // COMPRESS_GLSL
