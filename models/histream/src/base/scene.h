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



class Scene {
public:
    Scene(){};

    bool createObjScene(std::shared_ptr<FileIO> &fileio, std::shared_ptr<RaytracingIO> &raytracingio);


    bool createPrimObjScene(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &voxellstio);
    bool createPrimObj_Crown(PrimEntity & pe,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelebIO> &modelio, const Background& background);
    bool createPrimObj_Crowns(PrimEntity & pe,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelebIO> &modelio, const Background& background);
    bool createPrimObj_Building(PrimEntity & pe,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelebIO> &modelio, const Background& background);
    bool createPrimObj_Background(Background & background,nanovdb::GridBuilder<int32_t> &nanoBuilder, std::shared_ptr<VoxelebIO> &modelio);



    bool createPrimObjScene(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &voxelrtio);
    bool createPrimObj_Crown(PrimEntity & pe,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelrtIO> &modelio, const Background& background);
    bool createPrimObj_Crowns(PrimEntity & pe,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelrtIO> &modelio, const Background& background);
    bool createPrimObj_Building(PrimEntity & pe,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelrtIO> &modelio, const Background& background);
    bool createPrimObj_Background(Background & background,nanovdb::GridBuilder<int32_t> &nanoBuilder, std::shared_ptr<VoxelrtIO> &modelio);


    void outputObjMesh(ObjMesh model, std::string &fileName);


    PrimMesh XYZ2XZY(PrimMesh model,int mark=0);
   ObjMesh XYZ2XZY(ObjMesh model);

};


#endif //FIELD_SCENE_H
