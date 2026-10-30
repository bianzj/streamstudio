#include "classes.h"
#include "raster.h"

#include <cpl_json.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace stream {
namespace {

double number(const CPLJSONObject& object, const char* key, double fallback, const std::string& context)
{
    const auto value = object.GetObj(key);
    if (!value.IsValid()) return fallback;
    const auto type = value.GetType();
    if (type != CPLJSONObject::Type::Integer && type != CPLJSONObject::Type::Long && type != CPLJSONObject::Type::Double)
        throw std::runtime_error("地类参数 " + context + "." + key + " 须为数值");
    const double result = value.ToDouble();
    if (!std::isfinite(result)) throw std::runtime_error("地类参数 " + context + "." + key + " 无效");
    return result;
}

Ground readGround(const CPLJSONObject& object, Ground ground, const std::string& context)
{
    ground.emissivity = static_cast<float>(number(object, "emissivity", ground.emissivity, context));
    ground.conductivity = number(object, "conductivity", ground.conductivity, context);
    ground.volumetricCapacity = number(object, "volumetricCapacity", ground.volumetricCapacity, context);
    const double inertia = number(object, "thermalInertia", 0.0, context);
    if (inertia > 0.0 && !object.GetObj("volumetricCapacity").IsValid())
        ground.volumetricCapacity = inertia * inertia / std::max(ground.conductivity, 1e-6);
    ground.saturatedMoisture = static_cast<float>(number(object, "saturatedMoisture", ground.saturatedMoisture, context));
    ground.heatRoughnessKb = number(object, "heatRoughnessKb", ground.heatRoughnessKb, context);
    if (object.GetObj("surfaceResistance").IsValid()) {
        ground.moistureResistance = false;
        ground.fixedResistance = static_cast<float>(number(object, "surfaceResistance", ground.fixedResistance, context));
    }
    if (!(ground.emissivity > 0 && ground.emissivity <= 1) || !(ground.conductivity > 0) ||
        !(ground.volumetricCapacity > 0) ||
        !(ground.saturatedMoisture > 0 && ground.saturatedMoisture <= 1) || !(ground.fixedResistance > 0))
        throw std::runtime_error("地类参数 " + context + " 的地面参数超出范围");
    return ground;
}

}  // namespace

const ClassParameters* ClassTable::find(float code) const
{
    if (!std::isfinite(code)) return nullptr;
    const long value = std::lround(code);
    if (value < 0 || value > 255) return nullptr;
    const auto& entry = classes[static_cast<size_t>(value)];
    return entry.surface == SurfaceKind::Missing ? nullptr : &entry;
}

