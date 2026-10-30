//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_INSTANCEIO_H
#define FIELD_INSTANCEIO_H

#include <vulkan/vulkan.hpp>
#include <nvvk/resourceallocator_vk.hpp>
#include <nvvk/context_vk.hpp>
#include "structs.h"

class InstanceIO {
public:
    InstanceIO(){};


    std::shared_ptr<nvvk::Buffer>  m_pBufferInstanceLink;
    std::vector<Instance>      instances;
    std::vector<InstanceLink> instanceLinks;


    std::vector<Spectral> spectrals;
    std::vector<Thermal> thermals;
    std::shared_ptr<nvvk::Buffer>  m_pBufferSpectral;
    std::shared_ptr<nvvk::Buffer>  m_pBufferThermal;
};


#endif //FIELD_INSTANCEIO_H
