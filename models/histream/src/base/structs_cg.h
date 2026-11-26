// ==========================================================================
// 文件：structs_cg.h
// 功能：CPU 与 GPU（着色器）共用的内核数据结构：传感器、光照、材质/冠层/土壤/水体参数、
//       体元状态记录、推送常量与流体网格参数；部分结构须与 GLSL（shader/parameters.h 等）逐字节一致。
// 主要模块：
//   1. 传感器与光照：SensorMatrix、LightSet、Spectral、Thermal
//   2. 各模式推送常量：RayRTSetting（RayTracing）、VoxelLstSetting（VoxelEB）、VoxelRTSetting（VoxelRT）
//   3. 散射/太阳可见性诊断记录：ScatteringOrderStats、SolarVisibilityStats
//   4. 空气动力与冠层参数：AtomCond、AeroCond、Canopy
//   5. 几何与链接：Instance、VertexAttribute、MeshBuffer、MeshLink、VoxelLink、VoxelHex
//   6. 体元状态量：VoxelDir/Rad/Tempe/NetRad/Pnet/Raa/Rss/Air/Heatflux、TLAST、EBState
//   7. 流体耦合：FluidCellMeta、FluidParameters
//   8. 生化、土壤、水体、建筑与气象：LeafBio、SoilSet、WaterSet、BuildUp、Meteo
// 数据流：fileio/fileio_json 解析场景 → 填充这些结构 → 以缓冲或推送常量上传 → 着色器读写；
//         GPU 结果按相同结构读回供 CPU 后处理与输出。
// 关系：被 structs.h、各模式 IO（raytracingio.h、voxelebio.h、voxelrtio.h）及 virtualio.h 包含。
// 约定：带 static_assert 的结构（RayRTSetting、VoxelLstSetting、VoxelLink、VoxelHex、VoxelTempe、
//       VoxelAir、EBState、FluidParameters 等）的字节偏移/大小与 GLSL 对齐，修改字段需同步着色器；
//       温度 K、热通量 W m-2、阻抗 s m-1（各字段另有注明时以字段为准）。
// ==========================================================================

//
// Created by admin on 2024/1/25.
// For the kernel struct used both in the cpu and gpu part;
//

#ifndef FIELD_STRUCTS_CG_H
#define FIELD_STRUCTS_CG_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <string>
#include <vector>
#include <map>

#include "nvvk/resourceallocator_vk.hpp"




// 模块：传感器/相机参数，CPU 与 GPU 共用的内核结构。
// viewInverse / projInverse 为视图、投影矩阵的逆（用于由像素生成射线）；
// focalDist、aperture 为焦距与光圈（景深相关，待核对）；n_wave 为波段数；direction 的含义待核对。
struct alignas(16) SensorMatrix
{
    glm::mat4 viewInverse;
    glm::mat4 projInverse;
    float focalDist;
    float aperture;
    int n_wave{ 0 };
    int empty_{ 0 };

    glm::vec3 direction;
};

// 模块：光照设置（太阳与天空），CPU/GPU 共用。
// direction 为太阳方向；direct/diffuse 为直射/散射分量比例；Rin/Rli 为入射短波/长波
// （W m-2，见 structs.h 的 Meteo 同名字段）；skyTemperature、solarTemperature 单位 K。
struct alignas(16) LightSet
{
    glm::vec3 direction{ 100, 100, 0 };
    float direct{ 0.9 };
    float diffuse{ 0.1 };
    float Rin{600};
    float Rli{450};
    // DEFERRED[低] 可维护性: 默认 skyTemperature 为 150.1 K，而 LightXml 默认 250 K、atmosphereSkyOutputValue 默认 250 K；direction{100,100,0} 也未归一化且水平（太阳在地平线上）；若某路径未覆盖默认值，天空长波与太阳方向会明显偏离；建议：统一默认天空温度，并给出归一化的合理默认方向。
    float skyTemperature{ 150.1 };
    float solarTemperature{ 6000.0 };

};

