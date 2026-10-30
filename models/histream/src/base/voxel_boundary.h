#pragma once

#include <array>
#include <cstddef>
#include <unordered_set>

namespace voxel_boundary {
using Cell = std::array<int, 3>;
using Point = std::array<float, 3>;
using Quad = std::array<Point, 4>;
struct Hash {
    size_t operator()(const Cell& cell) const noexcept {
        size_t hash = 0;
        for (int value : cell)
            hash ^= std::hash<int>{}(value) + size_t(0x9e3779b9) + (hash << 6) + (hash >> 2);
        return hash;
    }
};

// Right-handed coordinates; both triangles (0,1,2) and (0,2,3) point OUT.
// Keep the input cell/state order unchanged; only the geometric boundary changes.
template<class Cells, class Emit>
void emit(const Cells& cells, Emit append) {
    static constexpr Cell directions[6] = {
        {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1}};
    static constexpr float corners[6][4][3] = {
        {{0,0,0},{0,0,1},{0,1,1},{0,1,0}},
        {{1,0,0},{1,1,0},{1,1,1},{1,0,1}},
        {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},
        {{0,1,0},{0,1,1},{1,1,1},{1,1,0}},
        {{0,0,0},{0,1,0},{1,1,0},{1,0,0}},
        {{0,0,1},{1,0,1},{1,1,1},{0,1,1}}};
    std::unordered_set<Cell, Hash> occupied;
    occupied.reserve(cells.size());
    for (const auto& cell : cells) occupied.insert({cell[0], cell[1], cell[2]});
    // FIXED[低] Bug: 外层遍历的是原始 cells 而非去重后的 occupied，输入含重复体元（多个实例/子网格重叠时常见）时同一外表面被重复输出，产生重合三角形（光线求交 z-fighting、面积统计翻倍）；已修复：外层改为遍历去重后的 occupied 集合（输出顺序随哈希顺序变化，几何语义不变）。
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
