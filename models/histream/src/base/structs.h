//
// Created by admin on 2024/1/24.
// It is used for the cpu part;
//

#ifndef FIELD_STRUCTS_H
#define FIELD_STRUCTS_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <string>
#include <vector>
#include <map>

#include "nvvk/resourceallocator_vk.hpp"
#include "structs_cg.h"
#include "terrain_environment.h"
#include "src/thirdparty/magic_enum.hpp"
#include "src/thirdparty/NanoVDB.h"
#include "src/thirdparty/nanoutil/GridBuilder.h"
#include "src/thirdparty/nanoutil/Primitives.h"

// DEFERRED[低] 数值: PI/DEG2RAD 只取 7–8 位有效数字且为无类型宏（PI 与 std::numbers::pi、DEG2RAD 与 PI/180 不完全一致），各模块混用会产生 1e-7 量级的不一致，名字 PI 也极易与其他库冲突；建议：改为 inline constexpr double kPi = std::numbers::pi 等带命名空间的常量。
#define PI 3.1415926
#define GROUP_SIZEXY 8 // Same group size as in compute shader
#define GROUP_SIZEX 64
#define TLASTNUM 8
#define SENSOR_HEIGHT 3000
#define SENSOR_FOV    0.5
#define ANGLE_COR 0.1
#define DEG2RAD  0.017453292
#define DIFFUSENUM 64
#define N1 2001
#define N2 161

using BufferT = nanovdb::HostBuffer;

// all binding index in addBindings


///--------------------------------------------------------------------------
/// ENUM VARIABLES
///--------------------------------------------------------------------------


enum class VoxelEBStage
{
    gap,
    directVNIR,
    directTIR,
    scatteringVNIR,
    diffuseTIR,
    bio,
    aero,
    evapo,
    budget,
    updateL,
    updateTp,
    fluidLbm,
    fluidCommit,
    out
};

enum class VoxelRTStage
{
    gap,
    diffuse,
    out
};

//enum class VoxelRTStage
//{
//    gap,
//    run,
//    outImage
//};


enum Queues
{
    eGCT,
    eCompute,
    eTransfer
};


enum Mode {
    eRaytracing,
    eVoxelEB,
    eVoxelRT,
    eFacetRT,
    eFacetEB,
};

enum RaytracingStageIndices
{
    eRaygen,
    eMiss,
    eMiss2,
    eClosestHit,
    eShaderGroupCount
};

enum class Projection
{
    PARALLAL,
    PERSPECTIVE
};

// This is used to determine the type variables
enum class Type
{
    NO,
    SOIL,
    VEGETATION,
    BUILDING,
    WATER,
    OTHER
};

// This is used to determine the spectral variables
enum class spectralType
{
    CUSTOM,  // ONE FOR ALL
    BSM,    // MODEL ONE FOR ONE
    PROSPECT,  // MODEL ONE FOR ONE
    OTHER     //  PATH ONE FOR ONE
};

enum class ShapeType
{
    CUBE,
    ELLIPSOID,
    PLANE
};

enum AeroType
{
    ONE,  // ONE for all
    image,  // ONE aerocond for the whole scene
    gridCal,   // different aerocond for each grid
};

///--------------------------------------------------------------------------
/// Coefficient and Varaible for fluspect and bsm models
///--------------------------------------------------------------------------

struct OptCoeff
{
//    float * nr_, *kdm_, *kab_, *kw_, *ks_, *phiI_, *phiII_;
    std::vector<float> wl_;
    std::vector<float> nr_;
    std::vector<float> kab_;
    std::vector<float> kca_;
    std::vector<float> kdm_;
    std::vector<float> ks_;
    std::vector<float> kw_;
    std::vector<float> phiI_;
    std::vector<float> phiII_;
    std::vector<float> kcaV_;
    std::vector<float> kcaV2_;
    std::vector<float> kcaZ_;
    std::vector<float> kcant_;
    std::vector<float> gsv1_;
    std::vector<float> gsv2_;
    std::vector<float> gsv3_;
    std::vector<float> nw_;
    std::vector<float> phi_;
};

// DEFERRED[中] 未初始化: FluspectParam 所有字段无默认值，若 JSON 缺省某项（如 Cs/N），fluspect 读到未初始化值（N 为垃圾时除零/NaN）；建议：给出 PROSPECT 常用默认值（Cab=40, Cw=0.009, Cdm=0.012, Cs=0, N=1.5）。
struct FluspectParam
{
    float Cab;
    float Cw;
    float Cdm;
    float Cs;
    float N;
};

struct BSMParam
{
    float SMC{25.0f};
    float BSMBrightness{0.5f};
    float BSMlat{25.0f};
    float BSMlon{45.0f};
};

