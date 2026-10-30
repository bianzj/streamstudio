//
// Created by admin on 2024/1/24.
//

#include "scene.h"
#include "hexvoxel.h"
#include "hexneighbors.h"
#include "hexprojection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <gdal_priv.h>


namespace {

struct DemHeightField {
    std::string path;
    int width{0};
    int height{0};
    float minimum{0.0f};
    std::vector<float> values;
    bool valid{false};
};

const DemHeightField& demHeightField(const Background& background)
{
    static DemHeightField cached;
    const std::string requested = background.isDEM ? background.DEMFile : std::string{};
    if (cached.path == requested) return cached;
    cached = {};
    // DEFERRED[中] 错误处理: path 在读取成功前就写入缓存；任一后续步骤抛异常后，下次以同一路径调用会命中缓存直接返回 valid=false，demHeightAboveMinimum 静默返回 0（DEM 被当作平地）；函数内 static 缓存也无锁，并行调用时存在数据竞争；建议：先填充局部对象，成功后再整体赋给缓存，并加互斥锁。
    cached.path = requested;
    if (requested.empty()) return cached;

    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpen(requested.c_str(), GA_ReadOnly));
    if (!dataset || dataset->GetRasterCount() < 1) {
        if (dataset) GDALClose(dataset);
        throw std::runtime_error("Cannot open DEM: " + requested);
    }
    cached.width = dataset->GetRasterXSize();
    cached.height = dataset->GetRasterYSize();
    if (cached.width < 2 || cached.height < 2) {
        GDALClose(dataset);
        throw std::runtime_error("DEM must contain at least 2 x 2 cells: " + requested);
    }
    cached.values.resize(static_cast<size_t>(cached.width) * cached.height);
    GDALRasterBand* band = dataset->GetRasterBand(1);
    int hasNoData = 0;
    const double noData = band->GetNoDataValue(&hasNoData);
    const CPLErr readResult = band->RasterIO(
        GF_Read, 0, 0, cached.width, cached.height, cached.values.data(),
        cached.width, cached.height, GDT_Float32, 0, 0);
    GDALClose(dataset);
    if (readResult != CE_None) throw std::runtime_error("Cannot read DEM elevations: " + requested);

    cached.minimum = std::numeric_limits<float>::infinity();
    for (float value : cached.values) {
        if (!std::isfinite(value) || (hasNoData && value == static_cast<float>(noData))) continue;
        cached.minimum = std::min(cached.minimum, value);
    }
    if (!std::isfinite(cached.minimum)) throw std::runtime_error("DEM contains no valid elevations: " + requested);
    for (float& value : cached.values) {
        if (!std::isfinite(value) || (hasNoData && value == static_cast<float>(noData))) value = cached.minimum;
    }
    cached.valid = true;
    return cached;
}

float demHeightAboveMinimum(const Background& background, float x, float z)
{
    const DemHeightField& dem = demHeightField(background);
    if (!dem.valid) return 0.0f;
    // DEFERRED[中] 物理: 第一个坐标 x 映射到 DEM 行（并翻转）、第二个坐标映射到 DEM 列；若场景 x=东、y=北（SensorXml 注释如此约定），则 DEM 被转置/旋转 90°。另外 DEM 被直接拉伸到 sceneSize，忽略其 GeoTransform 像元大小与原点，sceneSize<1 m 时还被钳到 1；建议：确认轴约定（x→列、y→行翻转），并按 GeoTransform 将世界坐标换算到像元坐标。
    const float u = std::clamp(z / std::max(1.0f, background.sceneSize.y), 0.0f, 1.0f) * (dem.width - 1);
    const float v = (1.0f - std::clamp(
        x / std::max(1.0f, background.sceneSize.x), 0.0f, 1.0f))
        * (dem.height - 1);
    const int x0 = static_cast<int>(std::floor(u));
    const int y0 = static_cast<int>(std::floor(v));
    const int x1 = std::min(dem.width - 1, x0 + 1);
    const int y1 = std::min(dem.height - 1, y0 + 1);
    const auto at = [&dem](int column, int row) {
        return dem.values[static_cast<size_t>(row) * dem.width + column];
    };
    const float top = at(x0, y0) * (1.0f - (u - x0)) + at(x1, y0) * (u - x0);
    const float bottom = at(x0, y1) * (1.0f - (u - x0)) + at(x1, y1) * (u - x0);
    return std::max(0.0f, top * (1.0f - (v - y0)) + bottom * (v - y0) - dem.minimum);
}

bool keepParticipatingMediumVoxel(const Canopy& canopy,
                                  const glm::ivec3& localVoxel,
                                  const Shape& shape,
                                  float stepSize)
{
    // Fog occupies its complete primitive. Fire uses the primitive only as a
    // bounding volume and is narrowed into a deterministic, upward flame.
    if (canopy.structureType != 2) return true;

    const int sizeX = std::max(1, static_cast<int>(std::ceil(shape.length / stepSize)));
    const int sizeY = std::max(1, static_cast<int>(std::ceil(shape.height / stepSize)));
    const int sizeZ = std::max(1, static_cast<int>(std::ceil(shape.width / stepSize)));
    const float x = (static_cast<float>(localVoxel.x) + 0.5f) / sizeX - 0.5f;
    const float y = std::clamp((static_cast<float>(localVoxel.y) + 0.5f) / sizeY, 0.0f, 1.0f);
    const float z = (static_cast<float>(localVoxel.z) + 0.5f) / sizeZ - 0.5f;

    // A small height-dependent displacement makes the flame asymmetric while
    // keeping it reproducible between runs and between VoxelRT/VoxelEB.
    constexpr float pi = 3.14159265358979323846f;
    const float centreX = 0.075f * y * std::sin(2.0f * pi * (1.35f * y + 0.11f));
    const float centreZ = 0.055f * y * std::sin(2.0f * pi * (1.85f * y + 0.37f));
    const float radius = 0.08f + 0.48f * (1.0f - std::pow(y, 0.72f));
    const float edgeNoise = 0.035f * std::sin(
        17.0f * x + 13.0f * z + 9.0f * y + 0.7f);
    const float radialDistance = std::sqrt(
        (x - centreX) * (x - centreX) + (z - centreZ) * (z - centreZ));
    return radialDistance <= radius + edgeNoise;
}

PrimMesh createVoxelDemBackground(const Background& background, float stepSize)
{
    PrimMesh mesh;
    mesh.meshId = 0;
    mesh.nVertices = 0;
    mesh.nIndices = 0;
    // FIXED[低] 数值: 未检查 stepSize>0；为 0 或极小时 sceneSize/stepSize 为 inf/巨大值，static_cast<int> 属未定义行为，columns*rows 分配可能耗尽内存；已修复：入口校验 stepSize 为有限正数，否则抛异常；顶点数/格数上限与共享顶点属性能改写，仍为 DEFERRED。
    if (!(stepSize > 0.0f) || !std::isfinite(stepSize))
        throw std::runtime_error("DEM background step size must be a finite positive value: " +
                                 std::to_string(stepSize));
    const int columns = std::max(1, static_cast<int>(std::floor(background.sceneSize.x / stepSize)));
    const int rows = std::max(1, static_cast<int>(std::floor(background.sceneSize.y / stepSize)));
    std::vector<float> tops(static_cast<size_t>(columns) * rows);
    const auto topAt = [&tops, rows](int column, int row) -> float& {
        return tops[static_cast<size_t>(column) * rows + row];
    };
    for (int column = 0; column < columns; ++column) {
        for (int row = 0; row < rows; ++row) {
            const float x0 = column * stepSize;
            const float x1 = std::min(background.sceneSize.x, (column + 1) * stepSize);
            const float y0 = row * stepSize;
            const float y1 = std::min(background.sceneSize.y, (row + 1) * stepSize);
            topAt(column, row) = std::floor(demHeightAboveMinimum(
                background, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f) / stepSize);
        }
    }
    const auto appendTriangle = [&mesh](const glm::vec3& a,
                                        const glm::vec3& b,
                                        const glm::vec3& c) {
        for (const glm::vec3& point : {a, b, c}) {
            VertexAttribute vertex{};
            vertex.pos = point;
            mesh.vertices.emplace_back(vertex);
            mesh.indices.emplace_back(mesh.nVertices++);
            ++mesh.nIndices;
        }
    };
    const auto appendQuad = [&appendTriangle](const glm::vec3& a,
                                              const glm::vec3& b,
                                              const glm::vec3& c,
                                              const glm::vec3& d) {
        appendTriangle(a, b, c);
        appendTriangle(a, c, d);
    };
    for (int column = 0; column < columns; ++column) {
        for (int row = 0; row < rows; ++row) {
            const float x0 = static_cast<float>(column);
            const float x1 = static_cast<float>(column + 1);
            const float y0 = static_cast<float>(row);
            const float y1 = static_cast<float>(row + 1);
            // Keep the terrain surface and its NanoVDB state in the same cell.
            const float surfaceTop = topAt(column, row);
            appendQuad({x0, y0, surfaceTop}, {x0, y1, surfaceTop},
                       {x1, y1, surfaceTop}, {x1, y0, surfaceTop});

            // Close every height transition. This avoids open terrain cracks
            // while retaining the original stable one-cell/one-state mapping.
            if (column + 1 < columns) {
                const float neighborTop = topAt(column + 1, row);
                if (surfaceTop > neighborTop) {
                    appendQuad({x1, y0, neighborTop}, {x1, y1, neighborTop},
                               {x1, y1, surfaceTop}, {x1, y0, surfaceTop});
                } else if (neighborTop > surfaceTop) {
                    appendQuad({x1, y0, surfaceTop}, {x1, y0, neighborTop},
                               {x1, y1, neighborTop}, {x1, y1, surfaceTop});
                }
            }
            if (row + 1 < rows) {
                const float neighborTop = topAt(column, row + 1);
                if (surfaceTop > neighborTop) {
                    appendQuad({x0, y1, neighborTop}, {x0, y1, surfaceTop},
                               {x1, y1, surfaceTop}, {x1, y1, neighborTop});
                } else if (neighborTop > surfaceTop) {
                    appendQuad({x0, y1, surfaceTop}, {x1, y1, surfaceTop},
                               {x1, y1, neighborTop}, {x0, y1, neighborTop});
                }
            }
            mesh.voxelIds.emplace_back(column, row, surfaceTop - 1.0f);
        }
    }
    return mesh;
}

struct ObjVoxelCoord {
    int x;
    int y;
    int z;

    bool operator==(const ObjVoxelCoord& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct ObjVoxelCoordHash {
    size_t operator()(const ObjVoxelCoord& value) const noexcept {
        size_t seed = std::hash<int>{}(value.x);
        seed ^= std::hash<int>{}(value.y) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<int>{}(value.z) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

std::vector<glm::vec3> clipVoxelPolygon(const std::vector<glm::vec3>& source,
                                        int axis, float plane, bool keepGreater)
{
    std::vector<glm::vec3> result;
    if (source.empty()) return result;

    auto inside = [axis, plane, keepGreater](const glm::vec3& point) {
        return keepGreater ? point[axis] >= plane - 1.0e-6f
                           : point[axis] <= plane + 1.0e-6f;
    };

    glm::vec3 previous = source.back();
    bool previousInside = inside(previous);
    for (const glm::vec3& current : source) {
        const bool currentInside = inside(current);
        if (currentInside != previousInside) {
            const float denominator = current[axis] - previous[axis];
            if (std::abs(denominator) > 1.0e-8f) {
                const float t = std::clamp((plane - previous[axis]) / denominator,
                                           0.0f, 1.0f);
                result.emplace_back(previous + t * (current - previous));
            }
        }
        if (currentInside) result.emplace_back(current);
        previous = current;
        previousInside = currentInside;
    }
    return result;
}

float triangleAreaInsideVoxel(const glm::vec3& a, const glm::vec3& b,
                              const glm::vec3& c, const ObjVoxelCoord& voxel)
{
    std::vector<glm::vec3> polygon{a, b, c};
    const int lower[3] = {voxel.x, voxel.y, voxel.z};
    for (int axis = 0; axis < 3 && polygon.size() >= 3; ++axis) {
        polygon = clipVoxelPolygon(polygon, axis, static_cast<float>(lower[axis]), true);
        polygon = clipVoxelPolygon(polygon, axis,
                                   static_cast<float>(lower[axis] + 1), false);
    }
    if (polygon.size() < 3) return 0.0f;

    float area = 0.0f;
    for (size_t index = 1; index + 1 < polygon.size(); ++index) {
        area += 0.5f * glm::length(glm::cross(
            polygon[index] - polygon[0], polygon[index + 1] - polygon[0]));
    }
    return area;
}

std::vector<glm::ivec3> voxelizeObjSurface(const ObjMesh& mesh, float voxelSize,
                                           float fillThreshold,
                                           size_t& rejectedVoxelCount,
                                           std::vector<float>* leafDensities = nullptr,
                                           std::vector<glm::vec3>* surfaceNormals = nullptr)
{
    if (!(voxelSize > 0.0f)) {
        throw std::runtime_error("OBJ voxelization requires voxelSize > 0");
    }

    std::unordered_map<ObjVoxelCoord, float, ObjVoxelCoordHash> coveredArea;
    std::unordered_map<ObjVoxelCoord, glm::vec3, ObjVoxelCoordHash> weightedNormal;
    const float inverseVoxelSize = 1.0f / voxelSize;
    const size_t triangleCount = mesh.indices.size() / 3;

    for (size_t triangle = 0; triangle < triangleCount; ++triangle) {
        const uint32_t ia = mesh.indices[triangle * 3 + 0];
        const uint32_t ib = mesh.indices[triangle * 3 + 1];
        const uint32_t ic = mesh.indices[triangle * 3 + 2];
        if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() ||
            ic >= mesh.vertices.size()) {
            continue;
        }

        const glm::vec3 a = mesh.vertices[ia].pos * inverseVoxelSize;
        const glm::vec3 b = mesh.vertices[ib].pos * inverseVoxelSize;
        const glm::vec3 c = mesh.vertices[ic].pos * inverseVoxelSize;
        const glm::vec3 faceCross = glm::cross(b - a, c - a);
        if (glm::length(faceCross) <= 1.0e-10f) continue;
        const glm::vec3 faceNormal = glm::normalize(faceCross);

        const glm::vec3 minimum = glm::min(a, glm::min(b, c));
        const glm::vec3 maximum = glm::max(a, glm::max(b, c));
        const int minX = static_cast<int>(std::floor(minimum.x));
        const int minY = static_cast<int>(std::floor(minimum.y));
        const int minZ = static_cast<int>(std::floor(minimum.z));
        const int maxX = static_cast<int>(std::floor(maximum.x));
        const int maxY = static_cast<int>(std::floor(maximum.y));
        const int maxZ = static_cast<int>(std::floor(maximum.z));

        // DEFERRED[中] 性能: 对每个三角形遍历其完整 3D 包围盒并对每格做 6 次多边形裁剪（每次分配 std::vector）；大而倾斜的三角形（如数百米的地面/屋顶，体元 0.1–0.5 m）包围盒体积是表面格数的 O(n) 倍，体元化可能耗时数小时；坐标极大时 floor→int 也会溢出；建议：先做三角形-盒 SAT 测试剔除，或按平面逐层扫描，裁剪用固定容量数组。
        for (int x = minX; x <= maxX; ++x) {
            for (int y = minY; y <= maxY; ++y) {
                for (int z = minZ; z <= maxZ; ++z) {
                    const ObjVoxelCoord key{x, y, z};
                    const float area = triangleAreaInsideVoxel(a, b, c, key);
                    if (area > 1.0e-8f) {
                        coveredArea[key] += area;
                        if (surfaceNormals) weightedNormal[key] += area * faceNormal;
                    }
                }
            }
        }
    }

    std::vector<glm::ivec3> activeVoxels;
    activeVoxels.reserve(coveredArea.size());
    rejectedVoxelCount = 0;
    const float threshold = std::clamp(fillThreshold, 0.0f, 1.0f);
    for (const auto& entry : coveredArea) {
        const float fillRatio = std::min(1.0f, entry.second);
        if (fillRatio + 1.0e-6f < threshold) {
            ++rejectedVoxelCount;
            continue;
        }
        activeVoxels.emplace_back(entry.first.x, entry.first.y, entry.first.z);
    }

    std::sort(activeVoxels.begin(), activeVoxels.end(),
              [](const glm::ivec3& left, const glm::ivec3& right) {
                  if (left.x != right.x) return left.x < right.x;
                  if (left.y != right.y) return left.y < right.y;
                  return left.z < right.z;
              });
    if (leafDensities) {
        // coveredArea is in voxel-face units: area / voxelSize^2.
        leafDensities->clear();
        leafDensities->reserve(activeVoxels.size());
        for (const glm::ivec3& voxel : activeVoxels) {
            leafDensities->push_back(
                coveredArea[ObjVoxelCoord{voxel.x, voxel.y, voxel.z}] / voxelSize);
        }
    }
    if (surfaceNormals) {
        // Facets whose normals cancel (two-sided sheets) leave a zero vector,
        // which the shaders treat as a horizontal surface.
        surfaceNormals->clear();
        surfaceNormals->reserve(activeVoxels.size());
        for (const glm::ivec3& voxel : activeVoxels) {
            const ObjVoxelCoord key{voxel.x, voxel.y, voxel.z};
            const glm::vec3 sum = weightedNormal[key];
            const float length = glm::length(sum);
            surfaceNormals->push_back(length > 0.25f * coveredArea[key]
                ? sum / length : glm::vec3(0.0f));
        }
    }
    return activeVoxels;
}

template <typename MapType>
int requiredId(const MapType& values, const std::string& name, const char* kind)
{
    const auto found = values.find(name);
    if (found == values.end())
        throw std::runtime_error(std::string("Unknown ") + kind + " name: '" + name + "'");
    return found->second;
}

// Optional bindings (thermal, canopy, material) keep id 0 when the name is
// unknown, but no longer silently.
template <typename MapType>
int mappedId(const MapType& values, const std::vector<std::string>& names,
             size_t index, const char* kind)
{
    if (names.empty() || values.empty()) return 0;
    const std::string& name = names[std::min(index, names.size() - 1)];
    const auto found = values.find(name);
    if (found != values.end()) return found->second;
    // Placeholder tables (e.g. VoxelEB without thermal entries) are not configurable.
    if (values.size() == 1 && values.begin()->first.rfind("__default_", 0) == 0)
        return values.begin()->second;
    static std::set<std::string> reported;
    if (reported.insert(std::string(kind) + '\n' + name).second) {
        std::cerr << "Warning: unknown " << kind << " name '" << name
                  << "', using the first " << kind << " entry" << std::endl;
    }
    return 0;
}

// Spectral bindings are required, as in RayTracing.
template <typename MapType>
int requiredMappedId(const MapType& values, const std::vector<std::string>& names,
                     size_t index, const char* kind)
{
    if (names.empty()) throw std::runtime_error(std::string("Missing ") + kind + " binding");
    return requiredId(values, names[std::min(index, names.size() - 1)], kind);
}

template <typename MapType>
int idOrZero(const MapType& values, const std::string& name)
{
    const auto found = values.find(name);
    return found == values.end() ? 0 : found->second;
}

// A primitive building always creates wall and roof meshes. A single schema
// binding applies to both; the optional second binding overrides the roof.
void preparePrimitiveBuilding(PrimEntity& entity)
{
    if (entity.meshNames.empty() || entity.meshNames.size() > 2)
        throw std::runtime_error("Primitive building requires one binding or a wall/roof pair: " + entity.primitiveName);
    const auto expand = [&entity](std::vector<std::string>& names, const char* field) {
        if (names.empty() || names.size() > 2)
            throw std::runtime_error(std::string("Invalid primitive building ") + field + ": " + entity.primitiveName);
        if (names.size() == 1) names.push_back(names.front());
    };
    expand(entity.meshNames, "mesh bindings");
    expand(entity.spectralNames, "spectral bindings");
    expand(entity.canopyNames, "canopy bindings");
    expand(entity.propNames, "material bindings");

    if (entity.isdisFromFile) {
        size_t skippedRows = 0;
        const auto rows = Utils::readNumericRows(entity.distributefile, "building position file", skippedRows);
        if (skippedRows > 0)
            throw std::runtime_error("Invalid building position row: " + entity.distributefile);
        entity.primDistributions.clear();
        entity.scales.clear();
        entity.rotations.clear();
        for (const auto& values : rows) {
            if (values.size() < 3 || values.size() > 5)
                throw std::runtime_error("Invalid building position row: " + entity.distributefile);
            const float scale = values.size() > 3 ? values[3] : 1.0f;
            const float rotation = values.size() > 4 ? values[4] : 0.0f;
            if (!(scale > 0.0f)) throw std::runtime_error("Building instance scale must be positive: " + entity.distributefile);
            entity.primDistributions.emplace_back(values[0], values[1], values[2]);
            entity.scales.push_back(scale);
            entity.rotations.push_back(rotation);
        }
        if (entity.primDistributions.empty())
            throw std::runtime_error("Building position file contains no instances: " + entity.distributefile);
    }
    entity.scales.resize(entity.primDistributions.size(), 1.0f);
    entity.rotations.resize(entity.primDistributions.size(), 0.0f);
}

struct RotatedHexContribution {
    std::array<float, 3> projectedArea{{0.0f, 0.0f, 0.0f}};
    std::array<std::vector<std::uint64_t>, 3> coverMask;
    float rho{0.0f};
    int sampleN{0};
};

struct HexCategoryAccum {
    int type{0};
    int spectralId{0};
    MeshLink prototype{};
    std::array<double, 3> projectedArea{{0.0, 0.0, 0.0}};
    std::array<std::vector<std::uint64_t>, 3> coverMask;
    double rho{0.0};
};

struct HexWorldAccum {
    ObjVoxelCoord coord{};
    int sampleN{0};
    std::vector<int> linkIndices;
    std::array<std::vector<std::uint64_t>, 3> unionMask;
    std::vector<HexCategoryAccum> categories;
};

struct HexMixAccumulator {
    std::unordered_map<ObjVoxelCoord, HexWorldAccum, ObjVoxelCoordHash> cells;
};

void orMask(std::vector<std::uint64_t>& destination,
            const std::vector<std::uint64_t>& source)
{
    if (destination.size() < source.size()) destination.resize(source.size(), 0U);
    for (std::size_t i = 0; i < source.size(); ++i) destination[i] |= source[i];
}

std::uint64_t maskPopulation(const std::vector<std::uint64_t>& mask)
{
    std::uint64_t result = 0;
    for (std::uint64_t word : mask) {
        while (word != 0U) {
            word &= word - 1U;
            ++result;
        }
    }
    return result;
}

void setMaskPixel(std::vector<std::uint64_t>& mask, std::size_t pixel,
                  std::size_t pixelCount)
{
    if (mask.empty()) mask.assign((pixelCount + 63U) / 64U, 0U);
    mask[pixel / 64U] |= std::uint64_t{1} << (pixel % 64U);
}

RotatedHexContribution rotateHexContribution(const hexvoxel::VoxelResult& source,
                                              int sampleN, float rotationDegrees)
{
    RotatedHexContribution result;
    result.rho = source.hex.rho;
    result.sampleN = sampleN;
    // DEFERRED[中] 物理: 旋转角被四舍五入到 90° 的整数倍，非 90° 倍数的实例旋转（如 30°、45°）在异质体元投影掩膜/消光系数中被静默当作 0° 或 90°，而几何本身按真实角度旋转，二者不一致；建议：非 90° 倍数时报警告/错误，或对任意角度重新投影。
    const int turns = static_cast<int>(std::llround(-rotationDegrees / 90.0f)) & 3;
    const std::size_t pixelCount = static_cast<std::size_t>(sampleN) * sampleN;

    auto rotateXZ = [sampleN, turns](int x, int z) {
        for (int turn = 0; turn < turns; ++turn) {
            const int nextX = z;
            const int nextZ = sampleN - 1 - x;
            x = nextX;
            z = nextZ;
        }
        return std::array<int, 2>{{x, z}};
    };

    for (int sourceAxis = 0; sourceAxis < 3; ++sourceAxis) {
        const int destinationAxis = (turns & 1) != 0
            ? (sourceAxis == 0 ? 2 : (sourceAxis == 2 ? 0 : 1))
            : sourceAxis;
        result.projectedArea[destinationAxis] += source.projectedArea[sourceAxis];
        const auto& sourceMask = source.coverMask[sourceAxis];
        for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
            if (pixel / 64U >= sourceMask.size()
                || (sourceMask[pixel / 64U] &
                    (std::uint64_t{1} << (pixel % 64U))) == 0U) continue;
            const int u = static_cast<int>(pixel % static_cast<std::size_t>(sampleN));
            const int v = static_cast<int>(pixel / static_cast<std::size_t>(sampleN));
            int x = 0, y = 0, z = 0;
            if (sourceAxis == 0) { y = u; z = v; }
            else if (sourceAxis == 1) { x = u; z = v; }
            else { x = u; y = v; }
            const auto rotated = rotateXZ(x, z);
            x = rotated[0]; z = rotated[1];
            int destinationU = 0, destinationV = 0;
            if (destinationAxis == 0) { destinationU = y; destinationV = z; }
            else if (destinationAxis == 1) { destinationU = x; destinationV = z; }
            else { destinationU = x; destinationV = y; }
            setMaskPixel(result.coverMask[destinationAxis],
                         static_cast<std::size_t>(destinationV) * sampleN
                             + destinationU,
                         pixelCount);
        }
    }
    return result;
}

void addHexContribution(HexMixAccumulator& accumulator, const ObjVoxelCoord& coord,
                        const std::vector<int>& linkIndices,
                        const RotatedHexContribution& contribution,
                        const MeshLink& prototype)
{
    HexWorldAccum& world = accumulator.cells[coord];
    world.coord = coord;
    // DEFERRED[低] Bug: 同一世界体元的多个贡献若 sampleN 不同，这里只取第一个，之后 orMask 把不同分辨率的掩膜按位 OR（像素索引含义不同），遮挡率计算错误且不报错（finalize 只校验相邻体元）；建议：sampleN 不一致时抛异常。
    if (world.sampleN == 0) world.sampleN = contribution.sampleN;
    if (world.linkIndices.empty()) world.linkIndices = linkIndices;

    auto category = std::find_if(world.categories.begin(), world.categories.end(),
        [&](const HexCategoryAccum& value) {
            return value.type == prototype.type
                && value.spectralId == prototype.spectralId
                && value.prototype.thermalId == prototype.thermalId;
        });
    if (category == world.categories.end()) {
        HexCategoryAccum value;
        value.type = prototype.type;
        value.spectralId = prototype.spectralId;
        value.prototype = prototype;
        world.categories.emplace_back(std::move(value));
        category = std::prev(world.categories.end());
    }

    category->rho += contribution.rho;
    for (int axis = 0; axis < 3; ++axis) {
        category->projectedArea[axis] += contribution.projectedArea[axis];
        orMask(category->coverMask[axis], contribution.coverMask[axis]);
        orMask(world.unionMask[axis], contribution.coverMask[axis]);
    }
}

template <typename ModelIO>
void finalizeHexMixtures(HexMixAccumulator& accumulator,
                         const std::shared_ptr<ModelIO>& modelio)
{
    if (accumulator.cells.empty()) return;
    auto& meshio = modelio->m_meshio;
    auto& instanceio = modelio->m_instanceio;
    auto& voxelio = modelio->m_voxelio;
    const std::size_t baseMaterialCount = meshio->spectralNames.size();
    const std::size_t sensorBands = baseMaterialCount > 0
        ? meshio->spectrals.size() / baseMaterialCount : 0U;
    const std::size_t fixedBands = baseMaterialCount > 0
        ? meshio->fixedSpectrals.size() / baseMaterialCount : 0U;

    std::vector<HexWorldAccum*> ordered;
    ordered.reserve(accumulator.cells.size());
    for (auto& entry : accumulator.cells) ordered.push_back(&entry.second);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        if (left->coord.x != right->coord.x) return left->coord.x < right->coord.x;
        if (left->coord.y != right->coord.y) return left->coord.y < right->coord.y;
        return left->coord.z < right->coord.z;
    });

