#ifndef HISTREAM_REFLECTANCE_TEXTURE_H
#define HISTREAM_REFLECTANCE_TEXTURE_H

#include <array>
#include <filesystem>
#include <memory>
#include <vector>

// Shortwave reflectance-variance template shared by FacetRT/FacetEB and
// RayTracing: linear luminance (8-bit images decoded from sRGB, alpha bands
// ignored, palettes expanded) with the image mean removed, scaled to [-1, 1].
struct ReflectanceVarianceTexture {
    int width{};
    int height{};
    std::vector<float> values;

    float sample(float u, float v) const;
};

std::shared_ptr<const ReflectanceVarianceTexture> loadReflectanceVarianceTexture(
    const std::filesystem::path& path);

// World-space triplanar lookup at a triangle centroid, weighted by |normal|.
// Walls keep image row 0 at the top; the ground is north-up, east-right.
float sampleReflectanceVarianceTriplanar(const ReflectanceVarianceTexture& texture,
                                         const std::array<float, 3>& center,
                                         const std::array<float, 3>& absoluteNormal,
                                         float repeatSize);

// Deviation statistics of one material (deviations already have zero area mean).
struct TextureDeviationStatistics {
    double area{};
    double areaWeightedSquare{};
    float minimumDeviation{};
    float maximumDeviation{};
};

// Factor = 1 + scale * deviation. relativeStd > 0 targets that area-weighted
// coefficient of variation; otherwise strength is the scale. Both are capped
// so factor > 0 and reflectance * factor <= maximumFactor.
float physicalTextureScale(float strength, float relativeStd,
                           const TextureDeviationStatistics& statistics,
                           float maximumFactor);

#endif
