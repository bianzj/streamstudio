//
// Created by admin on 2024/1/31.
//

#ifndef FIELD_SURFIO_H
#define FIELD_SURFIO_H

#include "structs.h"

class SurfIO {

public:
    SurfIO(){};
//    std::vector<SurfL> surfLs;

    std::vector<float> lads;
    std::shared_ptr<nvvk::Buffer>  m_pBufferLad;

};


#endif //FIELD_SURFIO_H