    std::size_t mixedCategoryCells = 0;
    // VoxelEB has 2002 fixed bands. Quantising the two-class weight permits
    // identical mixtures to share one material instead of duplicating a full
    // spectrum for every world voxel.
    std::map<std::tuple<int, int, int, int, int>, std::pair<int, int>> compositeCache;

    // Optional HEX mipmap hierarchy.  Each table contains all levels for
    // every parent voxel; level 0 is 1^3 and each following level doubles.
    std::map<std::tuple<int, int, int>, std::vector<hexprojection::Cell>> hierarchy;
    std::vector<int> lodDivisions;
    if (const char* path = std::getenv("HISTREAM_HEX_LOD_TABLE")) {
        std::ifstream input(path);
        std::string magic;
        float parentSize = 0.0f;
        int levelCount = 0;
        int rootCount = 0;
        if (!(input >> magic >> parentSize >> levelCount >> rootCount) ||
            magic != "HEX_LOD_V1" || !std::isfinite(parentSize) ||
            parentSize <= 0.0f || levelCount < 1 || levelCount > 6 ||
            rootCount < 1 || rootCount > 100000) {
            throw std::runtime_error("Invalid HEX_LOD_V1 header");
        }
        if (std::abs(parentSize - modelio->stepsize_surface) > 1.0e-5f ||
            std::abs(parentSize - modelio->stepsize_height) > 1.0e-5f) {
            throw std::runtime_error("HEX LOD parent size must match cubic voxels");
        }
        std::size_t entryCount = 0;
        for (int level = 0; level < levelCount; ++level) {
            int n = 0;
            if (!(input >> n) || n < 1 || n > 32 ||
                (level == 0 && n != 1) ||
                (level > 0 && n != 2 * lodDivisions.back())) {
                throw std::runtime_error("HEX LOD subdivisions must double from 1");
            }
            lodDivisions.push_back(n);
            entryCount += static_cast<std::size_t>(n) * n * n;
        }
        if (entryCount * static_cast<std::size_t>(rootCount) >= 16000000U) {
            throw std::runtime_error("HEX LOD table exceeds GPU index capacity");
        }
        for (int root = 0; root < rootCount; ++root) {
            int x = 0, y = 0, z = 0;
            if (!(input >> x >> y >> z)) {
                throw std::runtime_error("Missing HEX LOD root coordinate");
            }
            std::vector<hexprojection::Cell> values(entryCount);
            for (auto& cell : values) {
                for (float& value : cell) {
                    if (!(input >> value) || !std::isfinite(value) || value < 0.0f) {
                        throw std::runtime_error("Invalid HEX LOD cell");
                    }
                }
            }
            if (!hierarchy.emplace(std::make_tuple(x, y, z), std::move(values)).second) {
                throw std::runtime_error("Duplicate HEX LOD root coordinate");
            }
        }
        std::string trailing;
        if (input >> trailing) throw std::runtime_error("Unexpected HEX LOD data");
        if (hierarchy.size() != ordered.size()) {
            throw std::runtime_error("HEX LOD root count does not match scene");
        }
    }
    if (const char* cone = std::getenv("HISTREAM_HEX_CONE")) {
        if (std::string(cone) != "lod") {
            throw std::runtime_error("HISTREAM_HEX_CONE currently supports lod");
        }
        if (hierarchy.empty()) {
            throw std::runtime_error(
                "HISTREAM_HEX_CONE=lod requires HISTREAM_HEX_LOD_TABLE");
        }
    }
    for (HexWorldAccum* world : ordered) {
        if (world->categories.empty() || world->linkIndices.empty()) continue;
        const double pixelCount = static_cast<double>(world->sampleN) * world->sampleN;
        std::array<double, 3> totalCover{{0.0, 0.0, 0.0}};
        std::array<double, 3> totalProjected{{0.0, 0.0, 0.0}};
        std::vector<double> opticalContribution(world->categories.size(), 0.0);
        double rho = 0.0;

        for (int axis = 0; axis < 3; ++axis) {
            totalCover[axis] = pixelCount > 0.0
                ? static_cast<double>(maskPopulation(world->unionMask[axis])) / pixelCount
                : 0.0;
            double rawCoverSum = 0.0;
            std::vector<double> categoryCover(world->categories.size(), 0.0);
            for (std::size_t categoryIndex = 0;
                 categoryIndex < world->categories.size(); ++categoryIndex) {
                HexCategoryAccum& category = world->categories[categoryIndex];
                categoryCover[categoryIndex] = pixelCount > 0.0
                    ? static_cast<double>(maskPopulation(category.coverMask[axis])) / pixelCount
                    : 0.0;
                rawCoverSum += categoryCover[categoryIndex];
                totalProjected[axis] += category.projectedArea[axis];
            }
            if (rawCoverSum > 1.0e-12) {
                for (std::size_t categoryIndex = 0;
                     categoryIndex < world->categories.size(); ++categoryIndex) {
                    // 先保留实际联合遮挡，再按类别独立遮挡比例分配。
                    opticalContribution[categoryIndex] += totalCover[axis]
                        * categoryCover[categoryIndex] / rawCoverSum;
                }
            }
        }
        for (const HexCategoryAccum& category : world->categories) rho += category.rho;

        VoxelHex mixed{};
        mixed.model = 10;
        mixed.rho = static_cast<float>(std::max(rho, 0.0));
        float* ci[3] = {&mixed.ax, &mixed.ay, &mixed.az};
        for (int axis = 0; axis < 3; ++axis) {
            float value = 1.0f;
            if (totalProjected[axis] > 1.0e-12 && totalCover[axis] > 0.0) {
                const double minimumGap = pixelCount > 0.0 ? 0.5 / pixelCount : 1.0e-6;
                const double gap = std::max(1.0 - totalCover[axis], minimumGap);
                value = static_cast<float>(std::max(
                    -std::log(gap) / totalProjected[axis], 0.0));
            }
            *ci[axis] = value;
        }

        // Use the same discrete projection that produced the masks for the
        // current-scale descriptor, then retain pairwise gap correlation at
        // each of the six voxel faces.
        const float cellSize = modelio->stepsize_surface;
        if (std::abs(cellSize - modelio->stepsize_height) > 1.0e-5f) {
            throw std::runtime_error("Heterogeneous HEX requires cubic voxels");
        }
        if (hierarchy.empty()) {
            for (int axis = 0; axis < 3; ++axis) {
                *ci[axis] = hexneighbors::axisQ(
                    1.0 - totalCover[axis], rho, cellSize,
                    static_cast<std::size_t>(pixelCount));
            }
            for (int axis = 0; axis < 3; ++axis) {
                for (int sign = 0; sign < 2; ++sign) {
                    ObjVoxelCoord neighbour = world->coord;
                    if (axis == 0) neighbour.x += sign != 0 ? 1 : -1;
                    if (axis == 1) neighbour.y += sign != 0 ? 1 : -1;
                    if (axis == 2) neighbour.z += sign != 0 ? 1 : -1;
                    const auto found = accumulator.cells.find(neighbour);
                    float correlation = 0.0f;
                    if (found != accumulator.cells.end()) {
                        if (found->second.sampleN != world->sampleN) {
                            throw std::runtime_error(
                                "Neighbour HEX projection resolutions differ");
                        }
                        correlation = hexneighbors::beta(
                            world->unionMask[axis], found->second.unionMask[axis],
                            static_cast<std::size_t>(pixelCount));
                    }
                    const int face = axis * 2 + sign;
                    mixed.children[face / 3][face % 3] = correlation;
                }
            }
        }

        const int hexId = static_cast<int>(voxelio->voxelHexs.size());
        voxelio->voxelHexs.emplace_back(mixed);

        if (!hierarchy.empty()) {
            const auto found = hierarchy.find(std::make_tuple(
                world->coord.x, world->coord.y, world->coord.z));
            if (found == hierarchy.end()) {
                throw std::runtime_error("HEX LOD table missing parent voxel");
            }
            auto& root = voxelio->voxelHexs[hexId];
            root.model = 5;
            root.a = static_cast<float>(lodDivisions.size() - 1);
            root.b = static_cast<float>(lodDivisions.size());
            root.c = -1.0f;

            const auto& values = found->second;
            const int finest = lodDivisions.back();
            const std::size_t fineOffset = values.size() -
                static_cast<std::size_t>(finest) * finest * finest;
            const std::vector<hexprojection::Cell> fine(
                values.begin() + static_cast<std::ptrdiff_t>(fineOffset),
                values.end());
            if (!fine.empty()) {
                root.ax = fine.front()[0];
                root.ay = fine.front()[1];
                root.az = fine.front()[2];
                root.rho = fine.front()[3];
            }

            // Keep the six face beta values in metadata slots 6 and 7;
            // hierarchy levels are limited to six so these slots stay free.
            root.children[6][3] = 1.0f;
            for (int axis = 0; axis < 3; ++axis) {
                for (int sign = 0; sign < 2; ++sign) {
                    ObjVoxelCoord neighbour = world->coord;
                    if (axis == 0) neighbour.x += sign != 0 ? 1 : -1;
                    if (axis == 1) neighbour.y += sign != 0 ? 1 : -1;
                    if (axis == 2) neighbour.z += sign != 0 ? 1 : -1;
                    float correlation = 0.0f;
                    const auto neighbourFound = hierarchy.find(std::make_tuple(
                        neighbour.x, neighbour.y, neighbour.z));
                    if (neighbourFound != hierarchy.end()) {
                        const auto& neighbourValues = neighbourFound->second;
                        const std::size_t neighbourOffset = neighbourValues.size() -
                            static_cast<std::size_t>(finest) * finest * finest;
                        const std::vector<hexprojection::Cell> neighbourFine(
                            neighbourValues.begin() +
                                static_cast<std::ptrdiff_t>(neighbourOffset),
                            neighbourValues.end());
                        correlation = hexprojection::overlapLogRatio(
                            fine, neighbourFine, finest, axis, cellSize);
                    }
                    const int face = axis * 2 + sign;
                    root.children[6 + face / 3][face % 3] = correlation;
                }
            }

            std::size_t source = 0;
            for (std::size_t level = 0; level < lodDivisions.size(); ++level) {
                const int n = lodDivisions[level];
                // Appending child nodes can reallocate voxelHexs; reindex the root.
                voxelio->voxelHexs[hexId].children[level] = {
                    static_cast<float>(voxelio->voxelHexs.size()),
                    static_cast<float>(n), 1.0f / static_cast<float>(n), 0.0f};
                for (int cell = 0; cell < n * n * n; ++cell) {
                    const auto& value = values[source++];
                    VoxelHex node{};
                    node.model = 6;
                    node.ax = value[0];
                    node.ay = value[1];
                    node.az = value[2];
                    node.rho = value[3];
                    voxelio->voxelHexs.emplace_back(node);
                }
            }
        }
        for (int linkIndex : world->linkIndices) {
            if (linkIndex >= 0
                && linkIndex < static_cast<int>(voxelio->voxellinks.size())) {
                voxelio->voxellinks[linkIndex].hexId = hexId;
            }
        }

        if (world->categories.size() == 1U || sensorBands == 0U) continue;
        ++mixedCategoryCells;
        double contributionSum = 0.0;
        for (double value : opticalContribution) contributionSum += value;
        if (contributionSum <= 1.0e-12) {
            for (std::size_t i = 0; i < world->categories.size(); ++i) {
                opticalContribution[i] = std::accumulate(
                    world->categories[i].projectedArea.begin(),
                    world->categories[i].projectedArea.end(), 0.0);
                contributionSum += opticalContribution[i];
            }
        }
        if (contributionSum <= 1.0e-12) {
            std::fill(opticalContribution.begin(), opticalContribution.end(), 1.0);
            contributionSum = static_cast<double>(opticalContribution.size());
        }

        std::vector<std::size_t> retained(world->categories.size());
        std::iota(retained.begin(), retained.end(), 0U);
        std::stable_sort(retained.begin(), retained.end(),
            [&](std::size_t left, std::size_t right) {
                return opticalContribution[left] > opticalContribution[right];
            });
        if (retained.size() > 2U) retained.resize(2U);
        contributionSum = 0.0;
        for (std::size_t index : retained) {
            contributionSum += opticalContribution[index];
        }
        if (contributionSum <= 1.0e-12) {
            contributionSum = static_cast<double>(retained.size());
            for (std::size_t index : retained) opticalContribution[index] = 1.0;
        }
        const std::size_t dominant = retained.front();
        const std::size_t secondary = retained.size() > 1U ? retained[1] : retained[0];
        const int firstWeight = std::clamp(static_cast<int>(std::lround(
            opticalContribution[dominant] / contributionSum * 1024.0)), 0, 1024);
        opticalContribution[dominant] = static_cast<double>(firstWeight);
        opticalContribution[secondary] = static_cast<double>(1024 - firstWeight);
        contributionSum = 1024.0;
        const auto cacheKey = std::make_tuple(
            world->categories[dominant].spectralId,
            world->categories[secondary].spectralId,
            world->categories[dominant].prototype.thermalId,
            world->categories[secondary].prototype.thermalId,
            firstWeight);
        auto cached = compositeCache.find(cacheKey);
        int compositeSpectralId = -1;
        int compositeThermalId = -1;
        if (cached != compositeCache.end()) {
            compositeSpectralId = cached->second.first;
            compositeThermalId = cached->second.second;
        } else {
            compositeSpectralId = static_cast<int>(
                meshio->spectrals.size() / sensorBands);
            for (std::size_t band = 0; band < sensorBands; ++band) {
                double reflectance = 0.0, transmittance = 0.0;
                for (std::size_t i : retained) {
                    const std::size_t source = static_cast<std::size_t>(
                        world->categories[i].spectralId) * sensorBands + band;
                    if (source >= meshio->spectrals.size()) continue;
                    const double weight = opticalContribution[i] / contributionSum;
                    reflectance += weight * meshio->spectrals[source].reflectance;
                    transmittance += weight * meshio->spectrals[source].transmittance;
                }
                const float refl = static_cast<float>(std::clamp(reflectance, 0.0, 1.0));
                const float trans = static_cast<float>(
                    std::clamp(transmittance, 0.0, 1.0 - refl));
                // DEFERRED[低] 物理: 混合材质只保留 reflectance/transmittance，specularFraction/specularRoughness 回落为默认 0/0.25，含镜面材质的体元混合后镜面分量丢失；建议：同样按权重混合镜面参数。
                meshio->spectrals.push_back(Spectral{refl, trans});
            }
            if (fixedBands > 0U) {
                for (std::size_t band = 0; band < fixedBands; ++band) {
                    double reflectance = 0.0, transmittance = 0.0;
                    for (std::size_t i : retained) {
                        const std::size_t source = static_cast<std::size_t>(
                            world->categories[i].spectralId) * fixedBands + band;
                        if (source >= meshio->fixedSpectrals.size()) continue;
                        const double weight = opticalContribution[i] / contributionSum;
                        reflectance += weight * meshio->fixedSpectrals[source].reflectance;
                        transmittance += weight * meshio->fixedSpectrals[source].transmittance;
                    }
                    const float refl = static_cast<float>(std::clamp(reflectance, 0.0, 1.0));
                    const float trans = static_cast<float>(
                        std::clamp(transmittance, 0.0, 1.0 - refl));
                    meshio->fixedSpectrals.push_back(Spectral{refl, trans});
                }
            }
            if (!meshio->thermals.empty()) {
                double sunlit = 0.0, shaded = 0.0, thermalWeight = 0.0;
                for (std::size_t i : retained) {
                    const int thermalId = world->categories[i].prototype.thermalId;
                    if (thermalId < 0
                        || thermalId >= static_cast<int>(meshio->thermals.size())) continue;
                    const double weight = opticalContribution[i] / contributionSum;
                    // DEFERRED[低] 物理: 混合体元温度按面积权重对温度做线性平均，而热红外辐射与 T^4 成正比，温差大时（如 300 K 叶片 + 330 K 屋顶）等效亮温被低估约 0.5–1 K；建议：按 εσT^4 加权后再反算温度。
                    sunlit += weight * meshio->thermals[thermalId].sunlitTemperature;
                    shaded += weight * meshio->thermals[thermalId].shadedTemperature;
                    thermalWeight += weight;
                }
                if (thermalWeight > 1.0e-12) {
                    compositeThermalId = static_cast<int>(meshio->thermals.size());
                    meshio->thermals.push_back(Thermal{
                        static_cast<float>(sunlit / thermalWeight),
                        static_cast<float>(shaded / thermalWeight)});
                }
            }
            compositeCache.emplace(cacheKey,
                                   std::make_pair(compositeSpectralId,
                                                  compositeThermalId));
        }

        MeshLink composite = world->categories[dominant].prototype;
        composite.spectralId = compositeSpectralId;
        if (compositeThermalId >= 0) {
            composite.thermalId = compositeThermalId;
        }
        const int compositeMeshId = static_cast<int>(meshio->meshLinks.size());
        meshio->meshLinks.emplace_back(composite);
        const int compositeInstanceId = static_cast<int>(instanceio->instanceLinks.size());
        InstanceLink link{};
        link.meshId = compositeMeshId;
        instanceio->instanceLinks.emplace_back(link);
        for (int linkIndex : world->linkIndices) {
            if (linkIndex >= 0
                && linkIndex < static_cast<int>(voxelio->voxellinks.size())) {
                voxelio->voxellinks[linkIndex].instanceId = compositeInstanceId;
            }
        }
    }