// 模块：材质光谱属性：反射率、透射率，以及镜面反射比例与粗糙度（缺省 0 与 0.25）。
struct  Spectral{
    float reflectance;
    float transmittance;
    float specularFraction{0.0f};
    float specularRoughness{0.25f};
};

// 模块：材质热状态：向阳面与背阴面温度（单位 K，待核对）。
struct Thermal{
    float sunlitTemperature;
    float shadedTemperature;
};

// 模块：RayTracing 模式的推送常量/设置，布局须与 GLSL 一致（见下方 static_assert 偏移）。
// imageSize 来自传感器分辨率；band1..band3 为输出显示所用波段；sceneSize 为场景水平尺寸；
// periodicNeighborCount 为周期边界的邻域复制数；sceneTop 见字段注释。
struct alignas(16) RayRTSetting
{
    int frame{ 0 };
    int maxDepth{ 20 };
    int n_sample{ 32 };//64
    int n_wave{ 3 };

    glm::ivec2 imageSize{1000, 1000 }; // ->Sensor.resolution
    //nvmath::vec2f sceneSize_XYZ{ 100.0, 100.0 };
    int adaptiveMaxExtra{ 0 }; // adaptive sampling: per-pixel cap of extra samples (was unused band1)
    int adaptiveTile{ 1 }; // adaptive sampling: tile side for the sigma average (was unused band2)
    int adaptiveMinCount{ 0 }; // adaptive sampling pass 2: per-pixel minimum sample count (was unused band3)
    int isDisplay{ 0 };
    int isTemperature{ 0 };
    float adaptiveScale{ 0 }; // adaptive sampling: extra samples per unit of pilot sigma (was unused fireflyClampThreshold)
    int adaptivePass{ 0 }; // 0 normal, 1 adaptive pilot, 2 adaptive refinement (was unused debugging_mode)
    int periodicNeighborCount{ 0 };
    int skyboxEnabled{ 0 };
    int imageOffsetY{ 0 };
    glm::vec2 sceneSize{ 1.0f, 1.0f };
    // Highest world y of the geometry plus a margin; periodic traversal stops
    // for rays above it that do not descend (offline.rgen).
    float sceneTop{ 1.0e30f };
};
static_assert(offsetof(RayRTSetting, sceneSize) == 64);
static_assert(offsetof(RayRTSetting, sceneTop) == 72);
static_assert(sizeof(RayRTSetting) == 80);


// 模块：VoxelEB（体元能量平衡）的推送常量，64 位地址字段用于绕过存储缓冲描述符大小限制。
// 布局须与 GLSL 推送常量一致，偏移与总大小由下方 static_assert 固定为 128 字节。
// n_jump 为光谱加速宽度（voxeleb.cpp 中取自 spectralAccelerationWidth）。
struct alignas(32)  VoxelLstSetting
{

    int frame{0};
    // Was the unused `maxIteration`. Bit flags of the voxel transport options
    // (VOXEL_FLAG_* in shader/voxel_flags.glsl, voxelflags.h on the host).
    int voxelFlags{0};
    int maxDepth{32};
    int maxStep{64};
    int n_sample{16};
    int n_wave{5};
    int n_jump{10};
    float scale{1};
    glm::ivec2 imageSize{100, 100};  // imageSize and VoxelSize do not put together
    int isDisplay{0};
    int isface{80};
    glm::ivec3 voxelSize{50, 0, 50};
    int islad{0};
    int voxelCount{0};
    int periodicNeighborCount{0};
    int skyboxEnabled{0};
    // City scenes exceed Vulkan's storage-buffer descriptor range.
    uint64_t radianceAddress{0};
    int scatteringBandStart{0};
    int scatteringBandEnd{0};
    int scatteringOrder{0};
    int scatteringStage{0};
    uint64_t scatteringStatsAddress{0};
    // Evaluate final fluxes/residuals without another temperature update.
    int energyFinalize{0};
    // Number of explicit shortwave scattering orders of the current run.
    int scatteringOrders{1};
    // Energy-balance iteration within the current time node, 0-based.
    int energyIteration{0};
    // 1 when the radiance buffer also holds the per-voxel, per-sensor-band
    // multiple-scattering environment of the optical image
    // (shader/voxeleb/image_environment.glsl).
    int imageEnvironment{0};
};

