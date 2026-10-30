// histream-usd: reads any OpenUSD stage and writes HiStream-ready OBJ files plus a JSON manifest.
#include <pxr/pxr.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

constexpr const char* kFormat = "histream-usd-read-v1";
constexpr double kPi = 3.14159265358979323846;
const UsdTimeCode kTime = UsdTimeCode::EarliestTime();

std::string jsonString(const std::string& text)
{
    std::string result = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else if (c < 0x20) { char escaped[8]; std::snprintf(escaped, sizeof escaped, "\\u%04x", c); result += escaped; }
        else result += static_cast<char>(c);
    }
    return result + "\"";
}

std::string jsonNumber(double value)
{
    if (!std::isfinite(value)) return "0";
    char text[32];
    std::snprintf(text, sizeof text, "%.9g", value);
    return text;
}

// histream:* attributes in any namespace (Blender re-exports them as userProperties:histream:*).
// USD files exported before the rename carry streamsim:* and are still read.
using Attributes = std::map<std::string, std::string>;

Attributes histreamAttributes(const UsdPrim& prim)
{
    Attributes result;
    for (const UsdAttribute& attribute : prim.GetAttributes()) {
        const std::string name = attribute.GetName().GetString();
        size_t position = name.find("histream:");
        size_t prefixLength = 9;
        if (position == std::string::npos) {
            position = name.find("streamsim:");
            prefixLength = 10;
        }
        if (position == std::string::npos) continue;
        const std::string key = name.substr(position + prefixLength);
        if (prefixLength == 10 && result.count(key)) continue;
        VtValue value;
        if (!attribute.Get(&value, kTime)) continue;
        if (value.IsHolding<std::string>()) result[key] = jsonString(value.UncheckedGet<std::string>());
        else if (value.IsHolding<TfToken>()) result[key] = jsonString(value.UncheckedGet<TfToken>().GetString());
        else if (value.IsHolding<bool>()) result[key] = value.UncheckedGet<bool>() ? "true" : "false";
        else if (value.IsHolding<int>()) result[key] = jsonNumber(value.UncheckedGet<int>());
        else if (value.IsHolding<float>()) result[key] = jsonNumber(value.UncheckedGet<float>());
        else if (value.IsHolding<double>()) result[key] = jsonNumber(value.UncheckedGet<double>());
        else if (value.IsHolding<GfVec3d>()) {
            const GfVec3d v = value.UncheckedGet<GfVec3d>();
            result[key] = "[" + jsonNumber(v[0]) + "," + jsonNumber(v[1]) + "," + jsonNumber(v[2]) + "]";
        }
        // DEFERRED[低] 错误处理: 其他类型（float3/GfVec3f、int64、double2、数组、asset 路径等）被静默丢弃，DCC 工具常把向量属性写成 float3，导致配置项悄然丢失；jsonNumber 也把 NaN/Inf 静默写成 0。理由：新增类型分支并扩展 skipped 语义属功能扩展，需与下游属性解析一起验证。
    }
    return result;
}

std::string attributesJson(const Attributes& attributes)
{
    std::string result = "{";
    bool first = true;
    for (const auto& [key, value] : attributes) {
        result += (first ? "" : ",") + jsonString(key) + ":" + value;
        first = false;
    }
    return result + "}";
}

std::string stringAttribute(const Attributes& attributes, const std::string& key)
{
    const auto found = attributes.find(key);
    if (found == attributes.end() || found->second.size() < 2 || found->second.front() != '"') return {};
    std::string text = found->second.substr(1, found->second.size() - 2);
    std::string result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const char next = text[++i];
            result += next == 'n' ? '\n' : next == 't' ? '\t' : next == 'r' ? '\r' : next;
        } else result += text[i];
    }
    return result;
}

struct Group {
    std::string name, material;
    Attributes attributes;
    std::vector<GfVec3d> points;
    std::vector<GfVec3f> colors;
    std::vector<int> counts, indices;
};

struct Geometry {
    std::string name;
    Attributes attributes;
    std::vector<Group> groups;
    double minimumY{0.0};
    std::string objFile;
};

struct Placement {
    size_t geometry{};
    std::string path;
    Attributes attributes;
    GfVec3d translation;
    double scale{1.0}, rotation{0.0};
};

