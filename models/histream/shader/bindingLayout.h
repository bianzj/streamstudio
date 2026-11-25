// ==========================================================================
// 文件：bindingLayout.h
// 功能：histream 各 Vulkan 着色器管线的描述符绑定（binding）布局与 push constant 声明。
//       通过编译宏选择当前着色器所属的管线，每个宏段声明一套 B_* 绑定号与对应的
//       buffer/uniform/加速结构接口块。
// 主要模块：
//   1. RAYTRACING 段：RayTracing 模式（光谱/热材料、TLAS、传感器、光源、波长集、输出图像、
//      物理纹理偏移与因子）；push constant 为 RayRTSetting
//   2. VOXELLST 段：体元能量平衡（体元温度、冠层、风场/流体、土壤、水体、通量等）；
//      push constant 为 VoxellstSetting（宏名与模式对应关系待核对）
//   3. VOXELRT 段：VoxelRT 体元辐射（体元方向/辐射缓冲、冠层、NanoVDB、LAD、水体等）；
//      push constant 为 VoxelrtSetting
// 数据流：宿主端（C++/Vulkan）按相同的绑定号创建描述符集并写入缓冲；着色器通过本文件中
//         声明的名字（如 spectralMaterials、voxelDirs、outImage）访问。
// 与其他文件关系：依赖 settings.h（各 *Setting 结构体）与 parameters.h（材料、体元等数据结构）；
//         宿主端描述符布局必须与本文件的 B_* 编号一致（见 models/histream/src 中对应的管线创建代码）。
// 约定：各宏段互相独立，B_* 编号只在所属段内有意义（同名宏编号可不同，如 B_THERMAL 在
//       RAYTRACING 与 VOXELRT 中均为 1，而 VOXELLST 中为 2）；每段的 `// binding a-b` 行内注释
//       是分组提示（2026-11-25 已按当前 #define 更正；此前为不对应的旧编号 `// 1-5` 等），以 #define 为准。
// ==========================================================================

#ifndef BINDINGLAYOUT
#define BINDINGLAYOUT

// 依赖：settings.h 提供 RayRTSetting / VoxellstSetting / VoxelrtSetting 等 push constant 结构；
//       parameters.h 提供材料、体元、冠层等 GPU 数据结构。
#include "settings.h"
#include "parameters.h"


// 模块：RayTracing 管线绑定（B_SPECTRAL..B_TEXFACTOR，共 11 个）。
// 说明：行尾的 `// --` 为原作者标记，含义不明（待核对）。
#ifdef RAYTRACING
#define B_SPECTRAL 0
#define B_THERMAL 1
#define B_ModelLINK 2
#define B_INSTANCELINK 3
#define B_TLAS 4
#define B_SENSOR 5
#define B_LIGHT 6
#define B_WAVE 7
#define B_STORAGE 8
#define B_TEXOFFSET 9
#define B_TEXFACTOR 10

layout(binding = B_SPECTRAL) buffer _bufferOPTICAL { SpectralMaterial spectralMaterials[]; }; // --
layout(binding = B_THERMAL) buffer _bufferTHERMAL { ThermalMaterial thermalMaterials[]; };    // --
layout(binding = B_ModelLINK) buffer _bufferMODELLINK { MeshLink meshLinks[]; }; 
layout(binding = B_INSTANCELINK) buffer _BufferInstanceLink {InstanceLink instanceLinks[];};      // --
layout(binding = B_TLAS) uniform accelerationStructureEXT topLevelAS;
layout(binding = B_SENSOR) uniform _SensorMatrix { SensorMatrix sensorMatrix; };              // ??
// layout(binding = B_WAVEIND) buffer _bufferWaveInds {SensorWaveInd sensorWaveInds[];};
layout(binding = B_LIGHT) uniform _LightSet { LightSet lightSet; };                          // -- 
layout(binding = B_WAVE) buffer _bufferWAVE { float waveSets[]; };                         // --
layout(binding = B_STORAGE) buffer _bufferstorage { float outImage[]; };
// 物理纹理：按 TLAS 实例给出每三角形因子的起始偏移，0xFFFFFFFF 表示该实例无纹理因子。
// Physical texture: offset per TLAS instance (0xFFFFFFFF = none) into per-triangle factors.
layout(binding = B_TEXOFFSET) buffer _bufferTextureOffset { uint instanceTextureOffsets[]; };
layout(binding = B_TEXFACTOR) buffer _bufferTextureFactor { float textureFactors[]; };
// 顶点/索引缓冲通过 buffer_reference（物理地址）访问，不占用描述符绑定号。
layout(buffer_reference, scalar) buffer Vertices { VertexAttribute v[]; };
layout(buffer_reference, scalar) buffer Indices { uvec3 i[]; };
// push constant：RayTracing 的运行参数 RayRTSetting（每次 dispatch 传入）。
layout(push_constant) uniform _RtxState
{
  RayRTSetting setting;
};

