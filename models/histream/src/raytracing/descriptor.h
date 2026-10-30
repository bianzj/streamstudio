//
// Created by admin on 2024/1/25.
//

#ifndef FIELD_DESCRIPTOR_H
#define FIELD_DESCRIPTOR_H

#include "raytracingio.h"

class Descriptor {
public:

    enum RaytracingbindingInd
    {
        spectral,
        thermal,
        modelLink,
        instanceLink,
        tlas,
        sensor,
        //   waveInd,
        light,
        wave,
        storage,
        textureOffset,
        textureFactor
    };


    bool createDescriptor(std::shared_ptr<RaytracingIO> &raytracingio);
    void destroy(std::shared_ptr<RaytracingIO> &raytracingio);

};


#endif //FIELD_DESCRIPTOR_H
