// ==========================================================================
// 文件：RayTracing/rmiss.rmiss
// 功能：主光线的 miss 着色器（占位）：什么也不做。光线未命中时 PtPayload.hitT 保持 raygen 预设的值，
//   由 offline.rgen 据此判定为天空/背景。
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
