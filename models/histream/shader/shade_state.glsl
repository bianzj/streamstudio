#ifndef SHADE_STATE_GLSL
#define SHADE_STATE_GLSL

#include "settings.h"
#include "compress.glsl"
#include "parameters.h"
#include "bindinglayout.h"

//---------------------------------------------------------------------
// facet version
//-----------------------------------------------------------------------
ShadeState GetShadeState(in PtPayload hstate)
{
  ShadeState sstate;

  const uint instanceId  = hstate.instanceCustomIndex;  // Geometry of this instance
  const uint primitiveId = hstate.primitiveID;          // Triangle ID
  const vec3 bary   = vec3(1.0 - hstate.baryCoord.x - hstate.baryCoord.y, hstate.baryCoord.x, hstate.baryCoord.y);

  int meshId = instanceLinks[instanceId].meshId;
  // Primitive buffer addresses
  Indices  indices  = Indices(meshLinks[meshId].indexAddress);
  Vertices vertices = Vertices(meshLinks[meshId].vertexAddress);

  // Indices of this triangle primitive.
  uvec3 tri = indices.i[primitiveId];

  // All vertex attributes of the triangle.
  VertexAttribute attr0 = vertices.v[tri.x];
  VertexAttribute attr1 = vertices.v[tri.y];
  VertexAttribute attr2 = vertices.v[tri.z];

  // Getting the material index on this geometry

  // Vertex of the triangle
  const vec3 pos0           = attr0.position.xyz;
  const vec3 pos1           = attr1.position.xyz;
  const vec3 pos2           = attr2.position.xyz;
  const vec3 position       = pos0 * bary.x + pos1 * bary.y + pos2 * bary.z;
  const vec3 world_position = vec3(hstate.objectToWorld * vec4(position, 1.0));

  // Normal
  vec3 geom_normal  = normalize(cross(pos1 - pos0, pos2 - pos0));
  // Normals transform by the inverse transpose (rotated/nonuniformly scaled instances).
  vec3 wgeom_normal = normalize(transpose(mat3(hstate.worldToObject)) * geom_normal);

  vec3 geom_tangent, geom_binormal;
  createCoordinateSystem(geom_normal, geom_tangent, geom_binormal);
  vec3 wgeom_tangent, wgeom_binormal;
  createCoordinateSystem(wgeom_normal, wgeom_tangent, wgeom_binormal);
  // closestHit stores the object-space ray origin.
  vec3 world_lastPosition = vec3(hstate.objectToWorld * vec4(hstate.lastPosition, 1.0));

  sstate.position       = world_position;
  sstate.lastPosition   = world_lastPosition;
  sstate.normal         = wgeom_normal;
  sstate.tangent        = wgeom_tangent;
  sstate.binormal       = wgeom_binormal;
  sstate.instanceId     = instanceId;
  // sstate.spectralId     = instanceLinks[instanceId].spectralId;
  // sstate.thermalId      = instanceLinks[instanceId].thermalId;
  // sstate.typeId         = instanceLinks[instanceId].typeId; 
  sstate.spectralId     = meshLinks[meshId].spectralId;
  sstate.thermalId      = meshLinks[meshId].thermalId;
  sstate.typeId         = meshLinks[meshId].type; 
  sstate.localNormal  = geom_normal;
  sstate.localTangent = geom_tangent;
  sstate.localBinormal = geom_binormal;


  return sstate;
}

#endif  // SHADE_STATE_GLSL