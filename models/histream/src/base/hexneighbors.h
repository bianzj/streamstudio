#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace hexneighbors {

inline std::uint64_t bits(std::uint64_t value)
{
    std::uint64_t count = 0;
    while (value != 0U) {
        value &= value - 1U;
        ++count;
    }
    return count;
}

inline std::uint64_t word(const std::vector<std::uint64_t>& mask,
                          std::size_t index, std::size_t pixels)
{
    std::uint64_t value = index < mask.size() ? mask[index] : 0U;
    if (index == pixels / 64U && pixels % 64U != 0U) {
        value &= (std::uint64_t{1} << (pixels % 64U)) - 1U;
    }
    return value;
}

inline double gap(const std::vector<std::uint64_t>& mask, std::size_t pixels)
{
    if (pixels == 0U) throw std::runtime_error("HEX projection has no samples");
    std::uint64_t covered = 0;
    for (std::size_t index = 0; index < (pixels + 63U) / 64U; ++index) {
        covered += bits(word(mask, index, pixels));
    }
    return 1.0 - static_cast<double>(covered) / static_cast<double>(pixels);
}

inline float axisQ(double gapFraction, double density, double side,
                   std::size_t pixels)
{
    if (!(density > 0.0) || !(side > 0.0)) return 0.0f;
    const double lowerGap = 0.5 / static_cast<double>(pixels);
    return static_cast<float>(-
        std::log(std::clamp(gapFraction, lowerGap, 1.0)) / (density * side));
}

// log(P(gap in both neighbouring projections)) - log(P(gap A)) - log(P(gap B))
inline float beta(const std::vector<std::uint64_t>& first,
                  const std::vector<std::uint64_t>& second,
                  std::size_t pixels)
{
    const double firstGap = gap(first, pixels);
    const double secondGap = gap(second, pixels);
    if (firstGap <= 0.0 || secondGap <= 0.0) return 0.0f;

    std::uint64_t coveredUnion = 0;
    for (std::size_t index = 0; index < (pixels + 63U) / 64U; ++index) {
        coveredUnion += bits(word(first, index, pixels) |
                              word(second, index, pixels));
    }
    const double rawJoint = 1.0 - static_cast<double>(coveredUnion) /
        static_cast<double>(pixels);
    const double floor = 0.5 / static_cast<double>(pixels);
    const double joint = std::clamp(rawJoint,
                                    std::max(floor * floor,
                                             firstGap + secondGap - 1.0),
                                    std::min(firstGap, secondGap));
    return static_cast<float>(std::log(joint) - std::log(firstGap) -
                              std::log(secondGap));
}

}  // namespace hexneighbors
