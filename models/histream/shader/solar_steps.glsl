// ==========================================================================
// 文件：solar_steps.glsl
// 功能：计算太阳可见度射线（solar-visibility ray）的步数预算 SolarStepLimit，
//   供 VoxelRT 与 VoxelEB（voxelrad_solar）使用。
// 主要模块：
//   1. SOLAR_TRANSMITTANCE_CUTOFF：透过率截断阈值
//   2. SolarStepLimit：按体元网格大小/周期边界估计一条直线最多需要的 closest-hit 查询次数
// 依赖：traceray_comp.glsl（PeriodicSceneTop、periodicCrossingLimit）与 setting（voxelSize、
//   periodicNeighborCount）；调用方在 include 本文件之前必须已定义这些。
// ==========================================================================

#ifndef HISTREAM_SOLAR_STEPS_GLSL
#define HISTREAM_SOLAR_STEPS_GLSL

// Step budget of a solar-visibility ray (VoxelRT and VoxelEB voxelrad_solar).
//
// A solar ray is one straight line, so it can be followed until it leaves the
// scene instead of stopping after radiationMaxSteps (a budget meant for the
// branching diffuse paths). Each step is one closest-hit query; a straight
// line crosses at most nx + ny + nz cell boundaries of the grid. With periodic
// boundaries it crosses copies until it climbs above the grid top, which takes
// a ray parameter of height / d.y. The budget doubles that count (coincident
// faces of neighbouring voxels) and adds a margin; it never falls below the
// configured step count, so rays that ended within radiationMaxSteps are traced
// exactly as before. Reaching this budget is a geometric anomaly and is counted
// in the VoxelEB solar diagnostics.
//
// Requires traceray_comp.glsl (PeriodicSceneTop, periodicCrossingLimit).

// A solar ray stops once its transmittance is at or below this value and keeps
// it: the visibility is then too high by at most the cutoff, so the direct
// irradiance error of the voxel is <= SOLAR_TRANSMITTANCE_CUTOFF * E_beam.
// 模块：透过率截断阈值（无量纲）。
const float SOLAR_TRANSMITTANCE_CUTOFF = 1.0e-4;
// 作用：返回太阳射线的最大步数。输入：射线方向 direction、配置的步数 configuredSteps；
// 输出：max(configuredSteps, 估计值)，估计值上限 1e6。
// 步骤：1) 非周期时，直线最多穿越 nx+ny+nz 个单元边界；
//   2) 周期边界时，射线需爬升到网格顶部之上，射线参数约为 height/d.y，水平穿越数 ≈ 该值 × (d.x+d.z)，
//   d.y 下限取 0.01 防止水平射线除零；3) 预算取 2 倍（相邻体元共面）再加 16 的余量。
int SolarStepLimit(vec3 direction, int configuredSteps)
{
  vec3 cells = max(vec3(setting.voxelSize), vec3(1.0));
  vec3 d = abs(normalize(direction));
  float crossings = cells.x + cells.y + cells.z;
  if(periodicCrossingLimit(setting.periodicNeighborCount) > 0)
  {
    float top = PeriodicSceneTop();
    float height = (top < 1.0e20 ? max(top, cells.y) : cells.y) + 2.0;
    crossings = height + height / max(d.y, 0.01) * (d.x + d.z);
  }
  float limit = min(2.0 * crossings + 16.0, 1.0e6);
  return max(configuredSteps, int(limit));
}

#endif
