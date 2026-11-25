// ==========================================================================
// 文件：parameters.h
// 功能：着色器（GLSL）侧共用的输入参数与状态结构体定义，以及几个与冠层介质相关的小函数。
// 主要模块：
//   1. 材料/冠层参数：SpectralMaterial、ThermalMaterial、Canopy（含介质消光与 HG 相函数）
//   2. 几何与传感器：Sphere、AllAabb、VertexAttribute、SensorMatrix
//   3. 光源与链接表：LightSet、AtomCond、MeshLink、InstanceLink、VoxelLink、VoxelHex
//   4. 体元状态（VoxelEB 等）：VoxelDir/Tempe/Rad/NetRad/Pnet/Air/Raa/Rss/HeatFlux/Tlast、EBState
//   5. 流体与生化参数：FluidCellMeta、FluidParameters、LeafBio、SoilSet、WaterSet、Meteo
// 数据流：主机端（C++）按相同内存布局填充缓冲区，各着色器 #include 本文件后直接读写。
// 与其他文件的关系：布局须与 src/base/structs_cg.h 中同名结构体保持一致（各结构体处另有注明）；
//   依赖 global.glsl（类型与常量）；lidf_common.h 使用 Canopy 的 lidf/lidfValid。
// 注意：结构体字段顺序/数量即缓冲区布局，修改任一字段必须同步主机端定义。
// ==========================================================================

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

// 模块：输入参数（按实体/组件分类）。以下各结构体由主机端写入 GPU 缓冲区，按材料/网格/体元 id 索引。
// Wavelength link to sunlight source and camera metric as well as optical material


// reflectance and transmittance
// 光谱材料：反射率、透射率，以及镜面反射占比与粗糙度（供 specular/surface BRDF 使用）。
struct SpectralMaterial
{
	float reflectance;
	float transmittance;
    float specularFraction;
    float specularRoughness;
};
// sunlit and shaded temperature
// 热材料：直接给定的向阳/背阴表面温度，单位 K。
struct ThermalMaterial
{
	float sunlitTemperature;
	float shadedTemperature;
};

// canopy information
// 冠层/介质参数。structureType 区分冠层(0)、刚体(1)、火/烟(2)、雾(3)；
// 2、3 视为参与介质，用 extinction/scatteringAlbedo/asymmetry/emissionScale/fixedTemperature 描述。
// lai/density 等为叶面积指数与叶面积密度，G 为叶倾角投影函数，hspot 为热点参数。
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
	// 1: lidf holds the 13 SAIL leaf inclination class frequencies (host
	// derived from LIDFa/LIDFb), which define G(Omega) and Gamma; 0: constant
	// G (see lidf_common.h). Keep in sync with Canopy in src/base/structs_cg.h.
	int lidfValid;
	float lidf[13];
	// G at |cos zenith| (2k+1)/64, i.e. at DIFFUSE_DIRECTIONS[2k] and [2k+1].
	float diffuseG[32];
};

// 是否为参与介质（火/烟或雾），此类体元按体消光/发射/散射处理，而非叶片截获。
bool isParticipatingMedium(Canopy canopy)
{
	return canopy.structureType == 2 || canopy.structureType == 3;
}

// 作用：体元内沿射线路径的透过率 exp(-光学厚度)。
// 输入：冠层参数、叶面积密度 density、投影函数 G、路径长度 pathLength、尺度因子 scale；输出：0~1 的透过率。
// 说明：刚体（structureType==1）完全不透；介质用 extinction，植被用 density*G。
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

// 作用：Henyey–Greenstein 相函数，按 4π 归一化（12.5663706144 = 4π）。
// 输入：不对称因子 asymmetry（限幅到 ±0.99 防止分母为零）、散射角余弦 cosTheta；输出：相函数值（sr-1）。
float mediumPhaseHG(Canopy canopy, float cosTheta)
{
	float g = clamp(canopy.asymmetry, -0.99, 0.99);
	float denominator = max(1.0 + g * g - 2.0 * g * clamp(cosTheta, -1.0, 1.0), 0.0001);
	return (1.0 - g * g) / (12.5663706144 * pow(denominator, 1.5));
}

// 模块：几何实例（球、AABB）与顶点/传感器结构体。
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

// 传感器（相机）参数：视图/投影逆矩阵用于由像素生成射线；focalDist、aperture 描述景深；n_wave 为波段数。
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


// 光源集合：太阳方向 direction、直射 direct/散射 diffuse 辐照度（W m-2，按波段/条件取值）；
// Rin/Rli 为入射短波/长波（待核对具体含义）；skyTemperature、solarTemperature 为天空/太阳等效温度（K）。
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

// 单个波长条件（"原子条件"）：波长及对应的直射/散射太阳辐照分量。
struct AtomCond
{
	float wavelength;
	float direct;
	float diffuse;
};

// 模块：链接表。网格→材料/热/冠层/生化 id，实例→网格 id，体元→实例/空气动力/面元 id。
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
// 体元链接：体元网格坐标 voxelId、所属实例 instanceId、空气动力 aeroId、面元 faceId、
// 有效标志 isValid；leafDensity 为几何导出的叶面积密度（0 表示使用 Canopy 的 density）；
// normalX/Y/Z 为单状态实心 OBJ 单元的外法线（全 0 表示水平面）。
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