    std::cout << "Heterogeneous voxel merge: cells=" << ordered.size()
              << " mixed-categories=" << mixedCategoryCells
              << " shared-mixtures=" << compositeCache.size()
              << " descriptors=" << voxelio->voxelHexs.size() << std::endl;
}

template <typename ModelIO>
bool createObjFilledVoxels(Scene* scene, PrimEntity& entity,
                           nanovdb::GridBuilder<int32_t>& nanoBuilder,
                           std::shared_ptr<ModelIO>& modelio,
                           const Background& background,
                           HexMixAccumulator* mixAccumulator)
{
    // DEFERRED[中] 安全: entity.objFile 为窄字符 std::string，std::filesystem::exists/ObjLoader 在 Windows 上按 ANSI 代码页解释；main 又设置 GDAL_FILENAME_IS_UTF8=NO，若前端写入的 JSON 路径为 UTF-8，含中文的 OBJ 路径在此处报“Cannot open”；建议：全链路用 std::filesystem::u8path / wstring 传递路径。
    if (!std::filesystem::exists(entity.objFile)) {
        throw std::runtime_error("Cannot open OBJ for voxelization: " + entity.objFile);
    }

    ObjLoader objLoader;
    objLoader.loadModel(entity.objFile);
    if (objLoader.m_objmesh.indices.empty()) {
        throw std::runtime_error("OBJ has no triangles for voxelization: " + entity.objFile);
    }

    const bool separateMeshes = entity.meshNames.size() > 1U;
    const bool legacyBuildingFaces = !separateMeshes
        && entity.type == Type::BUILDING;
    const size_t propertyMeshCount = separateMeshes
        ? entity.meshNames.size() : legacyBuildingFaces ? 2U : 1U;

    VoxelDesigner designer;
    std::vector<PrimMesh> propertyMeshes(propertyMeshCount);
    std::vector<std::vector<float>> propertyLeafDensities(propertyMeshCount);
    std::vector<std::vector<glm::vec3>> propertyNormals(propertyMeshCount);
    std::vector<bool> propertyOpaque(propertyMeshCount, false);
    std::vector<bool> propertyGround(propertyMeshCount, false);
    size_t totalActive = 0;
    for (size_t meshIndex = 0; meshIndex < propertyMeshCount; ++meshIndex) {
        // DEFERRED[低] 性能: 每个属性网格都先整份复制 objLoader.m_objmesh，separateMeshes 时又用 loadMesh 重新解析整个 OBJ 文件，N 个网格即解析 N+1 次、复制 N 次；百万面 OBJ 时耗时与内存都显著放大（下方 source.scene = completeScene 也是逐网格整份复制）；建议：一次解析按对象名分组，只在需要时引用而非复制。
        ObjMesh sourceMesh = objLoader.m_objmesh;
        if (separateMeshes) {
            ObjLoader meshLoader;
            meshLoader.loadMesh(entity.objFile, entity.meshNames[meshIndex], true);
            sourceMesh = std::move(meshLoader.m_objmesh);
        }
        if (sourceMesh.indices.empty()) continue;

        size_t rejectedVoxelCount = 0;
        const Type propertyType = meshIndex < entity.types.size() ? entity.types[meshIndex] : entity.type;
        const bool opaque = propertyType == Type::BUILDING || propertyType == Type::SOIL || propertyType == Type::WATER;
        propertyOpaque[meshIndex] = opaque;
        propertyGround[meshIndex] = propertyType == Type::SOIL || propertyType == Type::WATER;
        const std::vector<glm::ivec3> activeXYZ = voxelizeObjSurface(
            sourceMesh, modelio->stepsize_surface,
            entity.voxelFillThreshold, rejectedVoxelCount,
            &propertyLeafDensities[meshIndex],
            opaque ? &propertyNormals[meshIndex] : nullptr);
        propertyMeshes[meshIndex] = designer.createTriVoxels(activeXYZ, opaque);
        totalActive += activeXYZ.size();
        std::cout << "OBJ mesh voxelization: " << entity.objFile
                  << " mesh=" << (separateMeshes ? entity.meshNames[meshIndex] : "all")
                  << " triangles=" << sourceMesh.indices.size() / 3
                  << " active=" << activeXYZ.size()
                  << " rejected=" << rejectedVoxelCount
                  << " threshold=" << entity.voxelFillThreshold << std::endl;
    }
    if (totalActive == 0U) {
        throw std::runtime_error("Configured OBJ meshes have no voxelizable triangles: "
                                 + entity.objFile);
    }

    // Do not pass meshes that became empty after fill-threshold filtering to
    // Vulkan. Empty vertex/index buffers are not valid acceleration-structure
    // inputs on all drivers.
    std::vector<size_t> activeMeshIndices;
    activeMeshIndices.reserve(propertyMeshCount);
    for (size_t meshIndex = 0; meshIndex < propertyMeshCount; ++meshIndex) {
        if (!propertyMeshes[meshIndex].voxelIds.empty()) {
            activeMeshIndices.emplace_back(meshIndex);
        } else {
            std::cout << "OBJ mesh skipped after voxelization: "
                      << (separateMeshes ? entity.meshNames[meshIndex] : "all")
                      << std::endl;
        }
    }

    struct HexMeshSource {
        hexvoxel::HexScene scene;
        hexvoxel::HexResult result;
        std::vector<int> lookup;
        bool ready{false};
    };
    std::vector<HexMeshSource> hexSources(propertyMeshCount);
    if (modelio->heterogeneousVoxel) {
        std::string error;
        hexvoxel::HexScene completeScene;
        if (hexvoxel::loadObj(entity.objFile, modelio->stepsize_surface,
                              completeScene, &error)) {
            for (size_t meshIndex = 0; meshIndex < propertyMeshCount; ++meshIndex) {
                if (propertyMeshes[meshIndex].voxelIds.empty()) continue;
                HexMeshSource& source = hexSources[meshIndex];
                source.scene = completeScene;
                if (separateMeshes) {
                    const std::string& meshName = entity.meshNames[meshIndex];
                    source.scene.tris.clear();
                    bool closed = true;
                    for (const hexvoxel::ObjInfo& object : completeScene.objects) {
                        if (object.name != meshName) continue;
                        // FIXED[低] 内存: 未校验 triOffset+triCount <= completeScene.tris.size()，对象表与三角形数组不一致（OBJ 解析异常）时迭代器越界属未定义行为；已修复：插入前用 size_t 求和做范围检查，越界时抛异常。
                        if (static_cast<size_t>(object.triOffset) + object.triCount >
                            completeScene.tris.size())
                            throw std::runtime_error(
                                "OBJ object table out of range in " + entity.objFile);
                        const auto first = completeScene.tris.begin()
                            + object.triOffset;
                        source.scene.tris.insert(source.scene.tris.end(), first,
                                                 first + object.triCount);
                        closed = closed && object.closed;
                    }
                    if (source.scene.tris.empty()) {
                        std::cerr << "Heterogeneous Mesh '" << meshName
                                  << "' not found in " << entity.objFile << std::endl;
                        continue;
                    }
                    source.scene.objects = {{
                        meshName, closed, 0,
                        static_cast<std::uint32_t>(source.scene.tris.size())}};
                }

                hexvoxel::HexConfig config;
                source.result = hexvoxel::compute(
                    source.scene, config, nullptr, 0, nullptr);
                const long long cellCount =
                    static_cast<long long>(source.result.gridX)
                    * source.result.gridY * source.result.gridZ;
                source.lookup.assign(
                    static_cast<size_t>(std::max(0LL, cellCount)), -1);
                for (size_t index = 0; index < source.result.voxels.size(); ++index) {
                    const auto& voxel = source.result.voxels[index];
                    const long long flat =
                        (static_cast<long long>(voxel.iz) * source.result.gridY
                            + voxel.iy) * source.result.gridX + voxel.ix;
                    if (flat >= 0 && flat < cellCount) {
                        source.lookup[static_cast<size_t>(flat)] =
                            static_cast<int>(index);
                    }
                }
                source.ready = true;
                std::cout << "Heterogeneous Mesh voxels: "
                          << (separateMeshes ? entity.meshNames[meshIndex] : "all")
                          << " grid=" << source.result.gridX << "x"
                          << source.result.gridY << "x" << source.result.gridZ
                          << " active=" << source.result.voxels.size()
                          << " mean-density=" << source.result.meanRhoAll
                          << " time=" << source.result.computeMs << " ms" << std::endl;
            }
        } else {
            std::cerr << "Heterogeneous voxel extraction failed: " << error << std::endl;
        }
    }

    auto heterogeneousContribution = [&](size_t meshIndex,
                                         const glm::ivec3& localVoxel,
                                         float rotationDegrees,
                                         RotatedHexContribution& output) -> bool {
        if (meshIndex >= hexSources.size() || !hexSources[meshIndex].ready) {
            return false;
        }
        const HexMeshSource& source = hexSources[meshIndex];
        // ObjLoader, heterogeneous extraction and Three.js now preserve the
        // same OBJ axes, so the active voxel maps directly to the source cell.
        const glm::ivec3 sourceVoxel = localVoxel;
        const long long ix = static_cast<long long>(sourceVoxel.x)
            - std::lround(source.scene.gridOrigin[0]);
        const long long iy = static_cast<long long>(sourceVoxel.y)
            - std::lround(source.scene.gridOrigin[1]);
        const long long iz = static_cast<long long>(sourceVoxel.z)
            - std::lround(source.scene.gridOrigin[2]);
        if (ix < 0 || iy < 0 || iz < 0 || ix >= source.result.gridX
            || iy >= source.result.gridY || iz >= source.result.gridZ) return false;
        const int lookup = source.lookup[static_cast<size_t>(
            (iz * source.result.gridY + iy) * source.result.gridX + ix)];
        if (lookup < 0) return false;
        output = rotateHexContribution(
            source.result.voxels[static_cast<size_t>(lookup)],
            source.result.sampleN, rotationDegrees);
        return true;
    };

    auto accessor = nanoBuilder.getAccessor();
    auto& meshio = modelio->m_meshio;
    auto& instanceio = modelio->m_instanceio;
    auto& voxelio = modelio->m_voxelio;
    int& modelMeshCount = modelio->n_modelmesh;
    int& instanceCount = modelio->n_instance;
    int& voxelCount = modelio->n_voxel;

    // OBJ、体素和 Three.js 均使用 +X 北、+Y 上、+Z 东。
    // 传统的 createTriEntity 使用 X/Z/Y（Z 为高度），不能再调用
    // XYZ2XZY，否则会把 OBJ 的高度轴 Y 换到水平轴 Z。
    std::vector<int> meshIdByProperty(propertyMeshCount, -1);
    int addedMeshCount = 0;
    for (const size_t meshIndex : activeMeshIndices) {
        PrimMesh mesh = propertyMeshes[meshIndex];
        mesh.meshId = modelMeshCount + addedMeshCount;
        meshIdByProperty[meshIndex] = mesh.meshId;
        meshio->primMeshes.emplace_back(std::move(mesh));

        MeshLink link{};
        const Type meshType = meshIndex < entity.types.size()
            ? entity.types[meshIndex] : entity.type;
        link.spectralId = requiredMappedId(meshio->spectralNames,
                                   entity.spectralNames, meshIndex, "spectral");
        link.thermalId = mappedId(meshio->thermalNames,
                                  entity.thermalNames, meshIndex, "thermal");
        link.canopyId = meshType == Type::WATER ? 0 :
            mappedId(meshio->canopyNames, entity.canopyNames, meshIndex, "canopy");
        if (meshType == Type::VEGETATION) {
            link.bioId = mappedId(meshio->leafbioNames, entity.propNames, meshIndex, "leaf biochemistry");
        } else if (meshType == Type::WATER) {
            link.bioId = mappedId(meshio->watersetNames, entity.propNames, meshIndex, "water property");
        } else {
            link.bioId = mappedId(meshio->soilsetNames, entity.propNames, meshIndex, "soil property");
        }
        link.type = static_cast<int>(meshType);
        meshio->meshLinks.emplace_back(link);
        ++addedMeshCount;
    }

    if (entity.isdisFromFile) {
        const auto records = Utils::readPositionFile(entity.distributefile);
        entity.primDistributions.resize(records.size());
        entity.scales.resize(records.size());
        entity.rotations.resize(records.size());
        for (size_t index = 0; index < records.size(); ++index) {
            entity.primDistributions[index] = glm::vec3(records[index].x, records[index].y, records[index].z);
            entity.scales[index] = records[index].scale;
            entity.rotations[index] = records[index].rotation;
        }
    }

    // Placed instances map world cells back into the object's voxelization
    // (inverse mapping): a rotated or scaled object fills every cell it
    // covers instead of scattering its unit-scale voxels with gaps.
    std::vector<std::unordered_map<ObjVoxelCoord, size_t, ObjVoxelCoordHash>>
        localVoxelLookup(propertyMeshCount);
    std::vector<std::pair<glm::vec3, glm::vec3>> localBounds(propertyMeshCount);
    for (size_t meshIndex = 0; meshIndex < propertyMeshCount; ++meshIndex) {
        const auto& voxelIds = propertyMeshes[meshIndex].voxelIds;
        if (voxelIds.empty()) continue;
        glm::vec3 lower(std::numeric_limits<float>::max());
        glm::vec3 upper(std::numeric_limits<float>::lowest());
        auto& lookup = localVoxelLookup[meshIndex];
        lookup.reserve(voxelIds.size());
        for (size_t localIndex = 0; localIndex < voxelIds.size(); ++localIndex) {
            const glm::vec3 voxel = voxelIds[localIndex];
            lookup.emplace(ObjVoxelCoord{static_cast<int>(std::floor(voxel.x)),
                                         static_cast<int>(std::floor(voxel.y)),
                                         static_cast<int>(std::floor(voxel.z))},
                           localIndex);
            lower = glm::min(lower, voxel);
            upper = glm::max(upper, voxel + glm::vec3(1.0f));
        }
        localBounds[meshIndex] = {lower, upper};
    }

    const glm::ivec3 sceneHalf{
        static_cast<int>(std::floor(modelio->voxelSize_XZY.x / 2.0f + 0.5f)),
        0,
        static_cast<int>(std::floor(modelio->voxelSize_XZY.z / 2.0f + 0.5f))};
    const int sceneVoxelHeight = std::max(1, static_cast<int>(std::ceil(
        modelio->sceneSize_XYZ.z / std::max(0.01f, modelio->stepsize_surface))));
    // DEFERRED[低] 数值: OBJ 体元化在竖直方向一律使用 stepsize_surface（高度上限、gridShift.y、地形高度），未检查 stepsize_height 是否相同；二者不等时 OBJ 体元的高度层与背景/基元体元的层号错位（异质路径才检查立方体元）；建议：入口处断言 stepsize_surface==stepsize_height 或分别按轴换算。

    for (size_t placementIndex = 0;
         placementIndex < entity.primDistributions.size(); ++placementIndex) {
        const glm::vec3 placement = entity.primDistributions[placementIndex];
        const float scaleValue = placementIndex < entity.scales.size()
            ? entity.scales[placementIndex] : 1.0f;
        const float rotationValue = placementIndex < entity.rotations.size()
            ? entity.rotations[placementIndex] : 0.0f;
        const int terrainHeight = static_cast<int>(std::round(
            demHeightAboveMinimum(background, placement.x, placement.y) /
            modelio->stepsize_surface));
        const glm::ivec3 gridShift{
            static_cast<int>(std::floor(placement.x / modelio->stepsize_surface)),
            static_cast<int>(std::floor(placement.z / modelio->stepsize_surface)) + terrainHeight,
            static_cast<int>(std::floor(placement.y / modelio->stepsize_surface))};

        const glm::mat4 unit(1.0f);
        const glm::mat4 rotation = glm::rotate(
            unit, glm::radians(-rotationValue), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 scale = glm::scale(unit, glm::vec3(scaleValue));
        const glm::mat4 objectTransform =
            glm::translate(unit, glm::vec3(gridShift - sceneHalf)) *
            rotation * scale;

        std::vector<int> instanceIdByProperty(propertyMeshCount, -1);
        int addedInstanceCount = 0;
        for (const size_t meshIndex : activeMeshIndices) {
            Instance instance{};
            instance.meshId = static_cast<uint32_t>(meshIdByProperty[meshIndex]);
            instance.object2worldMatrix = objectTransform;
            instance.world2objectMatrix =
                glm::transpose(glm::inverse(objectTransform));
            instanceIdByProperty[meshIndex] = instanceCount + addedInstanceCount;
            // DEFERRED[低] 可维护性: 字段名为 world2objectMatrix，实际存放的是 inverse 的转置（法向矩阵）；全文件一致如此，但若任一着色器/CPU 代码按名字用它把世界坐标点变换到物体空间，平移分量会落在第 4 行而得到错误结果；建议：改名为 normalMatrix，或另存真正的 inverse。
            instanceio->instances.emplace_back(instance);

            InstanceLink instanceLink{};
            instanceLink.meshId = instance.meshId;
            instanceio->instanceLinks.emplace_back(instanceLink);
            ++addedInstanceCount;
        }

        size_t insertedForPlacement = 0;
        size_t overlappedForPlacement = 0;
        size_t clippedForPlacement = 0;
        const size_t voxelMeshCount = legacyBuildingFaces ? 1U : propertyMeshCount;
        for (size_t meshIndex = 0; meshIndex < voxelMeshCount; ++meshIndex) {
            if (meshIdByProperty[meshIndex] < 0
                || instanceIdByProperty[meshIndex] < 0) continue;
            const std::vector<float>& leafDensities = propertyLeafDensities[meshIndex];
            const auto& lookup = localVoxelLookup[meshIndex];
            if (lookup.empty()) continue;
            const glm::mat4 localToGrid = rotation * scale;
            const glm::mat4 gridToLocal = glm::inverse(localToGrid);
            glm::vec3 gridLower(std::numeric_limits<float>::max());
            glm::vec3 gridUpper(std::numeric_limits<float>::lowest());
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec3 point(
                    corner & 1 ? localBounds[meshIndex].second.x : localBounds[meshIndex].first.x,
                    corner & 2 ? localBounds[meshIndex].second.y : localBounds[meshIndex].first.y,
                    corner & 4 ? localBounds[meshIndex].second.z : localBounds[meshIndex].first.z);
                const glm::vec3 mapped(localToGrid * glm::vec4(point, 1.0f));
                gridLower = glm::min(gridLower, mapped);
                gridUpper = glm::max(gridUpper, mapped);
            }
            const glm::ivec3 cellLower(glm::floor(gridLower));
            const glm::ivec3 cellUpper(glm::ceil(gridUpper));
            // DEFERRED[中] 物理: 逆映射只取世界格中心对应的单个局部体元；对非 90° 旋转或 scale<1 的实例，单体元厚的薄壳（叶片层、墙面）会因中心点落在相邻空格而出现孔洞/漏格，scale<1 时多个局部体元合并为一格也只计其中一个的叶面积；建议：对每个世界格做多点（如 2×2×2）超采样，或正向光栅化三角形到世界网格。
            for (int cellY = cellLower.y; cellY < cellUpper.y; ++cellY)
            for (int cellZ = cellLower.z; cellZ < cellUpper.z; ++cellZ)
            for (int cellX = cellLower.x; cellX < cellUpper.x; ++cellX) {
                const glm::ivec3 cell(cellX, cellY, cellZ);
                const glm::vec3 localPoint(gridToLocal *
                    glm::vec4(glm::vec3(cell) + glm::vec3(0.5f), 1.0f));
                const auto found = lookup.find(ObjVoxelCoord{
                    static_cast<int>(std::floor(localPoint.x)),
                    static_cast<int>(std::floor(localPoint.y)),
                    static_cast<int>(std::floor(localPoint.z))});
                if (found == lookup.end()) continue;
                const size_t localIndex = found->second;
                const glm::vec3 localVoxel = propertyMeshes[meshIndex].voxelIds[localIndex];
                // Instance scale k: a local voxel's facet area grows by k^2 and
                // spreads over k^3 world cells.
                const float leafDensity = entity.leafDensityFromGeometry
                    && localIndex < leafDensities.size()
                    ? leafDensities[localIndex] / std::max(scaleValue, 1.0e-6f) : 0.0f;
                const glm::ivec3 voxelId = gridShift + cell;

                if (voxelId.x < 0 || voxelId.z < 0 || voxelId.y < 0 ||
                    voxelId.x >= modelio->voxelSize_XZY.x ||
                    voxelId.y >= sceneVoxelHeight ||
                    voxelId.z >= modelio->voxelSize_XZY.z) {
                    ++clippedForPlacement;
                    continue;
                }

                const nanovdb::Coord coord(voxelId.x, voxelId.y, voxelId.z);
                const ObjVoxelCoord worldCoord{voxelId.x, voxelId.y, voxelId.z};
                RotatedHexContribution contribution;
                const bool hasContribution = mixAccumulator
                    && heterogeneousContribution(meshIndex,
                                                 glm::ivec3(localVoxel),
                                                 rotationValue, contribution);
                const int existingIndex = accessor.getValue(coord);
                std::vector<int> linkIndices;

                if (existingIndex < 0) {
                    const int firstLink = voxelCount;
                    accessor.setValue(coord, firstLink);
                    VoxelLink link{};
                    link.voxelId = voxelId;
                    link.instanceId = instanceIdByProperty[meshIndex];
                    link.aeroId = 0;
                    link.faceId = 0;
                    link.isValid = 1;
                    link.leafDensity = leafDensity;
                    link.hexId = -1;
                    const std::vector<glm::vec3>& normals = propertyNormals[meshIndex];
                    if (propertyOpaque[meshIndex] && localIndex < normals.size()) {
                        link.normal = glm::vec3(rotation * glm::vec4(normals[localIndex], 0.0f));
                        // Ground cannot face downward; such cells come from flipped winding.
                        if (propertyGround[meshIndex] && link.normal.y < 0.0f) link.normal = -link.normal;
                    }
                    voxelio->voxellinks.emplace_back(link);
                    linkIndices.push_back(voxelCount++);
                    ++insertedForPlacement;
                } else {
                    ++overlappedForPlacement;
                    // DEFERRED[低] 物理: 重叠格保留先写入者的 instanceId/类型，仅累加 leafDensity；若先写入的是建筑/土壤（不透明体元）而后来的是植被，叶面积被加到不透明体元上，若反之则植被体元吞掉建筑表面；不同类型重叠只计数不告警；建议：按类型优先级（不透明优先）决定归属，或在类型冲突时告警。
                    if (existingIndex < static_cast<int>(voxelio->voxellinks.size()))
                        voxelio->voxellinks[existingIndex].leafDensity += leafDensity;
                    if (mixAccumulator) {
                        const auto found = mixAccumulator->cells.find(worldCoord);
                        if (found != mixAccumulator->cells.end()) {
                            linkIndices = found->second.linkIndices;
                        }
                    }
                }

                if (hasContribution && !linkIndices.empty()) {
                    addHexContribution(
                        *mixAccumulator, worldCoord, linkIndices, contribution,
                        meshio->meshLinks[meshIdByProperty[meshIndex]]);
                }
            }
        }

        instanceCount += addedInstanceCount;
        std::cout << "OBJ placement " << placementIndex
                  << " terrain-height=" << terrainHeight * modelio->stepsize_surface
                  << "m inserted=" << insertedForPlacement
                  << " mesh-overlap=" << overlappedForPlacement
                  << " clipped=" << clippedForPlacement << std::endl;
    }

    modelMeshCount += addedMeshCount;
    return true;
}

} // namespace

