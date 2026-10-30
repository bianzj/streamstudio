//
// Created by admin on 2024/1/25.
//

#include "descriptor.h"


bool Descriptor::createDescriptor(std::shared_ptr<VoxelebIO> &modelio){

    auto & m_device = modelio->m_device;
    auto & bindings = modelio->m_bindings;
    auto & m_descSetLayout = modelio->m_descSetLayout;
    auto & m_descSetPool = modelio->m_descPool;
    auto & m_descSet = modelio->m_descSet;
    auto & voxelio = modelio->m_voxelio;
    auto & defined = modelio->m_defined;
    auto & meshio = modelio->m_meshio;
    auto & surfio = modelio->m_surfio;
    auto & instanceio = modelio->m_instanceio;
    auto & virtualio = modelio->m_virtualio;
 //   auto & sceneio = modelio->m_sceneio;


    VkShaderStageFlags flags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
    bindings.addBinding(VoxelebbindingInd::spectral, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::fixedSpectral, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::thermal, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::tempe, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::canopy, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::meshLink, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::instanceLink, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::voxelLink, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::nano, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::tlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, flags | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
    bindings.addBinding(VoxelebbindingInd::sensor, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::wave, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::light, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::atomcond, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::dir, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::netRad, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::pnet, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);

    bindings.addBinding(VoxelebbindingInd::storage, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags );
    bindings.addBinding(VoxelebbindingInd::meteo, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::aerocond, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);

    bindings.addBinding(VoxelebbindingInd::raa, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::leafBio, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::soilSet, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);

    bindings.addBinding(VoxelebbindingInd::rss, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::air, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::flux, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);

    bindings.addBinding(VoxelebbindingInd::tLast, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);

    bindings.addBinding(VoxelebbindingInd::state, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::lad, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::waterSet, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::fluidVelocityA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::fluidVelocityB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::fluidScalarA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::fluidScalarB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::fluidLbmA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::fluidLbmB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::fluidDensity, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::fluidMeta, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::fluidParameters, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, flags);
    bindings.addBinding(VoxelebbindingInd::hex, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);


    m_descSetLayout = bindings.createLayout(m_device);
    m_descSetPool = bindings.createPool(m_device, 1);
    m_descSet = nvvk::allocateDescriptorSet(m_device, m_descSetPool, m_descSetLayout);

    std::vector<VkWriteDescriptorSet> updates;

    // component    

    VkDescriptorBufferInfo dbiSpectral{meshio->m_pBufferSpectral->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::spectral, &dbiSpectral));


    VkDescriptorBufferInfo dbiFixedSpectral{meshio->m_pFixedSpectralBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::fixedSpectral, &dbiFixedSpectral));

    VkDescriptorBufferInfo dbiThermal{};
    if (meshio->thermals.size() > 0)
    {
        dbiThermal = {meshio->m_pBufferThermal->buffer, 0, VK_WHOLE_SIZE};
        updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::thermal, &dbiThermal));
    }

    VkDescriptorBufferInfo dbiTempe{voxelio->m_pTempeBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::tempe, &dbiTempe));

    VkDescriptorBufferInfo dbiCanopy{meshio->m_pBufferCanopy->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::canopy, &dbiCanopy));

    // link

    VkDescriptorBufferInfo dbiMeshLink{meshio->m_pBufferMeshLink->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::meshLink, &dbiMeshLink));

    VkDescriptorBufferInfo dbiInstanceLink{instanceio->m_pBufferInstanceLink->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::instanceLink, &dbiInstanceLink));

    VkDescriptorBufferInfo dbiVoxelLink{voxelio->m_pVoxelLinkBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::voxelLink, &dbiVoxelLink));

    VkDescriptorBufferInfo dbiNano{voxelio->m_pVoxelNanoBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::nano, &dbiNano));

    // Accelerate structure
    VkAccelerationStructureKHR tlas = modelio->m_pAccelStruct->getTlas();
    VkWriteDescriptorSetAccelerationStructureKHR descASInfo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    descASInfo.accelerationStructureCount = 1;
    descASInfo.pAccelerationStructures = &tlas;
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::tlas, &descASInfo));

    // sensor and light
    VkDescriptorBufferInfo dbiSensor{modelio->m_pBufferSensor->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::sensor, &dbiSensor));
    VkDescriptorBufferInfo dbiWave{modelio->m_pBufferWave->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::wave, &dbiWave));
    VkDescriptorBufferInfo dbiLight{modelio->m_pBufferLight->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::light, &dbiLight));


    VkDescriptorBufferInfo dbiAtomcond{modelio->m_pBufferAtomcond->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::atomcond, &dbiAtomcond));

    VkDescriptorBufferInfo dbiDir{voxelio->m_pDirBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::dir, &dbiDir));
    VkDescriptorBufferInfo dbiNetRad{voxelio->m_pNetRadBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::netRad, &dbiNetRad));
    VkDescriptorBufferInfo dbiPnet{voxelio->m_pPnetBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::pnet, &dbiPnet));

    VkDescriptorBufferInfo dbiStore{virtualio->m_pBufferStorage->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::storage, &dbiStore));

    VkDescriptorBufferInfo dbiMeteo{modelio->m_pMeteoBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::meteo, &dbiMeteo));

    VkDescriptorBufferInfo dbiAero{modelio->m_pBufferAero->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::aerocond, &dbiAero));


    VkDescriptorBufferInfo dbiRaa{voxelio->m_pRaaBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::raa, &dbiRaa));


    VkDescriptorBufferInfo dbiLeafbio{meshio->m_pLeafBioBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::leafBio, &dbiLeafbio));


    VkDescriptorBufferInfo dbiSoilset{meshio->m_pSoilSetBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::soilSet, &dbiSoilset));

    VkDescriptorBufferInfo dbiRss{voxelio->m_pRssBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::rss, &dbiRss));

    VkDescriptorBufferInfo dbiAir{voxelio->m_pAirBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::air, &dbiAir));

    VkDescriptorBufferInfo dbiFlux{voxelio->m_pFluxBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::flux, &dbiFlux));

    VkDescriptorBufferInfo dbiTlast{voxelio->m_pTLASTBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::tLast, &dbiTlast));

    VkDescriptorBufferInfo dbiState{voxelio->m_pStateBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::state, &dbiState));

    VkDescriptorBufferInfo dbiLad{surfio->m_pBufferLad->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::lad, &dbiLad));

    VkDescriptorBufferInfo dbiWaterset{meshio->m_pWaterSetBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::waterSet, &dbiWaterset));

    VkDescriptorBufferInfo dbiFluidVelocityA{modelio->m_pFluidVelocityA->buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo dbiFluidVelocityB{modelio->m_pFluidVelocityB->buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo dbiFluidScalarA{modelio->m_pFluidScalarA->buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo dbiFluidScalarB{modelio->m_pFluidScalarB->buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo dbiFluidLbmA{modelio->m_pFluidLbmA->buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo dbiFluidLbmB{modelio->m_pFluidLbmB->buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo dbiFluidDensity{modelio->m_pFluidDensity->buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo dbiFluidMeta{modelio->m_pFluidMeta->buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo dbiFluidParameters{modelio->m_pFluidParameters->buffer, 0, sizeof(FluidParameters)};
    VkDescriptorBufferInfo dbiHex{voxelio->m_pVoxelHexBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::fluidVelocityA, &dbiFluidVelocityA));
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::fluidVelocityB, &dbiFluidVelocityB));
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::fluidScalarA, &dbiFluidScalarA));
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::fluidScalarB, &dbiFluidScalarB));
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::fluidLbmA, &dbiFluidLbmA));
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::fluidLbmB, &dbiFluidLbmB));
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::fluidDensity, &dbiFluidDensity));
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::fluidMeta, &dbiFluidMeta));
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::fluidParameters, &dbiFluidParameters));
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::hex, &dbiHex));

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(updates.size()), updates.data(), 0, nullptr);
    return true;
}

void Descriptor::destroy(std::shared_ptr<VoxelebIO> &modelio) {

    vkDestroyDescriptorPool(modelio->m_device, modelio->m_descPool, nullptr);
    vkDestroyDescriptorSetLayout(modelio->m_device, modelio->m_descSetLayout, nullptr);


    modelio->m_descPool = VkDescriptorPool();
    modelio->m_descSetLayout = VkDescriptorSetLayout();
    modelio->m_descSet = VkDescriptorSet();

}









