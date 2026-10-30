#include "raster.h"

#include <ogr_spatialref.h>

#include <memory>
#include <stdexcept>

namespace stream {

namespace {

void requireAlignedGrid(GDALDataset& dataset, const Grid& grid, const std::string& label)
{
    const auto mismatch = [&](const std::string& reason) {
        throw std::runtime_error(label + "与地表类型栅格的" + reason + "不一致，请先对齐到同一网格");
    };
    const char* projection = dataset.GetProjectionRef();
    const bool hasProjection = projection && *projection;
    if (hasProjection != !grid.projection.empty()) mismatch("坐标系信息");
    if (hasProjection) {
        OGRSpatialReference reference, input;
        if (reference.SetFromUserInput(grid.projection.c_str()) != OGRERR_NONE ||
            input.SetFromUserInput(projection) != OGRERR_NONE)
            throw std::runtime_error(label + "或地表类型栅格的坐标系无法解析");
        reference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        input.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        if (!reference.IsSame(&input)) mismatch("坐标系");
    }

    double transform[6];
    const bool hasTransform = dataset.GetGeoTransform(transform) == CE_None;
    if (hasTransform != grid.hasTransform) mismatch("地理变换信息");
    // Explicit latitude/longitude inputs may use grids with no georeferencing.
    if (!hasTransform) return;
    for (int i = 0; i < 6; ++i)
        if (!std::isfinite(transform[i]) || !std::isfinite(grid.transform[i])) mismatch("地理变换");
    const auto* ref = grid.transform;
    const double determinant = ref[1] * ref[5] - ref[2] * ref[4];
    if (!std::isfinite(determinant) || determinant == 0)
        throw std::runtime_error("地表类型栅格的地理变换不可逆");
    // Measure corner displacement in reference pixels, not absolute map units:
    // tolerates roundoff without accepting shifted, rotated or rescaled grids.
    constexpr double tolerance = 1e-6;
    for (int column : {0, grid.width}) for (int row : {0, grid.height}) {
        const double dx = (transform[0] - ref[0]) + column * (transform[1] - ref[1]) + row * (transform[2] - ref[2]);
        const double dy = (transform[3] - ref[3]) + column * (transform[4] - ref[4]) + row * (transform[5] - ref[5]);
        const double pixelX = (ref[5] * dx - ref[2] * dy) / determinant;
        const double pixelY = (ref[1] * dy - ref[4] * dx) / determinant;
        if (!std::isfinite(pixelX) || !std::isfinite(pixelY) || std::abs(pixelX) > tolerance || std::abs(pixelY) > tolerance)
            mismatch("像元位置、分辨率或旋转");
    }
}

}  // namespace

std::filesystem::path pathFromUtf8(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }

std::string utf8FromPath(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

Field readField(const CPLJSONObject& parent, const std::string& key)
{
    Field field;
    const auto object = parent.GetObj(key);
    if (!object.IsValid() || object.GetType() != CPLJSONObject::Type::Object) return field;
    field.file = object.GetString("file", "");
    const auto value = object.GetObj("value");
    if (value.IsValid() && (value.GetType() == CPLJSONObject::Type::Integer || value.GetType() == CPLJSONObject::Type::Long ||
                            value.GetType() == CPLJSONObject::Type::Double))
        field.value = value.ToDouble();
    field.band = object.GetInteger("band", 0);
    return field;
}

GDALDatasetUniquePtr openRaster(const std::string& file, const std::string& label)
{
    if (!std::filesystem::is_regular_file(pathFromUtf8(file))) throw std::runtime_error(label + "文件不存在：" + file);
    GDALDatasetUniquePtr dataset(GDALDataset::Open(file.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    if (!dataset) throw std::runtime_error(label + "无法读取（需要 GDAL 可识别的栅格，如 GeoTIFF）：" + file);
    return dataset;
}

std::vector<float> readBand(GDALDataset& dataset, int band, const Grid& grid, const std::string& label)
{
    if (dataset.GetRasterXSize() != grid.width || dataset.GetRasterYSize() != grid.height)
        throw std::runtime_error(label + "网格为 " + std::to_string(dataset.GetRasterXSize()) + " × " + std::to_string(dataset.GetRasterYSize()) +
                                 "，与地表类型网格 " + std::to_string(grid.width) + " × " + std::to_string(grid.height) + " 不一致");
    requireAlignedGrid(dataset, grid, label);
    if (band < 1 || band > dataset.GetRasterCount())
        throw std::runtime_error(label + "没有第 " + std::to_string(band) + " 波段（共 " + std::to_string(dataset.GetRasterCount()) + " 个）");
    auto* raster = dataset.GetRasterBand(band);
    std::vector<float> values(grid.size());
    if (raster->RasterIO(GF_Read, 0, 0, grid.width, grid.height, values.data(), grid.width, grid.height, GDT_Float32, 0, 0) != CE_None)
        throw std::runtime_error(label + "读取失败");
    int hasNoData = 0;
    const double noData = raster->GetNoDataValue(&hasNoData);
    if (hasNoData) for (auto& value : values) if (value == static_cast<float>(noData)) value = kNaN;
    // Packed integer products (e.g. MODIS LAI stored ×10) declare scale/offset; apply them after the nodata test.
    int hasScale = 0, hasOffset = 0;
    const double scale = raster->GetScale(&hasScale);
    const double offset = raster->GetOffset(&hasOffset);
    if ((hasScale && scale != 1.0) || (hasOffset && offset != 0.0)) {
        const double s = hasScale ? scale : 1.0, o = hasOffset ? offset : 0.0;
        for (auto& value : values) if (std::isfinite(value)) value = static_cast<float>(value * s + o);
    }
    return values;
}

std::vector<float> fieldRaster(const Field& field, const Grid& grid, const std::string& label, double fallback, bool required)
{
    if (field.fromFile()) {
        auto dataset = openRaster(field.file, label);
        return readBand(*dataset, field.band > 0 ? field.band : 1, grid, label);
    }
    if (std::isfinite(field.value)) return std::vector<float>(grid.size(), static_cast<float>(field.value));
    if (required) throw std::runtime_error("缺少输入：" + label + "（需要栅格文件或常数）");
    return std::vector<float>(grid.size(), static_cast<float>(fallback));
}

void geographicFromGrid(const Grid& grid, std::vector<float>& latitude, std::vector<float>& longitude)
{
    if (!grid.hasTransform || grid.projection.empty()) throw std::runtime_error("地表类型文件没有地理参考，请提供纬度/经度栅格或常数");
    OGRSpatialReference source;
    if (source.importFromWkt(grid.projection.c_str()) != OGRERR_NONE) throw std::runtime_error("无法解析地表类型文件的坐标系");
    source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReference target;
    target.SetWellKnownGeogCS("WGS84");
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    std::unique_ptr<OGRCoordinateTransformation, decltype(&OGRCoordinateTransformation::DestroyCT)>
        transform(OGRCreateCoordinateTransformation(&source, &target), &OGRCoordinateTransformation::DestroyCT);
    if (!transform) throw std::runtime_error("无法把地表类型文件坐标转换为 WGS84 经纬度");
    latitude.assign(grid.size(), kNaN);
    longitude.assign(grid.size(), kNaN);
    std::vector<double> x(grid.width), y(grid.width);
    std::vector<int> success(grid.width);
    const double* t = grid.transform;
    for (int row = 0; row < grid.height; ++row) {
        for (int column = 0; column < grid.width; ++column) {
            x[column] = t[0] + (column + 0.5) * t[1] + (row + 0.5) * t[2];
            y[column] = t[3] + (column + 0.5) * t[4] + (row + 0.5) * t[5];
        }
        transform->Transform(grid.width, x.data(), y.data(), nullptr, success.data());
        for (int column = 0; column < grid.width; ++column)
            if (success[column]) {
                longitude[static_cast<size_t>(row) * grid.width + column] = static_cast<float>(x[column]);
                latitude[static_cast<size_t>(row) * grid.width + column] = static_cast<float>(y[column]);
            }
    }
}

void writeTiff(const std::filesystem::path& path, const Grid& grid, const std::vector<std::vector<float>>& bands,
               const std::vector<std::string>& names)
{
    auto* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (!driver) throw std::runtime_error("GDAL 缺少 GTiff 驱动");
    char** options = CSLSetNameValue(nullptr, "COMPRESS", "DEFLATE");
    GDALDatasetUniquePtr dataset(driver->Create(utf8FromPath(path).c_str(), grid.width, grid.height, static_cast<int>(bands.size()), GDT_Float32, options));
    CSLDestroy(options);
    if (!dataset) throw std::runtime_error("无法创建结果文件：" + utf8FromPath(path));
    if (grid.hasTransform) dataset->SetGeoTransform(const_cast<double*>(grid.transform));
    if (!grid.projection.empty()) dataset->SetProjection(grid.projection.c_str());
    std::string description = "band names = {";
    for (size_t index = 0; index < names.size(); ++index) description += (index ? ", " : "") + names[index];
    description += "}";
    dataset->SetMetadataItem("TIFFTAG_IMAGEDESCRIPTION", description.c_str());
    for (size_t index = 0; index < bands.size(); ++index) {
        auto* band = dataset->GetRasterBand(static_cast<int>(index) + 1);
        band->SetDescription(names[index].c_str());
        band->SetNoDataValue(std::numeric_limits<double>::quiet_NaN());
        if (band->RasterIO(GF_Write, 0, 0, grid.width, grid.height, const_cast<float*>(bands[index].data()), grid.width, grid.height, GDT_Float32, 0, 0) != CE_None)
            throw std::runtime_error("写入结果失败：" + utf8FromPath(path));
    }
}

CPLJSONObject bandStatistics(const std::string& name, const std::vector<float>& values)
{
    double sum = 0, minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
    long long count = 0;
    for (const float value : values)
        if (std::isfinite(value)) {
            sum += value;
            minimum = std::min<double>(minimum, value);
            maximum = std::max<double>(maximum, value);
            ++count;
        }
    CPLJSONObject item;
    item.Add("name", name);
    item.Add("validPixels", static_cast<GInt64>(count));
    if (count) {
        item.Add("minimum", minimum);
        item.Add("mean", sum / count);
        item.Add("maximum", maximum);
    }
    return item;
}

}  // namespace stream
