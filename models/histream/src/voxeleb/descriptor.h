// ==========================================================================
// 文件：descriptor.h
// 功能：VoxelEB（体元能量平衡）管线的描述符集（descriptor set）绑定号枚举与 Descriptor 类声明
// 主要模块：
//   1. VoxelebbindingInd：描述符绑定号枚举，顺序与着色器 shader/bindingLayout.h 中
//      VOXELLST 段的 B_SPECTRAL..B_SOILMEMORY 宏一一对应（B_RADS=15 无描述符，见下）
//   2. Descriptor::createDescriptor / destroy：创建/销毁描述符集布局、池和描述符集
// 数据流：VoxelebIO 持有各 GPU 缓冲 → descriptor.cpp 把它们写入描述符集 → pipeline.cpp 建管线
//         时引用该布局 → command.cpp 录制命令时绑定该描述符集，着色器按绑定号访问
// 与其他文件的关系：依赖 voxelebio.h（VoxelebIO 聚合所有缓冲与 Vulkan 对象）；
//   枚举改动必须同步修改 shader/bindingLayout.h 的 #define 编号
// 注意：体元辐射缓冲 voxelRads 不走描述符，而是经 push constant 的 radianceAddress 物理地址
//   访问，因此枚举里有 rads 项但 createDescriptor 并不添加/写入该绑定
// ==========================================================================

//
// Created by admin on 2024/1/25.
//

#ifndef FIELD_DESCRIPTOR_H
#define FIELD_DESCRIPTOR_H

#include "voxelebio.h"

class Descriptor {
public:

    // 模块：绑定号枚举。枚举值即 GLSL 中 layout(binding = B_xxx) 的编号（从 0 起连续）。
    // 说明：新增/调整顺序时需同步 shader/bindingLayout.h，且 descriptor.cpp 的 addBinding 与
    //       makeWrite 都使用这些枚举值。
    enum VoxelebbindingInd
    {
        // 材质光谱参数（按波段）、材质固定光谱、热学材质、体元温度、冠层参数
        // （对应 B_SPECTRAL / B_FIXEDSPECTRAL / B_THERMAL / B_TEMPE / B_CANOPY）
        spectral,  // for the specific band
        fixedSpectral, // for the all band;
        thermal,  // for the voxel with constant temperature;
        tempe, // voxel temperatures
        canopy,
        // 几何链接与加速结构：网格/实例/体元链接、NanoVDB 体元网格、TLAS
        meshLink,
        instanceLink,
        voxelLink,
        nano,
        tlas,
        // 传感器、波段、光源、大气条件（AtomCond）等辐射输入
        sensor,
        wave,
        light,
        atomcond,
        // 体元方向/辐射/净辐射/Pnet 等中间结果缓冲；注意 rads 不在描述符中（见文件头）
        dir,
        rads,
        netRad,
        pnet,
        // 输出图像存储、气象 Meteo、空气动力学条件、叶片生化/土壤/水体参数及体元热通量等
        storage,
        meteo,
        aerocond,
        raa,
        leafBio,
        soilSet,
        rss,
        air,
        flux,
        tLast,
        state,
        lad,
        waterSet,
        // 流体（LBM）场缓冲：速度/标量/分布函数采用 A/B 双缓冲，另有密度、元数据与参数 UBO
        fluidVelocityA,
        fluidVelocityB,
        fluidScalarA,
        fluidScalarB,
        fluidLbmA,
        fluidLbmB,
        fluidDensity,
        fluidMeta,
        fluidParameters,
        // VoxelHex 体元数据缓冲（具体含义待核对）与土壤记忆（soilMemory，土壤方法 1 的历史项）
        hex,
        soilMemory,
    };


    // 作用：按 VoxelebbindingInd 创建描述符布局/池/集，并把 VoxelebIO 中的缓冲写入描述符集。
    // 输入：已创建好各缓冲和 TLAS 的 VoxelebIO；输出：写入 modelio 的 m_descSetLayout、
    //       m_descPool、m_descSet；返回 true。
    bool createDescriptor(std::shared_ptr<VoxelebIO> &raytracingio);
    // 作用：销毁描述符池与布局，并把句柄复位为空（描述符集随池一起释放）。
    void destroy(std::shared_ptr<VoxelebIO> &raytracingio);

};


#endif //FIELD_DESCRIPTOR_H
