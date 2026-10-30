//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_VOXELIO_H
#define FIELD_VOXELIO_H

#include <vulkan/vulkan.hpp>
#include <nvvk/resourceallocator_vk.hpp>
#include <nvvk/context_vk.hpp>
#include "structs.h"



class VoxelIO {
public:
    VoxelIO(){};


    nanovdb::GridHandle<BufferT> nanoHandle;

    std::vector<VoxelLink> voxellinks;
    std::vector<VoxelHex> voxelHexs;
    std::vector<Spectral> spectrals;
    std::vector<Thermal> thermals;



    std::shared_ptr<nvvk::Buffer>  m_pBufferSpectral;
    std::shared_ptr<nvvk::Buffer>  m_pBufferThermal;


    std::shared_ptr<nvvk::Buffer> m_pVoxelLinkBuffer;    // voxel link, point to its instanceID (static property) and voxelID (dynamic property)
    std::shared_ptr<nvvk::Buffer> m_pVoxelNanoBuffer;    // NanoVDB data
    std::shared_ptr<nvvk::Buffer> m_pVoxelHexBuffer;     // 三轴聚集指数和体密度

    std::shared_ptr<nvvk::Buffer> m_pDirBuffer;     // Lst
    std::shared_ptr<nvvk::Buffer> m_pRadsBuffer;
    std::shared_ptr<nvvk::Buffer> m_pNetRadBuffer;
    std::shared_ptr<nvvk::Buffer> m_pPnetBuffer;
    std::shared_ptr<nvvk::Buffer> m_pTempeBuffer;
    std::shared_ptr<nvvk::Buffer> m_pSurfLBuffer;   // surfL


    std::shared_ptr<nvvk::Buffer> m_pLeafBioBuffer; // Bio
    std::shared_ptr<nvvk::Buffer> m_pSoilSetBuffer;
    std::shared_ptr<nvvk::Buffer> m_pRssBuffer;
    std::shared_ptr<nvvk::Buffer> m_pAirBuffer;


    std::shared_ptr<nvvk::Buffer> m_pRaaBuffer;

    std::shared_ptr<nvvk::Buffer> m_pFluxBuffer;    // Evapo
    std::shared_ptr<nvvk::Buffer> m_pTLASTBuffer;


    std::shared_ptr<nvvk::Buffer> m_pStateBuffer;   // budget
    EBState m_state{};

//    std::vector<SurfL> surfLs;

};


#endif //FIELD_VOXELIO_H
