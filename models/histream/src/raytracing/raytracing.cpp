//
// Created by admin on 2024/1/24.
//

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include "raytracing.h"
#include "src/base/atmosphere_lut.h"
#include "src/base/reflectance_texture.h"

namespace {

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

    for (size_t k = 0; k < samples.size(); ++k) {
        Statistics& stats = statistics[owners[k]];
        const float mean = stats.area > 0.0 ? static_cast<float>(stats.areaWeightedSample / stats.area) : 0.0f;
        samples[k] -= mean;
        stats.deviation.area += areas[k];
        stats.deviation.areaWeightedSquare += static_cast<double>(areas[k]) * samples[k] * samples[k];
        stats.deviation.minimumDeviation = std::min(stats.deviation.minimumDeviation, samples[k]);
        stats.deviation.maximumDeviation = std::max(stats.deviation.maximumDeviation, samples[k]);
    }
    const size_t bandCount = waves.size();
    for (size_t s = 0; s < statistics.size(); ++s) {
        Statistics& stats = statistics[s];
        if (!(stats.area > 0.0)) continue;
        float maximumFactor = std::numeric_limits<float>::infinity();
        for (size_t band = 0; band < bandCount; ++band) {
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
}


bool Raytracing::setup( AppSetting &appsetting, std::shared_ptr<RaytracingIO> &raytracingio){


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

    raytracingio->m_genCmdBuf.init(raytracingio->m_device,raytracingio->m_queueIndex);


//    VkPipelineCacheCreateInfo pipelineCacheInfo{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
//    vkCreatePipelineCache(raytracingio->m_device, &pipelineCacheInfo, nullptr, &raytracingio->m_pipelineCache);

    raytracingio->m_pAlloc  = std::make_shared<Allocator>();
    raytracingio->m_pAlloc->init(raytracingio->m_instance, raytracingio->m_device, raytracingio->m_physicalDevice);
    raytracingio->m_debug.setup(raytracingio->m_device);

    /// requesting ray tracing properties. from physical device.
    VkPhysicalDeviceProperties2 rayTracingProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    rayTracingProperties.pNext = &(raytracingio->m_rtProperties);
    vkGetPhysicalDeviceProperties2(raytracingio->m_physicalDevice, &rayTracingProperties);

    if (raytracingio->useSBTWrapper)
    {
        raytracingio->m_sbtWrapper.setup(raytracingio->m_device, raytracingio->m_queueIndex, raytracingio->m_pAlloc.get(), raytracingio->m_rtProperties);
    }

    raytracingio->m_pAccelStruct->m_rtBuilder.setup(raytracingio->m_device, raytracingio->m_pAlloc.get(),raytracingio->m_queueIndex);

    return true;

}




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

bool Raytracing::create(std::shared_ptr<RaytracingIO> &raytracingio) {


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

    return true;
}

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

    return true;
    //raytracingio->setting.maxDepth = fileio->m_pXmlInput.
}


bool Raytracing::run(std::shared_ptr<RaytracingIO> &raytracingio, std::shared_ptr<FileIO> &fileio) {

    std::cout << "PROGRESS\t5\t加载光线追踪场景与观测参数" << std::endl;

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


    if (raytracingio->isUAVTrave && raytracingio->n_pos > 0)
    {
        raytracingio->kangle = 0;
        for (int kpos = 0; kpos < raytracingio->n_pos; ++kpos) {
            raytracingio->k_pos = kpos;
            m_pGeometry->updateSensorPos(raytracingio, kpos);
            const glm::vec3 position = raytracingio->uavposes[kpos];
            std::cout << "OBSERVATION\t航点 " << (kpos + 1) << "/" << raytracingio->n_pos
                      << " X=" << position.x << " Y=" << position.y
                      << " H=" << position.z << std::endl;
            m_pCommand->run(raytracingio);
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
            m_pCommand->run(raytracingio);

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



void Raytracing::outputOrth(std::shared_ptr<RaytracingIO> &modelio, std::shared_ptr<FileIO> &fileio, int kangle, int kpos) {
VkBufferUsageFlags usage{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    int width = modelio->imageSize.x;
    int height = modelio->imageSize.y;
    int n_wave = modelio->n_wave;

    // 【修正1】使用 size_t 防止整数溢出 (3000*3000*100 超过 int 上限)
    size_t total_elements = (size_t)width * height * n_wave;
    VkDeviceSize bufferSize = total_elements * sizeof(float);

    nvvk::Buffer pixelBuffer = modelio->m_pAlloc->createBuffer(bufferSize, usage,
                                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    m_pVirtual->bufferToBuffer(modelio, *(modelio->m_virtualio->m_pBufferStorage), bufferSize, pixelBuffer);

    // map memory
    void *data = modelio->m_pAlloc->map(pixelBuffer);

    // 【修正2】创建 CPU 端内存副本 (Deep Copy)
    // 这是解决 0xc0000005 崩溃的核心：将显存数据拷贝到 CPU，使其脱离 Vulkan 生命周期
    float* pCpuData = new float[total_elements];
    if (pCpuData) {
        // 使用 memcpy 高速拷贝
        std::memcpy(pCpuData, data, bufferSize);
    } else {
        // 内存分配失败处理 (通常是因为没有编译为 x64)
        std::cerr << "Error: Failed to allocate CPU memory. Make sure to compile in x64 mode!" << std::endl;
        modelio->m_pAlloc->unmap(pixelBuffer);
        modelio->m_pAlloc->destroy(pixelBuffer);
        return; // 或者适当的错误处理
    }

    // --- 此时数据已安全在 pCpuData 中，不再依赖 pData/data 指针 ---

    // 【修正3】现在可以安全销毁 Vulkan 资源了
    modelio->m_pAlloc->unmap(pixelBuffer);
    modelio->m_pAlloc->destroy(pixelBuffer);

    Angle angle = modelio->angles[kangle];
    std::vector<float> waves = modelio->waves;
    glm::vec2 resolution = modelio->imageSize;

    // 计算校正参数
    Eigen::VectorXd cx;
    Eigen::VectorXd cy;
    m_pGeometry->orthcorrect(modelio, angle.vza, angle.vaa, cx, cy);
    std::cout << "x' = " << cx[0]<<std::endl;
    // 分配结果内存
    float *pData_orth = new float[total_elements];
    std::memset(pData_orth, 0, bufferSize);

    // --- 正射校正循环 (已优化健壮性) ---
    for(int i = 0; i < width; i++)
    {
        for(int j = 0; j < height; j++)
        {
            // 目标图像索引 (使用 long long 防止溢出)
            long long old = (long long)j * width + i;

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
                pData_orth[oldd] = atmosphereCorrectOutputValue(
                    static_cast<float>(valueSum),
                    fileio->m_pRaytracingXml->atmospherexml,
                    waves[k], fileio->m_pRaytracingXml->sensorxml.position.z,
                    angle.vza, modelio->isTemperature);

            }
        }
    }


    fileio->writeTIFData(
        modelio->projectDir, pData_orth, width, height, n_wave,
        angle, -1.0f, fileio->m_pRaytracingXml->atmospherexml.enabled ? "_a" : "", kpos, false,
        raytracingBandNames(waves, modelio->isTemperature));

    // 释放内存
    delete[] pCpuData;    // 记得释放拷贝的源数据
    delete[] pData_orth;  // 释放结果数据
}

void Raytracing::output(std::shared_ptr<RaytracingIO>& modelio, std::shared_ptr<FileIO>& fileio, int kangle, int kpos)
{
    VkBufferUsageFlags usage{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    int width = modelio->imageSize.x;
    int height = modelio->imageSize.y;
    int n_wave = modelio->n_wave;

    // 【修正1】使用 size_t 防止整数溢出 (3000*3000*100 超过 int 上限)
    size_t total_elements = (size_t)width * height * n_wave;
    VkDeviceSize bufferSize = total_elements * sizeof(float);

    nvvk::Buffer pixelBuffer = modelio->m_pAlloc->createBuffer(bufferSize, usage,
                                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    m_pVirtual->bufferToBuffer(modelio, *(modelio->m_virtualio->m_pBufferStorage), bufferSize, pixelBuffer);

    // map memory
    void *data = modelio->m_pAlloc->map(pixelBuffer);

    // 【修正2】创建 CPU 端内存副本 (Deep Copy)
    // 这是解决 0xc0000005 崩溃的核心：将显存数据拷贝到 CPU，使其脱离 Vulkan 生命周期
    float* pCpuData = new float[total_elements];
    if (pCpuData) {
        // 使用 memcpy 高速拷贝
        std::memcpy(pCpuData, data, bufferSize);
    } else {
        // 内存分配失败处理 (通常是因为没有编译为 x64)
        std::cerr << "Error: Failed to allocate CPU memory. Make sure to compile in x64 mode!" << std::endl;
        modelio->m_pAlloc->unmap(pixelBuffer);
        modelio->m_pAlloc->destroy(pixelBuffer);
        return; // 或者适当的错误处理
    }

    // --- 此时数据已安全在 pCpuData 中，不再依赖 pData/data 指针 ---

    // 【修正3】现在可以安全销毁 Vulkan 资源了
    modelio->m_pAlloc->unmap(pixelBuffer);
    modelio->m_pAlloc->destroy(pixelBuffer);

    Angle angle = modelio->angles[kangle];
    std::vector<float> waves = modelio->waves;
    glm::vec2 resolution = modelio->imageSize;

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

    fileio->writeTIFData(
        modelio->projectDir, pCpuData, width, height, n_wave,
        angle, -1.0f, fileio->m_pRaytracingXml->atmospherexml.enabled ? "_a" : "", kpos, false,
        raytracingBandNames(waves, modelio->isTemperature));

    // 释放内存
    delete[] pCpuData;    // 记得释放拷贝的源数据
}

void Raytracing::outputAlbedo(std::shared_ptr<RaytracingIO>& modelio, std::shared_ptr<FileIO>& fileio, int kangle)
{
    VkBufferUsageFlags usage{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    int width = modelio->imageSize.x;
    int height = modelio->imageSize.y;
    int n_wave = modelio->n_wave;
    VkDeviceSize bufferSize = static_cast<VkDeviceSize>(width) * height * n_wave * sizeof(float);
    nvvk::Buffer pixelBuffer = modelio->m_pAlloc->createBuffer(bufferSize, usage,
                                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    m_pVirtual->bufferToBuffer(modelio, *(modelio->m_virtualio->m_pBufferStorage), bufferSize, pixelBuffer);

    // write the buffer to disk
    void *data = modelio->m_pAlloc->map(pixelBuffer);
    float *pData = reinterpret_cast<float *>(data);

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


bool Raytracing::destroy( std::shared_ptr<RaytracingIO> &raytracingio)
{

    m_pBuffer->destroy(raytracingio);
    m_pDescriptor->destroy(raytracingio);
    m_pPipeline->destroy(raytracingio);
    m_pCommand->destroy(raytracingio);

    raytracingio->m_sbtWrapper.destroy();
    raytracingio->m_pAccelStruct->m_rtBuilder.destroy();

    vkDeviceWaitIdle(raytracingio->m_device);
    raytracingio->m_genCmdBuf.deinit();
//    vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
//    vkDestroyCommandPool(m_device, m_cmdPool, nullptr);
    raytracingio->m_pAlloc->deinit();

    return true;
}
