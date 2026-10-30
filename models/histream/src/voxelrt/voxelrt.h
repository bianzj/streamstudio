//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_Voxelrt_H
#define FIELD_Voxelrt_H

#include <string>
#include <vector>
#include <map>
#include <iomanip>
#include "src/Voxelrt/Voxelrtio.h"
#include "src/base/structs.h"
#include "src/base/geometry.h"
#include "src/base/compo.h"
#include "src/base/scene.h"
#include "src/Voxelrt/pipeline.h"
#include "src/base/virtualscreen.h"
#include "src/Voxelrt/buffer.h"
#include "src/base/accelstruct.h"
#include "src/Voxelrt/descriptor.h"
#include "src/Voxelrt/command.h"
#include "src/base/utils.h"

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


    bool setup(AppSetting &appsetting, std::shared_ptr<VoxelrtIO> &voxellstio);
    bool upload(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &voxellstio);
    bool create(std::shared_ptr<VoxelrtIO> & modelio);
    bool run(std::shared_ptr<VoxelrtIO> &modelio, std::shared_ptr<FileIO> &fileio);
    bool destroy( std::shared_ptr<VoxelrtIO> &modelio);



    bool updateSetting(std::shared_ptr<VoxelrtIO> &voxellstio);
    bool uploadSetting(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &voxellstio);

    void output(std::shared_ptr<VoxelrtIO> &modelio, std::shared_ptr<FileIO> &fileio, int knode, int kangle);
    void outputVoxel(std::shared_ptr<VoxelrtIO> &modelio, std::shared_ptr<FileIO> &fileio);
    void outputRadiationProcess(std::shared_ptr<VoxelrtIO> &modelio);
    void outputPos(std::shared_ptr<VoxelrtIO> &modelio, std::shared_ptr<FileIO> &fileio, int knode, int kangle);

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
