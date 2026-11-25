// ==========================================================================
// 文件：periodic_walk.glsl
// 功能：周期边界（沿世界 x、z 方向重复的矩形计算域）下的射线遍历工具，
//   RayTracing 与体元模式（VoxelRT/VoxelEB）共用。
// 主要模块：
//   1. 特化常量：PERIODIC_BOUNDARY_ENABLED、PERIODIC_CROSSING_LIMIT 与 periodicCrossingLimit()
//   2. PeriodicWalk 结构体：记录当前所在副本（tile）与下一次穿越边界的射线参数
//   3. periodicWalkBegin / SegmentEnd / Shift / Advance：类似 2D DDA 的逐副本步进
//   4. periodicTopEntry / periodicEscapedAbove：场景顶面之上空区的快速跳过与提前终止
// 数据流：调用方（traceray/offline.rgen 等）在每个副本内把射线原点平移 periodicWalkShift()，
//   只在射线参数区间 [segmentStart, segmentEnd] 内查询，命中位置仍用中央域坐标。
// 约定：x/z 为水平轴、y 向上；射线参数 t 沿 direction 计（长度单位 m，方向向量为单位向量）；
//   tile=(0,0) 为中央域；穿越次数上限由 setting.periodicNeighborCount 给出（最大 20）。
// ==========================================================================

#ifndef PERIODIC_WALK_GLSL
#define PERIODIC_WALK_GLSL


// Periodic boundary traversal shared by RayTracing and the voxel modes.
//
// The scene is one rectangular domain repeated along world x and z. A ray
// keeps its origin; the copies it crosses are visited in ray order, and each
// copy is queried by translating the ray origin by whole periods over exactly
// the ray-parameter interval [segmentStart, segmentEnd] spent inside that
// copy. Hit distances, hit positions (always in central-domain coordinates)
// and the ray origin seen by the hit shader therefore stay those of one
// straight ray, and a nearer hit in an earlier copy can never be replaced by
// a later one. "crossings" counts boundary passages; callers stop at
// periodicTraversalCount (setting.periodicNeighborCount).

// 表示"永远不会穿越该边界"（射线与该轴平行）的大数。
const float PERIODIC_WALK_HUGE = 1.0e30;

// Specialization constant 0: false when the run has no periodic boundaries
// (periodicTraversalCount = 0). The VoxelRT/VoxelEB hosts set it per run, so
// the driver removes every periodic branch and the non-periodic pipelines
// compile to the original single-query / single-box code (a runtime-uniform
// branch alone left the scattering pass about 30% slower). Pipelines that do
// not specialize it keep the default true and decide from
// setting.periodicNeighborCount at run time.
layout(constant_id = 0) const bool PERIODIC_BOUNDARY_ENABLED = true;

// Specialization constant 1: the run's crossing limit when the host knows it
// (VoxelRT/VoxelEB set clamp(periodicTraversalCount, 0, 20)); -1 keeps the
// limit a run-time value (pipelines that do not specialize it). A known limit
// lets the driver fold every "periodic or not" decision of a call site
// instead of compiling both alternatives into the periodic pipelines.
layout(constant_id = 1) const int PERIODIC_CROSSING_LIMIT = -1;

// Crossings allowed per query: 0 disables periodic traversal.
// 作用：返回本次查询允许的穿越次数。特化常量已知时直接取常量；
// 否则在周期启用时把 requested 限幅到 [0, 20]，未启用则为 0。
int periodicCrossingLimit(int requested)
{
  if(PERIODIC_CROSSING_LIMIT >= 0) return PERIODIC_CROSSING_LIMIT;
  return PERIODIC_BOUNDARY_ENABLED ? clamp(requested, 0, 20) : 0;
}

// 遍历状态：nextCrossing / crossingStep 分别为 x、z 方向下一次穿越边界的射线参数及相邻边界间距。
struct PeriodicWalk
{
  vec2 low;            // world x/z of the domain minimum corner
  vec2 size;           // period along world x and z
  ivec2 tile;          // copy being traversed, (0,0) = central domain
  ivec2 stepDirection;
  vec2 nextCrossing;   // ray parameter of the next x / z boundary
  vec2 crossingStep;   // ray-parameter distance between boundaries
  float segmentStart;  // ray parameter at which the current copy is entered
};

