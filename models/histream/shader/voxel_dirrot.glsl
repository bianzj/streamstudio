// ==========================================================================
// 文件：voxel_dirrot.glsl
// 功能：漫射 64 方向格点（global.glsl 的 DIFFUSE_DIRECTIONS）的按体元哈希三维随机旋转，
//       用于 VoxelRT 漫射与 VoxelEB 热红外漫射（HISTREAM_VOXEL_DIRROT=rot，
//       即 setting.voxelFlags 的 VOXEL_FLAG_DIRROT 位）。
// 约定：
//   * 旋转由 (体元编号, salt) 的整数哈希唯一确定（无迭代/时间依赖），同一体元每次运行相同；
//     salt 由调用方给出（VoxelRT：碰撞阶数/采样组/面编号；TIR：面编号，固定不随能量平衡迭代变化）。
//   * 旋转后方向集仍是 32 对精确反向对、等立体角，故球面平均权重 1/64 与半球 2/64 不变；
//     但 |y| 不再等于 (2k+1)/64，所以 Canopy.diffuseG[] 的下标查表不再适用，
//     叶片投影 G 与接收体权重必须对旋转后的方向解析计算（lidfProjection）。
//   * 旧版（flag 关）：VoxelRT 只绕 y 轴旋转（方位偏移），|y| 层不变，查表仍有效。
// 依赖：global.glsl、voxel_flags.glsl、bindingLayout 的 setting。
// 另有 C++ 孪生实现 tools/test-diffuse-directions.cpp（同一哈希与四元数公式），改动需同步。
// ==========================================================================
#ifndef VOXEL_DIRROT_GLSL
#define VOXEL_DIRROT_GLSL

#include "voxel_flags.glsl"

// pcg3d 整数哈希（Jarzynski & Olano 2020）。
uvec3 voxelPcg3d(uvec3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

// 均匀分布随机三维旋转（Shoemake 单位四元数），由体元编号与 salt 的哈希确定。
mat3 voxelDirRotation(ivec3 voxelId, uint salt)
{
    uvec3 h = voxelPcg3d(uvec3(voxelId) ^ uvec3(salt * 0x9E3779B9u, salt * 0x85EBCA6Bu, salt * 0xC2B2AE35u));
    h = voxelPcg3d(h);
    float u1 = float(h.x >> 8u) * (1.0 / 16777216.0);
    float u2 = float(h.y >> 8u) * (1.0 / 16777216.0);
    float u3 = float(h.z >> 8u) * (1.0 / 16777216.0);
    float a = sqrt(1.0 - u1);
    float b = sqrt(u1);
    float x = a * sin(6.28318530718 * u2);
    float y = a * cos(6.28318530718 * u2);
    float z = b * sin(6.28318530718 * u3);
    float w = b * cos(6.28318530718 * u3);
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;
    return mat3(1.0 - 2.0 * (yy + zz), 2.0 * (xy + wz), 2.0 * (xz - wy),
                2.0 * (xy - wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz + wx),
                2.0 * (xz + wy), 2.0 * (yz - wx), 1.0 - 2.0 * (xx + yy));
}

// 当前线程的漫射旋转（rot 模式下由调用方通过 SelectDiffuseRotation 设置）。
mat3 gDiffuseRotation = mat3(1.0);

bool VoxelDirRotEnabled()
{
    return VoxelFlag(VOXEL_FLAG_DIRROT);
}

// 漫射采样组数（VoxelRT），缺省 5。做高采样参考解时用 glslc -DVOXEL_DIFFUSE_SETS=n 单独编译（编译期常数，
// 保证缺省构建的循环展开与旧版相同，结果逐位不变）。
#ifndef VOXEL_DIFFUSE_SETS
#define VOXEL_DIFFUSE_SETS 5
#endif

void SelectDiffuseRotation(ivec3 voxelId, uint salt)
{
    if (VoxelDirRotEnabled()) gDiffuseRotation = voxelDirRotation(voxelId >> ((setting.voxelFlags >> 8) & 7), salt);
}

#endif
