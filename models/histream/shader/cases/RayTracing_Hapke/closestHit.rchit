// ==========================================================================
// 文件：cases/RayTracing_Hapke/closestHit.rchit
// 功能：RayTracing_Hapke 用例目录下的 closest-hit 着色器：把命中信息写入光线负载 PtPayload，不做着色。
// 与 RayTracing/closestHit.rchit 的唯一差别：lastPosition 保存的是世界系射线原点
//   （gl_WorldRayOriginEXT），而标准版保存物体系原点（gl_ObjectRayOriginEXT）。
//   shade_state.glsl 会把 lastPosition 当作物体系再乘 objectToWorld（待核对：本用例是否为旧版本）。
// 主要步骤：1. 复制 hitT、图元/实例 id、重心坐标；2. 保存变换矩阵与射线原点。
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

// 命中点只做数据搬运，不含光照计算。
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
