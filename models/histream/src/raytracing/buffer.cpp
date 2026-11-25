// ==========================================================================
// 文件：buffer.cpp（raytracing）
// 功能：实现 RayTracing 模式的 GPU 缓冲区上传与释放：光谱/热材料、波长集、网格顶点/索引及其设备地址、
//   实例链接、物理纹理因子、传感器与光源 uniform、加速结构、输出图像存储缓冲。
// 主要模块：
//   1. Buffer::createBuffer：按步骤创建全部缓冲（见函数内步骤 1–8）
//   2. Buffer::destroy：按创建的逆类别销毁
// 数据流：raytracingio（CPU 侧数据由 raytracing.cpp 的输入阶段准备好）→ 经 nvvk 分配器上传 →
//   m_p* 缓冲句柄保存在 RaytracingIO 中 → descriptor.cpp 绑定到着色器（binding 编号见 descriptor.h）。
// 注意：顶点/索引缓冲不放入描述符，着色器经 buffer_reference 通过设备地址访问（meshLinks）。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#include "buffer.h"
#include "base/outputsize.h"
#include <stdexcept>
#include <string>

// 作用：创建 RayTracing 所需的全部 GPU 缓冲，并构建加速结构。输入：raytracingio 中已准备好的 CPU 数据；
// 输出：写入各 m_pBuffer* 句柄；每个缓冲用独立命令缓冲提交并等待完成。
// 返回值：恒为 false，调用方 Raytracing::create 忽略它（返回约定待核对）。
bool Buffer::createBuffer(std::shared_ptr<RaytracingIO> &raytracingio) {

    ///--------------------------------------------------------------------
    ///  component properties
    ///--------------------------------------------------------------------

    VkDevice & m_device = raytracingio->m_device;
    nvvk::Queue &m_queue  = raytracingio->m_queues[eGCT];
    auto & meshio = raytracingio->m_meshio;
    auto & instanceio = raytracingio->m_instanceio;
    auto & virtualio = raytracingio->m_virtualio;
    auto & m_pAlloc = raytracingio->m_pAlloc;
    auto & m_pAccelStruct = raytracingio->m_pAccelStruct;
  //  auto & sceneio = raytracingIo->m_sceneio;


    // 步骤 1：创建命令池，随后每个缓冲上传各用一个命令缓冲，并 submitAndWait 同步完成。
    nvvk::CommandPool cmdGen(m_device, m_queue.familyIndex);

    // 步骤 2：光谱材料、热发射材料与波长集缓冲（storage buffer）。热材料缓冲即使为空也要创建，
    // 因为着色器总会读取该绑定。
    if (meshio->spectrals.size() > 0)
    {
        VkCommandBuffer cmdBufSpectral = cmdGen.createCommandBuffer();
        VkBufferUsageFlags     usage_ = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        meshio->m_pBufferSpectral = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufSpectral, meshio->spectrals, usage_));
        cmdGen.submitAndWait(cmdBufSpectral);
    }

    // offline.rgen always reads binding 1 (thermal emission), so it must exist.
    if (meshio->thermals.empty()) meshio->thermals.push_back(Thermal{300.0f, 300.0f});
    {
        VkCommandBuffer cmdBufThermal = cmdGen.createCommandBuffer();
        meshio->m_pBufferThermal = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufThermal, meshio->thermals, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        cmdGen.submitAndWait(cmdBufThermal);
    }

    if (raytracingio->waves.size() > 0)
    {
        VkCommandBuffer cmdBufWave = cmdGen.createCommandBuffer();
        raytracingio->m_pBufferWave = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufWave, raytracingio->waves, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        cmdGen.submitAndWait(cmdBufWave);
    }

    ///--------------------------------------------------------------------
    ///  Scene properties
    ///--------------------------------------------------------------------
    // 步骤 3：逐个模型（OBJ 网格）上传顶点与索引缓冲；它们既是存储缓冲（着色器按设备地址读取），
    // 又是构建加速结构的输入（带 DEVICE_ADDRESS 与 BUILD_INPUT_READ_ONLY 用途位）。
    // models buffer
    nvvk::CommandPool cmdBufGet(m_device, m_queue.familyIndex, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, m_queue.queue);
    for (int kmodel = 0; kmodel < meshio->objMeshes.size(); kmodel++)
    {

        VkCommandBuffer cmdBufModel = cmdBufGet.createCommandBuffer();
        MeshBuffer meshbuffer;
        meshbuffer.nbVertices = static_cast<uint32_t>(meshio->objMeshes[kmodel].nVertices);
        meshbuffer.nbIndices = static_cast<uint32_t>(meshio->objMeshes[kmodel].nIndices);

        meshbuffer.vertexBuffer =
                m_pAlloc->createBuffer(cmdBufModel, meshio->objMeshes[kmodel].vertices,
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                       | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
        meshbuffer.indexBuffer =
                m_pAlloc->createBuffer(cmdBufModel, meshio->objMeshes[kmodel].indices,
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                       | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
        cmdBufGet.submitAndWait(cmdBufModel);
        //m_pAlloc->finalizeAndReleaseStaging();
        meshio->m_bufferMeshes.emplace_back(meshbuffer);
    }

    // 步骤 4：写入每个模型的顶点/索引设备地址（meshLinks），再上传 meshLinks 缓冲（binding modelLink）。
    // modelLink
    VkCommandBuffer cmdBufModelLink = cmdGen.createCommandBuffer();
    for (int kmodel = 0; kmodel < meshio->objMeshes.size(); kmodel++)
    {

        meshio->meshLinks[kmodel].vertexAddress = nvvk::getBufferDeviceAddress(m_device, meshio->m_bufferMeshes[kmodel].vertexBuffer.buffer);
        meshio->meshLinks[kmodel].indexAddress = nvvk::getBufferDeviceAddress(m_device, meshio->m_bufferMeshes[kmodel].indexBuffer.buffer);
    }
    meshio->m_pBufferMeshLink = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufModelLink, meshio->meshLinks, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    cmdGen.submitAndWait(cmdBufModelLink);

    // 步骤 5：上传实例链接缓冲（instanceLinks，binding instanceLink）；没有实例时不创建。
    // instanceLink : add address first and then creat
    VkCommandBuffer cmdBufLink = cmdGen.createCommandBuffer();
    if (instanceio->instanceLinks.size() > 0)
        instanceio->m_pBufferInstanceLink = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufLink, instanceio->instanceLinks, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    cmdGen.submitAndWait(cmdBufLink);

    // 步骤 6：物理纹理因子：无纹理时放入 1 个占位值（偏移为 kNoTextureOffset、因子 1.0），保证绑定有效；
    // 并检查因子数组字节数不超过 GPU 的 maxStorageBufferRange，否则报错（每个实例三角形一个因子）。
    if (raytracingio->instanceTextureOffsets.empty()) raytracingio->instanceTextureOffsets.assign(1, kNoTextureOffset);
    if (raytracingio->textureFactors.empty()) raytracingio->textureFactors.assign(1, 1.0f);
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(raytracingio->m_physicalDevice, &properties);
        const uint64_t factorBytes = static_cast<uint64_t>(raytracingio->textureFactors.size()) * sizeof(float);
        if (factorBytes > properties.limits.maxStorageBufferRange)
            throw std::runtime_error("Physical texture factors need " + std::to_string(factorBytes) +
                " bytes (one per instance triangle), above this GPU's maxStorageBufferRange of " +
                std::to_string(properties.limits.maxStorageBufferRange) +
                " bytes; reduce textured instances/triangles or disable the physical texture");
    }
    VkCommandBuffer cmdBufTexture = cmdGen.createCommandBuffer();
    raytracingio->m_pBufferTextureOffset = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufTexture, raytracingio->instanceTextureOffsets, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    raytracingio->m_pBufferTextureFactor = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufTexture, raytracingio->textureFactors, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    cmdGen.submitAndWait(cmdBufTexture);

    ///--------------------------------------------------------------------
    ///  Accelerate properties
    ///--------------------------------------------------------------------
    // 步骤 7：用网格与实例构建加速结构（BLAS/TLAS）。
    m_pAccelStruct->createAccelStruct(m_device,meshio,instanceio);


    ///--------------------------------------------------------------------
    ///  geometry properties
    ///--------------------------------------------------------------------
    // 步骤 8：传感器与光源参数（uniform buffer）。同时具备 DEVICE_LOCAL 与 HOST_COHERENT，
    // 便于之后由主机直接更新（待核对）。
    VkCommandBuffer cmdBufSensor = cmdGen.createCommandBuffer();
    raytracingio->m_pBufferSensor = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufSensor, sizeof(SensorMatrix), &raytracingio->sensor,
                                                                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    cmdGen.submitAndWait(cmdBufSensor);

//    VkCommandBuffer cmdBufWaveInds = cmdGen.createCommandBuffer();
//    raytracingio->m_pBufferWaveInds = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufWaveInds, raytracingio->sensorWavelengthInds, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
//    cmdGen.submitAndWait(cmdBufWaveInds);

    VkCommandBuffer cmdBufLight = cmdGen.createCommandBuffer();
    raytracingio->m_pBufferLight = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufLight, sizeof(LightSet), &raytracingio->light,
                                                                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    cmdGen.submitAndWait(cmdBufLight);

    ///--------------------------------------------------------------------
    ///  Virtual Sceen properties
    ///--------------------------------------------------------------------
    // 步骤 9：输出图像存储缓冲，元素数由图像宽高与波段数 n_wave 决定（checkedOutputImageElements
    //  负责检查是否超出设备限制，待核对），初值为 0，结果由 raytracing.cpp 读回。
    VkCommandBuffer cmdBufStorage = cmdGen.createCommandBuffer();
    const size_t outputSize = checkedOutputImageElements(raytracingio->m_physicalDevice, raytracingio->imageSize.x, raytracingio->imageSize.y, raytracingio->n_wave);
    std::vector<float> outputImage(outputSize, 0.0);
    virtualio->m_pBufferStorage = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufStorage, outputImage, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufStorage);




    // 所有上传完成后释放暂存（staging）缓冲。
    m_pAlloc->finalizeAndReleaseStaging();

    return false;
}



