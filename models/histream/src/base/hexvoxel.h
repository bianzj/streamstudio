//
// hexvoxel.h
//
// 异质性体元 (heterogeneous voxel, "hex voxel") 核心计算方法
//
// 方法 (对应 README "voxelrt-hex: 异质性体元的模拟"):
//   1) 以面元 (facet / 三角网格) 为三维结构基准 —— "面元先计算";
//   2) 将面元投影到 x/y/z 三个正交方向，分别计算投影叶面积 P 和
//      投影并集覆盖率 C，再由 Beer-Lambert 空隙率反演聚集指数：
//         CI = -ln(1-C) / P
//      C 只是中间量，不能直接作为 CI 使用；
//   3) 由三个正交聚集指数外推任意方向 d 的聚集指数：
//         CI(d) = |dx|·CIx + |dy|·CIy + |dz|·CIz
//   4) 同时计算叶面积密度 rho（m2/m3）；
//   5) (CIx, CIy, CIz, rho) 构成异质体元描述符。运行时消光始终为
//         rho * G * CI(d)，其中 G 与 CI 相互独立。
//
// 本文件自包含: 仅依赖 C++17 标准库, 无 Vulkan/nvvk/glm 依赖, 因此既被
// 主工程 (src/base/scene.cpp 的 OBJ 体素化链路) 复用, 也被独立 CLI 工具
// (src/tools/hexvoxel) 直接编译运行。
//

#ifndef HEXVOXEL_H
#define HEXVOXEL_H

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace hexvoxel {

// ---------------------------------------------------------------------------
// 数据结构 (体元坐标 = 世界坐标 / voxelSize, 单位体元边长 = 1)
// ---------------------------------------------------------------------------

struct Tri3 {
    float p0[3];
    float p1[3];
    float p2[3];
};

// OBJ 中的一个对象 (o 语句分组, 无分组则整体一个对象)
struct ObjInfo {
    std::string name;
    bool closed{false};   // 是否封闭流形 (每条边恰好被两个三角共享)
    std::uint32_t triOffset{0};
    std::uint32_t triCount{0};
};

struct HexScene {
    std::vector<Tri3> tris;
    std::vector<ObjInfo> objects;
    float minPos[3]{0, 0, 0};   // 平移后 (从原点开始)
    float maxPos[3]{0, 0, 0};
    float voxelSize{1.0f};        // 物理体元边长，用于把面元面积换算为叶面积密度
    float gridOrigin[3]{0, 0, 0}; // 平移量 (体元坐标, = floor(原始最小顶点)),
                                  //   hexGridIndex = floor(vertexVoxel) - gridOrigin

    std::uint32_t objectOfTri(std::uint32_t triIndex) const;
};

// 单个体元的异质参数 (16 字节, 可直接上传 GPU SSBO)
struct HexVoxelCg {
    float ax{1.0f};  // CIx：沿 x 的聚集指数
    float ay{1.0f};  // CIy：沿 y 的聚集指数
    float az{1.0f};  // CIz：沿 z 的聚集指数
    float rho{0.0f}; // 混沌介质叶面积密度 (m2/m3)
};

// 由正交指数外推任意单位方向的聚集指数
inline float aExtrap(const HexVoxelCg& h, float dx, float dy, float dz) {
    const float wx = std::abs(dx), wy = std::abs(dy), wz = std::abs(dz);
    const float weight = wx + wy + wz;
    if (weight <= 1.0e-8f) return 1.0f;
    return std::max(0.0f,
        (wx * h.ax + wy * h.ay + wz * h.az) / weight);
}

struct HexConfig {
    int sampleN{64};  // 投影并集覆盖率的截面采样网格 N x N
    int colM{4};      // 每列 (沿投影方向) 采样点数
    int volK{4};      // 体积采样网格 K^3
    bool includeInteriors{true}; // 封闭固体内部体元也激活为 FULL (A=1, rho=1)
};

// 体元状态
enum class VoxelKind : unsigned char { EMPTY = 0, FULL = 1, MIXED = 2 };

struct VoxelResult {
    int ix{0}, iy{0}, iz{0};   // 体元网格坐标 (以场景最小角为原点)
    VoxelKind kind{VoxelKind::EMPTY};
    HexVoxelCg hex;
    // 每个方向的原始投影面积、并集遮挡率以及遮挡采样掩膜。
    // 掩膜保留到世界体元合并阶段，使多个实例/地类重叠时能先求真实
    // 联合遮挡，再按各类别独立遮挡的比例分配贡献。
    std::array<float, 3> projectedArea{{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> cover{{0.0f, 0.0f, 0.0f}};
    std::array<std::vector<std::uint64_t>, 3> coverMask;
};

// 某个方向上: 外推估计 A_est(d) 与面元基准真值 A_true(d) 的对比统计
struct DirStats {
    float dir[3]{0, 0, 1};
    std::string label;
    float meanAest{0};
    float meanAtrue{0};
    float meanAbsErr{0};
    float rmsErr{0};
    int nVoxels{0}; // 参与统计的非空体元数
};

struct HexResult {
    float voxelSize{1.0f};
    int sampleN{0};
    int gridX{0}, gridY{0}, gridZ{0};      // 体元网格尺寸
    long long nEmpty{0};
    long long nFull{0};
    long long nMixed{0};
    float meanRhoAll{0};     // 平均体密度 (全部体元, 含空)
    float meanRhoActive{0};  // 平均体密度 (非空体元)
    float meanA{0};          // 非空体元三方向指数均值
    float maxA{0};
    float maxRho{0};
    std::vector<VoxelResult> voxels;  // 仅非空体元 (FULL + MIXED)
    std::vector<DirStats> dirStats;
    double computeMs{0};
};

// 解析 OBJ (支持 v/vt/vn/f、o/g、usemtl), 缩放到体元坐标, 并按边配对
// 判断每个对象是否封闭。返回 false 时 *err 给出原因。
bool loadObj(const std::string& path, float voxelSize, HexScene& out,
             std::string* err);

// 主计算: 对网格 [0,gx)x[0,gy)x[0,gz) (由场景 AABB 推出) 逐体元计算
// (Ax, Ay, Az, rho) 与体元状态; 对 extraDirs 中的每个单位方向统计
// A_est(d) vs A_true(d) 的误差 (A_true 为面元直接投影基准 —— "面元先计算")。
HexResult compute(const HexScene& scene, const HexConfig& cfg,
                  const float* extraDirs, int nExtraDirs,
                  const char* const* dirLabels = nullptr);

// 输出
std::string toTsv(const HexResult& result);
std::string toJson(const HexResult& result, const std::string& objPath);

}  // namespace hexvoxel

#endif  // HEXVOXEL_H
