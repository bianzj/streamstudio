#ifndef STRUCTURE
#define STRUCTURE 1

// #ifdef __cplusplus
// #include <stdint.h>
// #include "nvmath/nvmath.h"
// // GLSL Type
// using ivec2 = nvmath::vec2i;
// using vec2  = nvmath::vec2f;
// using vec3  = nvmath::vec3f;
// using vec4  = nvmath::vec4f;
// using mat4  = nvmath::mat4f;
// using uint  = unsigned int;
// #endif

#include "global.glsl"

//////////////////////////////////////////////////////////////
//   Parameters used in Input
//////////////////////////////////////////////////////////////

//--------------------------------------------------------------------------------------------
// Entity /Component Property
//--------------------------------------------------------------------------------------------

// Wavelength link to sunlight source and camera metric as well as optical material


// reflectance and transmittance
struct SpectralMaterial
{
	float reflectance;
	float transmittance;
    float specularFraction;
    float specularRoughness;
};
// sunlit and shaded temperature
struct ThermalMaterial
{
	float sunlitTemperature;
	float shadedTemperature;
};

// canopy information
struct Canopy
{
	float lai;
	float density;
	float height;
	float width;
	float G;
	float LIDFa;
	float LIDFb;
	float hspot;
	float leafwidth;
	int structureType; // 0: canopy, 1: rigid, 2: fire/smoke, 3: fog
	float extinction;
	float scatteringAlbedo;
	float asymmetry;
	float emissionScale;
	float fixedTemperature;
};

bool isParticipatingMedium(Canopy canopy)
{
	return canopy.structureType == 2 || canopy.structureType == 3;
}

float canopyVoxelTransmittance(Canopy canopy, float density, float G, float pathLength, float scale)
{
	if(canopy.structureType == 1) return 0.0;
	// Surface intersections can include the empty segment from the previous
	// object.  A participating-medium sample must never integrate farther than
	// one voxel diagonal, otherwise a hot voxel creates view-aligned streaks.
	if(isParticipatingMedium(canopy)) pathLength = clamp(pathLength, 0.0, 1.7320508);
	float opticalDepth = isParticipatingMedium(canopy)
		? max(canopy.extinction, 0.0) * pathLength * scale
		: max(density * G, 0.0) * pathLength * scale;
	return exp(-opticalDepth);
}

float mediumPhaseHG(Canopy canopy, float cosTheta)
{
	float g = clamp(canopy.asymmetry, -0.99, 0.99);
	float denominator = max(1.0 + g * g - 2.0 * g * clamp(cosTheta, -1.0, 1.0), 0.0001);
	return (1.0 - g * g) / (12.5663706144 * pow(denominator, 1.5));
}

// sphere instance defination
struct Sphere
{
  vec3  center;
  float radius;
};

// aabb instance defination
struct AllAabb
{
  vec3 minimum;
  int empty;
  vec3 maximum;
};

struct hitValue
{
	float id;
};

//--------------------------------------------------------------------------------------------
// Entity / Component
//--------------------------------------------------------------------------------------------

struct VertexAttribute
{
	vec3 position;
	vec3 NRM;
	vec3 color;
	vec2 txt;
};

struct SensorMatrix
{
	mat4 viewInverse;
	mat4 projInverse;
	float focalDist;
	float aperture;
	int n_wave;
	int empty;
	vec3 direction;
};


struct LightSet
{
	vec3  direction;
	float direct;
	float diffuse;
	float Rin;
	float Rli;
	float skyTemperature;
	float solarTemperature;
};

struct AtomCond
{
	float wavelength;
	float direct;
	float diffuse;
};

// obj model link
struct MeshLink
{
	int type;
	int spectralId;
	int thermalId;
	int canopyId;
	int bioId;
	float angularEffectStrength;
    // int leafbioId;
    // int soilsetId;
	uint64_t vertexAddress;
	uint64_t indexAddress;
};

// obj instance link
struct InstanceLink
{
	int meshId;
};


// voxel link
struct VoxelLink
{
    ivec3 voxelId;
    int instanceId;
	int aeroId;
	int faceId;
	int isValid;
	float leafDensity; // geometry-derived leaf area density, 0 = use canopy density
	int hexId;
	// Outward surface normal of a one-state solid OBJ cell; zero = horizontal.
	float normalX;
	float normalY;
	float normalZ;
};

struct VoxelHex
{
    float ax;
    float ay;
    float az;
    float rho;
    float a;
    float b;
    float c;
    int model;
    vec4 children[8];
};

