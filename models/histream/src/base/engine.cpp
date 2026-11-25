// ==========================================================================
// 文件：engine.cpp
// 功能：实现 Engine——按模式创建、初始化、运行和销毁 histream 的各后端，并在 input() 中
//       完成工程 JSON 的前置校验、CPU 线程数设置与数据上传。
// 主要模块：
//   1. executableDirectory：取可执行文件所在目录（Windows 用 GetModuleFileNameW）
//   2. Engine::init：创建后端/IO 对象；非 Facet 模式从工程设置与环境变量选定 GPU 与计算后端
//   3. Engine::input：解析模式 → 校验 → 设置线程环境变量 → FileIO 读 JSON → init → setup/upload
//   4. Engine::create / run / destroy：按模式分派到各后端
//   5. Engine::facetShaderDirectory：定位 FacetRT/FacetEB 着色器目录
// 数据流：命令行(模式, 工程路径, 输出路径) → input → FileIO(*Xml) → 后端 setup/upload → create → run
//       → 退出码。FacetRT/FacetEB 不经 FileIO，只记录路径，由后端自己解析工程。
// 约定：run() 返回 0 成功、1 失败；FacetEB 返回 exitCode（2 = 已完成但有节点未收敛，结果保留，
//       服务端据此把状态置为 non_converged）。
// 环境变量：HISTREAM_COMPUTE_BACKEND（覆盖计算后端）；input 会设置 HISTREAM_CPU_THREADS 与
//       LP_NUM_THREADS（Mesa llvmpipe 的线程数，待核对）。
// ==========================================================================

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
// 作用：返回当前可执行文件所在目录；非 Windows 或失败时退回当前工作目录。
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


