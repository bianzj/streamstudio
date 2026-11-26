// ==========================================================================
// 文件：raytracing.h
// 功能：RayTracing 模式总控类 Raytracing 的声明；聚合几何、场景、光学参数、缓冲、描述符、
//       管线、命令等子模块（均以 shared_ptr 持有，构造时创建）。
// 主要模块：
//   1. 生命周期：setup → upload → create → run → destroy
//   2. 输出：output（透视）、outputOrth（正射）、outputAlbedo（平均反射率文本）
//   3. 设置：uploadSetting（XML → RaytracingIO）、updateSetting（→ 推送常量）
// 数据流：运行状态全部保存在 RaytracingIO（raytracingio.h）；本类只持有子模块对象。
// 关系：实现见 raytracing.cpp；由 base/engine.cpp 创建并按模式调用。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_RAYTRACING_H
#define FIELD_RAYTRACING_H
#include <string>
#include <vector>
#include <map>
#include "raytracingio.h"
#include "src/base/structs.h"
#include "src/base/geometry.h"
#include "src/base/compo.h"
#include "src/base/scene.h"
#include "src/raytracing/pipeline.h"
#include "src/base/virtualscreen.h"
#include "src/raytracing/buffer.h"
#include "src/base/accelstruct.h"
#include "src/raytracing/descriptor.h"
#include "src/raytracing/command.h"
#include "src/base/utils.h"

// 类：Raytracing——RayTracing 模式的总控。调用顺序：setup（设备）→ upload（读场景）→
// create（GPU 资源）→ run（逐观测角追踪并输出）→ destroy。
class Raytracing {
public:
    Raytracing(){
        m_pGeometry = std::make_shared<Geometry>();
        m_pScene = std::make_shared<Scene>();
        m_pCompo = std::make_shared<Compo>();

        m_pPipeline = std::make_shared<Pipeline>();
        m_pVirtual  = std::make_shared<VirtualScreen>();
        m_pBuffer = std::make_shared<Buffer>();
        m_pDescriptor = std::make_shared<Descriptor>();
        m_pCommand = std::make_shared<Command>();


    };

    // 生命周期接口（详见 raytracing.cpp 中各函数的说明）。
    bool setup(AppSetting &appsetting, std::shared_ptr<RaytracingIO> &raytracingio);
    bool upload(std::shared_ptr<FileIO> &fileio, std::shared_ptr<RaytracingIO> &raytracingio);
    bool create(std::shared_ptr<RaytracingIO> &raytracingio);
    bool run(std::shared_ptr<RaytracingIO> &raytracingio, std::shared_ptr<FileIO> &fileio);
    bool destroy( std::shared_ptr<RaytracingIO> &raytracingio);
    // 输出接口：kangle 为观测角序号；kpos 为 UAV 航点序号（-1 表示非航点观测）。
    void outputOrth(std::shared_ptr<RaytracingIO> &modelio, std::shared_ptr<FileIO> &fileio, int kangle, int kpos = -1);
    void output(std::shared_ptr<RaytracingIO> &modelio, std::shared_ptr<FileIO> &fileio, int kangle, int kpos = -1);
   // bool recordCommandBuffer();
    void outputAlbedo(std::shared_ptr<RaytracingIO> &modelio, std::shared_ptr<FileIO> &fileio, int kangle);



    // 设置接口：uploadSetting 读 XML 到 RaytracingIO；updateSetting 写推送常量。
    bool uploadSetting(std::shared_ptr<FileIO> &fileio, std::shared_ptr<RaytracingIO> &raytracingio);
    bool updateSetting(std::shared_ptr<RaytracingIO> &raytracingio);

    // 一次完整追踪：常规为 Command::run；自适应采样时为“先导遍 → 读回 σ → 分配 → 细化遍”（见 raytracing.cpp）。
    void trace(std::shared_ptr<RaytracingIO> &raytracingio);

  //  bool defineOPO(std::shared_ptr<RaytracingIO> &raytracingio);

    // 子模块：几何（观测/光源）、场景、光学参数、管线、虚拟屏幕、缓冲、描述符、命令。
    std::shared_ptr<Geometry> m_pGeometry;
    std::shared_ptr<Scene> m_pScene;
    std::shared_ptr<Compo> m_pCompo;
    std::shared_ptr<Pipeline> m_pPipeline;
    std::shared_ptr<VirtualScreen> m_pVirtual;
    std::shared_ptr<Buffer>  m_pBuffer;
    std::shared_ptr<Descriptor> m_pDescriptor;
    std::shared_ptr<Command> m_pCommand;
};


#endif //FIELD_RAYTRACING_H
