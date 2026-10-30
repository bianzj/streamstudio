//
// Created by admin on 2024/1/28.
//

#include "buffer.h"
#include "base/outputsize.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>



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

    nvvk::CommandPool cmdGen(m_device, m_queue.familyIndex);

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
    voxelio->m_pTempeBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufTempe, voxelTempes,
                                                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufTempe);

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
    if (fluidCellCount == 0
        || fluidCellCount > std::numeric_limits<uint32_t>::max() / 19U) {
        throw std::runtime_error("D3Q19 fluid grid is too large for 32-bit population indexing");
    }
    voxellstio->fluidCellCount = fluidCellCount;
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

    const float direction = glm::radians(voxellstio->fluid.windDirection);
    const float initialWind = !voxellstio->meteos.empty()
        ? std::max(0.0f, voxellstio->meteos[initialNode].u) : 0.0f;
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

    const auto denseIndex = [&](int x, int y, int z) -> int64_t {
        if (x < 0 || y < 0 || z < 0 || x >= voxellstio->fluidGridSize.x
            || y >= voxellstio->fluidGridSize.y || z >= voxellstio->fluidGridSize.z) return -1;
        return static_cast<int64_t>(x) + static_cast<int64_t>(voxellstio->fluidGridSize.x)
            * (static_cast<int64_t>(z) + static_cast<int64_t>(voxellstio->fluidGridSize.z) * y);
    };
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
    const auto kindPriority = [](int kind) {
        if (kind == 1) return 4;
        if (kind == 3 || kind == 4) return 3;
        if (kind == 2) return 2;
        return 0;
    };
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


    ///--------------------------------------------------------------------
    ///  Scene properties
    ///--------------------------------------------------------------------
    // obj models buffer
    for (int kmodel = 0; kmodel < meshio->primMeshes.size(); kmodel++)
    {
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

    // rads
    VkCommandBuffer cmdBufRads = cmdGen.createCommandBuffer();
    const VkDeviceSize radianceBytes = static_cast<VkDeviceSize>(n_voxel) * DIFFUSENUM * sizeof(VoxelRad);
    auto& radianceAllocator = voxellstio->m_radianceAllocator;
    radianceAllocator = std::make_unique<nvvk::ResourceAllocatorDedicated>(
        m_device, voxellstio->m_physicalDevice);
    voxelio->m_pRadsBuffer = std::make_shared<nvvk::Buffer>(radianceAllocator->createBuffer(
        radianceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    if (!voxelio->m_pRadsBuffer->buffer) throw std::runtime_error("Cannot allocate voxel radiance buffer");
    vkCmdFillBuffer(cmdBufRads, voxelio->m_pRadsBuffer->buffer, 0, radianceBytes, 0);
    cmdGen.submitAndWait(cmdBufRads);
    voxellstio->setting.radianceAddress =
        nvvk::getBufferDeviceAddress(m_device, voxelio->m_pRadsBuffer->buffer);
    if (!voxellstio->setting.radianceAddress) throw std::runtime_error("Voxel radiance device address is null");
    std::cout << "RADIANCE_BUFFER\tbytes=" << radianceBytes
              << "\taddress=" << voxellstio->setting.radianceAddress << std::endl;

    {
        constexpr VkDeviceSize statsBytes = 4 * sizeof(ScatteringOrderStats);
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

///--------------------------------------------------------------------
    ///  aero properties
    ///--------------------------------------------------------------------
    // aero
    auto cmdBufAero = cmdGen.createCommandBuffer();
    voxellstio->m_pBufferAero = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(cmdBufAero, voxellstio->aeroconds,
                                                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    cmdGen.submitAndWait(cmdBufAero);

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
    const Meteo& firstMeteo = voxellstio->meteos[0];
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

    ///--------------------------------------------------------------------
    ///  Evapo properties
    ///--------------------------------------------------------------------
    auto cmdBufEvapo = cmdGen.createCommandBuffer();
    // flux
    // FIXED[低] 内存: 通量缓冲只分配不清零，显存内容未定义（先读后写会读到垃圾值甚至 NaN）；已修复：缓冲增加 TRANSFER_DST 用途，并在 cmdBufEvapo（与 tLast 上传同一次提交）中用 vkCmdFillBuffer 全零清零。
    voxelio->m_pFluxBuffer = std::make_shared<nvvk::Buffer>(m_pAlloc->createBuffer(static_cast<size_t>(n_voxel) * sizeof(VoxelHeatflux),
                                                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    if (!voxelio->m_pFluxBuffer->buffer) throw std::runtime_error("Cannot allocate voxel flux buffer");
    vkCmdFillBuffer(cmdBufEvapo, voxelio->m_pFluxBuffer->buffer, 0, VK_WHOLE_SIZE, 0);
    // tLast
    const float initialTemperatureC = initialTemperature - 273.15f;
    // FIXED[低] 数值: n_voxel * TLASTNUM 为 int 乘法，n_voxel > 2.68e8 时有符号溢出（UB）；已修复：元素总数改用 size_t 计算，分配前用回除校验确认未回绕（并拒绝负的 n_voxel），溢出时抛 std::overflow_error。
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

    // state
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

    return false;

}



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

    m_pAlloc->destroy(*(meshio->m_pFixedSpectralBuffer));
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

    m_pAlloc->destroy(*(instanceio->m_pBufferInstanceLink));
    m_pAlloc->destroy(*(voxelio->m_pVoxelLinkBuffer));
    m_pAlloc->destroy(*(voxelio->m_pVoxelNanoBuffer));
    if (voxelio->m_pVoxelHexBuffer) m_pAlloc->destroy(*(voxelio->m_pVoxelHexBuffer));

    m_pAlloc->destroy(*(surfio->m_pBufferLad));

    m_pAlloc->destroy(*(voxellstio->m_pBufferSensor));
    m_pAlloc->destroy(*(voxellstio->m_pBufferWave));
   // m_pAlloc->destroy(*(voxellstio->m_pBufferWaveset));
    m_pAlloc->destroy(*(voxellstio->m_pBufferLight));

    for (int i = 0; i < meshio->m_bufferMeshes.size(); i++)
    {
        m_pAlloc->destroy((meshio->m_bufferMeshes[i].vertexBuffer));
        m_pAlloc->destroy((meshio->m_bufferMeshes[i].indexBuffer));
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


