// ==========================================================================
// 文件：classes.cpp
// 功能：读取地类参数表 classes/classes.json（kind = stream-classes，schemaVersion = 1），填充 ClassTable。
// 主要模块：
//   1. number()：带类型与有限性检查的数值字段读取（缺省返回 fallback）
//   2. readGround()：读取地面热/辐射/阻抗参数并校验范围（热惯量可换算体积热容）
//   3. ClassTable::find()：按像元地类码（浮点，四舍五入到 0–255）查表，缺失返回 nullptr
//   4. loadClassTable()：解析土壤光学、大气、空气动力学、各地类的冠层/叶片/光合/地面参数
// 数据流：classes.json → loadClassTable → ClassTable（256 项）→ app/image.cpp 逐像元取参数。
// 关系：类型定义在 classes.h，冠层/叶片/BSM 结构在 physics/types.h，光合/空气动力学/地面在 physics/energy.h。
// 约定：缺省值与 TiRT/TIRTEB read_defined 对齐（见 PORTING.md“地类参数”）；参数错误一律抛 std::runtime_error，
//       消息为中文并指出出错的地类字段路径（如 classes[13].ground）。
// ==========================================================================

#include "classes.h"
#include "raster.h"

#include <cpl_json.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace stream {
namespace {

// 作用：读取 JSON 数值字段。键不存在返回 fallback；类型不是整数/长整/浮点，或值非有限，抛出带 context 的错误。
// 说明：不接受字符串形式的数字，避免静默当作 0。
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

// 作用：在 ground 缺省值基础上读取地面参数（发射率、导热率、体积热容、饱和含水量、kB-1、表面阻抗）并校验。
// 说明：只给 thermalInertia 且没给 volumetricCapacity 时，按热惯量 I = sqrt(λ·C) 换算 C = I²/λ（λ 下限 1e-6）；
//       写了 surfaceResistance 即改用常数阻抗（关闭含水量关系）；发射率须在 (0,1]，其余须为正，否则报错。
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

// 作用：按地类码取参数。非有限、不在 0–255 或表中该项为 Missing 时返回 nullptr（调用方按无效/填充处理）。
const ClassParameters* ClassTable::find(float code) const
{
    if (!std::isfinite(code)) return nullptr;
    const long value = std::lround(code);
    if (value < 0 || value > 255) return nullptr;
    const auto& entry = classes[static_cast<size_t>(value)];
    return entry.surface == SurfaceKind::Missing ? nullptr : &entry;
}

// 作用：读取并校验 classes.json，返回 ClassTable。
// 步骤 1：检查 kind / schemaVersion；2：读取土壤（BSM 光学、缺省含水量、地面参数、阻抗模式）；
// 步骤 3：读取大气（CO2、O2、气象参考高度）与空气动力学系数；4：逐个地类读取名称、类型、冠层、叶片、光合、地面。
ClassTable loadClassTable(const std::filesystem::path& file)
{
    CPLJSONDocument document;
    if (!document.Load(utf8FromPath(file))) throw std::runtime_error("无法读取地类参数表：" + utf8FromPath(file));
    const auto root = document.GetRoot();
    if (root.GetString("kind", "") != "stream-classes" || root.GetInteger("schemaVersion", 0) != 1)
        throw std::runtime_error("地类参数表格式无效：" + utf8FromPath(file));

    ClassTable table;
    // 步骤 2：土壤。BSM 缺省 brightness 0.25、“latitude”25、“longitude”45（BSM 光谱形状参数），缺省含水量 0.25；
    //         resistance = "moisture" 时土壤表面阻抗取含水量关系，否则常数。
    const auto soil = root.GetObj("soil");
    const auto optics = soil.GetObj("optics");
    table.soilOptics.brightness = static_cast<float>(number(optics, "brightness", 0.25, "soil.optics"));
    table.soilOptics.latitude = static_cast<float>(number(optics, "latitude", 25, "soil.optics"));
    table.soilOptics.longitude = static_cast<float>(number(optics, "longitude", 45, "soil.optics"));
    table.defaultSoilMoisture = static_cast<float>(number(soil, "defaultMoisture", 0.25, "soil"));
    Ground soilGround;
    soilGround = readGround(soil, soilGround, "soil");
    soilGround.moistureResistance = soil.GetString("resistance", "constant") == "moisture";

    // 步骤 3：大气成分（CO2 ppm、O2 缺省 380 / 209，单位待核对）、气象参考高度缺省 10 m 与空气动力学系数缺省值。
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

    // 步骤 4：逐个地类。code 须在 0–255；surface = water / fill / 其他（植被）；填充类不读冠层与叶片；
    //         所有地类先继承土壤地面参数，再被各自的 ground 块覆盖。
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
            // 冠层缺省：高度 1 m、叶宽 0.05 m、叶片投影 G = 0.5、热点 0.2。下面记录但不使用的树冠/垄行结构键。
            // FIX(2026-11-21 C9): crown / row structure is not simulated (physics/effective_lai.cpp is not called).
            // Record the keys instead of ignoring them silently; image.cpp warns and writes them to the summary.
            for (const char* key : {"treeStand", "canopyHeight", "crownShape", "crownHeight", "crownRadius", "rows", "cropRows"})
                if (canopy.GetObj(key).IsValid())
                    entry.ignoredStructure += (entry.ignoredStructure.empty() ? "" : ",") + std::string("canopy.") + key;
            for (const char* key : {"rows", "cropRows", "crown", "structure"})
                if (item.GetObj(key).IsValid())
                    entry.ignoredStructure += (entry.ignoredStructure.empty() ? "" : ",") + std::string(key);
            if (canopy.GetObj("treeStand").IsValid()) entry.canopy.treeStand = static_cast<float>(number(canopy, "treeStand", 0.0, context));
            if (canopy.GetObj("canopyHeight").IsValid()) entry.canopy.canopyHeight = static_cast<float>(number(canopy, "canopyHeight", 0.0, context));
            if (canopy.GetObj("crownShape").IsValid()) entry.canopy.crownShape = static_cast<float>(number(canopy, "crownShape", 0.0, context));
            // 叶片 PROSPECT 参数与热红外反/透射率；缺省取 LeafOptics 的内置值。
            const auto leaf = item.GetObj("leaf");
            entry.leaf.cab = static_cast<float>(number(leaf, "cab", entry.leaf.cab, context));
            entry.leaf.cw = static_cast<float>(number(leaf, "cw", entry.leaf.cw, context));
            entry.leaf.cdm = static_cast<float>(number(leaf, "cdm", entry.leaf.cdm, context));
            entry.leaf.cs = static_cast<float>(number(leaf, "cs", entry.leaf.cs, context));
            entry.leaf.n = static_cast<float>(number(leaf, "n", entry.leaf.n, context));
            entry.leaf.reflTir = static_cast<float>(number(leaf, "reflTir", entry.leaf.reflTir, context));
            entry.leaf.transTir = static_cast<float>(number(leaf, "transTir", entry.leaf.transTir, context));
            // PROSPECT is defined for N >= 1 (one compact layer at least); N < 1 gives non-physical R/T.
            if (!(entry.leaf.n >= 1)) throw std::runtime_error("地类参数 " + context + ".leaf.n 须不小于 1（PROSPECT 结构参数）");
            // 光合：pathway C3 → 3，C4 → 4，其他（含 none）→ 0（无气孔光合，叶片阻抗取 625000 s m-1 的无气孔处理，见 PORTING.md）。
            // vcmax 缺省 0、Ball-Berry 斜率 9、暗呼吸/Vcmax 为 0.015、胁迫 1；temperature 数组需恰好 5 个数才覆盖缺省。
            const auto photo = item.GetObj("photosynthesis");
            const auto pathway = photo.GetString("pathway", "none");
            entry.photosynthesis.pathway = pathway == "C3" ? 3 : pathway == "C4" ? 4 : 0;
            entry.photosynthesis.vcmax = static_cast<float>(number(photo, "vcmax", 0, context));
            entry.photosynthesis.ballBerrySlope = static_cast<float>(number(photo, "ballBerrySlope", 9, context));
            entry.photosynthesis.respiration = static_cast<float>(number(photo, "respiration", 0.015, context));
            entry.photosynthesis.stress = static_cast<float>(number(photo, "stress", 1, context));
            // FIX(2026-11-21 B3): default true as TiRT biochemical.py (tcor = 1) and StreamEarth (Tcor = 1); false left
            // Vcmax/Rd/Kc/Ko at their 25 °C values for every class except grassland.
            entry.photosynthesis.temperatureCorrection = photo.GetBool("temperatureCorrection", true);
            const auto temperature = photo.GetArray("temperature");
            if (temperature.IsValid() && temperature.Size() == 5)
                for (int k = 0; k < 5; ++k) entry.photosynthesis.temperature[k] = static_cast<float>(temperature[k].ToDouble());
            // 地面：缺省继承土壤；非 soil 的地面（不透水面、冰雪、水体）按平面地面处理，用常数反射率（缺省 0.15）与常数发射率，
            // 不用含水量相关发射率与 Hapke 土壤表；snow / ice / glacier 同时置 ice = true（组分温度上限 273.15 K）。
            const auto ground = item.GetObj("ground");
            if (ground.IsValid()) {
                entry.ground = readGround(ground, soilGround, context + ".ground");
                // Any non-soil ground (impervious, snow, water) is a flat surface with a constant emissivity: no
                // moisture-dependent emissivity and no Hapke soil table (water: 2026-11-20, used to default to soil).
                const std::string groundSurface = ground.GetString("surface", "soil");
                entry.ice = groundSurface == "snow" || groundSurface == "ice" || groundSurface == "glacier";
                if (groundSurface != "soil") {
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

