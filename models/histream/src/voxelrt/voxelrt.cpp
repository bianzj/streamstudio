// ==========================================================================
// 文件：voxelrt.cpp
// 功能：VoxelRT（体元辐射）模式的顶层驱动：初始化 Vulkan 资源、读取工程设置、创建 GPU 资源、
//   按观测几何循环执行辐射传输，并把结果写成 TIFF 影像和三维辐射过程文件。
// 主要模块/步骤：
//   1. 匿名命名空间辅助函数：Planck 辐亮度、辐射过程元数据字段、波段名称
//   2. Voxelrt::setup：从 AppSetting 取得设备/队列，建立命令池、分配器、调试工具
//   3. Voxelrt::upload / uploadSetting / uploadDefined：读取工程 XML 与场景（光学、体元化、几何）
//   4. Voxelrt::updateSetting：把工程设置填入 GPU 推送常量 VoxelRTSetting
//   5. Voxelrt::create：创建缓冲、描述符、管线、命令
//   6. Voxelrt::run：按（天顶角、方位角）或 UAV 航点循环，运行传输并输出
//   7. outputRadiationProcess / outputVoxel / output / outputPos：回读 GPU 结果并写盘
//   8. Voxelrt::destroy：释放资源
// 数据流：FileIO（工程 XML）→ VoxelrtIO（运行时状态）→ Buffer/Descriptor/Pipeline/Command →
//   GPU 计算着色器（shader/voxelrt）→ 回读 → TIFF / voxelrt.bin + voxelrt.json。
// 与其他文件的关系：声明见 voxelrt.h；状态容器 voxelrtio.h；管线 pipeline.cpp；
//   派发顺序 command.cpp；缓冲 buffer.cpp；描述符 descriptor.cpp。
// 约定：波长大于 50 视为 nm，否则视为 μm；大于 2.5 μm 的波段按热红外（Planck 辐亮度）处理，
//   其余按反射（光学）波段处理。
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#include "voxelrt.h"
#include "src/base/atmosphere_lut.h"
#include "src/base/voxelflags.h"
#include "src/base/voxelmerge_stats.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace {
// 作用：黑体 Planck 光谱辐亮度。
// 输入：波长（>50 视为 nm，否则 μm）、温度 K；输出：W m-2 sr-1 μm-1。
// 说明：c1 = 2hc²（W μm4 m-2 sr-1），c2 = hc/k（μm K）；波长下限 1e-6 μm、
// 温度下限 1 K，避免除零/溢出；用 expm1 提高低温/长波下的精度。
float planckRadiance(const float wavelengthNanometers, const float temperatureKelvin) {
    constexpr double c1 = 1.1910439340652e8;
    constexpr double c2 = 14388.291040407;
    const double wavelengthMicrometers = wavelengthNanometers > 50.0f
        ? static_cast<double>(wavelengthNanometers) / 1000.0
        : static_cast<double>(wavelengthNanometers);
    const double wavelength = std::max(1.0e-6, wavelengthMicrometers);
    const double temperature = std::max(1.0, static_cast<double>(temperatureKelvin));
    return static_cast<float>(c1 /
        (std::pow(wavelength, 5.0) * std::expm1(c2 / (temperature * wavelength))));
}

// 作用：向 voxelrt.json 的元数据流写入一个“辐射过程”字段（JSON 对象）。
// 输入：波长 wavelength（nm 或 μm，同上约定）、该字段在 voxelrt.bin 每体元记录中的偏移 offset。
// 说明：热红外波段输出平均出射光谱辐亮度，光学波段输出归一化入射照明；
// label/definition 为前端显示用的中英文说明字符串。
void writeVoxelrtProcessField(std::ostream& metadata, const float wavelength,
                             const int offset) {
    // Same optical/thermal split as voxelrtBandNames and the VoxelRT shaders:
    // > 2.5 um (nm or um input) is a Planck-radiance band.
    const bool thermal = (wavelength > 50.0f ? wavelength / 1000.0f : wavelength) > 2.5f;
    metadata << "{\"id\":\"" << (thermal ? "spectral_radiance_" : "illumination_")
             << wavelength << "\",\"aliases\":[\"radiosity_" << wavelength
             << "\"],\"label\":\""
             << (thermal ? "平均出射光谱辐亮度 " : "归一化入射照明 ")
             << wavelength << (thermal ? " nm [W m⁻² sr⁻¹ μm⁻¹]" : " nm [-]")
             << "\",\"offset\":" << offset << ",\"wavelengthNm\":" << wavelength
             << ",\"quantity\":\""
             << (thermal ? "outgoing_spectral_radiance" : "incoming_normalized_illumination")
             << "\",\"unit\":\"" << (thermal ? "W m-2 sr-1 um-1" : "1")
             << "\",\"definition\":\""
             << (thermal
                 ? "(reflectance + transmittance) * environment_radiance + emissivity * mixed_Planck_radiance; local material average, without a sensor viewing direction"
                 : "environment_illumination + sunlit_fraction * configured_direct_fraction; configured direct/diffuse fractions, without spectral irradiance scaling or local outgoing reflectance")
             << "\"}";
}

