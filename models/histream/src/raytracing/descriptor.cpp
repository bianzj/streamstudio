//
// Created by admin on 2024/1/25.
//

#include "descriptor.h"


bool Descriptor::createDescriptor(std::shared_ptr<RaytracingIO> &raytracingio){

    auto & m_device = raytracingio->m_device;
    auto & bindings = raytracingio->m_bindings;
    auto & m_descSetLayout = raytracingio->m_descSetLayout;
    auto & m_descPool = raytracingio->m_descPool;
    auto & m_descSet = raytracingio->m_descSet;
    auto & meshio = raytracingio->m_meshio;
    auto & instanceio = raytracingio->m_instanceio;
    auto & virtualio = raytracingio->m_virtualio;
 //   auto & sceneio = raytracingio->m_sceneio;

    
    bindings.addBinding(RaytracingbindingInd::spectral, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    bindings.addBinding(RaytracingbindingInd::thermal, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    bindings.addBinding(RaytracingbindingInd::modelLink, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    bindings.addBinding(RaytracingbindingInd::instanceLink, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    bindings.addBinding(RaytracingbindingInd::tlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    bindings.addBinding(RaytracingbindingInd::sensor, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    //bindings.addBinding(RaytracingbindingInd::waveInd, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    bindings.addBinding(RaytracingbindingInd::light, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    bindings.addBinding(RaytracingbindingInd::wave, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    bindings.addBinding(RaytracingbindingInd::storage, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    bindings.addBinding(RaytracingbindingInd::textureOffset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    bindings.addBinding(RaytracingbindingInd::textureFactor, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);

    m_descSetLayout = bindings.createLayout(m_device);
    m_descPool = bindings.createPool(m_device, 1);
    m_descSet = nvvk::allocateDescriptorSet(m_device, m_descPool, m_descSetLayout);

    /// <summary>
    /// get needed buffers and update the descriptor sets.
    /// temBuffer needed corrected.
    /// </summary>
    std::vector<VkWriteDescriptorSet> updates;
    VkDescriptorBufferInfo dbiSpectral{ meshio->m_pBufferSpectral->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, RaytracingbindingInd::spectral, &dbiSpectral));
    VkDescriptorBufferInfo dbiThermal{ meshio->m_pBufferThermal->buffer, 0, VK_WHOLE_SIZE };
    updates.emplace_back(bindings.makeWrite(m_descSet, RaytracingbindingInd::thermal, &dbiThermal));
    VkDescriptorBufferInfo dbiModelLink{meshio->m_pBufferMeshLink->buffer, 0, VK_WHOLE_SIZE };
    updates.emplace_back(bindings.makeWrite(m_descSet, RaytracingbindingInd::modelLink, &dbiModelLink));

    VkDescriptorBufferInfo dbiInstance{ instanceio->m_pBufferInstanceLink->buffer, 0, VK_WHOLE_SIZE };
    updates.emplace_back(bindings.makeWrite(m_descSet, RaytracingbindingInd::instanceLink, &dbiInstance));

    VkAccelerationStructureKHR tlas = raytracingio->m_pAccelStruct->getTlas(); // get from m_accel
    VkWriteDescriptorSetAccelerationStructureKHR descASInfo{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    descASInfo.accelerationStructureCount = 1;
    descASInfo.pAccelerationStructures = &tlas;
    updates.emplace_back(bindings.makeWrite(m_descSet, RaytracingbindingInd::tlas, &descASInfo));

    VkDescriptorBufferInfo dbiSensor{ raytracingio->m_pBufferSensor->buffer, 0, VK_WHOLE_SIZE };
    updates.emplace_back(bindings.makeWrite(m_descSet, RaytracingbindingInd::sensor, &dbiSensor));
//    VkDescriptorBufferInfo dbiWaveInd{ raytracingio->m_pBufferWaveInds->buffer, 0, VK_WHOLE_SIZE };
//    updates.emplace_back(bindings.makeWrite(m_descSet, RaytracingbindingInd::waveInd, &dbiWaveInd));
    VkDescriptorBufferInfo dbiLight{ raytracingio->m_pBufferLight->buffer, 0, VK_WHOLE_SIZE };
    updates.emplace_back(bindings.makeWrite(m_descSet, RaytracingbindingInd::light, &dbiLight));
    VkDescriptorBufferInfo dbiWave{ raytracingio->m_pBufferWave->buffer, 0, VK_WHOLE_SIZE };
    updates.emplace_back(bindings.makeWrite(m_descSet, RaytracingbindingInd::wave, &dbiWave));


    VkDescriptorBufferInfo dbiStorage{ virtualio->m_pBufferStorage->buffer, 0, VK_WHOLE_SIZE };
    updates.emplace_back(bindings.makeWrite(m_descSet, RaytracingbindingInd::storage, &dbiStorage));
    VkDescriptorBufferInfo dbiTextureOffset{ raytracingio->m_pBufferTextureOffset->buffer, 0, VK_WHOLE_SIZE };
    updates.emplace_back(bindings.makeWrite(m_descSet, RaytracingbindingInd::textureOffset, &dbiTextureOffset));
    VkDescriptorBufferInfo dbiTextureFactor{ raytracingio->m_pBufferTextureFactor->buffer, 0, VK_WHOLE_SIZE };
    updates.emplace_back(bindings.makeWrite(m_descSet, RaytracingbindingInd::textureFactor, &dbiTextureFactor));

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(updates.size()), updates.data(), 0, nullptr);
    return true;
}

void Descriptor::destroy(std::shared_ptr<RaytracingIO> &raytracingio) {

    vkDestroyDescriptorPool(raytracingio->m_device, raytracingio->m_descPool, nullptr);
    vkDestroyDescriptorSetLayout(raytracingio->m_device, raytracingio->m_descSetLayout, nullptr);


    raytracingio->m_descPool = VkDescriptorPool();
    raytracingio->m_descSetLayout = VkDescriptorSetLayout();
    raytracingio->m_descSet = VkDescriptorSet();


}