#endif



// 模块：VOXELLST 管线绑定（B_SPECTRAL..B_SOILMEMORY，共 42 个）。
// 说明：此段包含体元温度/能量平衡所需的全部缓冲：材料、冠层、体元链接、NanoVDB、
//       方向/辐射、气象、空气动力学、土壤、水体、体元热通量、上一时刻温度（Tlast）、
//       EBState、LAD 及流体（LBM/速度/标量）场。
#ifdef VOXELLST

#define B_SPECTRAL 0
#define B_FIXEDSPECTRAL 1
#define B_THERMAL 2
#define B_TEMPE 3
#define B_CANOPY 4
#define B_MESHLINK 5
#define B_INSTANCELINK 6
#define B_VOXELLINK 7
#define B_NANO 8
#define B_TLAS 9
#define B_SENSOR 10
#define B_WAVE 11
#define B_LIGHT 12
#define B_ATOM 13
#define B_DIR 14
#define B_RADS 15
#define B_NETRAD 16
#define B_PNET 17
#define B_STORAGE 18
#define B_METEO 19
#define B_AERO 20
#define B_RAA 21
#define B_LEAFBIO 22
#define B_SOILSET 23
#define B_RSS 24
#define B_AIR 25
#define B_FLUX 26
#define B_TLAST 27
#define B_STATE 28
#define B_LAD 29 
#define B_WATERSET 30
#define B_FLUID_VELOCITY_A 31
#define B_FLUID_VELOCITY_B 32
#define B_FLUID_SCALAR_A 33
#define B_FLUID_SCALAR_B 34
#define B_FLUID_LBM_A 35
#define B_FLUID_LBM_B 36
#define B_FLUID_DENSITY 37
#define B_FLUID_META 38
#define B_FLUID_PARAMETERS 39
#define B_HEX 40
// 土壤方法 1 的指数和（sum-of-exponentials）历史项缓冲，见下方原注释。
// Soil method-1 sum-of-exponentials history: SOIL_MEMORY_MODES (sunlit, shaded)
// pairs per voxel; a single element unless some soil set uses it.
#define B_SOILMEMORY 41

