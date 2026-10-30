//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_COMPO_H
#define FIELD_COMPO_H

#include "src/raytracing/raytracingio.h"
#include "src/voxeleb/voxelebio.h"
#include "src/voxelrt/voxelrtio.h"
#include "fileio.h"
#include "src/base/utils.h"

class Compo {

public:
    Compo(){};

    bool createCompOptical(std::shared_ptr<FileIO> &fileio, std::shared_ptr<RaytracingIO> &raytracingio);
    bool createCompProperty(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &voxelebio);
    bool createCompOptical(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &voxelrtio);

    /// fluspect model
    float calctav(float alfa,float nr);
    void fluspect(OptCoeff fluspectCoeff, FluspectParam fluspectParam, std::vector<Spectral>& spectrals);
    // bsm model
    void bsm(OptCoeff bsmCoeff, BSMParam bsm, std::vector<Spectral> &spectral);

    // Resolve every source (model, panel values, imported TXT) onto requested wavelengths.
    std::vector<Spectral> resolveSpectrals(const SpectralXml& material,
                                           const std::vector<float>& outputWavelengths,
                                           const std::vector<float>& customWavelengths,
                                           const OptCoeff& coefficients);

};


#endif //FIELD_COMPO_H
