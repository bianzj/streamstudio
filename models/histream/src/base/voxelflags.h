// ==========================================================================
// 文件：voxelflags.h
// 功能：体元传输（VoxelRT / VoxelEB）的运行期开关，写入推送常量 voxelFlags。
//   优先级：环境变量 > 工程 control 字段 > 引擎缺省。位定义必须与 shader/voxel_flags.glsl 一致。
//   漫射方向旋转   工程 control.voxelDirectionRotation = "legacy"（缺省）| "rot"；环境 HISTREAM_VOXEL_DIRROT = legacy | rot | rot-all
//       legacy ：64 方向格点固定（VoxelRT 仅按体元哈希绕 y 轴旋转），与 2026-11-26 之前的版本逐位相同。
//                环境变量也接受 off / 0 / false / fixed。
//       rot    ：VoxelRT 漫射 + VoxelEB 热红外漫射按体元哈希做完整三维随机旋转
//                （接收体权重按旋转后方向解析计算；TIR 的旋转只取决于体元编号，
//                  不随能量平衡迭代变化，保证温度不动点不抖动）。耗时约 +10%。
//       rot-all：rot + VoxelEB 短波多次散射（保留给后续实现；当前着色器未使用，等同 rot）。
//   旋转共享块     工程 control.voxelDirectionRotationBlock = 1（缺省）| 2 | 4 | 8 | 16；环境 HISTREAM_VOXEL_DIRROT_BLOCK
//       rot 时 n³ 体元块共用一个旋转（更快、更相干）。
//   空节点跳过     工程 control.voxelEmptySkip（布尔，缺省 true）；环境 HISTREAM_VOXEL_SKIP = on | off
//       on ：沿体元网格的逐格 DDA 中用 NanoVDB 层级跳过整块空节点（8/128/4096 格）并裁剪到根包围盒；
//       off：旧版逐格 DDA（与 2026-11-26 之前的版本逐位相同；on 时结果在浮点舍入内相同，不逐位相同）。
//   HISTREAM_VOXEL_PASS_TIMING = 1：各阶段累计耗时（CPU 墙钟，含 GPU 等待）写入标准输出。
// ==========================================================================
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace voxelflags {

constexpr int kDirRot = 1;       // VOXEL_FLAG_DIRROT
constexpr int kDirRotAll = 2;    // VOXEL_FLAG_DIRROT_SCATTER
constexpr int kHdda = 4;         // VOXEL_FLAG_HDDA
constexpr int kValid = 1 << 20;  // VOXEL_FLAG_VALID

// Engine defaults (project fields absent): 0 = legacy angular sets, 1 = rotated sets.
constexpr int kDefaultDirectionRotation = 0;
constexpr int kDefaultDirectionRotationBlock = 1;
constexpr bool kDefaultEmptySkip = true;

// Proxy-geometry vertex format of the voxel meshes (control.voxelProxyGeometry / HISTREAM_VOXEL_PROXY), bits 12-13 of
// voxelFlags (VOXEL_PROXY_* in shader/voxel_flags.glsl):
//   0 full            44 B VertexAttribute (position, normal, colour, uv) + 32-bit index buffer (legacy layout)
//   1 position        12 B position-only vertices + index buffer
//   2 positionNoIndex 12 B position-only triangle-list vertices, no index buffer (BLAS indexType NONE)
constexpr int kProxyFull = 0;
constexpr int kProxyPosition = 1;
constexpr int kProxyPositionNoIndex = 2;
constexpr int kProxyShift = 12;
// Default: positionNoIndex. Output bit-identical to "full" (verified by SHA-256 on VoxelRT/VoxelEB cases, see
// docs/voxel-proxy-geometry.md); lifts the 16.26 M-face limit of one 44 B-vertex buffer (about 40 M faces with it).
constexpr int kDefaultProxyGeometry = kProxyPositionNoIndex;

inline int proxyMode(int flags) { return (flags >> kProxyShift) & 3; }

inline int parseProxy(const std::string& text, int fallback)
{
    if (text == "full" || text == "legacy" || text == "0") return kProxyFull;
    if (text == "position" || text == "positiononly" || text == "1") return kProxyPosition;
    if (text == "positionnoindex" || text == "noindex" || text == "2") return kProxyPositionNoIndex;
    return fallback;
}

inline std::string lowerEnvironment(const char* name)
{
    const char* value = std::getenv(name);
    std::string text = value ? value : "";
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Snap a block edge to 1, 2, 4, 8 or 16 (the shader stores log2).
inline int blockShift(int edge)
{
    int shift = 0;
    while ((2 << shift) <= edge && shift < 4) ++shift;
    return shift;
}

// Flags from the project values (rotation 0/1, block edge, empty-space skip, proxy geometry) with the environment
// variables overriding them.
inline int resolve(int projectRotation, int projectBlock, bool projectSkip,
                   int projectProxy = kDefaultProxyGeometry)
{
    int flags = kValid;
    flags |= (parseProxy(lowerEnvironment("HISTREAM_VOXEL_PROXY"), projectProxy) & 3) << kProxyShift;
    bool rotated = projectRotation != 0;
    bool rotatedAll = false;
    const std::string rotation = lowerEnvironment("HISTREAM_VOXEL_DIRROT");
    if (rotation == "legacy" || rotation == "off" || rotation == "0" || rotation == "false" || rotation == "fixed") rotated = false;
    else if (rotation == "rot" || rotation == "rot-all") { rotated = true; rotatedAll = rotation == "rot-all"; }
    if (rotated) flags |= kDirRot;
    if (rotatedAll) flags |= kDirRotAll;
    int block = projectBlock;
    if (const char* text = std::getenv("HISTREAM_VOXEL_DIRROT_BLOCK")) if (*text) block = std::atoi(text);
    flags |= blockShift(block) << 8;
    bool skip = projectSkip;
    const std::string skipText = lowerEnvironment("HISTREAM_VOXEL_SKIP");
    if (skipText == "on" || skipText == "1" || skipText == "hdda" || skipText == "true") skip = true;
    else if (skipText == "off" || skipText == "0" || skipText == "false" || skipText == "legacy") skip = false;
    if (skip) flags |= kHdda;
    return flags;
}

// Engine defaults with the environment overrides (callers without a project, e.g. built-in examples).
inline int fromEnvironment()
{
    return resolve(kDefaultDirectionRotation, kDefaultDirectionRotationBlock, kDefaultEmptySkip);
}

// Test knob (VoxelEB only): HISTREAM_VOXEL_TEST_RADIANCE_VOXELS=N makes the radiance buffer layout (sky-cache and
// image-environment bases) behave as for N voxels (N >= the real count), so that the 64-bit element indices beyond
// 2^32 entries (N > 44.7 M) can be exercised with a small scene; results must equal the normal run bit for bit.
inline int testRadianceVoxels(int realCount)
{
    const char* value = std::getenv("HISTREAM_VOXEL_TEST_RADIANCE_VOXELS");
    if (!value || !*value) return realCount;
    const long long requested = std::atoll(value);
    return requested > realCount ? static_cast<int>(std::min<long long>(requested, 2147483647LL)) : realCount;
}

inline bool passTimingEnabled()
{
    const char* value = std::getenv("HISTREAM_VOXEL_PASS_TIMING");
    return value && *value && std::string(value) != "0";
}

}  // namespace voxelflags
