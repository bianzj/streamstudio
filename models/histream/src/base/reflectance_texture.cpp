// ==========================================================================
// 文件：reflectance_texture.cpp
// 功能：加载“反射率扰动纹理”（ReflectanceVarianceTexture）并提供采样与强度换算，
//       供 FacetRT/FacetEB 与 RayTracing 为表面反射率叠加空间变化（因子 = 1 + scale × 偏差）。
// 主要模块：
//   1. 匿名命名空间工具：sRGB→线性、循环下标、GDAL 波段读取
//   2. ReflectanceVarianceTexture::sample：循环（平铺）双线性采样
//   3. loadReflectanceVarianceTexture：GDAL 读图、转线性亮度、去均值、归一化到 [-1,1]，并缓存
//   4. physicalTextureScale：由强度/相对标准差换算纹理缩放系数，并限制因子范围
//   5. sampleReflectanceVarianceTriplanar：按三角形质心与法向做世界坐标三平面采样
// 数据流：纹理文件 → 线性亮度 → 去均值 → values ∈ [-1,1]；调用方统计各材质的偏差统计量
//   （TextureDeviationStatistics）→ physicalTextureScale 得 scale → 采样值 × scale 得反射率扰动。
// 与其他文件的关系：接口声明在 reflectance_texture.h；依赖 GDAL 读取图像。
// 重要约定：世界轴 +X 为北、+Y 为上、+Z 为东；纹理 v=0 对应图像顶行；
//   本文件输出的偏差是相对量（无量纲），不是绝对反射率。
// ==========================================================================

#include "reflectance_texture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <gdal_priv.h>