// transmittance
struct VoxelDir
{
    float solar;
    // Uncollided diffuse-sky illumination relative to an open horizontal
    // surface (cosine-weighted for solids, sphere-averaged for foliage).
    float skyDiffuse;
};

struct VoxelTempe
{
    float sunlit;
    float shaded;
};

// radiance
struct VoxelRad
{
    float cumulated;
    float induced;
};

// net radiance
struct VoxelNetRad
{
    float directVrad;
    float diffuseVrad;
    float directTrad;
    float diffuseTrad;
};

struct VoxelPnet
{
    float directPnet;
    float diffusePnet;
};


// Leaf-surface state per conditional component: [0] shaded, [1] sunlit.
struct VoxelAir
{
    float cs[2];    // CO2 at the leaf surface (ppm)
    float ci[2];    // intercellular CO2 (ppm)
    float es[2];    // vapour pressure at the leaf surface (hPa)
    float heightM;
    float soilMoisture;
    float co2ExcessPpm;
};

struct VoxelRaa
{
	float raa;
};

struct VoxelRss
{
    float sunlit;   // surface resistance at sunlit surface in this voxel
    float shaded;   // surface resistance at shaded surface in this voxel
};

struct AeroCond
{
	int type;
	float L;
	float ustar;
    float hc_veg;
    float hc_build;
    float lai;
	float leafwidth;
    float cover;
};

// Rn - H - LE - G = 0. For water, G stores mixed-layer heat storage Qw.
struct VoxelHeatFlux
{
    float Hsunlit;  // sensible heat flux of sunlit component
    float Hshaded;  // sensible heat flux of shaded component
    float LEsunlit; // latent heat flux of sunlit component
    float LEshaded; // latent heat flux of shaded component
    float Gsunlit;  // change in heat storage of sunlit component
    float Gshaded;  // change in heat storage of shaded component
    float GPPsunlit;  // gross leaf photosynthesis, umol CO2 m-2 leaf s-1
    float GPPshaded;
    float NPPsunlit;  // net leaf assimilation (GPP - dark respiration)
    float NPPshaded;
};

struct VoxelTlast
{
	float sunlit;
	float shaded;
};

struct EBState
{
	int count;
};

struct FluidCellMeta
{
    int surfaceIndex;
    int kind;
    float lad;
    float padding;
};

struct FluidParameters
{
    ivec4 grid;
    vec4 spacingTime; // xyz spacing, w physical scalar-transport dt
    vec4 ambientWind;
    vec4 physics;
    vec4 sources; // smoke source, lattice speed cap, physical speed cap, m/s per lattice speed
    // Size of the radiative voxel grid used when mapping EB voxels into
    // the (potentially coarser) fluid grid. Keep this layout in sync with
    // FluidParameters in src/base/structs_cg.h.
    vec4 couplingSpacing; // xyz spacing, w terrain soil enabled
    vec4 heightProfile; // enabled, K/m lapse, reference height [m], vapor scale [m]
    vec4 localExchange; // x enables local leaf/soil exchange, yzw reserved
};




struct LeafBio
{
    float Vcmax;		// maximum carboxylation capacity (at optimum temperature)
    float leafM;			// ball-berry stomatal conductance parameter
    float BallBerry;
    float Type;
    float kV;			// extinction coefficient for a vertical profile
    float Rdparam;		// parameter for dark respiration
    float Tparam[5];
    float Tyear;
    float beta;
    float kNPQs;		// rate constant of sustained thermal dissipation
    float qLs;			// fraction of functional reaction centers
    float stressfactor;
    int Tcor;

    //FluspectParam fp;
};

struct SoilSet
{
    int method;
    float rss;			// soil resistance for evaporation
    float cs;			// volumetric heat capacity of the soil
    float rhos;
    float lambdas;
	float Tsoil;  // aeverage temperature 25
    float SMC;			// volumetric soil moisture content 0.25
//    float csSoil;
  //  float rbs;      // boundary prop

    float SatWater;
    int brdfModel;
    float hapkeB0;
    float hapkeH;
    float hapkeG;
    int thermalClass;
    float convectiveScale;

    //BSMParam bsm;
};

struct WaterSet
{
    float rss;
    float heatCapacity;
    float mixingDepth;
    float evaporationCoefficient;
    int brdfModel;
    float refractiveIndex;
    float slopeVariance;
    float diffuseFraction;
};


struct Meteo
{
	float z;
	float t;
	float u;
	float p;
	float Rin;
	float Rli;
	float Ta;
	float sm;
	float ea;
	float Ca;
	float Oa;
	float dtime;
};




#endif