bool Scene::createObjScene(std::shared_ptr<FileIO> &fileio, std::shared_ptr<RaytracingIO> &raytracingio) {


    float x = fileio->m_pRaytracingXml->scenexml.background.sceneSize.x; // lenght
    float y = fileio->m_pRaytracingXml->scenexml.background.sceneSize.y; // width
    float z = fileio->m_pRaytracingXml->scenexml.background.sceneSize.z; // height
/*    raytracingio->sMin = glm::vec3(-x / 2.0, 0, -y / 2.0);
    raytracingio->sMax = glm::vec3(x / 2.0, z, y / 2.0);*/
    raytracingio->sceneSize = glm::vec3(x,y,z);
    raytracingio->sceneOrigin = fileio->m_pRaytracingXml->scenexml.background.sceneOrigin;

    raytracingio->voxelSize = glm::vec3(x,z,y);
    raytracingio->voxelOrigin =  glm::vec3(0,0,0);


    bool isInterp = false;
    auto & meshio = raytracingio->m_meshio;
    auto & instanceio = raytracingio->m_instanceio;
    auto & scenexml = fileio->m_pRaytracingXml->scenexml;

    //auto & sceneio = raytracingio->m_sceneio;

    ObjLoader loader;

    //-------------------------
    //-- Background Mesh
    //-------------------------
    if(scenexml.background.isDEM) {
        loader.creatBackgroundFromDEM(
            scenexml.background.DEMFile,
            scenexml.background.sceneSize,
            scenexml.background.stepsize_surface);
        std::string objDir = fileio->m_pRaytracingXml->projectDir + "/dem.obj";
        outputObjMesh(loader.m_objmesh, objDir);
        isInterp = true;
//        return false;
    }else{
        const auto& spectra = fileio->m_pRaytracingXml->spectralxmls;
        const auto background = std::find_if(spectra.begin(), spectra.end(), [&](const SpectralXml& item) {
            return item.spectralName == scenexml.background.bgSpectralName;
        });
        if (background != spectra.end() && background->physicalTextureEnabled)
            loader.createGridBackground(scenexml.background.sceneSize, scenexml.background.stepsize_surface);
        else
            loader.createBackground(scenexml.background.sceneSize);
    }
    //n_modelmesh = 0;
    int & n_modelmesh = raytracingio->n_modelmesh;
    loader.m_objmesh.meshId = n_modelmesh;
    meshio->objMeshes.emplace_back(loader.m_objmesh);

//    std::string outpath = "D:/data/sim_albedo/demTest.obj";
//    outputObjMesh(loader.m_objmesh, outpath);
    //-------------------------
    //-- Background MeshLink
    //-------------------------
    MeshLink bgMeshLink{};
    bgMeshLink.type = int(Type::SOIL);
    std::string bgSpectralName = fileio->m_pRaytracingXml->scenexml.background.bgSpectralName;
    bgMeshLink.spectralId =  requiredId(meshio->spectralNames, bgSpectralName, "spectral");
    std::string bgThermalName =
        fileio->m_pRaytracingXml->scenexml.background.bgThermalName;
    int bgThermalIndex = 0;
    // DEFERRED[低] 错误处理: 背景热属性名未找到时静默使用 0 号条目，而网格/体元路径的 mappedId 至少会告警；拼写错误会让背景使用错误温度；建议：改用 mappedId 或 requiredId 统一处理。
    const auto bgThermalIt = meshio->thermalNames.find(bgThermalName);
    if (bgThermalIt != meshio->thermalNames.end())
        bgThermalIndex = bgThermalIt->second;
    bgMeshLink.thermalId = bgThermalIndex;
    bgMeshLink.angularEffectStrength =
        fileio->m_pRaytracingXml->scenexml.background.angularEffectStrength;
    meshio->meshLinks.emplace_back(bgMeshLink);
    //-------------------------
    //-- Background Instance
    //-------------------------
    Instance bgInstance{};
    bgInstance.meshId = static_cast<uint32_t>(n_modelmesh);
    n_modelmesh++;
    glm::mat4 bgunit = glm::mat4(1.0f);
    glm::vec3 bgShift = glm::vec3{0, 0, 0};
    glm::vec3 bgScale = glm::vec3{1.0, 1.0, 1.0};
    glm::mat4 bgMat = glm::scale(bgunit, bgScale) * glm::translate(bgunit,bgShift);
    bgInstance.object2worldMatrix = bgMat;
    bgInstance.world2objectMatrix = glm::transpose(glm::inverse(bgMat));
    instanceio->instances.emplace_back(bgInstance);


    //-------------------------
    //-- Background InstanceLink
    //-------------------------
    InstanceLink bgInstanceLink{};
    bgInstanceLink.meshId = bgInstance.meshId;
    instanceio->instanceLinks.emplace_back(bgInstanceLink);

    /// ------------------------------------
    /// Scene
    ///-------------------------------------



   // outputModel(loader.m_objmesh, m_pRaytracingXml->setting.outDir + "/background.obj");
   // std::cout << "Export background.obj" << std::endl;



    //-------------------------
    //-- Obj Models
    //-------------------------
    int n_obj = fileio->m_pRaytracingXml->scenexml.objEntities.size();
    for (int kobj = 0; kobj < n_obj; kobj++)
    {
        auto &objEntity = fileio->m_pRaytracingXml->scenexml.objEntities[kobj];



        std::vector<Utils::PositionRecord> placements;
        if (objEntity.isFromFile) {
            placements = Utils::readPositionFile(objEntity.file);
        } else if (objEntity.objDistributions.empty()) {
            // Same default as FacetRT: one unscaled instance at the scene centre.
            Utils::PositionRecord centre;
            centre.x = x * 0.5f;
            centre.y = y * 0.5f;
            placements.push_back(centre);
        }
        if (!placements.empty()) {
            if (isInterp)
            {
                std::vector<float> north(placements.size()), east(placements.size()), height(placements.size());
                for (size_t kin = 0; kin < placements.size(); ++kin) {
                    north[kin] = placements[kin].x;
                    east[kin] = placements[kin].y;
                    height[kin] = placements[kin].z;
                }
                loader.interpolateZValues(scenexml.background.sceneSize,
                    north.data(), east.data(), height.data(), static_cast<int>(placements.size()));
                for (size_t kin = 0; kin < placements.size(); ++kin) placements[kin].z = height[kin];
            }

            objEntity.objDistributions.resize(placements.size());
            objEntity.scales.resize(placements.size());
            objEntity.rotations.resize(placements.size());
            for (size_t kin = 0; kin < placements.size(); ++kin)
            {
                objEntity.objDistributions[kin] = glm::vec3(placements[kin].x, placements[kin].y, placements[kin].z);
                objEntity.scales[kin] = placements[kin].scale;
                objEntity.rotations[kin] = placements[kin].rotation;
            }
        }

        std::string fileName = objEntity.filePath;
        std::string objName = objEntity.objName;
        int n_mesh = objEntity.meshNames.size();
        for (int kmesh = 0; kmesh < n_mesh; kmesh++)
        {
            /// ------------------------------------
            /// model/mesh
            ///-------------------------------------
            std::string meshName = objEntity.meshNames[kmesh];
            // FIXED[高] 内存: spectralNames[kmesh] 与下方 types[kmesh] 未做长度检查，JSON 中光谱/类型绑定条目少于 meshNames 时越界读（UB/崩溃）；已修复：spectralNames 为空时抛异常，否则用 min(kmesh, size-1) 索引；types 按 kmesh < types.size() 判断，缺省用 Type{}。
            if (objEntity.spectralNames.empty())
                throw std::runtime_error("Missing spectral binding for OBJ mesh: " + meshName);
            const size_t meshBinding =
                std::min(static_cast<size_t>(kmesh), objEntity.spectralNames.size() - 1);
            std::string spectralName = objEntity.spectralNames[meshBinding];
            std::string thermalName;

            ObjLoader loader1;
            // A single binding may be the object-name fallback: an unmatched
            // name then loads the whole OBJ, as the voxel modes do.
            loader1.loadMesh(fileName, meshName, n_mesh > 1);
            if (loader1.m_objmesh.indices.empty()) {
                std::cerr << "OBJ mesh skipped (no triangles): " << meshName
                          << " in " << fileName << std::endl;
                continue;
            }
            loader1.m_objmesh.meshId = n_modelmesh;
            meshio->objMeshes.emplace_back(loader1.m_objmesh);

            /// ------------------------------------
            /// Mesh Link
            ///-------------------------------------

            int spectralIndex = requiredId(meshio->spectralNames, spectralName, "spectral");
            int thermalIndex = 0;
            // DEFERRED[低] 错误处理: 热属性名找不到时静默落到 0 号，与体元路径的 mappedId（会告警）不一致；建议：统一使用 mappedId。
            if (kmesh < static_cast<int>(objEntity.thermalNames.size())) {
                thermalName = objEntity.thermalNames[kmesh];
                const auto thermalIt = meshio->thermalNames.find(thermalName);
                if (thermalIt != meshio->thermalNames.end())
                    thermalIndex = thermalIt->second;
            }
            MeshLink meshLink{};
            meshLink.type = int(kmesh < static_cast<int>(objEntity.types.size())
                ? objEntity.types[kmesh] : Type{});
            meshLink.spectralId = spectralIndex;
            meshLink.thermalId = thermalIndex;
            meshio->meshLinks.emplace_back(meshLink);

            int n_instancet = objEntity.objDistributions.size();
            objEntity.scales.resize(objEntity.objDistributions.size(), 1.0f);
            objEntity.rotations.resize(objEntity.objDistributions.size(), 0.0f);
/*            if(n_instancet > 100){
                n_instancet = 100;
            }*/

            for (int kinstance = 0; kinstance < n_instancet; kinstance++)
            {
                /// ------------------------------------
                /// Instance
                ///-------------------------------------
                Instance instance{};
                instance.meshId = static_cast<uint32_t>(n_modelmesh);
                glm::vec3 shift0 = objEntity.objDistributions[kinstance];
                // FIXED[高] 内存: 位置直接写在 JSON 中（非文件）时 scales/rotations 不会按 objDistributions 补齐，二者条目少于位置数即越界读；已修复：进入实例循环前 resize(objDistributions.size(), 1.0f/0.0f)，索引安全。
                float scale0 = objEntity.scales[kinstance];
                float angle0 = objEntity.rotations[kinstance];

                //nvmath::vec3f shift = nvmath::vec3f{ shift0.x - x/2.0,shift0.z,shift0.y - z/2.0 };
                glm::vec3 shift;
                shift = glm::vec3{shift0.x - x / 2.0, shift0.z, shift0.y - y / 2.0};


                glm::mat4 unit = glm::mat4(1.0f);
                glm::vec3 scale = glm::vec3(scale0);
                glm::mat4 rotation = glm::rotate(unit, glm::radians(-angle0), glm::vec3(0.0, 1.0, 0.0));
                glm::mat4 mat = glm::translate(unit, shift) * rotation * glm::scale(unit, scale);
                instance.object2worldMatrix = mat;
                instance.world2objectMatrix = glm::transpose(glm::inverse(mat));
                instanceio->instances.emplace_back(instance);

                /// ------------------------------------
                /// InstanceLink
                ///-------------------------------------
                InstanceLink instanceLink{};
                instanceLink.meshId = instance.meshId;
                instanceio->instanceLinks.emplace_back(instanceLink);
            }
            n_modelmesh++;
        }

        int a = 10;
    }



    return true;
}



//-----------------------------------------------------------
//--- n_moxelmesh,n_instance,n_voxel
//--- primMeshes, meshlink, instance, instanceLink, nanovdb, voxellink
//------------------------------------------------------------

