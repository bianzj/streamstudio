#ifndef SETTINGS
#define SETTINGS



//////////////////////////////////////////////////////////////
//    Pushconstant
//////////////////////////////////////////////////////////////
//  Radiative transfer setting
struct RayRTSetting
{
	int frame;
	int maxDepth;
	int n_sample;
	int n_wave;
	ivec2 imageSize;  	// 1000*1000
	// vec2 sceneSize;		// 100*100
	int band1; 
	int band2;
	int band3;
	int isDisplay;
	int isTemperature;
	float fireflyClampThreshold;
	int debugging_mode;
	int periodicNeighborCount;
	int skyboxEnabled;
	int imageOffsetY;
	vec2 sceneSize;
};

struct VoxellstSetting
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
#endif
};

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
};


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
