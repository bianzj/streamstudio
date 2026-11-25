// ==========================================================================
// 文件：scene.h
// 功能：声明 Scene 类——把工程配置（XML/JSON 解析结果）转换成各模式引擎使用的场景几何：
//       网格（mesh）、实例（instance）及其链接，以及体元模式的 NanoVDB 稀疏体元网格。
// 主要模块：
//   1. createObjScene：RayTracing 模式，背景网格 + OBJ 模型实例（输出到 RaytracingIO）
//   2. createPrimObjScene（VoxelEB / VoxelRT 两个重载）：基元体元场景入口
//   3. createPrimObj_Crown / Crowns / Building / Background：各类基元实体的体元化
//   4. outputObjMesh、XYZ2XZY：调试导出 OBJ，以及 XYZ→XZY（第二轴为高度）的轴序转换
// 数据流：FileIO（解析后的工程）→ Scene::create* → 各模式 IO 对象（meshio / instanceio /
//         voxelio）→ 引擎（Vulkan）上传；实现见 scene.cpp。
// 约定：设计坐标为 XYZ（Z 向上），引擎内为 XZY（第二轴为高度）；长度单位为米，
//       体元坐标为整数格点（步长 stepsize_surface / stepsize_height）。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_SCENE_H
#define FIELD_SCENE_H

#include "src/base/fileio.h"
#include "src/base/meshio.h"
#include "structs.h"
#include "src/base/objloader.h"
#include "src/raytracing/raytracingio.h"
#include "src/voxeleb/voxelebio.h"
#include "src/voxelrt/voxelrtio.h"
#include "src/thirdparty/NanoVDB.h"
#include "src/thirdparty/nanoutil/GridBuilder.h"
#include "src/thirdparty/nanoutil/Primitives.h"
#include "src/base/voxeldesigner.h"



// 类：Scene——无状态的场景构建器（无成员变量），所有结果写入传入的 IO 对象。
class Scene {
public:
    Scene(){};

    // RayTracing：创建背景（平面/网格化/DEM）和 OBJ 模型实例，写入 raytracingio。
    bool createObjScene(std::shared_ptr<FileIO> &fileio, std::shared_ptr<RaytracingIO> &raytracingio);


    // VoxelEB：基元体元场景入口及其按实体类型拆分的子步骤（树冠、形状文件树冠、建筑、背景）。
    bool createPrimObjScene(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &voxellstio);
    bool createPrimObj_Crown(PrimEntity & pe,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelebIO> &modelio, const Background& background);
    bool createPrimObj_Crowns(PrimEntity & pe,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelebIO> &modelio, const Background& background);
    bool createPrimObj_Building(PrimEntity & pe,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelebIO> &modelio, const Background& background);
    bool createPrimObj_Background(Background & background,nanovdb::GridBuilder<int32_t> &nanoBuilder, std::shared_ptr<VoxelebIO> &modelio);



    // VoxelRT：与 VoxelEB 同构的重载，仅 IO 类型不同。
    bool createPrimObjScene(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &voxelrtio);
    bool createPrimObj_Crown(PrimEntity & pe,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelrtIO> &modelio, const Background& background);
    bool createPrimObj_Crowns(PrimEntity & pe,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelrtIO> &modelio, const Background& background);
    bool createPrimObj_Building(PrimEntity & pe,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelrtIO> &modelio, const Background& background);
    bool createPrimObj_Background(Background & background,nanovdb::GridBuilder<int32_t> &nanoBuilder, std::shared_ptr<VoxelrtIO> &modelio);


    // 调试用：把 OBJ 网格写成 Wavefront OBJ 文件。
    void outputObjMesh(ObjMesh model, std::string &fileName);


    // 轴序转换（交换 y、z）；PrimMesh 版本 mark = 1 时保留逐体元的 isValids / faceIds。
    PrimMesh XYZ2XZY(PrimMesh model,int mark=0);
   ObjMesh XYZ2XZY(ObjMesh model);

};


#endif //FIELD_SCENE_H
