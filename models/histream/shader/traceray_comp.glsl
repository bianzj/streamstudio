#ifndef TRACERAY_COMP
#define TRACERAY_COMP

#include "bindingLayout.h"
#include "functions.glsl"
#include "periodic_walk.glsl"

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
vec2 PeriodicDomainLow()
{
  return -vec2(floor(setting.voxelSize.x / 2.0 + 0.5),
               floor(setting.voxelSize.z / 2.0 + 0.5));
}

vec2 PeriodicDomainSize()
{
  return max(vec2(setting.voxelSize.x, setting.voxelSize.z), vec2(1.0));
}

// Top of the voxel grid (cell units, + one cell margin). Every interaction
// needs a grid cell (nanovdbGet), so nothing above it can affect a ray.
float PeriodicSceneTop()
{
#ifdef TURBID_GRID_AVAILABLE
  if(turbidGridReady) return TurbidGridTop() + 1.0;
#endif
  return PERIODIC_WALK_HUGE;
}

const int PERIODIC_QUERY_CENTRAL = 0;    // original scene only
const int PERIODIC_QUERY_TRANSPORT = 1;  // periodic copies, ray order
const int PERIODIC_QUERY_IMAGE = 2;      // as transport, plus ground fill

bool QueryClosestPeriodic(Ray r, float minLength, uint rayFlags,
                          int periodicMode,
                          out PtPayload result)
{
  result = prd;
  result.hitT = INFINITY;
  int count = periodicMode != PERIODIC_QUERY_CENTRAL
    ? clamp(setting.periodicNeighborCount, 0, 20) : 0;
  bool walking = count > 0;

  // Every ray, from a voxel or from the camera, visits the domain copies in
  // ray order with a fixed origin (periodic_walk.glsl): hitT, the hit shader
  // origin (lastPosition, used for turbid path lengths) and the hit position
  // (central-domain coordinates) are those of one straight ray. Rays above
  // the grid start at its top; rays above it that do not descend escape.
  // Without periodic boundaries the same loop issues the single unrestricted
  // query (zero shift, [minLength, INFINITY]); one query site keeps the
  // shaders as fast as before.
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

void ClosestHit(Ray r)
{
  PtPayload result;
  QueryClosestPeriodic(r, 0.0, gl_RayFlagsNoneEXT, PERIODIC_QUERY_CENTRAL, result);
  prd = result;
}

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
void ClosestHitT(Ray r)
{
  PtPayload result;
  QueryClosestPeriodic(r, 0.0, gl_RayFlagsNoneEXT, PERIODIC_QUERY_CENTRAL, result);
  prd = result;
}

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
