// ==========================================================================
// 文件：main.cpp
// 功能：histream 可执行程序入口：解析命令行、分派模式，并按固定顺序驱动 Engine 的生命周期。
// 主要模块/步骤：
//   1. 退出码约定与 printUsage（匿名命名空间）
//   2. main 步骤 1：Windows 下关闭崩溃/中止对话框（本程序是无交互的后台 worker）
//   3. main 步骤 2：模式分派——扩展模式（ExtensionRegistry）、--list-devices、内置模式
//   4. main 步骤 3：校验模式名与工程路径（--builtin-example 仅 Voxel/Raytracing 模式）
//   5. main 步骤 4：Engine.input → create → run → destroy，各阶段异常映射为不同退出码
// 命令行：histream <eVoxelEB|eFacetEB|eFacetRT|eVoxelRT|eRaytracing> <project.json> [output.json]
// 数据流：server.mjs 以子进程启动本程序 → 读取 project.json → 引擎计算 → 写 output.json（输出路径可选）。
// 退出码：0 成功；1 运行失败；2 FacetEB 有未收敛节点（结果保留）；3 资源创建失败；4 初始化失败；
//         5 设备查询失败；64 用法错误（与 server.mjs 的 HISTREAM_EXIT 一致）。
// 与其他文件：engine.h（Engine）、base/extension.h（扩展模式注册）、base/appsetting.cpp（--list-devices）。
// ==========================================================================



#pragma once
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <crtdbg.h>
#endif


#include <vulkan/vulkan.hpp>
#include <cpl_conv.h>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE


#include "src/base/engine.h"
#include "base/extension.h"
#include <filesystem>
#include <vector>


using namespace std;

namespace {
// Process exit codes (server.mjs HISTREAM_EXIT mirrors these):
//   0 success, 1 run failed, 2 FacetEB finished with non-converged nodes
//   (results kept), 3 resource creation failed, 4 initialization failed,
//   5 device query failed, 64 usage error (EX_USAGE).  Usage errors used to
//   share code 2 with "non-converged", so the server could not tell them apart.
// （用法错误使用 64，是为了与“FacetEB 未收敛=2”区分开。）
constexpr int kExitUsage = 64;

// 作用：向 stderr 打印用法，并追加各扩展模式自己声明的 usage 行。
void printUsage(const histream::ExtensionRegistry& extensions) {
    std::cerr << "Usage: histream <eVoxelEB|eFacetEB|eFacetRT|eVoxelRT|eRaytracing> <project.json> [output.json]\n"
                 "       histream <eVoxelEB|eVoxelRT|eRaytracing> --builtin-example\n"
                 "       histream --list-devices\n";
    for (const auto& extension : extensions.modes())
        std::cerr << "       " << extension.usage << '\n';
}
}

