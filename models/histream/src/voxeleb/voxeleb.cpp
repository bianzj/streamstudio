// ==========================================================================
// 文件：voxeleb.cpp
// 功能：VoxelEB 模式（体元能量平衡）的顶层流程实现：初始化、数据上传、逐时间节点求解与输出
// 主要模块/步骤：
//   1. 匿名命名空间工具：光学/热红外波长判别、普朗克反演亮温、波段名称
//   2. setup / upload / uploadSetting / updateSetting：初始化 Vulkan 与分配器，读入工程设置
//   3. uploadMeteo / updateMeteo / applyShortwaveSplit：气象序列加载，逐节点更新 GPU 气象缓冲
//      与短波直散分离
//   4. create / run / runObserver / destroy：创建 GPU 资源、能量平衡与辐射求解、观测输出、释放
//   5. output / outputTxt / outputVoxel / outputFluidSlices 等：结果写出
// 数据流：FileIO（工程 XML、气象）→ VoxelebIO（CPU 数据）→ buffer/descriptor/pipeline/command
//   创建 GPU 资源 → 逐时间节点更新气象并求解 → 输出影像/体元/能量收支
// 与其他文件的关系：声明见 voxeleb.h，状态见 voxelebio.h；场景/几何/成分由 base/ 下
//   Scene、Geometry、Compo 构建；短波直散分离见 base/shortwave_split.h
// 约定：长度 m、气象温度 Ta 以 °C 存储并在送入 GPU 时加 273.15 转为 K（待核对）、
//   土壤温度 K；波长 ≤ 2500 nm 视为光学波段（输出反射率），其余为热红外
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>
#include <gdal_priv.h>
#include "src/base/projectjson.h"
#include "voxeleb.h"
#include "src/base/atmosphere_lut.h"
#include "src/base/energy_output.h"
#include "src/base/shortwave_split.h"
#include "src/base/voxelflags.h"
#include "src/base/voxelmerge_stats.h"

namespace {

// 作用：判断波长是否属于光学（反射）波段。波长 ≤ 2.5 视为微米并换算为 nm，
//   换算后 ≤ 2500 nm 即为光学波段，其余按热红外处理。
bool isOpticalWavelength(float wavelength)
{
    const float wavelengthNanometers = wavelength <= 2.5f
        ? wavelength * 1000.0f : wavelength;
    return wavelengthNanometers <= 2500.0f;
}

// 作用：由单色辐亮度反演亮温（普朗克函数的逆）。
// 输入：波长（> 50 视为 nm，否则视为 µm）、辐亮度（W m-2 sr-1 µm-1）；输出：亮温 K，
//   辐亮度非有限或 ≤ 0 时返回 0。
// 说明：c1 = 2hc²（W µm4 m-2 sr-1），c2 = hc/k（µm·K），T = c2 / (λ·ln(c1/(L·λ^5) + 1))。
float inversePlanckTemperature(float wavelengthNanometers, float radiance)
{
    if (!std::isfinite(radiance) || radiance <= 0.0f) {
        return 0.0f;
    }
    constexpr double c1 = 1.191043934e8;
    constexpr double c2 = 14388.291040;
    const double wavelengthMicrometers = wavelengthNanometers > 50.0f
        ? static_cast<double>(wavelengthNanometers) / 1000.0
        : static_cast<double>(wavelengthNanometers);
    const double denominator = wavelengthMicrometers *
        std::log(c1 /
            (static_cast<double>(radiance) *
             std::pow(wavelengthMicrometers, 5.0)) + 1.0);
    return denominator > 0.0
        ? static_cast<float>(c2 / denominator)
        : 0.0f;
}

// 作用：生成各波段的输出名称与单位：光学波段为反射率，热红外波段按
//   temperatureOutput 选择亮温（K）或光谱辐亮度。
std::vector<std::string> voxelebBandNames(const std::vector<float>& waves,
                                          bool temperatureOutput)
{
    std::vector<std::string> names;
    names.reserve(waves.size());
    for (const float wave : waves) {
        std::ostringstream label;
        if (isOpticalWavelength(wave)) {
            label << "Reflectance [-] @ " << wave << " nm";
        } else if (temperatureOutput) {
            label << "Brightness temperature [K] @ " << wave << " nm";
        } else {
            label << "Spectral radiance [W m-2 sr-1 um-1] @ " << wave << " nm";
        }
        names.push_back(label.str());
    }
    return names;
}

} // namespace

// 模块：初始化。从 appsetting 取 Vulkan 设备/队列，初始化命令池、资源分配器、调试工具，
//   查询光追属性，并按需设置 SBT 与加速结构构建器。
bool Voxeleb::setup(AppSetting &appsetting, std::shared_ptr<VoxelebIO> &modelio){


    modelio->m_device = appsetting.m_context.m_device;
    modelio->m_physicalDevice = appsetting.m_context.m_physicalDevice;
    modelio->m_instance = appsetting.m_context.m_instance;
    modelio->m_queues = appsetting.m_queues;
    modelio->m_queue =  modelio->m_queues[eGCT].queue;
    modelio->m_queueIndex = modelio->m_queues[eGCT].familyIndex;
    //    m_instance = appSetting.m_context.m_instance;
//    m_device = appSetting.m_context.m_device;
//    m_physicalDevice = appSetting.m_context.m_physicalDevice;
//    m_queues = appSetting.m_queues;


//    VkCommandPoolCreateInfo poolCreateInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
//    poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
//    vkCreateCommandPool(modelio->m_device, &poolCreateInfo, nullptr, &modelio->m_cmdPool);

    modelio->m_genCmdBuf.init(modelio->m_device,modelio->m_queueIndex);


//    VkPipelineCacheCreateInfo pipelineCacheInfo{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
//    vkCreatePipelineCache(modelio->m_device, &pipelineCacheInfo, nullptr, &modelio->m_pipelineCache);

    modelio->m_pAlloc  = std::make_shared<Allocator>();
    modelio->m_pAlloc->init(modelio->m_instance, modelio->m_device, modelio->m_physicalDevice);
    modelio->m_debug.setup(modelio->m_device);

    VkPhysicalDeviceProperties2 rayTracingProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    rayTracingProperties.pNext = &(modelio->m_rtProperties);
    vkGetPhysicalDeviceProperties2(modelio->m_physicalDevice, &rayTracingProperties);

    if (modelio->useSBTWrapper)
    {
        modelio->m_sbtWrapper.setup(modelio->m_device, modelio->m_queueIndex, modelio->m_pAlloc.get(), modelio->m_rtProperties);
    }

    modelio->m_pAccelStruct->m_rtBuilder.setup(modelio->m_device, modelio->m_pAlloc.get(),modelio->m_queueIndex);



    return true;

}




// 模块：读入工程数据。顺序：defined 参数 → 成分属性 → 设置 → 场景 → 几何 → 气象 → 气溶胶；
//   设置必须先于场景构建，因为 hex 提取开关在场景构建时使用。
bool Voxeleb::upload(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &modelio){

    // auto & fileio = modelio->m_fileio;
    // auto & meshio = modelio->m_meshio;

    uploadDefined(fileio,modelio);
    m_pCompo->createCompProperty(fileio, modelio);
    // Hex extraction is part of scene construction, therefore its control
    // flag must be loaded before createPrimObjScene.
    uploadSetting(fileio, modelio);
    m_pScene->createPrimObjScene(fileio, modelio);
    // HISTREAM_VOXEL_MERGE_STATS=1|only: read-only two-level (fixscale) potential table; no behaviour change.
    voxelmerge::report(modelio, "VoxelEB");
    m_pGeometry->createGeometry(fileio,modelio);
//    defineOPO(modelio);
    uploadMeteo(fileio,modelio);
    uploadAero(fileio, modelio);

    return true;
}

// 模块：从工程 XML（sensor/setting/light/fluid/scene）复制运行设置到 VoxelebIO。
bool Voxeleb::uploadSetting(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &modelio) {

    modelio->n_wave = fileio->m_pVoxelebXml->sensorxml.waves.size();
    modelio->n_angle = fileio->m_pVoxelebXml->sensorxml.viewAngles.size();
    modelio->isTemperature =  fileio->m_pVoxelebXml->sensorxml.isTemperature;
    modelio->isDisplay = fileio->m_pVoxelebXml->sensorxml.isDisplay;
    modelio->isAlbedo = fileio->m_pVoxelebXml->sensorxml.isAlbedo;
    modelio->isImage = fileio->m_pVoxelebXml->sensorxml.isImage;
    modelio->isRadiationProcess = fileio->m_pVoxelebXml->sensorxml.isRadiationProcess;
    modelio->isEnergyProcess = fileio->m_pVoxelebXml->sensorxml.isEnergyProcess;
    // 兼容旧工程：只给出笼统的 isProcess 时按能量过程处理。
    if (!modelio->isRadiationProcess && !modelio->isEnergyProcess &&
        fileio->m_pVoxelebXml->sensorxml.isProcess) {
        modelio->isEnergyProcess = true;
    }
    modelio->isProcess = modelio->isRadiationProcess || modelio->isEnergyProcess ||
                         fileio->m_pVoxelebXml->sensorxml.isProcess;
    modelio->imageSize = fileio->m_pVoxelebXml->sensorxml.resolution;
    modelio->maxDepth = fileio->m_pVoxelebXml->settingxml.maxDepth;
    modelio->n_sample = fileio->m_pVoxelebXml->settingxml.n_sample;
    modelio->heterogeneousVoxel = fileio->m_pVoxelebXml->settingxml.heterogeneousVoxel;
    modelio->periodicNeighborCount =
        fileio->m_pVoxelebXml->settingxml.periodicNeighborCount;
    modelio->skyboxEnabled = fileio->m_pVoxelebXml->settingxml.skyboxEnabled;
    modelio->setting.n_jump = fileio->m_pVoxelebXml->settingxml.spectralAccelerationWidth;
    modelio->vegetationTemperatureMethod =
        fileio->m_pVoxelebXml->settingxml.vegetationTemperatureMethod;
    modelio->shortwaveScatteringOrders =
        fileio->m_pVoxelebXml->settingxml.shortwaveScatteringOrders;
    modelio->setting.maxStep = fileio->m_pVoxelebXml->settingxml.radiationMaxSteps;
    modelio->voxelFlags = voxelflags::resolve(fileio->m_pVoxelebXml->settingxml.voxelDirectionRotation,
        fileio->m_pVoxelebXml->settingxml.voxelDirectionRotationBlock, fileio->m_pVoxelebXml->settingxml.voxelEmptySkip,
        fileio->m_pVoxelebXml->settingxml.voxelProxyGeometry);
    modelio->diffuseFractionMode = fileio->m_pVoxelebXml->lightxml.diffuseFractionMode;
    // 固定直射比例限幅到 [0, 1]；nodeDirectFraction 初值同它，erbs 模式下逐节点更新。
    modelio->fixedDirectFraction = std::clamp(fileio->m_pVoxelebXml->lightxml.direct, 0.0f, 1.0f);
    modelio->nodeDirectFraction = modelio->fixedDirectFraction;
    modelio->fluid = fileio->m_pVoxelebXml->fluidxml;
    // The local-exchange closure replaces the surface-layer profile by a
    // local-wind bulk formula, so it has no scene surface layer to correct.
    modelio->stabilityCorrection = fileio->m_pVoxelebXml->settingxml.stabilityCorrection;
    modelio->urbanCanyon = fileio->m_pVoxelebXml->settingxml.urbanCanyon;
    if (modelio->stabilityCorrection && modelio->fluid.localExchange) {
        std::cout << "STABILITY_CORRECTION\tdisabled\treason=fluid.localExchange uses a local-wind bulk closure" << std::endl;
        modelio->stabilityCorrection = false;
    }
    modelio->terrainEnvironment = fileio->m_pVoxelebXml->scenexml.background.terrainEnvironment;

    return true;
}

// 模块：把 VoxelebIO 中的设置同步到推送常量 setting（VoxelLstSetting）。
// 说明：setting.voxelSize 的竖直分量被改写为按场景高度和表面步长取整的格数。
bool Voxeleb::updateSetting(std::shared_ptr<VoxelebIO> &modelio){

    // auto &opo = modelio->m_opo;

    // auto &sceneio = modelio->m_sceneio;

    modelio->setting.imageSize = modelio->imageSize;
    modelio->setting.n_wave = modelio->n_wave;
    modelio->setting.scale = modelio->stepsize_surface;
   // modelio->setting.isTemperature = modelio->isTemperature;
    modelio->setting.isDisplay = modelio->isDisplay;
    // Voxel transport options from HISTREAM_VOXEL_DIRROT / HISTREAM_VOXEL_SKIP (src/base/voxelflags.h).
    modelio->setting.voxelFlags = modelio->voxelFlags;
    modelio->setting.maxDepth = modelio->maxDepth;
    modelio->setting.n_sample = modelio->n_sample;
    modelio->setting.voxelSize = modelio->voxelSize_XZY;
    // Vertical cell count bounds the turbid-cell traversal in the shaders.
    modelio->setting.voxelSize.y = std::max(1, static_cast<int>(std::ceil(
        modelio->sceneSize_XYZ.z / std::max(0.01f, modelio->stepsize_surface))));
    modelio->setting.voxelCount = voxelflags::testRadianceVoxels(modelio->n_voxel);
    modelio->setting.periodicNeighborCount = modelio->periodicNeighborCount;
    modelio->setting.skyboxEnabled = modelio->skyboxEnabled ? 1 : 0;


    return true;
    //modelio->setting.maxDepth = fileio->m_pXmlInput.
}


