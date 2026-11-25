// ==========================================================================
// 文件：compo.h
// 功能：声明 Compo 类——把工程配置中的光谱/热属性/冠层等整理进各模式的 meshio，
//       并提供 PROSPECT（fluspect）与 BSM 光谱模型（实现见 compo.cpp）。
// 主要模块：
//   1. createCompOptical / createCompProperty：按模式（RayTracing、VoxelRT、VoxelEB）准备材质属性
//   2. calctav / fluspect：PROSPECT 叶片光谱
//   3. bsm：土壤 BSM 光谱
//   4. resolveSpectrals：任意光谱来源 → 目标波长上的反射率/透射率
// 与其他文件关系：由引擎初始化流程（待核对具体调用点）调用；数据来自 fileio.h 解析的 *Xml，
//       结果写入 raytracingio.h / voxelebio.h / voxelrtio.h 中的 m_meshio。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_COMPO_H
#define FIELD_COMPO_H

#include "src/raytracing/raytracingio.h"
#include "src/voxeleb/voxelebio.h"
#include "src/voxelrt/voxelrtio.h"
#include "fileio.h"
#include "src/base/utils.h"

// 类：Compo。无成员状态，所有函数都是对输入 IO 对象的“填表”操作。
class Compo {

public:
    Compo(){};

    // 材质光谱 + 热属性（RayTracing / VoxelRT 重载，成功返回 true）。
    bool createCompOptical(std::shared_ptr<FileIO> &fileio, std::shared_ptr<RaytracingIO> &raytracingio);
    // VoxelEB 的光谱、热属性、冠层与叶/土/水属性（注意：成功路径当前返回 false）。
    bool createCompProperty(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &voxelebio);
    bool createCompOptical(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &voxelrtio);

    /// fluspect model
    // PROSPECT 的 tav：折射率 nr、入射角 alfa（度）内的平均界面透射率。
    float calctav(float alfa,float nr);
    // PROSPECT 叶片反射率/透射率（N1 个波长点，输出先清空）。
    void fluspect(const OptCoeff& fluspectCoeff, FluspectParam fluspectParam, std::vector<Spectral>& spectrals);
    // bsm model
    // 土壤 BSM 反射率（输出追加在 spectral 末尾，透射率为 0）。
    void bsm(const OptCoeff& bsmCoeff, BSMParam bsm, std::vector<Spectral> &spectral);

    // Resolve every source (model, panel values, imported TXT) onto requested wavelengths.
    std::vector<Spectral> resolveSpectrals(const SpectralXml& material,
                                           const std::vector<float>& outputWavelengths,
                                           const std::vector<float>& customWavelengths,
                                           const OptCoeff& coefficients);

};


#endif //FIELD_COMPO_H