static_assert(offsetof(VoxelLstSetting, radianceAddress) == 80,
              "VoxelLstSetting must match the GLSL push-constant layout");
static_assert(offsetof(VoxelLstSetting, scatteringBandStart) == 88);
static_assert(offsetof(VoxelLstSetting, scatteringStatsAddress) == 104);
static_assert(offsetof(VoxelLstSetting, energyFinalize) == 112);
static_assert(offsetof(VoxelLstSetting, scatteringOrders) == 116);
static_assert(offsetof(VoxelLstSetting, energyIteration) == 120);
static_assert(offsetof(VoxelLstSetting, imageEnvironment) == 124);
static_assert(sizeof(VoxelLstSetting) == 128);

// 模块：多次散射统计记录（每个散射阶一条，32 字节），CPU 读回用于诊断。
// Three scattering orders plus the unscattered sky; matches scatter_stats.glsl.
struct ScatteringOrderStats {
    uint32_t maxStepTruncations{0};
    uint32_t transmittanceStops{0};
    uint32_t tracedRays{0};
    uint32_t reserved{0};
    float absorbedShortwave{0};
    float absorbedPar{0};
    float transmittanceResidual{0};
    float truncatedResidual{0};
};
static_assert(sizeof(ScatteringOrderStats) == 32);

// 模块：太阳可见性（间隙阶段）诊断记录，紧接 ScatteringOrderStats 之后存放。
// kSolarVisibilityStatsOffset 为其在统计缓冲中的字节偏移（4 条 ScatteringOrderStats 之后）。
// Solar-visibility diagnostics of the VoxelEB gap stage, stored right after
// the four ScatteringOrderStats; matches SolarVisibilityStatsRecord in
// scatter_stats.glsl. *Max fields are float bit patterns.
struct SolarVisibilityStats {
    uint32_t tracedRays{0};
    uint32_t truncations{0};
    uint32_t thresholdStops{0};
    uint32_t legacyTruncations{0};
    float truncatedResidual{0};
    uint32_t truncatedResidualMax{0};
    float thresholdResidual{0};
    uint32_t thresholdResidualMax{0};
    float legacyExcess{0};
    uint32_t legacyExcessMax{0};
    uint32_t maxStepsUsed{0};
    uint32_t stepLimit{0};
    uint32_t reserved[4]{};
};
static_assert(sizeof(SolarVisibilityStats) == 64);
constexpr uint64_t kSolarVisibilityStatsOffset = 4 * sizeof(ScatteringOrderStats);

// 模块：VoxelRT（体元辐射）模式的推送常量，字段含义与 VoxelLstSetting 的同名项相同；
// collisionOrders/collisionOrder 分别为总的与当前的碰撞（散射）阶数（待核对）。
// DEFERRED[低] 可维护性: VoxelRTSetting 同样作为推送常量与 GLSL 共享布局，却没有像 VoxelLstSetting 那样的 offsetof/sizeof static_assert，字段增删时 C++/GLSL 布局会悄然错位；建议：补充同样的布局断言。
struct alignas(32)  VoxelRTSetting
{

    int frame{0};
    int maxIteration{3};
    int maxDepth{16};
    int maxStep{64};
    int n_sample{16};
    int n_wave{5};
    int n_jump{10};
    float scale{1};
    glm::ivec2 imageSize{100, 100};  // imageSize and VoxelSize do not put together
    int isDisplay{0};
    int isface{80};
    glm::ivec3 voxelSize{50, 0, 50};
    int islad{0};
    int isTemperature{0};
    int voxelCount{0};
    int spectralBatchSize{1};
    int periodicNeighborCount{0};
    int skyboxEnabled{0};
    int collisionOrders{2};
    int collisionOrder{1};
    // Bit flags of the voxel transport options (VOXEL_FLAG_* in
    // shader/voxel_flags.glsl, voxelflags.h on the host). Appended, so the
    // offsets of the existing fields do not change.
    int voxelFlags{0};
};
static_assert(sizeof(VoxelRTSetting) <= 128, "VoxelRTSetting must fit the 128-byte push-constant range");

