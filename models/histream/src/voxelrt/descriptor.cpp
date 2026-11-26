// ==========================================================================
// 文件：descriptor.cpp
// 功能：VoxelRT 模式描述符集的创建与销毁
// 主要模块/步骤：
//   1. 声明 18 个 binding（顺序见 descriptor.h 的 VoxelrtbindingInd）：存储缓冲 + TLAS +
//      2 个 UNIFORM 缓冲（sensor、light）
//   2. 创建布局、描述符池（容量 1 个集）并分配描述符集
//   3. 把 buffer.cpp 创建的缓冲及加速结构 TLAS 写入描述符集
//   4. destroy：销毁池与布局并清空句柄
// 数据流：VoxelrtIO 中的缓冲/TLAS → vkUpdateDescriptorSets → VoxelRT 计算着色器
// 与其他文件的关系：声明见 descriptor.h；缓冲创建见 buffer.cpp；管线布局使用本布局（待核对）
// 注意：thermal 绑定仅在 meshio->thermals 非空时写入（与 buffer.cpp 的条件一致）
// ==========================================================================

//
// Created by admin on 2024/1/25.
//

#include "descriptor.h"


// 模块：创建 VoxelRT 描述符集。
// 输入：modelio 中已创建的各缓冲、TLAS；输出：m_descSetLayout、m_descPool、m_descSet。
// 说明：所有 binding 共用同一组着色器阶段标志（片元/计算/光线生成/相交）；
//   所有缓冲用 VK_WHOLE_SIZE 整体绑定。
bool Descriptor::createDescriptor(std::shared_ptr<VoxelrtIO> &modelio){

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


    // 步骤 1：声明 binding；加速结构 tlas 另加最近命中阶段。
    VkShaderStageFlags flags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
    bindings.addBinding(VoxelrtbindingInd::spectral, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::thermal, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::canopy, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::meshLink, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::instanceLink, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::voxelLink, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,flags);
    bindings.addBinding(VoxelrtbindingInd::nano, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::tlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,flags | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
    bindings.addBinding(VoxelrtbindingInd::sensor, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::wave, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::light, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::dir, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::rads, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::netRad, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::storage, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags );
    bindings.addBinding(VoxelrtbindingInd::lad, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::hex, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);
    bindings.addBinding(VoxelrtbindingInd::waterSet, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);


    // 步骤 2：创建布局、池与描述符集（池容量为 1 个集）。
    m_descSetLayout = bindings.createLayout(m_device);
    m_descSetPool = bindings.createPool(m_device, 1);
    m_descSet = nvvk::allocateDescriptorSet(m_device, m_descSetPool, m_descSetLayout);

    // 步骤 3：逐 binding 写入缓冲信息，最后一次性 vkUpdateDescriptorSets。
    //   thermal 为空时不写（着色器若静态使用该 binding 属风险，见 buffer.cpp 的 DEFERRED 说明）。
    std::vector<VkWriteDescriptorSet> updates;

    // component    

    VkDescriptorBufferInfo dbiSpectral{meshio->m_pBufferSpectral->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::spectral, &dbiSpectral));


    VkDescriptorBufferInfo dbiThermal{};
    if (meshio->m_pBufferThermal)
    {
        dbiThermal = {meshio->m_pBufferThermal->buffer, 0, VK_WHOLE_SIZE};
        updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::thermal, &dbiThermal));
    }


    VkDescriptorBufferInfo dbiCanopy{meshio->m_pBufferCanopy->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::canopy, &dbiCanopy));

    // link

    VkDescriptorBufferInfo dbiMeshLink{meshio->m_pBufferMeshLink->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::meshLink, &dbiMeshLink));

    VkDescriptorBufferInfo dbiInstanceLink{instanceio->m_pBufferInstanceLink->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::instanceLink, &dbiInstanceLink));

    VkDescriptorBufferInfo dbiVoxelLink{voxelio->m_pVoxelLinkBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::voxelLink, &dbiVoxelLink));

    VkDescriptorBufferInfo dbiNano{voxelio->m_pVoxelNanoBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::nano, &dbiNano));

    // Accelerate structure
    VkAccelerationStructureKHR tlas = modelio->m_pAccelStruct->getTlas();
    VkWriteDescriptorSetAccelerationStructureKHR descASInfo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    descASInfo.accelerationStructureCount = 1;
    descASInfo.pAccelerationStructures = &tlas;
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::tlas, &descASInfo));

    // sensor and light
    VkDescriptorBufferInfo dbiSensor{modelio->m_pBufferSensor->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::sensor, &dbiSensor));
    VkDescriptorBufferInfo dbiWave{modelio->m_pBufferWave->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::wave, &dbiWave));
    VkDescriptorBufferInfo dbiLight{modelio->m_pBufferLight->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::light, &dbiLight));



    VkDescriptorBufferInfo dbiDir{voxelio->m_pDirBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::dir, &dbiDir));
    VkDescriptorBufferInfo dbiRads{voxelio->m_pRadsBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::rads, &dbiRads));





    VkDescriptorBufferInfo dbiNetRad{voxelio->m_pNetRadBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::netRad, &dbiNetRad));

    VkDescriptorBufferInfo dbiStore{virtualio->m_pBufferStorage->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::storage, &dbiStore));

    VkDescriptorBufferInfo dbiLad{surfio->m_pBufferLad->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::lad, &dbiLad));
    VkDescriptorBufferInfo dbiHex{voxelio->m_pVoxelHexBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::hex, &dbiHex));
    VkDescriptorBufferInfo dbiWaterSet{meshio->m_pWaterSetBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelrtbindingInd::waterSet, &dbiWaterSet));

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(updates.size()), updates.data(), 0, nullptr);
    return true;
}

// 模块：销毁描述符池（描述符集随池释放）与布局，并把句柄置空。
void Descriptor::destroy(std::shared_ptr<VoxelrtIO> &modelio) {

    vkDestroyDescriptorPool(modelio->m_device, modelio->m_descPool, nullptr);
    vkDestroyDescriptorSetLayout(modelio->m_device, modelio->m_descSetLayout, nullptr);


    modelio->m_descPool = VkDescriptorPool();
    modelio->m_descSetLayout = VkDescriptorSetLayout();
    modelio->m_descSet = VkDescriptorSet();

}









