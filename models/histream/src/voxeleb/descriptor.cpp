// ==========================================================================
// 文件：descriptor.cpp
// 功能：创建/销毁 VoxelEB 管线使用的 Vulkan 描述符集（绑定号 0..41 中除 rads=15 外共 41 个绑定）
// 主要步骤：
//   1. 声明描述符布局：按 VoxelebbindingInd 逐项 addBinding（存储缓冲 / UBO / 加速结构）
//   2. 创建布局、描述符池（仅 1 个集）并分配描述符集
//   3. 收集 VkWriteDescriptorSet：材质/冠层 → 链接与 NanoVDB → TLAS → 传感器/光源/大气条件
//      → 体元中间量 → 气象/气动/生化/土壤/水体 → 流体场，最后统一 vkUpdateDescriptorSets
//   4. destroy：销毁池与布局并复位句柄
// 数据流：输入为 VoxelebIO 中已建好的各 GPU 缓冲（meshio/instanceio/voxelio/surfio/virtualio
//         及 modelio 自身缓冲）；输出为 modelio->m_descSetLayout / m_descPool / m_descSet
// 与其他文件的关系：绑定号与 shader/bindingLayout.h（VOXELLST 段）一致；布局由 pipeline.cpp
//   用于创建管线布局，描述符集在 command.cpp 中绑定
// 注意：所有绑定共用同一组着色器阶段（片元/计算/raygen/相交，TLAS 另加 closest-hit）；
//   缓冲一律以 VK_WHOLE_SIZE 绑定（流体参数 UBO 例外，仅绑定 sizeof(FluidParameters)）
// ==========================================================================

//
// Created by admin on 2024/1/25.
//

#include "descriptor.h"
#include <stdexcept>
#include <string>


