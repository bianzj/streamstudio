// ==========================================================================
// 文件：RayTracing_Lambert/closestHit.rchit
// 功能：RayTracing 模式主光线的最近命中着色器，只记录命中信息，不做着色。
// 主要模块：main() 把内建变量写入 PtPayload（hitT、图元/实例编号、重心坐标、
//   物体↔世界变换、射线起点）。
// 数据流：traceRayEXT 找到最近交点 → 本着色器填充 prd → raygen（offline.rgen）
//   调用 GetShadeState(prd)（shade_state.glsl）还原位置/法线/材质并着色。
// 与其他文件的关系：PtPayload 定义在 ../settings.h；payload 位置与 offline.rgen 的 prd
//   （location = 0）一致。
// 约定：hitT 为沿射线的距离（世界单位）；lastPosition 为本段射线起点，
//   用于后续计算出射方向 woWorld = position - lastPosition。
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

 //-------------------------------------------------------------------------------------------------
 // The Closest-Hit shader only returns the information of the hit. The shading will be done in 
 // the Ray-Generation shader or Ray-Query (compute)

#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_tracing : require  // This is about ray tracing

#include "../settings.h"
layout(location = 0) rayPayloadInEXT PtPayload prd;
hitAttributeEXT vec2 bary;

// 作用：把最近交点的几何标识保存到 payload，供 raygen 阶段重建着色状态。
// 说明：prd.seed 不在此修改，随机数种子由 raygen 维护。
void main()
{
  //prd.seed;
  prd.hitT                = gl_HitTEXT;
  prd.primitiveID         = gl_PrimitiveID;
  prd.instanceID          = gl_InstanceID;
  prd.instanceCustomIndex = gl_InstanceCustomIndexEXT;
  prd.baryCoord           = bary;
  prd.objectToWorld       = gl_ObjectToWorldEXT;
  prd.worldToObject       = gl_WorldToObjectEXT;
  prd.lastPosition        = gl_WorldRayOriginEXT;
}
