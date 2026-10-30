#ifndef TRACERAY_COMP
#define TRACERAY_COMP

#include "bindingLayout.h"
#include "functions.glsl"

vec3 periodicQueryOrigin(vec3 origin, vec3 direction, int tileIndex,
                         out float segmentMinimum, out float segmentMaximum)
{
  if(setting.periodicNeighborCount <= 0)
  {
    segmentMinimum = 0.0;
    segmentMaximum = INFINITY;
    return origin;
  }
  vec2 size = max(
    vec2(setting.voxelSize.x, setting.voxelSize.z) * setting.scale,
    vec2(0.01));
  ivec2 tile = periodicRayTileSegment(
    origin, direction, tileIndex, size, segmentMinimum, segmentMaximum);
  return origin - vec3(float(tile.x) * size.x, 0.0,
                       float(tile.y) * size.y);
}

bool QueryClosestPeriodic(Ray r, float minLength, uint rayFlags,
                          bool usePeriodicFill,
                          out PtPayload result)
{
  result = prd;
  result.hitT = INFINITY;
  bool found = false;
  int count = usePeriodicFill
    ? clamp(setting.periodicNeighborCount, 0, 20) : 0;
  for(int tileIndex = 0; tileIndex <= count; ++tileIndex)
  {
    float segmentMinimum;
    float segmentMaximum;
    vec3 queryOrigin = r.origin;
    // Passage zero is the original unrestricted scene. Boundary wrapping is a
    // fallback only when that ray misses; every pass queries the same scene.
    if(usePeriodicFill && tileIndex > 0)
      queryOrigin = periodicQueryOrigin(
        r.origin, r.direction, tileIndex, segmentMinimum, segmentMaximum);
    else
    {
      segmentMinimum = 0.0;
      segmentMaximum = INFINITY;
    }
    float queryMinimum = max(minLength, segmentMinimum);
    if(!(segmentMaximum > queryMinimum)) continue;
    rayQueryEXT rayQuery;
    rayQueryInitializeEXT(rayQuery, topLevelAS, rayFlags, 0xFF,
                          queryOrigin, queryMinimum,
                          r.direction, segmentMaximum);
    while(rayQueryProceedEXT(rayQuery))
    {
      if(rayQueryGetIntersectionTypeEXT(rayQuery, false) ==
         gl_RayQueryCandidateIntersectionTriangleEXT)
        rayQueryConfirmIntersectionEXT(rayQuery);
    }
    if(rayQueryGetIntersectionTypeEXT(rayQuery, true) ==
       gl_RayQueryCommittedIntersectionNoneEXT) continue;
    float hitT = rayQueryGetIntersectionTEXT(rayQuery, true);
    if(hitT >= result.hitT) continue;
    found = true;
    result.hitT = hitT;
    result.primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true);
    result.instanceID = rayQueryGetIntersectionInstanceIdEXT(rayQuery, true);
    result.instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true);
    result.baryCoord = rayQueryGetIntersectionBarycentricsEXT(rayQuery, true);
    result.lastPosition = rayQueryGetIntersectionObjectRayOriginEXT(rayQuery, true);
    result.objectToWorld = rayQueryGetIntersectionObjectToWorldEXT(rayQuery, true);
    result.worldToObject = rayQueryGetIntersectionWorldToObjectEXT(rayQuery, true);
    if(tileIndex == 0) return true;
  }
  if(!found && usePeriodicFill && count > 0 && r.direction.y < -1.0e-6)
  {
    float groundT = -r.origin.y / r.direction.y;
    if(groundT > minLength)
    {
      vec2 size = max(
        vec2(setting.voxelSize.x, setting.voxelSize.z) * setting.scale,
        vec2(0.01));
      vec3 groundPoint = r.origin + r.direction * groundT;
      ivec2 tile = ivec2(floor(
        vec2(groundPoint.x, groundPoint.z) / size + vec2(0.5)));
      vec3 queryOrigin = r.origin - vec3(
        float(tile.x) * size.x, 0.0, float(tile.y) * size.y);
      float verticalMargin = max(1.0, max(size.x, size.y) * 0.02);
      float rayMargin = verticalMargin / max(-r.direction.y, 1.0e-6);
      rayQueryEXT rayQuery;
      rayQueryInitializeEXT(rayQuery, topLevelAS, rayFlags, 0xFF,
                            queryOrigin, max(minLength, groundT - rayMargin),
                            r.direction, groundT + rayMargin);
      while(rayQueryProceedEXT(rayQuery))
      {
        if(rayQueryGetIntersectionTypeEXT(rayQuery, false) ==
           gl_RayQueryCandidateIntersectionTriangleEXT)
          rayQueryConfirmIntersectionEXT(rayQuery);
      }
      if(rayQueryGetIntersectionTypeEXT(rayQuery, true) !=
         gl_RayQueryCommittedIntersectionNoneEXT)
      {
        found = true;
        result.hitT = rayQueryGetIntersectionTEXT(rayQuery, true);
        result.primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true);
        result.instanceID = rayQueryGetIntersectionInstanceIdEXT(rayQuery, true);
        result.instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true);
        result.baryCoord = rayQueryGetIntersectionBarycentricsEXT(rayQuery, true);
        result.lastPosition = rayQueryGetIntersectionObjectRayOriginEXT(rayQuery, true);
        result.objectToWorld = rayQueryGetIntersectionObjectToWorldEXT(rayQuery, true);
        result.worldToObject = rayQueryGetIntersectionWorldToObjectEXT(rayQuery, true);
      }
    }
  }
  return found;
}

void ClosestHit(Ray r)
{
  PtPayload result;
  QueryClosestPeriodic(r, 0.0, gl_RayFlagsNoneEXT, false, result);
  prd = result;
}

// Sensor-image rays may continue into periodic copies after leaving the
// central study area.  Other transport passes deliberately use ClosestHit().
void ClosestHitPeriodic(Ray r, float minLength)
{
  PtPayload result;
  QueryClosestPeriodic(r, minLength,
                       gl_RayFlagsNoneEXT | gl_RayFlagsOpaqueEXT,
                       true, result);
  prd = result;
}

//-----------------------------------------------------------------------
// Shoot a ray an return the information of the closest hit, in the
// PtPayload structure (PRD)
//
void ClosestHitT(Ray r)
{
  PtPayload result;
  QueryClosestPeriodic(r, 0.0, gl_RayFlagsNoneEXT, false, result);
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


void ClosestHit(Ray r,float minLength)
{
  PtPayload result;
  QueryClosestPeriodic(r, minLength,
                       gl_RayFlagsNoneEXT | gl_RayFlagsOpaqueEXT,
                       false, result);
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
