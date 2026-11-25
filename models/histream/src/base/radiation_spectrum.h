// ==========================================================================
// 文件：radiation_spectrum.h
// 功能：入射光谱（太阳直射 Esun、天空散射 Esky）的读取与短波系数归一化，header-only。
// 主要模块：
//   1. normalizedShortwaveCoefficient：把单个光谱样本换算为 GPU 使用的归一化系数
//   2. readIncidentSpectrum：从文本文件读取每行一个样本的入射光谱
// 数据流：fileio.cpp 读入 rinfile/rlifile → readIncidentSpectrum → 按 VNIR 积分与直射比例
//   调用 normalizedShortwaveCoefficient → 得到 fesun_/fesky_ 及单位光谱形状。
// 重要约定：输入非法（非有限、为负、比例超出 [0,1]、文件缺失或样本不足）一律抛
//   std::runtime_error，不做静默修正；GPU 系数的比例因子为 1000。
// ==========================================================================

#pragma once
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// GPU coefficients use a scale of 1000; the fixed 1 nm VNIR integral and
// configured energy fraction determine their shape and absolute weight.
// 作用：coefficient = sample / integratedShape * fraction * 1000，即该波长样本在
//       积分归一化后的形状乘以能量占比（直射或散射）。
// 输入：sample 为该波长的光谱样本；integratedShape 为短波光谱积分；
//       fraction 为 [0,1] 的能量占比。fraction==0 直接返回 0（此时允许积分为 0）。
// 说明：内部用 double 计算，结果超出 float 范围则抛异常。
inline float normalizedShortwaveCoefficient(float sample, float integratedShape, float fraction)
{
    if (!std::isfinite(sample) || sample < 0.0f ||
        !std::isfinite(integratedShape) || integratedShape < 0.0f ||
        !std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0f)
        throw std::runtime_error("Shortwave spectrum requires finite nonnegative samples and integral, and a fraction in [0,1]");
    if (fraction == 0.0f) return 0.0f;
    if (integratedShape == 0.0f)
        throw std::runtime_error("A nonzero shortwave fraction requires a positive spectral integral");
    const double coefficient = static_cast<double>(sample) / integratedShape * fraction * 1000.0;
    if (!std::isfinite(coefficient) || coefficient > std::numeric_limits<float>::max())
        throw std::runtime_error("Normalized shortwave coefficient exceeds the float32 range");
    return static_cast<float>(coefficient);
}

// Incident spectrum files contain one sample per nonempty line. Extra columns
// retain their legacy allowance; the first token must be an entire number.
// 作用：读取入射光谱文件，返回逐行样本；至少需要 requiredSamples 个，多出的保留。
// 说明：路径按 UTF-8 打开；允许首行带 UTF-8 BOM；空行跳过；
//       每行只取第一列，必须整段是有限非负数，其余列忽略（兼容旧文件）。
inline std::vector<float> readIncidentSpectrum(const std::string& path, size_t requiredSamples)
{
    std::ifstream input(std::filesystem::u8path(path));
    if (!input.is_open())
        throw std::runtime_error("Cannot open incident spectrum: " + path);
    std::vector<float> samples;
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line))
    {
        ++lineNumber;
        if (lineNumber == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
            line.erase(0, 3);
        std::istringstream fields(line);
        std::string token;
        if (!(fields >> token)) continue;
        std::istringstream numeric(token);
        float sample;
        if (!(numeric >> sample) || numeric.peek() != std::char_traits<char>::eof() ||
            !std::isfinite(sample) || sample < 0.0f)
            throw std::runtime_error("Invalid incident spectrum sample at line " +
                std::to_string(lineNumber) + ": " + path);
        samples.push_back(sample);
    }
    if (input.bad())
        throw std::runtime_error("Cannot read incident spectrum: " + path);
    if (samples.size() < requiredSamples)
        throw std::runtime_error("Incident spectrum has fewer samples than required: " + path);
    return samples;
}