bool Scene::createPrimObjScene(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &voxellstio) {


    auto &scenexml = fileio->m_pVoxelebXml->scenexml;
    auto &voxellstxml = fileio->m_pVoxelebXml;
    auto &meshio = voxellstio->m_meshio;
    auto &instanceio = voxellstio->m_instanceio;
    auto &voxelio = voxellstio->m_voxelio;
    auto &background = fileio->m_pVoxelebXml->scenexml.background;
    nanovdb::GridBuilder<int32_t> nanoBuilder(-1);
    HexMixAccumulator hexMixtures;


    /// ------------------------------------
    /// Background
    ///-------------------------------------
    createPrimObj_Background(background, nanoBuilder, voxellstio);

    /// ------------------------------------
    /// voxel Components
    ///-------------------------------------
    for (int kVoxelModel = 0; kVoxelModel < scenexml.primEntities.size(); kVoxelModel++)
    {
        auto &voxelEntity = scenexml.primEntities[kVoxelModel];

        if (voxelEntity.voxelizeFromObj) {
            createObjFilledVoxels(this, voxelEntity, nanoBuilder, voxellstio,
                                  background,
                                  voxellstio->heterogeneousVoxel
                                      ? &hexMixtures : nullptr);
            continue;
        }

        if (voxelEntity.type == Type::VEGETATION)
        {
            if(voxelEntity.isshapeFromFile == true)
            {
                createPrimObj_Crowns(voxelEntity, nanoBuilder, voxellstio, background);
            }else {
                createPrimObj_Crown(voxelEntity, nanoBuilder, voxellstio, background);
            }
        }else if(voxelEntity.type == Type::BUILDING)
        {
            createPrimObj_Building(voxelEntity, nanoBuilder, voxellstio, background);
        }else if(voxelEntity.type == Type::WATER)
        {
            createPrimObj_Crown(voxelEntity, nanoBuilder, voxellstio, background);
        }

    }
    if (voxellstio->heterogeneousVoxel) {
        finalizeHexMixtures(hexMixtures, voxellstio);
    }
    voxellstio->m_voxelio->nanoHandle = nanoBuilder.getHandle<>();


    return true;
}

bool Scene::createPrimObj_Crown(PrimEntity & voxelEntity,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelebIO> &modelio, const Background& background){

    auto acc = nanoBuilder.getAccessor();
    auto &meshio = modelio->m_meshio;
    auto &instanceio = modelio->m_instanceio;
    auto &voxelio = modelio->m_voxelio;
    int &n_modelmesh = modelio->n_modelmesh;
    int &n_instance = modelio->n_instance;
    int &n_voxel = modelio->n_voxel;

    VoxelDesigner loader;
    /// ------------------------------------
    /// voxel model/mesh
    ///-------------------------------------
    std::string modelName = voxelEntity.primitiveName;
    Type type = voxelEntity.type;
    Shape shape = voxelEntity.shape;
    //meshio->types[0] = voxelEntity.types[0];
    // std::string aeroName = voxelEntity.aeroNames[0];


    PrimMesh currentVoxelModelXYZ1;
    currentVoxelModelXYZ1 = loader.createTriEntity(shape, modelio->stepsize_surface);
    PrimMesh currentVoxelModel1 = XYZ2XZY(currentVoxelModelXYZ1, 0); // (1,1,0) => (1,0,1) with  height = 0

    // bool a = loader.outputPrimMesh("D:/data/field_data/Sim_homo_LAI_0.0_timeSeries/crown.obj", currentVoxelModel1);

    currentVoxelModel1.meshId = n_modelmesh;
    meshio->primMeshes.emplace_back(currentVoxelModel1); //xzy
    MeshLink meshlink1{};
    meshlink1.spectralId = requiredMappedId(meshio->spectralNames,
                                    voxelEntity.spectralNames, 0, "spectral");
    meshlink1.thermalId = mappedId(meshio->thermalNames,
                                   voxelEntity.thermalNames, 0, "thermal");
    meshlink1.canopyId = type == Type::WATER ? 0 :
        mappedId(meshio->canopyNames, voxelEntity.canopyNames, 0, "canopy");
    if (type == Type::VEGETATION) {
        meshlink1.bioId = mappedId(meshio->leafbioNames,
                                   voxelEntity.propNames, 0, "leaf biochemistry");
    } else if (type == Type::SOIL || type == Type::BUILDING) {
        meshlink1.bioId = mappedId(meshio->soilsetNames,
                                   voxelEntity.propNames, 0, "soil property");
    } else if (type == Type::WATER) {
        meshlink1.bioId = mappedId(meshio->watersetNames,
                                   voxelEntity.propNames, 0, "water property");
    }
    //  meshlink1.aeroId = meshio->aeroNames.find(aeroName)->second;
    meshlink1.type = (int) type;
    meshio->meshLinks.emplace_back(meshlink1);

    const Canopy* mediumCanopy = meshlink1.canopyId >= 0 &&
        static_cast<size_t>(meshlink1.canopyId) < meshio->canopies.size()
        ? &meshio->canopies[meshlink1.canopyId] : nullptr;
    const bool isParticipatingMedium = mediumCanopy != nullptr &&
        (mediumCanopy->structureType == 2 || mediumCanopy->structureType == 3);



    if(voxelEntity.isdisFromFile == true){
        // Primitive crowns are placed unscaled and unrotated.
        const auto records = Utils::readPositionFile(voxelEntity.distributefile);
        voxelEntity.primDistributions.resize(records.size());
        voxelEntity.scales.assign(records.size(), 1.0f);
        voxelEntity.rotations.assign(records.size(), 0.0f);
        for (size_t kin = 0; kin < records.size(); ++kin)
        {
            voxelEntity.primDistributions[kin] = glm::vec3(records[kin].x, records[kin].y, records[kin].z);
        }
    }

    // instance

    voxelEntity.scales.resize(voxelEntity.primDistributions.size(), 1.0f);
    voxelEntity.rotations.resize(voxelEntity.primDistributions.size(), 0.0f);
    for (int kinstance = 0; kinstance < voxelEntity.primDistributions.size(); kinstance++) {
        // voxel instance1��change postion,

        const glm::vec3 placement = voxelEntity.primDistributions[kinstance];
        // Fire and fog positions are specified by horizontal centre and bottom
        // height. Primitive voxel geometry starts at its minimum corner.
        const float originX = placement.x -
            (isParticipatingMedium ? shape.length * 0.5f : 0.0f);
        const float originGroundY = placement.y -
            (isParticipatingMedium ? shape.width * 0.5f : 0.0f);
        glm::ivec3 shift0;
        // DEFERRED[中] 数值: float→int 隐式截断是向零取整，负坐标（如火/雾中心减半宽后 originX<0）会偏移一格，与 OBJ 路径使用的 std::floor 不一致；且 placement.z 与 DEM 高度相加后按 stepsize_surface 而非 stepsize_height 换算；建议：显式 std::floor，高度轴使用 stepsize_height。
        shift0 = {originX / modelio->stepsize_surface,
                  (placement.z + demHeightAboveMinimum(
                      background, placement.x, placement.y)) / modelio->stepsize_surface,
                  originGroundY / modelio->stepsize_surface};
        // FIXED[高] 内存: PrimEntity 默认 scales{1}/rotations{0} 只有 1 个元素，JSON 直接给出多个 primDistributions 而未给等长 scales/rotations 时（非文件路径），下方 scales[kinstance]/rotations[kinstance] 越界读；已修复：进入实例循环前按 primDistributions.size() resize scales/rotations（补齐 1.0f/0.0f）。
        // FIXED[中] Bug: 下方 Instance/InstanceLink 在插入体元之前 push_back，但本次放置若没有新增体元（isValid==0：全部与已有体元重叠或越界）会 continue 而不递增 n_instance，instances.size() 与 n_instance 脱节，后续 voxelLink.instanceId 指向错误实例（变换/meshId 错位）；已修复：isValid==0 时 pop_back 刚压入的 instances/instanceLinks 再 continue，使 instances.size() 始终等于 n_instance。

        ///-----------------------------------------------------------------------------
        ///
        ///------------------------------------------------------------------------------
        glm::ivec3 semiRange = glm::vec3{floor(modelio->voxelSize_XZY.x / 2.0+0.5), 0,
                                         floor(modelio->voxelSize_XZY.z / 2.0+0.5)};;

        float scale0 = voxelEntity.scales[kinstance];
        float angle0 = voxelEntity.rotations[kinstance];
        glm::vec3 shift = glm::vec3(shift0) - glm::vec3(semiRange);  // (5,0,5)

        //
        glm::mat4 unit = glm::mat4(1.0f);
        glm::vec3 scale = glm::vec3(scale0);
        glm::mat4 angle = glm::rotate(unit, glm::radians(-angle0), glm::vec3(0.0, 1.0, 0.0));
        // DEFERRED[中] Bug: 旋转矩阵 angle 计算后未使用；scale*translate 的顺序使平移量也被缩放；而体元插入只用 shift0+localVoxel，完全不考虑 scale/rotation，scale≠1 或 rotation≠0 时实例矩阵与 NanoVDB 体元位置不一致；建议：基元明确不支持缩放/旋转（校验并报错），或按 OBJ 路径用 translate*rotate*scale 并逆映射体元。
        glm::mat4 mat = glm::scale(unit, scale) * glm::translate(unit, shift);

        Instance instance1{};
        instance1.meshId = static_cast<uint32_t>(n_modelmesh);
        instance1.object2worldMatrix = mat;
        instance1.world2objectMatrix = glm::transpose(glm::inverse(mat));
        instanceio->instances.emplace_back(instance1);
        // voxel Instance Link
        InstanceLink instancelink1{};
        instancelink1.meshId = instance1.meshId;
        instanceio->instanceLinks.emplace_back(instancelink1);


        int isValid = 0;
        int insertedVoxelCount = 0;
        int occupiedVoxelCount = 0;
        int mediumVoxelCount = 0;

        for (int kvoxel = 0; kvoxel < currentVoxelModel1.voxelIds.size(); kvoxel++) {
            // this is what we did in the shader;
            // glm::ivec3 pos =  mat * glm::vec4(currentVoxelModel.voxelIds[kvoxel],1.0);
            const glm::ivec3 localVoxel = glm::ivec3(currentVoxelModel1.voxelIds[kvoxel]);
            if (isParticipatingMedium &&
                !keepParticipatingMediumVoxel(*mediumCanopy, localVoxel, shape,
                                              modelio->stepsize_surface)) continue;
            if (isParticipatingMedium) mediumVoxelCount++;
            glm::ivec3 Id = shift0 + localVoxel;
            int test = acc.getValue(nanovdb::Coord(Id.x, Id.y, Id.z));

            // the first bufferid;

            if (test < 0) {

                acc.setValue(nanovdb::Coord(Id.x, Id.y, Id.z), n_voxel);
                glm::ivec3 voxelPos = glm::ivec3(Id.x * 1.0, Id.y * 1.0, Id.z * 1.0);  //(5,0,5) with height = 0

                VoxelLink voxelLink{};
                voxelLink.voxelId = voxelPos;          //(5,0,5) with height = 0
                voxelLink.instanceId = n_instance;
                voxelLink.aeroId = 0;
                voxelLink.faceId = 0; // center
                voxelLink.isValid = 1; // center
                modelio->m_voxelio->voxellinks.emplace_back(voxelLink);
                n_voxel++;
                isValid = 1;
                insertedVoxelCount++;

            } else if (isParticipatingMedium) {
                occupiedVoxelCount++;
            }
        }
        if (isParticipatingMedium) {
            std::cout << "Participating medium voxelization: "
                      << voxelEntity.primitiveName
                      << " placement=" << kinstance
                      << " candidates=" << currentVoxelModel1.voxelIds.size()
                      << " medium-shape=" << mediumVoxelCount
                      << " inserted=" << insertedVoxelCount
                      << " occupied=" << occupiedVoxelCount << '\n';
        }
        if (isValid == 0) {
            instanceio->instances.pop_back();
            instanceio->instanceLinks.pop_back();
            continue;
        }

        n_instance++;

        //std::string info = "voxel entity " + std::to_string(kVoxelModel) + " done.\n";
        //LOGI(info.c_str());
        // float tt = acc.getValue(nanovdb::Coord(24, 0, 24));
    }
    n_modelmesh++;
    return true;
}

bool Scene::createPrimObj_Crowns(PrimEntity & voxelEntity,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelebIO> &modelio, const Background& background){

    auto acc = nanoBuilder.getAccessor();
    auto &meshio = modelio->m_meshio;
    auto &instanceio = modelio->m_instanceio;
    auto &voxelio = modelio->m_voxelio;
    int &n_modelmesh = modelio->n_modelmesh;
    int &n_instance = modelio->n_instance;
    int &n_voxel = modelio->n_voxel;

    VoxelDesigner loader;
    /// ------------------------------------
    /// voxel model/mesh
    ///-------------------------------------
    std::string modelName = voxelEntity.primitiveName;
    Type type = voxelEntity.type;

    //meshio->types[0] = voxelEntity.types[0];
    // std::string aeroName = voxelEntity.aeroNames[0];


    // Rows: shapeType height width length x y z [scale rotation (unused)].
    size_t skippedShapeRows = 0;
    const auto shapeRows = Utils::readNumericRows(voxelEntity.shapefile, "crown shape file", skippedShapeRows);
    if (skippedShapeRows > 0)
        throw std::runtime_error("Crown shape file has non-numeric rows: " + voxelEntity.shapefile);
    for (const auto& row : shapeRows) {
        if (row.size() < 7)
            throw std::runtime_error("Crown shape rows need 7 columns (type height width length x y z): " + voxelEntity.shapefile);
    }

    for (size_t k = 0; k < shapeRows.size(); k++) {
        const auto& row = shapeRows[k];

        // FIXED[中] 安全: 形状类型由浮点列直接强转为枚举，未校验 0..2 范围；height/width/length 未校验 >0，非法值使 createTriEntity 产生 0 个或海量体元；已修复：静默强转前校验 row[0]∈[0,2]（ShapeType 只有 CUBE/ELLIPSOID/PLANE），并校验三个尺寸为有限且非负，非法时抛出带形状文件名的异常。
        if (!std::isfinite(row[0]) || row[0] < 0.0f || row[0] > 2.0f)
            throw std::runtime_error("Crown shape type must be 0, 1 or 2: " + voxelEntity.shapefile);
        if (!(row[1] >= 0.0f) || !(row[2] >= 0.0f) || !(row[3] >= 0.0f) ||
            !std::isfinite(row[1]) || !std::isfinite(row[2]) || !std::isfinite(row[3]))
            throw std::runtime_error("Crown shape height/width/length must be finite and non-negative: " + voxelEntity.shapefile);
        Shape shape = Shape{ShapeType(static_cast<int>(row[0])), row[1], row[2], row[3], {row[4], row[5], row[6]}};


        PrimMesh currentVoxelModelXYZ1;
        currentVoxelModelXYZ1 = loader.createTriEntity(shape, modelio->stepsize_surface);
        PrimMesh currentVoxelModel1 = XYZ2XZY(currentVoxelModelXYZ1, 1); // (1,1,0) => (1,0,1) with  height = 0
        currentVoxelModel1.meshId = n_modelmesh;
        meshio->primMeshes.emplace_back(currentVoxelModel1); //xzy
        // FIXED[中] 内存: meshNames/spectralNames/canopyNames/propNames 直接取 [0] 未判空，缺少任一绑定即越界；下方 MeshLink meshlink1 未值初始化（水体类型时 bioId 及 vertexAddress 等为垃圾值），thermalId 固定为 0 而忽略 thermalNames；已修复：改用 requiredMappedId/mappedId（内含空表判空与下标钳制）取首个绑定，MeshLink 改为 meshlink1{} 值初始化；thermalId 仍为 0，行为不变。
        std::string meshName1 = voxelEntity.meshNames.empty()
            ? voxelEntity.primitiveName : voxelEntity.meshNames.front();
        MeshLink meshlink1{};
        meshlink1.spectralId = requiredMappedId(meshio->spectralNames, voxelEntity.spectralNames, 0, "spectral");
        meshlink1.thermalId = 0;
        meshlink1.canopyId = mappedId(meshio->canopyNames, voxelEntity.canopyNames, 0, "canopy");
        if (type == Type::VEGETATION) {
            // meshlink1.leafbioId = meshio->leafbioNames.find(propName)->second;
            meshlink1.bioId = requiredMappedId(meshio->leafbioNames, voxelEntity.propNames, 0, "leaf biochemistry");
        } else if (type == Type::SOIL || type == Type::BUILDING) {
            //  meshlink1.soilsetId = meshio->soilsetNames.find(propName)->second;
            meshlink1.bioId = requiredMappedId(meshio->soilsetNames, voxelEntity.propNames, 0, "soil property");
        }
        //  meshlink1.aeroId = meshio->aeroNames.find(aeroName)->second;
        meshlink1.type = (int) type;
        meshio->meshLinks.emplace_back(meshlink1);

        // instance


        for (int kinstance = 0; kinstance < 1; kinstance++) {
            // voxel instance1��change postion,

            glm::ivec3 shift0;
            shift0 = {shape.pos.x / modelio->stepsize_surface,
                      (shape.pos.z + demHeightAboveMinimum(background, shape.pos.x, shape.pos.y)) / modelio->stepsize_surface,
                      shape.pos.y / modelio->stepsize_surface};

            ///-----------------------------------------------------------------------------
            /// Attention!!!!
            ///------------------------------------------------------------------------------
            glm::ivec3 semiRange = glm::vec3{floor(modelio->voxelSize_XZY.x / 2.0+0.5), 0,
                                             floor(modelio->voxelSize_XZY.z / 2.0+0.5)};;

            float scale0 = 1;
            float angle0 = 0;
            glm::vec3 shift = glm::vec3(shift0) - glm::vec3(semiRange);  // (5,0,5)

            //
            glm::mat4 unit = glm::mat4(1.0f);
            glm::vec3 scale = glm::vec3(scale0);
            glm::mat4 angle = glm::rotate(unit, glm::radians(angle0), glm::vec3(0.0, 1.0, 0.0));
            glm::mat4 mat = glm::scale(unit, scale) * glm::translate(unit, shift);

            Instance instance1{};
            instance1.meshId = static_cast<uint32_t>(n_modelmesh);
            instance1.object2worldMatrix = mat;
            instance1.world2objectMatrix = glm::transpose(glm::inverse(mat));
            instanceio->instances.emplace_back(instance1);
            // voxel Instance Link
            InstanceLink instancelink1{};
            instancelink1.meshId = instance1.meshId;
            instanceio->instanceLinks.emplace_back(instancelink1);


            int isValid = 0;

            for (int kvoxel = 0; kvoxel < currentVoxelModel1.voxelIds.size(); kvoxel++) {
                // this is what we did in the shader;
                // glm::ivec3 pos =  mat * glm::vec4(currentVoxelModel.voxelIds[kvoxel],1.0);
                glm::ivec3 Id = shift0 + glm::ivec3(currentVoxelModel1.voxelIds[kvoxel]);
                int test = acc.getValue(nanovdb::Coord(Id.x, Id.y, Id.z));

                // the first bufferid;

                if (test < 0) {

                    acc.setValue(nanovdb::Coord(Id.x, Id.y, Id.z), n_voxel);
                    glm::ivec3 voxelPos = glm::ivec3(Id.x * 1.0, Id.y * 1.0, Id.z * 1.0);  //(5,0,5) with height = 0

                    VoxelLink voxelLink{};
                    voxelLink.voxelId = voxelPos;          //(5,0,5) with height = 0
                    voxelLink.instanceId = n_instance;
                    voxelLink.aeroId = 0;
                    voxelLink.faceId = 0; // center
                    voxelLink.isValid = 1; // center
                    modelio->m_voxelio->voxellinks.emplace_back(voxelLink);
                    n_voxel++;
                    isValid = 1;

                }
            }
            // FIXED[中] Bug: isValid==0 时已经 push 实例但 n_instance 未递增，后续 voxelLink.instanceId 与 instances 下标错位（与 1844/2590 同源，原文件未标注）；已修复：pop_back 撤销本次压入
            if (isValid == 0) {
                instanceio->instances.pop_back();
                instanceio->instanceLinks.pop_back();
                continue;
            }

            n_instance++;

            //std::string info = "voxel entity " + std::to_string(kVoxelModel) + " done.\n";
            //LOGI(info.c_str());
            // float tt = acc.getValue(nanovdb::Coord(24, 0, 24));
        }
        n_modelmesh++;
    }
    return true;
}