// 模块：大气辐射传输结果（与 MODTRAN 衔接）：按波长 wavelength 给出直射与散射辐照分量。
// atmospheric radiative transfer linked to modtrain
struct AtomCond
{
    float wavelength{0};
    float direct{0};
    float diffuse{0};
};

// 模块：空气动力学阻抗所需的状态/参数（植被 type=0，建筑 type=1）。
// L（应为 Monin-Obukhov 长度，待核对）与 ustar（摩擦速度）为迭代更新量，其余为高度、LAI、叶宽、覆盖度。
// aerodynamic condition for resistance
// DEFERRED[中] 未初始化: AeroCond/Canopy/LeafBio/SoilSet/WaterSet/BuildUp/Meteo 以及 Spectral、Thermal 的大部分物理字段无默认值；JSON 缺省某一项时（如 Canopy::LIDFa、LeafBio::Tparam、SoilSet::lambdas）上传到 GPU 的是栈上垃圾值，可能产生 NaN 或除零（如 leafwidth/hc 为 0 时空气动力学阻抗）；建议：给出物理合理的默认值（参照 SCOPE 默认参数），解析层对必填项显式报错。
struct AeroCond
{
    int type;  // 0 for vegtation, 1 for buildup
    float L;       // this is used for updating
    float ustar;   // this is used for updating
    float hc_veg;  // effective height
    float hc_build; // effective height
    float lai; // this is only used for the aeroy
    float leafwidth;
    float cover; // ???
};

// 模块：冠层/介质结构参数，CPU/GPU 共用，须与 shader/parameters.h 的 Canopy 保持一致。
// structureType：0 冠层、1 刚体、2 火/烟、3 雾；其后为介质的消光、散射反照率、不对称因子、发射比例。
// lidf[13] 为 SAIL 叶倾角分布频率，diffuseG[32] 为散射积分用的 G 函数（见字段注释）。
struct Canopy
{
    float lai;  // this is used for the aerodynamic resistance inside a system
    float density; // gap property
    float height;  // this is used for the aerodynamic resistance inside a system
    float width;  // ???
    float G;      // user projection coefficient; transport uses it only when lidfValid == 0
    float LIDFa;  // Verhoef/SAIL leaf inclination parameter a
    float LIDFb;  // Verhoef/SAIL leaf inclination parameter b
    float hspot;      // for the hotspot
    float leafwidth; // for the aerodynamic resistance
    int structureType{0}; // 0: canopy, 1: rigid, 2: fire/smoke, 3: fog
    float extinction{0.0f};
    float scatteringAlbedo{0.0f};
    float asymmetry{0.0f};
    float emissionScale{0.0f};
    float fixedTemperature{0.0f};
    // Host-derived from LIDFa/LIDFb (shader/lidf_common.h): 1 when lidf holds
    // the 13 SAIL class frequencies, which then define G(Omega) and Gamma.
    // 0 (rigid, media, LIDFa = LIDFb = 0, LIDFa > 1 = exact spherical with
    // G = 0.5, invalid LIDF): constant G with a spherical-shaped Gamma scaled
    // by G/0.5. Keep in sync with Canopy in shader/parameters.h.
    int lidfValid{0};
    float lidf[13]{};
    // G at the |cos zenith| (2k+1)/64 of the 64-direction diffuse quadrature
    // (DIFFUSE_DIRECTIONS[i] has |y| = (2*(i>>1)+1)/64); G when lidfValid == 0.
    float diffuseG[32]{};
};


// 模块：几何实例相关结构。InstanceLink 只记录实例所用网格 id。
struct InstanceLink
{
    uint32_t meshId;
};

