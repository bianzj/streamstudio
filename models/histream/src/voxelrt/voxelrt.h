// ==========================================================================
// 文件：voxelrt.h
// 功能：VoxelRT（体元辐射）模式顶层类 Voxelrt 的声明，把场景、几何、组分光学、缓冲、描述符、
//   管线、命令等子模块组织在一起，对外提供 setup / upload / create / run / destroy 流程。
// 主要模块：
//   1. 生命周期接口：setup → upload → create → run → destroy
//   2. 设置接口：uploadSetting（读工程设置）、updateSetting（写 GPU 推送常量）、uploadDefined
//   3. 输出接口：output / outputPos（TIFF 影像）、outputRadiationProcess（三维辐射场）、
//      outputVoxel（已无实现内容）
//   4. 成员：各子模块的 shared_ptr（构造函数中创建）
// 数据流：FileIO（工程 XML）+ AppSetting（Vulkan 上下文）→ VoxelrtIO（运行时状态）→ 子模块 → GPU。
// 与其他文件的关系：实现在 voxelrt.cpp；状态在 voxelrtio.h；管线在 pipeline.h；
//   缓冲/描述符/命令分别在同目录 buffer.h、descriptor.h、command.h。
// 说明：include 路径统一写成与实际目录、文件名一致的小写 "src/voxelrt/..."（此前误写为 Voxelrt/Voxelrtio.h，
//   在大小写敏感的文件系统上会找不到头文件）。
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_Voxelrt_H
#define FIELD_Voxelrt_H

#include <string>
#include <vector>
#include <map>
#include <iomanip>
#include "src/voxelrt/voxelrtio.h"
#include "src/base/structs.h"
#include "src/base/geometry.h"
#include "src/base/compo.h"
#include "src/base/scene.h"
#include "src/voxelrt/pipeline.h"
#include "src/base/virtualscreen.h"
#include "src/voxelrt/buffer.h"
#include "src/base/accelstruct.h"
#include "src/voxelrt/descriptor.h"
#include "src/voxelrt/command.h"
#include "src/base/utils.h"

// 模块：VoxelRT 模式入口类。所有运行时数据保存在 VoxelrtIO，本类只持有各子模块的实例。
class Voxelrt {
public:
    Voxelrt(){
        m_pGeometry = std::make_shared<Geometry>();
        m_pScene = std::make_shared<Scene>();
        m_pCompo = std::make_shared<Compo>();

        m_pPipeline = std::make_shared<Pipeline>();
        m_pVirtual  = std::make_shared<VirtualScreen>();
        m_pBuffer = std::make_shared<Buffer>();
        m_pDescriptor = std::make_shared<Descriptor>();
        m_pCommand = std::make_shared<Command>();
    };


    // 生命周期：setup 初始化 Vulkan 资源 → upload 读工程并构建场景 → create 创建 GPU 资源
    // → run 运行并输出 → destroy 释放。
    bool setup(AppSetting &appsetting, std::shared_ptr<VoxelrtIO> &voxellstio);
    bool upload(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &voxellstio);
    bool create(std::shared_ptr<VoxelrtIO> & modelio);
    bool run(std::shared_ptr<VoxelrtIO> &modelio, std::shared_ptr<FileIO> &fileio);
    bool destroy( std::shared_ptr<VoxelrtIO> &modelio);



    // 设置：uploadSetting 读工程 XML 到 VoxelrtIO；updateSetting 由 VoxelrtIO 生成 GPU 推送常量。
    bool updateSetting(std::shared_ptr<VoxelrtIO> &voxellstio);
    bool uploadSetting(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &voxellstio);

    // 输出：output 为正射/视角序列影像，outputPos 为透视或 UAV 航点影像，
    // outputRadiationProcess 为三维辐射场（voxelrt.bin/json）。
    void output(std::shared_ptr<VoxelrtIO> &modelio, std::shared_ptr<FileIO> &fileio, int knode, int kangle);
    void outputVoxel(std::shared_ptr<VoxelrtIO> &modelio, std::shared_ptr<FileIO> &fileio);
    void outputRadiationProcess(std::shared_ptr<VoxelrtIO> &modelio);
    void outputPos(std::shared_ptr<VoxelrtIO> &modelio, std::shared_ptr<FileIO> &fileio, int knode, int kangle);

    // 子模块：几何/场景/组分光学/管线/虚拟屏幕/缓冲/描述符/命令（均在构造函数中创建）。
    std::shared_ptr<Geometry> m_pGeometry;
    std::shared_ptr<Scene> m_pScene;
    std::shared_ptr<Compo> m_pCompo;
    std::shared_ptr<Pipeline> m_pPipeline;
    std::shared_ptr<VirtualScreen> m_pVirtual;
    std::shared_ptr<Buffer>  m_pBuffer;
    std::shared_ptr<Descriptor> m_pDescriptor;
    std::shared_ptr<Command> m_pCommand;

    bool uploadDefined(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &modelio);
};


#endif //FIELD_Voxelrt_H
