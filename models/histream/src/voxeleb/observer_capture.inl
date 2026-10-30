// Included by voxeleb.cpp. Reuse saved thermal states without advancing energy balance.
namespace {
template<class T>
std::vector<T> observerRead(const std::filesystem::path& path, size_t count) {
    if (std::filesystem::file_size(path) != count * sizeof(T))
        throw std::runtime_error("Observer state size mismatch: " + path.string());
    std::vector<T> data(count);
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(count * sizeof(T)));
    if (!file) throw std::runtime_error("Cannot read observer state: " + path.string());
    return data;
}

void observerUpload(const std::shared_ptr<VoxelebIO>& io, const nvvk::Buffer& destination,
                    const void* data, VkDeviceSize bytes) {
    nvvk::Buffer staging = io->m_pAlloc->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped = io->m_pAlloc->map(staging);
    std::memcpy(mapped, data, static_cast<size_t>(bytes));
    io->m_pAlloc->unmap(staging);
    nvvk::CommandPool pool(io->m_device, io->m_queueIndex);
    VkCommandBuffer cmd = pool.createCommandBuffer();
    VkBufferCopy copy{0, 0, bytes};
    vkCmdCopyBuffer(cmd, staging.buffer, destination.buffer, 1, &copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = destination.buffer;
    barrier.size = bytes;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    pool.submitAndWait(cmd);
    io->m_pAlloc->destroy(staging);
}
}

