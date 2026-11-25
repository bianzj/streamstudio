// ==========================================================================
// 文件：shade_state.glsl
// 功能：把面元模式（三角网格）光线命中的 PtPayload 还原成 ShadeState（世界系位置与几何法线、
//   局部系法线/切线、实例与材料 id）。
// 主要步骤：
//   1. 由实例 → 网格链接取顶点/索引缓冲区地址，读取命中三角形三个顶点
//   2. 用重心坐标插值位置，并经 objectToWorld 变到世界系
//   3. 由三角形几何计算法线，法线用 worldToObject 的逆转置变换到世界系，建立切线坐标系
//   4. 填入 spectralId/thermalId/typeId（取自 meshLinks）
// 与其他文件的关系：settings.h 定义 PtPayload/ShadeState；bindinglayout.h 提供 instanceLinks、
//   meshLinks 及顶点缓冲区引用类型；shade_state_voxel.glsl 是体元模式的对应版本。
// 约定：法线为几何（面）法线，不使用顶点法线插值；重心坐标顺序 (1-u-v, u, v)。
// ==========================================================================

#ifndef SHADE_STATE_GLSL
#define SHADE_STATE_GLSL

#include "settings.h"
#include "compress.glsl"
#include "parameters.h"
#include "bindinglayout.h"

//---------------------------------------------------------------------
// facet version
//-----------------------------------------------------------------------
// 作用：由命中负载 hstate 构造着色状态。
// 输入：命中的实例/图元 id、重心坐标、变换矩阵；输出：世界系 ShadeState。
ShadeState GetShadeState(in PtPayload hstate)
{
  ShadeState sstate;

  const uint instanceId  = hstate.instanceCustomIndex;  // Geometry of this instance
  const uint primitiveId = hstate.primitiveID;          // Triangle ID
  const vec3 bary   = vec3(1.0 - hstate.baryCoord.x - hstate.baryCoord.y, hstate.baryCoord.x, hstate.baryCoord.y);

  // 步骤 1：实例 → 网格链接 → 顶点/索引缓冲区（设备地址），取出命中三角形三个顶点。
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
  // 步骤 2：用重心坐标插值物体系位置，并经 objectToWorld 变到世界系。
  const vec3 pos0           = attr0.position.xyz;
  const vec3 pos1           = attr1.position.xyz;
  const vec3 pos2           = attr2.position.xyz;
  const vec3 position       = pos0 * bary.x + pos1 * bary.y + pos2 * bary.z;
  const vec3 world_position = vec3(hstate.objectToWorld * vec4(position, 1.0));

  // Normal
  // 步骤 3：几何法线由三角形边叉乘得到（朝向取决于顶点绕序）。
  vec3 geom_normal  = normalize(cross(pos1 - pos0, pos2 - pos0));
  // Normals transform by the inverse transpose (rotated/nonuniformly scaled instances).
  vec3 wgeom_normal = normalize(transpose(mat3(hstate.worldToObject)) * geom_normal);

  vec3 geom_tangent, geom_binormal;
  createCoordinateSystem(geom_normal, geom_tangent, geom_binormal);
  vec3 wgeom_tangent, wgeom_binormal;
  createCoordinateSystem(wgeom_normal, wgeom_tangent, wgeom_binormal);
  // closestHit stores the object-space ray origin.
  vec3 world_lastPosition = vec3(hstate.objectToWorld * vec4(hstate.lastPosition, 1.0));

  // 步骤 4：填充输出；材料类 id 取自网格链接（旧的按实例取值已注释掉）。
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