// 体元六面体（Hex）参数：ax/ay/az、rho、a/b/c、model 以及 8 个子单元 children（含义待核对，见 src/base/structs_cg.h）。
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

// 模块：体元能量平衡（VoxelEB）逐体元状态。向阳/背阴（sunlit/shaded）两种条件分别存储。
// transmittance
struct VoxelDir
{
    float solar;
    // Uncollided diffuse-sky illumination relative to an open horizontal
    // surface (cosine-weighted for solids, sphere-averaged for foliage).
    float skyDiffuse;
};

// 体元温度（K）：向阳、背阴；solarFraction 记录土壤方法 1 历史项上次写入时的向阳比例。
struct VoxelTempe
{
    float sunlit;
    float shaded;
    // Sunlit fraction at which the stored soil-method-1 histories were last
    // written (updateTp); < 0 before the first node. Keep in sync with
    // src/base/structs_cg.h.
    float solarFraction;
};

// radiance
struct VoxelRad
{
    float cumulated;
    float induced;
};

// 净辐射分量：直射/散射、短波(Vrad)/长波(Trad)四项（W m-2，待核对符号约定）。
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


// 叶面气体状态：CO2 与水汽压，用于光合/气孔导度迭代。数组下标约定 [0]=背阴，[1]=向阳。
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

// 空气动力条件：类型、Obukhov 长度 L(m)、摩擦速度 ustar(m s-1)、植被/建筑高度 hc_*、LAI、叶宽、覆盖度。
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

// 体元热通量（W m-2）：能量平衡残差 Rn - H - LE - G = 0 的各项，向阳/背阴分开；
// 同时携带光合量 GPP/NPP（umol CO2 m-2 leaf s-1）。
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

// 模块：EBState — MOST（Monin–Obukhov 相似理论）全场 1/L 归约用的整数定点累加器。
// 用整数原子加而非浮点原子加，结果与线程完成顺序无关；各量的定点单位见字段注释。
// Integer (fixed-point) accumulators keep the MOST reductions deterministic:
// float atomics would make 1/L depend on the thread completion order.
// Keep in sync with EBState in src/base/structs_cg.h. The neutral VoxelEB
// pipelines only address the leading count (pre-MOST declaration).
#define EB_STATE_BINS 128
#ifdef VOXELEB_MOST
struct EBState
{
	int count;
	int groundCount;                 // ground columns that reported u*
	int sensible[EB_STATE_BINS];     // sum of H per cell footprint, 0.1 W m-2
	int ustar[EB_STATE_BINS];        // sum of ground-column u*, 1e-4 m s-1
	int height[EB_STATE_BINS];       // sum of z - d, 0.01 m
	int waterCount;                  // scene water voxels (ship waterline contact)
	int waterTemperature;            // sum of water voxel temperatures, 0.01 K
};
#else
struct EBState
{
	int count;
	// Only count and the ship water sums are used by the neutral pipelines;
	// the middle lanes keep the offsets of the MOST declaration.
	int groundCount;
	int sensible[EB_STATE_BINS];
	int ustar[EB_STATE_BINS];
	int height[EB_STATE_BINS];
	int waterCount;
	int waterTemperature;
};
#endif

// 模块：流体（风场/烟气）网格单元元数据与全局参数。
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
    vec4 localExchange; // x enables local leaf/soil exchange, yzw reserved (VoxelEB MOST: y on, z scene 1/L)
};




// 模块：叶片生化参数（光合 Vcmax、Ball-Berry 气孔导度、暗呼吸与温度响应）。
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

// 土壤/不透水面参数：热方法 method、阻抗 rss、热容 cs、密度 rhos、导热 lambdas、
// 初始温度 Tsoil、含水量 SMC，Hapke BRDF 参数，以及船体（航速、吃水线、钢板、内热源）相关参数。
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
    int inertiaHistory;   // method 1: 0 = 8 nodes (legacy), 1 = + sum-of-exponentials tail
    // Ship hull (see structs_cg.h SoilSet); all 0 = off.
    float shipSpeed;
    float shipWindCosine;
    float waterlineHeight;
    float waterHeatTransferCoefficient;
    // Ship hull steel plate and internal heat source (structs_cg.h).
    float plateThickness;
    float plateLambda;
    float plateCapacity;
    float plateIndoorC;
    float plateIndoorH;
    float internalHeatFlux;

    //BSMParam bsm;
};

// 水体参数：热容、混合层深度、蒸发系数、BRDF 模型、折射率、波面斜率方差及漫射比例。
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


// 气象强迫（单时刻）：参考高度 z、时间 t、风速 u、气压 p、入射短/长波 Rin/Rli、
// 气温 Ta、土壤湿度 sm、水汽压 ea、CO2 Ca、O2 Oa、时间步长 dtime（单位待核对，见 docs/forcing-metadata.md）。
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
