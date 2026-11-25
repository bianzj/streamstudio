// ==========================================================================
// 文件：extension.cpp
// 功能：实现 histream::executablePath——返回正在运行的 histream 可执行文件的绝对路径，
//       供扩展模式在可执行文件旁查找运行时资源。
// 实现方式：
//   1. Windows：GetModuleFileNameW（缓冲 32768 个宽字符），失败或被截断时抛 std::runtime_error
//   2. 其他平台：读取符号链接 /proc/self/exe；失败时退回 absolute(argv0)（argv0 为空用 "histream"）
// 与其他文件关系：声明见 extension.h。
// ==========================================================================

#include "base/extension.h"

#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace histream {

// 作用：求可执行文件绝对路径。输入：argv0（仅在非 Windows 且 /proc/self/exe 不可用时使用）。
std::filesystem::path executablePath(const char* argv0) {
#ifdef _WIN32
    std::vector<wchar_t> name(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    if (!length || length >= name.size()) throw std::runtime_error("Cannot resolve HiStream executable path");
    return std::filesystem::path(std::wstring(name.data(), length));
#else
    std::error_code error;
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) path = std::filesystem::absolute(argv0 ? argv0 : "histream");
    return path;
#endif
}

}  // namespace histream
