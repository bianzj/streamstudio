// ==========================================================================
// 文件：extension.h
// 功能：histream 的“树外扩展”注册机制：声明扩展模式描述 ExtensionMode、注册表 ExtensionRegistry，
//       以及由 CMake 生成的 registerExtensions() 和取可执行文件路径的 executablePath()。
// 主要模块：
//   1. ExtensionMode：扩展模式名、用法行、运行回调
//   2. ExtensionRegistry：顺序保存模式，按名称查找
//   3. registerExtensions / executablePath 声明
// 与其他文件关系：extension.cpp 实现 executablePath；registerExtensions 由配置期生成的
//       build/generated/histream_extensions.cpp 提供；主程序启动时调用，核心求解器不依赖扩展代码。
// 约定：没有扩展时注册表为空；find 找不到返回 nullptr。
// ==========================================================================

#pragma once
// Out-of-tree HiStream extensions.
//
// The core engine knows no extension by name. A build may add extension directories with
// -DHISTREAM_EXTENSION_DIRS=<dir;...>; each directory provides histream-extension.cmake,
// links its code into histream and lists a registrar function in HISTREAM_EXTENSION_REGISTRARS.
// CMake then generates registerExtensions(), which calls every registrar once at start-up.
// Core solvers never depend on extension code; without extensions the registry is empty.

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace histream {

// 一个扩展模式：name 为命令行第一个参数；usage 为用法提示；run 接收完整命令行与可执行文件路径，
// 返回进程退出码。
struct ExtensionMode {
    // First command-line argument that selects this mode, e.g. "eExample".
    std::string name;
    // Usage line shown by `histream` without "Usage: ", e.g. "histream eExample input.json out".
    std::string usage;
    // Runs the mode and returns the process exit code. argv is the full command line.
    std::function<int(int argc, char** argv, const std::filesystem::path& executable)> run;
};

// 扩展注册表：add 追加模式（不检查重名），find 返回第一个同名模式，modes 返回全部。
class ExtensionRegistry {
public:
    void add(ExtensionMode mode) { modes_.push_back(std::move(mode)); }
    const ExtensionMode* find(const std::string& name) const {
        for (const auto& mode : modes_)
            if (mode.name == name) return &mode;
        return nullptr;
    }
    const std::vector<ExtensionMode>& modes() const { return modes_; }

private:
    std::vector<ExtensionMode> modes_;
};

// Generated at configure time (build/generated/histream_extensions.cpp).
void registerExtensions(ExtensionRegistry& registry);

// Absolute path of the running histream executable (extensions resolve their runtime resources
// next to it).
std::filesystem::path executablePath(const char* argv0);

}  // namespace histream
