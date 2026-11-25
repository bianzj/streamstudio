// ==========================================================================
// 文件：structs.h
// 功能：CPU 侧（主机端）通用数据结构、枚举和全局常量宏：工程配置（XML/JSON 解析结果）、
//       场景实体与网格的描述。GPU 侧共用的结构见 structs_cg.h，着色器常量见 shader/global.glsl。
// 主要模块：
//   1. 全局常量宏（PI、工作组大小、土壤历史节点数、传感器默认值、波长点数 N1/N2 等）
//   2. 枚举：各模式的计算阶段、队列、模式、投影、材质类型、光谱类型、形状类型、气动条件类型
//   3. 光谱模型参数：OptCoeff（PROSPECT/BSM 系数表）、FluspectParam、BSMParam
//   4. 配置片段：SpectralXml / ThermalXml / CanopyXml / SensorXml / LightXml / AtmosphereXml /
//      SettingXml / AeroCondXml / FluidXml / Background / MeteoMeta / MeteoXml 等
//   5. 场景实体：ObjEntity（OBJ 模型）、PrimEntity（基元：树冠/建筑/OBJ 体素化）、Shape
//   6. 网格：ObjMesh、PrimMesh（含体元坐标 voxelIds 与面有效标志）
//   7. 各模式的工程根结构：RaytracingXml、VoxelEBXml、VoxelRTXml
// 数据流：fileio.cpp / fileio_json.cpp 解析工程文件 → 填充本文件的结构 → scene.cpp、compo.cpp
//         等据此生成网格/材质 → 各模式 IO 对象 → GPU。
// 约定：长度 m；角度除特别注明均为度；温度 K 为主（个别字段为 °C，见字段旁注释）；
//       世界坐标 XYZ 为 东/北/高；体元模式内部使用 XZY（第二轴为高度）。
// 注意：部分字段无默认值（见文中 DEFERRED 标注），依赖解析代码一定赋值。
// ==========================================================================

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
// 全局常量宏（无类型，C++ 与着色器侧需保持一致的项见各自注释）。
// PI / DEG2RAD 为较低精度的近似值（约 7 位有效数字）。
#define PI 3.1415926
// GROUP_SIZEXY：二维计算着色器工作组边长（按像素分派）；GROUP_SIZEX：一维工作组大小（按体元/面元分派）。
#define GROUP_SIZEXY 8 // Same group size as in compute shader
#define GROUP_SIZEX 64
// TLASTNUM：土壤/材料温度历史的显式节点数（每体元存 8 个，下标 0 为最新），与 shader/global.glsl 一致。
#define TLASTNUM 8
// Sum-of-exponentials modes of the soil method-1 history beyond TLASTNUM nodes
// (SoilSet.inertiaHistory = 1); keep equal to shader/global.glsl.
#define SOIL_MEMORY_MODES 12
// 传感器默认几何：SENSOR_HEIGHT 为相机距场景原点的距离（场景单位），SENSOR_FOV 为窄视场角（正射相机用，单位待核对），
// ANGLE_COR 为避开 0°/45° 等特殊角度的偏移量（度），见 geometry.cpp。
#define SENSOR_HEIGHT 3000
#define SENSOR_FOV    0.5
#define ANGLE_COR 0.1
#define DEG2RAD  0.017453292
// DIFFUSENUM：漫射辐射场每体元的离散入射方向数。
#define DIFFUSENUM 64
// 波长点数：N1 = 短波点数（400 nm 起每 1 nm 一点，共 2001 个，PROSPECT/BSM 系数表同此），
// N2 = 热红外点数；二者连续存放，总计 N1 + N2 个点。
#define N1 2001
#define N2 161

// NanoVDB 网格使用的主机缓冲类型。
using BufferT = nanovdb::HostBuffer;

// all binding index in addBindings


///--------------------------------------------------------------------------
/// ENUM VARIABLES
///--------------------------------------------------------------------------


// 枚举：VoxelEB 计算阶段（大致按流水线顺序）：直射可见光近红外/热红外、可见光近红外散射、热红外漫射、
// 生化、空气动力、蒸散、能量收支、更新叶温/温度历史、流体（LBM）、输出等；gap 的含义待核对。
// 各阶段的具体含义见 voxeleb 目录下对应着色器。
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
    out,
    scatteringTrace
};

// 枚举：VoxelRT 的阶段（间隙 / 漫射 / 输出）。
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


// Vulkan 队列类别：图形+计算+传输、计算、传输。
enum Queues
{
    eGCT,
    eCompute,
    eTransfer
};


// 枚举：引擎模式（RayTracing、VoxelEB、VoxelRT、FacetRT、FacetEB）。
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

// 相机投影：PARALLAL（正射/平行，拼写沿用原代码）或 PERSPECTIVE（透视）。
enum class Projection
{
    PARALLAL,
    PERSPECTIVE
};