namespace {

// 作用：sRGB 编码值（0..1）转线性亮度，输入先限幅到 [0,1]（标准 sRGB 分段公式）。
float srgbToLinear(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f
        ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

// 作用：把下标循环折回 [0,size)，支持负数；用于纹理平铺采样。
int wrapIndex(int value, int size)
{
    const int wrapped = value % size;
    return wrapped < 0 ? wrapped + size : wrapped;
}

// 一个栅格波段的像素值（double）及其 nodata 标记，供 readBand 返回。
struct BandData {
    std::vector<double> values;
    bool hasNoData{false};
    double noData{0.0};
};

// 作用：把 GDAL 的某一波段整幅读成 double；失败时关闭数据集并抛异常。
// 说明：key 仅用于错误信息。
BandData readBand(GDALDataset* dataset, int bandIndex, int width, int height,
                  const std::string& key)
{
    GDALRasterBand* band = dataset->GetRasterBand(bandIndex);
    BandData data;
    data.values.resize(static_cast<size_t>(width) * height);
    int hasNoData = 0;
    data.noData = band->GetNoDataValue(&hasNoData);
    data.hasNoData = hasNoData != 0;
    const CPLErr result = band->RasterIO(
        GF_Read, 0, 0, width, height, data.values.data(),
        width, height, GDT_Float64, 0, 0);
    if (result != CE_None) {
        GDALClose(dataset);
        throw std::runtime_error("Cannot read reflectance variance texture: " + key);
    }
    return data;
}

}  // namespace

// 作用：在 (u,v) 处做双线性插值，u、v 取小数部分即周期平铺；空纹理返回 0。
// 说明：像素中心位于 (i+0.5)/size，所以先减 0.5 再取整；四邻域下标循环折回，
//       使平铺边界处无接缝。
float ReflectanceVarianceTexture::sample(float u, float v) const
{
    if (width <= 0 || height <= 0 || values.empty()) return 0.0f;
    // Texel centres sit at (i + 0.5) / size; u runs along columns and v down
    // the rows (v = 0 is the top image row).
    const float x = (u - std::floor(u)) * static_cast<float>(width) - 0.5f;
    const float y = (v - std::floor(v)) * static_cast<float>(height) - 0.5f;
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int x0 = wrapIndex(static_cast<int>(fx), width);
    const int y0 = wrapIndex(static_cast<int>(fy), height);
    const int x1 = (x0 + 1) % width;
    const int y1 = (y0 + 1) % height;
    const float tx = x - fx;
    const float ty = y - fy;
    const auto at = [this](int column, int row) {
        return values[static_cast<size_t>(row) * width + column];
    };
    const float top = at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx;
    const float bottom = at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx;
    return top * (1.0f - ty) + bottom * ty;
}

// 作用：读取纹理文件，转成零均值、最大绝对值为 1 的线性亮度偏差图。
// 输入：图像路径（GDAL 支持的格式）；输出：共享只读纹理，同一绝对路径复用缓存
//       （弱引用，无人使用时会被释放；缓存未加锁，假定在单线程中加载，待核对）。
// 失败（无法打开、小于 2x2、无色彩波段、无有效像素、近似常数）一律抛异常。
// 步骤：1 缓存查找与打开 → 2 读色彩通道转线性（调色板/多波段/灰度）→
//       3 按 Rec.709 权重算亮度并求均值 → 4 去均值并按最大绝对值归一化。
std::shared_ptr<const ReflectanceVarianceTexture> loadReflectanceVarianceTexture(
    const std::filesystem::path& path)
{
    const std::string key = std::filesystem::absolute(path).lexically_normal().string();
    static std::unordered_map<
        std::string, std::weak_ptr<const ReflectanceVarianceTexture>> cache;
    const auto cached = cache.find(key);
    if (cached != cache.end()) {
        if (const auto texture = cached->second.lock()) return texture;
    }

    // 步骤 1：缓存未命中，用 GDAL 打开文件并检查尺寸。
    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(
        GDALOpen(key.c_str(), GA_ReadOnly));
    if (!dataset || dataset->GetRasterCount() < 1) {
        if (dataset) GDALClose(dataset);
        throw std::runtime_error("Cannot open reflectance variance texture: " + key);
    }
    const int width = dataset->GetRasterXSize();
    const int height = dataset->GetRasterYSize();
    if (width < 2 || height < 2) {
        GDALClose(dataset);
        throw std::runtime_error(
            "Reflectance variance texture must contain at least 2 x 2 pixels: " + key);
    }
    const size_t pixelCount = static_cast<size_t>(width) * height;

    // Colour channels in linear units; NaN marks nodata. Byte data (and
    // 16-bit PNG) is sRGB-encoded; other types are taken as linear. Linear
    // scale and offset cancel in the normalisation below.
    // 步骤 2：把图像读成 rgb 三通道线性值（灰度图只填第 0 通道）。
    std::array<std::vector<float>, 3> rgb;
    for (auto& channel : rgb) channel.assign(pixelCount, 0.0f);
    bool gray = false;

    GDALRasterBand* firstBand = dataset->GetRasterBand(1);
    const GDALColorTable* palette = firstBand->GetColorTable();
    const GDALDriver* driver = dataset->GetDriver();
    const bool pngDriver = driver && std::string(driver->GetDescription()) == "PNG";
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // 分支 A：调色板图像——按索引展开为 RGB，无对应调色板项或等于 nodata 记为 NaN；
    // 灰度调色板只用第一分量。
    if (palette && firstBand->GetColorInterpretation() == GCI_PaletteIndex) {
        const BandData indices = readBand(dataset, 1, width, height, key);
        const bool grayPalette = palette->GetPaletteInterpretation() == GPI_Gray;
        const int entryCount = palette->GetColorEntryCount();
        for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
            const double raw = indices.values[pixel];
            const long index = std::isfinite(raw) ? std::lround(raw) : -1;
            const GDALColorEntry* entry = index >= 0 && index < entryCount
                ? palette->GetColorEntry(static_cast<int>(index)) : nullptr;
            if (!entry || (indices.hasNoData && raw == indices.noData)) {
                rgb[0][pixel] = rgb[1][pixel] = rgb[2][pixel] = nan;
                continue;
            }
            rgb[0][pixel] = srgbToLinear(entry->c1 / 255.0f);
            rgb[1][pixel] = grayPalette ? rgb[0][pixel] : srgbToLinear(entry->c2 / 255.0f);
            rgb[2][pixel] = grayPalette ? rgb[0][pixel] : srgbToLinear(entry->c3 / 255.0f);
        }
    // 分支 B：普通波段图像——忽略 Alpha 波段；有显式 R/G/B 解释就按其取，
    // 否则取前三个波段，只有一个波段时按灰度处理。
    } else {
        int redBand = 0, greenBand = 0, blueBand = 0;
        std::vector<int> colourBands;
        for (int index = 1; index <= dataset->GetRasterCount(); ++index) {
            const GDALColorInterp interpretation =
                dataset->GetRasterBand(index)->GetColorInterpretation();
            if (interpretation == GCI_AlphaBand) continue;
            if (interpretation == GCI_RedBand) redBand = index;
            else if (interpretation == GCI_GreenBand) greenBand = index;
            else if (interpretation == GCI_BlueBand) blueBand = index;
            colourBands.push_back(index);
        }
        if (colourBands.empty()) {
            GDALClose(dataset);
            throw std::runtime_error("Reflectance variance texture has no colour band: " + key);
        }
        std::array<int, 3> bands{};
        if (redBand && greenBand && blueBand) {
            bands = {redBand, greenBand, blueBand};
        } else if (colourBands.size() >= 3) {
            bands = {colourBands[0], colourBands[1], colourBands[2]};
        } else {
            bands = {colourBands[0], colourBands[0], colourBands[0]};
            gray = true;
        }

        const GDALDataType type = dataset->GetRasterBand(bands[0])->GetRasterDataType();
        // 8 位与 PNG 的 16 位视为 sRGB 编码，其他类型按线性值使用（线性缩放不影响后续归一化）。
        const bool srgb = type == GDT_Byte || (type == GDT_UInt16 && pngDriver);
        const float scale = type == GDT_Byte ? 1.0f / 255.0f
            : type == GDT_UInt16 ? 1.0f / 65535.0f : 1.0f;
        for (int channel = 0; channel < (gray ? 1 : 3); ++channel) {
            const BandData data = readBand(dataset, bands[channel], width, height, key);
            for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
                const double raw = data.values[pixel];
                if (!std::isfinite(raw) || (data.hasNoData && raw == data.noData)) {
                    rgb[channel][pixel] = nan;
                    continue;
                }
                const float value = static_cast<float>(raw) * scale;
                rgb[channel][pixel] = srgb ? srgbToLinear(value) : value;
            }
        }
    }
    GDALClose(dataset);

    // 步骤 3：按 Rec.709 系数（0.2126/0.7152/0.0722）求线性亮度，并累计有效像素均值。
    auto texture = std::make_shared<ReflectanceVarianceTexture>();
    texture->width = width;
    texture->height = height;
    texture->values.resize(pixelCount);
    double mean = 0.0;
    size_t validCount = 0;
    for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const float red = rgb[0][pixel];
        const float green = gray ? red : rgb[1][pixel];
        const float blue = gray ? red : rgb[2][pixel];
        const float luminance =
            0.2126f * red + 0.7152f * green + 0.0722f * blue;
        texture->values[pixel] = luminance;
        if (std::isfinite(luminance)) {
            mean += luminance;
            ++validCount;
        }
    }
    if (validCount == 0) {
        throw std::runtime_error(
            "Reflectance variance texture contains no valid pixels: " + key);
    }
    // 步骤 4：去均值（nodata 像素偏差置 0），再除以最大绝对偏差，使 values ∈ [-1,1]；
    // 全常数图像无法归一化，直接报错。
    mean /= static_cast<double>(validCount);
    float maximumAbsolute = 0.0f;
    for (float& value : texture->values) {
        // Nodata pixels carry no deviation.
        value = std::isfinite(value) ? value - static_cast<float>(mean) : 0.0f;
        maximumAbsolute = std::max(maximumAbsolute, std::abs(value));
    }
    if (!(maximumAbsolute > 1.0e-6f)) {
        throw std::runtime_error(
            "Reflectance variance texture is effectively constant: " + key);
    }
    for (float& value : texture->values) value /= maximumAbsolute;
    // 写入缓存（弱引用）。
    cache[key] = texture;
    return texture;
}

