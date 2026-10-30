//
// Created by admin on 2024/1/25.
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

#include "raytracingio.h"



class Pipeline {

public:
    Pipeline(){};

    bool createPipeline(std::shared_ptr<RaytracingIO> &raytracingio);
    void createShaderBindingTable(std::shared_ptr<RaytracingIO> &raytracingio);
    void destroy(std::shared_ptr<RaytracingIO> &raytracingio);

};


#endif //FIELD_PIPELINE_H
