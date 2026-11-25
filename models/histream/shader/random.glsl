// ==========================================================================
// 文件：random.glsl
// 功能：着色器内使用的伪随机数发生器（源自 NVIDIA vk_raytracing 示例，Apache-2.0）。
// 主要模块：
//   1. tea / initRandom：用 Tiny Encryption Algorithm 由（像素, 帧号）生成每像素每帧不同的种子
//   2. pcg / pcg2d / pcg3d：PCG 系列整数哈希，用于推进状态或由坐标生成哈希
//   3. rand / rand2 / Rnd：把 32 位整数状态转换为浮点随机数（rand/rand2 为 [0,1)，Rnd 为 [0,1]，见 Rnd）
// 用法：先用 initRandom()/tea() 得到 seed，再反复调用 rand(seed) 或 Rnd(seed)（均会更新 seed）。
// 注意：rand 与 Rnd 的 uint→float 转换方式不同，同一 seed 上不要混淆期望的取值范围（见 Rnd）。
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
// This file has random functions.
// Usage:
// - initialize the seed using tea(), with a value that is different for each pixel and each frame.
// - use rnd() with the seed

#ifndef RANDOM_GLSL
#define RANDOM_GLSL 1

//-----------------------------------------------------------------------
// Generate a random unsigned int from two unsigned int values, using 16 pairs
// of rounds of the Tiny Encryption Algorithm. See Zafar, Olano, and Curtis,
// "GPU Random Numbers via the Tiny Encryption Algorithm"
//-----------------------------------------------------------------------
uint tea(in uint val0, in uint val1)
{
  uint v0 = val0;
  uint v1 = val1;
  uint s0 = 0;

  for(uint n = 0; n < 16; n++)
  {
    s0 += 0x9e3779b9;
    v0 += ((v1 << 4) + 0xa341316c) ^ (v1 + s0) ^ ((v1 >> 5) + 0xc8013ea4);
    v1 += ((v0 << 4) + 0xad90777d) ^ (v0 + s0) ^ ((v0 >> 5) + 0x7e95761e);
  }

  return v0;
}

// 作用：由屏幕像素坐标和帧号生成随机种子（像素线性编号与 frame 一起做 TEA 混合）。
uint initRandom(in uvec2 resolution, in uvec2 screenCoord, in uint frame)
{
  return tea(screenCoord.y * resolution.x + screenCoord.x, frame);
}


//-----------------------------------------------------------------------
// https://www.pcg-random.org/
//-----------------------------------------------------------------------
uint pcg(inout uint state)
{
  uint prev = state * 747796405u + 2891336453u;
  uint word = ((prev >> ((prev >> 28u) + 4u)) ^ prev) * 277803737u;
  state     = prev;
  return (word >> 22u) ^ word;
}

//-----------------------------------------------------------------------
//-----------------------------------------------------------------------
// PCG 二维/三维哈希（无状态）：输入整数向量，输出混合后的整数向量。
uvec2 pcg2d(uvec2 v)
{
  v = v * 1664525u + 1013904223u;
  v.x += v.y * 1664525u;
  v.y += v.x * 1664525u;
  v = v ^ (v >> 16u);
  v.x += v.y * 1664525u;
  v.y += v.x * 1664525u;
  v = v ^ (v >> 16u);
  return v;
}

uvec3 pcg3d(uvec3 v)
{
  v = v * 1664525u + uvec3(1013904223u);
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v ^= v >> uvec3(16u);
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  return v;
}


//-----------------------------------------------------------------------
// Generate a random float in [0, 1) given the previous RNG state
//-----------------------------------------------------------------------
// 作用：取 PCG 输出的高 23 位作尾数，构造 [1,2) 的浮点数再减 1，得到严格的 [0,1) 均匀随机数。
float rand(inout uint seed)
{
  uint r = pcg(seed);
  return uintBitsToFloat(0x3f800000 | (r >> 9)) - 1.0f;
}

vec2 rand2(inout uint prev)
{
  return vec2(rand(prev), rand(prev));
}

// Rnd(): generate a random float in [0,1] given the previous RNG state (a number).
// recommend to use this function.
// 说明（2026-11-25 核实）：float(0xffffffffu) 在 float 中舍入为 2^32，所以 Rnd 实际是 float(prev)·2^-32；
// float(prev) 对 prev ≥ 2^32−128 会舍入到 2^32，此时返回恰为 1.0。因此取值区间是 [0,1]（闭），
// 不是 [0,1)；概率约 128/2^32 ≈ 3e-8/次。现有调用（samplingHemisphere、圆盘采样、bsdf 的 ±1/π 选择）
// 对 1.0 都无害（sqrt(1-r2)=0、cos/sin 周期、比较判断）；若新增用法为 log(1-u)、int(u·N)（越界）
// 或除以 u，需自行处理 1.0。把它改为 float(prev>>8)·2^-24 可得到严格 [0,1)，但会改变所有随机序列，
// 使全部随机采样结果发生统计性变化，故未改。
float Rnd(inout uint prev)
{
  prev = pcg(prev);
  return (float(prev) * (1.0 / float(0xffffffffu)));
}
#endif  // RANDOM_GLSL
