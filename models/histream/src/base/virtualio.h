//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_VIRTUALIO_H
#define FIELD_VIRTUALIO_H


#include "structs.h"
#include "structs_cg.h"

class VirtualIO {
public:
    VirtualIO(){};

    std::shared_ptr<nvvk::Buffer> m_pBufferStorage;
};


#endif //FIELD_VIRTUALIO_H
