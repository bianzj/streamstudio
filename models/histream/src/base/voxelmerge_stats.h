// ==========================================================================
// 文件：voxelmerge_stats.h
// 功能：两级体元（fixscale = k·scale）的潜力统计，只读，不改变任何行为。
//   HISTREAM_VOXEL_MERGE_STATS=1    ：体元化完成后打印 VOXEL_MERGE_* 行（表格），然后继续运行；
//   HISTREAM_VOXEL_MERGE_STATS=only ：打印后立即结束进程（只做体元化，几秒内出表）。
// 统计口径（"可合并"的定义）：
//   * 只考虑浊介质冠层（canopy.structureType == 0）与参与介质（火/雾，structureType 2/3）的体元；
//     刚体冠层（structureType 1，枝干）、土壤、建筑、水体都当作"表面"，不合并；HEX 层级模型 5 的体元不合并。
//   * 对齐的 k×k×k 块（块下标 = floor(体元下标 / k)，从格点原点起算）被合并，当且仅当：
//     块内 k³ 个格全部存在、全部为可合并类型、材质键完全相同
//     （type、spectralId、thermalId、canopyId、bioId、aeroId、HEX 模型），
//     且体密度 d（HEX 的 rho，或几何叶密度 leafDensity，或冠层缺省密度/介质消光）与三轴聚集指数
//     （HEX 的 ax/ay/az，无 HEX 时为 1）的相对极差 (max-min)/平均 ≤ tol。
//   * tol 取 0（逐位相等）、5%、10%、20%，另给 inf（只看几何：块是否被同种材质填满，是上界）。
//   * 两级：只有细格与 k 倍粗格两种；"多级"行允许 8/4/2 逐级回退（仅供参考）。
// 内存口径：bufferId 索引的辐射场 VoxelRad（VoxelEB：N×(64×8 + 64×4 + n_wave×4) 字节，含天空透过率缓存与图像环境槽；
//   VoxelRT：N×(64 + 2·n_wave)×8 字节）、VoxelLink 48 字节/体元、代理面三角形（每个浊介质体元 3 张中面 = 6 个三角形）。
// ==========================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace voxelmerge {

struct CellInfo {
    glm::ivec3 pos{0};
    int cls{5};        // 0 浊介质冠层，1 参与介质，2 刚体冠层，3 土壤，4 建筑，5 水体/其它
    bool eligible{false};
    std::array<int, 7> key{};
    float d{0.0f};
    std::array<float, 3> ci{1.0f, 1.0f, 1.0f};
};

inline uint64_t blockKey(glm::ivec3 b)
{
    return (uint64_t(uint32_t(b.x + (1 << 20))) << 42) | (uint64_t(uint32_t(b.y + (1 << 20))) << 21) | uint64_t(uint32_t(b.z + (1 << 20)));
}

struct Result {
    int blocks{0};
    int covered{0};
    int coveredTurbid{0};
};

