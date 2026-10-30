#pragma once
#include "types.h"

namespace stream {

struct EffectiveLai {
    float hemispherical = 0.0f;
    float solar = 0.0f;
    float viewing = 0.0f;
};

// Direction-specific effective LAI of homogeneous, crown or row canopies. A homogeneous
// canopy (no crown or row structure) returns its own LAI for every direction.
float directionalGap(const Canopy& canopy, const CropRows& rows, float zenithDeg, float azimuthDeg);
float directionalLai(const Canopy& canopy, const CropRows& rows, float zenithDeg, float azimuthDeg);
float hemisphericalLai(const Canopy& canopy, const CropRows& rows);
// Gauss-Legendre gap quadrature mapped to the equivalent homogeneous LAI.
float hemisphericalLaiAnalytical(const Canopy& canopy, const CropRows& rows);

EffectiveLai effectiveLai(const Canopy& canopy, const CropRows& rows, float szaDeg, float saaDeg,
                          float vzaDeg, float vaaDeg, bool analyticHemisphere = false);

}  // namespace stream
