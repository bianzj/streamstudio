// ==========================================================================
// 文件：queue.h
// 功能：定义 nvvk::Queue，保存 Vulkan 队列句柄及其队列族下标、队列下标。
// 主要模块：
//   1. NVIDIA 许可证声明（Apache-2.0，原作者文件，保持不动）
//   2. nvvk::Queue 结构体
// 与其他文件的关系：被 appsetting.h 包含，AppSetting::m_queues 以及各模式的 IO 类
//   （raytracingio.h、voxelrtio.h、voxelebio.h）持有 std::vector<nvvk::Queue>，
//   buffer.cpp 等通过 m_queues[eGCT] 取用图形/计算/传输队列。
// 注意：familyIndex/queueIndex 缺省为 ~0u，表示未初始化（无效）。
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
 * SPDX-FileCopyrightText: Copyright (c) 2014-2021 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */


#pragma once
#include "vulkan/vulkan_core.h"


namespace nvvk {
	// Holding the queue created at Vulkan context creation, using nvvk::Context
	// 结构体：一个 Vulkan 队列的句柄与索引；两个索引缺省为 ~0u（无效值）。
	struct Queue
	{
		VkQueue  queue{ VK_NULL_HANDLE };
		uint32_t familyIndex{ ~0u };
		uint32_t queueIndex{ ~0u };
	};

}  // namespace nvvk
