// ==========================================================================
// 文件：raytracing.cpp
// 功能：RayTracing 模式（Vulkan 光线追踪的离线反射率/亮温观测）的总控类 Raytracing 的实现：
//       初始化 Vulkan 资源、读取场景与观测设置、逐观测角（或 UAV 航点）运行光追并输出结果。
// 主要模块：
//   1. buildPhysicalTextureFactors  物理纹理：逐三角形的短波反射率缩放因子（与 FacetRT 一致）
//   2. raytracingBandNames / sceneTopHeight  输出波段名；场景最高点（周期边界遍历用）
//   3. setup / upload / create      设备与分配器初始化；读取场景；创建缓冲/描述符/管线/命令
//   4. uploadSetting / updateSetting  XML 设置 → RaytracingIO → 推送常量 RayRTSetting
//   5. run                          逐观测角（或 UAV 航点）发射光追，输出 TIFF / albedo.txt
//   6. outputOrth / output / outputAlbedo  读回 GPU 结果、大气校正并写出
//   7. destroy                      按相反顺序释放 Vulkan 资源
// 数据流：FileIO（RaytracingXml）→ upload 填充 RaytracingIO 的网格/实例/光学参数 →
//         create 建 GPU 资源 → run 逐角度 Command::run 追踪 → m_virtualio 存储缓冲读回 CPU →
//         大气校正（base/atmosphere_lut.h）→ FileIO::writeTIFData 写 TIFF。
// 关系：raytracing.h 声明；pipeline.cpp 创建管线；其余子模块（buffer/descriptor/command）
//       在同目录；几何/传感器更新在 base/geometry.cpp。
// 约定：波长 > 50 视为 nm，否则视为 μm；≤ 2.5 μm 为短波（输出反射因子 [-]），
//       更长波长输出亮温 [K] 或光谱辐亮度 [W m-2 sr-1 um-1]；角度单位为度。
//       标准输出里的 PROGRESS / OBSERVATION 行供前端（src/renderer/src/main.js）解析进度。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include "raytracing.h"
#include "src/base/atmosphere_lut.h"
#include "src/base/reflectance_texture.h"

