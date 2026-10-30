#pragma once
#include <cpl_json.h>
#include <gdal_priv.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace stream {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

std::filesystem::path pathFromUtf8(const std::string& text);
std::string utf8FromPath(const std::filesystem::path& path);

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

// WGS84 latitude/longitude of every pixel centre from the grid's geotransform and CRS.
void geographicFromGrid(const Grid& grid, std::vector<float>& latitude, std::vector<float>& longitude);

void writeTiff(const std::filesystem::path& path, const Grid& grid, const std::vector<std::vector<float>>& bands,
               const std::vector<std::string>& names);
CPLJSONObject bandStatistics(const std::string& name, const std::vector<float>& values);

}  // namespace stream