ClassTable loadClassTable(const std::filesystem::path& file)
{
    CPLJSONDocument document;
    if (!document.Load(utf8FromPath(file))) throw std::runtime_error("无法读取地类参数表：" + utf8FromPath(file));
    const auto root = document.GetRoot();
    if (root.GetString("kind", "") != "stream-classes" || root.GetInteger("schemaVersion", 0) != 1)
        throw std::runtime_error("地类参数表格式无效：" + utf8FromPath(file));

    ClassTable table;
    const auto soil = root.GetObj("soil");
    const auto optics = soil.GetObj("optics");
    table.soilOptics.brightness = static_cast<float>(number(optics, "brightness", 0.25, "soil.optics"));
    table.soilOptics.latitude = static_cast<float>(number(optics, "latitude", 25, "soil.optics"));
    table.soilOptics.longitude = static_cast<float>(number(optics, "longitude", 45, "soil.optics"));
    table.defaultSoilMoisture = static_cast<float>(number(soil, "defaultMoisture", 0.25, "soil"));
    Ground soilGround;
    soilGround = readGround(soil, soilGround, "soil");
    soilGround.moistureResistance = soil.GetString("resistance", "constant") == "moisture";

    const auto atmosphere = root.GetObj("atmosphere");
    table.co2 = number(atmosphere, "co2", 380, "atmosphere");
    table.o2 = number(atmosphere, "o2", 209, "atmosphere");
    table.referenceHeight = number(atmosphere, "referenceHeight", 10, "atmosphere");
    const auto aero = root.GetObj("aerodynamics");
    table.aerodynamics.cd = static_cast<float>(number(aero, "cd", 0.2, "aerodynamics"));
    table.aerodynamics.cd1 = static_cast<float>(number(aero, "cd1", 20.6, "aerodynamics"));
    table.aerodynamics.cr = number(aero, "cr", 0.35, "aerodynamics");
    table.aerodynamics.psicor = static_cast<float>(number(aero, "psicor", 0.2, "aerodynamics"));
    table.aerodynamics.cssoil = static_cast<float>(number(aero, "cssoil", 0.01, "aerodynamics"));
    table.aerodynamics.soilBoundary = static_cast<float>(number(aero, "soilBoundaryResistance", 10, "aerodynamics"));

    const auto list = root.GetArray("classes");
    for (int index = 0; index < list.Size(); ++index) {
        const auto item = list[index];
        const int code = item.GetInteger("code", -1);
        if (code < 0 || code > 255) throw std::runtime_error("地类参数表中的地类编码须为 0–255");
        const std::string context = "classes[" + std::to_string(code) + "]";
        ClassParameters entry;
        entry.name = item.GetString("name", "地类 " + std::to_string(code));
        const auto surface = item.GetString("surface", "vegetation");
        entry.surface = surface == "water" ? SurfaceKind::Water : surface == "fill" ? SurfaceKind::Fill : SurfaceKind::Vegetation;
        entry.ground = soilGround;
        if (entry.surface != SurfaceKind::Fill) {
            const auto canopy = item.GetObj("canopy");
            entry.canopy.type = code;
            entry.canopy.height = static_cast<float>(number(canopy, "height", 1.0, context));
            entry.canopy.leafWidth = static_cast<float>(number(canopy, "leafWidth", 0.05, context));
            entry.canopy.leafProjection = static_cast<float>(number(canopy, "leafProjection", 0.5, context));
            entry.canopy.hotspot = static_cast<float>(number(canopy, "hotspot", 0.2, context));
            const auto leaf = item.GetObj("leaf");
            entry.leaf.cab = static_cast<float>(number(leaf, "cab", entry.leaf.cab, context));
            entry.leaf.cw = static_cast<float>(number(leaf, "cw", entry.leaf.cw, context));
            entry.leaf.cdm = static_cast<float>(number(leaf, "cdm", entry.leaf.cdm, context));
            entry.leaf.cs = static_cast<float>(number(leaf, "cs", entry.leaf.cs, context));
            entry.leaf.n = static_cast<float>(number(leaf, "n", entry.leaf.n, context));
            entry.leaf.reflTir = static_cast<float>(number(leaf, "reflTir", entry.leaf.reflTir, context));
            entry.leaf.transTir = static_cast<float>(number(leaf, "transTir", entry.leaf.transTir, context));
            if (!(entry.leaf.n > 0)) throw std::runtime_error("地类参数 " + context + ".leaf.n 须大于 0");
            const auto photo = item.GetObj("photosynthesis");
            const auto pathway = photo.GetString("pathway", "none");
            entry.photosynthesis.pathway = pathway == "C3" ? 3 : pathway == "C4" ? 4 : 0;
            entry.photosynthesis.vcmax = static_cast<float>(number(photo, "vcmax", 0, context));
            entry.photosynthesis.ballBerrySlope = static_cast<float>(number(photo, "ballBerrySlope", 9, context));
            entry.photosynthesis.respiration = static_cast<float>(number(photo, "respiration", 0.015, context));
            entry.photosynthesis.stress = static_cast<float>(number(photo, "stress", 1, context));
            entry.photosynthesis.temperatureCorrection = photo.GetBool("temperatureCorrection", false);
            const auto temperature = photo.GetArray("temperature");
            if (temperature.IsValid() && temperature.Size() == 5)
                for (int k = 0; k < 5; ++k) entry.photosynthesis.temperature[k] = static_cast<float>(temperature[k].ToDouble());
            const auto ground = item.GetObj("ground");
            if (ground.IsValid()) {
                entry.ground = readGround(ground, soilGround, context + ".ground");
                if (ground.GetString("surface", "soil") != "soil") {
                    entry.groundKind = GroundKind::Flat;
                    entry.groundReflectance = static_cast<float>(number(ground, "reflectance", 0.15, context + ".ground"));
                    if (!(entry.groundReflectance >= 0 && entry.groundReflectance <= 1))
                        throw std::runtime_error("地类参数 " + context + ".ground.reflectance 须在 0–1 之间");
                }
            }
        }
        table.classes[static_cast<size_t>(code)] = entry;
    }
    return table;
}

}  // namespace stream

