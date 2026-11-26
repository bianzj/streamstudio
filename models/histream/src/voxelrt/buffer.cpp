// ==========================================================================
// 文件：buffer.cpp
// 功能：VoxelRT 模式（体元辐射）的 GPU 缓冲区创建与销毁
// 主要模块/步骤：
//   1. createBuffer：逐类上传缓冲（均同步 submitAndWait），数据为空时上传 1 元素占位
//      光谱/热/冠层/水体参数 → 网格顶点与索引 → mesh/instance/voxel 链接表与 NanoVDB
//      → 加速结构 → 传感器/光源/波长 → 体元温度/方向/辐射/净辐射 → 输出影像与 LAD
//   2. destroy：按指针判空依次释放缓冲与网格顶点/索引缓冲
// 数据流：VoxelrtIO 中 meshio/instanceio/voxelio/surfio/virtualio 的 CPU 数据 → nvvk::Buffer
//   → descriptor.cpp 写入描述符集 → 计算着色器
// 与其他文件的关系：声明见 buffer.h；binding 序号见 descriptor.h；输出缓冲大小检查见
//   base/outputsize.h；VoxelRTSetting 与 VoxelRad 等结构见 base/structs*.h
// 约定：占位缓冲只为保证描述符与析构的解引用有效，内容为零值；温度单位 K（待核对）
// ==========================================================================

//
// Created by admin on 2024/1/28.
//

#include "buffer.h"
#include "base/outputsize.h"
#include "base/voxellimits.h"
#include "base/proxymesh.h"
#include "base/voxelflags.h"
#include <cstdlib>
#include <iostream>



