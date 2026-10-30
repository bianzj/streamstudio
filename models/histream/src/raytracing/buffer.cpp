//
// Created by admin on 2024/1/24.
//

#include "buffer.h"
#include "base/outputsize.h"
#include <stdexcept>
#include <string>

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


    nvvk::CommandPool cmdGen(m_device, m_queue.familyIndex);

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

    // modelLink
    VkCommandBuffer cmdBufModelLink = cmdGen.createCommandBuffer();
    for (int kmodel = 0; kmodel < meshio->objMeshes.size(); kmodel++)
    {

        meshio->meshLinks[kmodel].vertexAddress = nvvk::getBufferDeviceAddress(m_device, meshio->m_bufferMeshes[kmodel].vertexBuffer.buffer);
        meshio->meshLinks[kmodel].indexAddress = nvvk::getBufferDeviceAddress(m_device, meshio->m_bufferMeshes[kmodel].indexBuffer.buffer);
    }
    meshio->m_pBufferMeshLink = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufModelLink, meshio->meshLinks, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    cmdGen.submitAndWait(cmdBufModelLink);

    // instanceLink : add address first and then creat
    VkCommandBuffer cmdBufLink = cmdGen.createCommandBuffer();
    if (instanceio->instanceLinks.size() > 0)
        instanceio->m_pBufferInstanceLink = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufLink, instanceio->instanceLinks, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    cmdGen.submitAndWait(cmdBufLink);

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
    m_pAccelStruct->createAccelStruct(m_device,meshio,instanceio);


    ///--------------------------------------------------------------------
    ///  geometry properties
    ///--------------------------------------------------------------------
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
    VkCommandBuffer cmdBufStorage = cmdGen.createCommandBuffer();
    const size_t outputSize = checkedOutputImageElements(raytracingio->m_physicalDevice, raytracingio->imageSize.x, raytracingio->imageSize.y, raytracingio->n_wave);
    std::vector<float> outputImage(outputSize, 0.0);
    virtualio->m_pBufferStorage = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufStorage, outputImage, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufStorage);




    m_pAlloc->finalizeAndReleaseStaging();

    return false;
}



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


