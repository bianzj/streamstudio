// ==========================================================================
// 文件：main.cpp
// 功能：stream.exe 命令行入口：解析子命令并分发到各引擎功能
// 主要模块/步骤：
//   1. usePackagedProj / utf8FromWide：Windows 下 PROJ 数据库路径与宽字符参数转 UTF-8
//   2. forcingCheck：校验气象强迫元数据（可选再打开数据核对变量、时间、站点）
//   3. dispatch：run / forcing-preset / forcing-check / forcing-register / era5-register
//   4. wmain/main：初始化 GDAL，捕获异常并以退出码返回
// 数据流：命令行 → args（UTF-8）→ image.cpp 的 runImage（run）或 forcing/ 下的登记与校验函数
// 退出码：0 成功；1 运行时异常；2 用法错误；3 校验失败或登记失败
// 约定：全程路径与字符串为 UTF-8；进度输出由 image.cpp 写 stdout，日志行以 [STREAM] 开头
// ==========================================================================

// stream: STREAM remote-sensing image simulation for Stream Studio.
//   stream run <config.json>
//   stream era5-register <pack.json> [--product auto|era5|era5-land] <file.nc>...
//   stream forcing-preset [name]
//   stream forcing-check <preset|metadata.json> [--data ...] [--json]
#include "image.h"
#include "../forcing/era5.h"
#include "../forcing/forcing_metadata.h"
#include "../forcing/station_source.h"
#include "../scene/raster.h"

#include <cpl_conv.h>
#include <cpl_json.h>
#include <gdal_priv.h>
#include <ogr_srs_api.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// 匿名命名空间：仅本文件使用的辅助函数。
namespace {

// 模块：PROJ 数据库定位（仅 Windows）。
// 说明：若可执行文件同目录下有 proj/proj.db，则覆盖 PROJ 的默认搜索路径；否则保持默认。
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

// 模块：UTF-16（wchar_t）转 UTF-8，含代理对（surrogate pair）合并为单个码点。
// 用于 Windows 的 wmain 参数转换，保证后续路径/JSON 处理统一为 UTF-8。
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

// 从 UTF-8 文件读取一行一个的 NetCDF 路径。  Windows 命令行上限约 32767
// 字符，按天/年登记的数据包无法把数百个路径直接放在命令行上。
std::vector<std::string> readFileList(const std::string& listPath)
{
    std::ifstream input(stream::pathFromUtf8(listPath));
    if (!input) throw std::runtime_error("无法打开文件列表：" + listPath);
    std::vector<std::string> files;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) files.push_back(line);
    }
    if (!files.empty() && files.front().size() >= 3
        && static_cast<unsigned char>(files.front()[0]) == 0xEF
        && static_cast<unsigned char>(files.front()[1]) == 0xBB
        && static_cast<unsigned char>(files.front()[2]) == 0xBF) {
        files.front().erase(0, 3);
    }
    if (files.empty()) throw std::runtime_error("文件列表为空：" + listPath);
    return files;
}

// 模块：stream forcing-check 子命令。
// 输入：args[1] 为预设名/元数据文件/内联 JSON；可选 --data 后接数据包索引或站点 CSV；--json 输出 JSON。
// 输出：返回 0 表示校验通过，3 表示有错误；错误、警告、信息分别输出。
// stream forcing-check <preset|metadata.json|JSON> [--json]: parses and validates a forcing metadata document.
int forcingCheck(const std::vector<std::string>& args)
{
    bool json = false, data = false;
    std::string specifier;
    std::vector<std::string> dataFiles;
    for (size_t index = 1; index < args.size(); ++index) {
        if (args[index] == "--json") json = true;
        else if (args[index] == "--data") data = true;
        else if (data) dataFiles.push_back(args[index]);
        else if (specifier.empty()) specifier = args[index];
    }
    stream::ForcingIssues issues;
    const stream::ForcingMetadata metadata = stream::resolveForcingMetadata(specifier, std::filesystem::current_path(), &issues);
    // 步骤 2：元数据本身无误且给出了 --data 时，再按元数据打开数据
    // （站点 CSV 或网格数据包），把数据侧的警告并入 issues；步骤 1 为上方的元数据解析。
    if (issues.ok() && !dataFiles.empty()) {
        // Second step: open the data with the metadata (variables / columns present, time parsing, coverage).
        try {
            if (metadata.station()) {
                std::vector<std::filesystem::path> files;
                for (const auto& file : dataFiles) files.push_back(stream::pathFromUtf8(file));
                const stream::StationSource source(metadata, files);
                for (const auto& text : source.report().warnings) issues.warning(text);
            } else {
                stream::era5::Pack pack;
                pack.open(stream::pathFromUtf8(dataFiles.front()), &metadata);
                for (const auto& text : pack.report().warnings) issues.warning(text);
            }
        } catch (const std::exception& error) {
            issues.error(error.what());
        }
    }
    // 步骤 3：输出结果。--json 时输出 ok/errors/warnings/notes（元数据无误时附 resolved 摘要）；
    // 否则逐行输出错误、警告、信息及通过提示。退出码 0 = 通过，3 = 有错误。
    if (json) {
        CPLJSONObject root;
        root.Add("ok", issues.ok());
        CPLJSONArray errors, warnings, notes;
        for (const auto& text : issues.errors) errors.Add(text);
        for (const auto& text : issues.warnings) warnings.Add(text);
        for (const auto& text : metadata.notes) notes.Add(text);
        root.Add("errors", errors);
        root.Add("warnings", warnings);
        root.Add("notes", notes);
        if (issues.ok()) {
            CPLJSONDocument summary;
            if (summary.LoadMemory(stream::forcingMetadataSummaryJson(metadata))) root.Add("resolved", summary.GetRoot());
        }
        std::cout << root.Format(CPLJSONObject::PrettyFormat::Pretty) << "\n";
    } else {
        for (const auto& text : issues.errors) std::cout << "错误：" << text << "\n";
        for (const auto& text : issues.warnings) std::cout << "警告：" << text << "\n";
        for (const auto& text : metadata.notes) std::cout << "信息：" << text << "\n";
        if (issues.ok()) std::cout << "气象强迫元数据 \"" << metadata.name << "\"（" << metadata.form << "）校验通过\n";
    }
    return issues.ok() ? 0 : 3;
}

