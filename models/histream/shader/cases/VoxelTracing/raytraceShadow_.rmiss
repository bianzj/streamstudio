// ==========================================================================
// 文件：VoxelTracing/raytraceShadow_.rmiss
// 功能：VoxelTracing 旧方案次级（遮挡）射线的 miss 着色器：未命中 → isShadowed = false。
// 数据流：被 raytrace_.rchit 发出的次级射线（missIndex = 1，payload location 1）调用。
//   命中时不进入本着色器（isShadowed 保持调用方的初值），因此语义为“未命中即未被遮挡”。
// 与其他文件的关系：配套 raytrace_.rchit；与 raytrace_.rmiss（主光线 miss）并列于 SBT 的 miss 组。
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

layout(location = 1) rayPayloadInEXT bool isShadowed;

void main()
{
    // 次级射线没打到任何几何 → 未被遮挡。
    isShadowed = false;
}