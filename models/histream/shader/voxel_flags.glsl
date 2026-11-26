// ==========================================================================
// 文件：voxel_flags.glsl
// 功能：体元传输选项位（推送常量 setting.voxelFlags），与 src/base/voxelflags.h 一致。
//   VOXEL_FLAG_VALID          ：宿主已写入本字段（旧版宿主在同一位置推送的是别的值/未初始化的字节，
//                               没有该位时一律按 0 处理，因此新旧着色器/可执行文件混用也保持旧行为）
//   VOXEL_FLAG_DIRROT         ：VoxelRT 漫射 / VoxelEB 热红外漫射使用按体元哈希的三维随机旋转方向集
//   VOXEL_FLAG_DIRROT_SCATTER ：保留（VoxelEB 短波散射，当前未使用）
//   VOXEL_FLAG_HDDA           ：开启 NanoVDB 层级跳空与包围盒裁剪（缺省关闭 = 旧版逐格 DDA）
//   VOXEL_PROXY_*（第 12-13 位）：体元代理网格的顶点格式（control.voxelProxyGeometry）
//                               0 full 44 B VertexAttribute + 索引缓冲（旧格式）
//                               1 position 12 B 仅位置顶点 + 索引缓冲
//                               2 positionNoIndex 12 B 仅位置三角形列表，无索引缓冲（第 p 个三角形 = 顶点 3p..3p+2）
//                               旧宿主（无 VALID 位）一律按 0 处理。
// ==========================================================================
#ifndef VOXEL_FLAGS_GLSL
#define VOXEL_FLAGS_GLSL

const int VOXEL_FLAG_DIRROT = 1;
const int VOXEL_FLAG_DIRROT_SCATTER = 2;
const int VOXEL_FLAG_HDDA = 4;
const int VOXEL_FLAG_VALID = 1 << 20;
const int VOXEL_PROXY_SHIFT = 12;
const int VOXEL_PROXY_FULL = 0;
const int VOXEL_PROXY_POSITION = 1;
const int VOXEL_PROXY_POSITION_NOINDEX = 2;

// 选项 flag 是否开启（宿主未写 voxelFlags 时恒为 false）。
bool VoxelFlag(int flag)
{
    return (setting.voxelFlags & VOXEL_FLAG_VALID) != 0 && (setting.voxelFlags & flag) != 0;
}

// 代理网格顶点格式（宿主未写 voxelFlags 时为 VOXEL_PROXY_FULL）。
int VoxelProxyMode()
{
    return (setting.voxelFlags & VOXEL_FLAG_VALID) != 0 ? ((setting.voxelFlags >> VOXEL_PROXY_SHIFT) & 3) : VOXEL_PROXY_FULL;
}

#endif