// Instance：网格 meshId 及其对象到世界、世界到对象的 4x4 变换矩阵。
struct  Instance
{
    int meshId;
    glm::mat4 object2worldMatrix;
    glm::mat4 world2objectMatrix;
};

// 顶点属性：位置、法向、颜色、纹理坐标。
struct VertexAttribute
{
    glm::vec3 pos;
    glm::vec3 nrm;
    glm::vec3 color;
    glm::vec2 texCoord;
};

// 模块：网格缓冲与网格链接（以下 MeshBuffer / MeshLink）。
///-----------------------------------------------------
/// Mesh seems like a geometry in a geometric optical model
///-----------------------------------------------------

// MeshBuffer：网格的顶点/索引数量及其 Vulkan 顶点缓冲、索引缓冲。
struct MeshBuffer
{
    uint32_t nbIndices{ 0 };
    uint32_t nbVertices{ 0 };
    nvvk::Buffer   vertexBuffer;
    nvvk::Buffer   indexBuffer;
    // Voxel proxy meshes only (src/base/proxymesh.h): bytes per vertex of vertexBuffer (the default is the 44-byte
    // VertexAttribute every other mode uses) and whether an index buffer exists (false: triangle-list vertices,
    // BLAS indexType NONE, nbIndices == nbVertices).
    uint32_t       vertexStride{ 44 };
    bool           indexed{ true };
};
static_assert(sizeof(VertexAttribute) == 44, "MeshBuffer::vertexStride default must equal the VertexAttribute size");

// MeshLink：网格到材质/热/冠层/生化参数的索引（spectralId、thermalId、canopyId、bioId），
// 以及顶点、索引缓冲的设备地址，供着色器按地址访问。
struct MeshLink
{
    int type;
    int spectralId;
    int thermalId;
    int canopyId;
    int bioId;
    float angularEffectStrength{0.0f};

    uint64_t vertexAddress;
    uint64_t indexAddress;
};

// 模块：体元链接记录（VoxelLink，48 字节，对应 GLSL std430 步长）。
// voxelId 为体元整数坐标；instanceId/aeroId/faceId 为所属实例、空气动力区域、面元索引；
// isValid 为 0 的体元被跳过；leafDensity、normal 为几何导出的叶密度与表面法向；
// hexId 指向异质体元参数 VoxelHex，-1 表示均匀体元（兼容旧行为）。
// voxel link
struct alignas(16) VoxelLink
{
    glm::ivec3 voxelId{0, 0, 0};
    int instanceId{0};
    int aeroId{0}; // this is used for a whole image or a aero divided image;
    int faceId{0}; //
    int isValid{0}; // is 0 go pass
    // One-sided OBJ facet area per voxel volume (m2/m3) for objects whose leaf
    // density comes from geometry; 0 means use the canopy density parameter.
    float leafDensity{0.0f};
    int hexId{-1}; // 异质性体元参数索引；-1 保持均匀体元行为
    // Outward surface normal of a one-state solid OBJ cell; zero = horizontal.
    glm::vec3 normal{0.0f, 0.0f, 0.0f};
};

// 模块：异质性体元参数（160 字节）：ax/ay/az、rho 与模型 model 相关的 a/b/c，children 为子体元数据；
// 各字段具体物理含义（model=10 的轴向消光与六面邻居 beta 等）见 GLSL 实现（待核对）。
struct VoxelHex
{
    float ax{1.0f};
    float ay{1.0f};
    float az{1.0f};
    float rho{0.0f};
    // model 10: axis extinction coefficients and six face-neighbour beta values.
    float a{1.0f};
    float b{1.0f};
    float c{1.0f};
    int model{0};
    std::array<std::array<float, 4>, 8> children{};
};

static_assert(sizeof(VoxelLink) == 48, "VoxelLink must match GLSL std430 stride");
static_assert(sizeof(VoxelHex) == 160, "VoxelHex must match GLSL std430 stride");