// 枚举：材质/实体的大类（土壤、植被、建筑、水体等），决定使用哪类参数集（SoilSet/LeafBio/WaterSet/BuildUp）。
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

// 基元形状类型：长方体、椭球、平面（形状文件第一列取 0/1/2 对应此顺序）。
enum class ShapeType
{
    CUBE,
    ELLIPSOID,
    PLANE
};

// 气动条件来源：全场一组、由图像给定、逐格计算。
enum AeroType
{
    ONE,  // ONE for all
    image,  // ONE aerocond for the whole scene
    gridCal,   // different aerocond for each grid
};

///--------------------------------------------------------------------------
/// Coefficient and Varaible for fluspect and bsm models
///--------------------------------------------------------------------------

// 结构：PROSPECT/Fluspect 与 BSM 土壤模型的光学系数表（按波长逐点，各向量长度应 >= N1，
// 由 defined.h 读入、compo.cpp 使用）；wl_ 为波长，其余为折射率、各成分吸收系数等（待核对各列物理含义）。
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
// 结构：Fluspect/PROSPECT 叶片参数：Cab 叶绿素、Cw 等效水厚度、Cdm 干物质、Cs 棕色素（待核对）、N 叶结构参数。
struct FluspectParam
{
    float Cab;
    float Cw;
    float Cdm;
    float Cs;
    float N;
};

// 结构：BSM 土壤光谱模型参数：SMC 土壤含水量（缺省 25，按百分数写法；soil_tir_emissivity.h 同时兼容百分数与体积分数）、
// BSMBrightness 亮度、BSMlat/BSMlon 光谱角参数（待核对）。
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




// 结构：一种光谱材质（spectralName）：类型（自定义/BSM/PROSPECT/外部文件）、可见光近红外反射/透射、
// 热红外反射/透射（refl_tir/tau_tir，refl_tir_wet 为饱和土壤的热红外反射率）、镜面反射和物理纹理参数。
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

// 结构：固定温度材质：向阳面与背阴面的温度（RayTracing/VoxelRT 直接使用）。
struct ThermalXml
{
    std::string thermalName;
    float sunlitTemperature;
    float shadedTemperature;
};

// 结构：冠层参数（名称 + Canopy，见 structs_cg.h）。
struct CanopyXml
{
    std::string canopyName;
    Canopy canopy;
};


// 结构：传感器配置：投影、分辨率、观测角（天顶、方位，度）、位置（世界 XYZ，m）、波段、
// 输出开关（图像/过程/辐射/能量/正射/反照率/温度）以及无人机轨迹（uavPoses 等）。
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



// 结构：光照配置：太阳角、直射/散射辐照、天空与太阳等效温度、总短波（RayTracing/VoxelRT 绝对太阳项）、
// 以及 VoxelEB/FacetEB 的直射/散射拆分模式（见 shortwave_split.h）。
struct LightXml {
    std::string name;
    // DEFERRED[中] 未初始化: solarAngle/direct/diffuse 无默认值，配置缺省时太阳角与入射辐照为垃圾值且不报错；solarAngle 的单位（度/弧度）与分量顺序（天顶/方位）也未注明；建议：给出默认值并在注释中写明 {zenith, azimuth} 单位为度、direct/diffuse 单位为 W/m²。
    glm::vec2 solarAngle;
    float direct;
    float diffuse;
    float skyTemperature{250};
    float solarTemperature{6000};
    // Horizontal global shortwave irradiance (W m-2) for the absolute 2.5-6 um
    // solar term of RayTracing/VoxelRT; <= 0 = clear-sky (Haurwitz) default.
    float shortwaveIrradiance{0};
    // VoxelEB/FacetEB direct/diffuse split of the meteorological shortwave
    // (light.diffuseFractionMode): 0 = fixed directScatteringRatio for every
    // node (legacy default), 1 = per node from measured direct/diffuse
    // columns or else the Erbs (1982) clearness-index model.
    int diffuseFractionMode{0};
};

// 结构：大气配置（是否启用、模式大气、水汽、气溶胶类型、能见度、LUT 文件）。
struct AtmosphereXml
{
    bool enabled{false};
    std::string model{"midlatitude-summer"};
    float waterVapor{2.0f};
    std::string aerosol{"rural"};
    float visibility{23.0f};
    std::string lutFile;
};


// 结构：运行设置：GPU 序号与计算后端（gpu/cpu/auto）、采样数与最大深度、体元异质/周期邻域、
// 天空盒、辐射加速求解器、散射阶数、植被温度方法（0 Ball-Berry / 1 Farquhar）、稳定度修正等。
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
    // VoxelEB Monin-Obukhov stability iteration (control.stabilityCorrection).
    bool stabilityCorrection{false};



//    bool isTemperature{false};
//    bool isDisplay{false};
//    bool isImage{false};
};