// 作用：销毁 createBuffer 创建的缓冲。注意：m_pBufferWave、m_pBufferMeshLink、m_pBufferInstanceLink、
// m_pBufferSensor、m_pBufferLight 被无条件解引用，而 createBuffer 中波长集与实例链接是有条件创建的，
// 为空时会出错（疑似问题，仅报告）。
void Buffer::destroy(std::shared_ptr<RaytracingIO> &raytracingio)
{
    // destroy component
    if (raytracingio->m_meshio->m_pBufferSpectral)
    {
        raytracingio->m_pAlloc->destroy(*(raytracingio->m_meshio->m_pBufferSpectral));
    }
    if (raytracingio->m_meshio->thermals.size() > 0)
    {
        raytracingio->m_pAlloc->destroy(*(raytracingio->m_meshio->m_pBufferThermal));
    }
    raytracingio->m_pAlloc->destroy(*(raytracingio->m_pBufferWave));

    // destroy scene
    for (auto& bufferModelTemp : raytracingio->m_meshio->m_bufferMeshes)
    {
        raytracingio->m_pAlloc->destroy(bufferModelTemp.indexBuffer);
        raytracingio->m_pAlloc->destroy(bufferModelTemp.vertexBuffer);
    }
    raytracingio->m_pAlloc->destroy(*(raytracingio->m_meshio->m_pBufferMeshLink));
    raytracingio->m_pAlloc->destroy(*(raytracingio->m_instanceio->m_pBufferInstanceLink));
    if (raytracingio->m_pBufferTextureOffset) raytracingio->m_pAlloc->destroy(*(raytracingio->m_pBufferTextureOffset));
    if (raytracingio->m_pBufferTextureFactor) raytracingio->m_pAlloc->destroy(*(raytracingio->m_pBufferTextureFactor));

    raytracingio->m_pAlloc->destroy(*(raytracingio->m_pBufferSensor));
    raytracingio->m_pAlloc->destroy(*(raytracingio->m_pBufferLight));

   // raytracingio->m_pAccelStruct->m_rtBuilder.destroy();

    // output
    raytracingio->m_pAlloc->destroy(*(raytracingio->m_virtualio->m_pBufferStorage));

}


