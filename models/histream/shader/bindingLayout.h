#ifndef BINDINGLAYOUT
#define BINDINGLAYOUT

#include "settings.h"
#include "parameters.h"


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
// Physical texture: offset per TLAS instance (0xFFFFFFFF = none) into per-triangle factors.
layout(binding = B_TEXOFFSET) buffer _bufferTextureOffset { uint instanceTextureOffsets[]; };
layout(binding = B_TEXFACTOR) buffer _bufferTextureFactor { float textureFactors[]; };
layout(buffer_reference, scalar) buffer Vertices { VertexAttribute v[]; };
layout(buffer_reference, scalar) buffer Indices { uvec3 i[]; };
layout(push_constant) uniform _RtxState
{
  RayRTSetting setting;
};

#endif



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


// 1-5
layout(binding = B_SPECTRAL) buffer _bufferOPTICAL { SpectralMaterial specMaterials[]; }; // --
layout(binding = B_FIXEDSPECTRAL) buffer _bufferFOPTICAL { SpectralMaterial spectralMaterials[]; };
layout(binding = B_THERMAL) buffer _bufferTem { ThermalMaterial tempMaterials[]; }; // --
layout(binding = B_TEMPE) buffer _bufferTHERMAL { VoxelTempe voxelTempes[]; };
layout(binding = B_CANOPY) buffer _BufferCanopy {Canopy canopies[];};
// 5-10 
layout(binding = B_MESHLINK) buffer _bufferMODELLINK { MeshLink meshLinks[]; }; 
layout(binding = B_INSTANCELINK) buffer _BufferInstanceLink {InstanceLink instanceLinks[];};
layout(binding = B_VOXELLINK) buffer _BufferBufferVoxelLink {VoxelLink voxelLinks[];};
layout(binding = B_NANO) buffer _BufferVoxelNANO {uint pnanovdb_buf_data[];};
layout(binding = B_TLAS)		uniform accelerationStructureEXT topLevelAS;
layout(binding = B_SENSOR) uniform _CameraMatrix { SensorMatrix sensorMatrix; };
//11-15
layout(binding = B_WAVE) buffer _bufferWAVE { float wave[]; };
layout(binding = B_LIGHT) uniform _LightSet { LightSet lightSet; };
layout(binding = B_ATOM) buffer _bufferWAVEset { AtomCond atomConds[]; };
layout(binding = B_DIR) buffer _bufferDir { VoxelDir voxelDirs[]; };
// Physical addresses allow the 64-direction scratch field to exceed 4 GiB.
layout(buffer_reference, scalar, buffer_reference_align = 8) buffer VoxelRadianceBuffer {
  VoxelRad values[];
};
#define voxelRads VoxelRadianceBuffer(setting.radianceAddress).values
//12 -20
layout(binding = B_NETRAD) buffer _bufferNET {VoxelNetRad voxelNetRads[];}; //
layout(binding = B_PNET) buffer _bufferPNRad { VoxelPnet voxelPnets[]; };
layout(binding = B_STORAGE) buffer _bufferstorage { float outImage[]; };
layout(binding = B_METEO) uniform UBOM{Meteo meteo;} ubom;
layout(binding = B_AERO) buffer BUFFER2{ AeroCond aeroConds[];};
//20-25
layout(binding = B_RAA) buffer RAA{VoxelRaa voxelRaas[];};
layout(binding = B_LEAFBIO) buffer UBOL{LeafBio leafBios[];};
layout(binding = B_SOILSET) buffer UBOS{SoilSet soilSets[];};
layout(binding = B_RSS) buffer BUFFER5{VoxelRss voxelRsss[];};
layout(binding = B_AIR) buffer BUFFER4{VoxelAir voxelAirs[];};
// 25-28
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
layout(binding = B_HEX) buffer _bufferHex { VoxelHex voxelHexs[]; };

layout(buffer_reference, scalar) buffer Vertices { VertexAttribute v[]; };
layout(buffer_reference, scalar) buffer Indices { uvec3 i[]; };
layout(push_constant) uniform _RtxState
{
  VoxellstSetting setting;
};

#endif




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


// 1-5
layout(binding = B_SPECTRAL) buffer _bufferOPTICAL { SpectralMaterial spectralMaterials[]; }; // --
layout(binding = B_THERMAL) buffer _bufferTem { ThermalMaterial thermalMaterials[]; }; // --
layout(binding = B_CANOPY) buffer _BufferCanopy {Canopy canopies[];};
// 5-10 
layout(binding = B_MESHLINK) buffer _bufferMODELLINK { MeshLink meshLinks[]; }; 
layout(binding = B_INSTANCELINK) buffer _BufferInstanceLink {InstanceLink instanceLinks[];};
layout(binding = B_VOXELLINK) buffer _BufferBufferVoxelLink {VoxelLink voxelLinks[];};
layout(binding = B_NANO) buffer _BufferVoxelNANO {uint pnanovdb_buf_data[];};
layout(binding = B_TLAS)		uniform accelerationStructureEXT topLevelAS;
layout(binding = B_SENSOR) uniform _CameraMatrix { SensorMatrix sensorMatrix; };
//11-15
layout(binding = B_WAVE) buffer _bufferWAVE { float wave[]; };
layout(binding = B_LIGHT) uniform _LightSet { LightSet lightSet; };
layout(binding = B_DIR) buffer _bufferDir { VoxelDir voxelDirs[]; };
layout(binding = B_RADS) buffer _bufferRad { VoxelRad voxelRads[]; };
//12 -20
//20-25

// 25-28
layout(binding = B_NETRAD) buffer _bufferNET {VoxelNetRad voxelNetRads[];}; //
layout(binding = B_STORAGE) buffer _bufferstorage { float outImage[]; };
layout(binding = B_LAD) buffer  _LAD{float lads[];};
layout(binding = B_HEX) buffer _bufferHex { VoxelHex voxelHexs[]; };
layout(binding = B_WATERSET) buffer _bufferWater { WaterSet waterSets[]; };

layout(buffer_reference, scalar) buffer Vertices { VertexAttribute v[]; };
layout(buffer_reference, scalar) buffer Indices { uvec3 i[]; };
layout(push_constant) uniform _RtxState
{
  VoxelrtSetting setting;
};

#endif


#endif

