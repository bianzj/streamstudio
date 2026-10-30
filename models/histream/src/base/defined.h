//
// Created by admin on 2024/1/28.
//

#ifndef FIELD_DEFINED_H
#define FIELD_DEFINED_H

// shader
// coefficient


#include "structs.h"




class DefinedIO {
public:

    DefinedIO(){};

    std::string definedDir = "";
    OptCoeff m_fluspectCoeff;

    std::shared_ptr<nvvk::Buffer> m_pAeroBuffer;    // Aero
//    std::vector<AeroCoeff> aerocoeffs;

    LeafBio leafbio;
    SoilSet soilset;
    Canopy canopy;
    MeteoMeta meta;
    Spectral spectral;

};


#endif //FIELD_DEFINED_H