// 匿名命名空间：只在本文件内使用的辅助函数。
namespace {

// 模块：物理纹理因子 buildPhysicalTextureFactors（与 FacetRT 的做法一致）。
// 输入：fileio 里的光谱材质（纹理路径/强度/相对标准差/重复尺寸）、io 里的网格与实例；
// 输出：io.instanceTextureOffsets（每个实例在因子数组中的起点，无纹理为 kNoTextureOffset）
//       与 io.textureFactors（按 实例×三角形 排列的无量纲因子，用于缩放短波反射率）。
// 步骤：1 加载各材质的纹理；2 逐实例逐三角形在世界坐标质心处三平面采样；
//       3 按纹理所属材质扣除面积加权均值；4 由短波波段上限求缩放系数；5 因子 = 1 + 系数×偏差。
// 说明：因子数组第 0 个元素固定为 1，保证没有纹理时着色器也能安全访问。
// Mirrors FacetRT: one triplanar sample at each world-space triangle centroid,
// per-spectrum area-weighted mean removed, then factor = 1 + scale * deviation
// with scale capped by the strength, by factor > 0 and by R * factor <= 1 - T
// in every shortwave band. Thermal bands are never modulated.
void buildPhysicalTextureFactors(const std::shared_ptr<FileIO>& fileio, RaytracingIO& io)
{
    const auto& instances = io.m_instanceio->instances;
    const auto& meshLinks = io.m_meshio->meshLinks;
    const auto& objMeshes = io.m_meshio->objMeshes;
    const auto& spectra = fileio->m_pRaytracingXml->spectralxmls;
    const auto& waves = fileio->m_pRaytracingXml->sensorxml.waves;
    io.instanceTextureOffsets.assign(std::max<size_t>(1, instances.size()), kNoTextureOffset);
    io.textureFactors.assign(1, 1.0f);

    // 步骤 1：逐材质加载纹理。textureSourceName 非空时，该材质借用另一材质的纹理与统计
    //         （textureOwners 记录统计归属）；未启用或强度/相对标准差都为 0 的材质不加载。
    std::vector<std::shared_ptr<const ReflectanceVarianceTexture>> textures(spectra.size());
    std::vector<size_t> textureOwners(spectra.size());
    bool anyTexture = false;
    for (size_t s = 0; s < spectra.size(); ++s) {
        textureOwners[s] = s;
        if (!spectra[s].textureSourceName.empty()) {
            const auto source = io.m_meshio->spectralNames.find(spectra[s].textureSourceName);
            if (source == io.m_meshio->spectralNames.end())
                throw std::runtime_error("Physical texture source not found: " + spectra[s].textureSourceName);
            textureOwners[s] = source->second;
        }
        if (!spectra[s].physicalTextureEnabled || !(spectra[s].physicalTextureStrength > 0.0f || spectra[s].physicalTextureRelativeStd > 0.0f)) continue;
        textures[s] = loadReflectanceVarianceTexture(spectra[s].physicalTexturePath);
        anyTexture = true;
    }
    if (!anyTexture) return;

    // 步骤 2：按纹理所属材质累计面积与面积加权采样值，供后面扣均值和换算缩放系数。
    struct Statistics {
        double area{};
        double areaWeightedSample{};
        TextureDeviationStatistics deviation{};
        float scale{};
        double areaWeightedFactor{};
        double areaWeightedFactorSquare{};
        float minimumFactor{std::numeric_limits<float>::infinity()};
        float maximumFactor{std::numeric_limits<float>::lowest()};
    };
    std::vector<Statistics> statistics(spectra.size());
    std::vector<float> samples, areas;
    std::vector<int> owners;
    for (size_t instance = 0; instance < instances.size(); ++instance) {
        const int meshId = instances[instance].meshId;
        if (meshId < 0 || meshId >= static_cast<int>(objMeshes.size()) ||
            meshId >= static_cast<int>(meshLinks.size())) continue;
        const int spectralId = meshLinks[meshId].spectralId;
        if (spectralId < 0 || spectralId >= static_cast<int>(textures.size()) || !textures[spectralId]) continue;
        const size_t textureOwner = textureOwners[spectralId];
        const ObjMesh& mesh = objMeshes[meshId];
        const glm::mat4& toWorld = instances[instance].object2worldMatrix;
        // 偏移以 uint32 存放，0xFFFFFFFF 保留作“无纹理”标记，因此三角形总数必须小于该值。
        if (samples.size() + mesh.indices.size() / 3 >= kNoTextureOffset)
            throw std::runtime_error("Physical texture factors exceed 2^32 instance triangles");
        io.instanceTextureOffsets[instance] = static_cast<uint32_t>(samples.size());
        for (size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
            const auto world = [&](size_t k) {
                return glm::vec3(toWorld * glm::vec4(mesh.vertices[mesh.indices[index + k]].pos, 1.0f));
            };
            const glm::vec3 a = world(0), b = world(1), c = world(2);
            const glm::vec3 normal = glm::cross(b - a, c - a);
            const float doubleArea = glm::length(normal);
            const glm::vec3 center = (a + b + c) / 3.0f;
            // 三平面权重取法线各分量的绝对值；退化（零面积）三角形退化为只用 Y 平面（地面）。
            const glm::vec3 weights = doubleArea > 0.0f ? glm::abs(normal / doubleArea) : glm::vec3(0.0f, 1.0f, 0.0f);
            const float sample = sampleReflectanceVarianceTriplanar(
                *textures[spectralId], {center.x, center.y, center.z}, {weights.x, weights.y, weights.z},
                spectra[spectralId].physicalTextureRepeatSize);
            const float area = 0.5f * doubleArea;
            samples.push_back(sample);
            areas.push_back(area);
            owners.push_back(static_cast<int>(textureOwner));
            statistics[textureOwner].area += area;
            statistics[textureOwner].areaWeightedSample += static_cast<double>(area) * sample;
        }
    }
    if (samples.empty()) return;

    // 步骤 3：按材质扣除面积加权均值，使纹理只改变空间分布而不改变材质平均反射率；
    //         同时累计偏差的面积加权平方和与极值（用于后面限制因子范围）。
    for (size_t k = 0; k < samples.size(); ++k) {
        Statistics& stats = statistics[owners[k]];
        const float mean = stats.area > 0.0 ? static_cast<float>(stats.areaWeightedSample / stats.area) : 0.0f;
        samples[k] -= mean;
        stats.deviation.area += areas[k];
        stats.deviation.areaWeightedSquare += static_cast<double>(areas[k]) * samples[k] * samples[k];
        stats.deviation.minimumDeviation = std::min(stats.deviation.minimumDeviation, samples[k]);
        stats.deviation.maximumDeviation = std::max(stats.deviation.maximumDeviation, samples[k]);
    }
    // 步骤 4：对每个材质，在所有短波（≤ 2.5 μm）波段上求允许的最大因子：
    //         因子 × R + T ≤ 1，即因子 ≤ (1 - T)/R；热红外波段不被纹理调制。
    const size_t bandCount = waves.size();
    for (size_t s = 0; s < statistics.size(); ++s) {
        Statistics& stats = statistics[s];
        if (!(stats.area > 0.0)) continue;
        float maximumFactor = std::numeric_limits<float>::infinity();
        for (size_t band = 0; band < bandCount; ++band) {
            // 波长单位判别：> 50 视为 nm（换算成 μm），否则已经是 μm。
            const float micrometres = waves[band] > 50.0f ? waves[band] / 1000.0f : waves[band];
            if (micrometres > 2.5f) continue;
            const Spectral& value = io.m_meshio->spectrals[s * bandCount + band];
            const float base = std::clamp(value.reflectance, 0.0f, 1.0f);
            if (!(base > 1.0e-6f)) continue;
            const float tau = std::clamp(value.transmittance, 0.0f, 1.0f - base);
            maximumFactor = std::min(maximumFactor, (1.0f - tau) / base);
        }
        stats.scale = physicalTextureScale(spectra[s].physicalTextureStrength, spectra[s].physicalTextureRelativeStd, stats.deviation, maximumFactor);
    }
    // 步骤 5：生成最终因子 = 1 + scale × 偏差，并统计均值、变异系数 cv 与范围用于日志。
    io.textureFactors.resize(samples.size());
    for (size_t k = 0; k < samples.size(); ++k) {
        Statistics& stats = statistics[owners[k]];
        const float factor = 1.0f + stats.scale * samples[k];
        io.textureFactors[k] = factor;
        stats.areaWeightedFactor += static_cast<double>(areas[k]) * factor;
        stats.areaWeightedFactorSquare += static_cast<double>(areas[k]) * factor * factor;
        stats.minimumFactor = std::min(stats.minimumFactor, factor);
        stats.maximumFactor = std::max(stats.maximumFactor, factor);
    }
    for (size_t s = 0; s < statistics.size(); ++s) {
        const Statistics& stats = statistics[s];
        if (!(stats.area > 0.0)) continue;
        const double mean = stats.areaWeightedFactor / stats.area;
        std::cout << "raytracing: physical texture " << spectra[s].spectralName
                  << ", area-weighted factor mean=" << mean
                  << ", cv=" << std::sqrt(std::max(0.0, stats.areaWeightedFactorSquare / stats.area - mean * mean))
                  << ", range=[" << stats.minimumFactor << ',' << stats.maximumFactor << "]" << std::endl;
    }
}

// 作用：生成输出 TIFF 的各波段名称（波长单位判别同上：> 50 为 nm）。
// 说明：≤ 2.5 μm 标为反射因子 [-]；更长波段按 temperatureOutput 标为亮温 [K] 或辐亮度。
std::vector<std::string> raytracingBandNames(const std::vector<float>& waves,
                                             bool temperatureOutput)
{
    std::vector<std::string> names;
    names.reserve(waves.size());
    for (float wave : waves) {
        const float micrometres = wave > 50.0f ? wave / 1000.0f : wave;
        std::ostringstream label;
        if (micrometres <= 2.5f)
            label << "Reflectance [-] @ " << wave << " nm";
        else if (temperatureOutput)
            label << "Brightness temperature [K] @ " << wave << " nm";
        else
            label << "Spectral radiance [W m-2 sr-1 um-1] @ " << wave << " nm";
        names.push_back(label.str());
    }
    return names;
}

// 作用：sceneTopHeight——求所有实例化几何的世界坐标最高点（y 向上，单位 m）。
// 做法：对每个网格求局部包围盒，再把 8 个角点变换到世界系取最大 y（保守上界）。
// 说明：没有几何时返回 1.0e30（相当于不限制）；有几何时加 max(0.01, 1e-4·|top|) 的余量。
//       结果写入推送常量 sceneTop（见 updateSetting）。
// Highest world y of all instanced geometry. Periodic traversal (offline.rgen)
// starts descending rays at this height and stops rays above it that do not
// descend, so wraps are never spent in empty space above the scene.
float sceneTopHeight(const RaytracingIO& io)
{
    const auto& objMeshes = io.m_meshio->objMeshes;
    std::vector<float> meshTop(objMeshes.size(), std::numeric_limits<float>::lowest());
    std::vector<glm::vec3> meshLow(objMeshes.size()), meshHigh(objMeshes.size());
    std::vector<char> meshValid(objMeshes.size(), 0);
    for (size_t m = 0; m < objMeshes.size(); ++m) {
        if (objMeshes[m].vertices.empty()) continue;
        glm::vec3 low(std::numeric_limits<float>::max());
        glm::vec3 high(std::numeric_limits<float>::lowest());
        for (const auto& vertex : objMeshes[m].vertices) {
            low = glm::min(low, vertex.pos);
            high = glm::max(high, vertex.pos);
        }
        meshLow[m] = low;
        meshHigh[m] = high;
        meshValid[m] = 1;
    }
    float top = std::numeric_limits<float>::lowest();
    for (const auto& instance : io.m_instanceio->instances) {
        const size_t m = instance.meshId;
        if (m >= objMeshes.size() || !meshValid[m]) continue;
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local{(corner & 1) ? meshHigh[m].x : meshLow[m].x,
                                  (corner & 2) ? meshHigh[m].y : meshLow[m].y,
                                  (corner & 4) ? meshHigh[m].z : meshLow[m].z};
            top = std::max(top, (instance.object2worldMatrix * glm::vec4(local, 1.0f)).y);
        }
    }
    if (!std::isfinite(top)) return 1.0e30f;
    return top + std::max(0.01f, 1.0e-4f * std::abs(top));
}