// 模块：体元辐射/温度/通量等逐体元状态记录（VoxelDir 至 VoxelHeatflux）。
// VoxelDir：solar 为太阳相关照射量（具体含义待核对），skyDiffuse 为未碰撞天空散射相对开阔水平面的比例。
struct VoxelDir
{
    float solar;
    // Uncollided diffuse-sky illumination relative to an open horizontal surface.
    float skyDiffuse;
};

// VoxelRad：逐体元、逐波段、逐方向的辐射场记录；induced 为本轮入射/散射源，cumulated 为累积量，二者用法随模式与阶数略有差异（见 shader）。
struct VoxelRad
{
    float cumulated;
    float induced;
    //float diffuse;
};

// VoxelTempe：体元向阳/背阴组分温度；solarFraction 为土壤方法 1 历史的向阳比例，首个时间节点提交前为负。
struct VoxelTempe
{
    float sunlit;
    float shaded;
    // Sunlit fraction of the stored soil-method-1 histories (VoxelEB
    // updateTp); negative until the first node has been committed.
    float solarFraction = -1.0f;
};
static_assert(sizeof(VoxelTempe) == 12, "VoxelTempe must match GLSL std430 stride");

// VoxelNetRad：可见近红外（VNIR）与热红外（TIR）波段的直射/散射辐射。
struct VoxelNetRad
{
    float directVrad;   // direct radiance at VNIR bands
    float diffuseVrad;  // diffuse radiance at VNIR bands
    float directTrad;   // direct radiance at TIR bands
    float diffuseTrad;  // diffuse radiance at TIR bands
};

// VoxelPnet：体元吸收的直射/散射 PAR 光量子通量（directPnet 见 voxelrad_direct_VNIR.comp）。
struct VoxelPnet
{
    float directPnet;
    float diffusePnet;
};

// VoxelRaa：体元空气动力学阻抗 raa（s m-1）。
// aerodynamic resistance
struct VoxelRaa
{
    float raa;  // aerodynamic resistance
};




// VoxelRss：体元向阳/背阴表面阻抗 rss（s m-1）。
// surface resistance
struct VoxelRss
{
    float sunlit;   // surface resistance at sunlit surface in this voxel
    float shaded;   // surface resistance at shaded surface in this voxel
};

// 模块：叶面空气状态（VoxelAir，36 字节）。数组下标 0 背阴、1 向阳；
// heightM、soilMoisture、co2ExcessPpm 来自 TerrainEnvironment（见 terrain_environment.h）。
// Leaf-surface state per conditional component: [0] shaded, [1] sunlit.
struct VoxelAir
{
    float cs[2];    // CO2 at the leaf surface (ppm)
    float ci[2];    // intercellular CO2 (ppm)
    float es[2];    // vapour pressure at the leaf surface (hPa)
    float heightM{0.0f}; // air sampling height above scene datum
    float soilMoisture{-1.0f}; // prescribed local m3/m3, -1 uses material SMC
    float co2ExcessPpm{0.0f}; // prescribed ambient excess, no transport
};
static_assert(sizeof(VoxelAir) == 36);
static_assert(offsetof(VoxelAir, heightM) == 24);
static_assert(offsetof(VoxelAir, soilMoisture) == 28);
static_assert(offsetof(VoxelAir, co2ExcessPpm) == 32);

// 模块：向阳/背阴组分的能量平衡通量（H 显热、LE 潜热、G 储热）及叶片光合 GPP/NPP。
// Rn - H - LE - G = 0
struct VoxelHeatflux
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

// 模块：温度历史记录（向阳/背阴），按 TLASTNUM 个节点/层存放，用于土壤热惯性（Duhamel 求和）等历史相关更新。
// temperature at LAST time nodes
struct TLAST
{
    float sunlit;   // sunlit temperature
    float shaded;   // shaded temperature
};

