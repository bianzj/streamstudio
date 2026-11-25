// ==========================================================================
// 文件：traceray_comp.glsl
// 功能：计算着色器（体元模式）里基于 rayQuery 的光线求交封装：最近命中、周期边界遍历、阴影（任意命中）。
// 主要模块：
//   1. StoreCommittedHit / QueryClosestSegment：在射线参数区间 [tMin,tMax] 内查询最近命中
//   2. PeriodicDomain*/PeriodicSceneTop：周期域的位置、大小与场景顶面（体元格单位）
//   3. QueryClosestPeriodic：按射线顺序遍历周期副本，并在传感器图像模式下做地面补全
//   4. ClosestHit / ClosestHitT / ClosestHitPeriodic / ClosestHit(r,minLength)：对外入口，
//      结果写入全局负载 prd（PtPayload）
//   5. AnyHit：阴影射线，只判断是否有任意命中
// 数据流：调用方构造 Ray → 本文件对 topLevelAS 做 rayQuery → 命中信息写入 prd，
//   shade_state*.glsl 再据此还原 ShadeState。
// 与其他文件的关系：periodic_walk.glsl 提供副本遍历；solar_steps.glsl 依赖 PeriodicSceneTop；
//   依赖 bindingLayout.h（topLevelAS、setting、prd）与 functions.glsl（INFINITY 等）。
// 约定：体元场景以"格"为长度单位（不是米），周期域大小为 voxelSize.xz 个格；射线参数 t 以格计。
// ==========================================================================

#ifndef TRACERAY_COMP
#define TRACERAY_COMP

#include "bindingLayout.h"
#include "functions.glsl"
#include "periodic_walk.glsl"

// 模块：命中信息拷贝与区间查询。
// Committed intersection of rayQuery copied into result.
void StoreCommittedHit(rayQueryEXT rayQuery, inout PtPayload result)
{
  result.hitT = rayQueryGetIntersectionTEXT(rayQuery, true);
  result.primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true);
  result.instanceID = rayQueryGetIntersectionInstanceIdEXT(rayQuery, true);
  result.instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true);
  result.baryCoord = rayQueryGetIntersectionBarycentricsEXT(rayQuery, true);
  result.lastPosition = rayQueryGetIntersectionObjectRayOriginEXT(rayQuery, true);
  result.objectToWorld = rayQueryGetIntersectionObjectToWorldEXT(rayQuery, true);
  result.worldToObject = rayQueryGetIntersectionWorldToObjectEXT(rayQuery, true);
}

// 作用：在射线参数区间 [tMin, tMax] 内求最近的三角形命中，命中则写入 result 并返回 true。
// 说明：遍历中逐个确认候选三角形交点，由 rayQuery 保留最近的已确认交点。
bool QueryClosestSegment(vec3 origin, vec3 direction, float tMin, float tMax,
                         uint rayFlags, inout PtPayload result)
{
  rayQueryEXT rayQuery;
  rayQueryInitializeEXT(rayQuery, topLevelAS, rayFlags, 0xFF,
                        origin, tMin, direction, tMax);
  while(rayQueryProceedEXT(rayQuery))
  {
    if(rayQueryGetIntersectionTypeEXT(rayQuery, false) ==
       gl_RayQueryCandidateIntersectionTriangleEXT)
      rayQueryConfirmIntersectionEXT(rayQuery);
  }
  if(rayQueryGetIntersectionTypeEXT(rayQuery, true) ==
     gl_RayQueryCommittedIntersectionNoneEXT) return false;
  StoreCommittedHit(rayQuery, result);
  return true;
}

// Voxel scenes are traced in cell units; the domain spans
// x in [-semi.x, voxelSize.x - semi.x] and z likewise (scene.cpp shifts every
// instance by -semiRange), so the period is voxelSize.xz cells (not metres).
// 周期域最小角（x/z）：中央域覆盖 [-semi, voxelSize-semi]，semi 为 voxelSize/2 四舍五入。
vec2 PeriodicDomainLow()
{
  return -vec2(floor(setting.voxelSize.x / 2.0 + 0.5),
               floor(setting.voxelSize.z / 2.0 + 0.5));
}

// 周期域在 x、z 方向的周期（格），下限 1 防止除零。
vec2 PeriodicDomainSize()
{
  return max(vec2(setting.voxelSize.x, setting.voxelSize.z), vec2(1.0));
}

// Top of the voxel grid (cell units, + one cell margin). Every interaction
// needs a grid cell (nanovdbGet), so nothing above it can affect a ray.
// 场景顶面高度（格）；无湍流网格时返回 HUGE，表示不启用"顶面之上提前终止"。
float PeriodicSceneTop()
{
#ifdef TURBID_GRID_AVAILABLE
  if(turbidGridReady) return TurbidGridTop() + 1.0;
#endif
  return PERIODIC_WALK_HUGE;
}

// 周期查询模式：CENTRAL 只查中央域；TRANSPORT 按射线顺序查各副本；IMAGE 在 TRANSPORT 基础上补地面。
const int PERIODIC_QUERY_CENTRAL = 0;    // original scene only
const int PERIODIC_QUERY_TRANSPORT = 1;  // periodic copies, ray order
const int PERIODIC_QUERY_IMAGE = 2;      // as transport, plus ground fill