// 作用：offline.rgen 把非有限（NaN/Inf）路径样本丢弃时，在输出缓冲末尾（图像平面之后）的计数器上加 1；
//   这里读回后若 > 0 就报警（正常运行应为 0；采样修正后实测 rt_crop / rt_urban 都是 0）。
void reportDroppedSamples(float count)
{
    if (count >= 1.0f)
        std::cerr << "WARNING raytracing: " << static_cast<long long>(count)
                  << " non-finite path sample(s) were dropped by the shader; "
                     "please report this scene (see docs/raytracing-variance-reduction.md)" << std::endl;
}
}


// 模块：setup——初始化设备相关对象。
// 输入：appsetting（Vulkan 上下文与队列）；输出：raytracingio 里的设备/队列/命令池/分配器/
//       光追属性（m_rtProperties）/SBT 包装器/加速结构构建器均就绪；返回 true。
// 步骤：1 复制设备句柄与图形队列；2 初始化命令池；3 创建资源分配器；
//       4 查询光追管线属性；5 初始化 SBTWrapper 与加速结构构建器。
bool Raytracing::setup( AppSetting &appsetting, std::shared_ptr<RaytracingIO> &raytracingio){


    // 步骤 1：取设备句柄和 eGCT（图形/计算/传输）队列及其族索引。
    raytracingio->m_device = appsetting.m_context.m_device;
    raytracingio->m_physicalDevice = appsetting.m_context.m_physicalDevice;
    raytracingio->m_instance = appsetting.m_context.m_instance;
    raytracingio->m_queues = appsetting.m_queues;
    raytracingio->m_queue =  raytracingio->m_queues[eGCT].queue;
    raytracingio->m_queueIndex = raytracingio->m_queues[eGCT].familyIndex;
    //    m_instance = appSetting.m_context.m_instance;
//    m_device = appSetting.m_context.m_device;
//    m_physicalDevice = appSetting.m_context.m_physicalDevice;
//    m_queues = appSetting.m_queues;


//    VkCommandPoolCreateInfo poolCreateInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
//    poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
//    vkCreateCommandPool(raytracingio->m_device, &poolCreateInfo, nullptr, &raytracingio->m_cmdPool);

    // 步骤 2：初始化一次性命令缓冲所用的命令池。
    raytracingio->m_genCmdBuf.init(raytracingio->m_device,raytracingio->m_queueIndex);


//    VkPipelineCacheCreateInfo pipelineCacheInfo{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
//    vkCreatePipelineCache(raytracingio->m_device, &pipelineCacheInfo, nullptr, &raytracingio->m_pipelineCache);

    // 步骤 3：创建显存资源分配器并绑定调试名工具。
    raytracingio->m_pAlloc  = std::make_shared<Allocator>();
    raytracingio->m_pAlloc->init(raytracingio->m_instance, raytracingio->m_device, raytracingio->m_physicalDevice);
    raytracingio->m_debug.setup(raytracingio->m_device);

    // 步骤 4：查询设备的光追管线属性（SBT 句柄大小/对齐等），后面 SBT 构建要用。
    /// requesting ray tracing properties. from physical device.
    VkPhysicalDeviceProperties2 rayTracingProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    rayTracingProperties.pNext = &(raytracingio->m_rtProperties);
    vkGetPhysicalDeviceProperties2(raytracingio->m_physicalDevice, &rayTracingProperties);

    if (raytracingio->useSBTWrapper)
    {
        raytracingio->m_sbtWrapper.setup(raytracingio->m_device, raytracingio->m_queueIndex, raytracingio->m_pAlloc.get(), raytracingio->m_rtProperties);
    }

    // 步骤 5：初始化加速结构（BLAS/TLAS）构建器。
    raytracingio->m_pAccelStruct->m_rtBuilder.setup(raytracingio->m_device, raytracingio->m_pAlloc.get(),raytracingio->m_queueIndex);

    return true;

}




// 模块：upload——读入场景与观测设置（纯 CPU 侧，不创建 GPU 资源）。
// 顺序有依赖：先读 defined 配置，再生成光学参数、场景网格/实例、观测与光源几何，
// 最后读设置并计算物理纹理因子（因子依赖已建好的 meshLinks 与光谱参数）。
bool Raytracing::upload(std::shared_ptr<FileIO> &fileio,std::shared_ptr<RaytracingIO> &raytracingio){

   // auto & fileio = raytracingio->m_fileio;
   // auto & meshio = raytracingio->m_meshio;


    fileio->readDefined(raytracingio->m_defined);
    m_pCompo->createCompOptical(fileio, raytracingio);
    m_pScene->createObjScene(fileio,raytracingio);
    m_pGeometry->createGeometry(fileio,raytracingio);
//    defineOPO(raytracingio);
    uploadSetting(fileio, raytracingio);
    buildPhysicalTextureFactors(fileio, *raytracingio);

    return true;
}

// 模块：create——创建 GPU 资源。顺序：缓冲 → 描述符（管线布局要用其 layout）→
// 管线（含 SBT）→ 命令 → 把设置写入推送常量。
bool Raytracing::create(std::shared_ptr<RaytracingIO> &raytracingio) {

    // An empty scene used to reach accelstruct/BLAS creation with an empty
    // instance list, producing a driver error before the friendly path.
    if (!raytracingio || !raytracingio->m_meshio || !raytracingio->m_instanceio
        || raytracingio->m_instanceio->instances.empty()
        || (raytracingio->m_meshio->objMeshes.empty() && raytracingio->m_meshio->primMeshes.empty())) {
        std::cerr << "RayTracing: scene has no mesh instance to trace" << std::endl;
        return false;
    }
    m_pBuffer->createBuffer(raytracingio);
    m_pDescriptor->createDescriptor(raytracingio);
    m_pPipeline->createPipeline(raytracingio);
    m_pCommand->create(raytracingio);
    updateSetting(raytracingio);
    return true;
}

//bool Raytracing::defineOPO(std::shared_ptr<RaytracingIO> &raytracingio){
//
//    //--------------------------------------------------------
//    //-- BRDF/Temperature
//    //-------------------------------------------------------
////    auto &opo = raytracingio->m_opo;
////    auto &fileio = raytracingio->m_fileio;
//    opo->isTemperature =  fileio->m_pXmlInput->sensor.isTemperature;
//
//    opo->isImage = fileio->m_pXmlInput->sensor.isImageSave;
//    opo->n_sample = fileio->m_pXmlInput->setting.n_sample;
//    opo->maxDepth = fileio->m_pXmlInput->setting.maxDepth;
//    return true;
//
//}