// 作用：打印命令行用法到 stderr，返回 2（用法错误）。
int usage()
{
    std::cerr << "usage: stream run <config.json>\n"
                 "       stream era5-register <pack.json> [--product auto|era5|era5-land] <file.nc>... [--files-from <list>]\n"
                 "       stream forcing-register <pack.json> --metadata <preset|metadata.json> <file.nc>... [--files-from <list>]\n"
                 "       stream forcing-preset [name]\n"
                 "       stream forcing-check <preset|metadata.json> [--data pack.json|station.csv ...] [--json]\n";
    return 2;
}

// 模块：子命令分发。args 不含程序名。
//   run <config>：调用 runImage；forcing-preset：列出预设或输出某预设 JSON；
//   forcing-check：校验元数据；forcing-register：以元数据登记任意网格强迫数据包；
//   era5-register：登记 ERA5/ERA5-Land 数据包（--product 可指定产品）。
// 其他参数组合返回 usage()。
int dispatch(const std::vector<std::string>& args)
{
    if (args.size() == 2 && args[0] == "run") return stream::runImage(args[1]);
    if (args.size() >= 1 && args[0] == "forcing-preset") {
        if (args.size() == 1) { for (const auto& name : stream::forcingPresetNames()) std::cout << name << "\n"; return 0; }
        std::cout << stream::forcingPresetJson(args[1]) << "\n";
        return 0;
    }
    if (args.size() >= 2 && args[0] == "forcing-check") return forcingCheck(args);
    if (args.size() >= 4 && args[0] == "forcing-register") {
        std::string spec;
        std::vector<std::string> files;
        for (size_t index = 2; index < args.size(); ++index) {
            if (args[index] == "--metadata" && index + 1 < args.size()) spec = args[++index];
            else if (args[index] == "--files-from" && index + 1 < args.size()) {
                for (const auto& file : readFileList(args[++index])) files.push_back(file);
            }
            else files.push_back(args[index]);
        }
        if (spec.empty()) throw std::runtime_error("forcing-register 需要 --metadata <预设名|元数据.json>");
        // 步骤：判断 spec 是预设名、内联 JSON（以 { 开头）还是文件；文件路径转为绝对路径后再登记。
        // A metadata file is stored by absolute path so the pack still finds it when moved next to the index.
        bool preset = spec.rfind("{", 0) == 0;
        for (const auto& name : stream::forcingPresetNames()) preset = preset || name == spec;
        if (!preset) spec = stream::utf8FromPath(std::filesystem::absolute(stream::pathFromUtf8(spec)));
        const stream::ForcingMetadata metadata = stream::resolveForcingMetadata(spec, std::filesystem::current_path());
        return stream::era5::registerForcingPack(files, stream::pathFromUtf8(args[1]), metadata, spec) ? 0 : 3;
    }
    if (args.size() >= 3 && args[0] == "era5-register") {
        std::string product = "auto";
        std::vector<std::string> files;
        for (size_t index = 2; index < args.size(); ++index) {
            if (args[index] == "--product" && index + 1 < args.size()) product = args[++index];
            else if (args[index] == "--files-from" && index + 1 < args.size()) {
                for (const auto& file : readFileList(args[++index])) files.push_back(file);
            }
            else files.push_back(args[index]);
        }
        return stream::era5::registerPack(files, stream::pathFromUtf8(args[1]), product) ? 0 : 3;
    }
    return usage();
}

}  // namespace

#ifdef _WIN32
// 程序入口：Windows 用 wmain 取得 UTF-16 参数并转为 UTF-8；其他平台用 main。
int wmain(int argc, wchar_t* argv[])
{
    std::vector<std::string> args;
    for (int index = 1; index < argc; ++index) args.push_back(utf8FromWide(argv[index]));
#else
int main(int argc, char* argv[])
{
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    // 步骤 1：stdout/stderr 设为无缓冲，使进度行（STREAM\tprogress ...）能被服务端实时读取。
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    if (args.empty()) return usage();
    // Paths are UTF-8 throughout; GDAL must interpret file names the same way.
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    // 步骤 2：注册 GDAL 驱动，并（Windows）指向随程序部署的 PROJ 数据库。
    GDALAllRegister();
    usePackagedProj();
    try {
        return dispatch(args);
    } catch (const std::exception& error) {
        std::cerr << "[STREAM] 错误：" << error.what() << '\n';
        return 1;
    }
}
