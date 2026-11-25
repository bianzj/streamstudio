// ==========================================================================
// 文件：descriptor.cpp（raytracing）
// 功能：实现 RayTracing 模式的描述符集：声明 11 个绑定、创建布局/池/集，并把 Buffer 创建的缓冲与 TLAS 写入。
// 主要模块：
//   1. Descriptor::createDescriptor：声明绑定 → 创建布局/池/描述符集 → 写入缓冲与 TLAS
//   2. Descriptor::destroy：销毁池与布局并重置句柄
// 数据流：Buffer 创建的缓冲句柄（raytracingio / meshio / instanceio / virtualio）与加速结构 TLAS
//   → 写入描述符集 m_descSet → pipeline 创建时使用 m_descSetLayout → command.cpp 绑定并追踪射线。
// 注意：绑定的可见着色器阶段不同，纹理偏移/因子只在 raygen 阶段可见。
// ==========================================================================

//
// Created by admin on 2024/1/25.
//

#include "descriptor.h"
#include <stdexcept>


// 作用：创建描述符集。步骤：1 声明绑定；2 创建布局、池与描述符集；3 为各绑定构造写入信息；
// 4 一次性 vkUpdateDescriptorSets。要求各缓冲和 TLAS 已创建（光谱、波长集等被无条件解引用）。
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

    
    // 步骤 1：声明 11 个绑定（编号与着色器一致）。存储缓冲：光谱/热材料、模型/实例链接、波长集、输出图像、纹理偏移与因子；
    // uniform：传感器、光源；加速结构：TLAS。
    // 纹理偏移与因子只对 raygen 阶段可见，其余对 fragment / compute / raygen 可见。
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

    // 步骤 2：创建描述符布局、容量为 1 个集的池，并分配描述符集。
    m_descSetLayout = bindings.createLayout(m_device);
    m_descPool = bindings.createPool(m_device, 1);
    m_descSet = nvvk::allocateDescriptorSet(m_device, m_descPool, m_descSetLayout);

    // 步骤 3：为每个绑定构造写入（缓冲取 VK_WHOLE_SIZE；TLAS 从加速结构对象获取）。
    // 光谱、波长集、实例链接缓冲在 createBuffer 中是有条件创建的（对应数组为空时不创建），
    // 而着色器总会读取这些绑定，因此缺失时直接抛出明确错误，避免解引用空指针。
    if (!meshio->m_pBufferSpectral)
        throw std::runtime_error("RayTracing descriptor: the spectral material buffer was not created (no spectral materials)");
    if (!raytracingio->m_pBufferWave)
        throw std::runtime_error("RayTracing descriptor: the wavelength buffer was not created (no wavelengths)");
    if (!instanceio->m_pBufferInstanceLink)
        throw std::runtime_error("RayTracing descriptor: the instance link buffer was not created (no instances)");
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

    // 步骤 4：一次性提交全部写入。
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(updates.size()), updates.data(), 0, nullptr);
    return true;
}

// 作用：销毁描述符池与布局（描述符集随池释放），并把三个句柄重置为空。
void Descriptor::destroy(std::shared_ptr<RaytracingIO> &raytracingio) {

    vkDestroyDescriptorPool(raytracingio->m_device, raytracingio->m_descPool, nullptr);
    vkDestroyDescriptorSetLayout(raytracingio->m_device, raytracingio->m_descSetLayout, nullptr);


    raytracingio->m_descPool = VkDescriptorPool();
    raytracingio->m_descSetLayout = VkDescriptorSetLayout();
    raytracingio->m_descSet = VkDescriptorSet();


}