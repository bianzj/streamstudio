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

float srgbToLinear(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f
        ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

int wrapIndex(int value, int size)
{
    const int wrapped = value % size;
    return wrapped < 0 ? wrapped + size : wrapped;
}

struct BandData {
    std::vector<double> values;
    bool hasNoData{false};
    double noData{0.0};
};

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
    std::array<std::vector<float>, 3> rgb;
    for (auto& channel : rgb) channel.assign(pixelCount, 0.0f);
    bool gray = false;

    GDALRasterBand* firstBand = dataset->GetRasterBand(1);
    const GDALColorTable* palette = firstBand->GetColorTable();
    const GDALDriver* driver = dataset->GetDriver();
    const bool pngDriver = driver && std::string(driver->GetDescription()) == "PNG";
    const float nan = std::numeric_limits<float>::quiet_NaN();

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
    cache[key] = texture;
    return texture;
}

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
