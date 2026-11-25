// ==========================================================================
// 文件：raster.cpp
// 功能：STREAM 栅格输入输出：用 GDAL 读取与地表类型网格对齐的 GeoTIFF 波段/常数，计算像元经纬度，
//       写出多波段 float32 GeoTIFF，并给出波段统计。
// 主要模块：
//   1. requireAlignedGrid：检查输入栅格与参考网格（地表类型栅格）的坐标系与地理变换一致
//   2. UTF-8 路径转换 pathFromUtf8 / utf8FromPath
//   3. readField / openRaster / readBand / fieldRaster：配置字段 {file, band} 或 {value} 的读取
//   4. gridIsGeographic / geographicFromGrid：判断地理坐标系；求每个像元中心的 WGS84 纬度/经度
//   5. writeTiff：写结果 GeoTIFF（DEFLATE 压缩，nodata = NaN，波段名写入描述）
//   6. bandStatistics：有效像元数、最小、均值、最大（写入 scene_summary.json）
// 数据流：run-config 中的栅格字段 → fieldRaster → std::vector<float>（行优先，nodata → NaN）；
//       结果波段 → writeTiff。
// 关系：声明在 raster.h；被 classes.cpp、app/image.cpp、forcing/* 使用。
// 约定：所有输入必须与地表类型栅格同网格（同尺寸、同坐标系、同地理变换），不做重投影或重采样；
//       数据按行优先存放，nodata 与 scale/offset 在读入时处理；经度/纬度为 WGS84，度。
// ==========================================================================

#include "raster.h"

#include <ogr_spatialref.h>

#include <memory>
#include <stdexcept>

namespace stream {

namespace {

// 作用：检查数据集与参考网格 grid 对齐，不一致则抛带 label 的中文错误（要求先对齐到同一网格）。
// 步骤 1：两者要么都有坐标系、要么都没有；有则用 GDAL 的 IsSame 比较（传统 GIS 轴序，避免经纬度顺序影响）；
// 步骤 2：地理变换有无须一致；都没有时直接通过（允许显式给纬度/经度的无地理参考网格）；
// 步骤 3：参考变换须可逆；
// 步骤 4：比较四个角点的位移，换算成参考像元数，容差 1e-6 像元：可容忍舍入误差，
//         但拒绝平移、旋转或缩放过的网格。
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

// UTF-8 字符串与 std::filesystem::path 互转（Windows 下路径含中文时保证正确）。
std::filesystem::path pathFromUtf8(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }

std::string utf8FromPath(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// 作用：读取形如 {"file":"…","band":n} 或 {"value":x} 的栅格字段；键不存在或不是对象时返回空 Field。
// 说明：value 必须是 JSON 数字（null 视为未给），字符串/布尔等明确报错，不再静默当作缺失（2026-11-24 C3）。
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
    else if (value.IsValid() && value.GetType() != CPLJSONObject::Type::Null)  // FIX(2026-11-24 C3): no silent "absent"
        throw std::runtime_error(key + ".value 须为数值（JSON 数字），当前为 " + value.ToString());
    field.band = object.GetInteger("band", 0);
    return field;
}

// 作用：以只读方式打开 GDAL 栅格；文件不存在或 GDAL 无法识别时抛错。
GDALDatasetUniquePtr openRaster(const std::string& file, const std::string& label)
{
    if (!std::filesystem::is_regular_file(pathFromUtf8(file))) throw std::runtime_error(label + "文件不存在：" + file);
    GDALDatasetUniquePtr dataset(GDALDataset::Open(file.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    if (!dataset) throw std::runtime_error(label + "无法读取（需要 GDAL 可识别的栅格，如 GeoTIFF）：" + file);
    return dataset;
}

// 作用：读取第 band 波段（1 起）为 float 数组（行优先）。
// 步骤 1：尺寸与对齐检查；2：波段号检查；3：读取；4：nodata → NaN；
// 步骤 5：若声明了 scale/offset（如 MODIS LAI 以 ×10 的整型存储），在 nodata 判定之后再应用 value*scale+offset。
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

// 作用：把 Field 展开成整幅栅格：来自文件则读取（波段缺省为 1）；给常数则整幅填该值；
//       都没有时，required 为真则报错，否则整幅填 fallback。
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

// 作用：判断网格坐标系是否为地理坐标（像元大小单位为度而不是米）；无坐标系或无法解析时返回 false。
bool gridIsGeographic(const Grid& grid)
{
    if (grid.projection.empty()) return false;
    OGRSpatialReference source;
    if (source.importFromWkt(grid.projection.c_str()) != OGRERR_NONE) return false;
    return source.IsGeographic() != 0;
}

// 作用：由网格地理变换与坐标系计算每个像元中心的 WGS84 纬度/经度（度），失败像元为 NaN。
// 步骤 1：检查有地理参考；2：建立到 WGS84 的坐标变换（传统 GIS 轴序，x = 经度、y = 纬度）；
// 步骤 3：按行批量变换像元中心（列、行各加 0.5）。
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

// 作用：把若干波段写成 float32 GeoTIFF（DEFLATE 压缩）；带地理变换/坐标系；nodata 设为 NaN。
// 说明：波段名同时写入每个波段的描述与 TIFFTAG_IMAGEDESCRIPTION（格式 band names = {a, b, …}）。
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
    // FIX(2026-11-24 R5): flushing and closing can fail (disk full, truncated file) after every RasterIO succeeded.
    // Only the return code counts: unrelated CPL errors (e.g. PROJ missing proj.db while writing the CRS) must not fail the run.
    if (dataset->Close() != CE_None)
        throw std::runtime_error("写入结果失败（刷盘或关闭文件出错，磁盘空间不足？）：" + utf8FromPath(path));
}

// 作用：统计有限值的个数、最小、均值、最大，返回 JSON 条目；没有有限值时只含 name 与 validPixels。
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
