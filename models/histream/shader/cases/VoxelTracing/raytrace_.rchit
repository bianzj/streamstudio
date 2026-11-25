// ==========================================================================
// 文件：VoxelTracing/raytrace_.rchit
// 功能：VoxelTracing 旧方案的最近命中着色器（调试版）：命中后令 hitID.id = -70，并从命中点向正上方
//   （+y）再发一条射线，探测头顶是否被遮挡。
// 主要模块：main()：计算命中点 → 设 id → traceRayEXT（missIndex = 1，payload location 1）。
// 数据流：主光线（raytrace_.rgen）命中 → 本着色器；次级射线未命中时由 raytraceShadow_.rmiss
//   把 isShadowed 置为 false；次级射线的结果目前没有被使用（isShadowed 不影响 hitID）。
// 与其他文件的关系：raytrace2_.rchit 是另一版本（输出命中距离、不发次级射线）。
// 归档说明：cases/ 为历史变体副本，include 的 "../xxx" 相对 shader/<子目录>（待核对）。
// ==========================================================================

#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_ray_query : enable
#extension GL_ARB_shader_clock : enable                 // Using clockARB
#extension GL_EXT_shader_image_load_formatted : enable  // The folowing extension allow to pass images as function parameters

#extension GL_NV_shader_sm_builtins : require     // Debug - gl_WarpIDNV, gl_SMIDNV
#extension GL_ARB_gpu_shader_int64 : enable       // Debug - heatmap value
#extension GL_EXT_shader_realtime_clock : enable  // Debug - heatmap timing

#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_debug_printf : enable
#extension GL_KHR_memory_scope_semantics:require
#extension GL_EXT_shader_atomic_float: require 

// 模式开关宏：被共用头文件用来选择体元追踪分支。
#define VOXELTRACING 1

#include "../bindinglayout.h"
#include "../parameters.h"
#include "../global.glsl"
#include "../bsdf.glsl"
// #include "../functions.glsl"
#include "../nanovdb_info.glsl"
#include "../shade_state_voxel.glsl"
#include "../random.glsl"

hitAttributeEXT vec2 attribs;

// location 0：主光线载荷（由 raygen 提供）；location 1：次级（遮挡）射线载荷 isShadowed。
layout(location = 0) rayPayloadInEXT hitValue hitID;
layout(location = 1) rayPayloadEXT bool isShadowed;

// 作用：命中点处向 +y 方向发探测射线。gl_HitTEXT 为命中距离，
//   origin = 射线起点 + 方向 × 命中距离 = 世界坐标命中点。
// 说明：hitID.id = -70 是固定的“命中”标记（原本想写 gl_PrimitiveID / 实例号，已被注释）；
//   次级射线用 TerminateOnFirstHit，tMin = 0.001 避免自相交。
void main()
{
    float tMin   = 0.001;
    float tMax   = INFINITY;
    vec3  origin = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;   //// gl_HitTEXT is the distance in intersection.
    uint  flags  = gl_RayFlagsTerminateOnFirstHitEXT ;//| gl_RayFlagsOpaqueEXT;// | gl_RayFlagsSkipClosestHitShaderEXT;
    vec3  rayD = vec3(0,1,0);
    hitID.id = -70;//gl_PrimitiveID;// gl_InstanceCustomIndexEXT;

    traceRayEXT(topLevelAS,     // acceleration structure
                flags,          // rayFlags
                0xFF,           // cullMask
                0,              // sbtRecordOffset
                0,              // sbtRecordStride
                1,              // missIndex
                origin.xyz,     // ray origin
                tMin,           // ray min range
                rayD,  // ray direction
                tMax,           // ray max range
                1               // payload (location = 1)
    );

    
}