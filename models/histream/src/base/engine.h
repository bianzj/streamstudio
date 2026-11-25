// ==========================================================================
// 文件：engine.h
// 功能：声明 Engine——histream 可执行程序的顶层调度器，按模式（Mode）创建并驱动
//       RayTracing / VoxelEB / VoxelRT / FacetRT / FacetEB 五种后端。
// 主要模块：
//   1. 条件编译：HISTREAM_ENABLE_FACET（默认 1）决定是否编译 Facet 后端
//   2. Engine 公共接口：input（解析模式与工程）→ init（建立后端对象）→ create（建 Vulkan/CPU 资源）
//      → run（执行并返回退出码）→ destroy（释放）
//   3. 私有成员：当前模式、输入/输出路径、AppSetting（GPU/后端选择）、FileIO，
//      以及每种模式各一对 后端对象 + IO 数据对象
// 与其他文件关系：engine.cpp 实现；main 程序按命令行调用；各后端见 src/raytracing、src/voxeleb、
//       src/voxelrt、src/facetrt、src/faceteb。
// 约定：run() 返回进程退出码，0 成功，1 失败；FacetEB 额外用 2 表示“已完成但有节点未收敛”。
// ==========================================================================

//
// Created by admin on 2024/1/25.
//

#ifndef FIELD_ENGINE_H
#define FIELD_ENGINE_H

#include "src/raytracing/raytracingio.h"
#include "src/raytracing/raytracing.h"
#include "src/voxeleb/voxeleb.h"
#include "src/voxeleb/voxelebio.h"
#include "src/voxelrt/voxelrt.h"
#include "src/base/appsetting.h"
#include "src/base/fileio.h"
// CMake defines HISTREAM_ENABLE_FACET=0 when the Facet backends are not compiled
// (option HISTREAM_ENABLE_FACET=OFF); other build setups keep them enabled.
#ifndef HISTREAM_ENABLE_FACET
#define HISTREAM_ENABLE_FACET 1
#endif
#if HISTREAM_ENABLE_FACET
#include "../facetrt/facetrt.h"
#include "../faceteb/faceteb.h"
#endif

#include <filesystem>
#include <utility>




// 类：Engine。调用顺序为 input → create → run → destroy（init 由 input 内部调用）。
class Engine {
public:
    Engine(){};

    // 解析模式字符串 V（eRaytracing/eVoxelEB/eVoxelRT/eFacetRT/eFacetEB）与工程 JSON 路径 path，
    // 校验与模式相关的限制、设置 CPU 线程环境变量，并创建/上传各后端数据；outputPath 可为空。
    void input(std::string path, std::string V, std::string outputPath = {});
    // 按模式创建后端对象与 IO 对象；非 Facet 模式还会初始化 AppSetting（GPU 序号与计算后端）。
    void init(Mode mode);
    // 创建所选后端的运行资源（流水线、缓冲等）；返回 false 表示失败。
    bool create();
    // 执行仿真，返回进程退出码（0 成功，1 失败，FacetEB 另有 2 = 有节点未收敛）。
    int run();
    // 释放所选后端与 AppSetting 资源。
    void destroy();

private:
    // 按优先级查找 Facet 着色器目录：可执行文件旁 shader/<name>，当前目录 shader/<name>，
    // 编译期宏给出的目录。
    std::string facetShaderDirectory(const char* name) const;

    // 当前运行模式与输入/输出路径。
    Mode m_mode{};
    std::string m_inputPath;
    std::string m_outputPath;
    // GPU 序号与计算后端设置，由 init 初始化、destroy 释放。
    AppSetting appSetting;
    std::shared_ptr<FileIO>       m_pFileio;
    // 各模式的后端对象（m_pXxx）与数据对象（m_pXxxio）成对出现，仅当前模式对应的一对非空。
    std::shared_ptr<Raytracing>   m_pRaytracing;
    std::shared_ptr<RaytracingIO> m_pRaytracingio;
    std::shared_ptr<Voxeleb>    m_pVoxeleb;
    std::shared_ptr<VoxelebIO>  m_pVoxelebio;
    std::shared_ptr<Voxelrt>    m_pVoxelrt;
    std::shared_ptr<VoxelrtIO>  m_pVoxelrtio;
#if HISTREAM_ENABLE_FACET
    std::shared_ptr<Facetrt>    m_pFacetrt;
    std::shared_ptr<FacetrtIO>  m_pFacetrtio;
    std::shared_ptr<Faceteb>    m_pFaceteb;
    std::shared_ptr<FacetebIO>  m_pFacetebio;
#endif

};


#endif //FIELD_ENGINE_H