bool Scene::createPrimObj_Building(PrimEntity & voxelEntity,nanovdb::GridBuilder<int32_t> &nanoBuilder, std::shared_ptr<VoxelebIO> &modelio, const Background& background) {
    preparePrimitiveBuilding(voxelEntity);

    auto acc = nanoBuilder.getAccessor();
    auto &meshio = modelio->m_meshio;
    auto &instanceio = modelio->m_instanceio;
    auto &voxelio = modelio->m_voxelio;
    int &n_modelmesh = modelio->n_modelmesh;
    int &n_instance = modelio->n_instance;
    int &n_voxel = modelio->n_voxel;

    VoxelDesigner loader;
    /// ------------------------------------
    /// voxel model/mesh
    ///-------------------------------------
    std::string modelName = voxelEntity.primitiveName;
    Type type = voxelEntity.type;
    Shape shape = voxelEntity.shape;
    //meshio->types[0] = voxelEntity.types[0];
    // std::string aeroName = voxelEntity.aeroNames[0];


    PrimMesh currentVoxelModelXYZ1;
    if (voxelEntity.isheightFromFile == 1) {
        currentVoxelModelXYZ1 = loader.createTriEntitiesFromTif_wall(voxelEntity.heightfile, modelio->voxelSize_XZY,modelio->stepsize_height);
    } else {
        currentVoxelModelXYZ1 = loader.createTriCube_wall(shape, modelio->stepsize_surface);
    }

    PrimMesh currentVoxelModel1 = XYZ2XZY(currentVoxelModelXYZ1, 1); // (1,1,0) => (1,0,1) with  height = 0
    currentVoxelModel1.meshId = n_modelmesh;
    meshio->primMeshes.emplace_back(currentVoxelModel1); //xzy
    MeshLink meshlink1{};
    meshlink1.spectralId = requiredMappedId(meshio->spectralNames, voxelEntity.spectralNames, 0, "spectral");
    meshlink1.thermalId = mappedId(meshio->thermalNames, voxelEntity.thermalNames, 0, "thermal");
    meshlink1.canopyId = mappedId(meshio->canopyNames, voxelEntity.canopyNames, 0, "canopy");
    if (type == Type::VEGETATION) {
        // meshlink1.leafbioId = meshio->leafbioNames.find(propName)->second;
        meshlink1.bioId = mappedId(meshio->leafbioNames, voxelEntity.propNames, 0, "leaf biochemistry");
    } else if (type == Type::SOIL || type == Type::BUILDING) {
        //  meshlink1.soilsetId = meshio->soilsetNames.find(propName)->second;
        meshlink1.bioId = mappedId(meshio->soilsetNames, voxelEntity.propNames, 0, "soil property");
    }
    //  meshlink1.aeroId = meshio->aeroNames.find(aeroName)->second;
    meshlink1.type = (int) type;
    meshio->meshLinks.emplace_back(meshlink1);

    PrimMesh currentVoxelModel2{};
    PrimMesh currentVoxelModelXYZ2;
    if (voxelEntity.isheightFromFile == 1) {
        currentVoxelModelXYZ2 = loader.createTriEntitiesFromTif_roof(voxelEntity.heightfile, modelio->voxelSize_XZY,modelio->stepsize_height);
    } else {
        currentVoxelModelXYZ2 = loader.createTriCube_roof(shape, modelio->stepsize_surface);
    }

    currentVoxelModel2 = XYZ2XZY(currentVoxelModelXYZ2, 1); // (1,1,0) => (1,0,1) with  height = 0
    currentVoxelModel2.meshId = n_modelmesh + 1;
    meshio->primMeshes.emplace_back(currentVoxelModel2); //xzy

    /// ------------------------------------
    /// voxel model/mesh
    ///-------------------------------------
    MeshLink meshlink2{};
    meshlink2.spectralId = requiredMappedId(meshio->spectralNames, voxelEntity.spectralNames, 1, "spectral");
    meshlink2.thermalId = mappedId(meshio->thermalNames, voxelEntity.thermalNames, 1, "thermal");
    meshlink2.canopyId = mappedId(meshio->canopyNames, voxelEntity.canopyNames, 1, "canopy");
    if (type == Type::VEGETATION) {
        // meshlink1.leafbioId = meshio->leafbioNames.find(propName)->second;
        meshlink2.bioId = mappedId(meshio->leafbioNames, voxelEntity.propNames, 1, "leaf biochemistry");
    } else if (type == Type::SOIL || type == Type::BUILDING) {
        //  meshlink1.soilsetId = meshio->soilsetNames.find(propName)->second;
        meshlink2.bioId = mappedId(meshio->soilsetNames, voxelEntity.propNames, 1, "soil property");
    }
    //  meshlink1.aeroId = meshio->aeroNames.find(aeroName)->second;
    meshlink2.type = (int) type;
    meshio->meshLinks.emplace_back(meshlink2);


    // instance

    voxelEntity.scales.resize(voxelEntity.primDistributions.size(), 1.0f);
    voxelEntity.rotations.resize(voxelEntity.primDistributions.size(), 0.0f);
    for (int kinstance = 0; kinstance < voxelEntity.primDistributions.size(); kinstance++) {
        // voxel instance1��change postion,

        glm::ivec3 shift0;
        shift0 = {voxelEntity.primDistributions[kinstance].x / modelio->stepsize_surface,
                  (voxelEntity.primDistributions[kinstance].z + demHeightAboveMinimum(
                      background, voxelEntity.primDistributions[kinstance].x,
                      voxelEntity.primDistributions[kinstance].y)) / modelio->stepsize_surface,
                  voxelEntity.primDistributions[kinstance].y / modelio->stepsize_surface};


        ///-----------------------------------------------------------------------------
        /// Attention!!!!
        ///------------------------------------------------------------------------------
        glm::ivec3 semiRange = glm::vec3{floor(modelio->voxelSize_XZY.x / 2.0+0.5), 0,
                                         floor(modelio->voxelSize_XZY.z / 2.0+0.5)};;

        float scale0 = voxelEntity.scales[kinstance];
        float angle0 = voxelEntity.rotations[kinstance];
        glm::vec3 shift = glm::vec3(shift0) - glm::vec3(semiRange);  // (5,0,5)

        //
        glm::mat4 unit = glm::mat4(1.0f);
        glm::vec3 scale = glm::vec3(scale0);
        glm::mat4 angle = glm::rotate(unit, glm::radians(-angle0), glm::vec3(0.0, 1.0, 0.0));
        // DEFERRED[中] Bug: 旋转矩阵 angle 计算后未使用；scale*translate 的顺序使平移量也被缩放；而体元插入只用 shift0+localVoxel，完全不考虑 scale/rotation，scale≠1 或 rotation≠0 时实例矩阵与 NanoVDB 体元位置不一致；建议：基元明确不支持缩放/旋转（校验并报错），或按 OBJ 路径用 translate*rotate*scale 并逆映射体元。
        glm::mat4 mat = glm::scale(unit, scale) * glm::translate(unit, shift);

        Instance instance1{};
        instance1.meshId = static_cast<uint32_t>(n_modelmesh);
        instance1.object2worldMatrix = mat;
        instance1.world2objectMatrix = glm::transpose(glm::inverse(mat));
        instanceio->instances.emplace_back(instance1);
        // voxel Instance Link
        InstanceLink instancelink1{};
        instancelink1.meshId = instance1.meshId;
        instanceio->instanceLinks.emplace_back(instancelink1);

        Instance instance2{};
        instance2.meshId = static_cast<uint32_t>(n_modelmesh + 1);
        instance2.object2worldMatrix = mat;
        instance2.world2objectMatrix = glm::transpose(glm::inverse(mat));
        instanceio->instances.emplace_back(instance2);
        // voxel Instance Link
        InstanceLink instancelink2{};
        instancelink2.meshId = instance2.meshId;
        instanceio->instanceLinks.emplace_back(instancelink2);


        // FIXED[中] 内存: isValids[kvoxel] 未校验 isValids.size()==voxelIds.size()（createTriEntitiesFromTif_wall 等生成器若未逐体元填写即越界读）；已修复：循环前把 isValids 补到 voxelIds.size()（int5{} 全零），保证下方索引不越界。
        if (currentVoxelModel1.isValids.size() < currentVoxelModel1.voxelIds.size())
            currentVoxelModel1.isValids.resize(currentVoxelModel1.voxelIds.size());
        int isValid = 0;
        if ((voxelEntity.meshNames.size() == 2) && (voxelEntity.type == Type::BUILDING)) {

            for (int kvoxel = 0; kvoxel < currentVoxelModel1.voxelIds.size(); kvoxel++) {
                // this is what we did in the shader;
                // glm::ivec3 pos =  mat * glm::vec4(currentVoxelModel.voxelIds[kvoxel],1.0);
                glm::ivec3 Id = shift0 + glm::ivec3(currentVoxelModel1.voxelIds[kvoxel]);
                int test = acc.getValue(nanovdb::Coord(Id.x, Id.y, Id.z));

                // the first bufferid;

                if (test < 0) {
                    ///-----------------------------------------------------------------------------
                    /// Attention!!!! only the first buffer is collected.
                    ///------------------------------------------------------------------------------

                    acc.setValue(nanovdb::Coord(Id.x, Id.y, Id.z), n_voxel);
                    glm::ivec3 voxelPos = glm::ivec3(Id.x * 1.0, Id.y * 1.0, Id.z * 1.0);  //(5,0,5) with height = 0


                    for (int kf = 0; kf < 4; kf++) {
                        VoxelLink voxelLink{};
                        voxelLink.voxelId = voxelPos;          //(5,0,5) with height = 0
                        voxelLink.instanceId = n_instance;
                        voxelLink.aeroId = 0;
                        voxelLink.faceId = kf+1; // center
                        voxelLink.isValid = currentVoxelModel1.isValids[kvoxel].values[kf]; // center
                        modelio->m_voxelio->voxellinks.emplace_back(voxelLink);

                        n_voxel++;
                        isValid = 1;
                    }

                    for (int kf = 4; kf < 5; kf++) {
                        VoxelLink voxelLink{};
                        voxelLink.voxelId = voxelPos;          //(5,0,5) with height = 0
                        voxelLink.instanceId = n_instance + 1;
                        voxelLink.aeroId = 0;
                        voxelLink.faceId = kf+1; // center
                        voxelLink.isValid = currentVoxelModel1.isValids[kvoxel].values[kf];
                        modelio->m_voxelio->voxellinks.emplace_back(voxelLink);

                        n_voxel++;
                        isValid = 1;
                    }


                }

            }

            n_instance += 2;

        }



        //std::string info = "voxel entity " + std::to_string(kVoxelModel) + " done.\n";
        //LOGI(info.c_str());
        // float tt = acc.getValue(nanovdb::Coord(24, 0, 24));
    }
    // All placements share the wall and roof meshes created once above.
    n_modelmesh += 2;
    return true;
}

bool Scene::createPrimObj_Background(Background & background,nanovdb::GridBuilder<int32_t> &nanoBuilder, std::shared_ptr<VoxelebIO> &modelio){

    auto acc = nanoBuilder.getAccessor();
    auto &meshio = modelio->m_meshio;
    auto &instanceio = modelio->m_instanceio;
    auto &voxelio = modelio->m_voxelio;
    auto &surfio = modelio->m_surfio;


    modelio->sceneSize_XYZ =  background.sceneSize;
    modelio->sceneOrigin_XYZ = background.sceneOrigin;
    // FIXED[中] 数值: 未校验 stepsize_surface>0，为 0 时除零得到 inf，转换为 int 属未定义行为；float→int 截断使 sceneSize 不是步长整数倍时最后不足一格的条带被丢弃，而 DEM 背景用 floor、OBJ 路径用 floor(x/2+0.5) 求半宽，三处取整规则不一致可能产生半格偏移；已修复：入口校验 stepsize_surface 为有限正数，否则抛异常（取整规则的统一仍为 DEFERRED）。
    if (!(background.stepsize_surface > 0.0f) || !std::isfinite(background.stepsize_surface))
        throw std::runtime_error("Background stepsize_surface must be a finite positive value: " +
                                 std::to_string(background.stepsize_surface));
    modelio->voxelSize_XZY = glm::ivec3(background.sceneSize.x / background.stepsize_surface,
                                        0,
                                        background.sceneSize.y / background.stepsize_surface);
    modelio->voxelOrigin_XZY = glm::ivec3(background.sceneOrigin.x / background.stepsize_surface,
                                          0,
                                          background.sceneOrigin.y / background.stepsize_surface);

    modelio->stepsize_surface = background.stepsize_surface;
    modelio->stepsize_height = background.stepsize_height;
    modelio->lat = background.lat;
    modelio->lon = background.lon;
    modelio->m_year = background.year;
    modelio->m_timezone = background.timezone;
//    modelio->stepsize_surface = 1.0;
    //modelio->n_surface = modelio->voxelSize_XZY.x * modelio->voxelSize_XZY.y;

    ///----------------------------------------------------------------
    /// From now on, using voxel space
    ///----------------------------------------------------------------

    int &n_modelmesh = modelio->n_modelmesh;
    int &n_instance = modelio->n_instance;
    int &n_voxel = modelio->n_voxel;

    /// ------------------------------------
    /// BACKGROUND mesh
    ///-------------------------------------

    VoxelDesigner loader;
    PrimMesh bgModelXYZ;
    if ((background.isDEM == true) && (background.DEMFile != "")) {
        bgModelXYZ = createVoxelDemBackground(background, modelio->stepsize_surface);
    } else {
        bgModelXYZ = loader.createTriBackground(modelio->sceneSize_XYZ.x, modelio->sceneSize_XYZ.y, background.stepsize_surface);
    }


    bgModelXYZ.meshId = n_modelmesh;
    PrimMesh bgModel = XYZ2XZY(bgModelXYZ);
    meshio->primMeshes.emplace_back(bgModel);

    // background model link
    MeshLink bgMeshLink{};
    // DEFERRED[低] 错误处理: 背景热属性名未找到时静默取 0 号，与基元/OBJ 路径的 mappedId（会告警）不一致；建议：改用 mappedId。
    const auto thermalIt = meshio->thermalNames.find(background.bgThermalName);
    bgMeshLink.thermalId = thermalIt == meshio->thermalNames.end() ? 0 : thermalIt->second;
    bgMeshLink.canopyId = 0;
    std::string bgSpectralName = background.bgSpectralName;
    bgMeshLink.spectralId = requiredId(meshio->spectralNames, bgSpectralName, "spectral");
//    std::string bgThermalName;
//    int bgThermalIndex = 0;
//    if (fileio->m_pVoxelebXml->sensorxml.isTemperature == true)
//    {
//        bgThermalName = scenexml.background.bgThermalName;
//        bgThermalIndex = meshio->thermalNames.find(bgThermalName)->second;
//    }

    int bgCanopyindex = 0;
    //  std::string bgCanopyName = scenexml.background.canopyName;
    //  bgMeshLink.canopyId = meshio->canopyNames.find(bgCanopyName)->second;
    int bgPropIndex = 0;
    std::string bgPropName = background.bgPropName;
    if (background.type == Type::WATER) {
        bgPropIndex = requiredId(meshio->watersetNames, bgPropName, "water property");
    } else {
        bgPropIndex = requiredId(meshio->soilsetNames, bgPropName, "soil property");
    }
    bgMeshLink.bioId = bgPropIndex;
    bgMeshLink.type = static_cast<int>(background.type);
    bgMeshLink.angularEffectStrength = background.angularEffectStrength;
    meshio->meshLinks.emplace_back(bgMeshLink);

    /// ------------------------------------
    /// BACKGROUND instance
    /// Voxelsize is XZY
    /// iN cg space: (voxelsize.x - voxelsize.x/2, voxelsize.y, voxelsize.z - voxelsize.z/2)
    ///-------------------------------------
    Instance bgInstance{};
    bgInstance.meshId = static_cast<uint32_t>(n_modelmesh);
    glm::mat4 bgunit = glm::mat4(1.0f);
    // here for 0-9 will become -5 - 4
    glm::vec3 bgShift = glm::vec3{-floor(modelio->voxelSize_XZY.x / 2.0+0.5), 0 - loader.minElevation,
                                  -floor(modelio->voxelSize_XZY.z / 2.0+0.5)};

//    glm::vec3 bgShift = glm::vec3{0,0,0};

    glm::vec3 bgScale = glm::vec3{1.0, 1.0, 1.0};
    glm::mat4 bgMat = glm::scale(bgunit, bgScale) * glm::translate(bgunit, bgShift);
    bgInstance.object2worldMatrix = bgMat;
    bgInstance.world2objectMatrix = glm::transpose(glm::inverse(bgMat));
    instanceio->instances.emplace_back(bgInstance);


    // background instanceLink
    InstanceLink bgInstanceLink{};
    bgInstanceLink.meshId = bgInstance.meshId;
    instanceio->instanceLinks.emplace_back(bgInstanceLink);

    //--------------------------------------------
    // background voxelLink
    //--------------------------------------------
    for (int kvoxel = 0; kvoxel < bgModel.voxelIds.size(); kvoxel++) {
        glm::ivec3 voxelId = glm::ivec3(bgModel.voxelIds[kvoxel]);// + nvmath::vec3i(bgShift);
        int test = acc.getValue(nanovdb::Coord(voxelId.x, voxelId.y, voxelId.z)); //(1,0,1)

        if (test < 0) {
            acc.setValue(nanovdb::Coord(voxelId.x, voxelId.y, voxelId.z), n_voxel);
            VoxelLink voxellink{};
            glm::ivec3 voxelId_ = glm::ivec3(voxelId.x * 1.0, voxelId.y * 1.0, voxelId.z * 1.0);
            voxellink.voxelId = voxelId_;
            voxellink.instanceId = n_instance;
            voxellink.faceId = 5;
            voxellink.isValid = 1;
            voxellink.aeroId = 0;

            voxelio->voxellinks.emplace_back(voxellink);
            n_voxel++;
        }
    }
    n_instance++;
    n_modelmesh++;


    if(background.isLad==true){
        // read lad tif
        int width = 0,height = 0, nband = 1;
        // FIXED[中] 错误处理: readImageinout1 的返回值被忽略，LAD 影像打不开时 lads 保持为空（不会走 else 分支补 0），着色器按体元索引读取空缓冲；已修复：检查返回值，读取失败或结果为空时抛出带路径的异常，避免零长度 lads 被按体元索引；影像 width×height 与背景网格尺寸的一致性校验仍为 DEFERRED。
        if (Utils::readImageinout1(background.ladfile,surfio->lads,width, height,nband) == 0 ||
            surfio->lads.empty())
            throw std::runtime_error("Cannot read LAD image: " + background.ladfile);
    }else{
        surfio->lads.emplace_back(0);
    }

    /// ------------------------------------
    ///  Divided  Background, Need to be provided
    ///-------------------------------------

    /// ------------------------------------
    ///  Divided  Background, Need to be provided
    ///-------------------------------------

    return true;
}

//-----------------------------------------------------------
//--- n_moxelmesh,n_instance,n_voxel
//--- primMeshes, meshlink, instance, instanceLink, nanovdb, voxellink
//------------------------------------------------------------

bool Scene::createPrimObjScene(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &voxellstio) {


    auto &scenexml = fileio->m_pVoxelrtXml->scenexml;
    auto &voxellstxml = fileio->m_pVoxelrtXml;
    auto &meshio = voxellstio->m_meshio;
    auto &instanceio = voxellstio->m_instanceio;
    auto &voxelio = voxellstio->m_voxelio;
    auto &background = fileio->m_pVoxelrtXml->scenexml.background;
    nanovdb::GridBuilder<int32_t> nanoBuilder(-1);
    HexMixAccumulator hexMixtures;


    /// ------------------------------------
    /// Background
    ///-------------------------------------
    createPrimObj_Background(background, nanoBuilder, voxellstio);

    /// ------------------------------------
    /// voxel Components
    ///-------------------------------------
    for (int kVoxelModel = 0; kVoxelModel < scenexml.primEntities.size(); kVoxelModel++)
    {
        auto &voxelEntity = scenexml.primEntities[kVoxelModel];

        if (voxelEntity.voxelizeFromObj) {
            createObjFilledVoxels(this, voxelEntity, nanoBuilder, voxellstio,
                                  background,
                                  voxellstio->heterogeneousVoxel
                                      ? &hexMixtures : nullptr);
            continue;
        }


        if (voxelEntity.type == Type::VEGETATION)
        {
            if(voxelEntity.isshapeFromFile == true)
            {
                createPrimObj_Crowns(voxelEntity, nanoBuilder, voxellstio, background);
            }else {
                createPrimObj_Crown(voxelEntity, nanoBuilder, voxellstio, background);
            }
        }else if(voxelEntity.type == Type::BUILDING)
        {
            createPrimObj_Building(voxelEntity, nanoBuilder, voxellstio, background);
        }

    }
    if (voxellstio->heterogeneousVoxel) {
        finalizeHexMixtures(hexMixtures, voxellstio);
    }
    voxellstio->m_voxelio->nanoHandle = nanoBuilder.getHandle<>();


    return true;
}