// 模块：创建 VoxelRT 全部 GPU 缓冲。
// 输入：voxellstio 中的场景/体元/传感器数据；输出：各子结构中的 nvvk::Buffer 智能指针。
// 说明：每个缓冲都用 cmdGen 录制上传并 submitAndWait，简单但串行；
//   数据为空时（spectral/canopy/instanceLink/voxel 链接表/waves）上传占位，保证绑定有效。
// 返回值：成功路径当前返回 false（见函数末尾说明）。
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

    // Scene-size limits (explicit errors; before this check an oversize scene silently truncated or read out of bounds).
    if (n_voxel > 0) {
        const uint64_t voxels = static_cast<uint64_t>(n_voxel);
        voxellimits::requireStorageRange(voxellstio->m_physicalDevice, "VoxelRT", "voxel link", voxels, sizeof(VoxelLink));
        voxellimits::requireStorageRange(voxellstio->m_physicalDevice, "VoxelRT", "voxel direction", voxels, sizeof(VoxelDir));
        voxellimits::requireStorageRange(voxellstio->m_physicalDevice, "VoxelRT", "voxel net radiation", voxels, sizeof(VoxelNetRad));
        voxellimits::requireStorageRange(voxellstio->m_physicalDevice, "VoxelRT", "voxel temperature", voxels, sizeof(VoxelTempe));
        voxellimits::requireDispatch(voxellstio->m_physicalDevice, "VoxelRT", voxels, GROUP_SIZEX);
    }
    // 步骤 1：光谱参数缓冲（meshio->spectrals）；为空时上传 1 个零元素占位。
    // spectral
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

    // 步骤 2：热参数缓冲（meshio->thermals）。即使工程没有热属性也必须创建
    // 一个 300 K 占位元素：着色器静态声明并读取 thermalMaterials 绑定，空描述符
    // 是未定义行为。mappedId 未命中时返回 0，正好落在占位元素上。
    const std::vector<Thermal> thermalFallback{ Thermal{ 300.0f, 300.0f } };
    const auto& thermals = meshio->thermals.empty() ? thermalFallback : meshio->thermals;
    {
        VkCommandBuffer cmdBufSpectral = cmdGen.createCommandBuffer();
        VkBufferUsageFlags usage_ = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        meshio->m_pBufferThermal = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufSpectral, thermals, usage_));
        cmdGen.submitAndWait(cmdBufSpectral);
    }




    // 步骤 3：冠层参数缓冲（meshio->canopies）；为空时上传占位。
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

    // 步骤 4：逐体元温度缓冲，初值向阳 305、背阴 295（单位 K，待核对）；
    //   带 TRANSFER_SRC，便于回读。
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
    // 步骤 5：逐网格上传顶点/索引缓冲；带 SHADER_DEVICE_ADDRESS 与加速结构构建输入标志，
    //   既供构建 BLAS，也供着色器经设备地址访问。
    // obj models buffer
    // Proxy-geometry vertex format (control.voxelProxyGeometry / HISTREAM_VOXEL_PROXY, src/base/proxymesh.h).
    const int proxyMode = voxelflags::proxyMode(voxellstio->voxelFlags);
    if (std::getenv("HISTREAM_VOXEL_PROXY_STATS"))
        std::cout << "PROXY_GEOMETRY\tmodel=VoxelRT\tmode=" << proxymesh::modeName(proxyMode) << std::endl;
    for (int kmodel = 0; kmodel < meshio->primMeshes.size(); kmodel++)
    {
        if (proxyMode != voxelflags::kProxyFull)
        {
            meshio->m_bufferMeshes.emplace_back(proxymesh::uploadPositionMesh(
                *m_pAlloc, voxellstio->m_physicalDevice, m_device, m_queue.familyIndex, m_queue.queue,
                "VoxelRT", static_cast<size_t>(kmodel), meshio->primMeshes[kmodel], proxyMode));
            continue;
        }
        // Legacy 44-byte vertices: nvvk uploads each buffer through a staging buffer of the same size, so both must fit
        // one allocation (an oversize mesh used to crash the driver without a message).
        proxymesh::requireFullMeshFits(voxellstio->m_physicalDevice, "VoxelRT", static_cast<size_t>(kmodel), meshio->primMeshes[kmodel]);
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

    // 步骤 6：把各网格顶点/索引缓冲的设备地址写入 meshLinks 并上传（着色器据此取几何）。
    // modelLink
    VkCommandBuffer cmdBufModelLink = cmdGen.createCommandBuffer();
    for (int kmodel = 0; kmodel < meshio->primMeshes.size(); kmodel++)
    {
        meshio->meshLinks[kmodel].vertexAddress = nvvk::getBufferDeviceAddress(m_device, meshio->m_bufferMeshes[kmodel].vertexBuffer.buffer);
        meshio->meshLinks[kmodel].indexAddress = meshio->m_bufferMeshes[kmodel].indexed
            ? nvvk::getBufferDeviceAddress(m_device, meshio->m_bufferMeshes[kmodel].indexBuffer.buffer) : 0;
    }
    meshio->m_pBufferMeshLink = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufModelLink, meshio->meshLinks, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    cmdGen.submitAndWait(cmdBufModelLink);


    // 步骤 7：实例链接表；为空时上传 1 元素占位。
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

    // 步骤 8：体元链接表、hex 表与 NanoVDB 网格数据；
    //   nanoHandle 为空时三者都上传占位（见 else 分支说明）。
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
    // 步骤 9：依据网格与实例构建加速结构（BLAS/TLAS）。
    m_pAccelStruct->m_physicalDevice = voxellstio->m_physicalDevice;  // explicit BLAS size checks (voxel modes only)
    m_pAccelStruct->m_modelName = "VoxelRT";
    m_pAccelStruct->createAccelStruct(m_device,meshio,instanceio);

    ///--------------------------------------------------------------------
    ///  geometry properties
    ///--------------------------------------------------------------------
    // 步骤 10：传感器矩阵与光源作为 UNIFORM 缓冲（设备本地 + HOST_COHERENT，
    //   便于后续更新，待核对），随后上传波长表（为空时占位）。
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

    // 步骤 11：逐体元 VoxelDir（solar、skyDiffuse）缓冲，初值全 0。
    // dir
    VkCommandBuffer cmdBufDir = cmdGen.createCommandBuffer();
    std::vector<VoxelDir> voxelDirs(n_voxel, VoxelDir{0, 0});
    voxelio->m_pDirBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufDir, voxelDirs,
                                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufDir);

    // 步骤 12：辐射场缓冲 VoxelRad，总条数 n_voxel × 2·n_wave，全零初始化（2026-11-26 起不再有 64 个方向暂存槽，
    //   每体元 8·2·n_wave 字节；布局见 voxelrt/voxelrad_diffuse_single.comp 的 voxelRadiationIndex）。
    // rads
    VkCommandBuffer cmdBufRads = cmdGen.createCommandBuffer();
    // Per band: the accumulated environment, then a ping-pong pair holding the previous and
    // current collision order of the iterative diffuse passes. The 64-direction integration
    // scratch that used to precede them now lives in thread-private shader memory.
    const size_t radiationStride = 2U * static_cast<size_t>(std::max(voxellstio->n_wave, 1));
    // The buffer is a plain storage buffer: its size must fit maxStorageBufferRange and its element
    // index the 32-bit shader arithmetic. Clear errors instead of silent truncation.
    voxellimits::requireStorageRange(voxellstio->m_physicalDevice, "VoxelRT", "radiance (voxelRads)",
        static_cast<uint64_t>(n_voxel), radiationStride * sizeof(VoxelRad));
    voxellimits::requireIndex32("VoxelRT", "radiance (voxelRads)", static_cast<uint64_t>(n_voxel), radiationStride);
    std::vector<VoxelRad> voxelRads(static_cast<size_t>(n_voxel) * radiationStride, VoxelRad{0, 0});
    voxelio->m_pRadsBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufRads, voxelRads,
                                                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufRads);

    // 步骤 13：逐体元 VoxelNetRad（VNIR/TIR 直射与散射辐亮度）缓冲，初值全 0。
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
    // 步骤 14：输出影像缓冲，元素数由 checkedOutputImageElements 按影像尺寸与波段数给出
    //   （含上限检查，细节见 base/outputsize.h），全零初始化；随后上传 LAD 数组（surfio->lads）。
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



