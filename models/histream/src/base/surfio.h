// ==========================================================================
// 文件：surfio.h
// 功能：SurfIO——体元/面元类模型共用的“叶面积密度（LAD）”数据容器，仅持有数据与 GPU 缓冲句柄。
// 主要模块：
//   1. SurfIO 类：lads（CPU 侧 LAD 数组）与 m_pBufferLad（对应的 Vulkan 设备缓冲）
// 数据流：场景加载后填充 lads → voxeleb/voxelrt 的 buffer.cpp 上传为 STORAGE 缓冲
//         → descriptor.cpp 以 VK_WHOLE_SIZE 绑定给着色器。
// 关系：被 voxelebio.h、voxelrtio.h 以 m_surfio 持有；类型依赖 structs.h。
// 注意：LAD 的单位与逐元素排布由生产者决定，本文件不做约定（待核对）。
// ==========================================================================

//
// Created by admin on 2024/1/31.
//

#ifndef FIELD_SURFIO_H
#define FIELD_SURFIO_H

#include "structs.h"

// 类：SurfIO。只做数据持有，无任何方法逻辑；缓冲由 buffer.cpp 创建与销毁。
class SurfIO {

public:
    SurfIO(){};
//    std::vector<SurfL> surfLs;

    // CPU 侧叶面积密度数组，上传到 GPU 前的原始数据。
    std::vector<float> lads;
    // LAD 的设备端存储缓冲（未创建时为空，销毁前需判空）。
    std::shared_ptr<nvvk::Buffer>  m_pBufferLad;

};


#endif //FIELD_SURFIO_H