// 作用：按 mode 创建对应后端对象与 IO 对象。
// 输入：mode；副作用：非 Facet 模式调用 appSetting.init(gpuIndex, 计算后端)。
// 说明：Facet 模式在此直接返回，不初始化 AppSetting；未编译 Facet 后端时请求 Facet 模式抛异常。
void Engine::init(Mode mode)
{
#if HISTREAM_ENABLE_FACET
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
#else
    if (mode == Mode::eFacetRT || mode == Mode::eFacetEB)
        throw std::runtime_error("This HiStream build has no Facet backends (HISTREAM_ENABLE_FACET=OFF)");
#endif

    // 步骤 1：确定 GPU 序号与计算后端：默认 0/"gpu"，已读入工程时取其 settingxml，
    //         最后允许环境变量 HISTREAM_COMPUTE_BACKEND 覆盖。
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


// 作用：解析模式并加载工程，准备好各后端的数据。
// 输入：path（工程 JSON）、V（模式字符串 eRaytracing/eVoxelEB/eVoxelRT/eFacetRT/eFacetEB）、
//       outputPath（输出目录，Facet 模式使用）。未知模式抛 std::runtime_error。
void Engine::input(std::string path, std::string V, std::string outputPath){

    // 步骤 1：模式字符串 → Mode 枚举。
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

    // 步骤 2：读取工程并做模式兼容性检查——地形环境（空气/土壤/CO2 谷地超额）仅支持 VoxelEB，
    //         网格镜面反射仅支持 RayTracing；随后设置 CPU 线程数。
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
        // 线程数：默认 32，工程 control.cpuThreads 小于 1 时回到默认，并限制不超过逻辑处理器数；
        // 通过环境变量传给后端和软件渲染库。_putenv_s 为 Windows CRT 函数（待核对跨平台可用性）。
        constexpr int kDefaultCpuThreads = 32;
        const int processors = static_cast<int>((std::max)(1U, std::thread::hardware_concurrency()));
        int cpuThreads = ProjectJson::integer(project.control(), "cpuThreads", kDefaultCpuThreads);
        if (cpuThreads < 1) cpuThreads = kDefaultCpuThreads;
        cpuThreads = (std::min)(cpuThreads, processors);
        const std::string value = std::to_string(cpuThreads);
        _putenv_s("HISTREAM_CPU_THREADS", value.c_str());
        _putenv_s("LP_NUM_THREADS", value.c_str());
    }
    // 步骤 3：Facet 模式只创建后端并记录输入/输出路径与着色器目录，由后端自行读取工程。
    if (m_mode == Mode::eFacetRT || m_mode == Mode::eFacetEB) {
        init(m_mode);
#if HISTREAM_ENABLE_FACET
        if (m_mode == Mode::eFacetRT) {
            m_pFacetrtio->inputPath = m_inputPath;
            m_pFacetrtio->shaderDirectory = facetShaderDirectory("facetrt");
            m_pFacetrtio->outputPath = m_outputPath;
        } else {
            m_pFacetebio->inputPath = m_inputPath;
            m_pFacetebio->shaderDirectory = facetShaderDirectory("faceteb");
            m_pFacetebio->outputPath = m_outputPath;
        }
#endif
        return;
    }

    // 步骤 4：其余模式由 FileIO 解析工程，再 init，然后 setup（Vulkan/设备）与 upload（场景数据上传），
    //         最后记录 definedDir/projectDir（预定义数据目录与工程目录）。
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

// 作用：创建所选后端的运行资源；未调用 input 时抛异常；失败返回 false。
//       Facet 后端此处才执行 setup/upload/create 全流程。
bool Engine::create() {

    if (!(
#if HISTREAM_ENABLE_FACET
          (m_mode == Mode::eFacetRT && m_pFacetrt && m_pFacetrtio) ||
          (m_mode == Mode::eFacetEB && m_pFaceteb && m_pFacetebio) ||
#endif
          (m_mode == Mode::eRaytracing && m_pRaytracing && m_pRaytracingio) ||
          (m_mode == Mode::eVoxelEB && m_pVoxeleb && m_pVoxelebio) ||
          (m_mode == Mode::eVoxelRT && m_pVoxelrt && m_pVoxelrtio))) {
        throw std::runtime_error("Engine::create: engine not initialized, call input() first");
    }

#if HISTREAM_ENABLE_FACET
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
#endif
    if(m_mode == Mode::eRaytracing) {
        return m_pRaytracing->create(m_pRaytracingio);
    }else if(m_mode == Mode::eVoxelEB){
        return m_pVoxeleb->create(m_pVoxelebio);
    }else if(m_mode == Mode::eVoxelRT){
        return m_pVoxelrt->create(m_pVoxelrtio);
    }
    return false;
}

// 作用：查找 Facet 着色器目录（name 为 "facetrt" 或 "faceteb"）。
// 顺序：可执行文件目录/shader/name → 当前目录/shader/name → 编译期宏目录 → 当前目录路径兜底。
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

// 作用：执行所选后端，返回进程退出码（0 成功，1 失败；FacetEB 返回其 exitCode，含 2 = 未收敛）。
int Engine::run() {
    if (!(
#if HISTREAM_ENABLE_FACET
          (m_mode == Mode::eFacetRT && m_pFacetrt && m_pFacetrtio) ||
          (m_mode == Mode::eFacetEB && m_pFaceteb && m_pFacetebio) ||
#endif
          (m_mode == Mode::eRaytracing && m_pRaytracing && m_pRaytracingio && m_pFileio) ||
          (m_mode == Mode::eVoxelEB && m_pVoxeleb && m_pVoxelebio && m_pFileio) ||
          (m_mode == Mode::eVoxelRT && m_pVoxelrt && m_pVoxelrtio && m_pFileio))) {
        throw std::runtime_error("Engine::run: engine not initialized, call input() first");
    }

#if HISTREAM_ENABLE_FACET
    if (m_mode == Mode::eFacetRT) {
        return m_pFacetrt && m_pFacetrt->run(m_pFacetrtio) ? 0 : 1;
    }
    if (m_mode == Mode::eFacetEB) {
        if (!m_pFaceteb || !m_pFacetebio) return 1;
        m_pFaceteb->run(m_pFacetebio);
        // 0 = converged, 2 = finished with non-converged nodes (results kept;
        // the server maps only this FacetEB code to status non_converged), 1 = failed.
        return m_pFacetebio->exitCode;
    }
#endif
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

// 作用：销毁所选后端并释放 AppSetting。
// 注意：Facet 模式提前返回，不调用 appSetting.destroy()（因为 init 时未初始化它）。
void Engine::destroy() {

#if HISTREAM_ENABLE_FACET
    if (m_mode == Mode::eFacetRT) {
        if (m_pFacetrt) m_pFacetrt->destroy(m_pFacetrtio);
        return;
    }
    if (m_mode == Mode::eFacetEB) {
        if (m_pFaceteb) m_pFaceteb->destroy(m_pFacetebio);
        return;
    }
#endif
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