// 模块：释放 createBuffer 创建的全部缓冲。
// 说明：除热参数与网格缓冲外均按指针判空；热参数仅在 thermals 非空时才被创建。
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
        if (meshio->m_pBufferThermal) m_pAlloc->destroy(*(meshio->m_pBufferThermal));
    }
    if (voxelio->m_pTempeBuffer) m_pAlloc->destroy(*(voxelio->m_pTempeBuffer));

    // 占位缓冲在 canopies 为空时也会创建，按指针判空销毁（原先按 size 判断会漏掉占位缓冲）。
    if (meshio->m_pBufferCanopy) m_pAlloc->destroy(*(meshio->m_pBufferCanopy));
    if (meshio->m_pWaterSetBuffer) m_pAlloc->destroy(*(meshio->m_pWaterSetBuffer));
    if (meshio->m_pBufferMeshLink) m_pAlloc->destroy(*(meshio->m_pBufferMeshLink));

    if (instanceio->m_pBufferInstanceLink) m_pAlloc->destroy(*(instanceio->m_pBufferInstanceLink));
    if (voxelio->m_pVoxelLinkBuffer) m_pAlloc->destroy(*(voxelio->m_pVoxelLinkBuffer));
    if (voxelio->m_pVoxelNanoBuffer) m_pAlloc->destroy(*(voxelio->m_pVoxelNanoBuffer));
    if (voxelio->m_pVoxelHexBuffer) m_pAlloc->destroy(*(voxelio->m_pVoxelHexBuffer));

    if (surfio->m_pBufferLad) m_pAlloc->destroy(*(surfio->m_pBufferLad));

    if (voxellstio->m_pBufferSensor) m_pAlloc->destroy(*(voxellstio->m_pBufferSensor));
    if (voxellstio->m_pBufferWave) m_pAlloc->destroy(*(voxellstio->m_pBufferWave));
   // m_pAlloc->destroy(*(voxellstio->m_pBufferWaveset));
    if (voxellstio->m_pBufferLight) m_pAlloc->destroy(*(voxellstio->m_pBufferLight));

    for (int i = 0; i < meshio->m_bufferMeshes.size(); i++)
    {
        m_pAlloc->destroy((meshio->m_bufferMeshes[i].vertexBuffer));
        if (meshio->m_bufferMeshes[i].indexBuffer.buffer) m_pAlloc->destroy((meshio->m_bufferMeshes[i].indexBuffer));
    }


    if (voxelio->m_pDirBuffer) m_pAlloc->destroy(*(voxelio->m_pDirBuffer));
    if (voxelio->m_pRadsBuffer) m_pAlloc->destroy(*(voxelio->m_pRadsBuffer));
    if (voxelio->m_pNetRadBuffer) m_pAlloc->destroy(*(voxelio->m_pNetRadBuffer));
    //m_pAlloc->destroy(*(voxelio->m_pPnetBuffer));
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


