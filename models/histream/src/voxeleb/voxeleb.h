// ==========================================================================
// 文件：voxeleb.h
// 功能：VoxelEB 模式（体元能量平衡）的顶层控制类 Voxeleb 声明
// 主要模块：
//   1. 组件成员：几何、场景、成分、管线、虚拟屏幕、缓冲、描述符、命令（各为独立类的共享指针）
//   2. 生命周期接口：setup / upload / create / run / runObserver / destroy
//   3. 输入上传：设置、气溶胶、气象、defined 参数；节点级更新 updateMeteo、applyShortwaveSplit
//   4. 输出接口：output / outputTxt / outputVoxel / outputFluidSlices
// 数据流：FileIO（工程文件）→ upload* → VoxelebIO（CPU 数据与 Vulkan 句柄）
//   → create（缓冲/描述符/管线）→ run（逐时间节点、逐角度求解）→ output*
// 与其他文件的关系：实现在 voxeleb.cpp；全部状态存放于 voxelebio.h 的 VoxelebIO；
//   GPU 资源由 voxeleb/ 目录下的 buffer、descriptor、command、pipeline 管理（待核对）
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_VOXELEB_H
#define FIELD_VOXELEB_H

#include <string>
#include <vector>
#include <map>
#include <iomanip>
#include "src/voxeleb/voxelebio.h"
#include "src/base/structs.h"
#include "src/base/geometry.h"
#include "src/base/compo.h"
#include "src/base/scene.h"
#include "src/voxeleb/pipeline.h"
#include "src/base/virtualscreen.h"
#include "src/voxeleb/buffer.h"
#include "src/base/accelstruct.h"
#include "src/voxeleb/descriptor.h"
#include "src/voxeleb/command.h"
#include "src/base/utils.h"

// 类：VoxelEB 的驱动入口。自身只持有各组件对象，运行状态都保存在 VoxelebIO 中。
class Voxeleb {
public:
    Voxeleb(){
        m_pGeometry = std::make_shared<Geometry>();
        m_pScene = std::make_shared<Scene>();
        m_pCompo = std::make_shared<Compo>();

        m_pPipeline = std::make_shared<Pipeline>();
        m_pVirtual  = std::make_shared<VirtualScreen>();
        m_pBuffer = std::make_shared<Buffer>();
        m_pDescriptor = std::make_shared<Descriptor>();
        m_pCommand = std::make_shared<Command>();
    };


    // 生命周期：setup（初始化 Vulkan 设备等，待核对）→ upload（读入工程数据）→
    //   create（建 GPU 资源）→ run / runObserver（求解）→ destroy（释放）。
    bool setup(AppSetting &appsetting, std::shared_ptr<VoxelebIO> &voxellstio);
    bool upload(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &voxellstio);
    bool create(std::shared_ptr<VoxelebIO> & modelio);
    bool run(std::shared_ptr<VoxelebIO> &modelio, std::shared_ptr<FileIO> &fileio);
    bool runObserver(std::shared_ptr<VoxelebIO> &modelio, std::shared_ptr<FileIO> &fileio,
                     const std::string& manifestPath);
    bool destroy( std::shared_ptr<VoxelebIO> &modelio);



    // 设置与外部输入：运行参数、气溶胶、气象、defined 参数的读入与更新。
    bool updateSetting(std::shared_ptr<VoxelebIO> &voxellstio);
    bool uploadSetting(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &voxellstio);

    bool uploadAero(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &voxellstio);



    bool uploadMeteo(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &modelio);
    bool updateMeteo(std::shared_ptr<VoxelebIO> &modelio, int knode);
    // Per-node direct/diffuse split of Rin (light.diffuseFractionMode); logs
    // SHORTWAVE_SPLIT and, in erbs mode, re-uploads LightSet and atomconds.
    void applyShortwaveSplit(std::shared_ptr<VoxelebIO> &modelio, int knode);

    // 输出：按时间节点/角度（/位置）写结果；体元量、流体切片等另有专用接口。
    void output(std::shared_ptr<VoxelebIO> &modelio, std::shared_ptr<FileIO> &fileio, int knode, int kangle, int kpos = -1);
    void outputTxt(std::shared_ptr<VoxelebIO> &modelio, std::shared_ptr<FileIO> &fileio, int kangle);
    void outputVoxel(std::shared_ptr<VoxelebIO> &modelio, std::shared_ptr<FileIO> &fileio);
    void outputFluidSlices(std::shared_ptr<VoxelebIO> &modelio);


    std::shared_ptr<Geometry> m_pGeometry;
    std::shared_ptr<Scene> m_pScene;
    std::shared_ptr<Compo> m_pCompo;
    std::shared_ptr<Pipeline> m_pPipeline;
    std::shared_ptr<VirtualScreen> m_pVirtual;
    std::shared_ptr<Buffer>  m_pBuffer;
    std::shared_ptr<Descriptor> m_pDescriptor;
    std::shared_ptr<Command> m_pCommand;
    std::vector<Meteo> m_stableMeteos;

    bool uploadDefined(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &modelio);
};


#endif //FIELD_VOXELEB_H
