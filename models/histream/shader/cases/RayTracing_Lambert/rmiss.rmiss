// ==========================================================================
// 文件：RayTracing_Lambert/rmiss.rmiss
// 功能：主光线（payload location 0）的 miss 着色器，此处为空实现（Dummy）。
// 主要模块：仅一个空的 main()。
// 数据流：主光线未命中几何时调用但不改 payload；offline.rgen 发射前已把 prd.hitT 置为 INFINITY，
//   “未命中”由 hitT == INFINITY 表示（射出场景的处理在 offline.rgen 的 PathTrace 内）。
// 与其他文件的关系：与 shadow.rmiss 配套，位于 SBT 的 miss 组；内容与 RayTracing_Hapke 版相同。
// 归档说明：cases/ 为历史变体副本，CMake 当前编译 shader/RayTracing 等目录（待核对）。
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
