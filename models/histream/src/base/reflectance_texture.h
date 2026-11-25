// ==========================================================================
// 文件：reflectance_texture.h
// 功能：声明反射率扰动纹理（ReflectanceVarianceTexture）的数据结构、加载、三平面采样，
//       以及由偏差统计量换算纹理缩放系数的接口（实现见 reflectance_texture.cpp）。
// 主要模块：
//   1. ReflectanceVarianceTexture：零均值、[-1,1] 的线性亮度偏差图及双线性采样
//   2. loadReflectanceVarianceTexture：带缓存的加载函数
//   3. sampleReflectanceVarianceTriplanar：世界坐标三平面采样
//   4. TextureDeviationStatistics / physicalTextureScale：强度换算与因子范围限制
// 重要约定：反射率因子 = 1 + scale × 偏差；偏差无量纲；世界轴 +X 北、+Y 上、+Z 东。
// ==========================================================================

#ifndef HISTREAM_REFLECTANCE_TEXTURE_H
#define HISTREAM_REFLECTANCE_TEXTURE_H

#include <array>
#include <filesystem>
#include <memory>
#include <vector>

// Shortwave reflectance-variance template shared by FacetRT/FacetEB and
// RayTracing: linear luminance (8-bit images decoded from sRGB, alpha bands
// ignored, palettes expanded) with the image mean removed, scaled to [-1, 1].
// 数据：width×height 的行主序偏差图（第 0 行为图像顶行）；sample 做循环双线性采样。
struct ReflectanceVarianceTexture {
    int width{};
    int height{};
    std::vector<float> values;

    float sample(float u, float v) const;
};

// 加载纹理文件（按绝对路径缓存复用）；失败抛异常。
std::shared_ptr<const ReflectanceVarianceTexture> loadReflectanceVarianceTexture(
    const std::filesystem::path& path);

// World-space triplanar lookup at a triangle centroid, weighted by |normal|.
// Walls keep image row 0 at the top; the ground is north-up, east-right.
float sampleReflectanceVarianceTriplanar(const ReflectanceVarianceTexture& texture,
                                         const std::array<float, 3>& center,
                                         const std::array<float, 3>& absoluteNormal,
                                         float repeatSize);

// Deviation statistics of one material (deviations already have zero area mean).
// 字段：area 为面积和；areaWeightedSquare 为 Σ(面积×偏差²)；
// minimumDeviation/maximumDeviation 为偏差极值（用于限制因子范围）。
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
