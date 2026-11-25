// ==========================================================================
// 文件：fluid_common.glsl
// 功能：流体（LBM）着色器共用的小工具：线性索引与三维格点互转、边界法向、入/出流判断、
//       固体判断、流场三线性采样、体元温度回馈。仅被 include，不含 main。
// 主要模块：
//   1. 格点索引：fluidCellFromLinear、clampFluidCell
//   2. 水平边界：fluidHorizontalBoundaryNormal / fluidOutsideHorizontalNormal、入/出流判定
//   3. fluidIsSolid 与 sampleFluidField（对固体格不参与的三线性插值）
//   4. 与稀疏体元的耦合：coupledSurfaceTemperature、fixedMediumTemperature
// 与其他文件：包含 fluid_coupling.glsl（提供 fluidLinearIndex、fluidParameters 等）；
//         被 fluid_lbm.comp 等流体着色器使用。
// 约定：网格线性索引 = x + nx·(z + nz·y)，y 为竖直；边界法向为水平 (x,z) 二维向外单位向量；
//         fluidMeta[i].kind == 1 表示固体格；温度 K。
// ==========================================================================

#ifndef HISTREAM_FLUID_COMMON_GLSL
#define HISTREAM_FLUID_COMMON_GLSL

#include "fluid_coupling.glsl"

// 线性索引 → (x, y, z) 格点；与 fluidLinearIndex 互逆（索引 = x + nx·(z + nz·y)）。
ivec3 fluidCellFromLinear(uint index)
{
    int nx = fluidParameters.grid.x;
    int nz = fluidParameters.grid.z;
    int layer = nx * nz;
    int y = int(index) / layer;
    int remainder = int(index) - y * layer;
    int z = remainder / nx;
    return ivec3(remainder - z * nx, y, z);
}

// 把格点坐标限制在网格范围内（用于取最近的边界格）。
ivec3 clampFluidCell(ivec3 cell)
{
    return clamp(cell, ivec3(0), fluidParameters.grid.xyz - ivec3(1));
}

// 边界格的水平向外单位法向 (x, z)：位于 x/z 边界面时给出方向，角点取对角方向并归一化，内部为 0。
// fluidOutsideHorizontalNormal 是同样的计算，但输入为越界的（网格外）格点。
vec2 fluidHorizontalBoundaryNormal(ivec3 cell)
{
    vec2 normal = vec2(0.0);
    if (cell.x == 0) normal.x -= 1.0;
    if (cell.x == fluidParameters.grid.x - 1) normal.x += 1.0;
    if (cell.z == 0) normal.y -= 1.0;
    if (cell.z == fluidParameters.grid.z - 1) normal.y += 1.0;
    float magnitude = length(normal);
    return magnitude > 0.0 ? normal / magnitude : vec2(0.0);
}

vec2 fluidOutsideHorizontalNormal(ivec3 cell)
{
    vec2 normal = vec2(0.0);
    if (cell.x < 0) normal.x -= 1.0;
    if (cell.x >= fluidParameters.grid.x) normal.x += 1.0;
    if (cell.z < 0) normal.y -= 1.0;
    if (cell.z >= fluidParameters.grid.z) normal.y += 1.0;
    float magnitude = length(normal);
    return magnitude > 0.0 ? normal / magnitude : vec2(0.0);
}

// 环境风沿向外法向的分量（m/s）：负值表示风从该边界吹入（入流），正值为出流。
// fluidIsInlet / fluidIsOutlet 用 ±1e-5 的死区判定。
float fluidAmbientNormalSpeed(vec2 outwardNormal)
{
    return dot(fluidParameters.ambientWind.xz, outwardNormal);
}

bool fluidIsInlet(ivec3 cell)
{
    vec2 normal = fluidHorizontalBoundaryNormal(cell);
    return length(normal) > 0.0 && fluidAmbientNormalSpeed(normal) < -1e-5;
}

bool fluidIsOutlet(ivec3 cell)
{
    vec2 normal = fluidHorizontalBoundaryNormal(cell);
    return length(normal) > 0.0 && fluidAmbientNormalSpeed(normal) > 1e-5;
}

// 网格外或 kind==1 的格视为固体（边界反弹/不参与插值）。
bool fluidIsSolid(ivec3 cell)
{
    int index = fluidLinearIndex(cell);
    return index < 0 || fluidMeta[index].kind == 1;
}

// 模块：流场三线性插值。position 为格点坐标（单位：格），先限幅到网格内，对 8 个角点按权重累加。
// 速度场与地形场景下忽略固体角点，并把剩余权重重新归一化，避免固体格的 0 值拖低结果；
// 全部角点无效时返回 0。velocityField 选择 fluidVelocityA（速度）或 fluidScalarA（标量）。
vec4 sampleFluidField(vec3 position, bool velocityField)
{
    vec3 limited = clamp(position, vec3(0.0), vec3(fluidParameters.grid.xyz - ivec3(1)));
    ivec3 lower = ivec3(floor(limited));
    ivec3 upper = min(lower + ivec3(1), fluidParameters.grid.xyz - ivec3(1));
    vec3 weight = fract(limited);
    vec4 result = vec4(0.0);
    float totalWeight = 0.0;
    for (int dz = 0; dz <= 1; ++dz)
    for (int dy = 0; dy <= 1; ++dy)
    for (int dx = 0; dx <= 1; ++dx) {
        ivec3 cell = ivec3(dx == 0 ? lower.x : upper.x,
                           dy == 0 ? lower.y : upper.y,
                           dz == 0 ? lower.z : upper.z);
        int index = fluidLinearIndex(cell);
        bool terrainField = fluidParameters.heightProfile.x > 0.5
            || fluidParameters.couplingSpacing.w > 0.5;
        if (index < 0 || ((velocityField || terrainField) && fluidMeta[index].kind == 1)) continue;
        float factor = (dx == 0 ? 1.0 - weight.x : weight.x)
                     * (dy == 0 ? 1.0 - weight.y : weight.y)
                     * (dz == 0 ? 1.0 - weight.z : weight.z);
        result += factor * (velocityField ? fluidVelocityA[index] : fluidScalarA[index]);
        totalWeight += factor;
    }
    return totalWeight > 1e-6 ? result / totalWeight : vec4(0.0);
}

// 体元地表温度（K）= 受阳/阴节点平均；体元下标越界、非有限或超出 [150, 2000] K 时返回 fallback。
float coupledSurfaceTemperature(int sparseIndex, float fallback)
{
    if (sparseIndex < 0 || sparseIndex >= setting.voxelCount) return fallback;
    float value = 0.5 * (voxelTempes[sparseIndex].sunlit + voxelTempes[sparseIndex].shaded);
    return (isnan(value) || isinf(value) || value < 150.0 || value > 2000.0)
        ? fallback : value;
}

// 参与介质冠层若设置了固定温度（>0，K）则返回该值，否则 fallback。
float fixedMediumTemperature(int sparseIndex, float fallback)
{
    if (sparseIndex < 0 || sparseIndex >= setting.voxelCount) return fallback;
    uint instanceIndex = voxelLinks[sparseIndex].instanceId;
    uint meshIndex = instanceLinks[instanceIndex].meshId;
    uint canopyIndex = meshLinks[meshIndex].canopyId;
    Canopy canopy = canopies[canopyIndex];
    return isParticipatingMedium(canopy) && canopy.fixedTemperature > 0.0
        ? canopy.fixedTemperature : fallback;
}

#endif
