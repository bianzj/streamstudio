// ==========================================================================
// 文件：shade_state_voxel.glsl
// 功能：体元模式（VoxelRT/VoxelEB 等）下，把三角网格光线命中的 PtPayload 还原成 ShadeState。
// 主要步骤：
//   1. 由实例 → 网格链接取顶点/索引缓冲区，读取命中三角形三个顶点
//   2. 重心坐标插值位置并变到世界系；几何法线用 worldToObject 逆转置变换
//   3. 切线/副法线在物体系构造后用 objectToWorld 变换（与 shade_state.glsl 的差别；仅在实例均匀缩放时保持正交）
//   4. 填入 spectralId/thermalId/typeId（取自 meshLinks）
// 与其他文件的关系：面元模式的对应版本是 shade_state.glsl，两者导出同名函数 GetShadeState，
//   同一着色器只会包含其中之一；文件末尾被注释掉的 GetShadeState0 是按体元编号取状态的旧草稿。
// 约定：使用几何（面）法线；世界系位置单位 m。
// ==========================================================================

#ifndef SHADE_STATE_VOXEL_GLSL
#define SHADE_STATE_VOXEL_GLSL

#include "settings.h"
#include "compress.glsl"
#include "parameters.h"
#include "bindinglayout.h"

//---------------------------------------------------------------------
//-----------------------------------------------------------------------
// 作用：由命中负载 hstate 构造着色状态（世界系位置/法线/切线 + 局部系基 + 材料 id）。
ShadeState GetShadeState(in PtPayload hstate)
{
  ShadeState sstate;

  const uint instanceId  = hstate.instanceCustomIndex;  // Geometry of this instance
  const uint primitiveId = hstate.primitiveID;          // Triangle ID
  const vec3 bary   = vec3(1.0 - hstate.baryCoord.x - hstate.baryCoord.y, hstate.baryCoord.x, hstate.baryCoord.y);


  // 步骤 1：实例 → 网格 → 顶点/索引缓冲区（设备地址），取命中三角形的三个顶点。
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

  // Vertex of the triangle
  // 步骤 2：重心坐标插值物体系位置，并经 objectToWorld 变到世界系。
  const vec3 pos0           = attr0.position.xyz;
  const vec3 pos1           = attr1.position.xyz;
  const vec3 pos2           = attr2.position.xyz;
  const vec3 position       = pos0 * bary.x + pos1 * bary.y + pos2 * bary.z;
  const vec3 world_position = vec3(hstate.objectToWorld * vec4(position, 1.0));

  // Normal
  // 步骤 3：几何法线 + 切线坐标系。法线用逆转置变换（支持旋转/非均匀缩放实例）；
  // 切线、副法线直接用 objectToWorld 变换。
  // 说明（2026-11-25 核实）：本工程的实例变换由 scene.cpp 用“标量缩放 + 旋转 + 平移”构造
  //   （scale0 → glm::vec3(scale0)，背景缩放恒为 1），只有均匀缩放，此时 objectToWorld 的线性部分是
  //   旋转×常数，切线/副法线/法线保持正交（面元版 shade_state.glsl 用世界法线重新构造切线，
  //   切线的具体朝向与此处不同，但同样正交）。
  //   若今后允许非均匀缩放实例，三者可能不再正交，需改为用 wgeom_normal 重新调用
  //   createCoordinateSystem 构造世界系切线/副法线（localTangent/localBinormal 仍保持物体系）。
  vec3 geom_normal  = normalize(cross(pos1 - pos0, pos2 - pos0));
  // Normals transform by inverse transpose, including rotated/nonuniformly
  // scaled instances. Multiplication by objectToWorld as a row was incorrect.
  vec3 wgeom_normal = normalize(transpose(mat3(hstate.worldToObject)) * geom_normal);

  //vec3 wgeom_tangent, wgeom_binormal;
  vec3 geom_tangent, geom_binormal;
  createCoordinateSystem(geom_normal, geom_tangent, geom_binormal);
  // vec3 wgeom_normal = normalize(vec3(mat4(hstate.objectToWorld) * vec4(geom_normal.xyz, 0)));
  vec3 wgeom_tangent  = normalize(vec3(mat4(hstate.objectToWorld) * vec4(geom_tangent.xyz, 0)));
  vec3 wgeom_binormal = normalize(vec3(mat4(hstate.objectToWorld) * vec4(geom_binormal.xyz, 0)));
  vec3 world_lastPosition = vec3(hstate.objectToWorld * vec4(hstate.lastPosition, 1.0));

  // 步骤 4：填充输出；材料类 id 取自网格链接。
  sstate.position       = world_position;
  sstate.lastPosition   = world_lastPosition;
  sstate.normal         = wgeom_normal;
  sstate.tangent        = wgeom_tangent;
  sstate.binormal       = wgeom_binormal;
  sstate.instanceId     = instanceId;
  
  sstate.spectralId     = meshLinks[meshId].spectralId;
  sstate.thermalId      = meshLinks[meshId].thermalId;
  sstate.typeId         = meshLinks[meshId].type; 

  sstate.localNormal  = geom_normal;
  sstate.localTangent = geom_tangent;
  sstate.localBinormal = geom_binormal;

  return sstate;
}


