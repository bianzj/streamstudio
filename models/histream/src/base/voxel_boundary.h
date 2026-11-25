// ==========================================================================
// 文件：voxel_boundary.h
// 功能：由体元（整数格点）集合提取外表面四边形，供 voxeldesigner 生成边界网格。
// 主要模块：
//   1. 类型别名与 Hash：Cell（整数格点）、Point、Quad（4 顶点）、格点哈希
//   2. emit：对每个占据体元的 6 个面，邻格为空则输出该面的四边形与外法向方向
// 数据流：输入体元格点列表 → 去重放入 unordered_set → 逐面判断邻居 → 回调 append(quad, 方向)。
// 关系：被 voxeldesigner.cpp 调用。
// 约定：右手坐标系；单位为格点（体元边长 1）；每个四边形的两个三角形 (0,1,2)、(0,2,3) 法向朝外。
// ==========================================================================

#pragma once

#include <array>
#include <cstddef>
#include <unordered_set>

namespace voxel_boundary {
using Cell = std::array<int, 3>;
using Point = std::array<float, 3>;
using Quad = std::array<Point, 4>;
// 格点哈希：boost 风格 hash_combine（0x9e3779b9 混合）逐分量组合，供 unordered_set 使用。
struct Hash {
    size_t operator()(const Cell& cell) const noexcept {
        size_t hash = 0;
        for (int value : cell)
            hash ^= std::hash<int>{}(value) + size_t(0x9e3779b9) + (hash << 6) + (hash >> 2);
        return hash;
    }
};

// 作用：遍历占据体元的 6 个面，邻居不被占据的面即外表面，输出四边形。
// 输入：cells 为 [i,j,k] 整数三元组序列；append(quad, 面外法向格点方向) 为输出回调。
// 说明：输入先去重，因此重复格点不会重复输出；输出顺序取决于 unordered_set 遍历顺序。
// Right-handed coordinates; both triangles (0,1,2) and (0,2,3) point OUT.
// Keep the input cell/state order unchanged; only the geometric boundary changes.
template<class Cells, class Emit>
void emit(const Cells& cells, Emit append) {
    // 6 个面的外法向方向，顺序 -x,+x,-y,+y,-z,+z，与下方 corners 的面顺序一一对应。
    static constexpr Cell directions[6] = {
        {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1}};
    // 每个面的 4 个角点（相对体元最小角的 0/1 偏移），顶点顺序保证右手系下法向朝外。
    static constexpr float corners[6][4][3] = {
        {{0,0,0},{0,0,1},{0,1,1},{0,1,0}},
        {{1,0,0},{1,1,0},{1,1,1},{1,0,1}},
        {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},
        {{0,1,0},{0,1,1},{1,1,1},{1,1,0}},
        {{0,0,0},{0,1,0},{1,1,0},{1,0,0}},
        {{0,0,1},{1,0,1},{1,1,1},{0,1,1}}};
    // 步骤 1：去重并建立占据集合，便于 O(1) 查询邻居。
    std::unordered_set<Cell, Hash> occupied;
    occupied.reserve(cells.size());
    for (const auto& cell : cells) occupied.insert({cell[0], cell[1], cell[2]});
    // 步骤 2：对每个体元的每个面，邻居被占据则面在内部，跳过；否则平移角点输出。
    for (const auto& cell : occupied) {
        for (int face = 0; face < 6; ++face) {
            Cell neighbour{cell[0] + directions[face][0], cell[1] + directions[face][1],
                           cell[2] + directions[face][2]};
            if (occupied.count(neighbour)) continue;
            Quad quad;
            for (int vertex = 0; vertex < 4; ++vertex)
                for (int axis = 0; axis < 3; ++axis)
                    quad[vertex][axis] = float(cell[axis]) + corners[face][vertex][axis];
            append(quad, directions[face]);
        }
    }
}
} // namespace voxel_boundary