std::string boundMaterialName(const UsdPrim& prim)
{
    const UsdShadeMaterial material = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
    return material ? material.GetPrim().GetName().GetString() : std::string();
}

// Points, per-face vertex counts and indices of a mesh-like gprim in its own space.
struct RawMesh {
    VtVec3fArray points;
    VtIntArray counts, indices;
    VtVec3fArray colors;
    bool leftHanded{false};
};

bool readRawMesh(const UsdPrim& prim, RawMesh& raw)
{
    if (const UsdGeomMesh mesh{prim}) {
        mesh.GetPointsAttr().Get(&raw.points, kTime);
        mesh.GetFaceVertexCountsAttr().Get(&raw.counts, kTime);
        mesh.GetFaceVertexIndicesAttr().Get(&raw.indices, kTime);
        TfToken orientation;
        mesh.GetOrientationAttr().Get(&orientation, kTime);
        raw.leftHanded = orientation == UsdGeomTokens->leftHanded;
        const UsdGeomPrimvar color = UsdGeomPrimvarsAPI(prim).GetPrimvar(TfToken("displayColor"));
        if (color && color.GetInterpolation() == UsdGeomTokens->vertex) {
            VtValue value;
            if (color.ComputeFlattened(&value, kTime) && value.IsHolding<VtVec3fArray>())
                raw.colors = value.UncheckedGet<VtVec3fArray>();
            if (raw.colors.size() != raw.points.size()) raw.colors.clear();
        }
        return !raw.points.empty() && !raw.counts.empty();
    }
    if (const UsdGeomCube cube{prim}) {
        double size = 2.0;
        cube.GetSizeAttr().Get(&size, kTime);
        const float h = static_cast<float>(size / 2.0);
        raw.points = {{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h}, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}};
        raw.counts = {4, 4, 4, 4, 4, 4};
        raw.indices = {0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 2, 3, 7, 6, 1, 2, 6, 5, 3, 0, 4, 7};
        return true;
    }
    if (const UsdGeomSphere sphere{prim}) {
        double radius = 1.0;
        sphere.GetRadiusAttr().Get(&radius, kTime);
        constexpr int kSegments = 16, kRings = 10;
        for (int ring = 0; ring <= kRings; ++ring) {
            const double polar = kPi * ring / kRings;
            for (int segment = 0; segment < kSegments; ++segment) {
                const double azimuth = 2.0 * kPi * segment / kSegments;
                raw.points.push_back(GfVec3f(static_cast<float>(radius * std::sin(polar) * std::cos(azimuth)),
                    static_cast<float>(radius * std::cos(polar)), static_cast<float>(radius * std::sin(polar) * std::sin(azimuth))));
            }
        }
        for (int ring = 0; ring < kRings; ++ring) {
            // DEFERRED[低] 数值: 两极处 ring=0 与 ring=kRings 的 16 个点完全重合，生成的首末两圈四边形都退化（三角化后有零面积三角形）；下游若按面积归一化法向或做面元面积加权（FacetRT/体元化）可能出现除零/NaN。理由：改用单顶点三角扇会改变网格拓扑、面数与导出结果，需单独评估。
            for (int segment = 0; segment < kSegments; ++segment) {
                const int a = ring * kSegments + segment, b = ring * kSegments + (segment + 1) % kSegments;
                raw.counts.push_back(4);
                for (const int index : {a, b, b + kSegments, a + kSegments}) raw.indices.push_back(index);
            }
        }
        return true;
    }
    return false;
}

