//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_BUFFER_H
#define FIELD_BUFFER_H

#include <nvvk/buffers_vk.hpp>
#include "raytracingio.h"


class Buffer {
public:
    Buffer(){}
    bool createBuffer(std::shared_ptr<RaytracingIO> &raytracingio);
    void destroy(std::shared_ptr<RaytracingIO> &raytracingio);

};


#endif //FIELD_BUFFER_H