// 作用：周期边界下的最近命中查询。输入：射线 r、最小起始距离 minLength、rayFlags、模式；
// 输出：result（命中信息，未命中时 hitT=INFINITY）；返回是否命中。
// 步骤：1) 确定穿越次数 count（CENTRAL 为 0）；2) 若射线在场景顶面之上且不下降，直接判定逃逸；
//   3) 否则用 periodicWalk 逐副本查询，每个副本只查 [segmentStart, segmentEnd]，首个命中即最近；
//   4) 传感器图像（IMAGE）模式下，下降射线用完穿越次数仍无命中时，在落地点所在副本补查地面。
bool QueryClosestPeriodic(Ray r, float minLength, uint rayFlags,
                          int periodicMode,
                          out PtPayload result)
{
  result = prd;
  result.hitT = INFINITY;
  int count = periodicMode != PERIODIC_QUERY_CENTRAL
    ? periodicCrossingLimit(setting.periodicNeighborCount) : 0;
  bool walking = count > 0;

  // Every ray, from a voxel or from the camera, visits the domain copies in
  // ray order with a fixed origin (periodic_walk.glsl): hitT, the hit shader
  // origin (lastPosition, used for turbid path lengths) and the hit position
  // (central-domain coordinates) are those of one straight ray. Rays above
  // the grid start at its top; rays above it that do not descend escape.
  // Without periodic boundaries the same loop issues the single unrestricted
  // query (zero shift, [minLength, INFINITY]); one query site keeps the
  // shaders as fast as before.
  // 步骤 2：非周期遍历时不使用顶面判定。
  float sceneTop = walking ? PeriodicSceneTop() : PERIODIC_WALK_HUGE;
  bool found = false;
  if(!walking || !periodicEscapedAbove(r.origin, r.direction, 0.0, sceneTop))
  {
    PeriodicWalk walk = periodicWalkBegin(r.origin, r.direction,
      walking ? max(minLength, periodicTopEntry(r.origin, r.direction, sceneTop)) : minLength,
      PeriodicDomainLow(), PeriodicDomainSize());
    for(int crossing = 0; ; ++crossing)
    {
      float segmentEnd = walking ? periodicWalkSegmentEnd(walk) : PERIODIC_WALK_HUGE;
      float segmentStart = walking ? max(minLength, walk.segmentStart) : minLength;
      vec3 shift = walking ? periodicWalkShift(walk) : vec3(0.0);
      if(segmentEnd > segmentStart
         && QueryClosestSegment(r.origin - shift, r.direction,
              segmentStart,
              segmentEnd >= PERIODIC_WALK_HUGE ? INFINITY : segmentEnd,
              rayFlags, result))
        return true;
      if(segmentEnd >= PERIODIC_WALK_HUGE || crossing >= count
         || periodicEscapedAbove(r.origin, r.direction, segmentEnd, sceneTop))
        break;
      periodicWalkAdvance(walk);
    }
  }
  if(walking && !found && periodicMode == PERIODIC_QUERY_IMAGE && r.direction.y < -1.0e-6)
  {
    // 步骤 4 的地面补查：以射线与 y=0 地面的交点为中心，沿射线取 ±rayMargin（垂直容差 verticalMargin，
    //   至少 1 格或周期的 2%）的小区间，把原点平移到落地点所在副本后查询。
    // Sensor images: a descending ray that used up its crossings still sees
    // the ground of the copy it lands in instead of a hole.
    float groundT = -r.origin.y / r.direction.y;
    if(groundT > minLength)
    {
      vec2 size = PeriodicDomainSize();
      vec3 groundPoint = r.origin + r.direction * groundT;
      ivec2 tile = ivec2(floor(
        (vec2(groundPoint.x, groundPoint.z) - PeriodicDomainLow()) / size));
      vec3 queryOrigin = r.origin - vec3(
        float(tile.x) * size.x, 0.0, float(tile.y) * size.y);
      float verticalMargin = max(1.0, max(size.x, size.y) * 0.02);
      float rayMargin = verticalMargin / max(-r.direction.y, 1.0e-6);
      found = QueryClosestSegment(queryOrigin, r.direction,
                                  max(minLength, groundT - rayMargin),
                                  groundT + rayMargin, rayFlags, result);
    }
  }
  return found;
}

// 入口：仅中央域的最近命中，结果写入 prd。（与下方带 minLength 的重载同名。）
void ClosestHit(Ray r)
{
  PtPayload result;
  QueryClosestPeriodic(r, 0.0, gl_RayFlagsNoneEXT, PERIODIC_QUERY_CENTRAL, result);
  prd = result;
}

// 入口：传感器图像射线（IMAGE 模式，仅不透明几何）。
// Sensor-image rays: periodic copies in ray order plus the ground fill.
void ClosestHitPeriodic(Ray r, float minLength)
{
  PtPayload result;
  QueryClosestPeriodic(r, minLength,
                       gl_RayFlagsNoneEXT | gl_RayFlagsOpaqueEXT,
                       PERIODIC_QUERY_IMAGE, result);
  prd = result;
}

