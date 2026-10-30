//
// Created by admin on 2024/1/28.
//

#include "buffer.h"
#include "base/outputsize.h"



bool Buffer::createBuffer(std::shared_ptr<VoxelrtIO> &voxellstio){

    VkDevice & m_device = voxellstio->m_device;
    nvvk::Queue &m_queue  = voxellstio->m_queues[eGCT];
    auto & meshio = voxellstio->m_meshio;
    auto & instanceio = voxellstio->m_instanceio;
    auto & virtualio = voxellstio->m_virtualio;
    auto & m_pAlloc = voxellstio->m_pAlloc;
    auto & voxelio = voxellstio->m_voxelio;
    auto & surfio = voxellstio->m_surfio;
    auto n_voxel = voxellstio->n_voxel;
    auto & m_pAccelStruct = voxellstio->m_pAccelStruct;
    auto & defined = voxellstio->m_defined;

    nvvk::CommandPool cmdGen(m_device, m_queue.familyIndex);

    // spectral
    // FIXED[高] Bug: spectral/canopy/wave/instanceLink/voxelLink/nano 只在非空时创建，但 Descriptor::createDescriptor 与 Buffer::destroy 无条件解引用这些 shared_ptr（如 meshio->m_pBufferSpectral->buffer），任一为空即空指针崩溃；已修复：spectral/canopy/wave/instanceLink/voxelLink/hex/nano 为空时仍创建 1 元素全零占位缓冲，createDescriptor 与 destroy 的解引用不会再命中空 shared_ptr；thermal 绑定缺失一项本轮未改（见下方 DEFERRED）。
    if (meshio->spectrals.size() > 0)
    {
        VkCommandBuffer cmdBufSpectral = cmdGen.createCommandBuffer();
        VkBufferUsageFlags usage_ = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        meshio->m_pBufferSpectral = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufSpectral, meshio->spectrals, usage_));
        cmdGen.submitAndWait(cmdBufSpectral);
    }
    else
    {
        // 描述符写入与析构都无条件解引用该缓冲：空数据时上传一个零元素占位。
        VkCommandBuffer cmdBufSpectral = cmdGen.createCommandBuffer();
        std::vector<Spectral> spectralPlaceholder(1);
        meshio->m_pBufferSpectral = std::make_shared<nvvk::Buffer>(
            m_pAlloc->createBuffer(cmdBufSpectral, spectralPlaceholder, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        cmdGen.submitAndWait(cmdBufSpectral);
    }


    // fixedSpectral
//    if (meshio->fixedSpectrals.size() > 0)
//    {
//        VkCommandBuffer cmdBufSpectral = cmdGen.createCommandBuffer();
//        VkBufferUsageFlags usage_ = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
//        meshio->m_pFixedSpectralBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufSpectral, meshio->fixedSpectrals, usage_));
//        cmdGen.submitAndWait(cmdBufSpectral);
//    }

    // DEFERRED[中] Bug: thermal 为空时既不创建缓冲、createDescriptor 也不写该绑定，着色器静态使用该 binding 属未定义行为；但补占位会改变 voxelrt.cpp 中 thermalId < thermals.size() 的回退语义（空 thermal 从"无效索引"变为有效索引 0），需先确认物理语义，本轮不改。
    if (meshio->thermals.size() > 0)
    {
        VkCommandBuffer cmdBufSpectral = cmdGen.createCommandBuffer();
        VkBufferUsageFlags usage_ = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        meshio->m_pBufferThermal = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufSpectral, meshio->thermals, usage_));
        cmdGen.submitAndWait(cmdBufSpectral);
    }




    // canopy
    if (meshio->canopies.size() > 0)
    {
        VkCommandBuffer cmdBufCanopy = cmdGen.createCommandBuffer();
        meshio->m_pBufferCanopy = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufCanopy, meshio->canopies, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        cmdGen.submitAndWait(cmdBufCanopy);
    }
    else
    {
        VkCommandBuffer cmdBufCanopy = cmdGen.createCommandBuffer();
        std::vector<Canopy> canopyPlaceholder(1);
        meshio->m_pBufferCanopy = std::make_shared<nvvk::Buffer>(
            m_pAlloc->createBuffer(cmdBufCanopy, canopyPlaceholder, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        cmdGen.submitAndWait(cmdBufCanopy);
    }

    // Water BRDF parameters are required by VoxelRT even in scenes without a
    // water object because the descriptor layout is shared by every pass.
    if (meshio->watersets.empty()) meshio->watersets.emplace_back();
    VkCommandBuffer cmdBufWaterSet = cmdGen.createCommandBuffer();
    meshio->m_pWaterSetBuffer = std::make_shared<nvvk::Buffer>(
        m_pAlloc->createBuffer(cmdBufWaterSet, meshio->watersets,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    cmdGen.submitAndWait(cmdBufWaterSet);

    // tempe
    VkCommandBuffer cmdBufTempe = cmdGen.createCommandBuffer();
    std::vector<VoxelTempe> voxelTempes(n_voxel, VoxelTempe{305, 295});
    voxelio->m_pTempeBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufTempe, voxelTempes,
                                                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufTempe);


    ///--------------------------------------------------------------------
    ///  Scene properties
    ///--------------------------------------------------------------------
    // obj models buffer
    for (int kmodel = 0; kmodel < meshio->primMeshes.size(); kmodel++)
    {
        // DEFERRED[低] 性能: 每个网格都新建一个 nvvk::CommandPool 并 submitAndWait，网格数上千时创建/销毁池与同步开销显著；建议：复用外层 cmdGen，把所有网格上传录制进一个命令缓冲后统一提交。
        nvvk::CommandPool cmdBufGet(m_device, m_queue.familyIndex, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, m_queue.queue);
        VkCommandBuffer cmdBufModel = cmdBufGet.createCommandBuffer();
        MeshBuffer model;
        model.nbVertices = static_cast<uint32_t>(meshio->primMeshes[kmodel].nVertices);
        model.nbIndices = static_cast<uint32_t>(meshio->primMeshes[kmodel].nIndices);

        VkBufferUsageFlags usage_ = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        model.vertexBuffer =
                m_pAlloc->createBuffer(cmdBufModel, meshio->primMeshes[kmodel].vertices, usage_);

        usage_ = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        model.indexBuffer =
                m_pAlloc->createBuffer(cmdBufModel, meshio->primMeshes[kmodel].indices, usage_);
        cmdBufGet.submitAndWait(cmdBufModel);
        m_pAlloc->finalizeAndReleaseStaging();
        meshio->m_bufferMeshes.emplace_back(model);
    }

    // modelLink
    VkCommandBuffer cmdBufModelLink = cmdGen.createCommandBuffer();
    for (int kmodel = 0; kmodel < meshio->primMeshes.size(); kmodel++)
    {
        meshio->meshLinks[kmodel].vertexAddress = nvvk::getBufferDeviceAddress(m_device, meshio->m_bufferMeshes[kmodel].vertexBuffer.buffer);
        meshio->meshLinks[kmodel].indexAddress = nvvk::getBufferDeviceAddress(m_device, meshio->m_bufferMeshes[kmodel].indexBuffer.buffer);
    }
    meshio->m_pBufferMeshLink = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufModelLink, meshio->meshLinks, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    cmdGen.submitAndWait(cmdBufModelLink);


    // instance link
    if (instanceio->instanceLinks.size() > 0)
    {
        VkCommandBuffer cmdBufInstanceLink = cmdGen.createCommandBuffer();
        VkBufferUsageFlags usage_ = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        instanceio->m_pBufferInstanceLink = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufInstanceLink, instanceio->instanceLinks, usage_));
        cmdGen.submitAndWait(cmdBufInstanceLink);
    }
    else
    {
        VkCommandBuffer cmdBufInstanceLink = cmdGen.createCommandBuffer();
        VkBufferUsageFlags usage_ = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        std::vector<InstanceLink> instanceLinkPlaceholder(1);
        instanceio->m_pBufferInstanceLink = std::make_shared<nvvk::Buffer>(
            m_pAlloc->createBuffer(cmdBufInstanceLink, instanceLinkPlaceholder, usage_));
        cmdGen.submitAndWait(cmdBufInstanceLink);
    }

    // voxelLink and nano
    if (voxelio->nanoHandle.size() > 0)
    {
        // voxel link; ps: this is not voxel intance link, but the voxel link
        VkCommandBuffer cmdBufVoxelLink = cmdGen.createCommandBuffer();
        VkBufferUsageFlags usage_ = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        voxelio->m_pVoxelLinkBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufVoxelLink, voxelio->voxellinks, usage_));
        cmdGen.submitAndWait(cmdBufVoxelLink);

        if (voxelio->voxelHexs.empty()) voxelio->voxelHexs.emplace_back();
        VkCommandBuffer cmdBufVoxelHex = cmdGen.createCommandBuffer();
        voxelio->m_pVoxelHexBuffer = std::make_shared<nvvk::Buffer>(
            m_pAlloc->createBuffer(cmdBufVoxelHex, voxelio->voxelHexs, usage_));
        cmdGen.submitAndWait(cmdBufVoxelHex);

        // voxel Nano
        VkCommandBuffer cmdBufVoxelNano = cmdGen.createCommandBuffer();
        voxelio->m_pVoxelNanoBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufVoxelNano, voxelio->nanoHandle.size(), voxelio->nanoHandle.data(),
                                                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
        cmdGen.submitAndWait(cmdBufVoxelNano);
    }
    else
    {
        // 没有 nano 网格时 descriptor 仍会写 voxelLink/hex/nano 三个绑定：各上传一个零元素占位。
        VkCommandBuffer cmdBufVoxelLink = cmdGen.createCommandBuffer();
        std::vector<VoxelLink> voxelLinkPlaceholder(1);
        voxelio->m_pVoxelLinkBuffer = std::make_shared<nvvk::Buffer>(
            m_pAlloc->createBuffer(cmdBufVoxelLink, voxelLinkPlaceholder, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        cmdGen.submitAndWait(cmdBufVoxelLink);

        VkCommandBuffer cmdBufVoxelHex = cmdGen.createCommandBuffer();
        std::vector<VoxelHex> voxelHexPlaceholder(1);
        voxelio->m_pVoxelHexBuffer = std::make_shared<nvvk::Buffer>(
            m_pAlloc->createBuffer(cmdBufVoxelHex, voxelHexPlaceholder, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        cmdGen.submitAndWait(cmdBufVoxelHex);

        VkCommandBuffer cmdBufVoxelNano = cmdGen.createCommandBuffer();
        std::vector<uint32_t> nanoPlaceholder(1, 0U);
        voxelio->m_pVoxelNanoBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufVoxelNano, nanoPlaceholder.size(), nanoPlaceholder.data(),
                                                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
        cmdGen.submitAndWait(cmdBufVoxelNano);
    }


    ///--------------------------------------------------------------------
    ///  Accelerate properties
    ///--------------------------------------------------------------------
    m_pAccelStruct->createAccelStruct(m_device,meshio,instanceio);

    ///--------------------------------------------------------------------
    ///  geometry properties
    ///--------------------------------------------------------------------
    VkCommandBuffer cmdBufSensor = cmdGen.createCommandBuffer();
    voxellstio->m_pBufferSensor = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufSensor, sizeof(SensorMatrix), &voxellstio->sensor,
                                                                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    cmdGen.submitAndWait(cmdBufSensor);


    VkCommandBuffer cmdBufLight = cmdGen.createCommandBuffer();
    voxellstio->m_pBufferLight = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufLight, sizeof(LightSet), &voxellstio->light,
                                                                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    cmdGen.submitAndWait(cmdBufLight);

    // sensor wave
    if (voxellstio->waves.size() > 0)
    {
        VkCommandBuffer cmdBufWave = cmdGen.createCommandBuffer();
        voxellstio->m_pBufferWave = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufWave, voxellstio->waves, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        cmdGen.submitAndWait(cmdBufWave);
    }
    else
    {
        VkCommandBuffer cmdBufWave = cmdGen.createCommandBuffer();
        std::vector<float> wavePlaceholder(1, 0.0f);
        voxellstio->m_pBufferWave = std::make_shared<nvvk::Buffer>(
            m_pAlloc->createBuffer(cmdBufWave, wavePlaceholder, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        cmdGen.submitAndWait(cmdBufWave);
    }

    // meteo and aero

    // dir
    VkCommandBuffer cmdBufDir = cmdGen.createCommandBuffer();
    std::vector<VoxelDir> voxelDirs(n_voxel, VoxelDir{0, 0});
    voxelio->m_pDirBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufDir, voxelDirs,
                                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufDir);

    // rads
    VkCommandBuffer cmdBufRads = cmdGen.createCommandBuffer();
    // Keep the directional integration scratch space separate from the
    // persistent per-band results. Otherwise every new band overwrites the
    // results of the preceding bands (most visibly band 0). Per band: the
    // accumulated environment, then a ping-pong pair holding the previous and
    // current collision order of the iterative diffuse passes.
    const size_t radiationStride = static_cast<size_t>(DIFFUSENUM) +
                                   2U * static_cast<size_t>(voxellstio->n_wave);
    std::vector<VoxelRad> voxelRads(static_cast<size_t>(n_voxel) * radiationStride, VoxelRad{0, 0});
    // DEFERRED[高] GPU同步: 该缓冲大小为 n_voxel×(64+2×n_wave)×8 字节，城市/高光谱场景很容易超过 4 GiB，而 VoxelRT 仍以普通 SSBO 描述符(VK_WHOLE_SIZE)绑定，超过 maxStorageBufferRange 时着色器越界访问/结果截断（VoxelEB 为此已改用 buffer_reference）；另外还要在主机先分配同等大小的全零 vector 再上传；建议：检查 maxStorageBufferRange 后改用设备地址访问，并用 vkCmdFillBuffer 清零而非主机上传。
    voxelio->m_pRadsBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufRads, voxelRads,
                                                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufRads);

    // netRad
    VkCommandBuffer cmdBufNetRad = cmdGen.createCommandBuffer();
    std::vector<VoxelNetRad> voxelNetRads(n_voxel, VoxelNetRad{0, 0, 0, 0});
    voxelio->m_pNetRadBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufNetRad, voxelNetRads,
                                                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufNetRad);

    ///--------------------------------------------------------------------
    ///  Virtual Sceen properties
    ///--------------------------------------------------------------------
    /// storage
    VkCommandBuffer cmdBufStorage = cmdGen.createCommandBuffer();
    const size_t outputSize = checkedOutputImageElements(voxellstio->m_physicalDevice, voxellstio->imageSize.x, voxellstio->imageSize.y, voxellstio->n_wave);
    std::vector<float> outputImage(outputSize, 0.0);
    virtualio->m_pBufferStorage = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufStorage, outputImage,
                                                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufStorage);


    VkCommandBuffer cmdBufLad = cmdGen.createCommandBuffer();
    surfio->m_pBufferLad = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufLad, surfio->lads, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufLad);


    m_pAlloc->finalizeAndReleaseStaging();

    // DEFERRED[低] 可维护性: 成功路径返回 false，与函数名语义相反，调用方若据此判断会误判失败；建议：成功返回 true，失败返回 false 或抛异常。
    return false;

}



