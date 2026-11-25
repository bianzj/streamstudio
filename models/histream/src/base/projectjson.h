// ==========================================================================
// 文件：projectjson.h
// 功能：声明 ProjectJson——HiStream 对 project.json 的只读封装（实现见 projectjson.cpp）。
// 主要模块：
//   1. load：加载并校验工程文件
//   2. 配置段访问器（scene/light/sensor/control/objects/spectra/...）
//   3. 路径解析（inputPath/projectDirectory/resolve/输出目录/内置 defined 目录/气象文件）
//   4. 传感器波段与观测角（sensorBands/viewAngles）
//   5. 静态宽松取值工具（numbers/names/string/number/integer/boolean）
// 数据流：project.json → ProjectJson → 引擎各模块按段读取配置。
// 重要约定：访问器在段缺失时返回空对象/空数组引用，取值函数缺失或类型不符时返回缺省值。
// ==========================================================================

#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// 类：project.json 的持有者。root() 为整个文件，configuration() 为其中的 configuration 子树。
class ProjectJson {
public:
    using Json = nlohmann::json;

    // 加载并校验工程文件，失败抛异常；文件名必须为 project.json。
    static ProjectJson load(const std::string& path);

    const Json& root() const { return m_root; }
    const Json& configuration() const { return m_configuration; }
    // 配置段访问器：返回 configuration 下的同名段，缺失时为空对象/空数组。
    const Json& scene() const;
    const Json& light() const;
    const Json& sensor() const;
    const Json& control() const;
    const Json& objects() const;
    const Json& spectra() const;
    const Json& canopies() const;
    const Json& thermals() const;
    const Json& materials() const;
    const Json& meteorology() const;
    const Json& atmosphere() const;
    const Json& fluid() const;
    // 工程中是否存在启用镜面反射的表面。
    bool hasSpecularReflection() const;

    // 路径：inputPath 为 project.json 的绝对路径；resolve 解析工程内的相对路径（含 assets 回退）；
    // runtimeDefinedDirectory 为程序自带的 defined/ 目录。
    std::filesystem::path inputPath() const { return m_inputPath; }
    std::filesystem::path projectDirectory() const { return m_inputPath.parent_path(); }
    std::filesystem::path resolve(const std::string& value) const;
    std::filesystem::path outputDirectory() const;
    std::filesystem::path runtimeDefinedDirectory() const;
    std::filesystem::path meteorologyPath() const;

    // Same rules as sensorBandValues() in src/renderer/src/project-schema.js
    // (see projectjson.cpp); the server relies on identical band lists.
    std::vector<float> sensorBands() const;
    std::vector<std::array<float, 2>> viewAngles() const;

    // 静态工具：numbers/names 解析列表；string/number/integer/boolean 按键取值，
    // 缺失或类型不符时返回 fallback。
    static std::vector<float> numbers(const Json& value);
    static std::vector<std::string> names(const Json& value);
    static std::string string(const Json& object, const char* key,
                              const std::string& fallback = {});
    static float number(const Json& object, const char* key, float fallback);
    static int integer(const Json& object, const char* key, int fallback);
    static bool boolean(const Json& object, const char* key, bool fallback);

private:
    Json m_root;
    Json m_configuration;
    std::filesystem::path m_inputPath;
};
