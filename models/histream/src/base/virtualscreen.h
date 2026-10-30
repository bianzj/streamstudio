//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_VIRTUALSCREEN_H
#define FIELD_VIRTUALSCREEN_H

#include "src/raytracing/raytracingio.h"
#include "src/voxelrt/voxelrtio.h"
#include "src/voxeleb/voxelebio.h"

class VirtualScreen {
public:
    VirtualScreen(){};

    bool bufferToBuffer(std::shared_ptr<RaytracingIO> & modelio,
                        const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut);

    bool bufferToBuffer(std::shared_ptr<VoxelebIO> & modelio,
                        const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut);

    bool bufferToBuffer(std::shared_ptr<VoxelrtIO> & modelio,
                        const nvvk::Buffer& bufferIn, VkDeviceSize size, const nvvk::Buffer& bufferOut);

};


#endif //FIELD_VIRTUALSCREEN_H
