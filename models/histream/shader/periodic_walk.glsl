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

const float PERIODIC_WALK_HUGE = 1.0e30;

// Specialization constant 0: false when the run has no periodic boundaries
// (periodicTraversalCount = 0). The VoxelRT/VoxelEB hosts set it per run, so
// the driver removes every periodic branch and the non-periodic pipelines
// compile to the original single-query / single-box code (a runtime-uniform
// branch alone left the scattering pass about 30% slower). Pipelines that do
// not specialize it keep the default true and decide from
// setting.periodicNeighborCount at run time.
layout(constant_id = 0) const bool PERIODIC_BOUNDARY_ENABLED = true;

// Crossings allowed per query: 0 disables periodic traversal.
int periodicCrossingLimit(int requested)
{
  return PERIODIC_BOUNDARY_ENABLED ? clamp(requested, 0, 20) : 0;
}

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