///--------------------------------------------------------------------------
/// Different XML packages
///--------------------------------------------------------------------------




struct SpectralXml
{
    std::string spectralName;
    // Internal provenance: Mesh BRDF overrides share the source texture budget.
    std::string textureSourceName;
    // DEFERRED[中] 未初始化: type、tau_tir、refl_tir 无默认值；resolveSpectrals 依赖 type 分支、热红外波段直接使用 refl_tir/tau_tir（未初始化值若有限则会被当作真实值使用）；建议：type{spectralType::CUSTOM}、refl_tir{0.05f}、tau_tir{0.0f}。
    spectralType type;
    std::vector<float> reflectances;
    std::vector<float> transmittance;
    FluspectParam fp;
    BSMParam bsm;
    std::string path;
    float tau_tir;
    float refl_tir;
    // TIR reflectance at saturation for a soil (BSM) material; -1 keeps the
    // built-in dry-to-wet emissivity gain, see soil_tir_emissivity.h.
    float refl_tir_wet{-1.0f};
    bool specularEnabled{false};
    float specularFraction{0.5f};
    float specularRoughness{0.25f};
    bool physicalTextureEnabled{false};
    std::string physicalTexturePath;
    float physicalTextureStrength{0.0f};
    float physicalTextureRelativeStd{0.0f};
    float physicalTextureRepeatSize{2.0f};
};

struct ThermalXml
{
    std::string thermalName;
    float sunlitTemperature;
    float shadedTemperature;
};

struct CanopyXml
{
    std::string canopyName;
    Canopy canopy;
};


struct SensorXml
{
    std::string name;
    // DEFERRED[低] 未初始化: projection 与 resolution 无默认值，配置缺省时相机投影类型与图像尺寸为垃圾值（resolution 进入缓冲尺寸计算）；建议：projection{Projection::PERSPECTIVE}、resolution{512,512} 或在解析时强制必填。
    Projection projection;
    glm::vec2 resolution;			   // { WIDTH, HEIGHT }
    std::vector<glm::vec2> viewAngles; // { zenith, azimuth }
    glm::vec3 position{0.0f, 0.0f, 3000.0f}; // world XYZ: east, north, height (m)
    std::vector<float> waves;
    bool isImage{true};
    bool isProcess{false};
    bool isRadiationProcess{false};
    bool isEnergyProcess{false};
    bool isOrth{false};
    bool isAlbedo{false};
    bool isTemperature{true};
    bool isDisplay{false};

    std::vector<glm::vec3> uavPoses;
    std::vector<float> uavViewAzimuths;
    float sensorFov{60};
};



struct LightXml {
    std::string name;
    // DEFERRED[中] 未初始化: solarAngle/direct/diffuse 无默认值，配置缺省时太阳角与入射辐照为垃圾值且不报错；solarAngle 的单位（度/弧度）与分量顺序（天顶/方位）也未注明；建议：给出默认值并在注释中写明 {zenith, azimuth} 单位为度、direct/diffuse 单位为 W/m²。
    glm::vec2 solarAngle;
    float direct;
    float diffuse;
    float skyTemperature{250};
    float solarTemperature{6000};
};

struct AtmosphereXml
{
    bool enabled{false};
    std::string model{"midlatitude-summer"};
    float waterVapor{2.0f};
    std::string aerosol{"rural"};
    float visibility{23.0f};
    std::string lutFile;
};


struct SettingXml
{
  //  bool isSaveImg{true}; // 0: not save img but statistical result; 1: save img and statistical result
  //  bool isAlbedo{false}; // 0: only work for angle set; 1: additional, get albedo result
   // bool isDEM{false};	  // use DEM file or not
  // 		  // i: the ith GPU work, but not work currently
  //  std::string outDir;	  // the outDir to read input and save imagal and statistical results
  //  bool isDisplay{0};
  //  bool isTemperature{0};
    int theGPU{0};
    std::string computeBackend{"gpu"}; // gpu | cpu (Mesa lavapipe) | auto
    int n_sample{32};
    int maxDepth{5};
    int isUAVtrave{false};
    bool heterogeneousVoxel{false};
    int periodicNeighborCount{0};
    bool skyboxEnabled{false};
    bool acceleratedRadiationSolver{false};
    int spectralAccelerationWidth{100};
    int shortwaveScatteringOrders{1};
    // VoxelRT: explicit collision orders; higher orders use recollision theory.
    int voxelCollisionOrders{2};
    int radiationMaxSteps{64};
    // 0: Ball-Berry empirical coupling; 1: Farquhar mechanistic coupling.
    int vegetationTemperatureMethod{0};



//    bool isTemperature{false};
//    bool isDisplay{false};
//    bool isImage{false};
};




