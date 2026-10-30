//
// Created by admin on 2024/1/25.
//

#include "projectjson.h"
#include "engine.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace {
std::filesystem::path executableDirectory()
{
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
    }
#endif
    return std::filesystem::current_path();
}
}


void Engine::init(Mode mode)
{
    if (mode == Mode::eFacetRT) {
        m_pFacetrt = std::make_shared<Facetrt>();
        m_pFacetrtio = std::make_shared<FacetrtIO>();
        return;
    }
    if (mode == Mode::eFacetEB) {
        m_pFaceteb = std::make_shared<Faceteb>();
        m_pFacetebio = std::make_shared<FacetebIO>();
        return;
    }

    int gpuIndex = 0;
    std::string backend = "gpu";
    if (m_pFileio) {
        const SettingXml* settings = nullptr;
        if (mode == Mode::eRaytracing && m_pFileio->m_pRaytracingXml)
            settings = &m_pFileio->m_pRaytracingXml->settingxml;
        else if (mode == Mode::eVoxelEB && m_pFileio->m_pVoxelebXml)
            settings = &m_pFileio->m_pVoxelebXml->settingxml;
        else if (mode == Mode::eVoxelRT && m_pFileio->m_pVoxelrtXml)
            settings = &m_pFileio->m_pVoxelrtXml->settingxml;
        if (settings) {
            gpuIndex = settings->theGPU;
            backend = settings->computeBackend;
        }
    }
    if (const char* environment = std::getenv("HISTREAM_COMPUTE_BACKEND"); environment && *environment)
        backend = environment;
    appSetting.init(gpuIndex, parseComputeBackend(backend));

    if(mode == Mode::eRaytracing) {
        m_pRaytracing = std::make_shared<Raytracing>();
        m_pRaytracingio = std::make_shared<RaytracingIO>();
    }else if(mode == Mode::eVoxelEB) {
        m_pVoxeleb = std::make_shared<Voxeleb>();
        m_pVoxelebio = std::make_shared<VoxelebIO>();
    }else if(mode == Mode::eVoxelRT) {
        m_pVoxelrt = std::make_shared<Voxelrt>();
        m_pVoxelrtio = std::make_shared<VoxelrtIO>();
    }
}


void Engine::input(std::string path, std::string V, std::string outputPath){

    if (V == "eRaytracing") {
        m_mode = Mode::eRaytracing;
    } else if (V == "eVoxelEB") {
        m_mode = Mode::eVoxelEB;
    } else if (V == "eVoxelRT") {
        m_mode = Mode::eVoxelRT;
    } else if (V == "eFacetRT") {
        m_mode = Mode::eFacetRT;
    } else if (V == "eFacetEB") {
        m_mode = Mode::eFacetEB;
    } else {
        throw std::runtime_error("Unknown mode: " + V);
    }

    m_inputPath = std::move(path);
    m_outputPath = std::move(outputPath);
    if (!m_inputPath.empty()) {
        const auto project = ProjectJson::load(m_inputPath);
        const auto& scene = project.scene();
        if (scene.contains("terrainEnvironment")) {
            const auto& env = scene["terrainEnvironment"];
            if (m_mode != Mode::eVoxelEB &&
                (ProjectJson::boolean(env, "airEnabled", false) ||
                 ProjectJson::boolean(env, "soilEnabled", false) ||
                 ProjectJson::number(env, "co2ValleyExcess", 0.0f) != 0.0f))
                throw std::runtime_error("TerrainEnvironment currently requires eVoxelEB");
        }
        if (m_mode != Mode::eRaytracing && project.hasSpecularReflection())
            throw std::runtime_error("Mesh specular reflection requires eRaytracing");
        // Default 32 threads, never more than this machine's logical processors.
        constexpr int kDefaultCpuThreads = 32;
        const int processors = static_cast<int>((std::max)(1U, std::thread::hardware_concurrency()));
        int cpuThreads = ProjectJson::integer(project.control(), "cpuThreads", kDefaultCpuThreads);
        if (cpuThreads < 1) cpuThreads = kDefaultCpuThreads;
        cpuThreads = (std::min)(cpuThreads, processors);
        const std::string value = std::to_string(cpuThreads);
        _putenv_s("HISTREAM_CPU_THREADS", value.c_str());
        _putenv_s("LP_NUM_THREADS", value.c_str());
    }
    if (m_mode == Mode::eFacetRT || m_mode == Mode::eFacetEB) {
        init(m_mode);
        if (m_mode == Mode::eFacetRT) {
            m_pFacetrtio->inputPath = m_inputPath;
            m_pFacetrtio->shaderDirectory = facetShaderDirectory("facetrt");
            m_pFacetrtio->outputPath = m_outputPath;
        } else {
            m_pFacetebio->inputPath = m_inputPath;
            m_pFacetebio->shaderDirectory = facetShaderDirectory("faceteb");
            m_pFacetebio->outputPath = m_outputPath;
        }
        return;
    }

    m_pFileio = std::make_shared<FileIO>();
    m_pFileio->readJson(m_inputPath, m_mode);
    init(m_mode);

    if(m_mode == Mode::eRaytracing) {
        m_pRaytracing->setup(appSetting, m_pRaytracingio);
        m_pRaytracing->upload(m_pFileio, m_pRaytracingio);

        m_pRaytracingio->definedDir = m_pFileio->m_pRaytracingXml->definedDir;
        m_pRaytracingio->projectDir = m_pFileio->m_pRaytracingXml->projectDir;
    }else if(m_mode == Mode::eVoxelEB){

        m_pVoxeleb->setup(appSetting, m_pVoxelebio);
        m_pVoxeleb->upload(m_pFileio, m_pVoxelebio);

        m_pVoxelebio->definedDir = m_pFileio->m_pVoxelebXml->definedDir;
        m_pVoxelebio->projectDir = m_pFileio->m_pVoxelebXml->projectDir;
    }else if(m_mode == Mode::eVoxelRT){
        m_pVoxelrt->setup(appSetting, m_pVoxelrtio);
        m_pVoxelrt->upload(m_pFileio, m_pVoxelrtio);

        m_pVoxelrtio->definedDir = m_pFileio->m_pVoxelrtXml->definedDir;
        m_pVoxelrtio->projectDir = m_pFileio->m_pVoxelrtXml->projectDir;
    }
    
}

