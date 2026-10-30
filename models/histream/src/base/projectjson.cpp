#include "projectjson.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr int kLatestProjectSchemaVersion = 6;

const ProjectJson::Json emptyObject = ProjectJson::Json::object();
const ProjectJson::Json emptyArray = ProjectJson::Json::array();

const ProjectJson::Json& objectMember(const ProjectJson::Json& value, const char* key) {
    if (!value.is_object()) return emptyObject;
    const auto found = value.find(key);
    return found != value.end() && found->is_object() ? *found : emptyObject;
}

const ProjectJson::Json& arrayMember(const ProjectJson::Json& value, const char* key) {
    if (!value.is_object()) return emptyArray;
    const auto found = value.find(key);
    return found != value.end() && found->is_array() ? *found : emptyArray;
}

std::filesystem::path executableDirectory() {
#ifdef _WIN32
    // DEFERRED[中] 安全: GetModuleFileNameA 按 ANSI 代码页返回，安装目录含代码页外字符时被替换为 '?'，runtimeDefinedDirectory()（optipar/Esun/Esky/meteo）与 assets 回退查找全部失败；建议：改用 GetModuleFileNameW 并以 std::filesystem::path(std::wstring) 构造。
    std::vector<char> buffer(32768, '\0');
    const DWORD length = GetModuleFileNameA(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size())
        return std::filesystem::path(std::string(buffer.data(), length)).parent_path();
#endif
    return std::filesystem::current_path();
}

float clean(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}
}

