#pragma once
#include "../physics/energy.h"
#include "../physics/types.h"

#include <array>
#include <filesystem>
#include <string>

namespace stream {

enum class SurfaceKind { Missing, Vegetation, Water, Fill };
enum class GroundKind { Soil, Flat };

struct ClassParameters {
    SurfaceKind surface = SurfaceKind::Missing;
    std::string name;
    Canopy canopy;
    LeafOptics leaf;
    Photosynthesis photosynthesis;
    GroundKind groundKind = GroundKind::Soil;
    float groundReflectance = 0.15f;  // flat spectral reflectance of a non-soil ground
    Ground ground;
};

struct ClassTable {
    std::array<ClassParameters, 256> classes;
    SoilOptics soilOptics;
    float defaultSoilMoisture = 0.25f;
    Aerodynamics aerodynamics;
    double referenceHeight = 10.0, co2 = 380.0, o2 = 209.0;

    const ClassParameters* find(float code) const;
};

ClassTable loadClassTable(const std::filesystem::path& file);

}  // namespace stream
