// ==========================================================================
// 文件：buffer.cpp
// 功能：VoxelEB（体元能量平衡）模式的 GPU 缓冲创建与销毁：把 VoxelebIO 里的 CPU 数据
//       （材质/热/冠层表、网格、体元链接、气象、状态初值、流体网格等）上传为 Vulkan 缓冲。
// 主要模块：
//   1. Buffer::createBuffer  按“表上传 → 温度初值 → 稠密流体网格 → 场景几何 → 加速结构 →
//                            观测/光源 → 辐射场 → 气动/气象 → 生物/空气 → 蒸散/土壤状态 →
//                            输出影像”的顺序创建全部缓冲（步骤编号见函数内注释）
//   2. Buffer::destroy       按创建的对应关系销毁缓冲
// 数据流：VoxelebIO（及 meshio、instanceio、voxelio、surfio、virtualio）→ 本文件 →
//         各 IO 对象上的 nvvk::Buffer 句柄 + 推送常量中的设备地址 → voxeleb 的各计算着色器。
// 关系：声明见 buffer.h；由 voxeleb.cpp 调用（createBuffer 在初始化，destroy 在收尾）；
//       数据结构定义在 base/structs_cg.h（须与 GLSL 对齐）；输出尺寸检查见 base/outputsize.h。
// 约定：温度缓冲内单位为 K（气温 Ta 为 °C，加 273.15 换算）；TLAST 土壤温度剖面单位为 °C；
//       空气采样高度 [m]、风速 [m s-1]、水汽压 [hPa]、CO2 [ppm]；
//       流体网格索引轴为 (x, y=垂直, z)，线性索引 x + nx·(z + nz·y)。
// ==========================================================================

//
// Created by admin on 2024/1/28.
//

#include "buffer.h"
#include "base/outputsize.h"
#include "base/voxellimits.h"
#include "base/voxelflags.h"
#include "base/proxymesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>