// 结构：气动条件输入（类型、气动条件值、文件路径、大气层步长）。
struct AeroCondXml
{
    AeroType aerotype;
    AeroCond aerocond; // ONE for all
    std::string path;
    float stepsize_atmosphere;
};

// 结构：VoxelEB 流体（D3Q19 格子玻尔兹曼，LBM）配置：时间步、浮力/拖曳/热耦合系数、最大速度、
// 输出选项等；pressureIterations 为旧工程字段，已被忽略。
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

// 结构：背景（地面/水面）与场景全局配置：场景尺寸（m，XYZ 长/宽/高）与原点、水平/垂直步长、
// DEM/LAD 文件、背景材质绑定名、经纬度与年份时区（供太阳角和气象时间换算）。
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

// 结构：气象元数据与缺省值（气温 t、Oa/Ca 大气氧/二氧化碳浓度、水汽压 ea、时间步 dTime(s)、
// 参考高度 z、Rin/Rli 入射短波/长波、风速 u、气压 p、起止年和年积日等）；单位见 DEFERRED 说明。
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

// 结构：OBJ 模型实体（RayTracing）：文件、子网格名及其光谱/热属性绑定、放置点/缩放/旋转（或位置文件）。
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

// 结构：基元形状：类型、高/宽/长（m）和位置 pos（m）。
struct Shape
{
    // DEFERRED[低] 未初始化: shapetype、height/width/length、pos 无默认值，配置缺省形状参数时体元化范围为垃圾值；PrimEntity::type、AeroCondXml::aerotype/stepsize_atmosphere、PropertyXml::type、ThermalXml 温度同样未初始化；建议：统一给出默认值。
    ShapeType shapetype;
    float height;
    float width;
    float length;
    glm::vec3 pos;
};

// 结构：基元实体（VoxelEB/VoxelRT）：树冠为 1 个绑定，建筑为 2 个（墙、屋顶）；形状可来自单个 shape、
// 形状文件或高度栅格；放置来自 primDistributions 或位置文件；voxelizeFromObj 时由 OBJ 三角网体素化。
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


// 轴对齐包围盒（最小/最大角点）。
struct Aabb
{
    glm::vec3 minimum;
    glm::vec3 maximum;
};

// 体元模型（立方体/椭球）：包围盒与体元中心点（当前文件内的使用情况待核对）。
struct VoxelModel  // <-- Cube, Ellipsoid
{
    int modelId;
    std::vector<Aabb> aabbs;
    std::vector<glm::ivec3> centerPoints;
};

// 观测/太阳角度组：vza/vaa 为观测天顶/方位，sza/saa 为太阳天顶/方位（度）。
struct Angle{
    float vza{0};
    float vaa{0};
    float sza{0};
    float saa{0};
};

// 结构：场景配置：背景 + OBJ 实体列表 + 基元实体列表。
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
// 结构：OBJ 三角网（顶点、索引）；nIndices / nVertices 与向量长度应一致。
struct ObjMesh
{
    int meshId;
    uint32_t nIndices;
    uint32_t nVertices;
    std::vector<VertexAttribute> vertices;
    std::vector<uint32_t>        indices;
};

// 5 个整数的小数组：对建筑体元表示 4 个墙面 + 1 个屋顶面是否有效（见 scene.cpp）。
struct int5
{
    int values[5]{0,0,0,0,0};
};

// 结构：基元网格：三角网 + 体元整数坐标 voxelIds（XZY 轴序）、faceIds，isValids 为逐体元 5 个面的有效标志。
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



// 各模式的工程根结构：RaytracingXml（RayTracing），下面是 VoxelEBXml 和 VoxelRTXml。
// RayTracing 无气象/气动/流体输入。
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

// 结构：材料参数集：按 type 选用叶生化（LeafBio）、土壤（SoilSet）、水体（WaterSet）或建筑（BuildUp）。
struct PropertyXml{
    std::string name;
    Type type;
    LeafBio leafbio;
    SoilSet soilset;
    WaterSet waterset;
    BuildUp buildup;
};


// 结构：气象输入：元数据、气象文件路径、起止时间节点。
struct MeteoXml
{
    MeteoMeta meta;
    std::string meteofile;
    //std::vector<Meteo> meteos;
    int startTimeNode{0};
    int endTimeNode{1};
};

// 结构：入射光谱文件：rinfile 为太阳直射光谱 Esun、rlifile 为天空散射光谱 Esky（见 fileio.cpp）。
struct AtomCondXml
{
    std::string rinfile;
    std::string rlifile;
};


// VoxelEB 工程根结构：系统/光照/传感器/大气设置、场景、材质（光谱、热、属性、冠层）、气象、气动、入射光谱、流体。
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

// VoxelRT 工程根结构：与 VoxelEBXml 相同但没有流体（fluidxml）。
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
