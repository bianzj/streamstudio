

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
constexpr int kExitUsage = 64;

void printUsage(const histream::ExtensionRegistry& extensions) {
    std::cerr << "Usage: histream <eVoxelEB|eFacetEB|eFacetRT|eVoxelRT|eRaytracing> <project.json> [output.json]\n"
                 "       histream <eVoxelEB|eVoxelRT|eRaytracing> --builtin-example\n"
                 "       histream --list-devices\n";
    for (const auto& extension : extensions.modes())
        std::cerr << "       " << extension.usage << '\n';
}
}

int main(int argc, char **argv) {

#ifdef _WIN32
    // This executable is a non-interactive worker. Report failures through
    // stderr/exit codes and suppress Windows crash/abort dialog boxes.
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
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");

    Engine engine;
    try {
        engine.input(inputPath, mode, outputPath);
    } catch (const std::exception& error) {
        std::cerr << "HiStream initialization failed: " << error.what() << '\n';
        return 4;
    }

    try {
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