bool Voxeleb::runObserver(std::shared_ptr<VoxelebIO>& io, std::shared_ptr<FileIO>& fileio,
                         const std::string& manifestPath) {
    using Json = ProjectJson::Json;
    std::ifstream input(std::filesystem::u8path(manifestPath));
    Json manifest;
    input >> manifest;
    if (manifest.at("format") != "stream3d-native-observer-v1" ||
        fileio->m_pVoxelebXml->sensorxml.projection != Projection::PERSPECTIVE)
        throw std::runtime_error("Observer requires a v1 manifest and perspective sensor");
    const auto root = std::filesystem::u8path(manifest.at("sourceRoot").get<std::string>());
    const size_t count = static_cast<size_t>(io->n_voxel);
    if (manifest.at("voxelCount").get<size_t>() != count)
        throw std::runtime_error("Observer source scene voxel count differs");
    const float scale = io->stepsize_surface;
    if (std::abs(manifest.at("voxelSizeM").get<float>() - scale) > 0.0001f)
        throw std::runtime_error("Observer voxel scale differs");
    // Validate EVERY row, including wall-face slots, before uploading temperatures.
    {
        const auto links = observerRead<int32_t>(root / "output/process/voxel_scene_links.bin", count * 6);
        const auto positions = observerRead<float>(root / manifest.at("positionFile").get<std::string>(), count * 8);
        for (size_t i = 0; i < count; ++i) {
            const auto& link = io->m_voxelio->voxellinks.at(i);
            const auto& instance = io->m_instanceio->instanceLinks.at(link.instanceId);
            const auto& mesh = io->m_meshio->meshLinks.at(instance.meshId);
            const int32_t expected[] = {link.instanceId, static_cast<int32_t>(instance.meshId),
                mesh.spectralId, mesh.canopyId, mesh.type, link.faceId};
            for (int j = 0; j < 6; ++j)
                if (links[i * 6 + j] != expected[j])
                    throw std::runtime_error("Observer material/face row order differs");
            for (int j = 0; j < 3; ++j)
                if (std::abs(positions[i * 8 + j] - (link.voxelId[j] + 0.5f) * scale) > 0.001f)
                    throw std::runtime_error("Observer voxel position order differs");
        }
    }
    std::filesystem::create_directories(io->projectDir);
    std::ofstream records(std::filesystem::path(io->projectDir) / "capture_records.jsonl");
    if (!records) throw std::runtime_error("Cannot create observer capture records");
    int frameIndex = 0;
    for (const auto& state : manifest.at("states")) {
        const int node = state.at("meteoNode").get<int>();
        if (node < 0 || node >= static_cast<int>(io->meteos.size()))
            throw std::runtime_error("Observer meteo node out of bounds");
        io->k_node = node;
        updateMeteo(io, node);
        m_pGeometry->updateAngle(io, 0);
        {
            const auto saved = observerRead<float>(root / state.at("thermalState").get<std::string>(), count * 3);
            std::vector<VoxelTempe> temperatures(count);
            std::vector<VoxelDir> directions(count);
            for (size_t i = 0; i < count; ++i) {
                const float sun = saved[3 * i], shade = saved[3 * i + 1], fraction = saved[3 * i + 2];
                if (!std::isfinite(sun) || !std::isfinite(shade) || !std::isfinite(fraction) ||
                    sun < 150 || sun > 500 || shade < 150 || shade > 500 || fraction < 0 || fraction > 1)
                    throw std::runtime_error("Invalid observer thermal state");
                temperatures[i] = {sun, shade};
                directions[i] = {fraction, 1.0f};
            }
            observerUpload(io, *io->m_voxelio->m_pTempeBuffer, temperatures.data(), count * sizeof(VoxelTempe));
            observerUpload(io, *io->m_voxelio->m_pDirBuffer, directions.data(), count * sizeof(VoxelDir));
        }
        // Recompute incident environmental longwave using the restored temperatures.
        // No budget, evaporation, temperature-update or fluid steps are executed.
        const glm::ivec3 dispatch((io->n_voxel + GROUP_SIZEX - 1) / GROUP_SIZEX, 1, 1);
        m_pCommand->submit(io, VoxelEBStage::directTIR, dispatch, std::nullopt, std::nullopt);
        m_pCommand->waitFence(io);
        m_pCommand->submit(io, VoxelEBStage::diffuseTIR, dispatch, std::nullopt, std::nullopt);
        m_pCommand->waitFence(io);
        for (auto record : state.at("frames")) {
            auto p = record.at("positionNEU_m").get<std::vector<float>>();
            if (p.size() != 3 || !std::isfinite(p[0]) || !std::isfinite(p[1]) ||
                !std::isfinite(p[2]) || p[2] <= 0 || p[0] < 0 || p[1] < 0 ||
                p[0] >= io->sceneSize_XYZ.x || p[1] >= io->sceneSize_XYZ.y)
                throw std::runtime_error("Invalid observer camera position");
            auto& angle = io->angles.front();
            angle.vza = record.at("vza").get<float>();
            angle.vaa = record.at("vaa").get<float>();
            if (!std::isfinite(angle.vza) || !std::isfinite(angle.vaa) || angle.vza < 0 || angle.vza > 180)
                throw std::runtime_error("Invalid observer camera angles");
            const float zenith = glm::radians(angle.vza), azimuth = glm::radians(angle.vaa);
            const glm::vec3 out(std::sin(zenith) * std::cos(azimuth), std::cos(zenith),
                                std::sin(zenith) * std::sin(azimuth));
            const glm::vec3 position(p[0] / scale - io->voxelSize_XZY.x * 0.5f,
                                     p[2] / scale, p[1] / scale - io->voxelSize_XZY.z * 0.5f);
            glm::vec3 up(0, 1, 0);
            if (std::abs(out.y) > 0.99999f) up = glm::vec3(1, 0, 0);
            SensorMatrix sensor = m_pGeometry->createSensor(position, position - out * 100.0f,
                io->voxelSize_XZY, fileio->m_pVoxelebXml->sensorxml.sensorFov, up);
            m_pGeometry->updateSensor(io, sensor);
            io->uavposes.emplace_back(p[0], p[1], p[2]);
            m_pCommand->runRT(io);
            output(io, fileio, node, 0, static_cast<int>(io->uavposes.size()) - 1);
            record["nativePositionIndex"] = io->uavposes.size() - 1;
            record["meteoNode"] = node;
            record["hourBJT"] = state.at("hourBJT");
            record["incomingShortwaveWm2"] = io->meteo.Rin;
            record["solarZenithDeg"] = angle.sza;
            record["thermalState"] = state.at("thermalState");
            record["nativeViewInverseRows"] = Json::array();
            record["nativeProjectionInverseRows"] = Json::array();
            for (int row = 0; row < 4; ++row) {
                record["nativeViewInverseRows"].push_back({sensor.viewInverse[0][row], sensor.viewInverse[1][row],
                    sensor.viewInverse[2][row], sensor.viewInverse[3][row]});
                record["nativeProjectionInverseRows"].push_back({sensor.projInverse[0][row], sensor.projInverse[1][row],
                    sensor.projInverse[2][row], sensor.projInverse[3][row]});
            }
            records << record.dump() << '\n';
            records.flush();
            if (!records) throw std::runtime_error("Cannot save observer camera record");
            std::cout << "CAPTURE\t" << ++frameIndex << "\t" << record.at("episode").get<std::string>() << std::endl;
        }
    }
    std::cout << "PROGRESS\t100\tObserver capture complete: " << frameIndex << std::endl;
    return true;
}