// 模块：能量平衡的 MOST（Monin-Obukhov）稳定度修正所需的整数累加状态。
// 用定点整数累加是为了让 GPU 原子加的结果确定、与线程顺序无关；各数组按字段注释的缩放因子存储。
// Layout must match EBState in shader/parameters.h (MOST fixed-point sums).
constexpr int kEnergyStateBins = 128;
struct EBState
{
    uint32_t count;
    int32_t groundCount;
    int32_t sensible[kEnergyStateBins]; // 0.1 W m-2 per cell footprint
    int32_t ustar[kEnergyStateBins];    // 1e-4 m s-1
    int32_t height[kEnergyStateBins];   // 0.01 m
    // Ship waterline contact: scene water voxel count and the sum of their
    // temperatures in 0.01 K (fixed point, deterministic).
    int32_t waterCount;
    int32_t waterTemperature;
};
static_assert(sizeof(EBState) == 8 + 3 * 4 * kEnergyStateBins + 8);

// 模块：流体网格单元元数据：surfaceIndex 为所属表面索引（-1 无），kind 为单元类型，lad 为叶面积密度。
struct alignas(16) FluidCellMeta
{
    int surfaceIndex{-1};
    int kind{0}; // 0 air, 1 solid, 2 vegetation, 3 fire, 4 fog
    float lad{0.0f};
    float padding{0.0f};
};

// 模块：流体耦合参数（128 字节，须与着色器一致）：网格、间距与时间步、环境风/温度、
// 物理系数、源项、地形空气廓线（heightProfile 与 TerrainEnvironment 对应）以及局地交换开关。
struct alignas(16) FluidParameters
{
    glm::ivec4 grid{1, 1, 1, 0};
    glm::vec4 spacingTime{1.0f, 1.0f, 1.0f, 5.0f}; // xyz spacing, w scalar dt
    glm::vec4 ambientWind{0.0f, 0.0f, 0.0f, 298.15f};
    glm::vec4 physics{0.033f, 0.3f, 1.0f, 0.1f};
    glm::vec4 sources{0.02f, 0.12f, 30.0f, 1.0f}; // smoke, lattice cap, speed cap, velocity scale
    glm::vec4 couplingSpacing{1.0f, 1.0f, 1.0f, 0.0f}; // xyz spacing, w terrain soil enabled
    glm::vec4 heightProfile{0.0f, 0.0065f, 2.0f, 2000.0f}; // enabled, lapse, reference height, vapor scale
    // x enables local leaf/soil exchange; y enables VoxelEB MOST stability
    // correction (control.stabilityCorrection); z is the current scene 1/L
    // [m-1] used by aeresist.comp; w reserved.
    glm::vec4 localExchange{0.0f};
};
static_assert(sizeof(FluidParameters) == 128);
static_assert(offsetof(FluidParameters, heightProfile) == 96);
static_assert(offsetof(FluidParameters, localExchange) == 112);


// 模块：叶片生化/光合参数（Vcmax、Ball-Berry 气孔参数、温度响应参数、非光化学淬灭速率等）。
// 多数字段无默认值，缺省时依赖解析层赋值。
struct LeafBio
{
    float Vcmax;		// maximum carboxylation capacity (at optimum temperature)
    float m;			// ball-berry stomatal conductance parameter
    float BallBerry;
    float Type;
    float kV;			// extinction coefficient for a vertical profile
    float Rdparam;		// parameter for dark respiration
    float Tparam[5];
    float Tyear;
    float beta;
    float kNPQs;		// rate constant of sustained thermal dissipation
    float qLs;			// fraction of functional reaction voxelIds
    float stressfactor;
    int Tcor;
};