bool Engine::create() {

    // FIXED[中] 未初始化: eRaytracing/eVoxelEB/eVoxelRT 分支直接解引用求解器指针，未调用 input()/init() 时为空指针崩溃（destroy() 已判空）；已修复：入口按模式判空求解器与 IO 指针，未初始化时抛出带说明的 runtime_error（run() 同改，main 已有异常捕获）。
    if (!((m_mode == Mode::eFacetRT && m_pFacetrt && m_pFacetrtio) ||
          (m_mode == Mode::eFacetEB && m_pFaceteb && m_pFacetebio) ||
          (m_mode == Mode::eRaytracing && m_pRaytracing && m_pRaytracingio) ||
          (m_mode == Mode::eVoxelEB && m_pVoxeleb && m_pVoxelebio) ||
          (m_mode == Mode::eVoxelRT && m_pVoxelrt && m_pVoxelrtio))) {
        throw std::runtime_error("Engine::create: engine not initialized, call input() first");
    }

    if (m_mode == Mode::eFacetRT) {
        if (!m_pFacetrt || !m_pFacetrt->setup(m_pFacetrtio)) return false;
        if (!m_pFacetrt->upload(m_pFacetrtio)) return false;
        return m_pFacetrt->create(m_pFacetrtio);
    }
    if (m_mode == Mode::eFacetEB) {
        if (!m_pFaceteb || !m_pFaceteb->setup(m_pFacetebio)) return false;
        if (!m_pFaceteb->upload(m_pFacetebio)) return false;
        return m_pFaceteb->create(m_pFacetebio);
    }
    if(m_mode == Mode::eRaytracing) {
        return m_pRaytracing->create(m_pRaytracingio);
    }else if(m_mode == Mode::eVoxelEB){
        return m_pVoxeleb->create(m_pVoxelebio);
    }else if(m_mode == Mode::eVoxelRT){
        return m_pVoxelrt->create(m_pVoxelrtio);
    }
    return false;
}

