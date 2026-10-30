//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_ACCELSTRUCTURE_H
#define FIELD_ACCELSTRUCTURE_H

#include <nvh/gltfscene.hpp>
#include "nvvk/resourceallocator_vk.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/raytraceKHR_vk.hpp"
#include "meshio.h"
#include "instanceio.h"



class AccelStruct {
public:
    AccelStruct(){};

    nvvk::RaytracingBuilderKHR::BlasInput objectToVkGeometryKHR(const VkDevice device, const MeshBuffer& model);
    bool createAccelStruct(VkDevice device, std::shared_ptr<MeshIO> &meshio, std::shared_ptr<InstanceIO> &instanceio);
    VkAccelerationStructureKHR getTlas() { return m_rtBuilder.getAccelerationStructure(); }

    nvvk::RaytracingBuilderKHR     m_rtBuilder;

};


#endif //FIELD_ACCELSTRUCTURE_H
