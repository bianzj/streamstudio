#ifndef HISTREAM_FLUID_COMMON_GLSL
#define HISTREAM_FLUID_COMMON_GLSL

#include "fluid_coupling.glsl"

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

ivec3 clampFluidCell(ivec3 cell)
{
    return clamp(cell, ivec3(0), fluidParameters.grid.xyz - ivec3(1));
}

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

bool fluidIsSolid(ivec3 cell)
{
    int index = fluidLinearIndex(cell);
    return index < 0 || fluidMeta[index].kind == 1;
}

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

float coupledSurfaceTemperature(int sparseIndex, float fallback)
{
    if (sparseIndex < 0 || sparseIndex >= setting.voxelCount) return fallback;
    float value = 0.5 * (voxelTempes[sparseIndex].sunlit + voxelTempes[sparseIndex].shaded);
    return (isnan(value) || isinf(value) || value < 150.0 || value > 2000.0)
        ? fallback : value;
}

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
