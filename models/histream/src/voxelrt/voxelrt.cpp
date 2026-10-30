//
// Created by admin on 2024/1/26.
//

#include "voxelrt.h"
#include "src/base/atmosphere_lut.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace {
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

void writeVoxelrtProcessField(std::ostream& metadata, const float wavelength,
                             const int offset) {
    const bool thermal = wavelength > 5000.0f;
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
    m_pGeometry->createGeometry(fileio,modelio);
//    defineOPO(modelio);
   // uploadMeteo(fileio,modelio);
  //  uploadAero(fileio,modelio);

    return true;
}

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

    return true;
}

bool Voxelrt::updateSetting(std::shared_ptr<VoxelrtIO> &modelio){

    // auto &opo = modelio->m_opo;

    // auto &sceneio = modelio->m_sceneio;

    modelio->setting.imageSize = modelio->imageSize;
    modelio->setting.n_wave = modelio->n_wave;
    modelio->setting.scale = modelio->stepsize_surface;
    modelio->setting.isTemperature = modelio->isTemperature ? 1 : 0;
    modelio->setting.isDisplay = modelio->isDisplay;
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



bool Voxelrt::uploadDefined(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &modelio)
{

    fileio->readDefined(modelio->m_defined);

    return false;
}

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

bool Voxelrt::run(std::shared_ptr<VoxelrtIO> &modelio, std::shared_ptr<FileIO> &fileio) {



    modelio->k_node = -1;
    std::cout << "PROGRESS\t5\t加载体元场景与辐射参数" << std::endl;


    if(modelio->isUAVTrave && modelio->n_pos > 0) {

        for (int kpos = 0; kpos < modelio->n_pos; kpos++) {
            modelio->k_pos = kpos;
            m_pGeometry->updateSensorPos(modelio, kpos);
            const glm::vec3 position = modelio->uavposes[kpos];
            std::cout << "OBSERVATION\t航点 " << (kpos + 1) << "/" << modelio->n_pos
                      << " X=" << position.x << " Y=" << position.y
                      << " H=" << position.z << std::endl;
//            Angle angle = modelio->angles[kangle];
//            std::cout << "Angle Info:"
//                      << "    vza_" << std::to_string(angle.vza) << "    vaa_" << std::to_string(angle.vaa)
//                      << "    sza_" << std::to_string(angle.sza) << "    saa_" << std::to_string(angle.saa)
//                      << std::endl;
            if (modelio->acceleratedRadiationSolver) {
                m_pCommand->runRTAccelerated(modelio);
            } else {
                m_pCommand->runRT(modelio);
            }
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
            if (modelio->acceleratedRadiationSolver) {
                m_pCommand->runRTAccelerated(modelio);
            } else {
                m_pCommand->runRT(modelio);
            }
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

void Voxelrt::outputRadiationProcess(std::shared_ptr<VoxelrtIO>& modelio) {
    if (!modelio || modelio->n_voxel <= 0 || !modelio->m_voxelio ||
        !modelio->m_voxelio->m_pDirBuffer || !modelio->m_voxelio->m_pRadsBuffer) {
        return;
    }

    const size_t voxelCount = static_cast<size_t>(modelio->n_voxel);
    const int bandCount = modelio->n_wave;
    const size_t radiationStride = static_cast<size_t>(DIFFUSENUM) +
                                   2U * static_cast<size_t>(modelio->n_wave);
    std::vector<VoxelDir> directions(voxelCount);
    // DEFERRED[低] 内存: 整个 rads 缓冲（含 64 方向暂存区，n_voxel×(64+2n_wave)×8 B）连同等大的 host-visible staging 一起回读，而这里只用到每体元 n_wave 个环境项；大场景可占用数 GB 主机内存；建议：用带偏移/步长的 vkCmdCopyBuffer 区域只回读所需的 DIFFUSENUM..DIFFUSENUM+n_wave 段。
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
        const MeshLink* mesh = nullptr;
        if (link.instanceId >= 0 &&
            link.instanceId < static_cast<int>(modelio->m_instanceio->instanceLinks.size())) {
            const uint32_t meshId = modelio->m_instanceio->instanceLinks[link.instanceId].meshId;
            if (meshId < modelio->m_meshio->meshLinks.size()) {
                mesh = &modelio->m_meshio->meshLinks[meshId];
            }
        }
        for (int band = 0; band < bandCount; ++band) {
            const size_t environmentIndex = voxel * radiationStride +
                static_cast<size_t>(DIFFUSENUM) + static_cast<size_t>(band);
            const float environmentValue = radiosities[environmentIndex].cumulated;
            const float environmentRadiance =
                std::isfinite(environmentValue) ? std::max(0.0f, environmentValue) : 0.0f;
            const float wavelength = band < static_cast<int>(modelio->waves.size())
                ? modelio->waves[band] : static_cast<float>(band + 1);
            // DEFERRED[中] 物理: 热红外判定阈值为 >5000 nm，而 voxelrtBandNames 用 >2.5 µm、FacetRT 用 >2500 nm；3–5 µm 中波红外波段在此被当作光学波段（加上 light.direct、不计发射），过程输出与图像波段名含义不一致；建议：统一用一个共享的 isThermalWavelength() 判定。
            const bool isThermalBand = wavelength > 5000.0f;
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

            const float averagedRadiance =
                sunlitFraction * sunlitRadiance +
                (1.0f - sunlitFraction) * shadedRadiance;
            binary.write(reinterpret_cast<const char*>(&averagedRadiance), sizeof(averagedRadiance));
        }
        binary.write(reinterpret_cast<const char*>(&sunlitFraction), sizeof(sunlitFraction));
    }
    binary.close();

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
    std::cout << "PROCESS\t" << metadataPath.string() << std::endl;
}



bool Voxelrt::destroy(std::shared_ptr<VoxelrtIO> & modelio){

    // FIXED[中] GPU同步: vkDeviceWaitIdle 放在销毁缓冲/描述符/管线之后才调用；若有仍在执行的提交（如异常路径跳过 waitFence），先销毁正在使用的资源是未定义行为；已修复：把 vkDeviceWaitIdle 提前到销毁任何资源之前，并检查其返回值（失败只记录，后续仍继续销毁）。
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

    Angle angle = modelio->angles[0];
    const float sensorHeight = kpos >= 0 && kpos < static_cast<int>(modelio->uavposes.size())
        ? modelio->uavposes[kpos].z : fileio->m_pVoxelrtXml->sensorxml.position.z;
    const size_t imageElements = static_cast<size_t>(width) * height;
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