// binding 0-4（2026-11-25 按当前 #define 更正分组注释，下同）
layout(binding = B_SPECTRAL) buffer _bufferOPTICAL { SpectralMaterial specMaterials[]; }; // --
layout(binding = B_FIXEDSPECTRAL) buffer _bufferFOPTICAL { SpectralMaterial spectralMaterials[]; };
layout(binding = B_THERMAL) buffer _bufferTem { ThermalMaterial tempMaterials[]; }; // --
layout(binding = B_TEMPE) buffer _bufferTHERMAL { VoxelTempe voxelTempes[]; };
layout(binding = B_CANOPY) buffer _BufferCanopy {Canopy canopies[];};
// binding 5-10
layout(binding = B_MESHLINK) buffer _bufferMODELLINK { MeshLink meshLinks[]; }; 
layout(binding = B_INSTANCELINK) buffer _BufferInstanceLink {InstanceLink instanceLinks[];};
layout(binding = B_VOXELLINK) buffer _BufferBufferVoxelLink {VoxelLink voxelLinks[];};
layout(binding = B_NANO) buffer _BufferVoxelNANO {uint pnanovdb_buf_data[];};
layout(binding = B_TLAS)		uniform accelerationStructureEXT topLevelAS;
layout(binding = B_SENSOR) uniform _CameraMatrix { SensorMatrix sensorMatrix; };
// binding 11-14（binding 15 即 B_RADS，见下）
layout(binding = B_WAVE) buffer _bufferWAVE { float wave[]; };
layout(binding = B_LIGHT) uniform _LightSet { LightSet lightSet; };
layout(binding = B_ATOM) buffer _bufferWAVEset { AtomCond atomConds[]; };
layout(binding = B_DIR) buffer _bufferDir { VoxelDir voxelDirs[]; };
// 注意：本段的体元辐射 voxelRads 不走描述符绑定：B_RADS=15 只是占位编号，没有 layout(binding = B_RADS) 的缓冲声明
//       （着色器里没有 layout(binding = B_RADS) 的缓冲声明），而是通过 push constant 中的 radianceAddress 物理地址访问，
//       避免超过 4 GiB 的限制。
// Physical addresses allow the 64-direction scratch field to exceed 4 GiB.
layout(buffer_reference, scalar, buffer_reference_align = 8) buffer VoxelRadianceBuffer {
  VoxelRad values[];
};
#define voxelRads VoxelRadianceBuffer(setting.radianceAddress).values
// binding 16-20：体元净辐射（voxelNetRads）、光合净辐射、输出图像、气象 UBO、气动条件
layout(binding = B_NETRAD) buffer _bufferNET {VoxelNetRad voxelNetRads[];}; //
layout(binding = B_PNET) buffer _bufferPNRad { VoxelPnet voxelPnets[]; };
layout(binding = B_STORAGE) buffer _bufferstorage { float outImage[]; };
layout(binding = B_METEO) uniform UBOM{Meteo meteo;} ubom;
layout(binding = B_AERO) buffer BUFFER2{ AeroCond aeroConds[];};
// binding 21-25
layout(binding = B_RAA) buffer RAA{VoxelRaa voxelRaas[];};
layout(binding = B_LEAFBIO) buffer UBOL{LeafBio leafBios[];};
layout(binding = B_SOILSET) buffer UBOS{SoilSet soilSets[];};
layout(binding = B_RSS) buffer BUFFER5{VoxelRss voxelRsss[];};
layout(binding = B_AIR) buffer BUFFER4{VoxelAir voxelAirs[];};
// binding 26-41（热通量、末次温度、能量平衡状态、叶面积密度、水体、流体场、HEX、土壤记忆）
layout(binding = B_FLUX) buffer FLUX{VoxelHeatFlux voxelHeatFlux[];};
layout(binding = B_TLAST) buffer BUFFER3{VoxelTlast voxelTlasts[];};
layout(binding = B_STATE) buffer  _UBOSS{EBState ebState;};
layout(binding = B_LAD) buffer  _LAD{float lads[];};
layout(binding = B_WATERSET) buffer UBOW{WaterSet waterSets[];};
layout(binding = B_FLUID_VELOCITY_A) buffer FLUIDVA{vec4 fluidVelocityA[];};
layout(binding = B_FLUID_VELOCITY_B) buffer FLUIDVB{vec4 fluidVelocityB[];};
layout(binding = B_FLUID_SCALAR_A) buffer FLUIDSA{vec4 fluidScalarA[];};
layout(binding = B_FLUID_SCALAR_B) buffer FLUIDSB{vec4 fluidScalarB[];};
layout(binding = B_FLUID_LBM_A) buffer FLUIDLBMA{float fluidLbmA[];};
layout(binding = B_FLUID_LBM_B) buffer FLUIDLBMB{float fluidLbmB[];};
layout(binding = B_FLUID_DENSITY) buffer FLUIDRHO{float fluidDensity[];};
layout(binding = B_FLUID_META) buffer FLUIDMETA{FluidCellMeta fluidMeta[];};
layout(binding = B_FLUID_PARAMETERS) uniform FLUIDPARAMS{FluidParameters fluidParameters;};
// VoxelHex 体元数据缓冲（具体含义待核对）。
layout(binding = B_HEX) buffer _bufferHex { VoxelHex voxelHexs[]; };
layout(binding = B_SOILMEMORY) buffer _bufferSoilMemory { vec2 soilMemories[]; };