// 模块：createBuffer——VoxelEB 模式一次性创建全部 GPU 缓冲（参数 voxellstio 即 VoxelebIO）。
// 输入：VoxelebIO 里的材质/热/冠层表、体元链接、气象节点、流体与地形环境设置、传感器/光源等；
// 输出：写回各 IO 对象中的 m_p*Buffer（nvvk::Buffer 的 shared_ptr），并填写推送常量里的
//       设备地址（radianceAddress、scatteringStatsAddress）与 imageEnvironment 开关。
// 步骤：1 材质/热/冠层表；2 体元温度初值；3 稠密流体网格（风/温湿初值、稀疏→稠密映射、D3Q19、参数）；
//       4 场景网格、实例/体元链接、NanoVDB；5 加速结构；6 传感器/光源/波长/大气/方向；
//       7 辐射场与诊断缓冲、净辐射、Pnet；8 气动参数；9 气象与 Raa；10 生物参数与空气；
// 副作用：每个缓冲经临时命令缓冲上传并同步等待（submitAndWait），因此只应在初始化时调用。
bool Buffer::createBuffer(std::shared_ptr<VoxelebIO> &voxellstio){

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

    // 步骤 1：光谱、固定光谱、热参数与冠层表，非空时才上传为存储缓冲。
    nvvk::CommandPool cmdGen(m_device, m_queue.familyIndex);

    // Scene-size limits: every per-voxel table below is a descriptor-bound storage buffer (<= maxStorageBufferRange)
    // and the 1D dispatches need ceil(N/64) work groups. Explicit errors with the voxel count and the limit; before
    // this check an oversize scene was truncated or read out of bounds without any message. The radiance buffer
    // (voxelRads) is addressed by device address with 64-bit element indices (shader/voxel_radindex.glsl), so only
    // the VRAM of the machine limits it.
    if (n_voxel > 0) {
        const uint64_t voxels = static_cast<uint64_t>(n_voxel);
        const VkPhysicalDevice device = voxellstio->m_physicalDevice;
        voxellimits::requireStorageRange(device, "VoxelEB", "voxel link", voxels, sizeof(VoxelLink));
        voxellimits::requireStorageRange(device, "VoxelEB", "voxel direction", voxels, sizeof(VoxelDir));
        voxellimits::requireStorageRange(device, "VoxelEB", "voxel temperature", voxels, sizeof(VoxelTempe));
        voxellimits::requireStorageRange(device, "VoxelEB", "voxel net radiation", voxels, sizeof(VoxelNetRad));
        voxellimits::requireStorageRange(device, "VoxelEB", "voxel PAR", voxels, sizeof(VoxelPnet));
        voxellimits::requireStorageRange(device, "VoxelEB", "aerodynamic resistance", voxels, sizeof(VoxelRaa));
        voxellimits::requireStorageRange(device, "VoxelEB", "surface resistance", voxels, sizeof(VoxelRss));
        voxellimits::requireStorageRange(device, "VoxelEB", "leaf air state", voxels, sizeof(VoxelAir));
        voxellimits::requireStorageRange(device, "VoxelEB", "heat flux", voxels, sizeof(VoxelHeatflux));
        voxellimits::requireStorageRange(device, "VoxelEB", "temperature history (TLAST)", voxels,
                                         static_cast<uint64_t>(TLASTNUM) * sizeof(TLAST));
        voxellimits::requireDispatch(device, "VoxelEB", voxels, GROUP_SIZEX);
    }

    // spectral
    if (meshio->spectrals.size() > 0)
    {
        VkCommandBuffer cmdBufSpectral = cmdGen.createCommandBuffer();
        VkBufferUsageFlags usage_ = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        meshio->m_pBufferSpectral = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufSpectral, meshio->spectrals, usage_));
        cmdGen.submitAndWait(cmdBufSpectral);
    }


    // fixedSpectral
    if (meshio->fixedSpectrals.size() > 0)
    {
        VkCommandBuffer cmdBufSpectral = cmdGen.createCommandBuffer();
        VkBufferUsageFlags usage_ = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        meshio->m_pFixedSpectralBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufSpectral, meshio->fixedSpectrals, usage_));
        cmdGen.submitAndWait(cmdBufSpectral);
    }

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

    // 步骤 2：体元温度初值（sunlit / shaded 两个分量，单位 K）。缺省 298.15 K；
    //         有气象节点时取起始节点气温 Ta（°C，仅接受 -100~100 °C 范围内的有限值）。
    //         地形空气廓线启用时，按体元的空气采样高度修正气温，并检查结果在 150~400 K 之内。
    // Energy-balance temperatures are prognostic state variables.  Thermal
    // material temperatures belong to the pure radiative-transfer modes and
    // must not seed or prescribe VoxelEB.  Start the nonlinear solve from the
    // air temperature of the first requested meteorological node instead.
    VkCommandBuffer cmdBufTempe = cmdGen.createCommandBuffer();
    const int initialNode = voxellstio->meteos.empty() ? 0 : std::clamp(
        voxellstio->startTimeNode, 0,
        static_cast<int>(voxellstio->meteos.size()) - 1);
    float initialTemperature = 298.15f;
    if (!voxellstio->meteos.empty()) {
        const float airTemperature = voxellstio->meteos[initialNode].Ta;
        if (std::isfinite(airTemperature)
            && airTemperature > -100.0f && airTemperature < 100.0f) {
            initialTemperature = airTemperature + 273.15f;
        }
    }
    std::vector<VoxelTempe> voxelTempes(
        n_voxel, VoxelTempe{initialTemperature, initialTemperature});
    const auto& environment = voxellstio->terrainEnvironment;
    environment.validate();
    std::vector<float> airSamplingHeights(n_voxel, 0.0f);
    std::vector<float> prescribedSoilMoistures(n_voxel, -1.0f);
    for (size_t i = 0; i < voxelio->voxellinks.size(); ++i) {
        const auto& link = voxelio->voxellinks[i];
        const float centerHeight = (link.voxelId.y + 0.5f) * voxellstio->stepsize_height;
        airSamplingHeights[i] = std::max(0.0f, centerHeight);
        if (link.instanceId < instanceio->instanceLinks.size()) {
            const auto meshId = instanceio->instanceLinks[link.instanceId].meshId;
            if (meshId < meshio->meshLinks.size()) {
                const auto& mesh = meshio->meshLinks[meshId];
                if (meshId == 0 || mesh.type == static_cast<int>(Type::SOIL)) {
                    const float groundHeight = std::max(0.0f,
                        (link.voxelId.y + 1.0f) * voxellstio->stepsize_height);
                    // Meteorological near-surface air is sampled 2 m above soil.
                    airSamplingHeights[i] = groundHeight + (voxellstio->fluid.localExchange
                        ? 0.5f * std::max(0.1f, voxellstio->fluid.voxelSize) : 2.0f);
                    if (environment.soilEnabled && mesh.type == static_cast<int>(Type::SOIL)) {
                        if (mesh.bioId >= meshio->soilsets.size())
                            throw std::runtime_error("TerrainEnvironment: soil material index out of range");
                        prescribedSoilMoistures[i] = environment.soilMoisture(
                            groundHeight, meshio->soilsets[mesh.bioId].SatWater);
                    }
                }
            }
        }
        if (environment.airEnabled) {
            const float localTemperature = environment.temperatureC(
                initialTemperature - 273.15f, airSamplingHeights[i]) + 273.15f;
            if (!std::isfinite(localTemperature) || localTemperature < 150.0f || localTemperature > 400.0f)
                throw std::runtime_error("TerrainEnvironment: initial air temperature outside 150..400 K");
            voxelTempes[i] = {localTemperature, localTemperature};
        }
    }
    // 覆盖规则：土壤体元用配置的初始土壤温度；冠层结构类型 2/3（对应火/雾，见流体单元类型）
    // 且 fixedTemperature > 0 的体元，温度固定为该值。
    // Soil starts from the configured initial soil temperature Tsold.
    const bool hasInitialSoilTemperature = std::isfinite(voxellstio->initialSoilTemperature);
    const auto isSoilVoxel = [&](size_t index) {
        const VoxelLink& link = voxelio->voxellinks[index];
        if (link.instanceId >= instanceio->instanceLinks.size()) return false;
        const uint32_t meshId = instanceio->instanceLinks[link.instanceId].meshId;
        return meshId < meshio->meshLinks.size()
            && meshio->meshLinks[meshId].type == static_cast<int>(Type::SOIL);
    };
    for (size_t sparseIndex = 0; sparseIndex < voxelio->voxellinks.size(); ++sparseIndex) {
        if (hasInitialSoilTemperature && isSoilVoxel(sparseIndex)) {
            voxelTempes[sparseIndex] = VoxelTempe{
                voxellstio->initialSoilTemperature, voxellstio->initialSoilTemperature};
        }
        const VoxelLink& link = voxelio->voxellinks[sparseIndex];
        if (link.instanceId >= instanceio->instanceLinks.size()) continue;
        const uint32_t meshId = instanceio->instanceLinks[link.instanceId].meshId;
        if (meshId >= meshio->meshLinks.size()) continue;
        const MeshLink& mesh = meshio->meshLinks[meshId];
        if (mesh.canopyId >= meshio->canopies.size()) continue;
        const Canopy& canopy = meshio->canopies[mesh.canopyId];
        if ((canopy.structureType == 2 || canopy.structureType == 3)
            && std::isfinite(canopy.fixedTemperature) && canopy.fixedTemperature > 0.0f) {
            voxelTempes[sparseIndex] = VoxelTempe{
                canopy.fixedTemperature, canopy.fixedTemperature};
        }
    }
    // 上传体元温度缓冲（设备本地；后续时间步在 GPU 上更新，并可作为拷贝源回读）。
    voxelio->m_pTempeBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufTempe, voxelTempes,
                                                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufTempe);

    // 步骤 3：稠密流体网格。网格索引轴为 (x, y=垂直, z)：水平间距取 max(0.1, fluid.voxelSize) m，
    //         垂直范围取场景高度与输出高度中较大者；未启用流体时退化为 1×1×1。
    //         与稀疏辐射体元的对应关系由 FluidCellMeta（surfaceIndex/kind/lad）记录。
    // Dense atmospheric grid used by the fast integrated fluid solver.  The
    // radiative/energy model remains sparse; FluidCellMeta links both grids.
    const float radiativeHorizontalSpacing = std::max(0.01f, voxellstio->stepsize_surface);
    const float radiativeVerticalSpacing = std::max(0.01f, voxellstio->stepsize_height);
    const float horizontalSpacing = std::max(0.1f, voxellstio->fluid.voxelSize);
    const float verticalSpacing = horizontalSpacing;
    const float fluidVerticalExtent = std::max(
        voxellstio->sceneSize_XYZ.z,
        voxellstio->fluid.outputHeight + 0.5f * verticalSpacing);
    voxellstio->fluidGridSize = voxellstio->fluid.enabled
        ? glm::ivec3{
            std::max(1, static_cast<int>(std::ceil(voxellstio->sceneSize_XYZ.x / horizontalSpacing))),
            std::max(1, static_cast<int>(std::ceil(fluidVerticalExtent / verticalSpacing))),
            std::max(1, static_cast<int>(std::ceil(voxellstio->sceneSize_XYZ.y / horizontalSpacing)))}
        : glm::ivec3{1, 1, 1};
    const uint64_t fluidCellCount = static_cast<uint64_t>(voxellstio->fluidGridSize.x)
        * static_cast<uint64_t>(voxellstio->fluidGridSize.y)
        * static_cast<uint64_t>(voxellstio->fluidGridSize.z);
    // D3Q19 格子玻尔兹曼每个单元存 19 个分布函数，总数需能用 32 位索引表示，否则报错。
    if (fluidCellCount == 0
        || fluidCellCount > std::numeric_limits<uint32_t>::max() / 19U) {
        throw std::runtime_error("D3Q19 fluid grid is too large for 32-bit population indexing");
    }
    voxellstio->fluidCellCount = fluidCellCount;
    // 记录空气采样高度的范围；启用流体时扩展到覆盖整个流体网格的垂直范围，
    // 供下面的气象节点检查（validateAirState）使用。
    if (!airSamplingHeights.empty()) {
        const auto range = std::minmax_element(airSamplingHeights.begin(), airSamplingHeights.end());
        voxellstio->terrainAirHeightRange = {*range.first, *range.second};
    }
    if (voxellstio->fluid.enabled) {
        voxellstio->terrainAirHeightRange.x = std::min(voxellstio->terrainAirHeightRange.x, 0.5f * verticalSpacing);
        voxellstio->terrainAirHeightRange.y = std::max(voxellstio->terrainAirHeightRange.y,
            (voxellstio->fluidGridSize.y - 0.5f) * verticalSpacing);
    }
    // Fail before stepping/output if any requested meteorological node would
    // prescribe invalid air temperatures anywhere in the calculation domain.
    for (int node = voxellstio->startTimeNode; node < voxellstio->endTimeNode; ++node) {
        const auto& meteo = voxellstio->meteos.at(static_cast<size_t>(node));
        environment.validateAirState(meteo.Ta, meteo.ea,
            voxellstio->terrainAirHeightRange.x, voxellstio->terrainAirHeightRange.y);
    }

    // 步骤 3.1：初始风场与格子速度换算。初始风速取起始节点风速 u（m s-1），
    //         风向分解为 (sin, 0, cos) 水平分量。
    const float direction = glm::radians(voxellstio->fluid.windDirection);
    const float initialWind = !voxellstio->meteos.empty()
        ? std::max(0.0f, voxellstio->meteos[initialNode].u) : 0.0f;
    // 格子速度上限（格子单位，入口处更严）：用于保持 LBM 的低马赫数稳定（取值依据待核对）。
    // latticeVelocityScale 为物理速度（m s-1）到格子速度的固定换算比，取遍历所有气象节点的最大风速
    // 和 fluid.maxVelocity 所需的最大值，且不小于 1。
    constexpr float latticeVelocityLimit = 0.18f;
    constexpr float inletLatticeVelocityLimit = 0.08f;
    float maximumMeteoWind = initialWind;
    for (const Meteo& meteo : voxellstio->meteos)
        maximumMeteoWind = std::max(maximumMeteoWind, std::max(0.0f, meteo.u));
    // A fixed conversion keeps already initialized populations valid when the
    // wind changes between meteorological nodes.
    const float latticeVelocityScale = std::max({
        1.0f,
        maximumMeteoWind / inletLatticeVelocityLimit,
        std::max(0.1f, voxellstio->fluid.maxVelocity) / latticeVelocityLimit});
    const float initialScalarTimeStep = std::max(0.001f, voxellstio->fluid.timeStep);
    const glm::vec4 wind{
        initialWind * std::sin(direction), 0.0f,
        initialWind * std::cos(direction), 0.0f};
    // 步骤 3.2：流体场初值。fluidVelocity：xyz 为风速 [m s-1]；
    //         fluidScalar：x=温度 [K]，z=水汽（气象 ea，缺省 15；单位 hPa，待核对），y、w 初值为 0；
    //         启用地形空气廓线时，按单元中心高度给温度和水汽赋垂直分布。
    std::vector<glm::vec4> fluidVelocity(static_cast<size_t>(fluidCellCount), wind);
    std::vector<glm::vec4> fluidScalar(
        static_cast<size_t>(fluidCellCount),
        glm::vec4(initialTemperature, 0.0f,
                  !voxellstio->meteos.empty() ? voxellstio->meteos[initialNode].ea : 15.0f,
                  0.0f));
    std::vector<FluidCellMeta> fluidMeta(static_cast<size_t>(fluidCellCount));
    if (environment.airEnabled) {
        const float referenceVapor = fluidScalar.front().z;
        for (int y = 0; y < voxellstio->fluidGridSize.y; ++y) {
            const float height = (y + 0.5f) * verticalSpacing;
            const float temperature = environment.temperatureC(initialTemperature - 273.15f, height);
            if (!std::isfinite(temperature) || temperature < -123.15f || temperature > 126.85f)
                throw std::runtime_error("TerrainEnvironment: fluid initial temperature outside 150..400 K");
            const float vapor = environment.vaporHpa(
                referenceVapor, height, temperature);
            for (int z = 0; z < voxellstio->fluidGridSize.z; ++z)
                for (int x = 0; x < voxellstio->fluidGridSize.x; ++x) {
                    const size_t i = static_cast<size_t>(x) + static_cast<size_t>(voxellstio->fluidGridSize.x)
                        * (z + static_cast<size_t>(voxellstio->fluidGridSize.z) * y);
                    fluidScalar[i].x = temperature + 273.15f;
                    fluidScalar[i].z = vapor;
                }
        }
    }
    const size_t horizontalCellCount = static_cast<size_t>(voxellstio->fluidGridSize.x)
        * static_cast<size_t>(voxellstio->fluidGridSize.z);
    std::vector<int> buildingTop(horizontalCellCount, -1);
    std::vector<int> buildingSurface(horizontalCellCount, -1);

    // denseIndex：稠密网格线性索引 x + nx·(z + nz·y)；越界返回 -1。
    const auto denseIndex = [&](int x, int y, int z) -> int64_t {
        if (x < 0 || y < 0 || z < 0 || x >= voxellstio->fluidGridSize.x
            || y >= voxellstio->fluidGridSize.y || z >= voxellstio->fluidGridSize.z) return -1;
        return static_cast<int64_t>(x) + static_cast<int64_t>(voxellstio->fluidGridSize.x)
            * (static_cast<int64_t>(z) + static_cast<int64_t>(voxellstio->fluidGridSize.z) * y);
    };
    // mappedRange：把稀疏体元的 [i·s, (i+1)·s) 区间映射到流体网格的索引闭区间 [first, last]；
    // 与网格无交集时返回空区间 {1, 0}；±1e-5 的容差避免边界浮点误差多算一格。
    const auto mappedRange = [](int sourceIndex, float sourceSpacing,
                                float targetSpacing, int targetCount) {
        const float lower = static_cast<float>(sourceIndex) * sourceSpacing;
        const float upper = static_cast<float>(sourceIndex + 1) * sourceSpacing;
        if (upper <= 0.0f || lower >= static_cast<float>(targetCount) * targetSpacing)
            return std::pair<int, int>{1, 0};
        const int first = std::max(0, static_cast<int>(std::floor(lower / targetSpacing + 1e-5f)));
        const int last = std::min(targetCount - 1,
            static_cast<int>(std::ceil(upper / targetSpacing - 1e-5f)) - 1);
        return std::pair<int, int>{first, last};
    };
    // 单元类型优先级：固体(1) > 火/雾(3、4) > 植被(2) > 空气(0)；多个体元落入同一流体单元时
    // 取优先级高者（相同时后者覆盖，植被的 lad 取最大值）。
    const auto kindPriority = [](int kind) {
        if (kind == 1) return 4;
        if (kind == 3 || kind == 4) return 3;
        if (kind == 2) return 2;
        return 0;
    };
    // 步骤 3.3：把稀疏辐射体元映射到稠密流体网格，填 fluidMeta（surfaceIndex、kind、lad）。
    //         kind：0 空气、1 固体、2 植被、3 火、4 雾；植被叶面积密度 lad = LAI / max(垂直间距, 冠高)。
    for (size_t sparseIndex = 0; sparseIndex < voxelio->voxellinks.size(); ++sparseIndex) {
        const VoxelLink& link = voxelio->voxellinks[sparseIndex];
        int kind = 1;
        float lad = 0.0f;
        int canopyStructure = 1;
        bool building = false;
        if (link.instanceId >= 0 && link.instanceId < static_cast<int>(instanceio->instanceLinks.size())) {
            const int meshId = instanceio->instanceLinks[link.instanceId].meshId;
            if (meshId >= 0 && meshId < static_cast<int>(meshio->meshLinks.size())) {
                const MeshLink& mesh = meshio->meshLinks[meshId];
                building = mesh.type == static_cast<int>(Type::BUILDING);
                // Background DEM voxels represent only the topmost soil cell.
                // Fill their columns too, so terrain interior cannot become air.
                building = building || ((environment.airEnabled || environment.soilEnabled) && meshId == 0);
                if (mesh.type == static_cast<int>(Type::VEGETATION)) {
                    canopyStructure = 0;
                    if (mesh.canopyId >= 0 && mesh.canopyId < static_cast<int>(meshio->canopies.size())) {
                        const Canopy& canopy = meshio->canopies[mesh.canopyId];
                        canopyStructure = canopy.structureType;
                        lad = std::max(0.0f, canopy.lai /
                            std::max(radiativeVerticalSpacing, canopy.height));
                    }
                    // 冠层结构类型 → 流体单元类型：1→固体(1)，2→火(3)，3→雾(4)，其余（含 0）→植被(2)。
                    kind = canopyStructure == 1 ? 1
                        : canopyStructure == 2 ? 3
                        : canopyStructure == 3 ? 4 : 2;
                }
            }
        }
        const glm::ivec3 id = link.voxelId;
        const auto rangeX = mappedRange(id.x, radiativeHorizontalSpacing,
                                        horizontalSpacing, voxellstio->fluidGridSize.x);
        const auto rangeZ = mappedRange(id.z, radiativeHorizontalSpacing,
                                        horizontalSpacing, voxellstio->fluidGridSize.z);
        if (rangeX.first > rangeX.second || rangeZ.first > rangeZ.second) continue;
        // 垂直序号为负的体元（地表以下的表面体元，待核对）只给最底层空气单元记录 surfaceIndex，
        // 不参与固体/植被分类。
        if (id.y < 0) {
            for (int z = rangeZ.first; z <= rangeZ.second; ++z)
                for (int x = rangeX.first; x <= rangeX.second; ++x) {
                    const int64_t air = denseIndex(x, 0, z);
                    if (air >= 0 && fluidMeta[air].surfaceIndex < 0)
                        fluidMeta[air].surfaceIndex = static_cast<int>(sparseIndex);
                }
            continue;
        }
        const auto rangeY = mappedRange(id.y, radiativeVerticalSpacing,
                                        verticalSpacing, voxellstio->fluidGridSize.y);
        if (rangeY.first > rangeY.second) continue;
        for (int y = rangeY.first; y <= rangeY.second; ++y)
            for (int z = rangeZ.first; z <= rangeZ.second; ++z)
                for (int x = rangeX.first; x <= rangeX.second; ++x) {
                    const int64_t cell = denseIndex(x, y, z);
                    FluidCellMeta& target = fluidMeta[cell];
                    if (kindPriority(kind) >= kindPriority(target.kind)) {
                        target = {static_cast<int>(sparseIndex), kind,
                                  kind == 2 ? std::max(target.lad, lad) : lad, 0.0f};
                    }
                    if (target.kind == 1) fluidVelocity[cell] = glm::vec4(0.0f);
                    if (building) {
                        const size_t column = static_cast<size_t>(x)
                            + static_cast<size_t>(voxellstio->fluidGridSize.x)
                            * static_cast<size_t>(z);
                        if (y >= buildingTop[column]) {
                            buildingTop[column] = y;
                            buildingSurface[column] = static_cast<int>(sparseIndex);
                        }
                    }
                }
    }
    // Imported building OBJ files usually provide walls and roofs but no
    // explicit volumetric interior. Fill every roof/wall footprint column
    // down to the ground so indoor cells are solid, not simulated outdoor air.
    for (int z = 0; z < voxellstio->fluidGridSize.z; ++z)
        for (int x = 0; x < voxellstio->fluidGridSize.x; ++x) {
            const size_t column = static_cast<size_t>(x)
                + static_cast<size_t>(voxellstio->fluidGridSize.x) * static_cast<size_t>(z);
            const int top = std::min(buildingTop[column], voxellstio->fluidGridSize.y - 1);
            if (top < 0) continue;
            for (int y = 0; y <= top; ++y) {
                const int64_t cell = denseIndex(x, y, z);
                fluidMeta[cell] = {buildingSurface[column], 1, 0.0f, 0.0f};
                fluidVelocity[cell] = glm::vec4(0.0f);
            }
        }
    // Expose solid surface temperatures to neighbouring air cells without
    // turning those cells into solids themselves.
    const glm::ivec3 neighbours[] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (int y = 0; y < voxellstio->fluidGridSize.y; ++y)
        for (int z = 0; z < voxellstio->fluidGridSize.z; ++z)
            for (int x = 0; x < voxellstio->fluidGridSize.x; ++x) {
                const int64_t cell = denseIndex(x, y, z);
                if (fluidMeta[cell].kind != 1 || fluidMeta[cell].surfaceIndex < 0) continue;
                for (const glm::ivec3& offset : neighbours) {
                    const int64_t adjacent = denseIndex(x + offset.x, y + offset.y, z + offset.z);
                    if (adjacent >= 0 && fluidMeta[adjacent].kind == 0
                        && fluidMeta[adjacent].surfaceIndex < 0)
                        fluidMeta[adjacent].surfaceIndex = fluidMeta[cell].surfaceIndex;
                }
            }

    // 步骤 3.4：D3Q19 格子：19 个离散速度方向 c_i 及其权重 w_i（静止 1/3，6 个轴向 1/18，12 个对角 1/36）。
    constexpr std::array<std::array<int, 3>, 19> latticeDirections{{
        {{0,0,0}}, {{1,0,0}}, {{-1,0,0}}, {{0,1,0}}, {{0,-1,0}},
        {{0,0,1}}, {{0,0,-1}}, {{1,1,0}}, {{-1,-1,0}}, {{1,-1,0}},
        {{-1,1,0}}, {{1,0,1}}, {{-1,0,-1}}, {{1,0,-1}}, {{-1,0,1}},
        {{0,1,1}}, {{0,-1,-1}}, {{0,1,-1}}, {{0,-1,1}}
    }};
    constexpr std::array<float, 19> latticeWeights{{
        1.0f / 3.0f,
        1.0f / 18.0f, 1.0f / 18.0f, 1.0f / 18.0f,
        1.0f / 18.0f, 1.0f / 18.0f, 1.0f / 18.0f,
        1.0f / 36.0f, 1.0f / 36.0f, 1.0f / 36.0f,
        1.0f / 36.0f, 1.0f / 36.0f, 1.0f / 36.0f,
        1.0f / 36.0f, 1.0f / 36.0f, 1.0f / 36.0f,
        1.0f / 36.0f, 1.0f / 36.0f, 1.0f / 36.0f
    }};
    if (fluidCellCount > std::numeric_limits<size_t>::max() / 19U) {
        throw std::runtime_error("D3Q19 fluid grid exceeds host address space");
    }
    std::vector<float> fluidLbm(static_cast<size_t>(fluidCellCount) * 19U);
    std::vector<float> fluidDensity(static_cast<size_t>(fluidCellCount), 1.0f);
    for (size_t cell = 0; cell < static_cast<size_t>(fluidCellCount); ++cell) {
        glm::vec3 latticeVelocity = fluidMeta[cell].kind == 1
            ? glm::vec3(0.0f)
            : glm::vec3(fluidVelocity[cell]) / latticeVelocityScale;
        const float speed = glm::length(latticeVelocity);
        if (speed > latticeVelocityLimit) {
            latticeVelocity *= latticeVelocityLimit / speed;
        }
        // 平衡态分布（密度 ρ=1）：f_i = w_i·[1 + 3 c_i·u + 4.5 (c_i·u)² − 1.5 |u|²]；
        // 固体单元速度取 0；速度超过 latticeVelocityLimit 时按比例缩回。
        const float velocitySquared = glm::dot(latticeVelocity, latticeVelocity);
        for (size_t direction = 0; direction < latticeDirections.size(); ++direction) {
            const auto& c = latticeDirections[direction];
            const float cu = static_cast<float>(c[0]) * latticeVelocity.x
                + static_cast<float>(c[1]) * latticeVelocity.y
                + static_cast<float>(c[2]) * latticeVelocity.z;
            fluidLbm[cell * 19U + direction] = latticeWeights[direction]
                * (1.0f + 3.0f * cu + 4.5f * cu * cu - 1.5f * velocitySquared);
        }
    }

    // 步骤 3.5：上传流体缓冲。速度、标量、LBM 分布函数各建 A/B 两份（推测为乒乓交换，待核对）；
    //         另有单元元数据与密度缓冲。
    const VkBufferUsageFlags fluidUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto createFluidVector = [&](const auto& values) {
        VkCommandBuffer command = cmdGen.createCommandBuffer();
        auto buffer = std::make_shared<nvvk::Buffer>(
            m_pAlloc->createBuffer(command, values, fluidUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
        cmdGen.submitAndWait(command);
        return buffer;
    };
    voxellstio->m_pFluidVelocityA = createFluidVector(fluidVelocity);
    voxellstio->m_pFluidVelocityB = createFluidVector(fluidVelocity);
    voxellstio->m_pFluidScalarA = createFluidVector(fluidScalar);
    voxellstio->m_pFluidScalarB = createFluidVector(fluidScalar);
    voxellstio->m_pFluidMeta = createFluidVector(fluidMeta);
    voxellstio->m_pFluidLbmA = createFluidVector(fluidLbm);
    voxellstio->m_pFluidLbmB = createFluidVector(fluidLbm);
    voxellstio->m_pFluidDensity = createFluidVector(fluidDensity);

    // 步骤 3.6：填写并上传流体参数 uniform（FluidParameters，布局见 base/structs_cg.h）：
    //         网格尺寸/开关、间距与时间步、环境风与温度、物理系数、源项、耦合间距、地形廓线。
    voxellstio->fluidParameters.grid = glm::ivec4(
        voxellstio->fluidGridSize, voxellstio->fluid.enabled ? 1 : 0);
    voxellstio->fluidParameters.spacingTime = glm::vec4(
        horizontalSpacing, verticalSpacing, horizontalSpacing, initialScalarTimeStep);
    voxellstio->fluidParameters.ambientWind = glm::vec4(
        wind.x, wind.y, wind.z, initialTemperature);
    voxellstio->fluidParameters.physics = glm::vec4(
        voxellstio->fluid.buoyancy, voxellstio->fluid.dragCoefficient,
        voxellstio->fluid.thermalCoupling, voxellstio->fluid.diffusivity);
    voxellstio->fluidParameters.sources = glm::vec4(
        voxellstio->fluid.smokeEmission,
        latticeVelocityLimit,
        voxellstio->fluid.maxVelocity, latticeVelocityScale);
    voxellstio->fluidParameters.couplingSpacing = glm::vec4(
        radiativeHorizontalSpacing, radiativeVerticalSpacing,
        radiativeHorizontalSpacing, environment.soilEnabled ? 1.0f : 0.0f);
    voxellstio->fluidParameters.heightProfile = glm::vec4(
        environment.airEnabled ? 1.0f : 0.0f, environment.temperatureLapseRate,
        environment.referenceHeight, environment.vaporScaleHeight);
    voxellstio->fluidParameters.localExchange = glm::vec4(
        voxellstio->fluid.localExchange ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    VkCommandBuffer fluidParameterCommand = cmdGen.createCommandBuffer();
    voxellstio->m_pFluidParameters = std::make_shared<nvvk::Buffer>(
        m_pAlloc->createBuffer(fluidParameterCommand, sizeof(FluidParameters),
            &voxellstio->fluidParameters,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    cmdGen.submitAndWait(fluidParameterCommand);


    // 步骤 4：场景几何缓冲：每个 primMesh 的顶点/索引缓冲（带设备地址，供 BLAS 构建与着色器读取），
    //          然后是 meshLink、实例链接、体元链接/六面体/NanoVDB 数据。
    ///--------------------------------------------------------------------
    ///  Scene properties
    ///--------------------------------------------------------------------
    // obj models buffer
    // Proxy-geometry vertex format (control.voxelProxyGeometry / HISTREAM_VOXEL_PROXY, src/base/proxymesh.h).
    const int proxyMode = voxelflags::proxyMode(voxellstio->voxelFlags);
    if (std::getenv("HISTREAM_VOXEL_PROXY_STATS"))
        std::cout << "PROXY_GEOMETRY\tmodel=VoxelEB\tmode=" << proxymesh::modeName(proxyMode) << std::endl;
    for (int kmodel = 0; kmodel < meshio->primMeshes.size(); kmodel++)
    {
        if (proxyMode != voxelflags::kProxyFull)
        {
            meshio->m_bufferMeshes.emplace_back(proxymesh::uploadPositionMesh(
                *m_pAlloc, voxellstio->m_physicalDevice, m_device, m_queue.familyIndex, m_queue.queue,
                "VoxelEB", static_cast<size_t>(kmodel), meshio->primMeshes[kmodel], proxyMode));
            continue;
        }
        // Legacy 44-byte vertices: both buffers and their same-size staging buffers must fit one allocation.
        proxymesh::requireFullMeshFits(voxellstio->m_physicalDevice, "VoxelEB", static_cast<size_t>(kmodel), meshio->primMeshes[kmodel]);
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
        meshio->meshLinks[kmodel].indexAddress = meshio->m_bufferMeshes[kmodel].indexed
            ? nvvk::getBufferDeviceAddress(m_device, meshio->m_bufferMeshes[kmodel].indexBuffer.buffer) : 0;
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


    // 步骤 5：用前面的网格与实例构建加速结构（BLAS/TLAS）。
    ///--------------------------------------------------------------------
    ///  Accelerate properties
    ///--------------------------------------------------------------------
    m_pAccelStruct->m_physicalDevice = voxellstio->m_physicalDevice;  // explicit BLAS size checks (voxel modes only)
    m_pAccelStruct->m_modelName = "VoxelEB";
    m_pAccelStruct->createAccelStruct(m_device,meshio,instanceio);

    // 步骤 6：观测/光源几何缓冲：传感器、光源 uniform；波长、大气条件存储缓冲；再建体元方向缓冲。
    ///--------------------------------------------------------------------
    ///  geometry properties
    ///--------------------------------------------------------------------
    VkCommandBuffer cmdBufSensor = cmdGen.createCommandBuffer();
    voxellstio->m_pBufferSensor = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufSensor, sizeof(SensorMatrix), &voxellstio->sensor,
                                                                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    cmdGen.submitAndWait(cmdBufSensor);

//    VkCommandBuffer cmdBufWaveInds = cmdGen.createCommandBuffer();
//    raytracingio->m_pBufferWaveInds = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufWaveInds, raytracingio->sensorWavelengthInds, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
//    cmdGen.submitAndWait(cmdBufWaveInds);

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

    // meteo and aero
    if (voxellstio->atomconds.size() > 0)
    {
        VkCommandBuffer cmdBufWave = cmdGen.createCommandBuffer();
        voxellstio->m_pBufferAtomcond = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufWave, voxellstio->atomconds, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        cmdGen.submitAndWait(cmdBufWave);
    }

    // dir
    VkCommandBuffer cmdBufDir = cmdGen.createCommandBuffer();
    std::vector<VoxelDir> voxelDirs(n_voxel, VoxelDir{0, 1});
    voxelio->m_pDirBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufDir, voxelDirs,
                                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufDir);

    // 步骤 7：体元辐射场缓冲：先清零，并把设备地址写入推送常量供着色器直接引用。
    // rads
    VkCommandBuffer cmdBufRads = cmdGen.createCommandBuffer();
    // Radiance field (N x DIFFUSENUM VoxelRad) followed by the per-direction
    // sky gap transmittance (N x DIFFUSENUM floats) that lets later spectral
    // groups reuse the first group's uncollided-sky trace
    // (voxelrad_scattering_VNIR.comp stage 4), then the optical image's
    // multiple-scattering environment (N x n_wave floats,
    // shader/voxeleb/image_environment.glsl). The shaders address the buffer by
    // device address with 64-bit element indices (shader/voxel_radindex.glsl), so the
    // former 2^32-entry limit (44.7 M voxels, silently wrapping the sky cache and
    // dropping the environment slots) is gone; only the available VRAM limits it.
    // (voxelflags::testRadianceVoxels: test knob that pretends a larger voxel count for the layout, normally == n_voxel.)
    const VkDeviceSize layoutVoxels = static_cast<VkDeviceSize>(voxelflags::testRadianceVoxels(n_voxel));
    const VkDeviceSize environmentFloats =
        layoutVoxels * static_cast<VkDeviceSize>(std::max(voxellstio->n_wave, 0));
    const bool imageEnvironment = environmentFloats > 0;
    voxellstio->setting.imageEnvironment = imageEnvironment ? 1 : 0;
    // 总字节数 = 辐射场 + 各方向天空间隙透射率 + （可选）图像多次散射环境；
    // 环境部分每个 VoxelRad 容纳 2 个 float，故按 (n+1)/2 个条目计。
    const VkDeviceSize radianceBytes = layoutVoxels * DIFFUSENUM * sizeof(VoxelRad)
        + layoutVoxels * DIFFUSENUM * sizeof(float)
        + (imageEnvironment ? (environmentFloats + 1) / 2 * sizeof(VoxelRad) : 0);
    auto& radianceAllocator = voxellstio->m_radianceAllocator;
    // 辐射场缓冲很大，用单独的专用分配器分配并由 VoxelebIO 持有（具体原因待核对）。
    radianceAllocator = std::make_unique<nvvk::ResourceAllocatorDedicated>(
        m_device, voxellstio->m_physicalDevice);
    const auto radianceFailure = [&](const std::string& reason) {
        return std::runtime_error("VoxelEB: cannot allocate the radiance buffer for " + voxellimits::grouped(static_cast<uint64_t>(layoutVoxels))
            + " voxels (" + voxellimits::grouped(radianceBytes) + " bytes, "
            + std::to_string(radianceBytes / std::max<VkDeviceSize>(layoutVoxels, 1))
            + " B per voxel at n_wave=" + std::to_string(std::max(voxellstio->n_wave, 0)) + "): " + reason
            + ". Reduce the scene size, the voxel count or the number of sensor bands.");
    };
    try {
        voxelio->m_pRadsBuffer = std::make_shared<nvvk::Buffer>(radianceAllocator->createBuffer(
            radianceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    } catch (const std::exception& error) {
        throw radianceFailure(error.what());
    }
    if (!voxelio->m_pRadsBuffer->buffer) throw radianceFailure("the allocation returned no buffer (out of device memory)");
    vkCmdFillBuffer(cmdBufRads, voxelio->m_pRadsBuffer->buffer, 0, radianceBytes, 0);
    cmdGen.submitAndWait(cmdBufRads);
    voxellstio->setting.radianceAddress =
        nvvk::getBufferDeviceAddress(m_device, voxelio->m_pRadsBuffer->buffer);
    if (!voxellstio->setting.radianceAddress) throw std::runtime_error("Voxel radiance device address is null");
    std::cout << "RADIANCE_BUFFER\tbytes=" << radianceBytes
              << "\timageEnvironment=" << voxellstio->setting.imageEnvironment
              << "\taddress=" << voxellstio->setting.radianceAddress << std::endl;

    {
        // 短波散射/太阳可见性诊断缓冲（统计块位于 kSolarVisibilityStatsOffset 之后），
        // 地址同样写入推送常量。
        constexpr VkDeviceSize statsBytes =
            kSolarVisibilityStatsOffset + sizeof(SolarVisibilityStats);
        voxellstio->m_pScatteringStats = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(
            statsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
        if (!voxellstio->m_pScatteringStats->buffer) {
            throw std::runtime_error("Cannot allocate shortwave scattering diagnostics");
        }
        voxellstio->setting.scatteringStatsAddress = nvvk::getBufferDeviceAddress(
            m_device, voxellstio->m_pScatteringStats->buffer);
        if (!voxellstio->setting.scatteringStatsAddress) {
            throw std::runtime_error("Shortwave scattering diagnostics address is null");
        }
    }

    // （步骤 7 续）净辐射 VoxelNetRad 与 PAR 吸收量 VoxelPnet 缓冲，逐体元，初值全为 0。
    // netRad
    VkCommandBuffer cmdBufNetRad = cmdGen.createCommandBuffer();
    std::vector<VoxelNetRad> voxelNetRads(n_voxel, VoxelNetRad{0, 0, 0, 0});
    voxelio->m_pNetRadBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufNetRad, voxelNetRads,
                                                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufNetRad);

    // pnet
    VkCommandBuffer cmdBufPnet = cmdGen.createCommandBuffer();
    std::vector<VoxelPnet> voxelPnets(n_voxel, VoxelPnet{0, 0});
    voxelio->m_pPnetBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufPnet, voxelPnets,
                                                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufPnet);

// 步骤 8：气动参数（aeroconds）。
///--------------------------------------------------------------------
    ///  aero properties
    ///--------------------------------------------------------------------
    // aero
    auto cmdBufAero = cmdGen.createCommandBuffer();
    voxellstio->m_pBufferAero = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufAero, voxellstio->aeroconds,
                                                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufAero);

    // 步骤 9：气象 uniform（当前节点 k_node 的 Meteo）与体元空气动力学阻抗 Raa 缓冲（初值 0）。
    ///--------------------------------------------------------------------
    ///  meteo properties
    ///--------------------------------------------------------------------
    // meteo
    auto cmdBufMeteo = cmdGen.createCommandBuffer();
    uint32_t size = sizeof(Meteo);
    // voxellstio->m_pMeteoBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufMeteo,sizeof(Meteo), &voxellstio->meteos[voxellstio->k_node],
    //                                                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    //                                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));
    voxellstio->m_pMeteoBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufMeteo,sizeof(Meteo), &voxellstio->meteos[voxellstio->k_node],
                                                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));

    cmdGen.submitAndWait(cmdBufMeteo);



    // surfL
//    VkCommandBuffer cmdBufSurfL = cmdGen.createCommandBuffer();
//    VkBufferUsageFlags usage_ = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
//    voxelio->m_pSurfLBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufSurfL, voxelio->surfLs, usage_));
//    cmdGen.submitAndWait(cmdBufSurfL);

    // raa
    VkCommandBuffer cmdBufRaa = cmdGen.createCommandBuffer();
    std::vector<VoxelRaa> voxelRaas(n_voxel, VoxelRaa{0});
    voxelio->m_pRaaBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufRaa, voxelRaas,
                                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufRaa);

    // 步骤 10：生物参数（叶片、土壤、水体参数表）与体元叶面空气状态 VoxelAir、表面阻抗 Rss。
    ///--------------------------------------------------------------------
    ///  bio properties
    ///--------------------------------------------------------------------
    auto cmdBufBio = cmdGen.createCommandBuffer();
    // leafBio
    meshio->m_pLeafBioBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufBio, meshio->leafbios, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    // soilSet
    meshio->m_pSoilSetBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufBio, meshio->soilsets, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    // waterSet
    meshio->m_pWaterSetBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufBio, meshio->watersets, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    // air
    // 叶面空气状态初值：cs = Ca、ci = 0.7·Ca（ppm），es = ea（hPa），向阳/背阴相同；
    // 再叠加地形环境给出的 CO2 超额 co2ExcessPpm（cs 全加，ci 按 0.7 倍加）。
    // 说明：取起始气象节点 initialNode，与前面的温度初值、风速、水汽压初值使用同一节点
    //       （此前误取第 0 个节点，startTimeNode ≠ 0 时与温度初值不一致，2026-11-25 修正；
    //       实测改后输出逐字节不变：es、cs 在 evapo 每轮迭代中按当前节点气象重写，初值未见影响）。
    const Meteo& firstMeteo = voxellstio->meteos[initialNode];
    const VoxelAir initialAir{{firstMeteo.Ca, firstMeteo.Ca},
                              {0.7f * firstMeteo.Ca, 0.7f * firstMeteo.Ca},
                              {firstMeteo.ea, firstMeteo.ea}};
    std::vector<VoxelAir> voxelair(n_voxel, initialAir);
    for (size_t i = 0; i < voxelair.size(); ++i) {
        voxelair[i].heightM = airSamplingHeights[i];
        voxelair[i].soilMoisture = prescribedSoilMoistures[i];
        voxelair[i].co2ExcessPpm = environment.co2ExcessPpm(airSamplingHeights[i]);
        for (int state = 0; state < 2; ++state) {
            voxelair[i].cs[state] += voxelair[i].co2ExcessPpm;
            voxelair[i].ci[state] += 0.7f * voxelair[i].co2ExcessPpm;
        }
        if (environment.airEnabled) {
            const auto& meteo = voxellstio->meteos[initialNode];
            const float temperature = environment.temperatureC(meteo.Ta, airSamplingHeights[i]);
            const float vapor = environment.vaporHpa(meteo.ea, airSamplingHeights[i], temperature);
            voxelair[i].es[0] = voxelair[i].es[1] = vapor;
        }
    }
    // voxelio->m_pAirBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufBio, voxelair,
    //                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    //                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));
    voxelio->m_pAirBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufBio, voxelair,
                                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufBio);

    // rss: 300 s/m is the physiology's default start when no previous value exists.
    VkCommandBuffer cmdBufRss = cmdGen.createCommandBuffer();
    std::vector<VoxelRss> voxelRsss(n_voxel, VoxelRss{300.0f, 300.0f});
    voxelio->m_pRssBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufRss, voxelRsss,
                                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufRss);

    // 步骤 11：蒸散/能量平衡状态：热通量 VoxelHeatflux（清零）、土壤温度历史 TLAST、
    //          土壤方法 1 的长历史记忆缓冲、能量平衡统计状态 EBState。
    ///--------------------------------------------------------------------
    ///  Evapo properties
    ///--------------------------------------------------------------------
    auto cmdBufEvapo = cmdGen.createCommandBuffer();
    // flux
    voxelio->m_pFluxBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(static_cast<size_t>(n_voxel) * sizeof(VoxelHeatflux),
                                                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    if (!voxelio->m_pFluxBuffer->buffer) throw std::runtime_error("Cannot allocate voxel flux buffer");
    vkCmdFillBuffer(cmdBufEvapo, voxelio->m_pFluxBuffer->buffer, 0, VK_WHOLE_SIZE, 0);
    // tLast
    // TLAST 初值（单位 °C）：每体元 TLASTNUM 层，由表面温度向深层温度线性过渡。
    // 表面温度：土壤体元用 initialSoilTemperature，否则用初始气温；
    // 深层温度：方法 2 土壤取该土壤集的 Tsoil（仅接受 -100~100 °C 的有限值），否则等于表面温度。
    const float initialTemperatureC = initialTemperature - 273.15f;
    const size_t tlastElementCount = static_cast<size_t>(n_voxel) * static_cast<size_t>(TLASTNUM);
    if (n_voxel < 0
        || tlastElementCount / static_cast<size_t>(TLASTNUM) != static_cast<size_t>(n_voxel)) {
        throw std::overflow_error("Voxeleb: tLast element count overflow");
    }
    std::vector<TLAST> tlast(
        tlastElementCount,
        TLAST{initialTemperatureC, initialTemperatureC});
    for (int i = 0; i < n_voxel; ++i) {
        const float surfaceTemperatureC = hasInitialSoilTemperature && isSoilVoxel(i)
            ? voxellstio->initialSoilTemperature - 273.15f : initialTemperatureC;
        float deepSoilTemperature = surfaceTemperatureC;
        const int instanceId = voxelio->voxellinks[i].instanceId;
        if (instanceId >= 0
            && instanceId < static_cast<int>(instanceio->instanceLinks.size())) {
            const int meshId = instanceio->instanceLinks[instanceId].meshId;
            if (meshId >= 0
                && meshId < static_cast<int>(meshio->meshLinks.size())) {
                const MeshLink& meshLink = meshio->meshLinks[meshId];
                const int bioId = meshLink.bioId;
                if (meshLink.type == static_cast<int>(Type::SOIL)
                    && bioId >= 0
                    && bioId < static_cast<int>(meshio->soilsets.size())
                    && meshio->soilsets[bioId].method == 2
                    && std::isfinite(meshio->soilsets[bioId].Tsoil)
                    && meshio->soilsets[bioId].Tsoil > -100.0f
                    && meshio->soilsets[bioId].Tsoil < 100.0f) {
                    deepSoilTemperature = meshio->soilsets[bioId].Tsoil;
                }
            }
        }
        for (int k = 0; k < TLASTNUM; ++k) {
            const float depthFraction = TLASTNUM > 1
                ? static_cast<float>(k) / static_cast<float>(TLASTNUM - 1)
                : 0.0f;
            const float profileTemperature = surfaceTemperatureC
                + depthFraction * (deepSoilTemperature - surfaceTemperatureC);
            tlast[i * TLASTNUM + k] =
                TLAST{profileTemperature, profileTemperature};
        }
    }
    voxelio->m_pTLASTBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufEvapo, tlast,
                                                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufEvapo);

    // Soil method-1 history beyond the TLASTNUM nodes (soilMemoryMode in
    // functions.glsl): SOIL_MEMORY_MODES (sunlit, shaded) pairs per voxel, all
    // zero (the uniform start history has no temperature changes).  Only
    // allocated when a method-1 soil set enables it (control.soilInertiaHistory).
    {
        bool soilMemoryUsed = false;
        for (const SoilSet& soil : meshio->soilsets)
            soilMemoryUsed = soilMemoryUsed || (soil.method == 1 && soil.inertiaHistory == 1);
        const size_t memoryElements = soilMemoryUsed
            ? static_cast<size_t>(n_voxel) * static_cast<size_t>(SOIL_MEMORY_MODES) : 1U;
        if (soilMemoryUsed)
            voxellimits::requireStorageRange(voxellstio->m_physicalDevice, "VoxelEB", "soil inertia history",
                                             static_cast<uint64_t>(n_voxel),
                                             static_cast<uint64_t>(SOIL_MEMORY_MODES) * sizeof(glm::vec2));
        std::vector<glm::vec2> soilMemory(memoryElements, glm::vec2(0.0f));
        auto cmdBufSoilMemory = cmdGen.createCommandBuffer();
        voxelio->m_pSoilMemoryBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(
            cmdBufSoilMemory, soilMemory, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
        cmdGen.submitAndWait(cmdBufSoilMemory);
        if (soilMemoryUsed)
            std::cout << "SOIL_INERTIA_HISTORY modes=" << SOIL_MEMORY_MODES << " explicitNodes=" << TLASTNUM
                      << " bytes=" << memoryElements * sizeof(glm::vec2) << std::endl;
    }

    // state
    {
        // Ship waterline contact (minimal VoxelEB ship): switched on only by a
        // Ship material with waterHeatTransferCoefficient > 0.
        bool shipWaterline = false;
        for (const SoilSet& soil : meshio->soilsets)
            shipWaterline = shipWaterline || soil.waterHeatTransferCoefficient > 0.0f;
        voxellstio->shipWaterline = shipWaterline;
        if (shipWaterline) std::cout << "SHIP_WATERLINE\tvoxelEB hull voxels below the waterline exchange with the scene water temperature" << std::endl;
    }
    // EBState 缓冲：能量平衡的定点累加统计量（布局须与 shader/parameters.h 一致，见 structs_cg.h）。
    auto cmdBufState = cmdGen.createCommandBuffer();
    size = sizeof(EBState);
    // voxelio->m_pStateBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufState,
    //                                                          size, &voxelio->m_state,
    //                                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    //                                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    voxelio->m_pStateBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufState,
                                                             size, &voxelio->m_state,
                                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));

    cmdGen.submitAndWait(cmdBufState);


    // 步骤 12：虚拟屏幕输出影像缓冲（宽×高×波段个 float，初值 0）与叶面积密度 lad 缓冲。
    ///--------------------------------------------------------------------
    ///  Virtual Sceen properties
    ///--------------------------------------------------------------------
    /// storage
    VkCommandBuffer cmdBufStorage = cmdGen.createCommandBuffer();
    // 先检查输出影像的元素数不超过设备单个存储缓冲上限（见 base/outputsize.h），超限抛异常。
    const size_t outputSize = checkedOutputImageElements(voxellstio->m_physicalDevice, voxellstio->imageSize.x, voxellstio->imageSize.y, voxellstio->n_wave);
    std::vector<float> outputImage(outputSize, 0.0);
    virtualio->m_pBufferStorage = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufStorage, outputImage,
                                                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufStorage);


    VkCommandBuffer cmdBufLad = cmdGen.createCommandBuffer();
    surfio->m_pBufferLad = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufLad, surfio->lads, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufLad);


    // 收尾：释放上传用的暂存缓冲。
    // 注意：函数总是返回 false（疑似遗留写法）；调用方 voxeleb.cpp 未使用返回值。
    m_pAlloc->finalizeAndReleaseStaging();

    return false;

}