template <class Model>
void report(const std::shared_ptr<Model>& modelio, const char* modelName)
{
    const char* env = std::getenv("HISTREAM_VOXEL_MERGE_STATS");
    if (!env || !*env || std::string(env) == "0") return;
    const bool only = std::string(env) == "only";

    const auto& links = modelio->m_voxelio->voxellinks;
    const auto& hexs = modelio->m_voxelio->voxelHexs;
    const auto& meshLinks = modelio->m_meshio->meshLinks;
    const auto& canopies = modelio->m_meshio->canopies;
    const auto& instanceLinks = modelio->m_instanceio->instanceLinks;

    std::vector<CellInfo> cells(links.size());
    std::array<int, 6> classCount{};
    int hierarchical = 0;
    for (size_t i = 0; i < links.size(); ++i) {
        const VoxelLink& link = links[i];
        CellInfo& cell = cells[i];
        cell.pos = link.voxelId;
        if (!link.isValid || link.instanceId < 0 || size_t(link.instanceId) >= instanceLinks.size()) { cell.cls = 5; ++classCount[5]; continue; }
        const MeshLink& mesh = meshLinks[instanceLinks[link.instanceId].meshId];
        const int type = mesh.type;  // 1 soil, 2 vegetation, 3 building, 4 water
        int cls = 5;
        const Canopy* canopy = (mesh.canopyId >= 0 && size_t(mesh.canopyId) < canopies.size()) ? &canopies[mesh.canopyId] : nullptr;
        if (type == 2 && canopy) cls = canopy->structureType == 0 ? 0 : (canopy->structureType == 1 ? 2 : 1);
        else if (type == 1) cls = 3;
        else if (type == 3) cls = 4;
        cell.cls = cls;
        ++classCount[cls];
        const VoxelHex* hex = (link.hexId >= 0 && size_t(link.hexId) < hexs.size()) ? &hexs[link.hexId] : nullptr;
        if (hex && hex->model == 5) ++hierarchical;
        cell.eligible = (cls == 0 || cls == 1) && !(hex && hex->model == 5);
        cell.key = {type, mesh.spectralId, mesh.thermalId, mesh.canopyId, mesh.bioId, link.aeroId, hex ? hex->model : -1};
        if (hex) {
            cell.d = hex->rho;
            cell.ci = {hex->ax, hex->ay, hex->az};
        } else if (cls == 1) {
            cell.d = canopy->extinction;
        } else {
            cell.d = link.leafDensity > 0.0f ? link.leafDensity : (canopy ? canopy->density : 0.0f);
        }
    }

    const int n = int(links.size());
    const int nTurbid = classCount[0] + classCount[1];
    const int nWave = std::max(modelio->n_wave, 1);
    const bool eb = std::string(modelName) == "VoxelEB";
    const double radBytes = eb ? double(64 * 8 + 64 * 4 + 4 * nWave) : double(64 + 2 * nWave) * 8.0;

    glm::ivec3 maxId(0);
    for (const auto& c : cells) maxId = glm::max(maxId, c.pos);
    std::printf("VOXEL_MERGE_SCENE\tmodel=%s\tscale=%.4g\tvoxels=%d\tgrid=%dx%dx%d\tturbidCanopy=%d\tmedium=%d\trigidVeg=%d\tsoil=%d\tbuilding=%d\twaterOther=%d\thexModel5=%d\tn_wave=%d\tradiationBytesPerVoxel=%.0f\n",
                modelName, modelio->stepsize_surface, n, maxId.x + 1, maxId.y + 1, maxId.z + 1, classCount[0], classCount[1], classCount[2],
                classCount[3], classCount[4], classCount[5], hierarchical, nWave, radBytes);

    // Evaluate a block set. taken: cells already merged at a coarser level (multi-level mode).
    auto evaluate = [&](int k, double tol, std::vector<char>& taken, bool commit, double minFill = 1.0) {
        Result result;
        std::unordered_map<uint64_t, std::vector<int>> blocks;
        blocks.reserve(cells.size() / 4 + 16);
        for (int i = 0; i < n; ++i) {
            if (!cells[i].eligible || taken[i]) continue;
            const glm::ivec3 p = cells[i].pos;
            blocks[blockKey(glm::ivec3(p.x >= 0 ? p.x / k : -((-p.x + k - 1) / k), p.y >= 0 ? p.y / k : -((-p.y + k - 1) / k),
                                       p.z >= 0 ? p.z / k : -((-p.z + k - 1) / k)))].push_back(i);
        }
        const size_t full = size_t(k) * k * k;
        for (auto& [key, list] : blocks) {
            if (list.size() < size_t(std::ceil(minFill * double(full) - 1e-9)) || list.size() > full) continue;
            const CellInfo& first = cells[list[0]];
            bool ok = true;
            float dMin = first.d, dMax = first.d, dSum = 0.0f;
            std::array<float, 3> cMin = first.ci, cMax = first.ci, cSum{0, 0, 0};
            for (int idx : list) {
                const CellInfo& c = cells[idx];
                if (c.key != first.key) { ok = false; break; }
                dMin = std::min(dMin, c.d); dMax = std::max(dMax, c.d); dSum += c.d;
                for (int a = 0; a < 3; ++a) { cMin[a] = std::min(cMin[a], c.ci[a]); cMax[a] = std::max(cMax[a], c.ci[a]); cSum[a] += c.ci[a]; }
            }
            const size_t present = list.size();
            if (!ok) continue;
            if (std::isfinite(tol)) {
                auto spread = [&](float lo, float hi, float sum) {
                    const double mean = sum / double(present);
                    return mean > 1e-12 ? (double(hi) - lo) / mean : (hi - lo > 0.0f ? 1e9 : 0.0);
                };
                if (spread(dMin, dMax, dSum) > tol) continue;
                bool ciOk = true;
                for (int a = 0; a < 3; ++a) ciOk &= spread(cMin[a], cMax[a], cSum[a]) <= tol;
                if (!ciOk) continue;
            }
            ++result.blocks;
            result.covered += int(present);
            if (commit) for (int idx : list) taken[idx] = 1;
        }
        return result;
    };

    const double tols[] = {0.0, 0.05, 0.10, 0.20, std::numeric_limits<double>::infinity()};
    std::printf("VOXEL_MERGE_HEADER\tk\ttol\tmergedBlocks\tcoveredFine\tvoxelsAfter\tvoxelReduction%%\tturbidReduction%%\tradiationMB_before\tradiationMB_after\tturbidTriangles_before\tturbidTriangles_after\n");
    auto printRow = [&](const char* label, double tol, const Result& r) {
        const int after = n - r.covered + r.blocks;
        const int turbidAfter = nTurbid - r.covered + r.blocks;
        std::printf("VOXEL_MERGE\t%s\t%s\t%d\t%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%d\t%d\n", label,
                    std::isfinite(tol) ? (std::to_string(int(tol * 100 + 0.5)) + "%").c_str() : "geom",
                    r.blocks, r.covered, after, 100.0 * (n - after) / std::max(n, 1),
                    100.0 * (nTurbid - turbidAfter) / std::max(nTurbid, 1), radBytes * n / 1048576.0, radBytes * after / 1048576.0,
                    6 * nTurbid, 6 * turbidAfter);
    };
    for (int k : {2, 4, 8}) {
        for (double tol : tols) {
            std::vector<char> taken(n, 0);
            printRow(("k=" + std::to_string(k)).c_str(), tol, evaluate(k, tol, taken, false));
        }
    }
    // Relaxed occupancy (information only): blocks that are only partly filled would also pick up the empty fine cells.
    for (int k : {2, 4, 8}) {
        for (double fill : {0.875, 0.75, 0.5}) {
            std::vector<char> taken(n, 0);
            printRow(("k=" + std::to_string(k) + " fill>=" + std::to_string(int(fill * 1000 + 0.5) / 10.0).substr(0, 4) + "%").c_str(), 0.10, evaluate(k, 0.10, taken, false, fill));
        }
    }
    for (double tol : tols) {  // multi-level: 8, then 4, then 2 (information only)
        std::vector<char> taken(n, 0);
        Result total;
        for (int k : {8, 4, 2}) {
            const Result r = evaluate(k, tol, taken, true);
            total.blocks += r.blocks; total.covered += r.covered;
        }
        printRow("multi8/4/2", tol, total);
    }
    std::fflush(stdout);
    if (only) std::_Exit(0);
}

}  // namespace voxelmerge