std::string Engine::facetShaderDirectory(const char* name) const
{
    const std::filesystem::path deployed = executableDirectory() / "shader" / name;
    // DEFERRED[中] 安全: path::string() 在 Windows 上按 ANSI 代码页转换，安装目录含代码页外字符时抛 std::system_error（或产生 '?' 路径），着色器加载失败；exists() 对权限错误也会抛异常。理由：需要把着色器目录整体改为 path/wstring 或 u8path（窄字符路径全链路改造），属本轮禁止改动范围。
    if (std::filesystem::exists(deployed)) {
        return deployed.string();
    }
    const std::filesystem::path local = std::filesystem::current_path() / "shader" / name;
    if (std::filesystem::exists(local)) {
        return local.string();
    }
#ifdef HISTREAM_FACETRT_SHADER_DIR
    if (std::string(name) == "facetrt") {
        return HISTREAM_FACETRT_SHADER_DIR;
    }
#endif
#ifdef HISTREAM_FACETEB_SHADER_DIR
    if (std::string(name) == "faceteb") {
        return HISTREAM_FACETEB_SHADER_DIR;
    }
#endif
    return local.string();
}

int Engine::run() {
    // FIXED[中] 未初始化: 求解器虽已用 && 短路判空，但 IO 指针未判空且未初始化时静默返回 1；已修复：入口按模式判空求解器与 IO 指针，未初始化时抛出带说明的 runtime_error（与 create() 一致）。
    // FIXED[中] 未初始化: run() 入口判空漏掉 m_pFileio，eRaytracing/eVoxelEB/eVoxelRT 分支把空 shared_ptr 传给求解器 run()（前轮补漏发现，原文件未标注）；已修复：这三个分支的守卫条件补 && m_pFileio，未初始化时抛出与 create() 一致的 runtime_error（create() 未使用 m_pFileio，无需改动）。
    if (!((m_mode == Mode::eFacetRT && m_pFacetrt && m_pFacetrtio) ||
          (m_mode == Mode::eFacetEB && m_pFaceteb && m_pFacetebio) ||
          (m_mode == Mode::eRaytracing && m_pRaytracing && m_pRaytracingio && m_pFileio) ||
          (m_mode == Mode::eVoxelEB && m_pVoxeleb && m_pVoxelebio && m_pFileio) ||
          (m_mode == Mode::eVoxelRT && m_pVoxelrt && m_pVoxelrtio && m_pFileio))) {
        throw std::runtime_error("Engine::run: engine not initialized, call input() first");
    }

    if (m_mode == Mode::eFacetRT) {
        return m_pFacetrt && m_pFacetrt->run(m_pFacetrtio) ? 0 : 1;
    }
    if (m_mode == Mode::eFacetEB) {
        if (!m_pFaceteb || !m_pFacetebio) return 1;
        m_pFaceteb->run(m_pFacetebio);
        return m_pFacetebio->exitCode;
    }
    if (m_mode == Mode::eRaytracing) {
        return m_pRaytracing && m_pRaytracing->run(m_pRaytracingio, m_pFileio) ? 0 : 1;
    }
    if (m_mode == Mode::eVoxelEB) {
        return m_pVoxeleb && m_pVoxeleb->run(m_pVoxelebio, m_pFileio) ? 0 : 1;
    }
    if (m_mode == Mode::eVoxelRT) {
        return m_pVoxelrt && m_pVoxelrt->run(m_pVoxelrtio, m_pFileio) ? 0 : 1;
    }
    return 1;
}

void Engine::destroy() {

    if (m_mode == Mode::eFacetRT) {
        if (m_pFacetrt) m_pFacetrt->destroy(m_pFacetrtio);
        return;
    }
    if (m_mode == Mode::eFacetEB) {
        if (m_pFaceteb) m_pFaceteb->destroy(m_pFacetebio);
        return;
    }
    // FIXED[低] 错误处理: 未判空直接解引用 m_pRaytracing/m_pVoxeleb/m_pVoxelrt，且 appSetting.destroy() 对未创建的 surface 调用 vkDestroySurfaceKHR；已修复：与 Facet 分支一致先判空，AppSetting::destroy() 仅在 m_surface 非空时销毁 surface。
    if(m_mode == Mode::eRaytracing) {
        if (m_pRaytracing) m_pRaytracing->destroy(m_pRaytracingio);
    }
    else if(m_mode == Mode::eVoxelEB) {
        if (m_pVoxeleb) m_pVoxeleb->destroy(m_pVoxelebio);
    }
    else if(m_mode == Mode::eVoxelRT) {
        if (m_pVoxelrt) m_pVoxelrt->destroy(m_pVoxelrtio);
    }
    appSetting.destroy();


}