// 模块：土壤/固体表面热与光学参数：热惯性（cs、rhos、lambdas）、含水量、BRDF（朗伯或 Hapke）、
// 木质/建筑分类，以及舰船船体选项（航速、水线、钢板层，全部默认 0 表示关闭，保持旧工程行为）。
struct SoilSet
{
    int method;
    float rss;			// soil resistance for evaporation
    float cs;			// volumetric heat capacity of the soil
    float rhos;         // specific mass of the soil
    float lambdas;       //  heat conductivity of the soil
    float Tsoil;        // aeverage temperature 25
    float smc;			// volumetric soil moisture content 0.25
    float SatWater;
    int brdfModel{0};             // 0: Lambert, 1: Hapke
    float hapkeB0{1.0f};          // opposition-effect amplitude
    float hapkeH{0.1f};           // opposition-effect angular width
    float hapkeG{0.0f};           // Henyey-Greenstein asymmetry
    int thermalClass{0};          // 0: ground/building surface, 1: woody solid
    float convectiveScale{1.0f};  // small branches exchange heat faster than broad surfaces
    // Method 1 history: 0 keeps the last TLASTNUM nodes only (legacy, truncated
    // Duhamel sum), 1 adds the older history as SOIL_MEMORY_MODES exponential
    // modes (control.soilInertiaHistory = "full").
    int inertiaHistory{0};
    // Ship hull (material type Ship; all 0 = off, old behaviour). speed: m/s
    // through the air; windCosine: cos(heading - wind-from direction), 0 keeps
    // the perpendicular sqrt(u^2+v^2); waterHeatTransferCoefficient > 0 turns
    // on the waterline: voxels whose centre lies below waterlineHeight (world y)
    // exchange heat with the scene water temperature at this coefficient.
    float shipSpeed{0.0f};
    float shipWindCosine{0.0f};
    float waterlineHeight{0.0f};
    float waterHeatTransferCoefficient{0.0f};
    // Ship hull steel plate (material type Ship, wallMode "wall" + voxelWall;
    // plateThickness 0 = off, the hull stays a semi-infinite solid). The plate
    // has TLASTNUM implicit layers stored in the Tlast profile; the inner face
    // exchanges with plateIndoorC through plateIndoorH. internalHeatFlux
    // (W m-2 of hull surface, any hull voxel) heats the inner layer (plate) or
    // enters the surface balance (no plate).
    float plateThickness{0.0f};
    float plateLambda{0.0f};
    float plateCapacity{0.0f};     // rho * cp, J m-3 K-1
    float plateIndoorC{22.0f};
    float plateIndoorH{8.0f};
    float internalHeatFlux{0.0f};
    //BSMParam bsm;
};

// 模块：开阔水体能量平衡与反射参数（朗伯或 Fresnel-Cox-Munk）。
// Open-water energy balance parameters.
struct WaterSet
{
    float rss;                    // surface resistance, normally 0 s/m
    float heatCapacity;           // volumetric heat capacity, J/(m3 K)
    float mixingDepth;            // effective mixed-layer depth, m
    float evaporationCoefficient; // 0..1 multiplier for latent heat flux
    int brdfModel{0};             // 0: Lambert, 1: Fresnel-Cox-Munk
    float refractiveIndex{1.333f};
    float slopeVariance{0.0f};     // <=0: derive from meteorological wind
    float diffuseFraction{0.02f};  // residual subsurface/Lambert component
};

// 模块：建筑/人工表面的热参数（cs、rhos、lambdas 取值范围见字段行注释），Tin 为室内温度（待核对）。
struct BuildUp
{
    int method;
    float rss;
    float cs;          // 1180
    float rhos;       // 1900 -2600
    float lambdas;    // 1.0 -1.7
    float Tin;
};

// 模块：单个时间节点的气象强迫，来自气象文件（列顺序 t Ta ea p u Rin Rli，见 fileio.cpp）。
// Ta 为气温（°C），ea 水汽压（hPa），u 风速（m s-1），Rin/Rli 入射短波/长波（W m-2）；
// p 单位待核对；dTime 为时间步长（默认 600，单位应为 s，待核对）。
struct Meteo
{
    float z;
    float t;		// time node
    float u;		// wind speed
    float p;		// air pressure
    float Rin;		// incoming shortwave radiation
    float Rli;		// incoming longwave radiation
    float Ta;		// air temperature
    float sm;		// volumetric soil moisture content
    float ea;		// atmospheric vapor pressure
    float Ca;		// CO2 concentration in the air
    float Oa;		// O2 concentration in the air
    float dTime {600};
};



#endif //FIELD_STRUCTS_CG_H
