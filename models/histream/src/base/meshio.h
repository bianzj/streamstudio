// ==========================================================================
// 文件：meshio.h
// 功能：MeshIO —— 网格/材质级数据的容器，保存场景中所有网格及其光谱、热学、冠层、叶片生化、
//   土壤、水体、建筑等参数的 CPU 数组、名称索引表和对应的 GPU 缓冲句柄，无逻辑。
// 主要内容：
//   1. GPU 缓冲句柄 m_pBuffer*/m_p*Buffer：Canopy、Spectral、Thermal、MeshLink、LeafBio 等
//   2. CPU 数组：canopies、leafbios、soilsets、watersets、buildups、spectrals、thermals 等
//   3. 网格数组：objMeshes（OBJ 网格）、primMeshes（基本体网格）、meshLinks（网格→参数的索引）
//   4. *Names 映射：名称 → 在对应数组中的下标，场景构建时按名称解析光谱/热学/土壤参数的下标
//   5. 叶片光谱模型参数 fp（FluspectParam）与土壤光谱模型参数 bsm（BSMParam）
// 数据流：compo.cpp 构建光谱/热学/土壤等数组与名称表 → scene.cpp 按名称查下标写入 meshLinks
//   → 各模式 buffer.cpp 上传 GPU → descriptor.cpp 绑定到着色器；
//   accelstruct.cpp 用 m_bufferMeshes 构建 BLAS。
// 与其他文件：被 raytracingio.h / voxelrtio.h / voxelebio.h 以 shared_ptr 持有；
//   与 instanceio.h（实例级数据）配对；结构体定义在 structs.h / structs_cg.h。
// 注意：不同模式只使用其中一部分缓冲，例如 m_pFixedSpectralBuffer、m_pLeafBioBuffer 在
//   voxeleb/buffer.cpp 中创建；其余模式下对应指针保持为空，销毁前须判空。
//   aeroNames 在当前代码中只出现在已注释掉的语句里（疑似遗留成员，待核对）。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_MESHIO_H
#define FIELD_MESHIO_H

#include <vulkan/vulkan.hpp>
#include <nvvk/resourceallocator_vk.hpp>
#include <nvvk/context_vk.hpp>
#include "structs.h"


// 类：MeshIO —— 网格/材质级数据容器（CPU 数组、名称表与 GPU 缓冲句柄）。
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


    // 名称 → 数组下标 的映射：由 compo.cpp 填充，scene.cpp 据此把 XML 中引用的名称解析为 meshLinks 的各类 Id。
    std::map<std::string,int>  spectralNames;
    std::map<std::string,int>  thermalNames;
    std::map<std::string,int> canopyNames;
    std::map<std::string,int> leafbioNames;
    std::map<std::string,int> soilsetNames;
    std::map<std::string,int> watersetNames;
    std::map<std::string,int> aeroNames;

};


#endif //FIELD_MESHIO_H