//-----------------------------------------------------------------------
// Shoot a ray an return the information of the closest hit, in the
// PtPayload structure (PRD)
//
// 入口：与 ClosestHit(Ray) 等价（仅中央域）。
void ClosestHitT(Ray r)
{
  PtPayload result;
  QueryClosestPeriodic(r, 0.0, gl_RayFlagsNoneEXT, PERIODIC_QUERY_CENTRAL, result);
  prd = result;
}

// 以下为已停用的旧版 ClosestHit（不经周期遍历，含 AABB 处理），保留作参考。
// //-----------------------------------------------------------------------
// // Shoot a ray an return the information of the closest hit, in the
// // PtPayload structure (PRD)
// //
// void ClosestHit(Ray r)
// {
//   uint rayFlags = gl_RayFlagsNoneEXT;//gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT;//
//   prd.hitT      = INFINITY;

//   // Initializes a ray query object but does not start traversal
//   rayQueryEXT rayQuery;
//   rayQueryInitializeEXT(rayQuery,     //
//                         topLevelAS,   // acceleration structure
//                         rayFlags,     // rayFlags
//                         0xFF,         // cullMask
//                         r.origin,     // ray origin
//                         0.001,          // ray min range
//                         r.direction,  // ray direction
//                         INFINITY);    // ray max range

//   int num = 0;
//   bool isFirst = true;
//   float hitT0 = -1;
//   float hitT = -1;
//   // Start traversal, and loop over all ray-scene intersections. When this finishes,
//   // rayQuery stores a "committed" intersection, the closest intersection (if any).
//   while(rayQueryProceedEXT(rayQuery))
//   {
//     if (rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
//     {
//         // Determine if an opaque triangle hit occurred
//         rayQueryConfirmIntersectionEXT(rayQuery);
//         break;
//     }
//     else if (rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionAABBEXT)
//     {
//         // // Get the t-value of the intersection (if there's no intersection, this will
//         // // be tMax = 10000.0). "true" says "get the committed intersection."
//         prd.instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false);
//         prd.primitiveID         = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false);
//         int loc = instanceLinks[prd.instanceCustomIndex].voxelIdOffset + prd.primitiveID;
//         AllAabb aabb = aabbs[loc];

//         hitT0 = hitAabb(aabb, r);
//         // if(hitT0 > 0)
//         // {
//         //   if(isFirst)
//         //   {
//         //     hitT = hitT0;
//         //     isFirst = false;
//         //     continue;
//         //   }
//         //   if(hitT0 < hitT)
//         //   {
//         //     hitT = hitT0;
//         //   }
//         // }     

//         // hitT = prd.primitiveID; 
//         hitT = hitT0;
//         rayQueryGenerateIntersectionEXT(rayQuery, hitT);
//         break;
//     }
//   }
  
//   // prd.hitT = num;
//   prd.hitT                = rayQueryGetIntersectionTEXT(rayQuery, true); 
//   // prd.hitT         = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true);
//   // // prd.instanceID          = rayQueryGetIntersectionInstanceIdEXT(rayQuery, true);
//   // prd.instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true);

//   // prd.lastPosition        = rayQueryGetIntersectionObjectRayOriginEXT(rayQuery,true);  
//   // prd.objectToWorld       = rayQueryGetIntersectionObjectToWorldEXT(rayQuery, true);
//   // prd.worldToObject       = rayQueryGetIntersectionWorldToObjectEXT(rayQuery, true);
// }


// Transport passes (solar gap, diffuse/scattering, TIR): with periodic
// boundaries the ray continues through the domain copies in ray order, at
// most periodicTraversalCount crossings per query (i.e. per free flight
// between two consecutive cell hits); otherwise the original scene only.
void ClosestHit(Ray r,float minLength)
{
  PtPayload result;
  QueryClosestPeriodic(r, minLength,
                       gl_RayFlagsNoneEXT | gl_RayFlagsOpaqueEXT,
                       PERIODIC_QUERY_TRANSPORT, result);
  prd = result;
}

//-----------------------------------------------------------------------
// Shadow ray - return true if a ray hits anything
//
// 输入：射线与最大距离 maxDist；输出：区间内是否有三角形命中（背面剔除，首次命中即终止）。
bool AnyHit(Ray r, float maxDist)
{
  uint rayFlags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT | gl_RayFlagsCullBackFacingTrianglesEXT;
  rayQueryEXT rayQuery;
  rayQueryInitializeEXT(rayQuery, topLevelAS, rayFlags, 0xFF,
                        r.origin, 0.0, r.direction, maxDist);
  while(rayQueryProceedEXT(rayQuery))
  {
    if(rayQueryGetIntersectionTypeEXT(rayQuery, false) ==
       gl_RayQueryCandidateIntersectionTriangleEXT)
      rayQueryConfirmIntersectionEXT(rayQuery);
  }
  return rayQueryGetIntersectionTypeEXT(rayQuery, true) !=
         gl_RayQueryCommittedIntersectionNoneEXT;
}




#endif
