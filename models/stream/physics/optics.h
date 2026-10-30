#pragma once
#include "types.h"

#include <filesystem>
#include <vector>

namespace stream {

OptCoeff readOptCoeff(const std::filesystem::path& file);

// PROSPECT leaf reflectance and transmittance on the 1 nm grid.
void prospect(const OptCoeff& coeff, const LeafOptics& leaf, std::vector<float>& reflectance, std::vector<float>& transmittance);

// BSM soil reflectance for a volumetric soil moisture.
void bsm(const OptCoeff& coeff, const SoilOptics& soil, float moisture, std::vector<float>& reflectance);

float calctav(float alfa, float nr);
float expint(float x);

}  // namespace stream
