#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace hexprojection {

using Cell = std::array<float, 4>;

// Logarithmic pairwise gap overlap for two neighbouring fine grids.
inline float overlapLogRatio(const std::vector<Cell>& first,
                             const std::vector<Cell>& second,
                             int n, int axis, float side)
{
    if (n < 1 || axis < 0 || axis > 2 ||
        first.size() != static_cast<std::size_t>(n) * n * n ||
        second.size() != first.size() || !(side > 0.0f)) {
        throw std::runtime_error("Invalid HEX overlap grid");
    }
    int transverse[2];
    int count = 0;
    for (int value = 0; value < 3; ++value) {
        if (value != axis) transverse[count++] = value;
    }
    double sumFirst = 0.0;
    double sumSecond = 0.0;
    double sumJoint = 0.0;
    for (int u = 0; u < n; ++u) {
        for (int v = 0; v < n; ++v) {
            double tauFirst = 0.0;
            double tauSecond = 0.0;
            for (int depth = 0; depth < n; ++depth) {
                int position[3];
                position[axis] = depth;
                position[transverse[0]] = u;
                position[transverse[1]] = v;
                const int index = (position[0] * n + position[1]) * n + position[2];
                tauFirst += first[index][axis] * side / n;
                tauSecond += second[index][axis] * side / n;
            }
            const double transmissionFirst = std::exp(-tauFirst);
            const double transmissionSecond = std::exp(-tauSecond);
            sumFirst += transmissionFirst;
            sumSecond += transmissionSecond;
            sumJoint += transmissionFirst * transmissionSecond;
        }
    }
    const double area = static_cast<double>(n) * n;
    const double gapFirst = sumFirst / area;
    const double gapSecond = sumSecond / area;
    if (gapFirst < 1.0e-12 || gapSecond < 1.0e-12) return 0.0f;
    return static_cast<float>(
        std::log(std::max(sumJoint / area, 1.0e-30)) -
        std::log(gapFirst) - std::log(gapSecond));
}

}  // namespace hexprojection
