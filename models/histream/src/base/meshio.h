//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_MESHIO_H
#define FIELD_MESHIO_H

#include <vulkan/vulkan.hpp>
#include <nvvk/resourceallocator_vk.hpp>
#include <nvvk/context_vk.hpp>
#include "structs.h"


class MeshIO {
public:
    MeshIO(){};
    MeshIO(int mode){};

    std::shared_ptr<nvvk::Buffer>  m_pBufferCanopy;
    std::shared_ptr<nvvk::Buffer>  m_pBufferSpectral;
    std::shared_ptr<nvvk::Buffer> m_pFixedSpectralBuffer;
    std::shared_ptr<nvvk::Buffer>  m_pBufferThermal;
    std::vector<MeshBuffer>        m_bufferMeshes;
    std::shared_ptr<nvvk::Buffer>  m_pBufferMeshLink;
//    std::shared_ptr<nvvk::Buffer> m_pCanopyBuffer;
    std::shared_ptr<nvvk::Buffer> m_pLeafBioBuffer; // Bio
    std::shared_ptr<nvvk::Buffer> m_pSoilSetBuffer;
    std::shared_ptr<nvvk::Buffer> m_pWaterSetBuffer;
    std::shared_ptr<nvvk::Buffer> m_pBuildupBuffer;

    std::vector<Canopy> canopies;
    std::vector<LeafBio> leafbios;
    std::vector<SoilSet> soilsets;
    std::vector<WaterSet> watersets;
    std::vector<BuildUp> buildups;
    std::vector<Spectral> spectrals;
    std::vector<Thermal> thermals;
    std::vector<Spectral> fixedSpectrals;


    FluspectParam fp;
    BSMParam bsm;

    std::vector<ObjMesh>      objMeshes;
    std::vector<PrimMesh>     primMeshes;
    //std::vector<MeshBuffer>   meshBuffers;
    std::vector<MeshLink>     meshLinks;
  //  std::vector<Type>  types;  type for each meshes;


    std::map<std::string,int>  spectralNames;
    std::map<std::string,int>  thermalNames;
    std::map<std::string,int> canopyNames;
    std::map<std::string,int> leafbioNames;
    std::map<std::string,int> soilsetNames;
    std::map<std::string,int> watersetNames;
    std::map<std::string,int> aeroNames;

};


#endif //FIELD_MESHIO_H