// 模块：destroy——销毁 createBuffer 创建的缓冲，辐射场缓冲由其专用分配器销毁并释放分配器。
// 注意：m_pFixedSpectralBuffer、m_pBufferWave、m_pBufferInstanceLink、m_pVoxelLinkBuffer、
//       m_pVoxelNanoBuffer 等在创建时有条件（表为空则不创建），因此销毁前都先判空，
//       未创建的缓冲直接跳过（避免解引用空指针）。
void Buffer::destroy(std::shared_ptr<VoxelebIO> &voxellstio){

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


    if (meshio->spectrals.size() > 0)
    {
        m_pAlloc->destroy(*(meshio->m_pBufferSpectral));
    }

    if (meshio->m_pFixedSpectralBuffer) m_pAlloc->destroy(*(meshio->m_pFixedSpectralBuffer));
//    m_pAlloc->destroy(*(voxelio->m_pSurfLBuffer));


    if (meshio->thermals.size() > 0) // no utilise the isTemperature
    {
        m_pAlloc->destroy(*(meshio->m_pBufferThermal));
    }
    m_pAlloc->destroy(*(voxelio->m_pTempeBuffer));

    if (meshio->canopies.size() > 0)
    {
        m_pAlloc->destroy(*(meshio->m_pBufferCanopy));
    }
    m_pAlloc->destroy(*(meshio->m_pBufferMeshLink));

    if (instanceio->m_pBufferInstanceLink) m_pAlloc->destroy(*(instanceio->m_pBufferInstanceLink));
    if (voxelio->m_pVoxelLinkBuffer) m_pAlloc->destroy(*(voxelio->m_pVoxelLinkBuffer));
    if (voxelio->m_pVoxelNanoBuffer) m_pAlloc->destroy(*(voxelio->m_pVoxelNanoBuffer));
    if (voxelio->m_pVoxelHexBuffer) m_pAlloc->destroy(*(voxelio->m_pVoxelHexBuffer));

    m_pAlloc->destroy(*(surfio->m_pBufferLad));

    m_pAlloc->destroy(*(voxellstio->m_pBufferSensor));
    if (voxellstio->m_pBufferWave) m_pAlloc->destroy(*(voxellstio->m_pBufferWave));
   // m_pAlloc->destroy(*(voxellstio->m_pBufferWaveset));
    m_pAlloc->destroy(*(voxellstio->m_pBufferLight));

    for (int i = 0; i < meshio->m_bufferMeshes.size(); i++)
    {
        m_pAlloc->destroy((meshio->m_bufferMeshes[i].vertexBuffer));
        if (meshio->m_bufferMeshes[i].indexBuffer.buffer) m_pAlloc->destroy((meshio->m_bufferMeshes[i].indexBuffer));
    }

    if (voxellstio->atomconds.size() > 0)
    {
        m_pAlloc->destroy(*(voxellstio->m_pBufferAtomcond));
    }

    m_pAlloc->destroy(*(voxelio->m_pDirBuffer));
    if (voxellstio->m_pScatteringStats) m_pAlloc->destroy(*voxellstio->m_pScatteringStats);
    voxellstio->m_radianceAllocator->destroy(*(voxelio->m_pRadsBuffer));
    voxellstio->m_radianceAllocator.reset();
    m_pAlloc->destroy(*(voxelio->m_pNetRadBuffer));
    m_pAlloc->destroy(*(voxelio->m_pPnetBuffer));
    m_pAlloc->destroy(*(virtualio->m_pBufferStorage));

    m_pAlloc->destroy(*(voxellstio->m_pMeteoBuffer));
    m_pAlloc->destroy(*(voxellstio->m_pBufferAero));

    m_pAlloc->destroy(*(voxelio->m_pRaaBuffer));
    m_pAlloc->destroy(*(meshio->m_pLeafBioBuffer));
    m_pAlloc->destroy(*(meshio->m_pSoilSetBuffer));
    m_pAlloc->destroy(*(meshio->m_pWaterSetBuffer));

    m_pAlloc->destroy(*(voxelio->m_pRssBuffer));
    m_pAlloc->destroy(*(voxelio->m_pAirBuffer));

    m_pAlloc->destroy(*(voxelio->m_pFluxBuffer));
    m_pAlloc->destroy(*(voxelio->m_pTLASTBuffer));
    if (voxelio->m_pSoilMemoryBuffer) m_pAlloc->destroy(*(voxelio->m_pSoilMemoryBuffer));
    m_pAlloc->destroy(*(voxelio->m_pStateBuffer));

    m_pAlloc->destroy(*voxellstio->m_pFluidVelocityA);
    m_pAlloc->destroy(*voxellstio->m_pFluidVelocityB);
    m_pAlloc->destroy(*voxellstio->m_pFluidScalarA);
    m_pAlloc->destroy(*voxellstio->m_pFluidScalarB);
    m_pAlloc->destroy(*voxellstio->m_pFluidLbmA);
    m_pAlloc->destroy(*voxellstio->m_pFluidLbmB);
    m_pAlloc->destroy(*voxellstio->m_pFluidDensity);
    m_pAlloc->destroy(*voxellstio->m_pFluidMeta);
    m_pAlloc->destroy(*voxellstio->m_pFluidParameters);

    //m_rtBuilder.destroy();
}


