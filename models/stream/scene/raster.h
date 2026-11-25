// ==========================================================================
// 文件：raster.h
// 功能：栅格读写接口声明：网格描述 Grid、栅格输入字段 Field、GDAL 读取/写出与经纬度计算（实现见 raster.cpp）。
// 主要模块：
//   1. 常量 kNaN 与 UTF-8 路径转换
//   2. Grid：宽高、仿射地理变换 transform[6]（GDAL 约定）、坐标系 WKT
//   3. Field：配置里的栅格输入（同网格文件的某波段，或常数）
//   4. 读取：readField / openRaster / readBand / fieldRaster
//   5. 网格几何：gridIsGeographic / geographicFromGrid
//   6. 输出：writeTiff、bandStatistics
// 约定：栅格数据按行优先存放在 std::vector<float>，无效值为 NaN；transform 为 GDAL 六参数
//       [x0, dx, rot, y0, rot, dy]；所有输入须与地表类型栅格同网格。
// ==========================================================================

#pragma once
#include <cpl_json.h>
#include <gdal_priv.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace stream {

// 无效/缺测像元的标记值（nodata 读入时统一转成 NaN）。
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

std::filesystem::path pathFromUtf8(const std::string& text);
std::string utf8FromPath(const std::filesystem::path& path);

// 结构体 Grid：参考网格（来自地表类型栅格）。transform 缺省为单位变换；hasTransform 标记文件是否真有地理变换；
// projection 为坐标系 WKT（可为空）。size() 为像元总数。
struct Grid {
    int width = 0, height = 0;
    double transform[6]{0, 1, 0, 0, 0, 1};
    bool hasTransform = false;
    std::string projection;
    size_t size() const { return static_cast<size_t>(width) * height; }
};

// A raster input: a same-grid GeoTIFF band or a constant.
struct Field {
    std::string file;
    double value = std::numeric_limits<double>::quiet_NaN();
    int band = 0;
    bool fromFile() const { return !file.empty(); }
    bool present() const { return fromFile() || std::isfinite(value); }
};

Field readField(const CPLJSONObject& parent, const std::string& key);
GDALDatasetUniquePtr openRaster(const std::string& file, const std::string& label);
std::vector<float> readBand(GDALDataset& dataset, int band, const Grid& grid, const std::string& label);
std::vector<float> fieldRaster(const Field& field, const Grid& grid, const std::string& label, double fallback, bool required);

// True when the grid CRS is geographic (pixel sizes are in degrees, not metres).
bool gridIsGeographic(const Grid& grid);

// WGS84 latitude/longitude of every pixel centre from the grid's geotransform and CRS.
void geographicFromGrid(const Grid& grid, std::vector<float>& latitude, std::vector<float>& longitude);

void writeTiff(const std::filesystem::path& path, const Grid& grid, const std::vector<std::vector<float>>& bands,
               const std::vector<std::string>& names);
CPLJSONObject bandStatistics(const std::string& name, const std::vector<float>& values);

}  // namespace stream