ProjectJson ProjectJson::load(const std::string& path) {
    if (path.empty()) throw std::invalid_argument("project.json path is required");
    ProjectJson result;
    // DEFERRED[中] 安全: 入口路径以窄 std::string 构造 path，Windows 上按 ANSI 代码页解释；若调用方（命令行/GUI）传入 UTF-8 编码的中文路径会找不到文件，而 resolve() 中对 JSON 内路径却按 UTF-8 处理，两者不一致；建议：main 使用 wmain/_wargv 获取参数，load 接受 std::filesystem::path。
    result.m_inputPath = std::filesystem::absolute(path).lexically_normal();
    std::string fileName = result.m_inputPath.filename().string();
    std::transform(fileName.begin(), fileName.end(), fileName.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (fileName != "project.json")
        throw std::invalid_argument("HiStream accepts project.json only");
    std::ifstream input(result.m_inputPath, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open project.json: " + result.m_inputPath.string());
    try { input >> result.m_root; }
    catch (const std::exception& error) {
        throw std::runtime_error(std::string("Invalid project.json: ") + error.what());
    }
    if (!result.m_root.is_object()) throw std::runtime_error("project.json root must be an object");
    const std::string format = string(result.m_root, "format", "histream-project");
    if (format != "histream-project" && format != "streamsim-project")
        throw std::runtime_error("Unsupported project format: " + format);
    const int version = integer(result.m_root, "schemaVersion", 1);
    if (version < 1 || version > kLatestProjectSchemaVersion)
        throw std::runtime_error("Unsupported project schemaVersion; expected 1-" +
                                 std::to_string(kLatestProjectSchemaVersion));
    result.m_configuration = objectMember(result.m_root, "configuration");
    if (result.m_configuration.empty()) throw std::runtime_error("project.json has no configuration object");
    return result;
}

const ProjectJson::Json& ProjectJson::scene() const { return objectMember(m_configuration, "scene"); }
const ProjectJson::Json& ProjectJson::light() const { return objectMember(m_configuration, "light"); }
const ProjectJson::Json& ProjectJson::sensor() const { return objectMember(m_configuration, "sensor"); }
const ProjectJson::Json& ProjectJson::control() const { return objectMember(m_configuration, "control"); }
const ProjectJson::Json& ProjectJson::objects() const { return arrayMember(objectMember(m_configuration, "objects"), "items"); }
const ProjectJson::Json& ProjectJson::spectra() const { return arrayMember(m_configuration, "spectra"); }
const ProjectJson::Json& ProjectJson::canopies() const { return arrayMember(m_configuration, "canopies"); }
const ProjectJson::Json& ProjectJson::thermals() const { return arrayMember(m_configuration, "thermals"); }
const ProjectJson::Json& ProjectJson::materials() const { return arrayMember(m_configuration, "materials"); }
const ProjectJson::Json& ProjectJson::meteorology() const { return objectMember(m_configuration, "meteo"); }
const ProjectJson::Json& ProjectJson::atmosphere() const { return objectMember(m_configuration, "atmosphere"); }
const ProjectJson::Json& ProjectJson::fluid() const { return objectMember(m_configuration, "fluid"); }

std::filesystem::path ProjectJson::resolve(const std::string& value) const {
    if (value.empty()) return {};
    // JSON strings are UTF-8; a plain std::string path would be decoded with
    // the ANSI code page and garble non-ASCII names.
    const std::filesystem::path path = std::filesystem::u8path(value);
    if (path.is_absolute()) return path.lexically_normal();
    const std::filesystem::path local = (projectDirectory() / path).lexically_normal();
    // FIXED[低] 错误处理: std::filesystem::exists 的无 error_code 重载在权限不足/网络盘断开时抛 filesystem_error，会导致整个项目加载失败；已修复：两处均改用 exists(p, ec) 重载，查询失败（ec 置位）按“不存在”处理并继续回退查找，不再抛异常。
    std::error_code existsError;
    if (std::filesystem::exists(local, existsError) || path.begin() == path.end() || *path.begin() != "assets")
        return local;
    for (std::filesystem::path parent = executableDirectory(); !parent.empty(); parent = parent.parent_path()) {
        const std::filesystem::path candidate = (parent / path).lexically_normal();
        existsError.clear();
        if (std::filesystem::exists(candidate, existsError)) return candidate;
        if (parent == parent.root_path()) break;
    }
    return local;
}

std::filesystem::path ProjectJson::outputDirectory() const {
    return resolve(string(m_configuration, "outDir", "output"));
}

std::filesystem::path ProjectJson::runtimeDefinedDirectory() const {
    return executableDirectory() / "defined";
}

std::filesystem::path ProjectJson::meteorologyPath() const {
    const std::string value = string(meteorology(), "path", "defined/meteo.txt");
    if (value.empty() || value == "defined/meteo.txt" || value == "HiStream 内置气象数据")
        return runtimeDefinedDirectory() / "meteo.txt";
    return resolve(value);
}

std::vector<float> ProjectJson::numbers(const Json& value) {
    std::vector<float> result;
    if (value.is_array()) {
        for (const auto& item : value) if (item.is_number()) result.push_back(item.get<float>());
        return result;
    }
    std::string text;
    if (value.is_string()) text = value.get<std::string>();
    else if (value.is_number()) text = value.dump();
    std::replace(text.begin(), text.end(), ';', ',');
    std::replace(text.begin(), text.end(), ' ', ',');
    std::istringstream input(text);
    std::string token;
    while (std::getline(input, token, ',')) {
        if (token.empty()) continue;
        try {
            const float parsed = std::stof(token);
            if (std::isfinite(parsed)) result.push_back(parsed);
        } catch (...) {}
    }
    return result;
}

std::vector<std::string> ProjectJson::names(const Json& value) {
    std::vector<std::string> result;
    if (value.is_array()) {
        for (const auto& item : value) if (item.is_string()) result.push_back(item.get<std::string>());
        return result;
    }
    if (!value.is_string()) return result;
    std::istringstream input(value.get<std::string>());
    std::string token;
    // FIXED[低] Bug: 逗号分隔的名字未去除首尾空白，"leaf, bark" 得到 " bark"，后续按名字查找光谱/网格失败（numbers() 却把空格当分隔符）；已修复：每个逗号分隔的 token 先 trim 空白字符，空 token 仍被跳过。
    const auto addTrimmed = [&result](std::string part) {
        const auto first = part.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return;
        const auto last = part.find_last_not_of(" \t\r\n");
        result.push_back(part.substr(first, last - first + 1));
    };
    while (std::getline(input, token, ',')) addTrimmed(token);
    return result;
}

std::string ProjectJson::string(const Json& object, const char* key, const std::string& fallback) {
    if (!object.is_object()) return fallback;
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    if (found->is_string()) return found->get<std::string>();
    return found->is_null() ? fallback : found->dump();
}

float ProjectJson::number(const Json& object, const char* key, float fallback) {
    if (!object.is_object()) return fallback;
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    try {
        if (found->is_number()) return clean(found->get<float>(), fallback);
        if (found->is_string()) return clean(std::stof(found->get<std::string>()), fallback);
    } catch (...) {}
    return fallback;
}

int ProjectJson::integer(const Json& object, const char* key, int fallback) {
    // FIXED[低] 数值: 整数经 float 中转，>2^24（16,777,216）的值（如随机种子、大计数）会丢失精度，超出 long 范围时 lround 结果未指定（Windows 上 long 为 32 位）；已修复：JSON 整数直接取 64 位整型并按 int 范围钳制，仅非整数数值/字符串仍走原 float 路径。
    if (!object.is_object()) return fallback;
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    if (found->is_number_unsigned()) {
        const unsigned long long value = found->get<unsigned long long>();
        return value > 2147483647ULL ? 2147483647 : static_cast<int>(value);
    }
    if (found->is_number_integer()) {
        const long long value = found->get<long long>();
        if (value > 2147483647LL) return 2147483647;
        if (value < -2147483647LL - 1) return -2147483647 - 1;
        return static_cast<int>(value);
    }
    return static_cast<int>(std::lround(number(object, key, static_cast<float>(fallback))));
}

bool ProjectJson::boolean(const Json& object, const char* key, bool fallback) {
    if (!object.is_object()) return fallback;
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    if (found->is_boolean()) return found->get<bool>();
    if (found->is_number_integer()) return found->get<int>() != 0;
    if (found->is_string()) return found->get<std::string>() == "true" || found->get<std::string>() == "1";
    return fallback;
}

bool ProjectJson::hasSpecularReflection() const {
    const auto enabled = [](const Json& setting) {
        return boolean(setting, "enabled", false) && number(setting, "fraction", 0.5f) > 0.0f;
    };
    const auto legacy = [&](const std::string& name) {
        if (!boolean(control(), "specularEnabled", true)) return false;
        for (const auto& spectrum : spectra())
            if (string(spectrum, "name") == name) return enabled(objectMember(spectrum, "specular"));
        return false;
    };
    const auto bindingEnabled = [&](const Json& binding, const std::string& fallback) {
        const std::string name = string(binding, "spectralName", fallback);
        if (!binding.contains("specularEnabled") &&
            (!binding.contains("specular") || !binding["specular"].is_object())) return legacy(name);
        const bool active = binding.contains("specularEnabled")
            ? boolean(binding, "specularEnabled", false)
            : boolean(binding["specular"], "enabled", false);
        if (!active) return false;
        for (const auto& spectrum : spectra())
            if (string(spectrum, "name") == name)
                return number(objectMember(spectrum, "specular"), "fraction", 0.5f) > 0.0f;
        return false;
    };
    if (bindingEnabled(objectMember(scene(), "background"), "soil")) return true;
    for (const auto& object : objects()) {
        const auto bindings = arrayMember(object, "meshes");
        const auto fallback = string(object, "spectralName", "soil");
        if (bindings.empty()) {
            if (bindingEnabled(object, fallback)) return true;
        } else {
            for (const auto& mesh : bindings) if (bindingEnabled(mesh, fallback)) return true;
        }
    }
    return false;
}

std::vector<float> ProjectJson::sensorBands() const {
    std::vector<float> values;
    std::set<long long> keys;
    const auto add = [&](float value) {
        if (!(value > 0.0f) || !std::isfinite(value)) return;
        const long long key = std::llround(value * 1000000.0);
        if (keys.insert(key).second) values.push_back(value);
    };
    const std::vector<float> custom = numbers(sensor().contains("bands") ? sensor()["bands"] : Json{});
    if (boolean(sensor(), "continuousBands", false)) {
        const float start = number(sensor(), "bandStart", 400.0f);
        const float end = number(sensor(), "bandEnd", 2500.0f);
        const float step = number(sensor(), "bandStep", 10.0f);
        if (start > 0.0f && end >= start && step > 0.0f) {
            for (int index = 0; index < 800 && start + index * step <= end + 1e-5f; ++index)
                add(start + index * step);
        }
    }
    for (float value : custom) add(value);
    if (values.empty()) values.push_back(10500.0f);
    std::sort(values.begin(), values.end());
    // DEFERRED[低] 错误处理: 连续波段循环上限 800 与此处 resize(800) 都会静默丢弃波段（排序后截掉的是最长波长，如热红外 10500 nm），用户不会收到任何提示；建议：超限时告警或报错。
    if (values.size() > 800) values.resize(800);
    return values;
}

std::vector<std::array<float, 2>> ProjectJson::viewAngles() const {
    std::vector<std::array<float, 2>> result;
    std::set<std::pair<int, int>> keys;
    const auto add = [&](float vza, float vaa) {
        vza = std::clamp(vza, 0.0f, 89.999f);
        vaa = std::fmod(std::fmod(vaa, 360.0f) + 360.0f, 360.0f);
        const auto key = vza < 1e-6f ? std::pair<int, int>{0, 0}
                                     : std::pair<int, int>{static_cast<int>(std::lround(vza * 10000)), static_cast<int>(std::lround(vaa * 10000))};
        if (keys.insert(key).second) result.push_back({vza, vaa});
    };
    if (string(sensor(), "projection", "parallel") == "perspective") {
        add(number(sensor(), "vza", 0.0f), number(sensor(), "vaa", 0.0f));
        return result;
    }
    const bool principal = boolean(sensor(), "principalPlane", false);
    const bool hemisphere = boolean(sensor(), "hemisphere", false);
    const float solarAzimuth = number(light(), "azimuth", 0.0f);
    if (!principal && !hemisphere) add(number(sensor(), "vza", 0.0f), number(sensor(), "vaa", 0.0f));
    if (principal) for (float offset : {0.0f, 90.0f})
        for (int angle = -75; angle <= 75; angle += 5)
            add(static_cast<float>(std::abs(angle)), solarAzimuth + offset + (angle < 0 ? 180.0f : 0.0f));
    if (hemisphere) {
        const float golden = static_cast<float>(3.14159265358979323846 * (3.0 - std::sqrt(5.0)));
        const float minimumCosine = std::cos(75.0f * 3.14159265358979323846f / 180.0f);
        for (int index = 0; index < 128; ++index) {
            // Match TiRT-EB: sample the VZA 0..75 degree spherical cap, not
            // the complete hemisphere down to the 90-degree horizon.
            const float cosine = 1.0f - (1.0f - minimumCosine)
                * static_cast<float>(index) / 127.0f;
            add(std::acos(cosine) * 180.0f / 3.14159265358979323846f,
                index * golden * 180.0f / 3.14159265358979323846f);
        }
    }
    return result;
}
