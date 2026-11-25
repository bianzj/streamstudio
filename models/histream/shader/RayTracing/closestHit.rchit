// ==========================================================================
// 文件：RayTracing/closestHit.rchit
// 功能：RayTracing 模式的 closest-hit 着色器：只把命中信息写入光线负载 PtPayload，不做着色。
// 主要步骤：
//   1. 复制命中距离 hitT、图元/实例 id、重心坐标
//   2. 保存 objectToWorld / worldToObject 变换与物体系射线原点 lastPosition
// 数据流：光线求交 → 本文件填充 prd → offline.rgen 中的 GetShadeState（shade_state.glsl）
//   读取 prd 还原几何并完成着色。
// 注意：lastPosition 存的是物体系射线原点（gl_ObjectRayOriginEXT），shade_state.glsl 会再变换到世界系。
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

// 命中点只做数据搬运，不含光照计算（着色在 raygen 阶段统一完成）。
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
  prd.lastPosition        = gl_ObjectRayOriginEXT;
}