// Splits a gprim by its materialBind GeomSubsets and appends the parts, transformed by toRoot.
void appendGprim(const UsdPrim& prim, const GfMatrix4d& toRoot, std::vector<Group>& groups)
{
    RawMesh raw;
    if (!readRawMesh(prim, raw)) return;
    const Attributes attributes = histreamAttributes(prim);
    std::string baseName = stringAttribute(attributes, "sourceName");
    if (baseName.empty()) baseName = prim.GetName().GetString();

    std::vector<size_t> faceStart(raw.counts.size() + 1, 0);
    for (size_t face = 0; face < raw.counts.size(); ++face) {
        const int count = raw.counts[face];
        if (count < 0 || static_cast<size_t>(count) > raw.indices.size() - faceStart[face]) return;
        faceStart[face + 1] = faceStart[face] + static_cast<size_t>(count);
    }

    std::vector<int> faceGroup(raw.counts.size(), 0);
    std::vector<std::pair<std::string, std::string>> parts{{baseName, boundMaterialName(prim)}};
    if (prim.IsA<UsdGeomMesh>()) {
        const auto subsets = UsdGeomSubset::GetGeomSubsets(UsdGeomImageable(prim), UsdGeomTokens->face, TfToken("materialBind"));
        for (const UsdGeomSubset& subset : subsets) {
            VtIntArray faces;
            subset.GetIndicesAttr().Get(&faces, kTime);
            const int part = static_cast<int>(parts.size());
            parts.push_back({baseName + "_" + subset.GetPrim().GetName().GetString(), boundMaterialName(subset.GetPrim())});
            for (const int face : faces) if (face >= 0 && static_cast<size_t>(face) < faceGroup.size()) faceGroup[face] = part;
        }
    }
    const bool reverse = raw.leftHanded != (toRoot.GetDeterminant3() < 0.0);
    std::vector<int> remap(raw.points.size(), -1);
    for (size_t part = 0; part < parts.size(); ++part) {
        Group group;
        group.name = parts[part].first;
        group.material = parts[part].second;
        group.attributes = attributes;
        std::vector<int> used;
        for (size_t face = 0; face < raw.counts.size(); ++face) {
            if (faceGroup[face] != static_cast<int>(part) || raw.counts[face] < 3) continue;
            std::vector<int> corners;
            for (size_t corner = faceStart[face]; corner < faceStart[face + 1]; ++corner) {
                const int source = raw.indices[corner];
                if (source < 0 || static_cast<size_t>(source) >= raw.points.size()) { corners.clear(); break; }
                if (remap[source] < 0) { remap[source] = static_cast<int>(used.size()); used.push_back(source); }
                corners.push_back(remap[source]);
            }
            if (corners.size() < 3) continue;
            if (reverse) std::reverse(corners.begin() + 1, corners.end());
            group.counts.push_back(static_cast<int>(corners.size()));
            group.indices.insert(group.indices.end(), corners.begin(), corners.end());
        }
        for (const int source : used) {
            group.points.push_back(toRoot.Transform(GfVec3d(raw.points[source])));
            if (!raw.colors.empty()) group.colors.push_back(raw.colors[source]);
            remap[source] = -1;
        }
        if (!group.counts.empty()) groups.push_back(std::move(group));
    }
}

uint64_t geometryHash(const std::vector<Group>& groups)
{
    uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
    for (const Group& group : groups) {
        for (const char c : group.name + "|" + group.material) mix(static_cast<unsigned char>(c));
        for (const int value : group.counts) mix(static_cast<uint64_t>(value));
        for (const int value : group.indices) mix(static_cast<uint64_t>(value));
        for (const GfVec3d& point : group.points)
            for (int axis = 0; axis < 3; ++axis) mix(static_cast<uint64_t>(std::llround(point[axis] * 1e5)));
    }
    return hash;
}

// HiStream placements are translate * rotateY * uniform scale.
bool decompose(const GfMatrix4d& matrix, Placement& placement)
{
    const GfVec3d rows[3] = {GfVec3d(matrix[0][0], matrix[0][1], matrix[0][2]),
                             GfVec3d(matrix[1][0], matrix[1][1], matrix[1][2]),
                             GfVec3d(matrix[2][0], matrix[2][1], matrix[2][2])};
    const double scale = rows[0].GetLength();
    if (!(scale > 1e-9) || matrix.GetDeterminant3() <= 0.0) return false;
    const double tolerance = 1e-4 * scale;
    // DEFERRED[低] Bug: 只检查三行长度相等、X/Z 行水平、Y 行竖直，未检查 rows[0]·rows[2]≈0；水平面内的剪切变换（X、Z 轴不正交但等长）会通过检查并被当作“旋转+均匀缩放”，导出的放置丢失剪切而几何变形。理由：正交性检查需引入新容差常量，且会拒绝此前可导出的剪切变换，属数值/行为变更。
    if (std::abs(rows[1].GetLength() - scale) > tolerance || std::abs(rows[2].GetLength() - scale) > tolerance) return false;
    if (rows[1][1] <= 0.0 || std::abs(rows[0][1]) > tolerance || std::abs(rows[2][1]) > tolerance
        || std::abs(rows[1][0]) > tolerance || std::abs(rows[1][2]) > tolerance) return false;
    // USD rotateY(a) maps +X to (cos a, 0, -sin a); HiStream rotation is -a.
    const double angle = std::atan2(-rows[0][2], rows[0][0]);
    placement.translation = GfVec3d(matrix[3][0], matrix[3][1], matrix[3][2]);
    placement.scale = scale;
    placement.rotation = -angle * 180.0 / kPi;
    return true;
}