// 以下为已停用的旧草稿（按体元编号取 ShadeState，引用的 primitiveId/bary/hstate 未定义，不可直接启用）。
// ShadeState GetShadeState0(uint voxelId)
// {
//   ShadeState sstate;

//   uint instanceId = voxelLinks[voxelId].instanceId;
//   int meshId = instanceLinks[instanceId].meshId;

//   // Primitive buffer addresses
//   Indices  indices  = Indices(meshLinks[meshId].indexAddress);
//   Vertices vertices = Vertices(meshLinks[meshId].vertexAddress);
//   // Indices of this triangle primitive.
//   uvec3 tri = indices.i[primitiveId];

//   // All vertex attributes of the triangle.
//   VertexAttribute attr0 = vertices.v[tri.x];
//   VertexAttribute attr1 = vertices.v[tri.y];
//   VertexAttribute attr2 = vertices.v[tri.z];

//   // Vertex of the triangle
//   const vec3 pos0           = attr0.position.xyz;
//   const vec3 pos1           = attr1.position.xyz;
//   const vec3 pos2           = attr2.position.xyz;
//   const vec3 position       = pos0 * bary.x + pos1 * bary.y + pos2 * bary.z;
//   const vec3 world_position = vec3(hstate.objectToWorld * vec4(position, 1.0));

//   // Normal
//   vec3 geom_normal  = normalize(cross(pos1 - pos0, pos2 - pos0));
//   vec3 wgeom_normal = normalize(vec3(geom_normal * hstate.objectToWorld));

//   //vec3 wgeom_tangent, wgeom_binormal;
//   vec3 geom_tangent, geom_binormal;
//   createCoordinateSystem(geom_normal, geom_tangent, geom_binormal);
//   // vec3 wgeom_normal = normalize(vec3(mat4(hstate.objectToWorld) * vec4(geom_normal.xyz, 0)));
//   vec3 wgeom_tangent  = normalize(vec3(mat4(hstate.objectToWorld) * vec4(geom_tangent.xyz, 0)));
//   vec3 wgeom_binormal = normalize(vec3(mat4(hstate.objectToWorld) * vec4(geom_binormal.xyz, 0)));
//   vec3 world_lastPosition = vec3(hstate.objectToWorld * vec4(hstate.lastPosition, 1.0));

//   sstate.position       = world_position;
//   sstate.lastPosition   = world_lastPosition;
//   sstate.normal         = wgeom_normal;
//   sstate.tangent        = wgeom_tangent;
//   sstate.binormal       = wgeom_binormal;
//   sstate.instanceId     = instanceId;
  
//   sstate.spectralId     = meshLinks[meshId].spectralId;
//   sstate.thermalId      = meshLinks[meshId].thermalId;
//   sstate.typeId         = meshLinks[meshId].type; 

//   sstate.localNormal  = geom_normal;
//   sstate.localTangent = geom_tangent;
//   sstate.localBinormal = geom_binormal;

//   return sstate;
// }

#endif  // SHADE_STATE_GLSL
