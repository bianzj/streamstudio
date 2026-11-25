// ==========================================================================
// 文件：voxelio.h
// 功能：VoxelIO——体元类模式（VoxelRT / VoxelEB）共用的体元数据与 GPU 缓冲区容器。
// 主要模块：
//   1. CPU 侧体元数据：NanoVDB 网格句柄、体元链接、体元聚集指数/体密度、光谱与热学参数
//   2. GPU 侧缓冲区指针（nvvk::Buffer）：辐射、净辐射、温度、生物物理、土壤、阻抗、
//      通量、土壤热历史等（各缓冲区的具体结构体见 structs.h，待核对）
//   3. 能量平衡状态 EBState（m_state）与其缓冲区
// 关系：被 voxelrtio.h、voxelebio.h 通过 std::shared_ptr<VoxelIO> 持有；缓冲区在各模式的
//       buffer.cpp 中创建/销毁，在 descriptor.cpp 中绑定描述符；本类本身只存放数据，不含逻辑。
// 注意：部分缓冲区只在 VoxelEB 中创建（如 m_pRaaBuffer、m_pRssBuffer、m_pFluxBuffer、
//       m_pTLASTBuffer），VoxelRT 路径下为空指针（待核对）。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_VOXELIO_H
#define FIELD_VOXELIO_H

#include <vulkan/vulkan.hpp>
#include <nvvk/resourceallocator_vk.hpp>
#include <nvvk/context_vk.hpp>
#include "structs.h"



// 类：VoxelIO——体元模式的数据容器（成员均为公开字段，无方法）。
class VoxelIO {
public:
    VoxelIO(){};


    // CPU 侧数据：NanoVDB 稀疏体元网格句柄（BufferT 见 structs.h），
    // 以及体元链接 / 三轴聚集指数与体密度 / 光谱 / 热学参数数组。
    nanovdb::GridHandle<BufferT> nanoHandle;

    std::vector<VoxelLink> voxellinks;
    std::vector<VoxelHex> voxelHexs;
    std::vector<Spectral> spectrals;
    std::vector<Thermal> thermals;



    std::shared_ptr<nvvk::Buffer>  m_pBufferSpectral;
    std::shared_ptr<nvvk::Buffer>  m_pBufferThermal;


    // GPU 缓冲区（体元几何与索引）：体元链接、NanoVDB 数据、体元 Hex 参数。
    std::shared_ptr<nvvk::Buffer> m_pVoxelLinkBuffer;    // voxel link, point to its instanceID (static property) and voxelID (dynamic property)
    std::shared_ptr<nvvk::Buffer> m_pVoxelNanoBuffer;    // NanoVDB data
    std::shared_ptr<nvvk::Buffer> m_pVoxelHexBuffer;     // 三轴聚集指数和体密度

    // GPU 缓冲区（辐射与温度结果）：方向/辐射/净辐射/Pnet/温度/surfL。
    std::shared_ptr<nvvk::Buffer> m_pDirBuffer;     // Lst
    std::shared_ptr<nvvk::Buffer> m_pRadsBuffer;
    std::shared_ptr<nvvk::Buffer> m_pNetRadBuffer;
    std::shared_ptr<nvvk::Buffer> m_pPnetBuffer;
    std::shared_ptr<nvvk::Buffer> m_pTempeBuffer;
    std::shared_ptr<nvvk::Buffer> m_pSurfLBuffer;   // surfL


    // GPU 缓冲区（能量平衡输入）：叶片生物物理、土壤参数、表面阻抗、空气参数。
    std::shared_ptr<nvvk::Buffer> m_pLeafBioBuffer; // Bio
    std::shared_ptr<nvvk::Buffer> m_pSoilSetBuffer;
    std::shared_ptr<nvvk::Buffer> m_pRssBuffer;
    std::shared_ptr<nvvk::Buffer> m_pAirBuffer;


    std::shared_ptr<nvvk::Buffer> m_pRaaBuffer;

    // GPU 缓冲区（能量平衡输出/状态）：热通量、土壤温度历史（TLAST）等。
    std::shared_ptr<nvvk::Buffer> m_pFluxBuffer;    // Evapo
    std::shared_ptr<nvvk::Buffer> m_pTLASTBuffer;
    // Soil method-1 history modes (vec2 per voxel and mode); one element when unused.
    std::shared_ptr<nvvk::Buffer> m_pSoilMemoryBuffer;


    // 能量平衡迭代状态（EBState）及其 GPU 缓冲区。
    std::shared_ptr<nvvk::Buffer> m_pStateBuffer;   // budget
    EBState m_state{};

//    std::vector<SurfL> surfLs;

};


#endif //FIELD_VOXELIO_H
