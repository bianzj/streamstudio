// ==========================================================================
// 文件：settings.h
// 功能：着色器端的 push constant 运行设置、光线负载（payload）与着色状态结构体定义。
// 主要模块：
//   1. RayRTSetting / VoxellstSetting / VoxelrtSetting：三类管线（RayTracing、体元 LST/EB、
//      体元 RT）的 push constant 参数
//   2. PtPayload / ShadowHitPayload：光线追踪命中着色器与 raygen 之间传递的负载
//   3. Ray、ShadeState：射线与命中点着色状态（法线、切线、材料 id 等，世界系/局部系）
// 数据流：主机端把设置打包为 push constant；raygen/compute 着色器读取；命中信息经 PtPayload 回传，
//   shade_state.glsl 将其还原成 ShadeState。
// 注意：字段顺序与主机端 C++ 同名结构体一一对应（push constant 布局），修改需同步；
//   VoxellstSetting 中 VOXELLST 宏下的字段仅在对应管线编译时存在。
// ==========================================================================

#ifndef SETTINGS
#define SETTINGS



//////////////////////////////////////////////////////////////
//    Pushconstant
//////////////////////////////////////////////////////////////
// 模块：push constant 设置。RayRTSetting 用于 RayTracing 管线（图像 imageSize、帧号 frame、
// 最大反弹深度 maxDepth、采样数 n_sample、波段数 n_wave、周期邻域数 periodicNeighborCount 等）。
//  Radiative transfer setting
struct RayRTSetting
{
	int frame;
	int maxDepth;
	int n_sample;
	int n_wave;
	ivec2 imageSize;  	// 1000*1000
	// vec2 sceneSize;		// 100*100
	int adaptiveMaxExtra; // adaptive sampling (RT_ADAPTIVE): per-pixel maximum sample count in pass 2

	int adaptiveTile;     // adaptive sampling: side of the square tile whose mean sigma sets the extra samples (warp coherence)
	int adaptiveMinCount; // adaptive sampling pass 2: per-pixel minimum sample count (adaptiveMaxExtra = maximum)
	int isDisplay;
	int isTemperature;
	float adaptiveScale;   // adaptive sampling: samples per unit of scout sigma (pass 2)
	int adaptivePass;     // 0 = normal, 1 = adaptive scout pass, 2 = adaptive estimate pass
	int periodicNeighborCount;
	int skyboxEnabled;
	int imageOffsetY;
	vec2 sceneSize;
	// Highest world y of any geometry (+ margin): periodic traversal stops for
	// rays above it that do not descend.
	float sceneTop;
};

// 体元（LST/EB 相关）管线的设置：体元网格尺寸 voxelSize（xzy 顺序）、尺度 scale、散射阶数与阶段等；
// VOXELLST 宏下额外字段用于多次散射的辐亮度缓冲与能量平衡迭代。
struct VoxellstSetting
{

	int frame;
	int voxelFlags;   // VOXEL_FLAG_* (shader/voxel_flags.glsl); was the unused maxIteration
	int maxDepth;
	int maxStep;
	int n_sample;
	int n_wave;
	int n_jump;
	float scale;
	ivec2 imageSize;
	int isDisplay;
	int isface;
	ivec3 voxelSize; // xzy
	int islad;
	int voxelCount;
	int periodicNeighborCount;
	int skyboxEnabled;
#if defined(VOXELLST)
	uint64_t radianceAddress;
	int scatteringBandStart;
	int scatteringBandEnd;
	int scatteringOrder;
	int scatteringStage;
	uint64_t scatteringStatsAddress;
	int energyFinalize;
	int scatteringOrders;
	int energyIteration;
	// 1 when the radiance buffer carries the image multiple-scattering
	// environment (voxeleb/image_environment.glsl).
	int imageEnvironment;
#endif
};

// VoxelRT 管线的设置：与 VoxellstSetting 类似，另含 spectralBatchSize 与碰撞阶数（collisionOrders/collisionOrder）。
struct VoxelrtSetting
{

	int frame;
	int maxIteration; 
	int maxDepth;
	int maxStep;
	int n_sample;
	int n_wave;
	int n_jump;
	float scale;
	ivec2 imageSize;
	int isDisplay;
	int isface;
	ivec3 voxelSize; // xzy
	int islad;
	int isTemperature;
	int voxelCount;
	int spectralBatchSize;
	int periodicNeighborCount;
	int skyboxEnabled;
	int collisionOrders; // explicit collision orders (diffuse passes)
	int collisionOrder;  // current diffuse pass, 1-based
	int voxelFlags;      // VOXEL_FLAG_* (shader/voxel_flags.glsl)
};


// PtPayload：closest-hit 写回的命中信息（命中距离、图元/实例 id、重心坐标、物体↔世界矩阵、射线起点）。
// 光线追踪过程中光线信息
struct PtPayload
{
	uint   seed;
	float  hitT;
	int    primitiveID;
	int    instanceID;
	int    instanceCustomIndex;
	float  alpha;
	vec2   baryCoord;
	vec3   lastPosition;
	mat4x3 objectToWorld;
	mat4x3 worldToObject;
};

// 光线追踪过程中判断阴影光线信息
struct ShadowHitPayload
{
	uint         seed;
	bool         isHit;
};

struct Ray
{
  vec3 origin;
  vec3 direction;
};

// 命中点着色状态：世界系位置/法线/切线/副法线，同时保留物体局部系的法线与切线，及实例、光谱、热、类型 id。
struct ShadeState
{
	vec3 normal;
	vec3 position;
	vec3 lastPosition;
	vec3 tangent;
	vec3 binormal;
	uint instanceId;
    uint spectralId;
    uint thermalId;
	uint typeId;
	vec3 localNormal;
	vec3 localTangent;
	vec3 localBinormal;
};




#endif