layout(buffer_reference, scalar) buffer Vertices { VertexAttribute v[]; };
layout(buffer_reference, scalar) buffer Indices { uvec3 i[]; };
// push constant：VOXELLST 的运行参数 VoxellstSetting（含 radianceAddress 等物理地址）。
layout(push_constant) uniform _RtxState
{
  VoxellstSetting setting;
};

#endif




// 模块：VoxelRT 管线绑定（B_SPECTRAL..B_WATERSET，共 18 个）。
//       与 VOXELLST 相比去掉了气象/土壤/流体等能量平衡相关缓冲，且 voxelRads 为普通 SSBO。
#ifdef VOXELRT

#define B_SPECTRAL 0
#define B_THERMAL 1
#define B_CANOPY 2
#define B_MESHLINK 3
#define B_INSTANCELINK 4
#define B_VOXELLINK 5
#define B_NANO 6
#define B_TLAS 7
#define B_SENSOR 8
#define B_WAVE 9
#define B_LIGHT 10
#define B_DIR 11
#define B_RADS 12
#define B_NETRAD 13
#define B_STORAGE 14
#define B_LAD 15  
#define B_HEX 16
#define B_WATERSET 17


// binding 0-4
layout(binding = B_SPECTRAL) buffer _bufferOPTICAL { SpectralMaterial spectralMaterials[]; }; // --
layout(binding = B_THERMAL) buffer _bufferTem { ThermalMaterial thermalMaterials[]; }; // --
layout(binding = B_CANOPY) buffer _BufferCanopy {Canopy canopies[];};
layout(binding = B_MESHLINK) buffer _bufferMODELLINK { MeshLink meshLinks[]; }; 
layout(binding = B_INSTANCELINK) buffer _BufferInstanceLink {InstanceLink instanceLinks[];};
// binding 5-8
layout(binding = B_VOXELLINK) buffer _BufferBufferVoxelLink {VoxelLink voxelLinks[];};
layout(binding = B_NANO) buffer _BufferVoxelNANO {uint pnanovdb_buf_data[];};
layout(binding = B_TLAS)		uniform accelerationStructureEXT topLevelAS;
layout(binding = B_SENSOR) uniform _CameraMatrix { SensorMatrix sensorMatrix; };
// binding 9-12
layout(binding = B_WAVE) buffer _bufferWAVE { float wave[]; };
layout(binding = B_LIGHT) uniform _LightSet { LightSet lightSet; };
layout(binding = B_DIR) buffer _bufferDir { VoxelDir voxelDirs[]; };
// 体元辐射缓冲：这里直接绑定为 SSBO（VOXELLST 段则用物理地址）。
layout(binding = B_RADS) buffer _bufferRad { VoxelRad voxelRads[]; };
// binding 13-17（本段没有 VOXELLST 段的气象/土壤/流体等缓冲，原 `12 -20`、`20-25` 等空分组注释已删除）
layout(binding = B_NETRAD) buffer _bufferNET {VoxelNetRad voxelNetRads[];}; //
layout(binding = B_STORAGE) buffer _bufferstorage { float outImage[]; };
layout(binding = B_LAD) buffer  _LAD{float lads[];};
layout(binding = B_HEX) buffer _bufferHex { VoxelHex voxelHexs[]; };
layout(binding = B_WATERSET) buffer _bufferWater { WaterSet waterSets[]; };

layout(buffer_reference, scalar) buffer Vertices { VertexAttribute v[]; };
layout(buffer_reference, scalar) buffer Indices { uvec3 i[]; };
// push constant：VoxelRT 的运行参数 VoxelrtSetting。
layout(push_constant) uniform _RtxState
{
  VoxelrtSetting setting;
};

#endif


#endif