UsdPrim objectRoot(const UsdPrim& gprim, const UsdPrim& defaultPrim)
{
    for (UsdPrim prim = gprim.GetParent(); prim && !prim.IsPseudoRoot(); prim = prim.GetParent())
        if (prim.IsInstance()) return prim;
    const UsdPrim parent = gprim.GetParent();
    if (!parent || parent.IsPseudoRoot() || parent == defaultPrim || !parent.IsA<UsdGeomXformable>()) return gprim;
    return parent;
}

std::string objectName(const UsdPrim& root, const Attributes& attributes)
{
    std::string name = stringAttribute(attributes, "objectName");
    if (!name.empty()) return name;
    name = root.GetName().GetString();
    // Duplicated Blender objects carry numeric suffixes (Tree_001).
    const std::string stripped = std::regex_replace(name, std::regex("_[0-9]+$"), "");
    return stripped.empty() ? name : stripped;
}

std::string fileStem(const std::string& name, std::set<std::string>& used)
{
    std::string stem;
    for (const unsigned char c : name) stem += (std::isalnum(c) || c == '_' || c == '-') ? static_cast<char>(c) : '_';
    if (stem.empty()) stem = "object";
    std::string candidate = stem;
    for (int suffix = 1; used.count(candidate); ++suffix) candidate = stem + "_" + std::to_string(suffix);
    used.insert(candidate);
    return candidate;
}

void writeObj(const std::filesystem::path& path, const Geometry& geometry, const std::string& source)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.u8string());
    out << "# Stream Studio import from OpenUSD: " << source << "\n";
    size_t base = 1;
    char line[160];
    for (const Group& group : geometry.groups) {
        std::string name = group.name;
        for (char& c : name) if (std::isspace(static_cast<unsigned char>(c))) c = '_';
        out << "g " << (name.empty() ? "mesh" : name) << "\n";
        for (size_t i = 0; i < group.points.size(); ++i) {
            const GfVec3d& p = group.points[i];
            // DEFERRED[低] 错误处理: 未检查坐标是否有限，USD 中的 NaN/Inf 点（或 minimumY 异常）会以 "nan"/"inf" 写入 OBJ，下游 OBJ 加载器解析失败或产生 NaN 包围盒。理由：剔除/报错非有限点会改变生成的面与包围盒（输出结果），需先确认期望行为。
            if (group.colors.empty())
                std::snprintf(line, sizeof line, "v %.9g %.9g %.9g\n", p[0], p[1] - geometry.minimumY, p[2]);
            else
                std::snprintf(line, sizeof line, "v %.9g %.9g %.9g %.6g %.6g %.6g\n", p[0], p[1] - geometry.minimumY, p[2],
                              group.colors[i][0], group.colors[i][1], group.colors[i][2]);
            out << line;
        }
        size_t corner = 0;
        for (const int count : group.counts) {
            out << 'f';
            for (int k = 0; k < count; ++k) out << ' ' << base + group.indices[corner++];
            out << '\n';
        }
        base += group.points.size();
    }
    if (!out) throw std::runtime_error("cannot write " + path.u8string());
}

