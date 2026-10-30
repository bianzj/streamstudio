//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_PIPELINE_H
#define FIELD_PIPELINE_H

#include <nvvk/pipeline_vk.hpp>
#include <nvvk/sbtwrapper_vk.hpp>
#include <nvvk/shadermodulemanager_vk.hpp>
#include <nvvk/shaders_vk.hpp>
#include <nvh/shaderfilemanager.hpp>
#include <nvh/fileoperations.hpp>
#include <nvh/alignment.hpp>
#include <future>

#include "voxelrtio.h"


class Pipeline {
public:
    Pipeline(){};

    bool createRTPipeline(std::shared_ptr<VoxelrtIO> &modelio);
    bool createAeroPipeline(std::shared_ptr<VoxelrtIO> &modelio);
    bool createBioPipeline(std::shared_ptr<VoxelrtIO> &modelio);
    bool createETPipeline(std::shared_ptr<VoxelrtIO> &modelio);
    bool createEBPipeline(std::shared_ptr<VoxelrtIO> &modelio);
    bool createPipeline(std::shared_ptr<VoxelrtIO> &modelio);
    void createShaderBindingTable(std::shared_ptr<VoxelrtIO> &modelio);
    void destroy(std::shared_ptr<VoxelrtIO> &modelio);

};


#endif //FIELD_PIPELINE_H
