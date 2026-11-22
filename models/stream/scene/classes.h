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
    // FIX(2026-11-21 C9): crown / row structure keys found in classes.json for this class (comma separated, empty =
    // none). They are read into `canopy` for the record only: the engine simulates a homogeneous canopy.
    std::string ignoredStructure;
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