int read(const std::string& input, const std::filesystem::path& outputDirectory)
{
    const UsdStageRefPtr stage = UsdStage::Open(input);
    if (!stage) throw std::runtime_error("cannot open USD stage: " + input);
    std::filesystem::create_directories(outputDirectory);

    const TfToken upAxis = UsdGeomGetStageUpAxis(stage);
    const double metersPerUnit = UsdGeomGetStageMetersPerUnit(stage);
    // Row-vector conversion into HiStream axes (Y-up, metres): Z-up (x, y, z) -> (x, z, -y).
    // DEFERRED[低] 物理: Z-up 转换把 USD +Y 映射为 HiStream -Z，而清单把 translation[0]/[2] 分别标为 north/east（scene.cpp 约定 +X 北、+Z 东）；对常见 Z-up 约定（+X 东、+Y 北，如 Blender 顶视图/GIS），导出的场景相对真实方位旋转 90°/镜像方位，太阳方位角与建筑朝向不一致。理由：修改方位映射会改变导出场景朝向，需先明确并文档化轴约定或提供可配置旋转。
    GfMatrix4d convert(1.0);
    if (upAxis == UsdGeomTokens->z) {
        convert.Set(1, 0, 0, 0,
                    0, 0, -1, 0,
                    0, 1, 0, 0,
                    0, 0, 0, 1);
    }
    GfMatrix4d units(1.0);
    units.SetScale(metersPerUnit > 0.0 ? metersPerUnit : 1.0);
    convert = convert * units;
    const GfMatrix4d convertInverse = convert.GetInverse();

    std::string projectJson, layerFormat;
    const VtDictionary layerData = stage->GetRootLayer()->GetCustomLayerData();
    const VtValue* entry = layerData.GetValueAtPath("histream");
    if (!entry) entry = layerData.GetValueAtPath("streamsim");
    if (entry && entry->IsHolding<VtDictionary>()) {
        const VtDictionary& data = entry->UncheckedGet<VtDictionary>();
        if (const auto found = data.find("project"); found != data.end() && found->second.IsHolding<std::string>())
            projectJson = found->second.UncheckedGet<std::string>();
        if (const auto found = data.find("format"); found != data.end() && found->second.IsHolding<std::string>())
            layerFormat = found->second.UncheckedGet<std::string>();
    }

    const UsdPrim defaultPrim = stage->GetDefaultPrim();
    UsdGeomXformCache cache(kTime);
    std::vector<std::string> skipped;
    struct Object { UsdPrim root; std::vector<UsdPrim> gprims; };
    std::vector<Object> objects;
    std::unordered_map<std::string, size_t> objectIndex;

    UsdPrimRange range = stage->Traverse(UsdTraverseInstanceProxies());
    for (auto it = range.begin(); it != range.end(); ++it) {
        const UsdPrim prim = *it;
        const UsdGeomImageable imageable(prim);
        if (imageable) {
            TfToken purpose;
            imageable.GetPurposeAttr().Get(&purpose, kTime);
            if (purpose == UsdGeomTokens->guide || purpose == UsdGeomTokens->proxy) { it.PruneChildren(); continue; }
            TfToken visibility;
            imageable.GetVisibilityAttr().Get(&visibility, kTime);
            if (visibility == UsdGeomTokens->invisible) { it.PruneChildren(); continue; }
        }
        if (!prim.IsA<UsdGeomMesh>() && !prim.IsA<UsdGeomCube>() && !prim.IsA<UsdGeomSphere>()) continue;
        const UsdPrim root = objectRoot(prim, defaultPrim);
        const Attributes rootAttributes = histreamAttributes(root);
        const bool ground = stringAttribute(rootAttributes, "role") == "ground"
            || stringAttribute(histreamAttributes(prim), "role") == "ground"
            || (root == prim && prim.GetParent() == defaultPrim && TfStringToLower(prim.GetName().GetString()) == "ground");
        if (ground) { skipped.push_back(prim.GetPath().GetString() + ": ground surface"); continue; }
        const std::string key = root.GetPath().GetString();
        const auto [found, inserted] = objectIndex.emplace(key, objects.size());
        if (inserted) objects.push_back({root, {}});
        objects[found->second].gprims.push_back(prim);
    }

    std::vector<Geometry> geometries;
    std::vector<Placement> placements;
    std::unordered_map<std::string, size_t> prototypeGeometry;
    std::unordered_map<uint64_t, size_t> hashedGeometry;
    for (const Object& object : objects) {
        const GfMatrix4d rootToWorld = cache.GetLocalToWorldTransform(object.root);
        const GfMatrix4d rootInverse = rootToWorld.GetInverse();
        const Attributes attributes = histreamAttributes(object.root);
        const std::string prototypeKey = object.root.IsInstance()
            ? object.root.GetPrototype().GetPath().GetString() : std::string();

        size_t geometryIndex = geometries.size();
        Geometry built;
        bool reuse = false;
        if (!prototypeKey.empty()) {
            if (const auto found = prototypeGeometry.find(prototypeKey); found != prototypeGeometry.end()) {
                geometryIndex = found->second;
                reuse = true;
            }
        }
        if (!reuse) {
            for (const UsdPrim& gprim : object.gprims)
                appendGprim(gprim, cache.GetLocalToWorldTransform(gprim) * rootInverse * convert, built.groups);
            if (built.groups.empty()) continue;
            if (prototypeKey.empty()) {
                // DEFERRED[中] Bug: 仅凭 64 位哈希判定几何相同，未做逐元素比较；且哈希只包含组名/材质/拓扑/坐标，不包含 gprim 上的 histream:* 属性（如光谱/类型绑定）以及几何对象名，两个网格相同但属性不同的对象会被合并，后者沿用前者的名称与属性（错误材质）。理由：本轮明确禁止改此 64 位哈希去重（需改比较逻辑与性能权衡）。
                const uint64_t hash = geometryHash(built.groups);
                if (const auto found = hashedGeometry.find(hash); found != hashedGeometry.end()) {
                    geometryIndex = found->second;
                    reuse = true;
                } else hashedGeometry[hash] = geometryIndex;
            } else prototypeGeometry[prototypeKey] = geometryIndex;
        }
        if (!reuse) {
            built.name = objectName(object.root, attributes);
            built.attributes = attributes;
            built.minimumY = std::numeric_limits<double>::infinity();
            for (const Group& group : built.groups)
                for (const GfVec3d& point : group.points) built.minimumY = std::min(built.minimumY, point[1]);
            geometries.push_back(std::move(built));
        }

        const GfMatrix4d local = convertInverse * rootToWorld * convert;
        GfMatrix4d grounded(1.0);
        grounded.SetTranslate(GfVec3d(0.0, geometries[geometryIndex].minimumY, 0.0));
        Placement placement;
        placement.path = object.root.GetPath().GetString();
        placement.attributes = attributes;
        if (decompose(grounded * local, placement)) {
            placement.geometry = geometryIndex;
            placements.push_back(std::move(placement));
            continue;
        }
        // Tilted, mirrored or non-uniformly scaled: bake the transform into a dedicated OBJ.
        // DEFERRED[低] 性能: 若该对象的原始几何是本轮刚新建的（非复用），它仍留在 geometries 中并被写成 OBJ、列入清单，但没有任何 placement 引用，产生孤立文件并使场景加载重复解析。理由：改写复用/清理逻辑会改变输出文件集合与清单，需要回归验证。
        Geometry baked;
        baked.name = geometries[geometryIndex].name;
        baked.attributes = geometries[geometryIndex].attributes;
        baked.groups = geometries[geometryIndex].groups;
        GfVec3d minimum(std::numeric_limits<double>::infinity()), maximum(-std::numeric_limits<double>::infinity());
        const bool mirrored = local.GetDeterminant3() < 0.0;
        for (Group& group : baked.groups) {
            for (GfVec3d& point : group.points) {
                point = local.Transform(point);
                for (int axis = 0; axis < 3; ++axis) {
                    minimum[axis] = std::min(minimum[axis], point[axis]);
                    maximum[axis] = std::max(maximum[axis], point[axis]);
                }
            }
            if (mirrored) {
                size_t corner = 0;
                for (const int count : group.counts) {
                    std::reverse(group.indices.begin() + corner + 1, group.indices.begin() + corner + count);
                    corner += count;
                }
            }
        }
        const GfVec3d anchor((minimum[0] + maximum[0]) / 2.0, minimum[1], (minimum[2] + maximum[2]) / 2.0);
        for (Group& group : baked.groups)
            for (GfVec3d& point : group.points) point = GfVec3d(point[0] - anchor[0], point[1], point[2] - anchor[2]);
        baked.minimumY = minimum[1];
        placement.geometry = geometries.size();
        placement.translation = anchor;
        placement.scale = 1.0;
        placement.rotation = 0.0;
        geometries.push_back(std::move(baked));
        placements.push_back(std::move(placement));
    }

    std::set<std::string> usedStems;
    for (Geometry& geometry : geometries) {
        const std::filesystem::path objPath =
            outputDirectory / std::filesystem::u8path(fileStem(geometry.name, usedStems) + ".obj");
        geometry.objFile = objPath.u8string();
        writeObj(objPath, geometry, input);
    }

    std::ostringstream json;
    json << "{\"format\":" << jsonString(kFormat) << ",\"source\":" << jsonString(input)
         << ",\"upAxis\":" << jsonString(upAxis.GetString()) << ",\"metersPerUnit\":" << jsonNumber(metersPerUnit)
         << ",\"histream\":";
    if (projectJson.empty()) json << "null";
    else json << "{\"format\":" << jsonString(layerFormat) << ",\"project\":" << jsonString(projectJson) << "}";
    json << ",\"geometries\":[";
    for (size_t index = 0; index < geometries.size(); ++index) {
        const Geometry& geometry = geometries[index];
        size_t faces = 0;
        json << (index ? "," : "") << "{\"id\":" << index << ",\"name\":" << jsonString(geometry.name)
             << ",\"objFile\":" << jsonString(geometry.objFile) << ",\"attributes\":" << attributesJson(geometry.attributes)
             << ",\"meshes\":[";
        for (size_t g = 0; g < geometry.groups.size(); ++g) {
            const Group& group = geometry.groups[g];
            std::string name = group.name;
            for (char& c : name) if (std::isspace(static_cast<unsigned char>(c))) c = '_';
            json << (g ? "," : "") << "{\"group\":" << jsonString(name.empty() ? "mesh" : name)
                 << ",\"material\":" << jsonString(group.material) << ",\"faces\":" << group.counts.size()
                 << ",\"attributes\":" << attributesJson(group.attributes) << "}";
            faces += group.counts.size();
        }
        json << "],\"faces\":" << faces << "}";
    }
    json << "],\"placements\":[";
    for (size_t index = 0; index < placements.size(); ++index) {
        const Placement& placement = placements[index];
        json << (index ? "," : "") << "{\"geometry\":" << placement.geometry << ",\"path\":" << jsonString(placement.path)
             << ",\"north\":" << jsonNumber(placement.translation[0]) << ",\"y\":" << jsonNumber(placement.translation[1])
             << ",\"east\":" << jsonNumber(placement.translation[2]) << ",\"scale\":" << jsonNumber(placement.scale)
             << ",\"rotation\":" << jsonNumber(placement.rotation)
             << ",\"attributes\":" << attributesJson(placement.attributes) << "}";
    }
    json << "],\"skipped\":[";
    for (size_t index = 0; index < skipped.size(); ++index) json << (index ? "," : "") << jsonString(skipped[index]);
    json << "]}";

    const std::filesystem::path manifest = outputDirectory / "usd-import.json";
    std::ofstream out(manifest, std::ios::binary);
    out << json.str() << '\n';
    if (!out) throw std::runtime_error("cannot write " + manifest.u8string());
    std::cout << "HISTREAM_USD\t" << manifest.u8string() << std::endl;
    return 0;
}

}

// Paths cross the process boundary as UTF-8 (OpenUSD and server.mjs both expect it);
// on Windows the narrow argv is the ANSI code page, so read the wide one instead.
#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    if (argc != 4 || std::wstring(argv[1]) != L"read") {
        std::cerr << "Usage: histream-usd read <scene.usd|usda|usdc|usdz> <output-directory>\n";
        return 2;
    }
    try {
        return read(std::filesystem::path(argv[2]).u8string(), std::filesystem::path(argv[3]));
    } catch (const std::exception& error) {
        std::cerr << "histream-usd: " << error.what() << '\n';
        return 1;
    }
}
#else
int main(int argc, char** argv)
{
    if (argc != 4 || std::string(argv[1]) != "read") {
        std::cerr << "Usage: histream-usd read <scene.usd|usda|usdc|usdz> <output-directory>\n";
        return 2;
    }
    try {
        return read(argv[2], std::filesystem::u8path(argv[3]));
    } catch (const std::exception& error) {
        std::cerr << "histream-usd: " << error.what() << '\n';
        return 1;
    }
}
#endif
