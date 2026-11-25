// ==========================================================================
// 文件：RayTracing_Hapke/shadow.rmiss
// 功能：阴影光线（payload location 1，miss 索引 1）的 miss 着色器。
// 主要模块：main() 把 ShadowHitPayload.isHit 置为 false，表示光线沿途无遮挡。
// 数据流：offline.rgen 的 AnyHit() 发射前先令 isHit = true，并跳过最近命中着色器；
//   只有光线什么也没打中才会进入本着色器，把 isHit 改回 false，
//   于是 isHit 的最终值即“是否被遮挡”。
// 与其他文件的关系：ShadowHitPayload 定义在 ../settings.h；与 rmiss.rmiss（主光线 miss）配套。
// 归档说明：cases/ 下是各方法的历史变体副本，CMake 当前编译的是 shader/RayTracing、voxeleb、voxelrt 等目录；
//   include 用 "../xxx" 相对 shader/<子目录> 书写，故变体需复制到该层级目录后才能编译（shadersCompile.py 用 versionName 选目录，待核对）。
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


#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : enable

#include "../settings.h"

//-------------------------------------------------------------------------------------------------
// This will be executed when sending shadow rays and missing all geometries
// - There are no hit shader for the shadow ray, therefore
// - Before calling Trace, set isHit=true
// - The default anyhit, closesthit won't change isHit, but if nothing is hit, it will be
//   set to false.
//-------------------------------------------------------------------------------------------------

//-----------------------------------------------------------------
// Miss shader for the shadow rayPayloadInEXT
//

// payload 与 offline.rgen 中 shadow_payload（location = 1）对应。
layout(location = 1) rayPayloadInEXT ShadowHitPayload payload;

void main()
{
  // 未命中任何几何 → 光线未被遮挡。
  payload.isHit = false;
}
