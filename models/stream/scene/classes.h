// ==========================================================================
// 文件：classes.h
// 功能：地类参数表的数据结构与加载接口（实现见 classes.cpp，数据文件为 classes/classes.json）。
// 主要模块：
//   1. SurfaceKind / GroundKind：地表类别（缺失、植被、水体、填充）与地面类别（土壤、平面）
//   2. ClassParameters：单个地类的冠层、叶片光学、光合、地面、冰雪标记
//   3. ClassTable：256 个地类码的参数数组 + 土壤光学、缺省土壤水分、空气动力学、大气常数
//   4. loadClassTable：从 JSON 文件读入 ClassTable
// 关系：app/image.cpp 用 ClassTable::find 按像元地类码取参数；结构体类型来自 physics/types.h、physics/energy.h。
// 约定：地类码 0–255；表中未定义的码 find() 返回 nullptr；填充值 18 与 255 一律视为填充（见 PORTING.md）。
// ==========================================================================

#pragma once
#include "../physics/energy.h"
#include "../physics/types.h"

#include <array>
#include <filesystem>
#include <string>

namespace stream {

// SurfaceKind：Missing = 表中未定义；Vegetation = 植被/裸地/城市等需要求能量平衡的类；
// Water = 水体（不求能量平衡）；Fill = 填充值。GroundKind：Soil = 土壤（含 Hapke 与含水量发射率）；Flat = 平面地面。
enum class SurfaceKind { Missing, Vegetation, Water, Fill };
enum class GroundKind { Soil, Flat };

// 结构体：单个地类的全部参数（冠层、叶片 PROSPECT、光合、地面、气孔/阻抗等），由 loadClassTable 填充。
// groundReflectance 为非土壤地面的平坦光谱反射率；ice = true 时该类组分温度上限为 273.15 K（融化热汇）。
struct ClassParameters {
    SurfaceKind surface = SurfaceKind::Missing;
    std::string name;
    Canopy canopy;
    LeafOptics leaf;
    Photosynthesis photosynthesis;
    GroundKind groundKind = GroundKind::Soil;
    float groundReflectance = 0.15f;  // flat spectral reflectance of a non-soil ground
    Ground ground;
    bool ice = false;  // ground.surface is snow / ice / glacier: component temperatures are capped at 273.15 K (energy.iceMelt)
    // FIX(2026-11-21 C9): crown / row structure keys found in classes.json for this class (comma separated, empty =
    // none). They are read into `canopy` for the record only: the engine simulates a homogeneous canopy.
    std::string ignoredStructure;
};

// 结构体：地类参数表。classes 按地类码索引；soilOptics 为 BSM 参数；defaultSoilMoisture 为缺省土壤含水量；
// aerodynamics 为空气动力学系数；referenceHeight（m）、co2、o2 为大气常数（缺省来自 classes.json，单位待核对）。
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
