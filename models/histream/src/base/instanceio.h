// ==========================================================================
// 文件：instanceio.h
// 功能：InstanceIO —— 实例（Instance）级数据的容器，只存放 CPU 侧数组与对应的 GPU 缓冲句柄，无逻辑。
// 主要内容：
//   1. instances / instanceLinks：场景实例数组及其关联记录（结构见 structs.h）
//   2. spectrals / thermals：光谱、热学参数数组（CPU 侧）
//   3. m_pBufferInstanceLink / m_pBufferSpectral / m_pBufferThermal：对应的 GPU 缓冲
// 数据流：场景构建阶段填充数组 → 各模式 buffer.cpp 把 instanceLinks 等上传为 GPU 存储缓冲
//   → descriptor.cpp 绑定到着色器；accelstruct.cpp 用 instances 构建 TLAS。
// 与其他文件：被 raytracingio.h / voxelrtio.h / voxelebio.h 以 shared_ptr 持有；
//   与 meshio.h（网格级数据）配对使用。
// 注意：本类所有成员均为公开数据；instanceio.cpp 仅包含本头文件。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_INSTANCEIO_H
#define FIELD_INSTANCEIO_H

#include <vulkan/vulkan.hpp>
#include <nvvk/resourceallocator_vk.hpp>
#include <nvvk/context_vk.hpp>
#include "structs.h"

// 类：InstanceIO —— 实例级数据容器（CPU 数组 + GPU 缓冲句柄）。
// 说明：m_pBufferInstanceLink 由各模式 buffer.cpp 用 instanceLinks 创建；
//   voxelrt 的销毁代码会先判空，其他模式直接解引用，调用前需确认已创建。
class InstanceIO {
public:
    InstanceIO(){};


    std::shared_ptr<nvvk::Buffer>  m_pBufferInstanceLink;
    std::vector<Instance>      instances;
    std::vector<InstanceLink> instanceLinks;


    std::vector<Spectral> spectrals;
    std::vector<Thermal> thermals;
    std::shared_ptr<nvvk::Buffer>  m_pBufferSpectral;
    std::shared_ptr<nvvk::Buffer>  m_pBufferThermal;
};


#endif //FIELD_INSTANCEIO_H
