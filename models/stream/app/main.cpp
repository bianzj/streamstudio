// stream: STREAM remote-sensing image simulation for Stream Studio.
//   stream run <config.json>
//   stream era5-register <pack.json> [--product auto|era5|era5-land] <file.nc>...
#include "image.h"
#include "../forcing/era5.h"
#include "../scene/raster.h"

#include <cpl_conv.h>
#include <gdal_priv.h>
#include <ogr_srs_api.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

// PROJ looks for proj.db in its build-time prefix, which does not exist on a customer machine; use the copy
// deployed next to the executable.
void usePackagedProj()
{
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) return;
    buffer.resize(length);
    const std::filesystem::path directory = std::filesystem::path(buffer).parent_path() / "proj";
    if (!std::filesystem::is_regular_file(directory / "proj.db")) return;
    const std::string path = stream::utf8FromPath(directory);
    const char* const paths[] = {path.c_str(), nullptr};
    OSRSetPROJSearchPaths(paths);
#endif
}

std::string utf8FromWide(const wchar_t* text)
{
    std::string out;
    for (const wchar_t* p = text; *p; ++p) {
        uint32_t code = static_cast<uint16_t>(*p);
        if (code >= 0xD800 && code <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF) {
            code = 0x10000 + ((code - 0xD800) << 10) + (static_cast<uint16_t>(p[1]) - 0xDC00);
            ++p;
        }
        if (code < 0x80) out += static_cast<char>(code);
        else if (code < 0x800) { out += static_cast<char>(0xC0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3F)); }
        else if (code < 0x10000) { out += static_cast<char>(0xE0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F)); }
    }
    return out;
}

int usage()
{
    std::cerr << "usage: stream run <config.json>\n"
                 "       stream era5-register <pack.json> [--product auto|era5|era5-land] <file.nc>...\n";
    return 2;
}

int dispatch(const std::vector<std::string>& args)
{
    if (args.size() == 2 && args[0] == "run") return stream::runImage(args[1]);
    if (args.size() >= 3 && args[0] == "era5-register") {
        std::string product = "auto";
        std::vector<std::string> files;
        for (size_t index = 2; index < args.size(); ++index) {
            if (args[index] == "--product" && index + 1 < args.size()) product = args[++index];
            else files.push_back(args[index]);
        }
        return stream::era5::registerPack(files, stream::pathFromUtf8(args[1]), product) ? 0 : 3;
    }
    return usage();
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[])
{
    std::vector<std::string> args;
    for (int index = 1; index < argc; ++index) args.push_back(utf8FromWide(argv[index]));
#else
int main(int argc, char* argv[])
{
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    if (args.empty()) return usage();
    // Paths are UTF-8 throughout; GDAL must interpret file names the same way.
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    GDALAllRegister();
    usePackagedProj();
    try {
        return dispatch(args);
    } catch (const std::exception& error) {
        std::cerr << "[STREAM] 错误：" << error.what() << '\n';
        return 1;
    }
}