// 作用：求纹理因子 1 + scale·deviation 中的 scale。
// 输入：strength（0..1，限幅）；relativeStd>0 时改为按面积加权变异系数目标
//       scale = relativeStd / sqrt(面积加权偏差方差)；statistics 为该材质偏差统计；
//       maximumFactor 为反射率×因子的上限（有限时生效）。
// 限制：min 偏差为负时 scale ≤ 0.999/|min|（保证因子 > 0）；
//       max 偏差为正时 scale ≤ (maximumFactor-1)/max；结果不为负。
// 方差≈0（<1e-12）时 scale 取 0，表示该材质无纹理效果。
float physicalTextureScale(float strength, float relativeStd,
                           const TextureDeviationStatistics& statistics,
                           float maximumFactor)
{
    float scale = std::clamp(strength, 0.0f, 1.0f);
    if (relativeStd > 0.0f) {
        const double variance = statistics.area > 0.0
            ? statistics.areaWeightedSquare / statistics.area : 0.0;
        scale = variance > 1.0e-12
            ? static_cast<float>(relativeStd / std::sqrt(variance)) : 0.0f;
    }
    if (statistics.minimumDeviation < 0.0f)
        scale = std::min(scale, 0.999f / -statistics.minimumDeviation);
    if (statistics.maximumDeviation > 0.0f && std::isfinite(maximumFactor))
        scale = std::min(scale, std::max(0.0f, maximumFactor - 1.0f) / statistics.maximumDeviation);
    return std::max(0.0f, scale);
}

// 作用：三平面采样。三张投影分别对应三个世界轴向，按法向分量绝对值加权平均。
// 输入：center 为三角形质心（世界坐标，m）；normal 为法向各分量绝对值；
//       repeatSize 为纹理重复一次对应的世界长度（m，下限 0.01）。
// 输出：无量纲偏差（约 [-1,1]），调用方再乘 physicalTextureScale 的 scale。
float sampleReflectanceVarianceTriplanar(const ReflectanceVarianceTexture& texture,
                                         const std::array<float, 3>& center,
                                         const std::array<float, 3>& normal,
                                         float repeatSize)
{
    // World axes: +X north, +Y up, +Z east. Image columns run along the
    // horizontal axis and rows downward, so walls (X- or Z-facing) take v = -y
    // and stay upright; the ground is north-up/east-right like the DEM
    // (u = east, v = south = -x).
    const float weightSum = std::max(1.0e-6f, normal[0] + normal[1] + normal[2]);
    const float inverseRepeat = 1.0f / std::max(0.01f, repeatSize);
    const float north = center[0] * inverseRepeat;
    const float up = center[1] * inverseRepeat;
    const float east = center[2] * inverseRepeat;
    return (
        normal[0] * texture.sample(east, -up) +
        normal[1] * texture.sample(east, -north) +
        normal[2] * texture.sample(north, -up)) / weightSum;
}