// 模块：uploadSetting——把 RaytracingXml 中的传感器/设置/场景参数复制到 RaytracingIO。
// 说明：sceneSize 取自 scenexml.background.sceneSize，用作周期边界的水平域尺寸。
bool Raytracing::uploadSetting(std::shared_ptr<FileIO> &fileio, std::shared_ptr<RaytracingIO> &raytracingio) {

    raytracingio->n_wave = fileio->m_pRaytracingXml->sensorxml.waves.size();
    raytracingio->n_angle = fileio->m_pRaytracingXml->sensorxml.viewAngles.size();
    raytracingio->isTemperature =  fileio->m_pRaytracingXml->sensorxml.isTemperature;
    raytracingio->isDisplay = fileio->m_pRaytracingXml->sensorxml.isDisplay;
    raytracingio->isAlbedo = fileio->m_pRaytracingXml->sensorxml.isAlbedo;
    raytracingio->isImage = fileio->m_pRaytracingXml->sensorxml.isImage;
    raytracingio->isOrth = fileio->m_pRaytracingXml->sensorxml.isOrth;
    raytracingio->imageSize = fileio->m_pRaytracingXml->sensorxml.resolution;
    raytracingio->maxDepth = fileio->m_pRaytracingXml->settingxml.maxDepth;
    raytracingio->n_sample = fileio->m_pRaytracingXml->settingxml.n_sample;
    raytracingio->periodicNeighborCount =
        fileio->m_pRaytracingXml->settingxml.periodicNeighborCount;
    raytracingio->skyboxEnabled =
        fileio->m_pRaytracingXml->settingxml.skyboxEnabled;
    raytracingio->sceneSize =
        fileio->m_pRaytracingXml->scenexml.background.sceneSize;
    raytracingio->rtSeed = fileio->m_pRaytracingXml->settingxml.rtSeed;
    raytracingio->rtSampler = fileio->m_pRaytracingXml->settingxml.rtSampler;
    raytracingio->rtAdaptive = fileio->m_pRaytracingXml->settingxml.rtAdaptive;
    if (const char* adaptive = std::getenv("HISTREAM_RT_ADAPTIVE"); adaptive && *adaptive)
        raytracingio->rtAdaptive = std::string(adaptive) != "0";
    if (const char* scout = std::getenv("HISTREAM_RT_ADAPTIVE_SCOUT"); scout && *scout)
        raytracingio->adaptiveScoutFraction = std::clamp(static_cast<float>(std::atof(scout)), 0.05f, 1.0f);
    if (const char* tile = std::getenv("HISTREAM_RT_ADAPTIVE_TILE"); tile && *tile)
        raytracingio->adaptiveTile = std::max(1, static_cast<int>(std::strtol(tile, nullptr, 10)));
    // Environment overrides (experiments / variance measurements).
    if (const char* seed = std::getenv("HISTREAM_RT_SEED"); seed && *seed)
        raytracingio->rtSeed = static_cast<int>(std::strtol(seed, nullptr, 10));
    if (const char* sampler = std::getenv("HISTREAM_RT_SAMPLER"); sampler && *sampler) {
        const std::string name(sampler);
        if (name == "sobol") raytracingio->rtSampler = 1;
        else if (name == "legacy") raytracingio->rtSampler = 0;
        else throw std::runtime_error("HISTREAM_RT_SAMPLER must be 'legacy' or 'sobol', got: " + name);
    }
    // The legacy sampler pins sample 0 to the pixel centre, which biases per-pixel sample counts that
    // differ from pixel to pixel (measured +0.2% at 16 spp): adaptive sampling needs the Sobol sampler.
    if (raytracingio->rtAdaptive && raytracingio->rtSampler == 0) {
        std::cerr << "WARNING raytracing: adaptive sampling requires the sobol sampler; ignored" << std::endl;
        raytracingio->rtAdaptive = false;
    }
    // Fewer than 8 samples per pixel leave no room for a pilot pass: run the normal path.
    if (raytracingio->rtAdaptive && fileio->m_pRaytracingXml->settingxml.n_sample < 8) raytracingio->rtAdaptive = false;

    return true;
}

// 模块：updateSetting——把 RaytracingIO 中的设置写入推送常量结构 RayRTSetting。
// 说明：sceneSize 的 x/y 下限取 0.01 m，避免周期域尺寸为 0（待核对）；
//       sceneTop 由 sceneTopHeight 求得；周期边界启用（遍历次数 > 0）时打印参数日志。
bool Raytracing::updateSetting(std::shared_ptr<RaytracingIO> &raytracingio){

   // auto &opo = raytracingio->m_opo;

   // auto &sceneio = raytracingio->m_sceneio;
    raytracingio->setting.imageSize = raytracingio->imageSize;
    raytracingio->setting.n_wave = raytracingio->n_wave;
    raytracingio->setting.isTemperature = raytracingio->isTemperature;
    raytracingio->setting.isDisplay = raytracingio->isDisplay;
    raytracingio->setting.maxDepth = raytracingio->maxDepth;
    raytracingio->setting.n_sample = raytracingio->n_sample;
    raytracingio->setting.periodicNeighborCount = raytracingio->periodicNeighborCount;
    raytracingio->setting.skyboxEnabled = raytracingio->skyboxEnabled ? 1 : 0;
    raytracingio->setting.sceneSize = {
        std::max(0.01f, raytracingio->sceneSize.x),
        std::max(0.01f, raytracingio->sceneSize.y)};
    raytracingio->setting.sceneTop = sceneTopHeight(*raytracingio);
    raytracingio->setting.frame = raytracingio->rtSeed;
    if (raytracingio->rtSeed != 0 || raytracingio->rtSampler != 0)
        std::cout << "raytracing: sampler "
                  << (raytracingio->rtSampler != 0 ? "sobol" : "legacy")
                  << ", seed " << raytracingio->rtSeed << std::endl;
    if (raytracingio->setting.periodicNeighborCount > 0)
        std::cout << "raytracing: periodic boundaries, traversal count "
                  << raytracingio->setting.periodicNeighborCount << ", domain "
                  << raytracingio->setting.sceneSize.x << " x " << raytracingio->setting.sceneSize.y
                  << " m, scene top " << raytracingio->setting.sceneTop << " m" << std::endl;

    return true;
    //raytracingio->setting.maxDepth = fileio->m_pXmlInput.
}


