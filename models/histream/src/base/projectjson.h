#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

class ProjectJson {
public:
    using Json = nlohmann::json;

    static ProjectJson load(const std::string& path);

    const Json& root() const { return m_root; }
    const Json& configuration() const { return m_configuration; }
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
    bool hasSpecularReflection() const;

    std::filesystem::path inputPath() const { return m_inputPath; }
    std::filesystem::path projectDirectory() const { return m_inputPath.parent_path(); }
    std::filesystem::path resolve(const std::string& value) const;
    std::filesystem::path outputDirectory() const;
    std::filesystem::path runtimeDefinedDirectory() const;
    std::filesystem::path meteorologyPath() const;

    std::vector<float> sensorBands() const;
    std::vector<std::array<float, 2>> viewAngles() const;

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