// 函数：main。输入：argv[1]=模式，argv[2]=工程 JSON 路径，argv[3]=可选输出路径。
// 返回：见文件头的退出码约定。
int main(int argc, char **argv) {

#ifdef _WIN32
    // This executable is a non-interactive worker. Report failures through
    // stderr/exit codes and suppress Windows crash/abort dialog boxes.
    // 步骤 1：作为后台 worker，不弹出系统错误框，失败只通过 stderr 与退出码报告。
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    // const InputParser parser(argc, argv);
    // const string path = parser.getString("-w", "");     //// 路径，单独运行时的文件夹
    // const string version = parser.getString("-v", "");  ////
    // std::string filePath;
    // if (version.empty())
    // {
    //     cout << "Wrong version input" << endl;
    //     return true;
    // }
    // if (!path.empty())
    // {
    // }


    // std::string version = "eVoxeleb";
    // std::string path= "D://data//hyperSpectral//test_1202";
    // std::string filePath;

    // 步骤 2：确定运行模式并先处理扩展模式、空参数与设备查询。
    std::string mode = argc > 1 ? argv[1] : "";
    // Out-of-tree extension modes (none in the core build; see src/base/extension.h).
    histream::ExtensionRegistry extensions;
    histream::registerExtensions(extensions);
    if (mode.empty()) {
        // No arguments: print usage instead of silently running a built-in example.
        printUsage(extensions);
        return kExitUsage;
    }
    if (const auto* extension = extensions.find(mode)) {
        try {
            return extension->run(argc, argv, histream::executablePath(argv[0]));
        } catch (const std::exception& error) {
            std::cerr << "HiStream " << mode << " failed: " << error.what() << '\n';
            return 1;
        }
    }
    // --list-devices：输出一行 HISTREAM_DEVICES<TAB>JSON，供服务端枚举可用 GPU/CPU 设备。
    if (mode == "--list-devices") {
        try {
            std::cout << "HISTREAM_DEVICES\t" << AppSetting::describeDevices() << std::endl;
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "HiStream device query failed: " << error.what() << '\n';
            return 5;
        }
    }
    // DEFERRED[中] 安全: Windows 下 argv 为 ANSI 代码页窄字符串，工程/输出路径中含当前代码页无法表示的字符（如英文系统上的中文目录）时会被替换成 '?' 无法恢复，后续所有文件打开失败；建议：用 wmain 或 GetCommandLineW+CommandLineToArgvW 取宽字符参数，内部统一用 std::filesystem::path 传递。
    // 步骤 3：内置模式的参数校验——模式名、工程路径与 --builtin-example。
    std::string inputPath = argc > 2 ? argv[2] : "";
    std::string outputPath = argc > 3 ? argv[3] : "";
    if (mode != "eVoxelEB" && mode != "eFacetEB" && mode != "eFacetRT"
        && mode != "eVoxelRT" && mode != "eRaytracing") {
        printUsage(extensions);
        return kExitUsage;
    }
    // An empty project path used to fall through to FileIO's built-in XML
    // example, silently writing into the example directory and exiting 0.
    // Keep that only behind the explicit --builtin-example switch (Voxel/
    // Raytracing modes; Facet modes have no built-in example).
    if (inputPath == "--builtin-example") {
        if (mode == "eFacetEB" || mode == "eFacetRT") {
            std::cerr << "--builtin-example is not available for " << mode << '\n';
            return kExitUsage;
        }
        inputPath.clear();
    } else if (inputPath.empty()) {
        std::cerr << "HiStream: project.json path is required for " << mode << '\n';
        printUsage(extensions);
        return kExitUsage;
    }

    // Engine path strings are native code-page strings (std::filesystem::path::string()),
    // so GDAL must interpret file names the same way in every module.
    // 步骤 4：创建并驱动引擎。先设置 GDAL 文件名按本地代码页解释，与引擎内路径字符串一致。
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");

    Engine engine;
    try {
        // 4a 读取工程并解析（失败 → 退出码 4）。
        engine.input(inputPath, mode, outputPath);
    } catch (const std::exception& error) {
        std::cerr << "HiStream initialization failed: " << error.what() << '\n';
        return 4;
    }

    try {
        // 4b 创建 GPU 资源（失败 → 退出码 3）。
        if (!engine.create()) {
            std::cerr << "Failed to create engine resources for " << mode << '\n';
            engine.destroy();
            return 3;
        }
    } catch (const std::exception& error) {
        // Partially created GPU resources are left to process teardown.
        std::cerr << "HiStream resource creation failed: " << error.what() << '\n';
        return 3;
    }

    // 4c 运行计算（run 的返回值即退出码，异常按 1 处理），随后释放资源；
    //     若运行成功但清理失败，也记为失败（1）。
    int runCode = 1;
    try {
        runCode = engine.run();
    } catch (const std::exception& error) {
        std::cerr << "HiStream run failed: " << error.what() << '\n';
        runCode = 1;
    }
    try {
        engine.destroy();
    } catch (const std::exception& error) {
        std::cerr << "HiStream cleanup failed: " << error.what() << '\n';
        if (runCode == 0) runCode = 1;
    }
    return runCode;
}
