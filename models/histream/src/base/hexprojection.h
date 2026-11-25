// ==========================================================================
// 文件：hexprojection.h
// 功能：HEX 的 LOD（多层细分）表路径下，由两个相邻体元的精细网格求面方向的间隙相关量
// 主要模块：
//   1. Cell：精细网格单元的 4 个分量 {ax, ay, az, rho}（来自 HEX_LOD_V1 表）
//   2. overlapLogRatio：沿指定轴逐条光线积分光学厚度，得到两个体元透过率的对数联合比
// 数据流：scene.cpp 读取 HISTREAM_HEX_LOD_TABLE → 取每个根体元最精细层的 n³ 单元 →
//         对六个面的邻居调用本文件 → 结果写入根 VoxelHex 的 children[6..7] 槽
// 与其他文件的关系：hexneighbors.h 是未使用 LOD 表时（位掩码路径）的对应量
// 约定：网格线性下标 index = (x · n + y) · n + z；axis 取 0/1/2 对应 x/y/z；
//       单元大小为 side / n，Cell 前三个分量是各轴向的消光系数（单位 1/长度）
// ==========================================================================

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace hexprojection {

using Cell = std::array<float, 4>;

// 作用：计算相邻两个体元精细网格沿 axis 方向的间隙相关量
//       beta = ln(<T1·T2>) - ln(<T1>) - ln(<T2>)，<·> 为横截面上的平均。
// 输入：first / second 两个 n³ 网格；n 每边细分数；axis 视线轴；side 体元边长。
// 输出：无量纲 beta；输入非法时抛异常；任一间隙率 < 1e-12 时返回 0。
// 说明：T 为沿轴一条光线的透过率 exp(-τ)，τ 为该光线上各单元 Cell[axis]·side/n 之和。
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
    // 步骤 1：确定与 axis 垂直的两个横向轴（transverse），用于遍历横截面 (u, v)。
    int transverse[2];
    int count = 0;
    for (int value = 0; value < 3; ++value) {
        if (value != axis) transverse[count++] = value;
    }
    // 步骤 2：对每条横截面光线沿 depth 累加光学厚度，得到两个体元的透过率及其乘积。
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
    // 步骤 3：横截面平均得到间隙率与联合间隙率，联合项下限 1e-30 防止 log(0)。
    const double area = static_cast<double>(n) * n;
    const double gapFirst = sumFirst / area;
    const double gapSecond = sumSecond / area;
    if (gapFirst < 1.0e-12 || gapSecond < 1.0e-12) return 0.0f;
    return static_cast<float>(
        std::log(std::max(sumJoint / area, 1.0e-30)) -
        std::log(gapFirst) - std::log(gapSecond));
}

}  // namespace hexprojection