struct AeroCondXml
{
    AeroType aerotype;
    AeroCond aerocond; // ONE for all
    std::string path;
    float stepsize_atmosphere;
};

struct FluidXml
{
    bool enabled{false};
    float timeStep{5.0f};
    int pressureIterations{20}; // Legacy project field; ignored by D3Q19 LBM.
    float windDirection{0.0f};
    float buoyancy{0.033f};
    float dragCoefficient{0.3f};
    float thermalCoupling{1.0f};
    float diffusivity{0.1f};
    float smokeEmission{0.02f};
    float maxVelocity{30.0f};
    bool outputWind{false};
    bool outputAirTemperature{false};
    float outputHeight{2.0f};
    float voxelSize{1.0f};
    bool localExchange{false}; // leaf/soil exchange against local resolved or prescribed air
};

struct Background
{
    TerrainEnvironment terrainEnvironment;
    // scene information
    // DEFERRED[中] 未初始化: sceneSize/sceneOrigin（注释写有默认值但未实际初始化）、stepsize_surface/stepsize_height、lat/lon 均无初始值；stepsize 为 0/垃圾时体元化按 sceneSize/stepsize 计算格数会除零或得到巨大分配；建议：按注释给出默认值并在解析后校验 stepsize>0。
    glm::vec3 sceneSize;       // default:  (100,10,100)
    glm::vec3 sceneOrigin;     // default : (0,0,0)
    Type type{Type::SOIL};

/*    glm::vec3 sMin{ -5,0,-5 };
    glm::vec3 sMax{ 5,5,5 };*/
    float stepsize_surface;
    float stepsize_height;
    float voxelFillThreshold{0.05f};
    // for dem option
    bool isDEM{false};
    std::string DEMFile;

    std::string bgName{"Bg"};
    std::string bgSpectralName; // SPECTRAL
    std::string bgThermalName; // TEMPERTURE
    std::string bgPropName; // BSM
    // Unified 0..1 strength for background optical and thermal Hapke effects.
    float angularEffectStrength{0.0f};
  //  std::string bgAeroName;

    // this is used for a group background
    std::string bgFileName;
    std::vector<std::string> bgNames;
    std::vector<std::string> bgSpectralNames;
    std::vector<std::string> bgThermalNames;
    std::vector<std::string> bgPropNames;

    float lat; // this is used for solar angle
    float lon; // this is used for solar angle
    int year{2019};       // calendar year of meteo day-of-year values
    float timezone{8.0f}; // hours east of UTC for meteo timestamps

    bool isLad{false};
    std::string ladfile;


};

struct MeteoMeta
{
    // DEFERRED[中] 未初始化/单位: t、RIn、Rli、u、p 及 start/end 年份与 DOY 无默认值；Tsold{25} 为摄氏度而全局其它温度（Thermal 300 K、skyTemperature 250）为开尔文，ea{15} 为 hPa、p 单位未注明；建议：全部给默认值并在字段旁标注单位，最好统一温度单位为 K。
    float t;
    float Oa{209};
    float Ca{380};
    float ea{15} ;
    float dTime{1800};
    float z{15};
    float RIn;
    float Rli;
    float Tsold{25};
    float SatWater{0.45};
    float sm{0.25};
    float u;
    float p;
    int startYear;
    int endYear;
    int startDoy;
    int endDoy;

};

struct ObjEntity
{
    // here vector for different mesh in the object
    std::string objName;
    std::string filePath;
    std::vector<std::string> meshNames;
    std::vector<Type> types;
    std::vector<std::string> spectralNames;
    std::vector<std::string> thermalNames;
    std::vector<glm::vec3> objDistributions;
    std::vector<float> scales;
    std::vector<float> rotations;

    bool isFromFile{false};
    std::string file;

};

struct Shape
{
    // DEFERRED[低] 未初始化: shapetype、height/width/length、pos 无默认值，配置缺省形状参数时体元化范围为垃圾值；PrimEntity::type、AeroCondXml::aerotype/stepsize_atmosphere、PropertyXml::type、ThermalXml 温度同样未初始化；建议：统一给出默认值。
    ShapeType shapetype;
    float height;
    float width;
    float length;
    glm::vec3 pos;
};

struct PrimEntity
{
    /// for crown the number of vector is 1
    /// for building the number of vector is 2
    std::string primitiveName;
    std::vector<std::string> meshNames;  // delete
    std::vector<std::string> spectralNames;
    std::vector<std::string> thermalNames;
    std::vector<std::string> canopyNames;
    std::vector<std::string> propNames;

    Type      type; // vegetation or building
    Shape     shape;

    // using a default shape of a series of shape
    bool isshapeFromFile{false};
    std::string shapefile;

    /// if height then go
    /// else using distributions (or file)

    bool isheightFromFile{false};
    std::string heightfile;