bool Scene::createPrimObj_Crown(PrimEntity & voxelEntity,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelrtIO> &modelio, const Background& background){

    auto acc = nanoBuilder.getAccessor();
    auto &meshio = modelio->m_meshio;
    auto &instanceio = modelio->m_instanceio;
    auto &voxelio = modelio->m_voxelio;
    int &n_modelmesh = modelio->n_modelmesh;
    int &n_instance = modelio->n_instance;
    int &n_voxel = modelio->n_voxel;

    VoxelDesigner loader;
    /// ------------------------------------
    /// voxel model/mesh
    ///-------------------------------------
    std::string modelName = voxelEntity.primitiveName;
    Type type = voxelEntity.type;
    Shape shape = voxelEntity.shape;
    //meshio->types[0] = voxelEntity.types[0];
    // std::string aeroName = voxelEntity.aeroNames[0];


    PrimMesh currentVoxelModelXYZ1;
    currentVoxelModelXYZ1 = loader.createTriEntity(shape, modelio->stepsize_surface);
    PrimMesh currentVoxelModel1 = XYZ2XZY(currentVoxelModelXYZ1, 0); // (1,1,0) => (1,0,1) with  height = 0
    currentVoxelModel1.meshId = n_modelmesh;
    meshio->primMeshes.emplace_back(currentVoxelModel1); //xzy
    MeshLink meshlink1{};
    meshlink1.spectralId = requiredMappedId(meshio->spectralNames,
                                    voxelEntity.spectralNames, 0, "spectral");
    meshlink1.thermalId = mappedId(meshio->thermalNames,
                                   voxelEntity.thermalNames, 0, "thermal");
    meshlink1.canopyId = type == Type::WATER ? 0 :
        mappedId(meshio->canopyNames, voxelEntity.canopyNames, 0, "canopy");
//    if (type == Type::VEGETATION) {
//        // meshlink1.leafbioId = meshio->leafbioNames.find(propName)->second;
//        meshlink1.bioId = meshio->leafbioNames.find(propName1)->second;
//    } else if (type == Type::SOIL || type == Type::BUILDING) {
//        //  meshlink1.soilsetId = meshio->soilsetNames.find(propName)->second;
//        meshlink1.bioId = meshio->soilsetNames.find(propName1)->second;
//    }
    //  meshlink1.aeroId = meshio->aeroNames.find(aeroName)->second;
    meshlink1.type = (int) type;
    meshio->meshLinks.emplace_back(meshlink1);

    const Canopy* mediumCanopy = meshlink1.canopyId >= 0 &&
        static_cast<size_t>(meshlink1.canopyId) < meshio->canopies.size()
        ? &meshio->canopies[meshlink1.canopyId] : nullptr;
    const bool isParticipatingMedium = mediumCanopy != nullptr &&
        (mediumCanopy->structureType == 2 || mediumCanopy->structureType == 3);



    if(voxelEntity.isdisFromFile == true){
        // Primitive crowns are placed unscaled and unrotated.
        const auto records = Utils::readPositionFile(voxelEntity.distributefile);
        voxelEntity.primDistributions.resize(records.size());
        voxelEntity.scales.assign(records.size(), 1.0f);
        voxelEntity.rotations.assign(records.size(), 0.0f);
        for (size_t kin = 0; kin < records.size(); ++kin)
        {
            voxelEntity.primDistributions[kin] = glm::vec3(records[kin].x, records[kin].y, records[kin].z);
        }
    }

    // instance

    voxelEntity.scales.resize(voxelEntity.primDistributions.size(), 1.0f);
    voxelEntity.rotations.resize(voxelEntity.primDistributions.size(), 0.0f);
    for (int kinstance = 0; kinstance < voxelEntity.primDistributions.size(); kinstance++) {
        // voxel instance1��change postion,

        const glm::vec3 placement = voxelEntity.primDistributions[kinstance];
        // Fire and fog positions are specified by horizontal centre and bottom
        // height. Primitive voxel geometry starts at its minimum corner.
        const float originX = placement.x -
            (isParticipatingMedium ? shape.length * 0.5f : 0.0f);
        const float originGroundY = placement.y -
            (isParticipatingMedium ? shape.width * 0.5f : 0.0f);
        glm::ivec3 shift0;
        // DEFERRED[中] 数值: float→int 隐式截断是向零取整，负坐标（如火/雾中心减半宽后 originX<0）会偏移一格，与 OBJ 路径使用的 std::floor 不一致；且 placement.z 与 DEM 高度相加后按 stepsize_surface 而非 stepsize_height 换算；建议：显式 std::floor，高度轴使用 stepsize_height。
        shift0 = {originX / modelio->stepsize_surface,
                  (placement.z + demHeightAboveMinimum(
                      background, placement.x, placement.y)) / modelio->stepsize_surface,
                  originGroundY / modelio->stepsize_surface};
        // FIXED[高] 内存: PrimEntity 默认 scales{1}/rotations{0} 只有 1 个元素，JSON 直接给出多个 primDistributions 而未给等长 scales/rotations 时（非文件路径），下方 scales[kinstance]/rotations[kinstance] 越界读；已修复：进入实例循环前按 primDistributions.size() resize scales/rotations（补齐 1.0f/0.0f）。
        // FIXED[中] Bug: 下方 Instance/InstanceLink 在插入体元之前 push_back，但本次放置若没有新增体元（isValid==0：全部与已有体元重叠或越界）会 continue 而不递增 n_instance，instances.size() 与 n_instance 脱节，后续 voxelLink.instanceId 指向错误实例（变换/meshId 错位）；已修复：isValid==0 时 pop_back 刚压入的 instances/instanceLinks 再 continue，使 instances.size() 始终等于 n_instance。

        ///-----------------------------------------------------------------------------
        ///
        ///------------------------------------------------------------------------------
        glm::ivec3 semiRange = glm::vec3{floor(modelio->voxelSize_XZY.x / 2.0+0.5), 0,
                                         floor(modelio->voxelSize_XZY.z / 2.0+0.5)};;

        float scale0 = voxelEntity.scales[kinstance];
        float angle0 = voxelEntity.rotations[kinstance];
        glm::vec3 shift = glm::vec3(shift0) - glm::vec3(semiRange);  // (5,0,5)

        //
        glm::mat4 unit = glm::mat4(1.0f);
        glm::vec3 scale = glm::vec3(scale0);
        glm::mat4 angle = glm::rotate(unit, glm::radians(-angle0), glm::vec3(0.0, 1.0, 0.0));
        // DEFERRED[中] Bug: 旋转矩阵 angle 计算后未使用；scale*translate 的顺序使平移量也被缩放；而体元插入只用 shift0+localVoxel，完全不考虑 scale/rotation，scale≠1 或 rotation≠0 时实例矩阵与 NanoVDB 体元位置不一致；建议：基元明确不支持缩放/旋转（校验并报错），或按 OBJ 路径用 translate*rotate*scale 并逆映射体元。
        glm::mat4 mat = glm::scale(unit, scale) * glm::translate(unit, shift);

        Instance instance1{};
        instance1.meshId = static_cast<uint32_t>(n_modelmesh);
        instance1.object2worldMatrix = mat;
        instance1.world2objectMatrix = glm::transpose(glm::inverse(mat));
        instanceio->instances.emplace_back(instance1);
        // voxel Instance Link
        InstanceLink instancelink1{};
        instancelink1.meshId = instance1.meshId;
        instanceio->instanceLinks.emplace_back(instancelink1);


        int isValid = 0;
        int insertedVoxelCount = 0;
        int occupiedVoxelCount = 0;
        int mediumVoxelCount = 0;

        for (int kvoxel = 0; kvoxel < currentVoxelModel1.voxelIds.size(); kvoxel++) {
            // this is what we did in the shader;
            // glm::ivec3 pos =  mat * glm::vec4(currentVoxelModel.voxelIds[kvoxel],1.0);
            const glm::ivec3 localVoxel = glm::ivec3(currentVoxelModel1.voxelIds[kvoxel]);
            if (isParticipatingMedium &&
                !keepParticipatingMediumVoxel(*mediumCanopy, localVoxel, shape,
                                              modelio->stepsize_surface)) continue;
            if (isParticipatingMedium) mediumVoxelCount++;
            glm::ivec3 Id = shift0 + localVoxel;
            int test = acc.getValue(nanovdb::Coord(Id.x, Id.y, Id.z));

            // the first bufferid;

            if (test < 0) {

                acc.setValue(nanovdb::Coord(Id.x, Id.y, Id.z), n_voxel);
                glm::ivec3 voxelPos = glm::ivec3(Id.x * 1.0, Id.y * 1.0, Id.z * 1.0);  //(5,0,5) with height = 0

                VoxelLink voxelLink{};
                voxelLink.voxelId = voxelPos;          //(5,0,5) with height = 0
                voxelLink.instanceId = n_instance;
                voxelLink.aeroId = 0;
                voxelLink.faceId = 0; // center
                voxelLink.isValid = 1; // center
                modelio->m_voxelio->voxellinks.emplace_back(voxelLink);
                n_voxel++;
                isValid = 1;
                insertedVoxelCount++;

            } else if (isParticipatingMedium) {
                occupiedVoxelCount++;
            }
        }
        if (isParticipatingMedium) {
            std::cout << "Participating medium voxelization: "
                      << voxelEntity.primitiveName
                      << " placement=" << kinstance
                      << " candidates=" << currentVoxelModel1.voxelIds.size()
                      << " medium-shape=" << mediumVoxelCount
                      << " inserted=" << insertedVoxelCount
                      << " occupied=" << occupiedVoxelCount << '\n';
        }
        if (isValid == 0) {
            instanceio->instances.pop_back();
            instanceio->instanceLinks.pop_back();
            continue;
        }

        n_instance++;

        //std::string info = "voxel entity " + std::to_string(kVoxelModel) + " done.\n";
        //LOGI(info.c_str());
        // float tt = acc.getValue(nanovdb::Coord(24, 0, 24));
    }
    n_modelmesh++;
    return true;
}

bool Scene::createPrimObj_Crowns(PrimEntity & voxelEntity,nanovdb::GridBuilder<int32_t> &nanoBuilder,std::shared_ptr<VoxelrtIO> &modelio, const Background& background){

    auto acc = nanoBuilder.getAccessor();
    auto &meshio = modelio->m_meshio;
    auto &instanceio = modelio->m_instanceio;
    auto &voxelio = modelio->m_voxelio;
    int &n_modelmesh = modelio->n_modelmesh;
    int &n_instance = modelio->n_instance;
    int &n_voxel = modelio->n_voxel;

    VoxelDesigner loader;
    /// ------------------------------------
    /// voxel model/mesh
    ///-------------------------------------
    std::string modelName = voxelEntity.primitiveName;
    Type type = voxelEntity.type;

    //meshio->types[0] = voxelEntity.types[0];
    // std::string aeroName = voxelEntity.aeroNames[0];


    // Rows: shapeType height width length x y z [scale rotation (unused)].
    size_t skippedShapeRows = 0;
    const auto shapeRows = Utils::readNumericRows(voxelEntity.shapefile, "crown shape file", skippedShapeRows);
    if (skippedShapeRows > 0)
        throw std::runtime_error("Crown shape file has non-numeric rows: " + voxelEntity.shapefile);
    for (const auto& row : shapeRows) {
        if (row.size() < 7)
            throw std::runtime_error("Crown shape rows need 7 columns (type height width length x y z): " + voxelEntity.shapefile);
    }

    for (size_t k = 0; k < shapeRows.size(); k++) {
        const auto& row = shapeRows[k];

        // FIXED[中] 安全: 形状类型由浮点列直接强转为枚举，未校验 0..2 范围；height/width/length 未校验 >0，非法值使 createTriEntity 产生 0 个或海量体元；已修复：静默强转前校验 row[0]∈[0,2]（ShapeType 只有 CUBE/ELLIPSOID/PLANE），并校验三个尺寸为有限且非负，非法时抛出带形状文件名的异常。
        if (!std::isfinite(row[0]) || row[0] < 0.0f || row[0] > 2.0f)
            throw std::runtime_error("Crown shape type must be 0, 1 or 2: " + voxelEntity.shapefile);
        if (!(row[1] >= 0.0f) || !(row[2] >= 0.0f) || !(row[3] >= 0.0f) ||
            !std::isfinite(row[1]) || !std::isfinite(row[2]) || !std::isfinite(row[3]))
            throw std::runtime_error("Crown shape height/width/length must be finite and non-negative: " + voxelEntity.shapefile);
        Shape shape = Shape{ShapeType(static_cast<int>(row[0])), row[1], row[2], row[3], {row[4], row[5], row[6]}};


        PrimMesh currentVoxelModelXYZ1;
        currentVoxelModelXYZ1 = loader.createTriEntity(shape, modelio->stepsize_surface);
        PrimMesh currentVoxelModel1 = XYZ2XZY(currentVoxelModelXYZ1, 1); // (1,1,0) => (1,0,1) with  height = 0
        currentVoxelModel1.meshId = n_modelmesh;
        meshio->primMeshes.emplace_back(currentVoxelModel1); //xzy
        // FIXED[中] 内存: meshNames/spectralNames/canopyNames/propNames 直接取 [0] 未判空，缺少任一绑定即越界；下方 MeshLink meshlink1 未值初始化（水体类型时 bioId 及 vertexAddress 等为垃圾值），thermalId 固定为 0 而忽略 thermalNames；已修复：改用 requiredMappedId/mappedId（内含空表判空与下标钳制）取首个绑定，MeshLink 改为 meshlink1{} 值初始化；thermalId 仍为 0，行为不变。
        std::string meshName1 = voxelEntity.meshNames.empty()
            ? voxelEntity.primitiveName : voxelEntity.meshNames.front();
        std::string propName1 = voxelEntity.propNames.empty()
            ? std::string() : voxelEntity.propNames.front();
        MeshLink meshlink1{};
        meshlink1.spectralId = requiredMappedId(meshio->spectralNames, voxelEntity.spectralNames, 0, "spectral");
        meshlink1.thermalId = 0;
        meshlink1.canopyId = mappedId(meshio->canopyNames, voxelEntity.canopyNames, 0, "canopy");
//        if (type == Type::VEGETATION) {
//            // meshlink1.leafbioId = meshio->leafbioNames.find(propName)->second;
//            meshlink1.bioId = meshio->leafbioNames.find(propName1)->second;
//        } else if (type == Type::SOIL || type == Type::BUILDING) {
//            //  meshlink1.soilsetId = meshio->soilsetNames.find(propName)->second;
//            meshlink1.bioId = meshio->soilsetNames.find(propName1)->second;
//        }
        //  meshlink1.aeroId = meshio->aeroNames.find(aeroName)->second;
        meshlink1.type = (int) type;
        meshio->meshLinks.emplace_back(meshlink1);

        // instance


        for (int kinstance = 0; kinstance < 1; kinstance++) {
            // voxel instance1��change postion,

            glm::ivec3 shift0;
            shift0 = {shape.pos.x / modelio->stepsize_surface,
                      (shape.pos.z + demHeightAboveMinimum(background, shape.pos.x, shape.pos.y)) / modelio->stepsize_surface,
                      shape.pos.y / modelio->stepsize_surface};

            ///-----------------------------------------------------------------------------
            /// Attention!!!!
            ///------------------------------------------------------------------------------
            glm::ivec3 semiRange = glm::vec3{floor(modelio->voxelSize_XZY.x / 2.0+0.5), 0,
                                             floor(modelio->voxelSize_XZY.z / 2.0+0.5)};;

            float scale0 = 1;
            float angle0 = 0;
            glm::vec3 shift = glm::vec3(shift0) - glm::vec3(semiRange);  // (5,0,5)

            //
            glm::mat4 unit = glm::mat4(1.0f);
            glm::vec3 scale = glm::vec3(scale0);
            glm::mat4 angle = glm::rotate(unit, glm::radians(angle0), glm::vec3(0.0, 1.0, 0.0));
            glm::mat4 mat = glm::scale(unit, scale) * glm::translate(unit, shift);

            Instance instance1{};
            instance1.meshId = static_cast<uint32_t>(n_modelmesh);
            instance1.object2worldMatrix = mat;
            instance1.world2objectMatrix = glm::transpose(glm::inverse(mat));
            instanceio->instances.emplace_back(instance1);
            // voxel Instance Link
            InstanceLink instancelink1{};
            instancelink1.meshId = instance1.meshId;
            instanceio->instanceLinks.emplace_back(instancelink1);


            int isValid = 0;

            for (int kvoxel = 0; kvoxel < currentVoxelModel1.voxelIds.size(); kvoxel++) {
                // this is what we did in the shader;
                // glm::ivec3 pos =  mat * glm::vec4(currentVoxelModel.voxelIds[kvoxel],1.0);
                glm::ivec3 Id = shift0 + glm::ivec3(currentVoxelModel1.voxelIds[kvoxel]);
                int test = acc.getValue(nanovdb::Coord(Id.x, Id.y, Id.z));

                // the first bufferid;

                if (test < 0) {

                    acc.setValue(nanovdb::Coord(Id.x, Id.y, Id.z), n_voxel);
                    glm::ivec3 voxelPos = glm::ivec3(Id.x * 1.0, Id.y * 1.0, Id.z * 1.0);  //(5,0,5) with height = 0

                    VoxelLink voxelLink{};
                    voxelLink.voxelId = voxelPos;          //(5,0,5) with height = 0
                    voxelLink.instanceId = n_instance;
                    voxelLink.aeroId = 0;
                    voxelLink.faceId = 0; // center
                    voxelLink.isValid = 1; // center
                    modelio->m_voxelio->voxellinks.emplace_back(voxelLink);
                    n_voxel++;
                    isValid = 1;

                }
            }
            // FIXED[中] Bug: isValid==0 时已经 push 实例但 n_instance 未递增，后续 voxelLink.instanceId 与 instances 下标错位（与 1844/2590 同源，原文件未标注）；已修复：pop_back 撤销本次压入
            if (isValid == 0) {
                instanceio->instances.pop_back();
                instanceio->instanceLinks.pop_back();
                continue;
            }

            n_instance++;

            //std::string info = "voxel entity " + std::to_string(kVoxelModel) + " done.\n";
            //LOGI(info.c_str());
            // float tt = acc.getValue(nanovdb::Coord(24, 0, 24));
        }
        n_modelmesh++;
    }
    return true;
}