// 模块：读入气象序列（meteos、atomconds）及直散分离所需数据，备份到 m_stableMeteos。
// 说明：初始土壤温度 Tsold 仅在 150–400 K 内才采用，否则保持 NaN（即沿用气温）。
bool  Voxeleb::uploadMeteo(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &modelio){


   // Utils::readascfileinout(meteofile,0,1,)
   modelio->startTimeNode = fileio->m_pVoxelebXml->meteoxml.startTimeNode;
   modelio->endTimeNode = fileio->m_pVoxelebXml->meteoxml.endTimeNode;
   const float initialSoilTemperature = fileio->m_pVoxelebXml->meteoxml.meta.Tsold;
   if (std::isfinite(initialSoilTemperature) && initialSoilTemperature >= 150.0f
       && initialSoilTemperature <= 400.0f) {
       modelio->initialSoilTemperature = initialSoilTemperature;
   }
   fileio->readMeteo(modelio->m_defined,modelio->n_node,modelio->meteos,modelio->atomconds);
   modelio->meteoMeasuredSplit = fileio->m_meteoMeasuredSplit;
   modelio->unitShortwaveShapes = fileio->m_unitShortwaveShapes;
   m_stableMeteos = modelio->meteos;
   return true;
}

// 模块：切换到第 knode 个时间节点的气象，并写入 GPU。
// 步骤 1：检查节点范围，越界抛异常。
// 步骤 2：用 vkCmdUpdateBuffer 更新气象缓冲；若启用流体，同时重算流体子步数与环境风参数。
// 步骤 3：加传输→计算着色器的缓冲屏障，提交并等待完成。
bool Voxeleb::updateMeteo(std::shared_ptr<VoxelebIO> &modelio, int knode){

    if (knode < 0 || knode >= static_cast<int>(m_stableMeteos.size())) {
        throw std::out_of_range("VoxelEB meteorology node " + std::to_string(knode) +
                                " is outside the loaded series (" +
                                std::to_string(m_stableMeteos.size()) + ")");
    }
    modelio->meteo = m_stableMeteos[knode];

    nvvk::CommandPool cmdBufGet(modelio->m_device, modelio->m_queueIndex);
    vk::CommandBuffer cmdBuf = cmdBufGet.createCommandBuffer();
    vkCmdUpdateBuffer(cmdBuf, (*modelio->m_pMeteoBuffer).buffer, 0, sizeof(Meteo), &modelio->meteo);
    if (modelio->m_pFluidParameters) {
        // 流体子步数 = max(标量时间步给出的子步数, 流风穿越一个计算域所需子步数（上限 4096）)；
        //   时间间隔下限 0.1，风速下限 0。子步长 = interval / 子步数（dTime 单位待核对，推测为 s）。
        const float interval = std::max(0.1f, modelio->meteo.dTime);
        const float windSpeed = std::max(0.0f, modelio->meteo.u);
        const int scalarSubsteps = std::max(1, static_cast<int>(std::ceil(
            interval / std::max(0.001f, modelio->fluid.timeStep))));
        const float velocityScale = std::max(1.0f, modelio->fluidParameters.sources.w);
        const float latticeWindSpeed = windSpeed / velocityScale;
        const int domainCells = std::max(modelio->fluidGridSize.x, modelio->fluidGridSize.z);
        const int flowSubsteps = windSpeed > 0.001f
            ? static_cast<int>(std::min(4096.0,
                std::ceil(static_cast<double>(domainCells)
                    / std::max(0.01, static_cast<double>(latticeWindSpeed)))))
            : 1;
        // One domain crossing is normally sufficient for this simple steady
        // urban-flow update. Cap only this convergence estimate; an explicitly
        // requested scalar time step is still honored.
        modelio->fluidSubsteps = std::max(scalarSubsteps, flowSubsteps);
        modelio->fluidParameters.spacingTime.w = interval / modelio->fluidSubsteps;
        // 环境风：x = u·sin(风向)，z = u·cos(风向)，w = 气温 Ta + 273.15（°C → K）。
        //   风向的角度约定（来向/去向）待核对。
        const float direction = glm::radians(modelio->fluid.windDirection);
        modelio->fluidParameters.ambientWind = glm::vec4(
            windSpeed * std::sin(direction), 0.0f,
            windSpeed * std::cos(direction), modelio->meteo.Ta + 273.15f);
        vkCmdUpdateBuffer(cmdBuf, modelio->m_pFluidParameters->buffer, 0,
                          sizeof(FluidParameters), &modelio->fluidParameters);
    }
    // 屏障：保证 vkCmdUpdateBuffer（传输写）对之后的计算着色器读可见。
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = (*modelio->m_pMeteoBuffer).buffer;
    barrier.offset = 0;
    barrier.size = sizeof(Meteo);
    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 1, &barrier, 0, nullptr);
    if (modelio->m_pFluidParameters) {
        barrier.buffer = modelio->m_pFluidParameters->buffer;
        barrier.size = sizeof(FluidParameters);
        vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 1, &barrier, 0, nullptr);
    }
    cmdBufGet.submitAndWait(cmdBuf);

    return true;
}

// 模块：逐节点短波直散分离（light.diffuseFractionMode：0 固定比例，1 erbs 模型）。
// 步骤 1：取天顶角（首个角度的 sza；无角度时取 180°，视为夜间）与年积日。
// 步骤 2：调用 shortwave_split::split 得到直射比例并打印 SHORTWAVE_SPLIT 日志。
// 步骤 3：固定模式到此为止；erbs 模式下更新 LightSet 与 VNIR 的 atomconds 并重传 GPU。
void Voxeleb::applyShortwaveSplit(std::shared_ptr<VoxelebIO> &modelio, int knode)
{
    const float zenith = modelio->angles.empty() ? 180.0f : modelio->angles.front().sza;
    const int dayOfYear = static_cast<int>(std::floor(modelio->meteo.t));
    std::array<float, 2> measured{std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::quiet_NaN()};
    if (knode >= 0 && knode < static_cast<int>(modelio->meteoMeasuredSplit.size()))
        measured = modelio->meteoMeasuredSplit[static_cast<size_t>(knode)];
    const shortwave_split::Split split = shortwave_split::split(
        modelio->diffuseFractionMode, modelio->fixedDirectFraction, modelio->meteo.Rin,
        zenith, dayOfYear, measured[0], measured[1]);
    std::cout << "SHORTWAVE_SPLIT\tnode=" << knode
              << "\tmode=" << (modelio->diffuseFractionMode == shortwave_split::kModeErbs ? "erbs" : "fixed")
              << "\tsource=" << split.source
              << "\trin=" << modelio->meteo.Rin
              << "\tsza=" << zenith
              << "\tkt=" << split.clearnessIndex
              << "\tdirectFraction=" << split.directFraction << std::endl;
    if (modelio->diffuseFractionMode != shortwave_split::kModeErbs) return;
    modelio->nodeDirectFraction = split.directFraction;
    if (modelio->unitShortwaveShapes.empty() || !modelio->m_pBufferAtomcond) {
        throw std::runtime_error("VoxelEB per-node shortwave split needs the incident spectrum shapes");
    }
    // Light: shaders read LightSet.direct/diffuse (MWIR solar, low-sun merge).
    modelio->light.direct = split.directFraction;
    modelio->light.diffuse = 1.0f - split.directFraction;
    if (!modelio->angles.empty()) m_pGeometry->updateAngle(modelio, 0);
    // VNIR coefficients: fraction-1 shapes times this node's split.
    const size_t count = std::min(modelio->unitShortwaveShapes.size(), modelio->atomconds.size());
    for (size_t i = 0; i < count; ++i) {
        modelio->atomconds[i].direct = modelio->unitShortwaveShapes[i].direct * split.directFraction;
        modelio->atomconds[i].diffuse = modelio->unitShortwaveShapes[i].diffuse * (1.0f - split.directFraction);
    }
    // 把更新后的 atomconds 写回 GPU 缓冲，vkCmdUpdateBuffer 每次最多 65536 字节，故分块。
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(count * sizeof(AtomCond));
    nvvk::CommandPool pool(modelio->m_device, modelio->m_queueIndex);
    vk::CommandBuffer command = pool.createCommandBuffer();
    // vkCmdUpdateBuffer accepts at most 65536 bytes per call.
    for (VkDeviceSize offset = 0; offset < bytes; offset += 65536) {
        const VkDeviceSize chunk = std::min<VkDeviceSize>(65536, bytes - offset);
        vkCmdUpdateBuffer(command, modelio->m_pBufferAtomcond->buffer, offset, chunk,
                          reinterpret_cast<const char*>(modelio->atomconds.data()) + offset);
    }
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    pool.submitAndWait(command);
}

// 模块：读入 defined 参数（材质/物种等自定义库，待核对）到 modelio->m_defined。
bool Voxeleb::uploadDefined(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &modelio)
{

    fileio->readDefined(modelio->m_defined);

    return true;
}

// 模块：创建 GPU 资源。顺序：缓冲 → 描述符 → 管线（失败则返回 false）→ 命令/同步对象，
//   最后调用 updateSetting 同步推送常量。
bool Voxeleb::create(std::shared_ptr<VoxelebIO> &modelio) {


    m_pBuffer->createBuffer(modelio);
    m_pDescriptor->createDescriptor(modelio);
    if (!m_pPipeline->createPipeline(modelio)) {
        return false;
    }
    m_pCommand->create(modelio);
    updateSetting(modelio);
    return true;
}