// 作用：生成每个波段的 TIFF 波段说明文字。
// 规则：<= 2.5 μm 为反射率；更长波段在 temperatureOutput 时为亮温（K），否则为光谱辐亮度。
std::vector<std::string> voxelrtBandNames(const std::vector<float>& waves,
                                          const bool temperatureOutput) {
    std::vector<std::string> names;
    names.reserve(waves.size());
    for (const float wave : waves) {
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
}



// 模块：初始化。把 AppSetting 中的 Vulkan 上下文/队列交给 VoxelrtIO，
// 建立命令池、资源分配器和调试命名工具，并查询光追属性、初始化 SBT 与加速结构构建器。
// 说明：VoxelRT 的核心是计算着色器；SBT/加速结构这两项初始化沿用其他模式的框架，
// 是否被 VoxelRT 实际用到（待核对）。
bool Voxelrt::setup(AppSetting &appsetting, std::shared_ptr<VoxelrtIO> &modelio){


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




// 模块：读取工程并构建场景。顺序：defined 文件 → 组分光学参数 → 运行设置
// → 体元场景 → 几何；运行设置必须先于体元化（原因见函数内已有注释）。
bool Voxelrt::upload(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &modelio){

    // auto & fileio = modelio->m_fileio;
    // auto & meshio = modelio->m_meshio;

    uploadDefined(fileio,modelio);
    m_pCompo->createCompOptical(fileio, modelio);
    // Scene voxelization needs this switch before it extracts per-voxel Hex
    // parameters.  Previously uploadSetting ran after scene creation, so the
    // UI option never reached the heterogeneous voxel path.
    uploadSetting(fileio, modelio);
    m_pScene->createPrimObjScene(fileio, modelio);
    // HISTREAM_VOXEL_MERGE_STATS=1|only: read-only two-level (fixscale) potential table; no behaviour change.
    voxelmerge::report(modelio, "VoxelRT");
    m_pGeometry->createGeometry(fileio,modelio);
//    defineOPO(modelio);
   // uploadMeteo(fileio,modelio);
  //  uploadAero(fileio,modelio);

    return true;
}

// 模块：把 VoxelRT 工程 XML（sensorxml、settingxml）中的传感器与求解设置复制到 VoxelrtIO。
// 涉及：波段/视角数、温度/显示/反照率/影像/辐射过程输出开关、影像分辨率、最大深度、
// 采样数、非均质体元、周期边界邻域数、天空盒、加速求解器、体元碰撞（散射）阶数。
bool Voxelrt::uploadSetting(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &modelio) {

    modelio->n_wave = fileio->m_pVoxelrtXml->sensorxml.waves.size();
    modelio->n_angle = fileio->m_pVoxelrtXml->sensorxml.viewAngles.size();
    modelio->isTemperature =  fileio->m_pVoxelrtXml->sensorxml.isTemperature;
    modelio->isDisplay = fileio->m_pVoxelrtXml->sensorxml.isDisplay;
    modelio->isAlbedo = fileio->m_pVoxelrtXml->sensorxml.isAlbedo;
    modelio->isImage = fileio->m_pVoxelrtXml->sensorxml.isImage;
    modelio->isRadiationProcess = fileio->m_pVoxelrtXml->sensorxml.isRadiationProcess;
    modelio->imageSize = fileio->m_pVoxelrtXml->sensorxml.resolution;
    modelio->maxDepth = fileio->m_pVoxelrtXml->settingxml.maxDepth;
    modelio->n_sample = fileio->m_pVoxelrtXml->settingxml.n_sample;
    modelio->heterogeneousVoxel = fileio->m_pVoxelrtXml->settingxml.heterogeneousVoxel;
    modelio->periodicNeighborCount =
        fileio->m_pVoxelrtXml->settingxml.periodicNeighborCount;
    modelio->skyboxEnabled = fileio->m_pVoxelrtXml->settingxml.skyboxEnabled;
    modelio->acceleratedRadiationSolver =
        fileio->m_pVoxelrtXml->settingxml.acceleratedRadiationSolver;
    modelio->collisionOrders = fileio->m_pVoxelrtXml->settingxml.voxelCollisionOrders;
    modelio->voxelFlags = voxelflags::resolve(fileio->m_pVoxelrtXml->settingxml.voxelDirectionRotation,
        fileio->m_pVoxelrtXml->settingxml.voxelDirectionRotationBlock, fileio->m_pVoxelrtXml->settingxml.voxelEmptySkip,
        fileio->m_pVoxelrtXml->settingxml.voxelProxyGeometry);

    return true;
}

// 模块：把 VoxelrtIO 的运行参数填入 GPU 推送常量 modelio->setting（VoxelRTSetting）。
// 说明：scale 为体元边长 stepsize_surface（m）；voxelSize 取自 voxelSize_XZY，
// 其 y 分量（竖直层数）随后按场景高度重新计算；体元边长各向同性，竖直也用 stepsize_surface
// （scene.cpp 要求 stepsize_surface == stepsize_height，读入时二者同取 voxelSize）。
// 有意不改用 stepsize_height：着色器的推送常量只有一个 scale（取 stepsize_surface），三轴共用，
// 竖直层数必须与该边长一致，否则二者不等时层数与着色器的体元坐标不符。
bool Voxelrt::updateSetting(std::shared_ptr<VoxelrtIO> &modelio){

    // auto &opo = modelio->m_opo;

    // auto &sceneio = modelio->m_sceneio;

    modelio->setting.imageSize = modelio->imageSize;
    modelio->setting.n_wave = modelio->n_wave;
    modelio->setting.scale = modelio->stepsize_surface;
    modelio->setting.isTemperature = modelio->isTemperature ? 1 : 0;
    modelio->setting.isDisplay = modelio->isDisplay;
    // Voxel transport options from HISTREAM_VOXEL_DIRROT / HISTREAM_VOXEL_SKIP (src/base/voxelflags.h).
    modelio->setting.voxelFlags = modelio->voxelFlags;
    modelio->setting.maxDepth = modelio->maxDepth;
    // Dense vegetation can require hundreds of ordered triangle crossings.
    // Tie the limit to ray depth and keep it within a practical GPU range.
    modelio->setting.maxStep = std::clamp(modelio->maxDepth * 128, 128, 2048);
    modelio->setting.n_sample = modelio->n_sample;
    modelio->setting.voxelSize = modelio->voxelSize_XZY;
    // Vertical cell count bounds the turbid-cell traversal in the shaders.
    modelio->setting.voxelSize.y = std::max(1, static_cast<int>(std::ceil(
        modelio->sceneSize_XYZ.z / std::max(0.01f, modelio->stepsize_surface))));
    modelio->setting.voxelCount = modelio->n_voxel;
    modelio->setting.spectralBatchSize = 1;
    modelio->setting.collisionOrders = modelio->collisionOrders;
    modelio->setting.collisionOrder = 1;
    modelio->setting.periodicNeighborCount = modelio->periodicNeighborCount;
    modelio->setting.skyboxEnabled = modelio->skyboxEnabled ? 1 : 0;


    return true;
    //modelio->setting.maxDepth = fileio->m_pXmlInput.
}



// 模块：读取 defined 数据到 m_defined（具体内容见 FileIO::readDefined，待核对）。
// 注意：函数总是返回 false，调用方 upload 不检查该返回值。
bool Voxelrt::uploadDefined(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &modelio)
{

    fileio->readDefined(modelio->m_defined);

    return false;
}

// 模块：创建 GPU 资源。顺序：缓冲 → 描述符 → 计算管线 → 命令 → 推送常量。
// 管线创建失败（找不到/无法加载 .spv）时返回 false，由上层中止。
bool Voxelrt::create(std::shared_ptr<VoxelrtIO> &modelio) {


    m_pBuffer->createBuffer(modelio);
    m_pDescriptor->createDescriptor(modelio);
    if (!m_pPipeline->createPipeline(modelio)) {
        return false;
    }
    m_pCommand->create(modelio);
    updateSetting(modelio);
    return true;
}

// 模块：主循环。对每个观测几何运行体元辐射传输并输出结果。
// 分支：UAV 航点模式（isUAVTrave）按航点循环，太阳取第一个角度；
//       常规模式按 angles（vza/vaa/sza/saa）循环。
// 副作用：向 stdout 打印 PROGRESS / OBSERVATION 行，供上层服务解析进度（待核对）。
bool Voxelrt::run(std::shared_ptr<VoxelrtIO> &modelio, std::shared_ptr<FileIO> &fileio) {



    modelio->k_node = -1;
    std::cout << "PROGRESS\t5\t加载体元场景与辐射参数" << std::endl;

    // The gap and diffuse passes depend on the sun only, never on the view:
    // reuse them for every observation/waypoint that shares the previous sun.
    // 步骤 1：定义“带复用的传输”函数。gap 与 diffuse 阶段只依赖太阳方向，与观测方向无关；
    // 太阳角度与上一次相同时只重跑 out 阶段（成像），省去重算漫射传输。
    bool transportValid = false;
    float transportSza = 0.0f, transportSaa = 0.0f;
    const auto runTransport = [&](const Angle& sun) {
        const bool reuse = transportValid && sun.sza == transportSza && sun.saa == transportSaa;
        if (modelio->acceleratedRadiationSolver) {
            m_pCommand->runRTAccelerated(modelio, 4, reuse);
        } else {
            m_pCommand->runRT(modelio, reuse);
        }
        transportValid = true;
        transportSza = sun.sza;
        transportSaa = sun.saa;
    };

    // 步骤 2：按观测模式循环——UAV 航点 或 视角序列。
    if(modelio->isUAVTrave && modelio->n_pos > 0) {

        for (int kpos = 0; kpos < modelio->n_pos; kpos++) {
            modelio->k_pos = kpos;
            m_pGeometry->updateSensorPos(modelio, kpos);
            const glm::vec3 position = modelio->uavposes[kpos];
            std::cout << "OBSERVATION\t航点 " << (kpos + 1) << "/" << modelio->n_pos
                      << " X=" << position.x << " Y=" << position.y
                      << " H=" << position.z << std::endl;
            runTransport(modelio->angles.front());
            if (modelio->isRadiationProcess && kpos == 0) {
                outputRadiationProcess(modelio);
            }
            if (modelio->isImage) {
                outputPos(modelio, fileio, -1, kpos);
            }
        }
    }else {

        for (int kangle = 0; kangle < modelio->n_angle; kangle++) {
            modelio->k_angle = kangle;
            m_pGeometry->updateAngle(modelio, kangle);
            Angle angle = modelio->angles[kangle];
            std::cout << "OBSERVATION\t"
                      << "    vza_" << std::to_string(angle.vza) << "    vaa_" << std::to_string(angle.vaa)
                      << "    sza_" << std::to_string(angle.sza) << "    saa_" << std::to_string(angle.saa)
                      << std::endl;
            runTransport(angle);
            if (modelio->isRadiationProcess && kangle == 0) {
                outputRadiationProcess(modelio);
            }
            if (modelio->isImage) {
                if (fileio->m_pVoxelrtXml->sensorxml.projection == Projection::PERSPECTIVE)
                    outputPos(modelio, fileio, -1, -1);
                else
                    output(modelio, fileio, -1, kangle);
            }
            std::cout << "PROGRESS\t" << 10 + 85 * (kangle + 1) / std::max(1, modelio->n_angle)
                      << "\t辐射传输观测 " << kangle + 1 << "/" << modelio->n_angle << std::endl;
        }
    }

    std::cout << "PROGRESS\t100\t体元辐射传输计算完成" << std::endl;
    return true;
}

// 模块：导出体元辐射过程（三维场景辐射场）到工程目录下的 voxelrt.bin / voxelrt.json。
// 仅在 isRadiationProcess 且第一个观测（kangle==0 或 kpos==0）后调用一次，因为 VoxelRT
// 只有一个静态辐射场。输入：n_voxel、方向缓冲（VoxelDir）、辐射缓冲（VoxelRad）。
// 步骤：回读 GPU 缓冲 → 写二进制记录 → 写 JSON 元数据（字段偏移、单位、定义）。
void Voxelrt::outputRadiationProcess(std::shared_ptr<VoxelrtIO>& modelio) {
    if (!modelio || modelio->n_voxel <= 0 || !modelio->m_voxelio ||
        !modelio->m_voxelio->m_pDirBuffer || !modelio->m_voxelio->m_pRadsBuffer) {
        return;
    }

    // 步骤 1：从 GPU 回读每体元的方向数据（VoxelDir，含 solar 日照比例）与辐射缓冲（VoxelRad）。
    //         辐射缓冲每体元步长 = 2*n_wave（2026-11-26 起不再有 64 槽方向暂存区）；
    //         环境项（cumulated）位于 band 处（布局见 voxelrt/buffer.cpp）。
    const size_t voxelCount = static_cast<size_t>(modelio->n_voxel);
    const int bandCount = modelio->n_wave;
    const size_t radiationStride = 2U * static_cast<size_t>(std::max(modelio->n_wave, 1));
    std::vector<VoxelDir> directions(voxelCount);
    // DEFERRED[低] 内存: 整个 rads 缓冲（n_voxel×2n_wave×8 B）连同等大的 host-visible staging 一起回读，而这里只用到每体元前 n_wave 个环境项；大场景可占用主机内存；建议：用带步长的 vkCmdCopyBuffer 区域只回读所需的段。
    std::vector<VoxelRad> radiosities(voxelCount * radiationStride);
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
    download(*modelio->m_voxelio->m_pDirBuffer, directions.data(),
             voxelCount * sizeof(VoxelDir));
    download(*modelio->m_voxelio->m_pRadsBuffer, radiosities.data(),
             radiosities.size() * sizeof(VoxelRad));

    // 步骤 2：写 voxelrt.bin（float32 小端，体元交错）。每体元记录依次为：
    //         体元中心 x、y、z（m）、各波段平均辐射量（nband 个）、光照比例 sunlit_fraction，
    //         共 4 + n_wave 个 float，与下面 JSON 中的 recordFloats / offset 对应。
    // VoxelRT has one static radiation field.  Store it beside the TIFFs,
    // matching FacetRT's root-level three-dimensional scene output.
    const std::filesystem::path directory = std::filesystem::path(modelio->projectDir);
    std::filesystem::create_directories(directory);
    const std::filesystem::path binaryPath = directory / "voxelrt.bin";
    const std::filesystem::path metadataPath = directory / "voxelrt.json";
    std::ofstream binary(binaryPath, std::ios::binary | std::ios::trunc);
    if (!binary) {
        throw std::runtime_error("Cannot write voxel radiation process file: " + binaryPath.string());
    }
    // 体元中心坐标 = (体元整数号 + 0.5) * 体元边长；边长下限 1e-4 m 防止退化。
    const float scale = std::max(0.0001f, modelio->stepsize_surface);
    for (size_t voxel = 0; voxel < voxelCount; ++voxel) {
        const VoxelLink& link = modelio->m_voxelio->voxellinks[voxel];
        const float position[3] = {
            (static_cast<float>(link.voxelId.x) + 0.5f) * scale,
            (static_cast<float>(link.voxelId.y) + 0.5f) * scale,
            (static_cast<float>(link.voxelId.z) + 0.5f) * scale
        };
        binary.write(reinterpret_cast<const char*>(position), sizeof(position));
        const float sunlitFraction = std::clamp(directions[voxel].solar, 0.0f, 1.0f);
        // 由体元所属实例找到对应网格（材料）；越界时 mesh 保持为空，热红外项退化为环境辐亮度。
        const MeshLink* mesh = nullptr;
        if (link.instanceId >= 0 &&
            link.instanceId < static_cast<int>(modelio->m_instanceio->instanceLinks.size())) {
            const uint32_t meshId = modelio->m_instanceio->instanceLinks[link.instanceId].meshId;
            if (meshId < modelio->m_meshio->meshLinks.size()) {
                mesh = &modelio->m_meshio->meshLinks[meshId];
            }
        }
        for (int band = 0; band < bandCount; ++band) {
            const size_t environmentIndex = voxel * radiationStride + static_cast<size_t>(band);
            const float environmentValue = radiosities[environmentIndex].cumulated;
            // 环境项非有限值（NaN/Inf）一律置 0，并截断为非负，防止污染输出文件。
            const float environmentRadiance =
                std::isfinite(environmentValue) ? std::max(0.0f, environmentValue) : 0.0f;
            const float wavelength = band < static_cast<int>(modelio->waves.size())
                ? modelio->waves[band] : static_cast<float>(band + 1);
            // One optical/thermal split (> 2.5 um) shared with voxelrtBandNames
            // and the diffuse/image shaders, so 3-5 um bands are thermal everywhere.
            const bool isThermalBand =
                (wavelength > 50.0f ? wavelength / 1000.0f : wavelength) > 2.5f;
            float shadedRadiance = environmentRadiance;
            // Optical values are normalized incoming illumination. Longwave
            // values use Planck radiance and must never include that fraction.
            float sunlitRadiance = environmentRadiance +
                (isThermalBand ? 0.0f : std::max(0.0f, modelio->light.direct));

            // Longwave radiance is emitted by the local surface.  Compute the
            // illuminated and shaded states separately, then mix them using
            // the voxel's solar transmittance/fraction.
            if (isThermalBand && mesh != nullptr &&
                mesh->thermalId >= 0 &&
                mesh->thermalId < static_cast<int>(modelio->m_meshio->thermals.size())) {
                float reflectance = 0.0f;
                float transmittance = 0.0f;
                const size_t spectralIndex =
                    static_cast<size_t>(std::max(0, mesh->spectralId)) *
                    static_cast<size_t>(std::max(1, modelio->n_wave)) +
                    static_cast<size_t>(band);
                if (spectralIndex < modelio->m_meshio->spectrals.size()) {
                    reflectance = std::clamp(
                        modelio->m_meshio->spectrals[spectralIndex].reflectance, 0.0f, 1.0f);
                    transmittance = std::clamp(
                        modelio->m_meshio->spectrals[spectralIndex].transmittance,
                        0.0f, 1.0f - reflectance);
                }
                const float emissivity = std::max(0.0f, 1.0f - reflectance - transmittance);
                const float scatteredRadiance =
                    (reflectance + transmittance) * environmentRadiance;
                const Thermal& thermal = modelio->m_meshio->thermals[mesh->thermalId];
                shadedRadiance = scatteredRadiance + emissivity *
                    planckRadiance(wavelength, thermal.shadedTemperature);
                sunlitRadiance = scatteredRadiance + emissivity *
                    planckRadiance(wavelength, thermal.sunlitTemperature);
            }

            // 按日照比例混合受光/背阴两种状态：sunlit*S + shaded*(1-S)，与 JSON 的 aggregation 字段一致。
            const float averagedRadiance =
                sunlitFraction * sunlitRadiance +
                (1.0f - sunlitFraction) * shadedRadiance;
            binary.write(reinterpret_cast<const char*>(&averagedRadiance), sizeof(averagedRadiance));
        }
        binary.write(reinterpret_cast<const char*>(&sunlitFraction), sizeof(sunlitFraction));
    }
    binary.close();

    // 步骤 3：写 voxelrt.json 元数据：记录格式、体元尺寸、每个波段字段（偏移 3+band）
    //         以及最后的 sunlit_fraction 字段（偏移 3+n_wave）。
    std::ofstream metadata(metadataPath, std::ios::trunc);
    metadata << std::setprecision(9)
             << "{\n  \"kind\": \"voxel-radiation-process\",\n"
             << "  \"model\": \"voxelrt\",\n"
             << "  \"processType\": \"radiation\",\n"
             << "  \"geometry\": \"voxel\",\n"
             << "  \"schemaVersion\": 2,\n"
             << "  \"voxelCount\": " << voxelCount << ",\n"
             << "  \"voxelSize\": " << scale << ",\n"
             << "  \"dataFile\": \"" << binaryPath.filename().string() << "\",\n"
             << "  \"dataType\": \"float32-little-endian\",\n"
             << "  \"layout\": \"voxel-interleaved\",\n"
             << "  \"aggregation\": \"sunlit_fraction * sunlit_value + (1 - sunlit_fraction) * shaded_value; quantity and units are defined per field\",\n"
             << "  \"recordFloats\": " << 4 + bandCount << ",\n"
             << "  \"positionOffsets\": [0,1,2],\n"
             << "  \"fields\": [";
    for (int band = 0; band < bandCount; ++band) {
        if (band != 0) metadata << ',';
        const float wavelength = band < static_cast<int>(modelio->waves.size())
            ? modelio->waves[band] : static_cast<float>(band + 1);
        writeVoxelrtProcessField(metadata, wavelength, 3 + band);
    }
    if (bandCount != 0) metadata << ',';
    metadata << "{\"id\":\"sunlit_fraction\",\"label\":\"光照比例 [-]\",\"offset\":"
             << 3 + bandCount << "}]\n}\n";
    // 向 stdout 报告元数据文件路径，供上层服务发现并加载（待核对）。
    std::cout << "PROCESS\t" << metadataPath.string() << std::endl;
}



// 模块：释放资源。先等待设备空闲，再依次销毁缓冲、SBT、加速结构构建器、描述符、管线、
// 命令对象、命令池与分配器。
// 注意：函数总是返回 false（含成功路径），调用方是否依赖该返回值（待核对）。
bool Voxelrt::destroy(std::shared_ptr<VoxelrtIO> & modelio){

    const VkResult waitIdleResult = vkDeviceWaitIdle(modelio->m_device);
    if (waitIdleResult != VK_SUCCESS) {
        std::cerr << "Voxelrt::destroy: vkDeviceWaitIdle failed, VkResult="
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



// 模块：常规观测（正射/视角序列）的 TIFF 输出。
// 输入：knode（-1 表示无时间戳）、kangle（观测角序号）；读取虚拟屏幕存储缓冲
//       （nWave 个波段平面，先波段后像元）。
// 输出：经几何校正和（可选）大气校正的多波段 TIFF，经 FileIO::writeTIFData 写出。
// 步骤：1) 回读像素；2) orthcorrect 求坐标映射系数；3) 逐像元重采样 + 大气校正；4) 写文件。
void Voxelrt::output(std::shared_ptr<VoxelrtIO> &modelio, std::shared_ptr<FileIO> &fileio, int knode, int kangle) {
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

    // 步骤 2：取得该视角的几何校正映射。cx、cy 为 4 个系数，源像元坐标 =
    //         i*c0 + j*c1 + i*j*c2 + c3（i、j 为输出像元列、行；双线性型映射，待核对）。
    Angle angle = modelio->angles[kangle];
    Eigen::VectorXd cx;
    Eigen::VectorXd cy;
    m_pGeometry->orthcorrect(modelio, angle.vza, angle.vaa, cx, cy);

    std::vector<float> orthData(totalElements, 0.0f);
    for (int i = 0; i < width; ++i) {
        for (int j = 0; j < height; ++j) {
            // 步骤 3：映射后截断取整，越界像元跳过（保持 0）；值为 0 的源像元视为无数据，
            //         不做大气校正，避免把背景变成非零。
            const int ii = static_cast<int>(i * cx[0] + j * cx[1] + i * j * cx[2] + cx[3]);
            const int jj = static_cast<int>(i * cy[0] + j * cy[1] + i * j * cy[2] + cy[3]);
            if (ii < 0 || ii >= width || jj < 0 || jj >= height) {
                continue;
            }

            const size_t destination = static_cast<size_t>(j) * width + i;
            const size_t source = static_cast<size_t>(jj) * width + ii;
            for (int band = 0; band < nWave; ++band) {
                const size_t bandOffset = static_cast<size_t>(band) * imageElements;
                const float value = sourceData[bandOffset + source];
                if (value != 0.0f) {
                    orthData[bandOffset + destination] = atmosphereCorrectOutputValue(
                        value, fileio->m_pVoxelrtXml->atmospherexml,
                        modelio->waves[band], fileio->m_pVoxelrtXml->sensorxml.position.z,
                        angle.vza, modelio->isTemperature);
                }
            }
        }
    }

    const float time = knode == -1 ? -1.0f : modelio->meteo.t;
    fileio->writeTIFData(
        modelio->projectDir, orthData.data(), width, height, nWave,
        angle, time, fileio->m_pVoxelrtXml->atmospherexml.enabled ? "_v_a" : "_v", -1, false,
        voxelrtBandNames(modelio->waves, modelio->isTemperature));
}

// 模块：透视投影或 UAV 航点观测的 TIFF 输出（不做正射重采样，像元与传感器像面一一对应）。
// 输入：knode（-1 表示无时间戳）、kpos（航点序号，-1 表示单一固定传感器）。
// 说明：太阳/视角信息取 angles[0]；传感器高度优先取该航点高度 uavposes[kpos].z。
//       对没有命中任何体元的像元（值为 0 或非有限）且位于视场内的，用天空辐射模型填充；
//       其余像元只做大气校正。
void Voxelrt::outputPos(std::shared_ptr<VoxelrtIO> &modelio, std::shared_ptr<FileIO> &fileio, int knode, int kpos) {
    VkBufferUsageFlags usage{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const int width = modelio->imageSize.x;
    const int height = modelio->imageSize.y;
    const int nWave = modelio->n_wave;
    if (width <= 0 || height <= 0 || nWave <= 0 || modelio->angles.empty()) {
        return;
    }

    const size_t totalElements = static_cast<size_t>(width) * height * nWave;
    const VkDeviceSize bufferSize = totalElements * sizeof(float);
    nvvk::Buffer pixelBuffer = modelio->m_pAlloc->createBuffer(
        bufferSize, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    m_pVirtual->bufferToBuffer(
        modelio, *(modelio->m_virtualio->m_pBufferStorage), bufferSize, pixelBuffer);

    void* mappedData = modelio->m_pAlloc->map(pixelBuffer);
    std::vector<float> outputData(totalElements);
    std::memcpy(outputData.data(), mappedData, static_cast<size_t>(bufferSize));
    modelio->m_pAlloc->unmap(pixelBuffer);
    modelio->m_pAlloc->destroy(pixelBuffer);

    // 透视多角度时调用方已把当前角度写入 k_angle；此前固定用 angles[0]
    // 会让每个角度覆盖同一 TIFF，并使用角度 0 的天空/大气参数。
    const int angleIndex = std::clamp(modelio->k_angle, 0, std::max(0, static_cast<int>(modelio->angles.size()) - 1));
    Angle angle = modelio->angles[static_cast<size_t>(angleIndex)];
    const float sensorHeight = kpos >= 0 && kpos < static_cast<int>(modelio->uavposes.size())
        ? modelio->uavposes[kpos].z : fileio->m_pVoxelrtXml->sensorxml.position.z;
    const size_t imageElements = static_cast<size_t>(width) * height;
    // 步骤 2：求每个像元对应的视线天顶角；负值表示该像元不是天空像元（由 atmosphereSkyPixelZenith
    //         的返回值判定，具体条件待核对）。
    std::vector<float> skyZenith(imageElements, -1.0f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float zenith = 0.0f;
            if (atmosphereSkyPixelZenith(
                    x, y, width, height,
                    fileio->m_pVoxelrtXml->sensorxml.sensorFov,
                    angle.vza, angle.vaa, zenith)) {
                skyZenith[static_cast<size_t>(y) * width + x] = zenith;
            }
        }
    }
    // 步骤 3：逐波段逐像元：空像元且为天空 → 天空输出值；否则 → 大气校正后的输出值。
    for (int band = 0; band < nWave; ++band) {
        const size_t offset = static_cast<size_t>(band) * imageElements;
        for (size_t pixel = 0; pixel < imageElements; ++pixel) {
            float& value = outputData[offset + pixel];
            if ((!std::isfinite(value) || value == 0.0f) && skyZenith[pixel] >= 0.0f) {
                value = atmosphereSkyOutputValue(
                    fileio->m_pVoxelrtXml->atmospherexml,
                    modelio->waves[band], sensorHeight, skyZenith[pixel],
                    modelio->isTemperature,
                    fileio->m_pVoxelrtXml->lightxml.skyTemperature);
            } else {
                value = atmosphereCorrectOutputValue(
                    value, fileio->m_pVoxelrtXml->atmospherexml,
                    modelio->waves[band], sensorHeight,
                    angle.vza, modelio->isTemperature);
            }
        }
    }
    const float time = knode == -1 ? -1.0f : modelio->meteo.t;
    fileio->writeTIFData(
        modelio->projectDir, outputData.data(), width, height, nWave,
        angle, time, fileio->m_pVoxelrtXml->atmospherexml.enabled ? "_v_a" : "_v", kpos, false,
        voxelrtBandNames(modelio->waves, modelio->isTemperature));
}


// 模块：体元通量/温度等 3D 输出的占位。函数体已全部注释掉（旧 VoxelEB 遗留），
// VoxelRT 现用 outputRadiationProcess 导出三维辐射场。
void Voxelrt::outputVoxel(std::shared_ptr<VoxelrtIO> &modelio, std::shared_ptr<FileIO> &fileio) {


//    VkBufferUsageFlags usage{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
//    int num = modelio->n_voxel;
//    VkDeviceSize bufferSize = num * sizeof(VoxelHeatflux);
//    nvvk::Buffer pixelBuffer = modelio->m_pAlloc->createBuffer(bufferSize, usage,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
//    m_pVirtual->bufferToBuffer(modelio, *(modelio->m_voxelio->m_pFluxBuffer), bufferSize, pixelBuffer);
//    // write the buffer to disk
//    void *data = modelio->m_pAlloc->map(pixelBuffer);
//    VoxelHeatflux *pData = reinterpret_cast<VoxelHeatflux *>(data);
//    VoxelHeatflux test0 = pData[0];
//    modelio->m_pAlloc->unmap(pixelBuffer);
//    modelio->m_pAlloc->destroy(pixelBuffer);


//    VkBufferUsageFlags usage{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
//    int num = modelio->n_voxel;
//    VkDeviceSize bufferSize = num * sizeof(VoxelRss);
//    nvvk::Buffer pixelBuffer = modelio->m_pAlloc->createBuffer(bufferSize, usage,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
//    m_pVirtual->bufferToBuffer(modelio, *(modelio->m_voxelio->m_pRssBuffer), bufferSize, pixelBuffer);
//    // write the buffer to disk
//    void *data = modelio->m_pAlloc->map(pixelBuffer);
//    VoxelRss *pData = reinterpret_cast<VoxelRss *>(data);
//    VoxelRss test0 = pData[0];
//    modelio->m_pAlloc->unmap(pixelBuffer);
//    modelio->m_pAlloc->destroy(pixelBuffer);


//    VkBufferUsageFlags usage{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
//    int num = modelio->n_voxel;
//    VkDeviceSize bufferSize = num * sizeof(VoxelTempe);
//    nvvk::Buffer pixelBuffer = modelio->m_pAlloc->createBuffer(bufferSize, usage,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
//    m_pVirtual->bufferToBuffer(modelio, *(modelio->m_voxelio->m_pTempeBuffer), bufferSize, pixelBuffer);
//    // write the buffer to disk
//    void *data = modelio->m_pAlloc->map(pixelBuffer);
//    VoxelTempe *pData = reinterpret_cast<VoxelTempe *>(data);
//    VoxelTempe test0 = pData[0];
//    modelio->m_pAlloc->unmap(pixelBuffer);
//    modelio->m_pAlloc->destroy(pixelBuffer);

}


//bool Voxelrt::uploadAero(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &modelio) {
//
//
//    if(fileio->m_pVoxelrtXml->aerocondxml.aerotype == AeroType::ONE) {
//        modelio->aeroconds.emplace_back(fileio->m_pVoxelrtXml->aerocondxml.aerocond);
//    }else if (fileio->m_pVoxelrtXml->aerocondxml.aerotype == AeroType::image)
//    {
//        int a = 10;
//    }else if(fileio->m_pVoxelrtXml->aerocondxml.aerotype == AeroType::gridCal){
//        int b = 10;
//    }
//
//    return false;
//
//
//}
