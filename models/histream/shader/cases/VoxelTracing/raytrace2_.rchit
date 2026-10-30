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

#define VOXELTRACING 1

#include "../bindinglayout.h"
#include "../parameters.h"
#include "../global.glsl"
// #include "../bsdf.glsl"
// #include "../functions.glsl"
// #include "../nanovdb_info.glsl"
// #include "../shade_state_voxel.glsl"
// #include "../random.glsl"

hitAttributeEXT vec2 attribs;

layout(location = 0) rayPayloadInEXT hitValue hitID;
layout(location = 1) rayPayloadEXT bool isShadowed;

void main()
{
    float tMin   = 0.001;
    float tMax   = INFINITY;
    vec3  origin = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    uint  flags  = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipClosestHitShaderEXT;
    vec3  rayD = vec3(0,1,0);
    hitID.id = gl_HitTEXT;

    // traceRayEXT(topLevelAS,  // acceleration structure
    //             flags,       // rayFlags
    //             0xFF,        // cullMask
    //             0,           // sbtRecordOffset
    //             0,           // sbtRecordStride
    //             1,           // missIndex
    //             origin,      // ray origin
    //             tMin,        // ray min range
    //             rayD,        // ray direction
    //             tMax,        // ray max range
    //             1            // payload (location = 1)
    // );

    //gl_HitTEXT;//gl_PrimitiveID;
}