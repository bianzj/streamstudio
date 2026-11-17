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