// 模块：VoxelEB 主流程，逐时间节点做能量平衡并输出观测。
// 步骤 1：若设置了环境变量 HISTREAM_OBSERVER_MANIFEST，转入 runObserver。
// 步骤 2：启用地形环境时，写 diagnostics/terrain-environment.json 记录输入与模型假设。
// 步骤 3：清空并重写 result_statistics.txt 表头。
// 步骤 4：对 [startTimeNode, endTimeNode) 内每个节点：更新气象/太阳位置/短波直散分离 →
//   （可选）流体推进 → 能量平衡 runEB → 输出体元与逐角度观测影像。
// 返回值：成功 true；runEB 失败时返回 false。
bool Voxeleb::run(std::shared_ptr<VoxelebIO> &modelio, std::shared_ptr<FileIO> &fileio) {
    if (const char* observer = std::getenv("HISTREAM_OBSERVER_MANIFEST")) {
        return runObserver(modelio, fileio, observer);
    }

    // 步骤 2：地形环境诊断。JSON 中的 *Solved/*Coupled 等字段（均为 false）标明这些过程未求解。
    const auto& environment = modelio->terrainEnvironment;
    if (environment.airEnabled || environment.soilEnabled || environment.co2ValleyExcess != 0) {
        const std::filesystem::path directory = std::filesystem::path(modelio->projectDir) / "diagnostics";
        std::filesystem::create_directories(directory);
        const ProjectJson::Json metadata = {
            {"kind", "terrain-environment-input"}, {"mode", "eVoxelEB"},
            {"heightDatum", "DEM minimum; flat ground when no DEM"},
            {"airEnabled", environment.airEnabled}, {"soilEnabled", environment.soilEnabled},
            {"referenceHeight_m", environment.referenceHeight},
            {"temperatureLapseRate_K_per_m", environment.temperatureLapseRate},
            {"vaporScaleHeight_m", environment.vaporScaleHeight},
            {"drainageHeight_m", environment.drainageHeight},
            {"soilDry_m3_per_m3", environment.soilDry}, {"soilValley_m3_per_m3", environment.soilValley},
            {"soilScaleHeight_m", environment.soilScaleHeight},
            {"co2ValleyExcess_ppm", environment.co2ValleyExcess},
            {"co2ScaleHeight_m", environment.co2ScaleHeight},
            {"co2TransportSolved", false},
            {"localExchangeClosure", modelio->fluid.localExchange},
            {"airSamplingHeightRange_m", {modelio->terrainAirHeightRange.x, modelio->terrainAirHeightRange.y}},
            {"drainageModel", "prescribed common elevation; not routed HAND"},
            {"soilWaterBalance", false}, {"evaporationVaporSourceCoupled", false},
            {"fluidEnabled", modelio->fluid.enabled}, {"siteCalibrationVerified", false}
        };
        std::ofstream stream(directory / "terrain-environment.json", std::ios::trunc);
        stream << metadata.dump(2) << '\n';
        stream.close();
        if (!stream) throw std::runtime_error("Cannot write terrain environment metadata");
    }


// for(int knode = 72; knode < 75;knode = knode + 1) {
    // 步骤 3：重置统计文件。
    /// 清除txt文件信息
    // 以写入模式打开文件（std::ios::trunc 会清空文件）
    std::ofstream file(modelio->projectDir + "\\result_statistics.txt", std::ios::trunc);
    // 检查是否成功打开
    if (!file.is_open()) {
        std::cerr << "Error: Could not clear file " << modelio->projectDir + "\\result_statistics.txt" << std::endl;
    }
    file << "t SZA SAA VZA VAA wavelength "
         << (modelio->isTemperature ? "Temperature_K" : "Radiance")
         << "\n";
    // 文件内容已被清空，无需额外操作
    file.close(); // 显式关闭（可选）


    std::cout << "PROGRESS\t5\t加载体元场景与气象驱动" << std::endl;
    std::cout << "PROGRESS\t10\t温度方法：土壤按材质 0/1/2，植被="
              << (modelio->vegetationTemperatureMethod == 0
                      ? "Ball-Berry 经验法" : "Farquhar 机制法")
              << std::endl;

    //modelio->startTimeNode = 63;
    //modelio->endTimeNode = 74;
    // 步骤 4：时间节点循环（半开区间 [startTimeNode, endTimeNode)）。
    for(int knode = modelio->startTimeNode; knode < modelio->endTimeNode;knode = knode +1) {
    // for(int knode = 0; knode < 144;knode = knode + 1) {
        modelio->k_node = knode;

        updateMeteo(modelio, knode);

        // Energy balance must use the solar position of the current meteo node.
        if (!modelio->angles.empty()) {
            m_pGeometry->updateAngle(modelio, 0);
        }
        applyShortwaveSplit(modelio, knode);

        // Explicit staggered coupling: advance the atmospheric field with the
        // latest surface temperatures, then use that wind/air state in the
        // current energy-balance solve. Updated surfaces feed the next node.
        if (modelio->fluid.enabled) {
            m_pCommand->runFluid(modelio, modelio->fluidSubsteps);
        }

        // 步骤 5：本节点能量平衡求解（GPU）；失败则中止整个运行。
        if (!m_pCommand->runEB(modelio)) return false;

        // 诊断（仅在设置环境变量时）：回读 VoxelTempe，按网格类型（0..5）统计向阳/背阴温度极值，
        //   输出 TEMPERATURE_RANGE 日志。
        if (std::getenv("HISTREAM_TEMPERATURE_DIAGNOSTICS") != nullptr &&
            modelio->m_voxelio->m_pTempeBuffer) {
            const size_t count = static_cast<size_t>(modelio->n_voxel);
            std::vector<VoxelTempe> temperatures(count);
            const VkDeviceSize bytes = count * sizeof(VoxelTempe);
            nvvk::Buffer staging = modelio->m_pAlloc->createBuffer(
                bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            m_pVirtual->bufferToBuffer(modelio, *modelio->m_voxelio->m_pTempeBuffer,
                                       bytes, staging);
            void* mapped = modelio->m_pAlloc->map(staging);
            std::memcpy(temperatures.data(), mapped, static_cast<size_t>(bytes));
            modelio->m_pAlloc->unmap(staging);
            modelio->m_pAlloc->destroy(staging);

            float minima[6], maxima[6];
            size_t typeCounts[6]{};
            std::fill_n(minima, 6, std::numeric_limits<float>::max());
            std::fill_n(maxima, 6, std::numeric_limits<float>::lowest());
            for (size_t index = 0; index < count && index < modelio->m_voxelio->voxellinks.size(); ++index) {
                const VoxelLink& voxel = modelio->m_voxelio->voxellinks[index];
                if (voxel.instanceId < 0 || voxel.instanceId >= static_cast<int>(modelio->m_instanceio->instanceLinks.size())) continue;
                const uint32_t meshId = modelio->m_instanceio->instanceLinks[voxel.instanceId].meshId;
                if (meshId >= modelio->m_meshio->meshLinks.size()) continue;
                const int type = modelio->m_meshio->meshLinks[meshId].type;
                if (type < 0 || type >= 6) continue;
                minima[type] = std::min(minima[type], std::min(temperatures[index].sunlit, temperatures[index].shaded));
                maxima[type] = std::max(maxima[type], std::max(temperatures[index].sunlit, temperatures[index].shaded));
                ++typeCounts[type];
            }
            for (int type = 0; type < 6; ++type) {
                if (typeCounts[type] == 0) continue;
                std::cout << "TEMPERATURE_RANGE\ttype=" << type << "\tcount="
                          << typeCounts[type] << "\tmin=" << minima[type]
                          << "\tmax=" << maxima[type] << std::endl;
            }
        }

        if (modelio->fluid.enabled) {
            outputFluidSlices(modelio);
        }

        // 进度：10%–95% 按已完成节点数线性映射。
        const int completed = knode - modelio->startTimeNode + 1;
        const int total = std::max(1, modelio->endTimeNode - modelio->startTimeNode);
        const int progress = 10 + 85 * completed / total;
        std::cout << "PROGRESS\t" << progress << "\t能量平衡时间节点 "
                  << completed << "/" << total << " T="
                  << std::to_string(modelio->meteo.t) << std::endl;
        // Validate the completed GPU state even when process export is disabled.
        outputVoxel(modelio,fileio);
    //
    //    if (knode == 73)
        // if (1)
        {
        // 步骤 6：观测。常规模式逐角度观测；UAV 巡航模式每个节点只观测 1 次，
        //   航点按 (knode - startTimeNode) % n_pos 轮换。
        const int observationCount = modelio->isUAVTrave ? 1 : modelio->n_angle;
        const int cruisePosition = modelio->isUAVTrave && modelio->n_pos > 0
            ? (knode - modelio->startTimeNode) % modelio->n_pos : -1;
        for (int kangle = 0; kangle < observationCount; kangle++) {
            modelio->k_angle = kangle;
            if (cruisePosition >= 0) {
                modelio->k_pos = cruisePosition;
                m_pGeometry->updateSensorPos(modelio, cruisePosition);
            } else {
                m_pGeometry->updateAngle(modelio,kangle);
            }

            Angle angle = modelio->angles[kangle];

            ////updateSetting(modelio);
            // 步骤 7：对当前观测几何做辐射传输（Command::runRT），随后输出。
            m_pCommand->runRT(modelio);

    //        std::cout << "Success: " << kangle << std::endl;


                std::cout << "OBSERVATION\t"
                      << "    vza_" << std::to_string(angle.vza) << "    vaa_" << std::to_string(angle.vaa)
                      << "    sza_" << std::to_string(angle.sza) << "    saa_" << std::to_string(angle.saa) << std::endl;
                if (cruisePosition >= 0) {
                    const glm::vec3 position = modelio->uavposes[cruisePosition];
                    std::cout << "OBSERVATION\t航点 " << (cruisePosition + 1) << "/" << modelio->n_pos
                              << " X=" << position.x << " Y=" << position.y
                              << " H=" << position.z << std::endl;
                }

                // isImage 为真时写出影像 TIFF；outputTxt 总是追加统计行。
                if (modelio->isImage)
                {
                    output(modelio,fileio,knode, kangle, cruisePosition);
                }

                outputTxt(modelio, fileio, kangle);
            }
            // output(modelio,fileio,knode, kangle);
        }

    }
    std::cout << "PROGRESS\t100\t体元能量平衡计算完成" << std::endl;
    return true;
}



// 模块：释放资源。先 vkDeviceWaitIdle，再依次销毁缓冲、SBT、加速结构、描述符、管线、
//   命令对象、命令池与分配器。注意：函数末尾固定返回 false（见 return 处）。
bool Voxeleb::destroy(std::shared_ptr<VoxelebIO> & modelio){

    const VkResult waitIdleResult = vkDeviceWaitIdle(modelio->m_device);
    if (waitIdleResult != VK_SUCCESS) {
        std::cerr << "Voxeleb::destroy: vkDeviceWaitIdle failed, VkResult="
                  << static_cast<int>(waitIdleResult) << std::endl;
    }
    m_pBuffer->destroy(modelio);
    modelio->m_sbtWrapper.destroy();
    modelio->m_pAccelStruct->m_rtBuilder.destroy();

    m_pDescriptor->destroy(modelio);
    m_pPipeline->destroy(modelio);
    m_pCommand->destroy(modelio);

    modelio->m_genCmdBuf.deinit();
    modelio->m_pAlloc->deinit();
    
    return false;

}



// 模块：把当前观测角度的影像写成 TIFF。
// 输入：knode 时间节点，kangle 观测角度序号，kpos 航点序号（-1 为非巡航）。
// 步骤 1：检查影像尺寸/波段/角度序号合法，否则直接返回。
// 步骤 2：把 GPU 输出缓冲回读到主机（布局：波段主序，[band][y][x]，每波段 imageElements 个 float）。
// 步骤 3：逐像元选取源像元（正射投影时按 orthcorrect 的多项式映射，越界跳过）。
// 步骤 4：逐波段取值：天空像元且无有效信号 → 大气模型填天空值；其余 → 大气订正，
//   热红外且 isTemperature 时由辐亮度反演亮温（inversePlanckTemperature）。
// 步骤 5：写 TIFF（大气启用时后缀 _v_a，否则 _v）。
void Voxeleb::output(std::shared_ptr<VoxelebIO> &modelio, std::shared_ptr<FileIO> &fileio, int knode, int kangle, int kpos) {
    VkBufferUsageFlags usage{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const int width = modelio->imageSize.x;
    const int height = modelio->imageSize.y;
    const int nWave = modelio->n_wave;
    if (width <= 0 || height <= 0 || nWave <= 0
        || kangle < 0 || static_cast<size_t>(kangle) >= modelio->angles.size()) {
        return;
    }

    const size_t imageElements = static_cast<size_t>(width) * height;
    const size_t totalElements = imageElements * nWave;
    const VkDeviceSize bufferSize = totalElements * sizeof(float);
    nvvk::Buffer pixelBuffer = modelio->m_pAlloc->createBuffer(
        bufferSize, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    m_pVirtual->bufferToBuffer(
        modelio, *(modelio->m_virtualio->m_pBufferStorage), bufferSize, pixelBuffer);

    void* mappedData = modelio->m_pAlloc->map(pixelBuffer);
    std::vector<float> sourceData(totalElements);
    std::memcpy(sourceData.data(), mappedData, static_cast<size_t>(bufferSize));
    modelio->m_pAlloc->unmap(pixelBuffer);
    modelio->m_pAlloc->destroy(pixelBuffer);

    Angle angle = modelio->angles[kangle];
    const bool perspective = fileio->m_pVoxelebXml->sensorxml.projection == Projection::PERSPECTIVE;
    const float sensorHeight = kpos >= 0 && kpos < static_cast<int>(modelio->uavposes.size())
        ? modelio->uavposes[kpos].z : fileio->m_pVoxelebXml->sensorxml.position.z;
    std::vector<float> outputData(totalElements, 0.0f);
    const bool observerCapture = std::getenv("HISTREAM_OBSERVER_MANIFEST") != nullptr;
    std::vector<unsigned char> surfaceMask(observerCapture ? imageElements : 0, 0);
    Eigen::VectorXd cx;
    Eigen::VectorXd cy;
    // 正射投影：cx、cy 为 4 个系数，源像元 ii = cx0·i + cx1·j + cx2·i·j + cx3（取整），jj 同理。
    if (!perspective) m_pGeometry->orthcorrect(modelio, angle.vza, angle.vaa, cx, cy);

    for (int i = 0; i < width; ++i) {
        for (int j = 0; j < height; ++j) {
            const size_t destination = static_cast<size_t>(j) * width + i;
            size_t source = destination;
            if (!perspective) {
                const int ii = static_cast<int>(i * cx[0] + j * cx[1] + i * j * cx[2] + cx[3]);
                const int jj = static_cast<int>(i * cy[0] + j * cy[1] + i * j * cy[2] + cy[3]);
                if (ii < 0 || ii >= width || jj < 0 || jj >= height) continue;
                source = static_cast<size_t>(jj) * width + ii;
            }
            // 仅透视投影判断天空像元并给出该像元的天顶角（待核对该函数的角度约定）。
            float skyZenith = 0.0f;
            const bool isSkyPixel = perspective && atmosphereSkyPixelZenith(
                i, j, width, height,
                fileio->m_pVoxelebXml->sensorxml.sensorFov,
                angle.vza, angle.vaa, skyZenith);
            // A dark optical sample can be a real surface (especially at night).
            // A thermal hit rules out sky even if its optical radiance is zero.
            bool thermalSurfaceHit = false;
            for (int band = 0; band < nWave; ++band) {
                const float signal = sourceData[static_cast<size_t>(band) * imageElements + source];
                if (!isOpticalWavelength(modelio->waves[band]) && std::isfinite(signal) && signal > 0.0f)
                    thermalSurfaceHit = true;
            }
            if (observerCapture) surfaceMask[destination] = thermalSurfaceHit ? 1 : 0;
            for (int band = 0; band < nWave; ++band) {
                const size_t bandOffset = static_cast<size_t>(band) * imageElements;
                const float sourceValue = sourceData[bandOffset + source];
                float value = sourceValue;
                // 天空像元且源值无效或为 0：用大气模型填值；光学波段在夜间（Rin ≤ 0.5）置 0。
                if ((!std::isfinite(sourceValue) || sourceValue == 0.0f) && isSkyPixel && !thermalSurfaceHit) {
                    value = atmosphereSkyOutputValue(
                        fileio->m_pVoxelebXml->atmospherexml,
                        modelio->waves[band], sensorHeight, skyZenith,
                        modelio->isTemperature,
                        fileio->m_pVoxelebXml->lightxml.skyTemperature);
                    if (isOpticalWavelength(modelio->waves[band]) && modelio->meteo.Rin <= 0.5f)
                        value = 0.0f;
                } else {
                    value = atmosphereCorrectOutputValue(
                        sourceValue, fileio->m_pVoxelebXml->atmospherexml,
                        modelio->waves[band], sensorHeight, angle.vza, false);
                    if (modelio->isTemperature && !isOpticalWavelength(modelio->waves[band]) && value > 0.0f)
                        value = inversePlanckTemperature(modelio->waves[band], value);
                }
                if (value != 0.0f) outputData[bandOffset + destination] = value;
            }
        }
    }

    // 观测器模式下额外保存热红外有信号的“地表命中”掩膜（每像元 1 字节）。
    if (observerCapture) {
        const auto path = std::filesystem::path(modelio->projectDir) /
            ("surface_mask_P=" + std::to_string(kpos) + ".bin");
        std::ofstream mask(path, std::ios::binary | std::ios::trunc);
        mask.write(reinterpret_cast<const char*>(surfaceMask.data()),
                   static_cast<std::streamsize>(surfaceMask.size()));
        if (!mask) throw std::runtime_error("Cannot save observer surface-hit mask");
    }
    const float time = modelio->meteo.t;
    fileio->writeTIFData(
        modelio->projectDir, outputData.data(), width, height, nWave, angle, time,
        fileio->m_pVoxelebXml->atmospherexml.enabled ? "_v_a" : "_v", kpos, false,
        voxelebBandNames(modelio->waves, modelio->isTemperature));
}
// 模块：把当前观测角度各波段的影像平均值追加到 result_statistics.txt。
// 步骤 1：回读 GPU 输出缓冲（布局同 output）；步骤 2：按 orthcorrect 多项式做正射重采样
//   （与 output 不同，此处不区分透视投影）；热红外且 isTemperature 时反演亮温。
// 步骤 3：每波段只对值 > 0 的像元求平均（0 视为无数据）；步骤 4：按波段追加一行
//   “t sza saa vza vaa wavelength mean”（表头在 run 中写入）。
void Voxeleb::outputTxt(std::shared_ptr<VoxelebIO>& modelio, std::shared_ptr<FileIO>& fileio, int kangle)
{
    VkBufferUsageFlags usage{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const int width = modelio->imageSize.x;
    const int height = modelio->imageSize.y;
    const int nWave = modelio->n_wave;
    if (width <= 0 || height <= 0 || nWave <= 0
        || kangle < 0 || static_cast<size_t>(kangle) >= modelio->angles.size()) {
        return;
    }

    const size_t imageElements = static_cast<size_t>(width) * height;
    const size_t totalElements = imageElements * nWave;
    const VkDeviceSize bufferSize = totalElements * sizeof(float);
    nvvk::Buffer pixelBuffer = modelio->m_pAlloc->createBuffer(
        bufferSize, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    m_pVirtual->bufferToBuffer(
        modelio, *(modelio->m_virtualio->m_pBufferStorage), bufferSize, pixelBuffer);

    void* mappedData = modelio->m_pAlloc->map(pixelBuffer);
    std::vector<float> sourceData(totalElements);
    std::memcpy(sourceData.data(), mappedData, static_cast<size_t>(bufferSize));
    modelio->m_pAlloc->unmap(pixelBuffer);
    modelio->m_pAlloc->destroy(pixelBuffer);

    Angle angle = modelio->angles[kangle];
    Eigen::VectorXd cx;
    Eigen::VectorXd cy;
    m_pGeometry->orthcorrect(modelio, angle.vza, angle.vaa, cx, cy);

    std::vector<float> orthData(totalElements, 0.0f);
    for (int i = 0; i < width; ++i) {
        for (int j = 0; j < height; ++j) {
            const int ii = static_cast<int>(i * cx[0] + j * cx[1] + i * j * cx[2] + cx[3]);
            const int jj = static_cast<int>(i * cy[0] + j * cy[1] + i * j * cy[2] + cy[3]);
            if (ii < 0 || ii >= width || jj < 0 || jj >= height) {
                continue;
            }

            const size_t destination = static_cast<size_t>(j) * width + i;
            const size_t source = static_cast<size_t>(jj) * width + ii;
            for (int band = 0; band < nWave; ++band) {
                const size_t bandOffset = static_cast<size_t>(band) * imageElements;
                float value = sourceData[bandOffset + source];
                if (modelio->isTemperature && !isOpticalWavelength(modelio->waves[band])
                    && value > 0.0f) {
                    value = inversePlanckTemperature(
                        modelio->waves[band], value);
                }
                if (value != 0.0f) {
                    orthData[bandOffset + destination] = value;
                }
            }
        }
    }

    // 步骤 3：逐波段统计正值像元均值，无有效像元时为 0。
    const float time = modelio->meteo.t;
    std::vector<float> meanValues(static_cast<size_t>(nWave), 0.0f);
    for (int band = 0; band < nWave; ++band) {
        const float* bandBegin = orthData.data() + static_cast<size_t>(band) * imageElements;
        double sum = 0.0;
        size_t count = 0;
        for (size_t pixel = 0; pixel < imageElements; ++pixel) {
            const float value = bandBegin[pixel];
            if (value > 0.0f) {
                sum += value;
                ++count;
            }
        }
        meanValues[static_cast<size_t>(band)] = count == 0
            ? 0.0f : static_cast<float>(sum / count);
    }

    // 步骤 4：以追加方式写统计行；路径用 Windows 反斜杠拼接。
    std::ofstream outfile(modelio->projectDir + "\\result_statistics.txt", std::ios::app);
    if (!outfile.is_open()) {
        std::cerr << "Error: Could not open file "
                  << modelio->projectDir + "\\result_statistics.txt" << std::endl;
        return;
    }

    for (int band = 0; band < nWave; ++band) {
        outfile << time << " " << angle.sza << " " << angle.saa << " "
                << angle.vza << " " << angle.vaa << " " << modelio->waves[band] << " "
                << meanValues[static_cast<size_t>(band)]
                << "\n";
    }
}
// 模块：回读并校验本时间节点的体元状态，写出诊断文件（run 每节点调用，即使不导出过程数据）。
// 步骤 1：检查所需 GPU 缓冲存在，否则抛异常。
// 步骤 2：下载方向、净辐射、热通量、温度、光量子通量、气孔/表面阻抗缓冲到主机。
// 步骤 3：为每个有效体元调用 histream::energy_output::calculate 得到能量收支（Balance）。
// 步骤 4：必要时下载土壤剖面温度 TLAST。
// 步骤 5：写 diagnostics/physiology_node=*.json（及 .bin）：生理量统计与有效性检查，
//   发现非有限/无效状态则抛异常。
// 步骤 6：写 diagnostics/energy_balance_node=*.json：辐射/热通量收支、闭合残差、稳定度与
//   太阳可见性诊断。
// 输出的 .bin 为每体元 10 个 float32（小端）交错存放，偏移见 JSON 的 fields。
void Voxeleb::outputVoxel(std::shared_ptr<VoxelebIO> &modelio, std::shared_ptr<FileIO> &fileio) {
    (void)fileio;
    if (!modelio || modelio->n_voxel <= 0) return;
    if (!modelio->m_voxelio ||
        !modelio->m_voxelio->m_pDirBuffer || !modelio->m_voxelio->m_pNetRadBuffer ||
        !modelio->m_voxelio->m_pFluxBuffer ||
        !modelio->m_voxelio->m_pTempeBuffer ||
        !modelio->m_voxelio->m_pPnetBuffer || !modelio->m_voxelio->m_pRssBuffer) {
        throw std::runtime_error("Missing VoxelEB buffers required for state validation");
    }

    // 步骤 2：主机侧数组与下载函数（经暂存缓冲回读）。
    const size_t voxelCount = static_cast<size_t>(modelio->n_voxel);
    if (modelio->m_voxelio->voxellinks.size() < voxelCount) {
        throw std::runtime_error("VoxelEB link count does not match the GPU state");
    }
    std::vector<VoxelDir> directions(voxelCount);
    std::vector<VoxelNetRad> netRadiation(voxelCount);
    std::vector<VoxelHeatflux> heatFlux(voxelCount);
    std::vector<VoxelTempe> temperatures(voxelCount);
    std::vector<VoxelPnet> photonFlux(voxelCount);
    std::vector<VoxelRss> surfaceResistance(voxelCount);
    const auto download = [&](const nvvk::Buffer& source, void* destination, VkDeviceSize size) {
        nvvk::Buffer staging = modelio->m_pAlloc->createBuffer(
            size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        m_pVirtual->bufferToBuffer(modelio, source, size, staging);
        void* mapped = modelio->m_pAlloc->map(staging);
        std::memcpy(destination, mapped, static_cast<size_t>(size));
        modelio->m_pAlloc->unmap(staging);
        modelio->m_pAlloc->destroy(staging);
    };
    download(*modelio->m_voxelio->m_pDirBuffer, directions.data(), voxelCount * sizeof(VoxelDir));
    download(*modelio->m_voxelio->m_pNetRadBuffer, netRadiation.data(), voxelCount * sizeof(VoxelNetRad));
    download(*modelio->m_voxelio->m_pFluxBuffer, heatFlux.data(), voxelCount * sizeof(VoxelHeatflux));
    download(*modelio->m_voxelio->m_pTempeBuffer, temperatures.data(), voxelCount * sizeof(VoxelTempe));
    download(*modelio->m_voxelio->m_pPnetBuffer, photonFlux.data(), voxelCount * sizeof(VoxelPnet));
    download(*modelio->m_voxelio->m_pRssBuffer, surfaceResistance.data(), voxelCount * sizeof(VoxelRss));

    // 体元 → 组分编码：土壤 1、植被 2、建筑 3、水体 5，其它为 0（0 表示不参与统计）；
    //   编码 4 在此未使用（待核对）。
    const auto componentForVoxel = [&](size_t voxel) -> float {
        if (!modelio->m_instanceio || !modelio->m_meshio || voxel >= modelio->m_voxelio->voxellinks.size()) return 0;
        const int instance = modelio->m_voxelio->voxellinks[voxel].instanceId;
        if (instance < 0 || instance >= static_cast<int>(modelio->m_instanceio->instanceLinks.size())) return 0;
        const int mesh = modelio->m_instanceio->instanceLinks[instance].meshId;
        if (mesh < 0 || mesh >= static_cast<int>(modelio->m_meshio->meshLinks.size())) return 0;
        const int type = modelio->m_meshio->meshLinks[mesh].type;
        return type == static_cast<int>(Type::SOIL) ? 1 : type == static_cast<int>(Type::VEGETATION) ? 2
            : type == static_cast<int>(Type::BUILDING) ? 3 : type == static_cast<int>(Type::WATER) ? 5 : 0;
    };

    // 是否带土壤剖面输出：船体钢板（建筑且 plateThickness > 0）或土壤方法 method == 2。
    const auto isProfileSoilVoxel = [&](size_t voxel) {
        if (!modelio->m_instanceio || !modelio->m_meshio ||
            voxel >= modelio->m_voxelio->voxellinks.size()) {
            return false;
        }
        const int instanceId = modelio->m_voxelio->voxellinks[voxel].instanceId;
        if (instanceId < 0 ||
            instanceId >= static_cast<int>(modelio->m_instanceio->instanceLinks.size())) {
            return false;
        }
        const int meshId = modelio->m_instanceio->instanceLinks[instanceId].meshId;
        if (meshId < 0 || meshId >= static_cast<int>(modelio->m_meshio->meshLinks.size())) {
            return false;
        }
        const MeshLink& meshLink = modelio->m_meshio->meshLinks[meshId];
        // Ship steel plate voxels also report their plate layers (outer to inner).
        if (meshLink.bioId >= 0 &&
            meshLink.bioId < static_cast<int>(modelio->m_meshio->soilsets.size()) &&
            meshLink.type == static_cast<int>(Type::BUILDING) &&
            modelio->m_meshio->soilsets[meshLink.bioId].plateThickness > 0.0f) {
            return true;
        }
        return meshLink.type == static_cast<int>(Type::SOIL) &&
            meshLink.bioId >= 0 &&
            meshLink.bioId < static_cast<int>(modelio->m_meshio->soilsets.size()) &&
            modelio->m_meshio->soilsets[meshLink.bioId].method == 2;
    };
    // 是否为“参与介质”体元：植被类且冠层 structureType 为 2 或 3（据下文注释为火/雾，待核对）。
    const auto isParticipatingVoxel = [&](size_t voxel) {
        if (componentForVoxel(voxel) != 2) return false;
        const auto& link = modelio->m_voxelio->voxellinks[voxel];
        const auto& instance = modelio->m_instanceio->instanceLinks[link.instanceId];
        const int canopy = modelio->m_meshio->meshLinks[instance.meshId].canopyId;
        if (canopy < 0 || canopy >= static_cast<int>(modelio->m_meshio->canopies.size())) return false;
        const int structure = modelio->m_meshio->canopies[canopy].structureType;
        return structure == 2 || structure == 3;
    };
    // 步骤 3：逐体元能量收支。热红外反射/透射率取固定材质谱表（见下方注释）的末波段。
    //   Exchange 类型：参与介质 prescribedMedium，叶片 twoSidedLeaf，其它 singleSurface。
    std::vector<histream::energy_output::Balance> energyBalance(voxelCount);
    for (size_t voxel = 0; voxel < voxelCount; ++voxel) {
        using namespace histream::energy_output;
        const float component = componentForVoxel(voxel);
        if (component == 0 || !modelio->m_voxelio->voxellinks[voxel].isValid) continue;
        const auto& link = modelio->m_voxelio->voxellinks[voxel];
        const auto& instance = modelio->m_instanceio->instanceLinks.at(link.instanceId);
        const auto& mesh = modelio->m_meshio->meshLinks.at(instance.meshId);
        // budget.comp reads the fixed 2002-band material table, not the
        // sensor-specific spectra. Its final band is the 10500 nm emissivity.
        const size_t thermalBand = static_cast<size_t>(mesh.spectralId) * (N1 + 1) + N1;
        if (mesh.spectralId < 0 || thermalBand >= modelio->m_meshio->fixedSpectrals.size()) {
            throw std::runtime_error("Missing VoxelEB thermal energy spectrum");
        }
        const auto& thermal = modelio->m_meshio->fixedSpectrals[thermalBand];
        const auto& radiation = netRadiation[voxel];
        const auto& flux = heatFlux[voxel];
        Input input;
        input.directShortwave = radiation.directVrad;
        input.diffuseShortwave = radiation.diffuseVrad;
        input.directLongwave = radiation.directTrad;
        input.diffuseLongwave = radiation.diffuseTrad;
        input.sunlitTemperature = temperatures[voxel].sunlit;
        input.shadedTemperature = temperatures[voxel].shaded;
        input.sunlitFraction = directions[voxel].solar;
        input.thermalReflectance = thermal.reflectance;
        input.thermalTransmittance = thermal.transmittance;
        input.sensibleSunlit = flux.Hsunlit;
        input.sensibleShaded = flux.Hshaded;
        input.latentSunlit = flux.LEsunlit;
        input.latentShaded = flux.LEshaded;
        input.storageSunlit = flux.Gsunlit;
        input.storageShaded = flux.Gshaded;
        input.exchange = isParticipatingVoxel(voxel) ? Exchange::prescribedMedium
            : component == 2 ? Exchange::twoSidedLeaf : Exchange::singleSurface;
        energyBalance[voxel] = calculate(input);
    }
    // 步骤 4：仅在能量过程开启且 TLAST 缓冲存在时，才检查并下载土壤剖面温度。
    bool hasSoilProfile = false;
    if (modelio->isEnergyProcess && modelio->m_voxelio->m_pTLASTBuffer) {
        for (size_t voxel = 0; voxel < voxelCount && !hasSoilProfile; ++voxel) {
            hasSoilProfile = isProfileSoilVoxel(voxel);
        }
    }
    std::vector<TLAST> soilProfile;
    if (hasSoilProfile) {
        soilProfile.resize(voxelCount * TLASTNUM);
        download(*modelio->m_voxelio->m_pTLASTBuffer, soilProfile.data(),
                 voxelCount * TLASTNUM * sizeof(TLAST));
    }

    // 时间标签：meteo.t 为 年积日 + 日内小数；换算为 DOY<d>_<HH>-<MM>，进位处理四舍五入后的 1440 分钟。
    const double julianTime = static_cast<double>(modelio->meteo.t);
    int day = static_cast<int>(std::floor(julianTime));
    int totalMinutes = static_cast<int>(std::llround((julianTime - day) * 1440.0));
    if (totalMinutes >= 1440) {
        day += totalMinutes / 1440;
        totalMinutes %= 1440;
    }
    std::ostringstream time;
    time << "DOY" << day << '_' << std::setw(2) << std::setfill('0') << totalMinutes / 60
         << '-' << std::setw(2) << std::setfill('0') << totalMinutes % 60;

    // 步骤 5：生理诊断（10 项指标），单位见 metricUnits；光学量/通量下限检查见下方。
    // A separate diagnostic contract keeps existing process-record offsets stable.
    const std::filesystem::path diagnosticDirectory =
        std::filesystem::path(modelio->projectDir) / "diagnostics";
    std::filesystem::create_directories(diagnosticDirectory);
    const std::string diagnosticStem = "physiology_node=" + std::to_string(modelio->k_node) + "_T=" + time.str();
    constexpr size_t metricCount = 10;
    const char* metricNames[metricCount] = {"directPar", "diffusePar", "rssSunlit", "rssShaded",
        "gppSunlit", "gppShaded", "netAssimilationSunlit", "netAssimilationShaded",
        "latentHeatSunlit", "latentHeatShaded"};
    const char* metricUnits[metricCount] = {"umol photons m-2 leaf s-1", "umol photons m-2 leaf s-1",
        "s m-1", "s m-1", "umol CO2 m-2 leaf s-1", "umol CO2 m-2 leaf s-1",
        "umol CO2 m-2 leaf s-1", "umol CO2 m-2 leaf s-1", "W m-2", "W m-2"};
    double sums[metricCount]{};
    float minima[metricCount], maxima[metricCount];
    size_t counts[metricCount]{}, invalidCounts[metricCount]{};
    std::fill_n(minima, metricCount, std::numeric_limits<float>::max());
    std::fill_n(maxima, metricCount, std::numeric_limits<float>::lowest());
    size_t invalidVoxelCount = 0, vegetationCount = 0;
    std::ofstream diagnosticBinary;
    if (modelio->isEnergyProcess) {
        diagnosticBinary.open(diagnosticDirectory / (diagnosticStem + ".bin"), std::ios::binary | std::ios::trunc);
        if (!diagnosticBinary) throw std::runtime_error("Cannot write physiology diagnostic binary");
    }
    for (size_t voxel = 0; voxel < voxelCount; ++voxel) {
        const auto& flux = heatFlux[voxel];
        float values[metricCount] = {photonFlux[voxel].directPnet, photonFlux[voxel].diffusePnet,
            surfaceResistance[voxel].sunlit, surfaceResistance[voxel].shaded,
            flux.GPPsunlit, flux.GPPshaded, flux.NPPsunlit, flux.NPPshaded,
            flux.LEsunlit, flux.LEshaded};
        const bool medium = isParticipatingVoxel(voxel);
        if (medium) {
            // Fire/fog have no leaf physiology and never initialize leaf rss.
            std::fill_n(values + 2, 6, std::numeric_limits<float>::quiet_NaN());
        }
        if (diagnosticBinary.is_open()) {
            diagnosticBinary.write(reinterpret_cast<const char*>(values), sizeof(values));
        }
        if (!modelio->m_voxelio->voxellinks[voxel].isValid || componentForVoxel(voxel) == 0) continue;
        // 体元有效性：温度、显热/储热通量、净辐射分量须为有限值，向阳比例须在 [0, 1]，
        //   适用闭合的体元其净辐射与能量残差也须有限。
        bool invalid = !std::isfinite(temperatures[voxel].sunlit) || !std::isfinite(temperatures[voxel].shaded)
            || !std::isfinite(flux.Hsunlit) || !std::isfinite(flux.Hshaded)
            || !std::isfinite(flux.Gsunlit) || !std::isfinite(flux.Gshaded)
            || !std::isfinite(directions[voxel].solar) || directions[voxel].solar < 0.0f || directions[voxel].solar > 1.0f
            || !std::isfinite(netRadiation[voxel].directVrad) || !std::isfinite(netRadiation[voxel].diffuseVrad)
            || !std::isfinite(netRadiation[voxel].directTrad) || !std::isfinite(netRadiation[voxel].diffuseTrad)
            || (energyBalance[voxel].closureApplicable &&
                (!std::isfinite(energyBalance[voxel].netRadiation) ||
                 !std::isfinite(energyBalance[voxel].energyResidual)));
        const bool vegetation = componentForVoxel(voxel) == 2 && !medium;
        if (vegetation) ++vegetationCount;
        for (size_t metric = 0; metric < metricCount; ++metric) {
            if (medium && metric >= 2 && metric < 8) continue;
            const bool valid = std::isfinite(values[metric]) &&
                (metric >= 6 || values[metric] >= 0.0f);
            invalid = invalid || !valid;
            if (!vegetation) continue;
            if (!valid) { ++invalidCounts[metric]; continue; }
            ++counts[metric];
            sums[metric] += values[metric];
            minima[metric] = std::min(minima[metric], values[metric]);
            maxima[metric] = std::max(maxima[metric], values[metric]);
        }
        if (invalid) ++invalidVoxelCount;
    }
    if (diagnosticBinary.is_open()) {
        diagnosticBinary.close();
        if (!diagnosticBinary) throw std::runtime_error("Cannot save physiology diagnostic binary");
    }
    const auto diagnosticPath = diagnosticDirectory / (diagnosticStem + ".json");
    std::ofstream diagnostic(diagnosticPath, std::ios::trunc);
    diagnostic << std::setprecision(9)
        << "{\n\"schemaVersion\":1,\"kind\":\"voxel-physiology-diagnostics\",\"node\":" << modelio->k_node
        << ",\"julianTime\":" << modelio->meteo.t << ",\"voxelCount\":" << voxelCount
        << ",\"vegetationCount\":" << vegetationCount << ",\"invalidVoxelCount\":" << invalidVoxelCount
        << ",\"valid\":" << (invalidVoxelCount == 0 ? "true" : "false")
        << ",\"noData\":\"NaN\",\"aggregation\":\"unweighted leaf-canopy voxel states; participating media excluded; not canopy-area totals\"";
    if (modelio->isEnergyProcess) {
        diagnostic << ",\"dataFile\":\"" << diagnosticStem << ".bin\",\"dataType\":\"float32-little-endian\""
            << ",\"layout\":\"voxel-interleaved\",\"recordFloats\":" << metricCount;
    }
    diagnostic << ",\"fields\":[";
    for (size_t metric = 0; metric < metricCount; ++metric) {
        if (metric) diagnostic << ',';
        diagnostic << "{\"id\":\"" << metricNames[metric] << "\",\"unit\":\"" << metricUnits[metric]
            << "\",\"offset\":" << metric << ",\"finiteCount\":" << counts[metric]
            << ",\"invalidCount\":" << invalidCounts[metric] << ",\"min\":";
        if (counts[metric]) diagnostic << minima[metric]; else diagnostic << "null";
        diagnostic << ",\"max\":";
        if (counts[metric]) diagnostic << maxima[metric]; else diagnostic << "null";
        diagnostic << ",\"mean\":";
        if (counts[metric]) diagnostic << sums[metric] / counts[metric]; else diagnostic << "null";
        diagnostic << '}';
    }
    diagnostic << "]\n}\n";
    diagnostic.close();
    if (!diagnostic) throw std::runtime_error("Cannot save physiology diagnostics");
    // 存在无效体元则抛异常（诊断 JSON 已写出，路径随异常信息给出）。
    if (invalidVoxelCount != 0) {
        throw std::runtime_error("Non-finite or invalid VoxelEB state in " + std::to_string(invalidVoxelCount)
            + " voxels; diagnostic: " + diagnosticPath.string());
    }
    // 步骤 6：能量收支闭合诊断（6 项：吸收短波、吸收长波、发射长波、净辐射、热通量和、残差）。
    // Keep process binary offsets unchanged and expose closure separately.
    // Assess the refreshed final state even when process exports are disabled.
    {
        constexpr size_t balanceMetricCount = 6;
        const char* balanceNames[balanceMetricCount] = {
            "absorbedShortwave", "absorbedLongwave", "emittedLongwave",
            "netRadiation", "heatFluxSum", "energyResidual"};
        double balanceSums[balanceMetricCount]{};
        double balanceMin[balanceMetricCount], balanceMax[balanceMetricCount];
        std::fill_n(balanceMin, balanceMetricCount, std::numeric_limits<double>::max());
        std::fill_n(balanceMax, balanceMetricCount, std::numeric_limits<double>::lowest());
        size_t applicableCount = 0, prescribedMediumCount = 0;
        for (size_t voxel = 0; voxel < voxelCount; ++voxel) {
            if (!modelio->m_voxelio->voxellinks[voxel].isValid || componentForVoxel(voxel) == 0) continue;
            const auto& balance = energyBalance[voxel];
            if (!balance.closureApplicable) { ++prescribedMediumCount; continue; }
            ++applicableCount;
            const double values[balanceMetricCount] = {
                balance.absorbedShortwave, balance.absorbedLongwave, balance.emittedLongwave,
                balance.netRadiation, balance.heatFluxSum, balance.energyResidual};
            for (size_t metric = 0; metric < balanceMetricCount; ++metric) {
                balanceSums[metric] += values[metric];
                balanceMin[metric] = std::min(balanceMin[metric], values[metric]);
                balanceMax[metric] = std::max(balanceMax[metric], values[metric]);
            }
        }
        const auto path = diagnosticDirectory / ("energy_balance_node=" +
            std::to_string(modelio->k_node) + "_T=" + time.str() + ".json");
        std::ofstream balanceDiagnostic(path, std::ios::trunc);
        balanceDiagnostic << std::setprecision(12)
            << "{\n\"schemaVersion\":1,\"kind\":\"voxel-energy-balance-diagnostics\",\"node\":"
            << modelio->k_node << ",\"julianTime\":" << modelio->meteo.t
            << ",\"applicableVoxelCount\":" << applicableCount
            << ",\"prescribedMediumCount\":" << prescribedMediumCount
            << ",\"closureApplicable\":" << (applicableCount > 0 ? "true" : "false")
            << ",\"finalStateRefreshed\":" << (modelio->energyFinalized ? "true" : "false")
            << ",\"temperatureIterations\":" << modelio->energyIterations
            << ",\"budgetResidualToleranceWm2\":2"
            << ",\"budgetStatesOutsideTolerance\":" << modelio->energyFinalUnconvergedStateCount
            << ",\"stabilityCorrection\":" << (modelio->stabilityCorrection ? "true" : "false");
        if (modelio->urbanCanyon)
            balanceDiagnostic << ",\"urbanCanyonAirTemperature_K\":" << modelio->fluidParameters.urbanExchange.w
                              << ",\"urbanCanyonSurfaceCoefficient_W_m2_K\":" << modelio->fluidParameters.urbanExchange.y
                              << ",\"urbanCanyonConvectiveVelocity_m_s\":" << modelio->urbanConvectiveVelocity;
        if (modelio->stabilityCorrection) {
            balanceDiagnostic << ",\"stability\":{\"available\":" << (modelio->stabilityAvailable ? "true" : "false")
                << ",\"inverseObukhovLength\":" << modelio->inverseObukhov
                << ",\"zOverL\":" << modelio->inverseObukhov * modelio->stabilityHeight
                << ",\"effectiveHeight\":" << modelio->stabilityHeight
                << ",\"frictionVelocity\":" << modelio->frictionVelocity
                << ",\"sceneSensibleHeat\":" << modelio->stabilitySensibleHeat
                << ",\"aerodynamicUpdates\":" << modelio->stabilityAeroUpdates
                << ",\"lastZOverLChange\":" << modelio->stabilityChange
                << ",\"basis\":\"scene 1/L from domain-mean H (per ground area) and ground-column mean u*; Paulson/linear MOST, -5<=z/L<=1\"}";
        }
        {
            // Direct-sun visibility rays (gap stage); details in scattering_node=*.json.
            const auto& solar = modelio->solarStats;
            float residualMax = 0.0f;
            std::memcpy(&residualMax, &solar.truncatedResidualMax, sizeof(residualMax));
            balanceDiagnostic << ",\"solarVisibility\":{\"tracedRays\":" << solar.tracedRays
                << ",\"solarTruncations\":" << solar.truncations
                << ",\"truncatedResidualSum\":" << solar.truncatedResidual
                << ",\"truncatedResidualMax\":" << residualMax
                << ",\"truncationErrorBoundMaxWm2\":" << residualMax * modelio->solarBeamIrradiance
                << ",\"thresholdStops\":" << solar.thresholdStops
                << ",\"thresholdErrorBoundWm2\":" << 1.0e-4 * modelio->solarBeamIrradiance << '}';
        }
        balanceDiagnostic
            << ",\"budgetResidualWithinTolerance\":"
            << (modelio->energyFinalized && modelio->energyFinalUnconvergedStateCount == 0 ? "true" : "false")
            << ",\"budgetStateCountBasis\":\"sunlit/shaded conditional states; water uses the shared mixed-state residual; not voxel count\""
            << ",\"aggregation\":\"unweighted applicable voxel flux densities; mixed leaf and surface area bases; not domain power or domain closure\""
            << ",\"energyResidualDefinition\":\"netRadiation-heatFluxSum; no clipping or forced closure\""
            << ",\"leafHeatFluxSum\":\"2*(H+LE); original H/LE fields remain per face; leaf G excluded\""
            << ",\"solidHeatFluxSum\":\"H+LE+G; G is soil/building conduction or water storage\""
            << ",\"prescribedMedium\":\"notApplicable; Rn process placeholder=0; excluded from statistics\""
            << ",\"stateTiming\":\"LW, exchange and physiology refreshed at exported surface temperatures before history advances; residual does not establish full physiological or air-state convergence\""
            << ",\"fields\":[";
        for (size_t metric = 0; metric < balanceMetricCount; ++metric) {
            if (metric) balanceDiagnostic << ',';
            balanceDiagnostic << "{\"id\":\"" << balanceNames[metric]
                << "\",\"unit\":\"W m-2\",\"count\":" << applicableCount << ",\"min\":";
            if (applicableCount) balanceDiagnostic << balanceMin[metric]; else balanceDiagnostic << "null";
            balanceDiagnostic << ",\"max\":";
            if (applicableCount) balanceDiagnostic << balanceMax[metric]; else balanceDiagnostic << "null";
            balanceDiagnostic << ",\"mean\":";
            if (applicableCount) balanceDiagnostic << balanceSums[metric] / applicableCount;
            else balanceDiagnostic << "null";
            balanceDiagnostic << '}';
        }
        balanceDiagnostic << "]\n}\n";
        balanceDiagnostic.close();
        if (!balanceDiagnostic) throw std::runtime_error("Cannot save energy balance diagnostics");
        std::cout << "ENERGY_BALANCE\tnode=" << modelio->k_node
            << "\titerations=" << modelio->energyIterations
            << "\tfinalStateRefreshed=" << modelio->energyFinalized
            << "\tstatesOutsideTolerance=" << modelio->energyFinalUnconvergedStateCount
            << "\tsolarTruncations=" << modelio->solarStats.truncations;
        if (modelio->stabilityCorrection) {
            std::cout << "\tstability=" << (modelio->stabilityAvailable ? "most" : "unavailable")
                << "\tinverseObukhov=" << modelio->inverseObukhov
                << "\tzOverL=" << modelio->inverseObukhov * modelio->stabilityHeight
                << "\tustar=" << modelio->frictionVelocity
                << "\tsceneH=" << modelio->stabilitySensibleHeat
                << "\taeroUpdates=" << modelio->stabilityAeroUpdates
                << "\tstabilityChange=" << modelio->stabilityChange;
        } else {
            std::cout << "\tstability=neutral";
        }
        std::cout << "\tdiagnostics=" << path.string() << std::endl;
    }

    // 步骤 7：过程数据导出（仅在 isRadiationProcess / isEnergyProcess 开启时），写入 process/ 目录：
    //   voxelthermal_T=*.bin（每体元 3 个 float：向阳温度 K、背阴温度 K、向阳比例）、
    //   首节点一次性的 voxel_scene_links.bin 与 material_sensor_spectra.csv、
    //   voxelrt_/voxeleb_T=*.bin/.json（逐体元过程量）、以及可选的 voxelfluid_T=*.bin/.json。
    if (!modelio->isRadiationProcess && !modelio->isEnergyProcess) return;

    const std::filesystem::path directory = std::filesystem::path(modelio->projectDir) / "process";
    std::filesystem::create_directories(directory);
    const float scale = std::max(0.0001f, modelio->stepsize_surface);
    // Preserve separate sunlit/shaded states for later mobile-camera rendering.
    const std::string thermalFile = "voxelthermal_T=" + time.str() + ".bin";
    std::vector<float> thermalState(voxelCount * 3);
    for (size_t voxel = 0; voxel < voxelCount; ++voxel) {
        thermalState[voxel * 3] = temperatures[voxel].sunlit;
        thermalState[voxel * 3 + 1] = temperatures[voxel].shaded;
        thermalState[voxel * 3 + 2] = std::clamp(directions[voxel].solar, 0.0f, 1.0f);
    }
    std::ofstream thermalBinary(directory / thermalFile, std::ios::binary | std::ios::trunc);
    thermalBinary.write(reinterpret_cast<const char*>(thermalState.data()),
                        static_cast<std::streamsize>(thermalState.size() * sizeof(float)));
    thermalBinary.close();
    if (!thermalBinary) throw std::runtime_error("Cannot save voxel thermal state");

    // 仅在首个时间节点写一次静态文件：体元-场景链接（每体元 6 个 int32：
    //   instanceId、meshId、spectralId、canopyId、type、faceId）与材质光谱表。
    if (modelio->k_node == modelio->startTimeNode) {
        // Stable row order matches every hourly process and thermal-state file.
        std::vector<int32_t> links(voxelCount * 6);
        for (size_t voxel = 0; voxel < voxelCount; ++voxel) {
            const auto& link = modelio->m_voxelio->voxellinks.at(voxel);
            const auto& instance = modelio->m_instanceio->instanceLinks.at(link.instanceId);
            const auto& mesh = modelio->m_meshio->meshLinks.at(instance.meshId);
            const int32_t row[] = {link.instanceId, static_cast<int32_t>(instance.meshId),
                mesh.spectralId, mesh.canopyId, mesh.type, link.faceId};
            std::copy_n(row, 6, links.data() + voxel * 6);
        }
        std::ofstream linkBinary(directory / "voxel_scene_links.bin", std::ios::binary | std::ios::trunc);
        linkBinary.write(reinterpret_cast<const char*>(links.data()),
                         static_cast<std::streamsize>(links.size() * sizeof(int32_t)));
        linkBinary.close();
        if (!linkBinary) throw std::runtime_error("Cannot save voxel scene links");
        std::ofstream spectra(directory / "material_sensor_spectra.csv", std::ios::trunc);
        spectra << std::setprecision(9) << "spectral_id,wavelength_nm,reflectance,transmittance\n";
        const auto& optical = modelio->m_meshio->spectrals;
        const size_t bands = modelio->waves.size();
        for (size_t value = 0; bands > 0 && value < optical.size(); ++value) {
            spectra << value / bands << ',' << modelio->waves[value % bands] << ','
                    << optical[value].reflectance << ',' << optical[value].transmittance << '\n';
        }
        spectra.close();
        if (!spectra) throw std::runtime_error("Cannot save material spectra");
    }
    // 写一类过程文件（type 为 "radiation" 或 "energy"）。每体元记录（float32 小端交错）：
    //   位置 3 个（体元中心 = (voxelId + 0.5)·stepsize_surface，m）+ 过程量 + [土壤剖面温度 TLASTNUM 个，
    //   仅 energy 且存在剖面] + 状态 2 个（组分编码、温度 K）。
    //   radiation 过程量 3 个：短波（散射 + 向阳比例·直射，VNIR 通道）、长波（TIR 通道）、净辐射；
    //   energy 过程量 5 个：LE、H、G、GPP、NPP。成对的向阳/背阴量按向阳比例加权求和。
    //   componentOffset 为状态字段起点的 float 偏移，recordFloats = componentOffset + 2。
    //   元数据 JSON 同时记录字段偏移与辐射/碳通量约定。
    const auto writeProcess = [&](const std::string& type) {
        // Radiation in VoxelEB is a time sequence of VoxelRT fields;
        // energy fluxes retain the VoxelEB model name.
        const std::string processModel =
            type == "radiation" ? "voxelrt" : "voxeleb";
        const std::string stem = processModel + "_T=" + time.str();
        const std::filesystem::path binaryPath = directory / (stem + ".bin");
        const std::filesystem::path metadataPath = directory / (stem + ".json");
        const bool writeSoilProfile = type == "energy" && hasSoilProfile;
        const int componentOffset = (type == "energy" ? 8 : 6) + (writeSoilProfile ? TLASTNUM : 0);
        std::ofstream binary(binaryPath, std::ios::binary | std::ios::trunc);
        if (!binary) {
            throw std::runtime_error("Cannot write voxel process file: " + binaryPath.string());
        }
        for (size_t voxel = 0; voxel < voxelCount; ++voxel) {
            const VoxelLink& link = modelio->m_voxelio->voxellinks[voxel];
            const float sunlit = std::clamp(directions[voxel].solar, 0.0f, 1.0f);
            const float shaded = 1.0f - sunlit;
            const float position[3] = {
                (static_cast<float>(link.voxelId.x) + 0.5f) * scale,
                (static_cast<float>(link.voxelId.y) + 0.5f) * scale,
                (static_cast<float>(link.voxelId.z) + 0.5f) * scale
            };
            binary.write(reinterpret_cast<const char*>(position), sizeof(position));
            float values[5]{};
            size_t valueCount = 3;
            if (type == "radiation") {
                values[0] = netRadiation[voxel].diffuseVrad +
                            sunlit * netRadiation[voxel].directVrad;
                values[1] = netRadiation[voxel].diffuseTrad +
                            sunlit * netRadiation[voxel].directTrad;
                values[2] = static_cast<float>(energyBalance[voxel].netRadiation);
            } else {
                valueCount = 5;
                values[0] = sunlit * heatFlux[voxel].LEsunlit + shaded * heatFlux[voxel].LEshaded;
                values[1] = sunlit * heatFlux[voxel].Hsunlit + shaded * heatFlux[voxel].Hshaded;
                values[2] = sunlit * heatFlux[voxel].Gsunlit + shaded * heatFlux[voxel].Gshaded;
                values[3] = sunlit * heatFlux[voxel].GPPsunlit + shaded * heatFlux[voxel].GPPshaded;
                values[4] = sunlit * heatFlux[voxel].NPPsunlit + shaded * heatFlux[voxel].NPPshaded;
            }
            binary.write(reinterpret_cast<const char*>(values), valueCount * sizeof(float));
            if (writeSoilProfile) {
                float layerTemperatures[TLASTNUM];
                if (isProfileSoilVoxel(voxel)) {
                    for (int layer = 0; layer < TLASTNUM; ++layer) {
                        const TLAST& temperature = soilProfile[voxel * TLASTNUM + layer];
                        layerTemperatures[layer] =
                            sunlit * temperature.sunlit + shaded * temperature.shaded;
                    }
                } else {
                    std::fill_n(layerTemperatures, TLASTNUM,
                                std::numeric_limits<float>::quiet_NaN());
                }
                binary.write(reinterpret_cast<const char*>(layerTemperatures),
                             sizeof(layerTemperatures));
            }
            const float state[2] = { componentForVoxel(voxel),
                sunlit * temperatures[voxel].sunlit + shaded * temperatures[voxel].shaded };
            binary.write(reinterpret_cast<const char*>(state), sizeof(state));
        }
        binary.close();

        std::ofstream metadata(metadataPath, std::ios::trunc);
        if (!metadata) {
            throw std::runtime_error("Cannot write voxel process metadata: " + metadataPath.string());
        }
        metadata << std::setprecision(9)
                 << "{\n  \"kind\": \"voxel-" << type << "-process\",\n"
                 << "  \"model\": \"" << processModel << "\",\n"
                 << "  \"processType\": \"" << type << "\",\n"
                 << "  \"geometry\": \"voxel\",\n"
                 << "  \"node\": " << modelio->k_node << ",\n"
                 << "  \"julianTime\": " << modelio->meteo.t << ",\n"
                 << "  \"time\": \"" << time.str() << "\",\n";
        if (!modelio->angles.empty()) {
            metadata << "  \"solarZenith\": " << modelio->angles.front().sza << ",\n"
                     << "  \"solarAzimuth\": " << modelio->angles.front().saa << ",\n";
        }
        metadata << "  \"voxelCount\": " << voxelCount << ",\n"
                 << "  \"voxelSize\": " << scale << ",\n"
                 << "  \"dataFile\": \"" << binaryPath.filename().string() << "\",\n"
                 << "  \"dataType\": \"float32-little-endian\",\n"
                 << "  \"layout\": \"voxel-interleaved\",\n"
                 << "  \"recordFloats\": "
                 << componentOffset + 2 << ",\n"
                 << "  \"componentOffset\": " << componentOffset << ",\n"
                 << "  \"energyState\": {\"finalStateRefreshed\":"
                 << (modelio->energyFinalized ? "true" : "false")
                 << ",\"temperatureIterations\":" << modelio->energyIterations
                 << ",\"budgetResidualToleranceWm2\":2,\"budgetStatesOutsideTolerance\":"
                 << modelio->energyFinalUnconvergedStateCount << "},\n"
                 << "  \"positionOffsets\": [0,1,2],\n"
                 << "  \"thermalState\": {\"dataFile\":\"" << thermalFile
                 << "\",\"recordFloats\":3,\"fields\":[\"sunlitTemperatureK\",\"shadedTemperatureK\",\"sunlitFraction\"]},\n"
                 << "  \"sceneLinks\": {\"dataFile\":\"voxel_scene_links.bin\",\"dataType\":\"int32-little-endian\",\"recordInts\":6,"
                    "\"fields\":[\"instanceId\",\"meshId\",\"spectralId\",\"canopyId\",\"type\",\"faceId\"]},\n"
                 << "  \"fields\": ";
        if (type == "radiation") {
            metadata << "[{\"id\":\"shortwaveRadiation\",\"label\":\"短波辐射 [W m⁻²]\",\"offset\":3},"
                        "{\"id\":\"longwaveRadiation\",\"label\":\"长波辐射 [W m⁻²]\",\"offset\":4},"
                        "{\"id\":\"netRadiation\",\"label\":\"净辐射 [W m⁻²]\",\"offset\":5},"
                        "{\"id\":\"temperature\",\"label\":\"温度 [K]\",\"offset\":" << componentOffset + 1 << "}],\n"
                     << "  \"radiationConvention\": {\n"
                     << "    \"shortwaveAndLongwaveFields\": \"absorbed diffuse (both leaf faces, full sphere) + sunlitFraction*absorbed direct\",\n"
                     << "    \"netRadiation\": \"independent absorbed shortwave + absorbed longwave - thermal emission\",\n"
                     << "    \"leafNetRadiation\": \"diffuse(SW+LW, both faces) + sunlitFraction*direct(SW+LW) - 2*emissivity*sigma*weighted(T^4); per leaf area\",\n"
                     << "    \"solidNetRadiation\": \"diffuse(SW+LW) + sunlitFraction*direct(SW+LW) - emissivity*sigma*weighted(T^4); per surface area\",\n"
                     << "    \"thermalSpectrum\": \"fixed 10500 nm material emissivity; sigma=5.6704e-8 W m-2 K-4\",\n"
                     << "    \"prescribedMediumNetRadiation\": \"notApplicable; finite zero placeholder for fire/fog without a solved surface energy balance\",\n"
                     << "    \"stateTiming\": \"LW and heat flux refreshed at exported temperatures before history advances\"\n"
                     << "  },\n"
                     << "  \"energyBalanceDiagnostic\": \"../diagnostics/energy_balance_node=" << modelio->k_node
                     << "_T=" << time.str() << ".json\"\n}\n";
        } else {
            metadata << "[{\"id\":\"latentHeat\",\"label\":\"潜热 [W m⁻²]\",\"offset\":3},"
                        "{\"id\":\"sensibleHeat\",\"label\":\"显热 [W m⁻²]\",\"offset\":4},"
                        "{\"id\":\"surfaceHeatFlux\",\"label\":\"表面热通量 [W m⁻²]\",\"offset\":5},"
                        "{\"id\":\"gpp\",\"label\":\"GPP [μmol CO₂ m⁻²叶面积 s⁻¹]\",\"offset\":6},"
                        "{\"id\":\"npp\",\"label\":\"叶片净同化 [μmol CO₂ m⁻²叶面积 s⁻¹]\",\"offset\":7},"
                        "{\"id\":\"temperature\",\"label\":\"温度 [K]\",\"offset\":" << componentOffset + 1 << "}]";
            if (writeSoilProfile) {
                metadata << ",\n  \"soilProfile\": {\n"
                         << "    \"layerCount\": " << TLASTNUM << ",\n"
                         << "    \"temperatureUnit\": \"degC\",\n"
                         << "    \"depthUnit\": \"m\",\n"
                         << "    \"depths\": [0,0.02,0.04,0.10,0.20,0.40,0.60,1.00],\n"
                         << "    \"offsets\": [8,9,10,11,12,13,14,15],\n"
                         << "    \"aggregation\": \"actual-soil-column\",\n"
                         << "    \"lowerBoundary\": \"material-Tsoil-at-1m\"\n"
                         << "  }";
            }
            metadata << ",\n  \"carbonConvention\": {\n"
                     << "    \"spatialBasis\": \"leaf-area\",\n"
                     << "    \"unit\": \"umol CO2 m-2 leaf s-1\",\n"
                     << "    \"gpp\": \"gross leaf photosynthesis Ag\",\n"
                     << "    \"npp\": \"net leaf assimilation Ag-Rd; excludes stem and root respiration\"\n"
                     << "  }";
            metadata << "\n}\n";
        }
        std::cout << "PROCESS\t" << metadataPath.string() << std::endl;
    };
    if (modelio->isRadiationProcess) writeProcess("radiation");
    if (modelio->isEnergyProcess) writeProcess("energy");
    // 流体过程导出（能量过程且启用流体时）：下载速度场 A 与标量场 A，按网格顺序写出。
    //   每格记录 8 个 float（启用 airEnabled 时 10 个）：位置 xyz（m）、速度 xyz、
    //   标量 x（空气温度 K）、标量 y（烟雾浓度）、[水汽压 hPa、相对湿度 %]。
    //   索引顺序：y 为最外层（index / (nx·nz)），其次 z，最内层 x。
    if (modelio->isEnergyProcess && modelio->fluid.enabled &&
        modelio->m_pFluidVelocityA && modelio->m_pFluidScalarA &&
        modelio->fluidCellCount > 0) {
        const size_t cellCount = static_cast<size_t>(modelio->fluidCellCount);
        std::vector<glm::vec4> velocity(cellCount);
        std::vector<glm::vec4> scalar(cellCount);
        download(*modelio->m_pFluidVelocityA, velocity.data(), cellCount * sizeof(glm::vec4));
        download(*modelio->m_pFluidScalarA, scalar.data(), cellCount * sizeof(glm::vec4));

        const std::string stem = "voxelfluid_T=" + time.str();
        const std::filesystem::path binaryPath = directory / (stem + ".bin");
        const std::filesystem::path metadataPath = directory / (stem + ".json");
        std::ofstream binary(binaryPath, std::ios::binary | std::ios::trunc);
        if (!binary) throw std::runtime_error("Cannot write fluid process file: " + binaryPath.string());
        const int nx = modelio->fluidGridSize.x;
        const int nz = modelio->fluidGridSize.z;
        const int layer = nx * nz;
        const bool outputHumidity = modelio->terrainEnvironment.airEnabled;
        const size_t recordFloats = outputHumidity ? 10U : 8U;
        for (size_t index = 0; index < cellCount; ++index) {
            const int y = static_cast<int>(index) / layer;
            const int remainder = static_cast<int>(index) - y * layer;
            const int z = remainder / nx;
            const int x = remainder - z * nx;
            const float vapor = outputHumidity ? std::clamp(scalar[index].z, 0.0f,
                TerrainEnvironment::saturationHpa(scalar[index].x - 273.15f)) : 0.0f;
            const float relativeHumidity = outputHumidity ? 100.0f * vapor /
                std::max(0.001f, TerrainEnvironment::saturationHpa(scalar[index].x - 273.15f)) : 0.0f;
            const float record[10] = {
                (x + 0.5f) * modelio->fluidParameters.spacingTime.x,
                (y + 0.5f) * modelio->fluidParameters.spacingTime.y,
                (z + 0.5f) * modelio->fluidParameters.spacingTime.z,
                velocity[index].x, velocity[index].y, velocity[index].z,
                scalar[index].x, scalar[index].y, vapor, relativeHumidity
            };
            binary.write(reinterpret_cast<const char*>(record), recordFloats * sizeof(float));
        }
        binary.close();

        std::ofstream metadata(metadataPath, std::ios::trunc);
        if (!metadata) throw std::runtime_error("Cannot write fluid process metadata: " + metadataPath.string());
        metadata << std::setprecision(9)
                 << "{\n  \"kind\": \"voxel-fluid-process\",\n"
                 << "  \"model\": \"voxelfluid\",\n"
                 << "  \"processType\": \"fluid\",\n"
                 << "  \"geometry\": \"voxel\",\n"
                 << "  \"node\": " << modelio->k_node << ",\n"
                 << "  \"julianTime\": " << modelio->meteo.t << ",\n"
                 << "  \"time\": \"" << time.str() << "\",\n"
                 << "  \"voxelCount\": " << cellCount << ",\n"
                 << "  \"voxelSize\": " << modelio->fluidParameters.spacingTime.x << ",\n"
                 << "  \"gridSize\": [" << modelio->fluidGridSize.x << ','
                 << modelio->fluidGridSize.y << ',' << modelio->fluidGridSize.z << "],\n"
                 << "  \"dataFile\": \"" << binaryPath.filename().string() << "\",\n"
                 << "  \"dataType\": \"float32-little-endian\",\n"
                 << "  \"layout\": \"voxel-interleaved\",\n"
                 << "  \"recordFloats\": " << recordFloats << ",\n"
                 << "  \"positionOffsets\": [0,1,2],\n"
                 << "  \"fields\": [{\"id\":\"windX\",\"label\":\"X 风速 [m s⁻¹]\",\"offset\":3},"
                    "{\"id\":\"windVertical\",\"label\":\"垂直风速 [m s⁻¹]\",\"offset\":4},"
                    "{\"id\":\"windY\",\"label\":\"Y 风速 [m s⁻¹]\",\"offset\":5},"
                    "{\"id\":\"airTemperature\",\"label\":\"空气温度 [K]\",\"offset\":6},"
                    "{\"id\":\"smoke\",\"label\":\"烟雾浓度 [0–1]\",\"offset\":7}"
                 << (outputHumidity ? ",{\"id\":\"vaporPressure\",\"label\":\"水汽压 [hPa]\",\"offset\":8},"
                    "{\"id\":\"relativeHumidity\",\"label\":\"相对湿度 [%]\",\"offset\":9}" : "")
                 << "]\n}\n";
        std::cout << "PROCESS\t" << metadataPath.string() << std::endl;
    }
}

// 模块：把流体（LBM）场在指定高度的水平切片写成 GeoTIFF（每个时间节点调用一次）。
// 步骤 1：未启用流体/未请求输出/缓冲缺失/网格尺寸非法时直接返回。
// 步骤 2：下载元数据（及按需的速度场、标量场）。
// 步骤 3：按 fluid.outputHeight 求切片层 layerY（限幅到 [0, ny-1]），抽取该层的风与气温。
// 步骤 4：写 fluid_wind_H=*m_T=*.tif（4 波段）与 fluid_air_temperature_H=*m_T=*.tif（1 波段）。
// 约定：网格索引 x + nx·(z + nz·y)，y 为竖直层；无数据值为 NaN；气温由 K 转为 °C。
void Voxeleb::outputFluidSlices(std::shared_ptr<VoxelebIO>& modelio)
{
    if (!modelio || !modelio->fluid.enabled
        || (!modelio->fluid.outputWind && !modelio->fluid.outputAirTemperature)
        || modelio->fluidCellCount == 0 || !modelio->m_pFluidMeta
        || (modelio->fluid.outputWind && !modelio->m_pFluidVelocityA)
        || (modelio->fluid.outputAirTemperature && !modelio->m_pFluidScalarA)) {
        return;
    }

    const int nx = modelio->fluidGridSize.x;
    const int ny = modelio->fluidGridSize.y;
    const int nz = modelio->fluidGridSize.z;
    if (nx <= 0 || ny <= 0 || nz <= 0) return;

    const size_t cellCount = static_cast<size_t>(modelio->fluidCellCount);
    std::vector<FluidCellMeta> meta(cellCount);
    std::vector<glm::vec4> velocity;
    std::vector<glm::vec4> scalar;
    if (modelio->fluid.outputWind) velocity.resize(cellCount);
    if (modelio->fluid.outputAirTemperature) scalar.resize(cellCount);

    const auto download = [&](const nvvk::Buffer& source, void* destination, VkDeviceSize size) {
        nvvk::Buffer staging = modelio->m_pAlloc->createBuffer(
            size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        m_pVirtual->bufferToBuffer(modelio, source, size, staging);
        void* mapped = modelio->m_pAlloc->map(staging);
        std::memcpy(destination, mapped, static_cast<size_t>(size));
        modelio->m_pAlloc->unmap(staging);
        modelio->m_pAlloc->destroy(staging);
    };
    download(*modelio->m_pFluidMeta, meta.data(), cellCount * sizeof(FluidCellMeta));
    if (modelio->fluid.outputWind) {
        download(*modelio->m_pFluidVelocityA, velocity.data(), cellCount * sizeof(glm::vec4));
    }
    if (modelio->fluid.outputAirTemperature) {
        download(*modelio->m_pFluidScalarA, scalar.data(), cellCount * sizeof(glm::vec4));
    }

    // 切片高度取层中心：actualHeight = (layerY + 0.5)·spacingY，与请求高度的差异记入元数据。
    const float spacingX = std::max(0.0001f, modelio->fluidParameters.spacingTime.x);
    const float spacingY = std::max(0.0001f, modelio->fluidParameters.spacingTime.y);
    const float spacingZ = std::max(0.0001f, modelio->fluidParameters.spacingTime.z);
    const int layerY = std::clamp(
        static_cast<int>(std::floor(modelio->fluid.outputHeight / spacingY)), 0, ny - 1);
    const float actualHeight = (static_cast<float>(layerY) + 0.5f) * spacingY;
    const float noData = std::numeric_limits<float>::quiet_NaN();
    const size_t pixelCount = static_cast<size_t>(nx) * static_cast<size_t>(nz);

    std::vector<std::vector<float>> windBands;
    std::vector<float> temperature;
    if (modelio->fluid.outputWind) {
        windBands.assign(4, std::vector<float>(pixelCount, noData));
    }
    if (modelio->fluid.outputAirTemperature) {
        temperature.assign(pixelCount, noData);
    }
    // 抽取切片：目标行号 nz-1-z（z 方向上下翻转，使影像上方对应较大的 z）。
    //   meta.kind == 1 的格点不写入，保持 NaN（含义待核对，推测为固体/障碍格）。
    //   风 4 波段：水平风速、X 分量、竖直分量（速度的 y 分量）、Y 分量（速度的 z 分量）。
    for (int z = 0; z < nz; ++z) {
        for (int x = 0; x < nx; ++x) {
            const size_t source = static_cast<size_t>(x)
                + static_cast<size_t>(nx) * (static_cast<size_t>(z)
                + static_cast<size_t>(nz) * static_cast<size_t>(layerY));
            const size_t destination = static_cast<size_t>(nz - 1 - z)
                * static_cast<size_t>(nx) + static_cast<size_t>(x);
            if (meta[source].kind == 1) continue;
            if (modelio->fluid.outputWind) {
                const glm::vec3 value = glm::vec3(velocity[source]);
                windBands[0][destination] = glm::length(glm::vec2(value.x, value.z));
                windBands[1][destination] = value.x;
                windBands[2][destination] = value.y;
                windBands[3][destination] = value.z;
            }
            if (modelio->fluid.outputAirTemperature) {
                temperature[destination] = scalar[source].x - 273.15f;
            }
        }
    }

    // 时间标签：同 outputVoxel（DOY<d>_<HH>-<MM>）。
    const double julianTime = static_cast<double>(modelio->meteo.t);
    int day = static_cast<int>(std::floor(julianTime));
    int totalMinutes = static_cast<int>(std::llround((julianTime - day) * 1440.0));
    if (totalMinutes >= 1440) {
        day += totalMinutes / 1440;
        totalMinutes %= 1440;
    }
    std::ostringstream time;
    time << "DOY" << day << '_' << std::setw(2) << std::setfill('0')
         << totalMinutes / 60 << '-' << std::setw(2) << std::setfill('0')
         << totalMinutes % 60;
    std::ostringstream height;
    height << std::fixed << std::setprecision(2) << modelio->fluid.outputHeight;

    const std::filesystem::path directory(modelio->projectDir);
    std::filesystem::create_directories(directory);
    GDALAllRegister();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (!driver) throw std::runtime_error("GDAL GTiff driver is unavailable");
    // GeoTransform：原点取场景 X 起点与 Y 上沿（sceneOrigin.y + sceneSize.y），像元 X 间距 spacingX、
    //   Y 间距 -spacingZ（北向上）；坐标为场景局部坐标（m），无地理投影。
    const double geoTransform[6] = {
        static_cast<double>(modelio->sceneOrigin_XYZ.x), static_cast<double>(spacingX), 0.0,
        static_cast<double>(modelio->sceneOrigin_XYZ.y + modelio->sceneSize_XYZ.y),
        0.0, -static_cast<double>(spacingZ)
    };
    const std::string requestedHeight = std::to_string(modelio->fluid.outputHeight);
    const std::string resolvedHeight = std::to_string(actualHeight);

    const auto createDataset = [&](const std::filesystem::path& path, int bandCount) {
        GDALDataset* dataset = driver->Create(path.string().c_str(), nx, nz, bandCount,
                                              GDT_Float32, nullptr);
        if (!dataset) throw std::runtime_error("Cannot create fluid GeoTIFF: " + path.string());
        double transform[6];
        std::copy(std::begin(geoTransform), std::end(geoTransform), transform);
        dataset->SetGeoTransform(transform);
        dataset->SetMetadataItem("MODEL", "VoxelEB D3Q19 BGK LBM");
        dataset->SetMetadataItem("TIME", time.str().c_str());
        dataset->SetMetadataItem("REQUESTED_HEIGHT_M", requestedHeight.c_str());
        dataset->SetMetadataItem("ACTUAL_HEIGHT_M", resolvedHeight.c_str());
        dataset->SetMetadataItem("COORDINATE_SYSTEM", "Local scene coordinates in metres");
        return dataset;
    };
    const auto writeBand = [&](GDALDataset* dataset, int index,
                               const std::vector<float>& values,
                               const char* description, const char* unit) {
        GDALRasterBand* band = dataset->GetRasterBand(index);
        band->SetDescription(description);
        band->SetUnitType(unit);
        band->SetNoDataValue(static_cast<double>(noData));
        if (band->RasterIO(GF_Write, 0, 0, nx, nz,
                           const_cast<float*>(values.data()), nx, nz,
                           GDT_Float32, 0, 0) != CE_None) {
            throw std::runtime_error("Cannot write fluid GeoTIFF raster band");
        }
    };

    if (modelio->fluid.outputWind) {
        const std::filesystem::path path = directory /
            ("fluid_wind_H=" + height.str() + "m_T=" + time.str() + ".tif");
        GDALDataset* dataset = createDataset(path, 4);
        try {
            writeBand(dataset, 1, windBands[0], "Horizontal wind speed", "m/s");
            writeBand(dataset, 2, windBands[1], "Wind X component", "m/s");
            writeBand(dataset, 3, windBands[2], "Wind vertical component", "m/s");
            writeBand(dataset, 4, windBands[3], "Wind Y component", "m/s");
        } catch (...) {
            GDALClose(dataset);
            throw;
        }
        GDALClose(dataset);
        std::cout << "FLUID_OUTPUT\t" << path.string() << std::endl;
    }
    if (modelio->fluid.outputAirTemperature) {
        const std::filesystem::path path = directory /
            ("fluid_air_temperature_H=" + height.str() + "m_T=" + time.str() + ".tif");
        GDALDataset* dataset = createDataset(path, 1);
        try {
            writeBand(dataset, 1, temperature, "Air temperature", "degree Celsius");
        } catch (...) {
            GDALClose(dataset);
            throw;
        }
        GDALClose(dataset);
        std::cout << "FLUID_OUTPUT\t" << path.string() << std::endl;
    }
}


// 模块：气溶胶条件载入。目前只有 AeroType::ONE（单一条件）被写入 aeroconds；
//   image 与 gridCal 分支为未实现的占位（只有无用的局部变量）；函数固定返回 false。
bool Voxeleb::uploadAero(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &modelio) {


    if(fileio->m_pVoxelebXml->aerocondxml.aerotype == AeroType::ONE) {
        modelio->aeroconds.emplace_back(fileio->m_pVoxelebXml->aerocondxml.aerocond);
    }else if (fileio->m_pVoxelebXml->aerocondxml.aerotype == AeroType::image)
    {
        int a = 10;
    }else if(fileio->m_pVoxelebXml->aerocondxml.aerotype == AeroType::gridCal){
        int b = 10;
    }

    return false;


}
// runObserver（HISTREAM_OBSERVER_MANIFEST 观测器模式）的实现在 observer_capture.inl 中，
//   通过 #include 并入本编译单元（该文件不在本次整理范围）。
#include "observer_capture.inl"