    /// if using distributeion file
    /// else using distributions
    bool isdisFromFile{false};
    std::string distributefile;
    std::vector<glm::vec3> primDistributions{glm::vec3(0,0,0)};
    std::vector<float> scales{1};
    std::vector<float> rotations{0};

    bool voxelizeFromObj{false};
    std::string objFile;
    float voxelFillThreshold{0.05f};
    // true: turbid voxels take their leaf area density from the OBJ facet
    // area inside each voxel instead of the canopy density parameter.
    bool leafDensityFromGeometry{false};
    // A vegetation OBJ may contain both foliage and woody meshes.
    std::vector<Type> types;
};


struct Aabb
{
    glm::vec3 minimum;
    glm::vec3 maximum;
};

struct VoxelModel  // <-- Cube, Ellipsoid
{
    int modelId;
    std::vector<Aabb> aabbs;
    std::vector<glm::ivec3> centerPoints;
};

struct Angle{
    float vza{0};
    float vaa{0};
    float sza{0};
    float saa{0};
};

struct SceneXml
{



    //float stepSize;		       // DEM Sampling
    Background background;

    // Scene Component Information
    std::vector<ObjEntity> objEntities;
    std::vector<PrimEntity> primEntities;



    //glm::vec2 demResolution;   // to DEM resolution if(-1) change nothing;
};


// DEFERRED[低] 未初始化: ObjMesh/PrimMesh 的 meshId、nIndices、nVertices、meshcenter 无默认值，且 nIndices/nVertices 与 vectors.size() 冗余，两者不同步时 BLAS 构建按错误数量读取；建议：给默认值，或去掉冗余计数改用 size()。
struct ObjMesh
{
    int meshId;
    uint32_t nIndices;
    uint32_t nVertices;
    std::vector<VertexAttribute> vertices;
    std::vector<uint32_t>        indices;
};

struct int5
{
    int values[5]{0,0,0,0,0};
};

struct PrimMesh
{
    int meshId;
    uint32_t nIndices;
    uint32_t nVertices;
    std::vector<VertexAttribute> vertices;
    std::vector<uint32_t>        indices;
    std::vector<glm::vec3> voxelIds;
    glm::vec3 meshcenter;
    std::vector<int> faceIds; //would be remove in the future;
    std::vector<int5> isValids;
};



struct RaytracingXml
{
    std::string projectDir;
    std::string definedDir;
    // system setting
    SettingXml settingxml;
    // run setting
    LightXml lightxml;  // for solar angle
    SensorXml sensorxml; // for viewing angle
    AtmosphereXml atmospherexml;

    SceneXml scenexml;
    // component materials
    std::vector<SpectralXml> spectralxmls;
    std::vector<ThermalXml> thermalxmls;

};

struct PropertyXml{
    std::string name;
    Type type;
    LeafBio leafbio;
    SoilSet soilset;
    WaterSet waterset;
    BuildUp buildup;
};


struct MeteoXml
{
    MeteoMeta meta;
    std::string meteofile;
    //std::vector<Meteo> meteos;
    int startTimeNode{0};
    int endTimeNode{1};
};

struct AtomCondXml
{
    std::string rinfile;
    std::string rlifile;
};


struct  VoxelEBXml
{
    // main IO part
    std::string projectDir;
    // define is a inside directory
    std::string definedDir;
    // system setting
    SettingXml settingxml;

    // run setting
    LightXml lightxml;  // for solar angle
    SensorXml sensorxml; // for viewing angle
    AtmosphereXml atmospherexml;


    // Scene structural, i.e., background and its result;
    SceneXml scenexml;

    // component materials
    std::vector<SpectralXml> spectralxmls;
    std::vector<ThermalXml> thermalxmls;
    std::vector<PropertyXml> propxmls;
    std::vector<CanopyXml> canopyxmls;

    MeteoXml meteoxml;
    AeroCondXml aerocondxml;
    AtomCondXml atomcondxml;
    FluidXml fluidxml;


};

struct  VoxelRTXml
{
    // main IO part
    std::string projectDir;
    // define is a inside directory
    std::string definedDir;
    // system setting
    SettingXml settingxml;

    // run setting
    LightXml lightxml;  // for solar angle
    SensorXml sensorxml; // for viewing angle
    AtmosphereXml atmospherexml;


    // Scene structural, i.e., background and its result;
    SceneXml scenexml;

    // component materials
    std::vector<SpectralXml> spectralxmls;
    std::vector<ThermalXml> thermalxmls;
    std::vector<PropertyXml> propxmls;
    std::vector<CanopyXml> canopyxmls;

    MeteoXml meteoxml;
    AeroCondXml aerocondxml;
    AtomCondXml atomcondxml;


};





#endif //FIELD_STRUCTS_H
