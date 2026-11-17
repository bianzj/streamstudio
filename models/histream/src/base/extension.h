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

struct ExtensionMode {
    // First command-line argument that selects this mode, e.g. "eExample".
    std::string name;
    // Usage line shown by `histream` without "Usage: ", e.g. "histream eExample input.json out".
    std::string usage;
    // Runs the mode and returns the process exit code. argv is the full command line.
    std::function<int(int argc, char** argv, const std::filesystem::path& executable)> run;
};

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
