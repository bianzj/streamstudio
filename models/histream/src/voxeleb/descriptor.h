//
// Created by admin on 2024/1/25.
//

#ifndef FIELD_DESCRIPTOR_H
#define FIELD_DESCRIPTOR_H

#include "voxelebio.h"

class Descriptor {
public:

    enum VoxelebbindingInd
    {
        spectral,  // for the specific band
        fixedSpectral, // for the all band;
        thermal,  // for the voxel with constant temperature;
        tempe, // voxel temperatures
        canopy,
        meshLink,
        instanceLink,
        voxelLink,
        nano,
        tlas,
        sensor,
        wave,
        light,
        atomcond,
        dir,
        rads,
        netRad,
        pnet,
        storage,
        meteo,
        aerocond,
        raa,
        leafBio,
        soilSet,
        rss,
        air,
        flux,
        tLast,
        state,
        lad,
        waterSet,
        fluidVelocityA,
        fluidVelocityB,
        fluidScalarA,
        fluidScalarB,
        fluidLbmA,
        fluidLbmB,
        fluidDensity,
        fluidMeta,
        fluidParameters,
        hex,
    };


    bool createDescriptor(std::shared_ptr<VoxelebIO> &raytracingio);
    void destroy(std::shared_ptr<VoxelebIO> &raytracingio);

};


#endif //FIELD_DESCRIPTOR_H