// 作用：创建 VoxelEB 的描述符布局、池与描述符集，并把所有缓冲写入描述符集。
// 输入：modelio，其中各缓冲（材质、体元、气象、流体等）与 TLAS 必须已创建；
// 输出：写入 modelio->m_descSetLayout / m_descPool / m_descSet；恒返回 true。
// 副作用：向 modelio->m_bindings 追加绑定（重复调用前是否需清空，待核对）。
bool Descriptor::createDescriptor(std::shared_ptr<VoxelebIO> &modelio){

    // 步骤 0：取出设备、绑定表及各子 IO 的引用（仅为书写简洁，不改变所有权）。
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


    // 步骤 1：声明描述符布局。所有绑定共享同一组着色器阶段，绑定号取自 VoxelebbindingInd，
    // 须与 shader/bindingLayout.h 一致。rads（体元辐射）不在此声明：着色器用 push constant
    // 里的 radianceAddress 物理地址访问，因此绑定号 15 在布局中是空缺。
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
    bindings.addBinding(VoxelebbindingInd::soilMemory, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags);


    // 步骤 2：创建布局、池（只容纳 1 个描述符集）并分配描述符集。
    m_descSetLayout = bindings.createLayout(m_device);
    m_descSetPool = bindings.createPool(m_device, 1);
    m_descSet = nvvk::allocateDescriptorSet(m_device, m_descSetPool, m_descSetLayout);

    // 步骤 3：为每个绑定准备 VkDescriptorBufferInfo 并收集写入操作，最后一次性更新。
    // 注意：dbiXxx 局部变量必须存活到 vkUpdateDescriptorSets 调用，makeWrite 只保存其指针。
    std::vector<VkWriteDescriptorSet> updates;

    // 条件创建的缓冲（表为空时 createBuffer 不创建）缺失时直接抛出明确错误，避免下面解引用空指针。
    {
        const auto requireBuffer = [](const std::shared_ptr<nvvk::Buffer>& buffer, const char* name) {
            if (!buffer)
                throw std::runtime_error(std::string("VoxelEB descriptor: the ") + name + " buffer was not created (its source table is empty)");
        };
        requireBuffer(meshio->m_pBufferSpectral, "spectral material");
        requireBuffer(meshio->m_pFixedSpectralBuffer, "fixed spectral material");
        requireBuffer(meshio->m_pBufferCanopy, "canopy");
        requireBuffer(instanceio->m_pBufferInstanceLink, "instance link");
        requireBuffer(voxelio->m_pVoxelLinkBuffer, "voxel link");
        requireBuffer(voxelio->m_pVoxelNanoBuffer, "NanoVDB voxel");
        requireBuffer(modelio->m_pBufferWave, "wavelength");
        requireBuffer(modelio->m_pBufferAtomcond, "atmospheric condition");
    }
    // component    

    // 3.1 材质与冠层：光谱材质、固定光谱材质、热学材质、体元温度、冠层
    VkDescriptorBufferInfo dbiSpectral{meshio->m_pBufferSpectral->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::spectral, &dbiSpectral));


    VkDescriptorBufferInfo dbiFixedSpectral{meshio->m_pFixedSpectralBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::fixedSpectral, &dbiFixedSpectral));

    // 热学材质缓冲仅在场景存在热学材质时才写入；否则该绑定不写描述符
    // （着色器在无热学材质时是否会访问它，待核对）。
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

    // 3.2 几何链接：网格链接、实例链接、体元链接，以及 NanoVDB 体元网格数据
    VkDescriptorBufferInfo dbiMeshLink{meshio->m_pBufferMeshLink->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::meshLink, &dbiMeshLink));

    VkDescriptorBufferInfo dbiInstanceLink{instanceio->m_pBufferInstanceLink->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::instanceLink, &dbiInstanceLink));

    VkDescriptorBufferInfo dbiVoxelLink{voxelio->m_pVoxelLinkBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::voxelLink, &dbiVoxelLink));

    VkDescriptorBufferInfo dbiNano{voxelio->m_pVoxelNanoBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::nano, &dbiNano));

    // Accelerate structure
    // 3.3 顶层加速结构 TLAS（加速结构描述符，不是缓冲）
    VkAccelerationStructureKHR tlas = modelio->m_pAccelStruct->getTlas();
    VkWriteDescriptorSetAccelerationStructureKHR descASInfo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    descASInfo.accelerationStructureCount = 1;
    descASInfo.pAccelerationStructures = &tlas;
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::tlas, &descASInfo));

    // sensor and light
    // 3.4 辐射输入：传感器（UBO）、波段表、光源（UBO）、大气条件 AtomCond
    VkDescriptorBufferInfo dbiSensor{modelio->m_pBufferSensor->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::sensor, &dbiSensor));
    VkDescriptorBufferInfo dbiWave{modelio->m_pBufferWave->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::wave, &dbiWave));
    VkDescriptorBufferInfo dbiLight{modelio->m_pBufferLight->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::light, &dbiLight));


    VkDescriptorBufferInfo dbiAtomcond{modelio->m_pBufferAtomcond->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::atomcond, &dbiAtomcond));

    // 3.5 体元中间量：方向、净辐射、Pnet（体元吸收/净辐射相关缓冲，含义待核对）、输出图像存储
    VkDescriptorBufferInfo dbiDir{voxelio->m_pDirBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::dir, &dbiDir));
    VkDescriptorBufferInfo dbiNetRad{voxelio->m_pNetRadBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::netRad, &dbiNetRad));
    VkDescriptorBufferInfo dbiPnet{voxelio->m_pPnetBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::pnet, &dbiPnet));

    VkDescriptorBufferInfo dbiStore{virtualio->m_pBufferStorage->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::storage, &dbiStore));

    // 3.6 气象（UBO）、空气动力学条件，以及逐体元阻抗 Raa、叶片生化参数、土壤参数、
    // 表面阻抗 Rss、空气状态 Air、热通量 Flux、上一时刻温度 Tlast、EBState、LAD、水体参数
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

    // 3.7 流体（LBM）场：速度/标量/分布函数为 A/B 双缓冲，另有密度、元数据；
    // 参数 UBO 只绑定 sizeof(FluidParameters)；最后是 VoxelHex 与土壤记忆缓冲。
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
    VkDescriptorBufferInfo dbiSoilMemory{voxelio->m_pSoilMemoryBuffer->buffer, 0, VK_WHOLE_SIZE};
    updates.emplace_back(bindings.makeWrite(m_descSet, VoxelebbindingInd::soilMemory, &dbiSoilMemory));

    // 步骤 4：一次性提交全部描述符写入。
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(updates.size()), updates.data(), 0, nullptr);
    return true;
}

// 作用：销毁描述符池（描述符集随之释放）与布局，并把句柄复位为空，避免重复销毁。
void Descriptor::destroy(std::shared_ptr<VoxelebIO> &modelio) {

    vkDestroyDescriptorPool(modelio->m_device, modelio->m_descPool, nullptr);
    vkDestroyDescriptorSetLayout(modelio->m_device, modelio->m_descSetLayout, nullptr);


    modelio->m_descPool = VkDescriptorPool();
    modelio->m_descSetLayout = VkDescriptorSetLayout();
    modelio->m_descSet = VkDescriptorSet();

}









