// ==========================================================================
// 文件：voxel_radindex.glsl
// 功能：VoxelEB 辐射缓冲 voxelRads（经 setting.radianceAddress 物理地址访问）的 64 位元素下标。
//       方向场下标 voxel*64+angle、天空缝隙透过率与图像环境槽的下标（N*96 起）此前是 32 位 uint 运算，
//       体元数超过 2^32/96 = 44.7 M 时静默回绕、写坏方向场；现在全部用 uint64_t 计算（仅改地址运算，数值逐位不变）。
// 要求：主着色器已声明 GL_EXT_shader_explicit_arithmetic_types_int64（bindingLayout.h 的 uint64_t 推送常量本来就需要）。
// ==========================================================================
#ifndef VOXEL_RADINDEX_GLSL
#define VOXEL_RADINDEX_GLSL

// 方向场条目：voxel 号 v、方向号 k（0..DIFFUSE_ANGLE_NUM-1）。
#define VOXEL_RAD_FIELD(v, k) (uint64_t(v) * uint64_t(DIFFUSE_ANGLE_NUM) + uint64_t(k))

// 访问方向场/天空/环境槽条目：GLSL 不允许 64 位数组下标，所以按物理地址取该条目（VoxelRad 8 字节，地址 8 对齐）。
// 例：VOXEL_RAD(VOXEL_RAD_FIELD(v, k)).induced = x;  atomicAdd(VOXEL_RAD(i).induced, …)。
#define VOXEL_RAD(i) VoxelRadianceBuffer(setting.radianceAddress + uint64_t(i) * uint64_t(8)).values[0]

#endif