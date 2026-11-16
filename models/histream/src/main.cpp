

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
#ifdef HISTREAM_ENABLE_SCENE_ANIMATION
#include "animation/scene_animation.h"
#endif
#include <filesystem>
#include <vector>


using namespace std;

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

    std::string mode = argc > 1 ? argv[1] : "eVoxelEB";
    if (mode == "eSceneAnimation") {
#ifndef HISTREAM_ENABLE_SCENE_ANIMATION
        std::cerr << "HiStream scene animation module is not enabled; build with HISTREAM_ENABLE_SCENE_ANIMATION=ON\n";
        return 2;
#else
        if (argc != 4) {
            std::cerr << "Usage: histream eSceneAnimation animation.json new-output-directory\n";
            return 2;
        }
        try {
            std::filesystem::path executable;
#ifdef _WIN32
            std::vector<wchar_t> name(32768,L'\0');
            const auto length=GetModuleFileNameW(nullptr,name.data(),static_cast<DWORD>(name.size()));
            if(!length || length>=name.size()) throw std::runtime_error("Cannot resolve HiStream executable path");
            executable=std::wstring(name.data(),length);
#else
            std::error_code ec;
            executable=std::filesystem::read_symlink("/proc/self/exe",ec);
            if(ec) executable=std::filesystem::absolute(argv[0]);
#endif
            return runSceneAnimation(argv[2], argv[3], executable.parent_path() / "shader/animation/scene.comp.spv", executable);
        } catch (const std::exception& error) {
            std::cerr << "HiStream GPU scene animation failed: " << error.what() << '\n';
            return 1;
        }
#endif
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
        std::cerr << "Usage: histream [eVoxelEB|eFacetEB|eFacetRT|eVoxelRT|eRaytracing] [project.json] [output.json]\n"
                     "       histream --list-devices\n"
                     "       histream eSceneAnimation animation.json new-output-directory\n";
        return 2;
    }

    // Engine path strings are native code-page strings (std::filesystem::path::string()),
    // so GDAL must interpret file names the same way in every module.
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");

    // DEFERRED[低] 错误处理: 默认模式 eVoxelEB 且未给工程路径时 inputPath 为空，Engine::input 仍会 readJson("")，报出含义不清的文件错误而不是用法提示；建议：inputPath 为空时直接打印 Usage 并返回 2。
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
