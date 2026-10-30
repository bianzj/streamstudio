//
// Created by admin on 2024/1/25.
//

#ifndef FIELD_DESCRIPTOR_H
#define FIELD_DESCRIPTOR_H

#include "voxelrtio.h"

class Descriptor {
public:

    enum VoxelrtbindingInd
    {
        spectral,  // for the specific band
        thermal,  // for the voxel with constant temperature;
        canopy,
        meshLink,
        instanceLink,
        voxelLink,
        nano,
        tlas,
        sensor,
        wave,
        light,
        dir,
        rads,
        netRad,
        storage,
        lad,
        hex,
        waterSet,
    };


    bool createDescriptor(std::shared_ptr<VoxelrtIO> &raytracingio);
    void destroy(std::shared_ptr<VoxelrtIO> &raytracingio);

};


#endif //FIELD_DESCRIPTOR_H