// 作用：在射线参数 tStart 处初始化遍历。
// 步骤：1) 求 tStart 处位置落在哪个副本 tile；2) 按方向符号确定 x/z 的前进方向（|d|<1e-8 视为平行）；
//   3) 计算到下一条 x/z 边界的射线参数 nextCrossing 与边界间隔 crossingStep（平行轴取 HUGE）。
// size 下限取 1e-4 避免除零。
PeriodicWalk periodicWalkBegin(vec3 origin, vec3 direction, float tStart,
                               vec2 low, vec2 size)
{
  const float directionEpsilon = 1.0e-8;
  PeriodicWalk w;
  w.low = low;
  w.size = max(size, vec2(1.0e-4));
  vec2 start = vec2(origin.x, origin.z) + vec2(direction.x, direction.z) * tStart;
  vec2 position = (start - low) / w.size;
  w.tile = ivec2(floor(position));
  vec2 dir = vec2(direction.x, direction.z);
  w.stepDirection = ivec2(
    dir.x > directionEpsilon ? 1 : (dir.x < -directionEpsilon ? -1 : 0),
    dir.y > directionEpsilon ? 1 : (dir.y < -directionEpsilon ? -1 : 0));
  vec2 ray = dir / w.size;
  vec2 nextBoundary = vec2(
    w.stepDirection.x > 0 ? float(w.tile.x + 1) : float(w.tile.x),
    w.stepDirection.y > 0 ? float(w.tile.y + 1) : float(w.tile.y));
  w.nextCrossing = vec2(
    w.stepDirection.x == 0 ? PERIODIC_WALK_HUGE
      : tStart + max((nextBoundary.x - position.x) / ray.x, 0.0),
    w.stepDirection.y == 0 ? PERIODIC_WALK_HUGE
      : tStart + max((nextBoundary.y - position.y) / ray.y, 0.0));
  w.crossingStep = vec2(
    w.stepDirection.x == 0 ? PERIODIC_WALK_HUGE : abs(1.0 / ray.x),
    w.stepDirection.y == 0 ? PERIODIC_WALK_HUGE : abs(1.0 / ray.y));
  w.segmentStart = tStart;
  return w;
}

// End of the interval spent in the current copy (PERIODIC_WALK_HUGE when the
// ray never leaves it, e.g. a vertical ray).
float periodicWalkSegmentEnd(PeriodicWalk w)
{
  return min(w.nextCrossing.x, w.nextCrossing.y);
}

// Translation that maps the current copy onto the central domain.
vec3 periodicWalkShift(PeriodicWalk w)
{
  return vec3(float(w.tile.x) * w.size.x, 0.0, float(w.tile.y) * w.size.y);
}

// Enter the next copy. A corner passage (both boundaries within tolerance)
// is one crossing into the diagonal neighbour.
void periodicWalkAdvance(inout PeriodicWalk w)
{
  float segmentEnd = periodicWalkSegmentEnd(w);
  float tolerance = 1.0e-5 * max(1.0, segmentEnd);
  bool crossX = w.nextCrossing.x - segmentEnd <= tolerance;
  bool crossZ = w.nextCrossing.y - segmentEnd <= tolerance;
  if(crossX)
  {
    w.tile.x += w.stepDirection.x;
    w.nextCrossing.x += w.crossingStep.x;
  }
  if(crossZ)
  {
    w.tile.y += w.stepDirection.y;
    w.nextCrossing.y += w.crossingStep.y;
  }
  w.segmentStart = segmentEnd;
}

// Ray parameter at which a ray that starts above sceneTop and descends
// reaches it. Nothing lies above the top, so a periodic walk may start there
// without using crossings in empty space (high or distant cameras).
float periodicTopEntry(vec3 origin, vec3 direction, float sceneTop)
{
  if(origin.y <= sceneTop || direction.y >= 0.0) return 0.0;
  return (origin.y - sceneTop) / (-direction.y);
}

// True when the ray at parameter t is above sceneTop and not descending:
// no copy can be hit any more.
bool periodicEscapedAbove(vec3 origin, vec3 direction, float t, float sceneTop)
{
  return direction.y >= 0.0 && origin.y + direction.y * t > sceneTop;
}

#endif
