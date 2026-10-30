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
