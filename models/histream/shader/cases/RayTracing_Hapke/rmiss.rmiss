// ==========================================================================
// 文件：RayTracing_Hapke/rmiss.rmiss
// 功能：主光线（payload location 0）的 miss 着色器，此处为空实现（Dummy）。
// 主要模块：仅一个空的 main()。
// 数据流：主光线未命中任何几何时调用本着色器，但不修改 payload；
//   offline.rgen 在发射前已把 prd.hitT 置为 INFINITY，因此“未命中”
//   由 hitT == INFINITY 表示（天空/射出场景的处理在 offline.rgen 的 PathTrace 内完成）。
// 与其他文件的关系：与 shadow.rmiss（阴影光线的 miss 着色器）配套，位于 SBT 的 miss 组。
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

//-------------------------------------------------------------------------------------------------
// Dummy - not needed

void main() {}