// 作用：一次完整追踪。常规：Command::run。自适应采样（rtAdaptive，可选，默认关闭）：
//   1) 侦察遍：每像元 n0 ≈ 25% 预算（HISTREAM_RT_ADAPTIVE_SCOUT 可改）个样本，用与正式样本不同的种子，只产生单样本相对标准差 σ；
//   2) 读回 σ，求缩放系数 s，使每像元样本数 clamp(round(s·σ), 预算/4, 预算·4) 的平均等于预算；
//   3) 正式遍：每像元 n_i 个样本（常规种子，Sobol 序号 0 … n_i-1 连续），侦察样本不进入结果，因此分配对估计无偏。
//   总开销 = 预算 + 侦察样本（约 +25% 样本）。
void Raytracing::trace(std::shared_ptr<RaytracingIO> &io)
{
    if (!io->rtAdaptive) {
        m_pCommand->run(io);
        return;
    }
    const int total = io->n_sample;
    const int scout = std::clamp(static_cast<int>(std::lround(io->adaptiveScoutFraction * total)), 4, total);
    const int minCount = std::max(2, total / 4);
    const int maxCount = 4 * total;
    const size_t pixels = static_cast<size_t>(io->imageSize.x) * io->imageSize.y;
    const size_t auxBase = pixels * static_cast<size_t>(io->n_wave) + 16;

    io->setting.n_sample = scout;
    io->setting.adaptivePass = 1;
    m_pCommand->run(io);

    // 读回 σ 平面（辅助平面 1）。
    const VkDeviceSize readBytes = (auxBase + 2 * pixels) * sizeof(float);
    VkBufferUsageFlags usage{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    nvvk::Buffer readback = io->m_pAlloc->createBuffer(readBytes, usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    m_pVirtual->bufferToBuffer(io, *(io->m_virtualio->m_pBufferStorage), readBytes, readback);
    const float* mapped = static_cast<const float*>(io->m_pAlloc->map(readback));
    std::vector<float> sigma(mapped + auxBase + pixels, mapped + auxBase + 2 * pixels);
    io->m_pAlloc->unmap(readback);
    io->m_pAlloc->destroy(readback);

    // tile 平均 σ（与着色器一致）：同一 tile 内的像元样本数相同，保持 warp 内线程负载均衡。
    const int tile = std::max(1, io->adaptiveTile);
    if (tile > 1) {
        const int w = io->imageSize.x, h = io->imageSize.y;
        std::vector<float> averaged(sigma.size());
        for (int ty0 = 0; ty0 < h; ty0 += tile)
            for (int tx0 = 0; tx0 < w; tx0 += tile) {
                double sum = 0.0; int count = 0;
                for (int y = ty0; y < std::min(ty0 + tile, h); ++y)
                    for (int x = tx0; x < std::min(tx0 + tile, w); ++x) { sum += sigma[static_cast<size_t>(y) * w + x]; ++count; }
                const float mean = static_cast<float>(sum / count);
                for (int y = ty0; y < std::min(ty0 + tile, h); ++y)
                    for (int x = tx0; x < std::min(tx0 + tile, w); ++x) averaged[static_cast<size_t>(y) * w + x] = mean;
            }
        sigma.swap(averaged);
    }
    // 二分求 s：Σ clamp(floor(s·σ + 0.5), 最小, 最大) ≥ 目标（最小的 s）。
    const double target = static_cast<double>(total) * static_cast<double>(pixels);
    const auto countOf = [&](double scale, float value) {
        return std::clamp(std::floor(scale * value + 0.5), static_cast<double>(minCount), static_cast<double>(maxCount));
    };
    const auto countTotal = [&](double scale) {
        double sum = 0.0;
        for (const float value : sigma) sum += countOf(scale, value);
        return sum;
    };
    double low = 0.0, high = 1.0e7;
    for (int iteration = 0; iteration < 80; ++iteration) {
        const double mid = 0.5 * (low + high);
        if (countTotal(mid) >= target) high = mid; else low = mid;
    }
    const double scale = high;
    int smallest = maxCount, largest = 0;
    double sum = 0.0;
    for (const float value : sigma) {
        const int count = static_cast<int>(countOf(scale, value));
        smallest = std::min(smallest, count); largest = std::max(largest, count); sum += count;
    }
    std::cout << "raytracing: adaptive sampling, scout " << scout << " + " << sum / static_cast<double>(std::max<size_t>(1, pixels))
              << " samples per pixel on average (min " << smallest << ", max " << largest << "), budget " << total << std::endl;

    io->setting.n_sample = total;
    io->setting.adaptivePass = 2;
    io->setting.adaptiveScale = static_cast<float>(scale);
    io->setting.adaptiveMinCount = minCount;
    io->setting.adaptiveMaxExtra = maxCount;
    io->setting.adaptiveTile = tile;
    m_pCommand->run(io);
    io->setting.adaptivePass = 0;
    io->setting.adaptiveScale = 0.0f;
    io->setting.adaptiveMinCount = 0;
    io->setting.adaptiveMaxExtra = 0;
}
// 模块：run——主循环。两种观测方式：
//   - UAV 巡航（isUAVTrave 且 n_pos > 0）：逐航点更新传感器位置，运行并输出（中心投影）；
//   - 常规：逐观测角 kangle 更新角度，运行并按投影类型输出 TIFF，必要时输出 albedo.txt。
// 输出：标准输出的 PROGRESS（百分比 5→10→…→100）与 OBSERVATION 行，结果文件由 output* 写出。
bool Raytracing::run(std::shared_ptr<RaytracingIO> &raytracingio, std::shared_ptr<FileIO> &fileio) {

    if (!raytracingio || !fileio) {
        std::cerr << "RayTracing: input/output objects are missing" << std::endl;
        return false;
    }
    const bool uavTraverse = raytracingio->isUAVTrave && raytracingio->n_pos > 0;
    if (uavTraverse) {
        if (raytracingio->uavposes.empty()) {
            std::cerr << "RayTracing: UAV traverse has no camera positions" << std::endl;
            return false;
        }
    } else if (raytracingio->n_angle <= 0) {
        // Both loops below would be empty, yet the function used to print
        // PROGRESS 100 and return true -> exit code 0 with no TIFF.
        std::cerr << "RayTracing: no observation angle or camera position is configured" << std::endl;
        return false;
    }
    std::cout << "PROGRESS\t5\t加载光线追踪场景与观测参数" << std::endl;

    // 步骤 1：若要输出 albedo，先清空旧的 albedo.txt（outputAlbedo 之后以追加方式写）。
    /// 清除txt文件信息
    if (raytracingio->isAlbedo)
    {
        // 以写入模式打开文件（std::ios::trunc 会清空文件）
        std::ofstream file(raytracingio->projectDir + "\\albedo.txt", std::ios::trunc);
        // 检查是否成功打开
        if (!file.is_open()) {
            std::cerr << "Error: Could not clear file " << raytracingio->projectDir + "\\albedo.txt" << std::endl;
        }
        // 文件内容已被清空，无需额外操作
        file.close(); // 显式关闭（可选）
    }


    // 步骤 2：按观测方式循环；进度条 10%~95% 之间按航点/观测角均分。
    if (uavTraverse)
    {
        raytracingio->kangle = 0;
        for (int kpos = 0; kpos < raytracingio->n_pos; ++kpos) {
            raytracingio->k_pos = kpos;
            m_pGeometry->updateSensorPos(raytracingio, kpos);
            const glm::vec3 position = raytracingio->uavposes[kpos];
            std::cout << "OBSERVATION\t航点 " << (kpos + 1) << "/" << raytracingio->n_pos
                      << " X=" << position.x << " Y=" << position.y
                      << " H=" << position.z << std::endl;
            trace(raytracingio);
            if (raytracingio->isImage || raytracingio->isOrth)
                output(raytracingio, fileio, 0, kpos);
            std::cout << "PROGRESS\t" << 10 + 85 * (kpos + 1) / std::max(1, raytracingio->n_pos)
                      << "\t中心投影巡航 " << (kpos + 1) << "/" << raytracingio->n_pos << std::endl;
        }
    } else {
        for(int kangle = 0; kangle < raytracingio->n_angle; kangle++)
        {
            const int beginProgress = 10 + static_cast<int>(80.0 * kangle /
                std::max(1, raytracingio->n_angle));
            std::cout << "PROGRESS\t" << beginProgress << "\t光线追踪观测 "
                      << (kangle + 1) << "/" << raytracingio->n_angle << std::endl;
            raytracingio->kangle = kangle;
            m_pGeometry->updateAngle(raytracingio,kangle);
            trace(raytracingio);

            if (raytracingio->isImage || raytracingio->isOrth) {
                if (fileio->m_pRaytracingXml->sensorxml.projection == Projection::PERSPECTIVE)
                    output(raytracingio, fileio, kangle);
                else
                    outputOrth(raytracingio, fileio, kangle);
            }
            if (raytracingio->isAlbedo) outputAlbedo(raytracingio,fileio,kangle);

            std::cout << "Success: " << kangle << std::endl;
            const int completedProgress = 10 + static_cast<int>(85.0 * (kangle + 1) /
                std::max(1, raytracingio->n_angle));
            std::cout << "PROGRESS\t" << completedProgress << "\t完成观测 "
                      << (kangle + 1) << "/" << raytracingio->n_angle << std::endl;
        }
    }

    std::cout << "PROGRESS\t100\t简单光线追踪计算完成" << std::endl;
    return true;
}

//bool Raytracing::createGPUBuffer(std::shared_ptr<RaytracingIO> &raytracingio){
//
//return false;
//}



// 模块：outputOrth——正射投影（isOrth / 非透视）输出：读回渲染结果，按拟合映射重采样到
//       正射栅格，做大气校正后写 TIFF。
// 输入：modelio（GPU 存储缓冲，布局 [波段][行][列] 的 float，单位同 raygen 输出：
//       反射因子 [-] / 亮温 [K] / 辐亮度）、kangle（观测角序号）、kpos（航点序号，-1 为无）。
// 步骤：1 GPU→主机可见缓冲→CPU 副本；2 取观测角并求校正系数 cx、cy；
//       3 逐输出像元用映射定位源位置并做双线性插值；4 大气校正；5 写 TIFF 并释放内存。
// 说明：覆盖区之外的输出像元保持 0（不写数据）。
void Raytracing::outputOrth(std::shared_ptr<RaytracingIO> &modelio, std::shared_ptr<FileIO> &fileio, int kangle, int kpos) {
VkBufferUsageFlags usage{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    int width = modelio->imageSize.x;
    int height = modelio->imageSize.y;
    int n_wave = modelio->n_wave;

    // 【修正1】使用 size_t 防止整数溢出 (3000*3000*100 超过 int 上限)
    size_t total_elements = (size_t)width * height * n_wave;
    VkDeviceSize bufferSize = total_elements * sizeof(float);

    // 步骤 1：分配主机可见缓冲，把 GPU 存储缓冲拷贝进来，再深拷贝为 CPU 数组（见下方修正说明）。
    // 末尾多读 1 个 float：offline.rgen 丢弃的非有限样本计数（见 reportDroppedSamples）。
    const VkDeviceSize readSize = bufferSize + sizeof(float);
    nvvk::Buffer pixelBuffer = modelio->m_pAlloc->createBuffer(readSize, usage,
                                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    m_pVirtual->bufferToBuffer(modelio, *(modelio->m_virtualio->m_pBufferStorage), readSize, pixelBuffer);

    // map memory
    void *data = modelio->m_pAlloc->map(pixelBuffer);

    // 【修正2】创建 CPU 端内存副本 (Deep Copy)
    // 这是解决 0xc0000005 崩溃的核心：将显存数据拷贝到 CPU，使其脱离 Vulkan 生命周期
    // 用 std::vector 持有副本：new[] 分配失败会抛 std::bad_alloc 而不会返回空指针，原先的空指针判断没有作用；
    // 改为 vector 后析构时自动释放，也不再需要手工 delete[]。
    std::vector<float> cpuDataStorage(total_elements + 1);
    float* pCpuData = cpuDataStorage.data();
    // 使用 memcpy 高速拷贝
    std::memcpy(pCpuData, data, readSize);
    reportDroppedSamples(pCpuData[total_elements]);
    // 【修正3】现在可以安全销毁 Vulkan 资源了
    modelio->m_pAlloc->unmap(pixelBuffer);
    modelio->m_pAlloc->destroy(pixelBuffer);

    Angle angle = modelio->angles[kangle];
    std::vector<float> waves = modelio->waves;
    glm::vec2 resolution = modelio->imageSize;

    // 步骤 2：取观测角（vza/vaa，度），由 Geometry::orthcorrect 求 x、y 两个方向的映射系数。
    // 计算校正参数
    Eigen::VectorXd cx;
    Eigen::VectorXd cy;
    m_pGeometry->orthcorrect(modelio, angle.vza, angle.vaa, cx, cy);
    // 分配结果内存
    float *pData_orth = new float[total_elements];
    std::memset(pData_orth, 0, bufferSize);

    // 步骤 3：遍历输出栅格 (i 列, j 行)，按映射在原始渲染图中取 4 邻点做双线性插值。
    // --- 正射校正循环 (已优化健壮性) ---
    for(int i = 0; i < width; i++)
    {
        for(int j = 0; j < height; j++)
        {
            // 目标图像索引 (使用 long long 防止溢出)
            long long old = (long long)j * width + i;

            // 映射形式：源坐标 = a·c[0] + b·c[1] + a·b·c[2] + c[3]（cx 给源列，cy 给源行；
            // a=输出列、b=输出行，系数由 Geometry::orthcorrect 拟合）。
            // The fitted mapping yields raster-edge coordinates (pixel k spans
            // [k, k+1]); shift to sample-centre indices before interpolating.
            const auto mapped = [&](const Eigen::VectorXd& c, double a, double b) {
                return a * c[0] + b * c[1] + a * b * c[2] + c[3];
            };
            double sourceX = mapped(cx, i, j) - 0.5;
            double sourceY = mapped(cy, i, j) - 0.5;
            // The output raster edges map onto the scene border. Raw pixels
            // straddling that border are only partly covered, so keep the
            // bilinear footprint inside fully covered raw pixels.
            const double coveredX0 = std::ceil(mapped(cx, -0.5, j) - 1.0e-6);
            const double coveredX1 = std::floor(mapped(cx, width - 0.5, j) + 1.0e-6) - 1.0;
            const double coveredY0 = std::ceil(mapped(cy, i, -0.5) - 1.0e-6);
            const double coveredY1 = std::floor(mapped(cy, i, height - 0.5) + 1.0e-6) - 1.0;
            if (sourceX < -0.5 || sourceX > width - 0.5 ||
                sourceY < -0.5 || sourceY > height - 0.5 ||
                coveredX1 < coveredX0 || coveredY1 < coveredY0)
            {
                continue;
            }
            // 把采样位置限制在完全被场景覆盖的原始像元范围内，避免边缘像元与背景混合。
            sourceX = std::clamp(sourceX, std::max(0.0, coveredX0), std::min(width - 1.0, coveredX1));
            sourceY = std::clamp(sourceY, std::max(0.0, coveredY0), std::min(height - 1.0, coveredY1));
            const int x0 = static_cast<int>(std::floor(sourceX));
            const int y0 = static_cast<int>(std::floor(sourceY));
            const int x1 = std::min(x0 + 1, width - 1);
            const int y1 = std::min(y0 + 1, height - 1);
            const double fx = sourceX - x0;
            const double fy = sourceY - y0;
            const int sampleX[4] = {x0, x1, x0, x1};
            const int sampleY[4] = {y0, y0, y1, y1};
            const double sampleWeight[4] = {
                (1.0 - fx) * (1.0 - fy), fx * (1.0 - fy),
                (1.0 - fx) * fy, fx * fy};

            for(int k = 0; k < n_wave; k++)
            {
                // 【修正5】使用 long long 计算波段偏移，防止计算溢出
                long long band_offset = (long long)k * width * height;
                long long oldd = band_offset + old;
                // Zero is valid data (shadow, black surface); the footprint clamp
                // above already keeps samples inside the covered raster.
                double valueSum = 0.0;
                for (int sample = 0; sample < 4; ++sample)
                {
                    const long long source = band_offset
                        + static_cast<long long>(sampleY[sample]) * width
                        + sampleX[sample];
                    valueSum += sampleWeight[sample] * pCpuData[source];
                }
                // 步骤 4：对插值结果做大气校正（传感器高度取 sensorxml.position.z，观测天顶角 vza）。
                pData_orth[oldd] = atmosphereCorrectOutputValue(
                    static_cast<float>(valueSum),
                    fileio->m_pRaytracingXml->atmospherexml,
                    waves[k], fileio->m_pRaytracingXml->sensorxml.position.z,
                    angle.vza, modelio->isTemperature);

            }
        }
    }


    // 步骤 5：写多波段 TIFF；启用大气校正时模型后缀为 "_a"，波段名见 raytracingBandNames。
    if (!fileio->writeTIFData(
        modelio->projectDir, pData_orth, width, height, n_wave,
        angle, -1.0f, fileio->m_pRaytracingXml->atmospherexml.enabled ? "_a" : "", kpos, false,
        raytracingBandNames(waves, modelio->isTemperature))) {
        delete[] pData_orth;
        throw std::runtime_error("RayTracing: failed to write output TIFF");
    }

    // 释放内存
    // pCpuData 由 cpuDataStorage 持有，函数结束时自动释放。
    delete[] pData_orth;  // 释放结果数据
}

// 模块：output——透视投影（含 UAV 航点）输出：读回渲染结果，处理天空像元与大气校正，写 TIFF。
// 输入：modelio（GPU 存储缓冲 [波段][行][列]）、kangle、kpos（UAV 航点序号，-1 为常规观测）。
// 步骤：1 GPU→CPU 副本；2 取观测角（航点用自身航向）；3 求天空像元的天顶角；
//       4 逐波段逐像元：天空像元用天空辐射值，其余做大气校正；5 写 TIFF 并释放内存。
void Raytracing::output(std::shared_ptr<RaytracingIO>& modelio, std::shared_ptr<FileIO>& fileio, int kangle, int kpos)
{
    VkBufferUsageFlags usage{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    int width = modelio->imageSize.x;
    int height = modelio->imageSize.y;
    int n_wave = modelio->n_wave;

    // 【修正1】使用 size_t 防止整数溢出 (3000*3000*100 超过 int 上限)
    size_t total_elements = (size_t)width * height * n_wave;
    VkDeviceSize bufferSize = total_elements * sizeof(float);

    // 步骤 1：GPU 存储缓冲 → 主机可见缓冲 → CPU 副本（同 outputOrth）。
    const VkDeviceSize readSize = bufferSize + sizeof(float);
    nvvk::Buffer pixelBuffer = modelio->m_pAlloc->createBuffer(readSize, usage,
                                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    m_pVirtual->bufferToBuffer(modelio, *(modelio->m_virtualio->m_pBufferStorage), readSize, pixelBuffer);

    // map memory
    void *data = modelio->m_pAlloc->map(pixelBuffer);

    // 【修正2】创建 CPU 端内存副本 (Deep Copy)
    // 这是解决 0xc0000005 崩溃的核心：将显存数据拷贝到 CPU，使其脱离 Vulkan 生命周期
    // 用 std::vector 持有副本：new[] 分配失败会抛 std::bad_alloc 而不会返回空指针，原先的空指针判断没有作用；
    // 改为 vector 后析构时自动释放，也不再需要手工 delete[]。
    std::vector<float> cpuDataStorage(total_elements + 1);
    float* pCpuData = cpuDataStorage.data();
    // 使用 memcpy 高速拷贝
    std::memcpy(pCpuData, data, readSize);
    reportDroppedSamples(pCpuData[total_elements]);

    // --- 此时数据已安全在 pCpuData 中，不再依赖 pData/data 指针 ---

    // 【修正3】现在可以安全销毁 Vulkan 资源了
    modelio->m_pAlloc->unmap(pixelBuffer);
    modelio->m_pAlloc->destroy(pixelBuffer);

    // 步骤 2：取观测角；UAV 航点的方位角 vaa 以该航点航向为准（见下方英文说明）。
    Angle angle = modelio->angles[kangle];
    // Cruise (UAV) waypoints each have their own heading.  Geometry::updateSensorPos
    // already writes it into angles.front(), but take it explicitly from the waypoint
    // so the TIFF VAA metadata and sky-pixel zenith never fall back to the first heading.
    if (kpos >= 0 && kpos < static_cast<int>(modelio->uavViewAzimuths.size()))
        angle.vaa = modelio->uavViewAzimuths[static_cast<size_t>(kpos)];
    std::vector<float> waves = modelio->waves;
    glm::vec2 resolution = modelio->imageSize;

    // 步骤 3：确定传感器高度（航点用 uavposes[kpos].z，否则用 XML 中的 position.z），
    //         并逐像元判断是否为天空像元：是则记录其天顶角，否则记 -1。
    const size_t imageElements = static_cast<size_t>(width) * height;
    const float sensorHeight = kpos >= 0 && kpos < static_cast<int>(modelio->uavposes.size())
        ? modelio->uavposes[kpos].z : fileio->m_pRaytracingXml->sensorxml.position.z;
    std::vector<float> skyZenith(imageElements, -1.0f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float zenith = 0.0f;
            if (atmosphereSkyPixelZenith(
                    x, y, width, height,
                    fileio->m_pRaytracingXml->sensorxml.sensorFov,
                    angle.vza, angle.vaa, zenith)) {
                skyZenith[static_cast<size_t>(y) * width + x] = zenith;
            }
        }
    }
    // 步骤 4：逐波段逐像元处理。值为 0 或非有限（光线未命中场景，天空盒关闭时输出 0）且
    //         属于天空像元 → 用天空输出值（含天空温度）；其余像元 → 大气校正。
    for (int band = 0; band < n_wave; ++band) {
        const size_t offset = static_cast<size_t>(band) * imageElements;
        for (size_t pixel = 0; pixel < imageElements; ++pixel) {
            float& value = pCpuData[offset + pixel];
            if ((!std::isfinite(value) || value == 0.0f) && skyZenith[pixel] >= 0.0f) {
                value = atmosphereSkyOutputValue(
                    fileio->m_pRaytracingXml->atmospherexml,
                    waves[band], sensorHeight, skyZenith[pixel],
                    modelio->isTemperature,
                    fileio->m_pRaytracingXml->lightxml.skyTemperature);
            } else {
                value = atmosphereCorrectOutputValue(
                    value, fileio->m_pRaytracingXml->atmospherexml,
                    waves[band], sensorHeight,
                    angle.vza, modelio->isTemperature);
            }
        }
    }

    // 步骤 5：写多波段 TIFF（参数同 outputOrth）。
    if (!fileio->writeTIFData(
        modelio->projectDir, pCpuData, width, height, n_wave,
        angle, -1.0f, fileio->m_pRaytracingXml->atmospherexml.enabled ? "_a" : "", kpos, false,
        raytracingBandNames(waves, modelio->isTemperature))) {
        throw std::runtime_error("RayTracing: failed to write output TIFF");
    }

    // 释放内存
    // pCpuData 由 cpuDataStorage 持有，函数结束时自动释放。
}

// 模块：outputAlbedo——把每个波段的平均反射率追加写入工程目录下的 albedo.txt。
// 输入：modelio（GPU 存储缓冲）、kangle；每个波段一行：
//       波长 sza saa vza vaa 平均值（角度单位为度；run 开始时已清空该文件）。
// 步骤：1 读回 GPU 结果；2 逐波段求 >0 像元的均值；3 释放缓冲；4 追加写文本。
void Raytracing::outputAlbedo(std::shared_ptr<RaytracingIO>& modelio, std::shared_ptr<FileIO>& fileio, int kangle)
{
    VkBufferUsageFlags usage{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    int width = modelio->imageSize.x;
    int height = modelio->imageSize.y;
    int n_wave = modelio->n_wave;
    VkDeviceSize bufferSize = static_cast<VkDeviceSize>(width) * height * n_wave * sizeof(float);
    // 步骤 1：GPU 存储缓冲 → 主机可见缓冲（此处直接在映射内存上统计，不做 CPU 副本）。
    nvvk::Buffer pixelBuffer = modelio->m_pAlloc->createBuffer(bufferSize, usage,
                                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    m_pVirtual->bufferToBuffer(modelio, *(modelio->m_virtualio->m_pBufferStorage), bufferSize, pixelBuffer);

    // write the buffer to disk
    void *data = modelio->m_pAlloc->map(pixelBuffer);
    float *pData = reinterpret_cast<float *>(data);

    // albedo.txt 口径（说明，未改算法）: per band, the mean of the raw top-of-canopy
    // image over pixels with value > 0 — no atmosphere correction (unlike the TIFF
    // products).  Zero pixels are excluded because a camera ray that misses the
    // scene with the skybox off leaves 0 (offline.rgen), indistinguishable here
    // from a genuinely black/shadowed sample; the mean is therefore an upper
    // estimate when the view contains true zero-valued surfaces.
    std::vector<float> meanValues(static_cast<size_t>(n_wave), 0.0f);
    float *walker = pData;
    for (int kband = 0; kband < n_wave; kband++) {
        double sum = 0.0;
        size_t count = 0;
        for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
            if (walker[i] > 0.0f) {
                sum += walker[i];
                ++count;
            }
        }
        meanValues[static_cast<size_t>(kband)] = count == 0
            ? 0.0f : static_cast<float>(sum / count);
        walker += static_cast<size_t>(width) * height;
    }

    modelio->m_pAlloc->unmap(pixelBuffer);
    modelio->m_pAlloc->destroy(pixelBuffer);

    Angle angle = modelio->angles[kangle];
    std::vector<float> waves = modelio->waves;

    // 步骤 4：以追加方式写 albedo.txt（下面的 1~4 序号是原作者自己的流程说明）。
    // 1. 打开文件（追加模式：std::ios::app）
    std::ofstream outfile(modelio->projectDir + "\\albedo.txt", std::ios::app);  // 如果文件不存在会自动创建

    // 2. 检查文件是否成功打开
    if (!outfile.is_open()) {
        std::cerr << "Error: Could not open file " << modelio->projectDir + "\\albedo.txt" << std::endl;
        return;
    }

    // 3. 写入数据（每行一个数）
    for (int num=0; num < n_wave; num++) {
        outfile << waves[num] << " " << angle.sza << " " << angle.saa << " " <<  angle.vza << " " << angle.vaa << " " << meanValues[static_cast<size_t>(num)] << "\n";  // 换行分隔
    }

    // 4. 关闭文件（析构函数会自动调用，但显式关闭更安全）
    outfile.close();


}


// 模块：destroy——释放 Vulkan 资源：先各子模块（缓冲/描述符/管线/命令），
// 再 SBT 包装器与加速结构构建器；等待设备空闲后释放命令池与分配器。
bool Raytracing::destroy( std::shared_ptr<RaytracingIO> &raytracingio)
{

    // Wait before destroying anything that may still be referenced by an
    // in-flight command buffer.
    vkDeviceWaitIdle(raytracingio->m_device);
    // Pipeline (and its pipeline layout) references the descriptor-set layout;
    // destroy it before the descriptor module, otherwise the layout outlives
    // its last reference.
    m_pPipeline->destroy(raytracingio);
    m_pDescriptor->destroy(raytracingio);
    m_pBuffer->destroy(raytracingio);
    m_pCommand->destroy(raytracingio);

    raytracingio->m_sbtWrapper.destroy();
    raytracingio->m_pAccelStruct->m_rtBuilder.destroy();

    raytracingio->m_genCmdBuf.deinit();
//    vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
//    vkDestroyCommandPool(m_device, m_cmdPool, nullptr);
    raytracingio->m_pAlloc->deinit();

    return true;
}
