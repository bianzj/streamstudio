// ==========================================================================
// 文件：defined.h
// 功能：定义 DefinedIO——各模式 IO 对象共用的“预定义数据”容器：
//       程序基目录（其下 defined\ 子目录放预定义数据）、PROSPECT/BSM 系数表，以及若干默认参数对象。
// 主要成员：
//   1. definedDir：程序基目录，预定义数据文件位于其下 defined\ 子目录（见 fileio.cpp）
//   2. m_fluspectCoeff：PROSPECT 与 BSM 的系数表（OptCoeff，按列读入，波长 N1 个点），供 compo.cpp 使用
//   3. m_pAeroBuffer：气溶胶系数的 GPU 缓冲（本目录内未见其他引用，待核对）
//   4. leafbio / soilset / canopy / meta / spectral：默认或模板参数对象（具体用途待核对）
// 与其他文件关系：raytracingio.h、voxelebio.h、voxelrtio.h 各以 shared_ptr<DefinedIO> m_defined
//       持有本对象；fileio.cpp 的 readDefined 从 defined\ 目录读入系数表，compo.cpp 读取。
// ==========================================================================

//
// Created by admin on 2024/1/28.
//

#ifndef FIELD_DEFINED_H
#define FIELD_DEFINED_H

// shader
// coefficient


#include "structs.h"




// 类：DefinedIO。保存预定义系数与默认参数，无逻辑函数。
class DefinedIO {
public:

    DefinedIO(){};

    std::string definedDir = "";
    OptCoeff m_fluspectCoeff;

    std::shared_ptr<nvvk::Buffer> m_pAeroBuffer;    // Aero
//    std::vector<AeroCoeff> aerocoeffs;

    LeafBio leafbio;
    SoilSet soilset;
    Canopy canopy;
    MeteoMeta meta;
    Spectral spectral;

};


#endif //FIELD_DEFINED_H