void Buffer::destroy(std::shared_ptr<VoxelrtIO> &voxellstio){

    VkDevice & m_device = voxellstio->m_device;
    nvvk::Queue &m_queue  = voxellstio->m_queues[eGCT];
    auto & meshio = voxellstio->m_meshio;
    auto & instanceio = voxellstio->m_instanceio;
    auto & virtualio = voxellstio->m_virtualio;
    auto & m_pAlloc = voxellstio->m_pAlloc;
    auto & voxelio = voxellstio->m_voxelio;
    auto n_voxel = voxellstio->n_voxel;
    auto & m_pAccelStruct = voxellstio->m_pAccelStruct;
    auto & defined = voxellstio->m_defined;
    auto & surfio = voxellstio->m_surfio;


    // 占位缓冲在 spectral/canopy 为空时也会创建，按指针判空销毁（原先按 size 判断会漏掉占位缓冲）。
    if (meshio->m_pBufferSpectral) m_pAlloc->destroy(*(meshio->m_pBufferSpectral));

   // m_pAlloc->destroy(*(meshio->m_pFixedSpectralBuffer));
//    m_pAlloc->destroy(*(voxelio->m_pSurfLBuffer));


    if (meshio->thermals.size() > 0) // no utilise the isTemperature
    {
        // FIXED[中] 空指针: m_pBufferThermal 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
        if (meshio->m_pBufferThermal) m_pAlloc->destroy(*(meshio->m_pBufferThermal));
    }
    // FIXED[中] 空指针: destroy 无条件解引用 voxelio->m_pTempeBuffer，Buffer::create 未执行或中途失败（createBuffer 抛异常等）时为空即崩溃（静态复核发现，原文件未标注）；已修复：按同函数内 spectral/canopy 的写法先判 shared_ptr 非空再销毁。
    if (voxelio->m_pTempeBuffer) m_pAlloc->destroy(*(voxelio->m_pTempeBuffer));

    // 占位缓冲在 canopies 为空时也会创建，按指针判空销毁（原先按 size 判断会漏掉占位缓冲）。
    if (meshio->m_pBufferCanopy) m_pAlloc->destroy(*(meshio->m_pBufferCanopy));
    if (meshio->m_pWaterSetBuffer) m_pAlloc->destroy(*(meshio->m_pWaterSetBuffer));
    // FIXED[中] 空指针: m_pBufferMeshLink 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
    if (meshio->m_pBufferMeshLink) m_pAlloc->destroy(*(meshio->m_pBufferMeshLink));

    // FIXED[中] 空指针: instanceLink/voxelLink/voxelNano 三个缓冲在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：逐个先判 shared_ptr 非空再销毁，销毁顺序与资源依赖关系保持不变。
    if (instanceio->m_pBufferInstanceLink) m_pAlloc->destroy(*(instanceio->m_pBufferInstanceLink));
    if (voxelio->m_pVoxelLinkBuffer) m_pAlloc->destroy(*(voxelio->m_pVoxelLinkBuffer));
    if (voxelio->m_pVoxelNanoBuffer) m_pAlloc->destroy(*(voxelio->m_pVoxelNanoBuffer));
    if (voxelio->m_pVoxelHexBuffer) m_pAlloc->destroy(*(voxelio->m_pVoxelHexBuffer));

    // FIXED[中] 空指针: m_pBufferLad 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
    if (surfio->m_pBufferLad) m_pAlloc->destroy(*(surfio->m_pBufferLad));

    // FIXED[中] 空指针: m_pBufferSensor 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
    if (voxellstio->m_pBufferSensor) m_pAlloc->destroy(*(voxellstio->m_pBufferSensor));
    // FIXED[中] 空指针: destroy 无条件解引用 voxellstio->m_pBufferWave，create 未执行或中途失败时为空即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
    if (voxellstio->m_pBufferWave) m_pAlloc->destroy(*(voxellstio->m_pBufferWave));
   // m_pAlloc->destroy(*(voxellstio->m_pBufferWaveset));
    // FIXED[中] 空指针: m_pBufferLight 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
    if (voxellstio->m_pBufferLight) m_pAlloc->destroy(*(voxellstio->m_pBufferLight));

    for (int i = 0; i < meshio->m_bufferMeshes.size(); i++)
    {
        m_pAlloc->destroy((meshio->m_bufferMeshes[i].vertexBuffer));
        m_pAlloc->destroy((meshio->m_bufferMeshes[i].indexBuffer));
    }


    // FIXED[中] 空指针: m_pDirBuffer 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
    if (voxelio->m_pDirBuffer) m_pAlloc->destroy(*(voxelio->m_pDirBuffer));
    // FIXED[中] 空指针: m_pRadsBuffer 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
    if (voxelio->m_pRadsBuffer) m_pAlloc->destroy(*(voxelio->m_pRadsBuffer));
    // FIXED[中] 空指针: m_pNetRadBuffer 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
    if (voxelio->m_pNetRadBuffer) m_pAlloc->destroy(*(voxelio->m_pNetRadBuffer));
    //m_pAlloc->destroy(*(voxelio->m_pPnetBuffer));
    // FIXED[中] 空指针: m_pBufferStorage 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
    if (virtualio->m_pBufferStorage) m_pAlloc->destroy(*(virtualio->m_pBufferStorage));



    //m_pAlloc->destroy(*(voxelio->m_pRaaBuffer));
    //m_pAlloc->destroy(*(meshio->m_pLeafBioBuffer));
    //m_pAlloc->destroy(*(meshio->m_pSoilSetBuffer));

    //m_pAlloc->destroy(*(voxelio->m_pRssBuffer));
    //m_pAlloc->destroy(*(voxelio->m_pAirBuffer));

    //m_pAlloc->destroy(*(voxelio->m_pFluxBuffer));
    //m_pAlloc->destroy(*(voxelio->m_pTLASTBuffer));
    //m_pAlloc->destroy(*(voxelio->m_pStateBuffer));

    //m_rtBuilder.destroy();
}


