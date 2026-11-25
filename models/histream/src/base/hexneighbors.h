// ==========================================================================
// 文件：hexneighbors.h
// 功能：HEX（异质体元）按当前尺度的 64 位位掩码投影，计算沿轴的消光系数 q 与相邻体元间的
//       间隙相关量 beta（头文件内联实现，无状态）
// 主要模块：
//   1. bits：64 位字的置位个数（popcount）
//   2. word：取掩码的第 index 个 64 位字，并屏蔽超出像素数的尾部位
//   3. gap：由覆盖掩码得到间隙率（未被遮挡的像素占比）
//   4. axisQ：由 Beer-Lambert 形式 q = -ln(gap) / (rho · side) 求沿轴的消光系数
//   5. beta：相邻两个体元投影之间的联合间隙对数相关量
// 数据流：scene.cpp 把每个体元三个轴向的 unionMask（每个像素 1 位）累积好 → 本文件求 axisQ
//         写入 VoxelHex 的 ax/ay/az，求 beta 写入 children 的六个面相关槽
// 与其他文件的关系：hexprojection.h 是同类量的另一种来源（由 LOD 表的精细网格计算）
// 注意：掩码位与像素一一对应，像素总数 pixels = sampleN²（待核对）；间隙率下限取 0.5/pixels，
//       避免 log(0)
// ==========================================================================

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace hexneighbors {

// 作用：统计 64 位整数中值为 1 的位数（Kernighan 逐次清除最低位法）。
inline std::uint64_t bits(std::uint64_t value)
{
    std::uint64_t count = 0;
    while (value != 0U) {
        value &= value - 1U;
        ++count;
    }
    return count;
}

// 作用：读取掩码 mask 的第 index 个 64 位字。
// 说明：index 越界按全 0 处理；最后一个字中超出 pixels 的高位被清零，
//       防止未使用的尾部位被误计入覆盖像素。
inline std::uint64_t word(const std::vector<std::uint64_t>& mask,
                          std::size_t index, std::size_t pixels)
{
    std::uint64_t value = index < mask.size() ? mask[index] : 0U;
    if (index == pixels / 64U && pixels % 64U != 0U) {
        value &= (std::uint64_t{1} << (pixels % 64U)) - 1U;
    }
    return value;
}

// 作用：间隙率 = 1 - 被覆盖像素数 / 像素总数，即光线穿过体元不被遮挡的概率。
// 输入：覆盖掩码 mask、像素总数 pixels（为 0 时抛异常）；输出：[0, 1] 的无量纲值。
inline double gap(const std::vector<std::uint64_t>& mask, std::size_t pixels)
{
    if (pixels == 0U) throw std::runtime_error("HEX projection has no samples");
    std::uint64_t covered = 0;
    for (std::size_t index = 0; index < (pixels + 63U) / 64U; ++index) {
        covered += bits(word(mask, index, pixels));
    }
    return 1.0 - static_cast<double>(covered) / static_cast<double>(pixels);
}

// 作用：由间隙率反演沿某一轴向的消光系数 q = -ln(gap) / (density · side)。
// 输入：gapFraction 间隙率；density 体元密度 rho；side 体元边长；pixels 像素数。
// 说明：density 或 side 非正时返回 0；间隙率被限幅到 [0.5/pixels, 1]，
//       即全遮挡时按“半个像素的间隙”处理，避免对数发散。
inline float axisQ(double gapFraction, double density, double side,
                   std::size_t pixels)
{
    if (!(density > 0.0) || !(side > 0.0)) return 0.0f;
    const double lowerGap = 0.5 / static_cast<double>(pixels);
    return static_cast<float>(-
        std::log(std::clamp(gapFraction, lowerGap, 1.0)) / (density * side));
}

// 作用：相邻两个体元在同一轴向投影的间隙相关量 beta（对数形式，无量纲）。
// 输入：两个体元的覆盖掩码 first / second 与像素数 pixels。
// 说明：任一方完全无间隙时返回 0（视为不相关）；联合间隙率由两掩码的按位或求得，
//       并限幅在 Fréchet 边界 [max(下限², gA+gB-1), min(gA, gB)] 内，
//       保证 beta 数值有限。beta = 0 表示两侧间隙相互独立。
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
    // 步骤：联合间隙率 = 1 - 两掩码并集的覆盖比例。
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