bool Scene::createPrimObj_Building(PrimEntity & voxelEntity,nanovdb::GridBuilder<int32_t> &nanoBuilder, std::shared_ptr<VoxelrtIO> &modelio, const Background& background) {
    preparePrimitiveBuilding(voxelEntity);

    auto acc = nanoBuilder.getAccessor();
    auto &meshio = modelio->m_meshio;
    auto &instanceio = modelio->m_instanceio;
    auto &voxelio = modelio->m_voxelio;
    int &n_modelmesh = modelio->n_modelmesh;
    int &n_instance = modelio->n_instance;
    int &n_voxel = modelio->n_voxel;

    VoxelDesigner loader;
    /// ------------------------------------
    /// voxel model/mesh
    ///-------------------------------------
    std::string modelName = voxelEntity.primitiveName;
    Type type = voxelEntity.type;
    Shape shape = voxelEntity.shape;
    //meshio->types[0] = voxelEntity.types[0];
    // std::string aeroName = voxelEntity.aeroNames[0];


    PrimMesh currentVoxelModelXYZ1;
    if (voxelEntity.isheightFromFile == 1) {
        currentVoxelModelXYZ1 = loader.createTriEntitiesFromTif_wall(voxelEntity.heightfile, modelio->voxelSize_XZY,modelio->stepsize_height);
    } else {
        currentVoxelModelXYZ1 = loader.createTriCube_wall(shape, modelio->stepsize_surface);
    }

    PrimMesh currentVoxelModel1 = XYZ2XZY(currentVoxelModelXYZ1, 1); // (1,1,0) => (1,0,1) with  height = 0
    currentVoxelModel1.meshId = n_modelmesh;
    meshio->primMeshes.emplace_back(currentVoxelModel1); //xzy
    MeshLink meshlink1{};
    meshlink1.spectralId = requiredMappedId(meshio->spectralNames, voxelEntity.spectralNames, 0, "spectral");
    meshlink1.thermalId = mappedId(meshio->thermalNames, voxelEntity.thermalNames, 0, "thermal");
    meshlink1.canopyId = mappedId(meshio->canopyNames, voxelEntity.canopyNames, 0, "canopy");
    //  meshlink1.aeroId = meshio->aeroNames.find(aeroName)->second;
    meshlink1.type = (int) type;
    meshio->meshLinks.emplace_back(meshlink1);

    PrimMesh currentVoxelModel2{};
    PrimMesh currentVoxelModelXYZ2;
    if (voxelEntity.isheightFromFile == 1) {
        currentVoxelModelXYZ2 = loader.createTriEntitiesFromTif_roof(voxelEntity.heightfile, modelio->voxelSize_XZY,modelio->stepsize_height);
    } else {
        currentVoxelModelXYZ2 = loader.createTriCube_roof(shape, modelio->stepsize_surface);
    }

    currentVoxelModel2 = XYZ2XZY(currentVoxelModelXYZ2, 1); // (1,1,0) => (1,0,1) with  height = 0
    currentVoxelModel2.meshId = n_modelmesh + 1;
    meshio->primMeshes.emplace_back(currentVoxelModel2); //xzy

    /// ------------------------------------
    /// voxel model/mesh
    ///-------------------------------------
    MeshLink meshlink2{};
    meshlink2.spectralId = requiredMappedId(meshio->spectralNames, voxelEntity.spectralNames, 1, "spectral");
    meshlink2.thermalId = mappedId(meshio->thermalNames, voxelEntity.thermalNames, 1, "thermal");
    meshlink2.canopyId = mappedId(meshio->canopyNames, voxelEntity.canopyNames, 1, "canopy");
    //  meshlink1.aeroId = meshio->aeroNames.find(aeroName)->second;
    meshlink2.type = (int) type;
    meshio->meshLinks.emplace_back(meshlink2);


    // instance

    voxelEntity.scales.resize(voxelEntity.primDistributions.size(), 1.0f);
    voxelEntity.rotations.resize(voxelEntity.primDistributions.size(), 0.0f);
    for (int kinstance = 0; kinstance < voxelEntity.primDistributions.size(); kinstance++) {
        // voxel instance1��change postion,

        glm::ivec3 shift0;
        shift0 = {voxelEntity.primDistributions[kinstance].x / modelio->stepsize_surface,
                  (voxelEntity.primDistributions[kinstance].z + demHeightAboveMinimum(
                      background, voxelEntity.primDistributions[kinstance].x,
                      voxelEntity.primDistributions[kinstance].y)) / modelio->stepsize_surface,
                  voxelEntity.primDistributions[kinstance].y / modelio->stepsize_surface};


        ///-----------------------------------------------------------------------------
        /// Attention!!!!
        ///------------------------------------------------------------------------------
        glm::ivec3 semiRange = glm::vec3{floor(modelio->voxelSize_XZY.x / 2.0+0.5), 0,
                                         floor(modelio->voxelSize_XZY.z / 2.0+0.5)};;

        float scale0 = voxelEntity.scales[kinstance];
        float angle0 = voxelEntity.rotations[kinstance];
        glm::vec3 shift = glm::vec3(shift0) - glm::vec3(semiRange);  // (5,0,5)

        //
        glm::mat4 unit = glm::mat4(1.0f);
        glm::vec3 scale = glm::vec3(scale0);
        glm::mat4 angle = glm::rotate(unit, glm::radians(-angle0), glm::vec3(0.0, 1.0, 0.0));
        // DEFERRED[中] Bug: 旋转矩阵 angle 计算后未使用；scale*translate 的顺序使平移量也被缩放；而体元插入只用 shift0+localVoxel，完全不考虑 scale/rotation，scale≠1 或 rotation≠0 时实例矩阵与 NanoVDB 体元位置不一致；建议：基元明确不支持缩放/旋转（校验并报错），或按 OBJ 路径用 translate*rotate*scale 并逆映射体元。
        glm::mat4 mat = glm::scale(unit, scale) * glm::translate(unit, shift);

        Instance instance1{};
        instance1.meshId = static_cast<uint32_t>(n_modelmesh);
        instance1.object2worldMatrix = mat;
        instance1.world2objectMatrix = glm::transpose(glm::inverse(mat));
        instanceio->instances.emplace_back(instance1);
        // voxel Instance Link
        InstanceLink instancelink1{};
        instancelink1.meshId = instance1.meshId;
        instanceio->instanceLinks.emplace_back(instancelink1);

        Instance instance2{};
        instance2.meshId = static_cast<uint32_t>(n_modelmesh + 1);
        instance2.object2worldMatrix = mat;
        instance2.world2objectMatrix = glm::transpose(glm::inverse(mat));
        instanceio->instances.emplace_back(instance2);
        // voxel Instance Link
        InstanceLink instancelink2{};
        instancelink2.meshId = instance2.meshId;
        instanceio->instanceLinks.emplace_back(instancelink2);


        // FIXED[中] 内存: isValids[kvoxel] 未校验 isValids.size()==voxelIds.size()（createTriEntitiesFromTif_wall 等生成器若未逐体元填写即越界读）；已修复：循环前把 isValids 补到 voxelIds.size()（int5{} 全零），保证下方索引不越界。
        if (currentVoxelModel1.isValids.size() < currentVoxelModel1.voxelIds.size())
            currentVoxelModel1.isValids.resize(currentVoxelModel1.voxelIds.size());
        int isValid = 0;
        if ((voxelEntity.meshNames.size() == 2) && (voxelEntity.type == Type::BUILDING)) {

            for (int kvoxel = 0; kvoxel < currentVoxelModel1.voxelIds.size(); kvoxel++) {
                // this is what we did in the shader;
                // glm::ivec3 pos =  mat * glm::vec4(currentVoxelModel.voxelIds[kvoxel],1.0);
                glm::ivec3 Id = shift0 + glm::ivec3(currentVoxelModel1.voxelIds[kvoxel]);
                int test = acc.getValue(nanovdb::Coord(Id.x, Id.y, Id.z));

                // the first bufferid;

                if (test < 0) {
                    ///-----------------------------------------------------------------------------
                    /// Attention!!!! only the first buffer is collected.
                    ///------------------------------------------------------------------------------

                    acc.setValue(nanovdb::Coord(Id.x, Id.y, Id.z), n_voxel);
                    glm::ivec3 voxelPos = glm::ivec3(Id.x * 1.0, Id.y * 1.0, Id.z * 1.0);  //(5,0,5) with height = 0


                    for (int kf = 0; kf < 4; kf++) {
                        VoxelLink voxelLink{};
                        voxelLink.voxelId = voxelPos;          //(5,0,5) with height = 0
                        voxelLink.instanceId = n_instance;
                        voxelLink.aeroId = 0;
                        voxelLink.faceId = kf+1; // center
                        voxelLink.isValid = currentVoxelModel1.isValids[kvoxel].values[kf]; // center
                        modelio->m_voxelio->voxellinks.emplace_back(voxelLink);

                        n_voxel++;
                        isValid = 1;
                    }

                    for (int kf = 4; kf < 5; kf++) {
                        VoxelLink voxelLink{};
                        voxelLink.voxelId = voxelPos;          //(5,0,5) with height = 0
                        voxelLink.instanceId = n_instance + 1;
                        voxelLink.aeroId = 0;
                        voxelLink.faceId = kf+1; // center
                        voxelLink.isValid = currentVoxelModel1.isValids[kvoxel].values[kf];
                        modelio->m_voxelio->voxellinks.emplace_back(voxelLink);

                        n_voxel++;
                        isValid = 1;
                    }


                }

            }

            n_instance += 2;

        }



        //std::string info = "voxel entity " + std::to_string(kVoxelModel) + " done.\n";
        //LOGI(info.c_str());
        // float tt = acc.getValue(nanovdb::Coord(24, 0, 24));
    }
    // All placements share the wall and roof meshes created once above.
    n_modelmesh += 2;
    return true;
}

bool Scene::createPrimObj_Background(Background & background,nanovdb::GridBuilder<int32_t> &nanoBuilder, std::shared_ptr<VoxelrtIO> &modelio){

    auto acc = nanoBuilder.getAccessor();
    auto &meshio = modelio->m_meshio;
    auto &instanceio = modelio->m_instanceio;
    auto &voxelio = modelio->m_voxelio;
    auto &surfio = modelio->m_surfio;


    modelio->sceneSize_XYZ =  background.sceneSize;
    modelio->sceneOrigin_XYZ = background.sceneOrigin;
    // FIXED[中] 数值: 未校验 stepsize_surface>0，为 0 时除零得到 inf，转换为 int 属未定义行为；float→int 截断使 sceneSize 不是步长整数倍时最后不足一格的条带被丢弃，而 DEM 背景用 floor、OBJ 路径用 floor(x/2+0.5) 求半宽，三处取整规则不一致可能产生半格偏移；已修复：入口校验 stepsize_surface 为有限正数，否则抛异常（取整规则的统一仍为 DEFERRED）。
    if (!(background.stepsize_surface > 0.0f) || !std::isfinite(background.stepsize_surface))
        throw std::runtime_error("Background stepsize_surface must be a finite positive value: " +
                                 std::to_string(background.stepsize_surface));
    modelio->voxelSize_XZY = glm::ivec3(background.sceneSize.x / background.stepsize_surface,
                                        0,
                                        background.sceneSize.y / background.stepsize_surface);
    modelio->voxelOrigin_XZY = glm::ivec3(background.sceneOrigin.x / background.stepsize_surface,
                                          0,
                                          background.sceneOrigin.y / background.stepsize_surface);

    modelio->stepsize_surface = background.stepsize_surface;
    modelio->stepsize_height = background.stepsize_height;
    modelio->lat = background.lat;
    modelio->lon = background.lon;
    modelio->m_year = background.year;
    modelio->m_timezone = background.timezone;
//    modelio->stepsize_surface = 1.0;
    //modelio->n_surface = modelio->voxelSize_XZY.x * modelio->voxelSize_XZY.y;

    ///----------------------------------------------------------------
    /// From now on, using voxel space
    ///----------------------------------------------------------------

    int &n_modelmesh = modelio->n_modelmesh;
    int &n_instance = modelio->n_instance;
    int &n_voxel = modelio->n_voxel;

    /// ------------------------------------
    /// BACKGROUND mesh
    ///-------------------------------------

    VoxelDesigner loader;
    PrimMesh bgModelXYZ;
    if ((background.isDEM == true) && (background.DEMFile != "")) {
        bgModelXYZ = createVoxelDemBackground(background, modelio->stepsize_surface);
    } else {
        bgModelXYZ = loader.createTriBackground(modelio->sceneSize_XYZ.x, modelio->sceneSize_XYZ.y, background.stepsize_surface);
    }


    bgModelXYZ.meshId = n_modelmesh;
    PrimMesh bgModel = XYZ2XZY(bgModelXYZ);
    meshio->primMeshes.emplace_back(bgModel);

    // background model link
    MeshLink bgMeshLink{};
    std::string bgThermalName = background.bgThermalName;
    bgMeshLink.thermalId =  idOrZero(meshio->thermalNames, bgThermalName);
    bgMeshLink.canopyId = 0;
    std::string bgSpectralName = background.bgSpectralName;
    bgMeshLink.spectralId = requiredId(meshio->spectralNames, bgSpectralName, "spectral");
//    std::string bgThermalName;
//    int bgThermalIndex = 0;
//    if (fileio->m_pVoxelebXml->sensorxml.isTemperature == true)
//    {
//        bgThermalName = scenexml.background.bgThermalName;
//        bgThermalIndex = meshio->thermalNames.find(bgThermalName)->second;
//    }

    int bgCanopyindex = 0;
    //  std::string bgCanopyName = scenexml.background.canopyName;
    //  bgMeshLink.canopyId = meshio->canopyNames.find(bgCanopyName)->second;
    int bgPropIndex = 0;
    std::string bgPropName = background.bgPropName;
    if (background.type == Type::WATER) {
        const auto propIt = meshio->watersetNames.find(bgPropName);
        bgPropIndex = propIt == meshio->watersetNames.end() ? 0 : propIt->second;
    } else {
        const auto propIt = meshio->soilsetNames.find(bgPropName);
        bgPropIndex = propIt == meshio->soilsetNames.end() ? 0 : propIt->second;
    }
    bgMeshLink.bioId = bgPropIndex;
    bgMeshLink.type = static_cast<int>(background.type);
    bgMeshLink.angularEffectStrength = background.angularEffectStrength;
    meshio->meshLinks.emplace_back(bgMeshLink);

    /// ------------------------------------
    /// BACKGROUND instance
    /// Voxelsize is XZY
    /// iN cg space: (voxelsize.x - voxelsize.x/2, voxelsize.y, voxelsize.z - voxelsize.z/2)
    ///-------------------------------------
    Instance bgInstance{};
    bgInstance.meshId = static_cast<uint32_t>(n_modelmesh);
    glm::mat4 bgunit = glm::mat4(1.0f);
    // here for 0-9 will become -5 - 4
    glm::vec3 bgShift = glm::vec3{-floor(modelio->voxelSize_XZY.x / 2.0+0.5), 0 - loader.minElevation,
                                  -floor(modelio->voxelSize_XZY.z / 2.0+0.5)};

//    glm::vec3 bgShift = glm::vec3{0,0,0};

    glm::vec3 bgScale = glm::vec3{1.0, 1.0, 1.0};
    glm::mat4 bgMat = glm::scale(bgunit, bgScale) * glm::translate(bgunit, bgShift);
    bgInstance.object2worldMatrix = bgMat;
    bgInstance.world2objectMatrix = glm::transpose(glm::inverse(bgMat));
    instanceio->instances.emplace_back(bgInstance);


    // background instanceLink
    InstanceLink bgInstanceLink{};
    bgInstanceLink.meshId = bgInstance.meshId;
    instanceio->instanceLinks.emplace_back(bgInstanceLink);

    //--------------------------------------------
    // background voxelLink
    //--------------------------------------------
    for (int kvoxel = 0; kvoxel < bgModel.voxelIds.size(); kvoxel++) {
        glm::ivec3 voxelId = glm::ivec3(bgModel.voxelIds[kvoxel]);// + nvmath::vec3i(bgShift);
        int test = acc.getValue(nanovdb::Coord(voxelId.x, voxelId.y, voxelId.z)); //(1,0,1)

        if (test < 0) {
            acc.setValue(nanovdb::Coord(voxelId.x, voxelId.y, voxelId.z), n_voxel);
            VoxelLink voxellink{};
            glm::ivec3 voxelId_ = glm::ivec3(voxelId.x * 1.0, voxelId.y * 1.0, voxelId.z * 1.0);
            voxellink.voxelId = voxelId_;
            voxellink.instanceId = n_instance;
            voxellink.faceId = 5;
            voxellink.isValid = 1;
            voxellink.aeroId = 0;

            voxelio->voxellinks.emplace_back(voxellink);
            n_voxel++;
        }
    }
    n_instance++;
    n_modelmesh++;


    if(background.isLad==true){
        // read lad tif
        int width = 0,height = 0, nband = 1;
        // FIXED[中] 错误处理: readImageinout1 的返回值被忽略，LAD 影像打不开时 lads 保持为空（不会走 else 分支补 0），着色器按体元索引读取空缓冲；已修复：检查返回值，读取失败或结果为空时抛出带路径的异常，避免零长度 lads 被按体元索引；影像 width×height 与背景网格尺寸的一致性校验仍为 DEFERRED。
        if (Utils::readImageinout1(background.ladfile,surfio->lads,width, height,nband) == 0 ||
            surfio->lads.empty())
            throw std::runtime_error("Cannot read LAD image: " + background.ladfile);
    }else{
        surfio->lads.emplace_back(0);
    }

    /// ------------------------------------
    ///  Divided  Background, Need to be provided
    ///-------------------------------------

    /// ------------------------------------
    ///  Divided  Background, Need to be provided
    ///-------------------------------------

    return true;
}



PrimMesh Scene::XYZ2XZY(PrimMesh model,int mark){
    PrimMesh temp;
    temp.meshId = model.meshId;
    temp.nIndices = model.nIndices;
    temp.nVertices = model.nVertices;
    temp.indices = std::move(model.indices);
    temp.vertices.reserve(model.vertices.size());
    for (const VertexAttribute& source : model.vertices)
    {
        VertexAttribute vertex{};
        vertex.color = source.color;
        // DEFERRED[中] 物理: 位置做了 y/z 交换，但法向 nrm 原样拷贝，未同步交换分量，交换后的网格中法向与面不一致（着色器若使用顶点法向计算余弦/朝向会出错）；另外 y/z 交换是镜像变换，会反转三角形绕序（正反面翻转）；建议：nrm 同样交换为 {x,z,y}，并交换每个三角形的两个索引以保持绕序（ObjMesh 版本同理）。
        vertex.nrm = source.nrm;
        vertex.texCoord = source.texCoord;
        vertex.pos = {source.pos.x, source.pos.z, source.pos.y};
        temp.vertices.emplace_back(vertex);
    }
    temp.nVertices = static_cast<uint32_t>(temp.vertices.size());
    for (int i = 0; i < model.voxelIds.size(); i++)
    {
        glm::vec3 center = {
                model.voxelIds[i].x,
                model.voxelIds[i].z,
                model.voxelIds[i].y};
        temp.voxelIds.emplace_back(center);
    }

    if (mark == 1) {
        temp.isValids.reserve(model.voxelIds.size());
        temp.faceIds.reserve(model.voxelIds.size());
        for (size_t i = 0; i < model.voxelIds.size(); ++i) {
            temp.isValids.emplace_back(i < model.isValids.size() ? model.isValids[i] : int5{});
            temp.faceIds.emplace_back(i < model.faceIds.size() ? model.faceIds[i] : 0);
        }
    }

    return temp;
}


ObjMesh Scene::XYZ2XZY(ObjMesh model){
    ObjMesh temp;
    temp.meshId = model.meshId;
    temp.nIndices = model.nIndices;
    temp.nVertices = model.nVertices;
    temp.indices = model.indices;
    // FIXED[中] 内存: 以 model.nVertices 为循环上界而非 vertices.size()，二者不同步（如加载器只填了 vector 未更新计数）时越界读；已修复：上界取 min(nVertices, vertices.size())，计数大于向量长度时不再越界（按值传参/整份复制 indices 的性能问题仍为 DEFERRED）。
    const int vertexCount = static_cast<int>(
        std::min<size_t>(model.nVertices, model.vertices.size()));
    for (int i = 0; i< vertexCount;i++)
    {
        VertexAttribute vertex = {};
        vertex.color = model.vertices[i].color;
        vertex.nrm = model.vertices[i].nrm;
        vertex.texCoord = model.vertices[i].texCoord;
        vertex.pos = {model.vertices[i].pos.x,
                      model.vertices[i].pos.z,
                      model.vertices[i].pos.y};
        temp.vertices.emplace_back(vertex);
    }


    return temp;
}

void Scene::outputObjMesh(ObjMesh model, std::string &fileName) {
    // DEFERRED[低] 错误处理: 窄字符路径打开（中文 projectDir 失败）且打开失败时静默不输出；ObjMesh 按值传入复制整个 DEM 网格；默认 6 位有效数字输出坐标，大场景（>1e5 m 量级或 UTM 坐标）精度丢失；建议：u8path 打开并报错，const 引用传参，设置 std::setprecision(9)。
    std::ofstream outfile(fileName);

    if (outfile.is_open()){
        for (auto & vertice : model.vertices){
            outfile << "v " << vertice.pos.x << " "
            << vertice.pos.y << " "
            << vertice.pos.z << " " << std::endl;
        }
        for (int k = 0; k < model.indices.size(); k = k+3){
            outfile << "f " << model.indices[k] + 1 << " "
            << model.indices[k + 1] + 1 << " "
            << model.indices[k + 2] + 1 << " " << std::endl;
        }
        outfile.close();
    }

}
