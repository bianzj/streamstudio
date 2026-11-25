// ==========================================================================
// 文件：projectjson.cpp
// 功能：HiStream 引擎读取 project.json（GUI/服务端写出的工程文件）的统一入口，
//       负责加载校验、按段取子对象、路径解析，以及带缺省值的宽松字段读取。
// 主要模块：
//   1. 匿名命名空间：空对象/空数组哨兵、objectMember/arrayMember、executableDirectory
//   2. ProjectJson::load：路径与文件名检查、JSON 解析、format/schemaVersion 校验
//   3. 各配置段访问器（scene/light/sensor/control/objects/spectra/...）
//   4. 路径解析：resolve、outputDirectory、runtimeDefinedDirectory、meteorologyPath
//   5. 宽松取值：numbers/names/string/number/integer/boolean（缺失或类型不符回退缺省值）
//   6. hasSpecularReflection：判断工程中是否有任何对象/背景启用镜面反射
//   7. sensorBands：传感器波段列表（须与前端 project-schema.js 逐条规则一致）
//   8. viewAngles：观测几何（VZA/VAA）列表，支持单角度、主平面、半球采样
// 数据流：命令行给出 project.json 路径 → load → 各模块经访问器读取 configuration 下的段。
// 与其他文件的关系：声明在 projectjson.h；sensorBands 与
//   src/renderer/src/project-schema.js 的 sensorBandValues() 必须保持一致，
//   服务端 server.mjs 按同一波段列表解析 Facet 结果。
// 重要约定：角度单位为度；VAA 归一化到 [0,360)；波长数值单位与 project.json 一致（nm）；
//   缺失/非法字段一律回退缺省值而不抛异常（仅顶层结构错误才抛异常）。
// ==========================================================================

#include "projectjson.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
// 本引擎能读取的 project.json schemaVersion 上限；超出则 load 报错。
// （升级工程格式时需与前端 project-schema 同步，待核对）
constexpr int kLatestProjectSchemaVersion = 6;

// 空对象/空数组哨兵：段缺失或类型不符时返回其引用，使访问器永远不返回悬空引用。
const ProjectJson::Json emptyObject = ProjectJson::Json::object();
const ProjectJson::Json emptyArray = ProjectJson::Json::array();

// 作用：取 value[key] 且要求其为对象；value 非对象、键缺失或类型不符时返回空对象。
const ProjectJson::Json& objectMember(const ProjectJson::Json& value, const char* key) {
    if (!value.is_object()) return emptyObject;
    const auto found = value.find(key);
    return found != value.end() && found->is_object() ? *found : emptyObject;
}

// 作用：取 value[key] 且要求其为数组；否则返回空数组。
const ProjectJson::Json& arrayMember(const ProjectJson::Json& value, const char* key) {
    if (!value.is_object()) return emptyArray;
    const auto found = value.find(key);
    return found != value.end() && found->is_array() ? *found : emptyArray;
}

// 作用：返回可执行文件所在目录，用于定位随程序发布的 defined/ 与 assets/。
// 说明：非 Windows 或取路径失败时退化为当前工作目录。
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

// 作用：把 NaN/Inf 替换为缺省值，避免非有限数进入后续物理计算。
float clean(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}
}

// 作用：加载并校验 project.json。
// 输入：project.json 路径（相对路径按当前目录转为绝对路径）；输出：ProjectJson 对象。
// 校验：文件名必须是 project.json；format 缺省 histream-project；
//       schemaVersion 缺省 1、范围 1..kLatestProjectSchemaVersion；必须有 configuration 对象。
// 失败一律抛异常（调用方负责报告）。
ProjectJson ProjectJson::load(const std::string& path) {
    if (path.empty()) throw std::invalid_argument("project.json path is required");
    ProjectJson result;
    // DEFERRED[中] 安全: 入口路径以窄 std::string 构造 path，Windows 上按 ANSI 代码页解释；若调用方（命令行/GUI）传入 UTF-8 编码的中文路径会找不到文件，而 resolve() 中对 JSON 内路径却按 UTF-8 处理，两者不一致；建议：main 使用 wmain/_wargv 获取参数，load 接受 std::filesystem::path。
    result.m_inputPath = std::filesystem::absolute(path).lexically_normal();
    // 步骤 1：只接受名为 project.json 的文件（不区分大小写）。
    std::string fileName = result.m_inputPath.filename().string();
    std::transform(fileName.begin(), fileName.end(), fileName.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (fileName != "project.json")
        throw std::invalid_argument("HiStream accepts project.json only");
    // 步骤 2：读入并解析 JSON，要求根为对象。
    std::ifstream input(result.m_inputPath, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open project.json: " + result.m_inputPath.string());
    try { input >> result.m_root; }
    catch (const std::exception& error) {
        throw std::runtime_error(std::string("Invalid project.json: ") + error.what());
    }
    if (!result.m_root.is_object()) throw std::runtime_error("project.json root must be an object");
    // 步骤 3：校验格式标识与 schemaVersion；缺省值兼容没有这些字段的旧工程。
    const std::string format = string(result.m_root, "format", "histream-project");
    if (format != "histream-project")
        throw std::runtime_error("Unsupported project format: " + format);
    const int version = integer(result.m_root, "schemaVersion", 1);
    if (version < 1 || version > kLatestProjectSchemaVersion)
        throw std::runtime_error("Unsupported project schemaVersion; expected 1-" +
                                 std::to_string(kLatestProjectSchemaVersion));
    // 步骤 4：缓存 configuration 子树（拷贝），其后各访问器都从它取段。
    result.m_configuration = objectMember(result.m_root, "configuration");
    if (result.m_configuration.empty()) throw std::runtime_error("project.json has no configuration object");
    return result;
}

// 配置段访问器：均返回 configuration 下对应成员；缺失时返回空对象/空数组。
// 注意 objects() 取的是 configuration.objects.items，meteorology() 对应键名 "meteo"。
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

// 作用：把工程内的路径字符串解析为绝对/规范化路径。
// 规则：绝对路径原样规范化；相对路径先相对工程目录；若不存在且以 assets 开头，
//       则从可执行文件目录逐级向上查找同名 assets 资源，找不到仍返回工程目录下的路径。
std::filesystem::path ProjectJson::resolve(const std::string& value) const {
    if (value.empty()) return {};
    // JSON strings are UTF-8; a plain std::string path would be decoded with
    // the ANSI code page and garble non-ASCII names.
    const std::filesystem::path path = std::filesystem::u8path(value);
    if (path.is_absolute()) return path.lexically_normal();
    const std::filesystem::path local = (projectDirectory() / path).lexically_normal();
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

// 输出目录：configuration.outDir（缺省 "output"），相对工程目录解析。
std::filesystem::path ProjectJson::outputDirectory() const {
    return resolve(string(m_configuration, "outDir", "output"));
}

// 随程序发布的 defined/ 目录（光谱、气象等内置数据），位于可执行文件旁。
std::filesystem::path ProjectJson::runtimeDefinedDirectory() const {
    return executableDirectory() / "defined";
}

// 作用：气象文件路径。path 为空、defined/meteo.txt 或 GUI 的占位文本
//       "HiStream 内置气象数据" 时使用内置 defined/meteo.txt，否则按工程路径解析。
std::filesystem::path ProjectJson::meteorologyPath() const {
    const std::string value = string(meteorology(), "path", "defined/meteo.txt");
    if (value.empty() || value == "defined/meteo.txt" || value == "HiStream 内置气象数据")
        return runtimeDefinedDirectory() / "meteo.txt";
    return resolve(value);
}

// 作用：把数组、数字或字符串（分隔符 , ; 空格）解析为 float 列表。
// 说明：数组中非数字项、字符串中无法解析或非有限的片段被静默丢弃。
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

// 作用：把字符串数组或逗号分隔的字符串解析为名称列表（逗号分隔时去首尾空白、跳过空项）。
std::vector<std::string> ProjectJson::names(const Json& value) {
    std::vector<std::string> result;
    if (value.is_array()) {
        for (const auto& item : value) if (item.is_string()) result.push_back(item.get<std::string>());
        return result;
    }
    if (!value.is_string()) return result;
    std::istringstream input(value.get<std::string>());
    std::string token;
    const auto addTrimmed = [&result](std::string part) {
        const auto first = part.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return;
        const auto last = part.find_last_not_of(" \t\r\n");
        result.push_back(part.substr(first, last - first + 1));
    };
    while (std::getline(input, token, ',')) addTrimmed(token);
    return result;
}

// 宽松取字符串：键缺失或为 null 回退 fallback；非字符串值以 JSON 文本（dump）返回。
std::string ProjectJson::string(const Json& object, const char* key, const std::string& fallback) {
    if (!object.is_object()) return fallback;
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    if (found->is_string()) return found->get<std::string>();
    return found->is_null() ? fallback : found->dump();
}

// 宽松取浮点数：接受数值或可解析的字符串；解析失败或非有限值回退 fallback。
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

// 宽松取整数：整型值饱和到 int 范围（防止溢出）；浮点/字符串经 number 后四舍五入。
int ProjectJson::integer(const Json& object, const char* key, int fallback) {
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

// 宽松取布尔：接受 bool、整数（非 0 为真）、字符串 "true"/"1"；其他类型回退 fallback。
bool ProjectJson::boolean(const Json& object, const char* key, bool fallback) {
    if (!object.is_object()) return fallback;
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    if (found->is_boolean()) return found->get<bool>();
    if (found->is_number_integer()) return found->get<int>() != 0;
    if (found->is_string()) return found->get<std::string>() == "true" || found->get<std::string>() == "1";
    return fallback;
}

// 作用：判断工程是否至少有一个启用镜面反射的表面（背景或任一对象/网格）。
// 说明：每个绑定（背景、对象、网格）的镜面开关可以是新写法 specularEnabled/specular.enabled，
//       也可以缺省，缺省时回退旧写法——control.specularEnabled（缺省开）加光谱项
//       specular.enabled 且 fraction>0（fraction 缺省 0.5）。
bool ProjectJson::hasSpecularReflection() const {
    // 光谱项的 specular 设置是否生效：enabled 且 fraction>0。
    const auto enabled = [](const Json& setting) {
        return boolean(setting, "enabled", false) && number(setting, "fraction", 0.5f) > 0.0f;
    };
    // 旧工程兼容：绑定未声明镜面开关时，由 control 总开关与同名光谱项的 specular 设置决定。
    const auto legacy = [&](const std::string& name) {
        if (!boolean(control(), "specularEnabled", true)) return false;
        for (const auto& spectrum : spectra())
            if (string(spectrum, "name") == name) return enabled(objectMember(spectrum, "specular"));
        return false;
    };
    // 判断单个绑定（背景/对象/网格）：按 spectralName（缺省 fallback）找光谱项，
    // 绑定自身开关为开且该光谱项 specular.fraction>0 才算启用。
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
    // 背景默认光谱名 soil；对象未写 spectralName 时同样回退 soil，网格回退到所属对象的名称。
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

// Sensor band list.  MUST stay rule-for-rule identical to sensorBandValues() in
// src/renderer/src/project-schema.js: the server rasterizes Facet results with
// the JS list, so any difference in band count or order makes it read the
// per-band records at the wrong offsets.  JS rules reproduced here:
//   * values are rounded to 6 decimals (cleanNumber) and de-duplicated on that key;
//   * a continuous range yields floor((end-start)/step + 1e-9) + 1 bands and also
//     appends `end` when the step does not land on it exactly;
//   * custom bands keep their room: the range is truncated to 800 - (#custom);
//   * the merged list is sorted by wavelength and then truncated to 800.
// Arithmetic is done in double as in JS; the result is converted to float last.
// （规则说明见上方原注释）实现分三步：解析自定义波段 → 生成连续范围 → 去重、排序、截断。
std::vector<float> ProjectJson::sensorBands() const {
    // 以下几个 lambda 复刻 JS 的数值语义（Number、truthiness 等），保持与前端结果一致。
    constexpr size_t kMaxSensorBands = 800;  // project-schema.js MAX_SENSOR_BANDS
    const auto cleanNumber = [](double value) { return std::round(value * 1000000.0) / 1000000.0; };
    const auto bandKey = [](double cleaned) { return std::llround(cleaned * 1000000.0); };
    // JS Number(token): the whole token must be numeric.
    const auto parseNumber = [](const std::string& token, double& parsed) {
        if (token.empty()) return false;
        char* end = nullptr;
        parsed = std::strtod(token.c_str(), &end);
        return end == token.c_str() + token.size() && std::isfinite(parsed);
    };
    // project-schema.js number(value, fallback): null/blank -> fallback.
    const auto jsNumber = [&](const char* key, double fallback) {
        const Json& owner = sensor();
        const auto found = owner.find(key);
        if (found == owner.end() || found->is_null()) return fallback;
        double parsed = fallback;
        if (found->is_number()) parsed = found->get<double>();
        else if (found->is_boolean()) parsed = found->get<bool>() ? 1.0 : 0.0;
        else if (found->is_string()) {
            std::string text = found->get<std::string>();
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return fallback;
            text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
            if (!parseNumber(text, parsed)) return fallback;
        } else return fallback;
        return std::isfinite(parsed) ? parsed : fallback;
    };
    // JS truthiness of sensor.continuousBands.
    const auto truthy = [&](const char* key) {
        const Json& owner = sensor();
        const auto found = owner.find(key);
        if (found == owner.end() || found->is_null()) return false;
        if (found->is_boolean()) return found->get<bool>();
        if (found->is_number()) { const double v = found->get<double>(); return v != 0.0 && !std::isnan(v); }
        if (found->is_string()) return !found->get<std::string>().empty();
        return true;
    };
    // JS: String(sensor.bands || '').split(/[\s,;]+/).map(Number).filter(finite && > 0)
    // 步骤 1：解析 sensor.bands 中的自定义波段（分隔符为空白、逗号、分号；仅保留 >0 的有限数）。
    std::vector<double> custom;
    {
        std::string text;
        const Json& owner = sensor();
        const auto found = owner.find("bands");
        if (found != owner.end()) {
            const auto append = [&text](const Json& item) {
                if (!text.empty()) text += ',';
                if (item.is_string()) text += item.get<std::string>();
                else if (item.is_number()) text += item.dump();
            };
            if (found->is_array()) for (const auto& item : *found) append(item);
            else if (found->is_string() || found->is_number()) append(*found);
        }
        std::string token;
        const auto flush = [&]() {
            double parsed = 0.0;
            if (parseNumber(token, parsed) && parsed > 0.0) custom.push_back(parsed);
            token.clear();
        };
        for (const char ch : text) {
            if (ch == ',' || ch == ';' || std::isspace(static_cast<unsigned char>(ch))) flush();
            else token += ch;
        }
        flush();
    }
    // 记录自定义波段的去重键（6 位小数），用于从 800 的上限中为其预留名额。
    std::set<long long> customKeys;
    for (double value : custom) customKeys.insert(bandKey(cleanNumber(value)));

    // 步骤 2：若启用 continuousBands，按 bandStart/bandEnd/bandStep（缺省 400/2500/10）生成连续波段。
    std::vector<double> merged;
    if (truthy("continuousBands")) {
        const double start = jsNumber("bandStart", 400.0);
        const double end = jsNumber("bandEnd", 2500.0);
        const double step = jsNumber("bandStep", 10.0);
        if (start > 0.0 && end >= start && step > 0.0) {
            // 1e-9 容差避免浮点误差少算最后一个波段；capacity 为扣除自定义波段后的剩余名额。
            const double count = std::floor((end - start) / step + 1e-9) + 1.0;
            const double capacity = static_cast<double>(
                kMaxSensorBands > customKeys.size() ? kMaxSensorBands - customKeys.size() : 0);
            const size_t generated = static_cast<size_t>((std::min)(count, capacity));
            for (size_t index = 0; index < generated; ++index)
                merged.push_back(cleanNumber(start + static_cast<double>(index) * step));
            if (count <= capacity && static_cast<double>(merged.size()) < capacity &&
                std::abs(start + (count - 1.0) * step - end) > 1e-6)
                merged.push_back(cleanNumber(end));
        }
    }
    // 步骤 3：合并自定义波段，6 位小数去重，升序排序，超过 800 截断（并向 stderr 报告）。
    merged.insert(merged.end(), custom.begin(), custom.end());
    std::vector<double> unique;
    std::set<long long> keys;
    for (double value : merged) {
        const double cleaned = cleanNumber(value);
        if (keys.insert(bandKey(cleaned)).second) unique.push_back(cleaned);
    }
    std::sort(unique.begin(), unique.end());
    if (unique.size() > kMaxSensorBands) {
        // Same truncation as JS slice(0, 800); report it instead of dropping silently.
        std::cerr << "HiStream: sensor band list truncated from " << unique.size()
                  << " to " << kMaxSensorBands << " bands\n";
        unique.resize(kMaxSensorBands);
    }
    std::vector<float> values;
    values.reserve(unique.size());
    for (double value : unique) values.push_back(static_cast<float>(value));
    // The engine needs at least one band; the GUI rejects an empty list
    // (validateProject) and server.mjs falls back to the same 10500 nm.
    if (values.empty()) values.push_back(10500.0f);
    return values;
}

// 作用：生成传感器观测角列表，每项 {VZA, VAA}（度）。
// 模式：perspective 投影只取单个 vza/vaa；否则按 principalPlane（主平面+垂直主平面）、
//       hemisphere（128 个半球均匀采样）或单角度（二者都关时）生成。
// 说明：VZA 限幅到 [0,89.999]；VAA 归一化到 [0,360)；key 以 1e-4 度量化去重，
//       天顶（VZA≈0）不论 VAA 合并为一项。
std::vector<std::array<float, 2>> ProjectJson::viewAngles() const {
    std::vector<std::array<float, 2>> result;
    std::set<std::pair<int, int>> keys;
    // 添加一个观测角：限幅、归一化 VAA、去重后加入 result。
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
    // 主平面：VAA = 太阳方位 + offset（0 为主平面，90 为垂直主平面）；
    // VZA 从 -75 到 75 步长 5，负角度代表对侧（VAA 加 180）。
    if (principal) for (float offset : {0.0f, 90.0f})
        for (int angle = -75; angle <= 75; angle += 5)
            add(static_cast<float>(std::abs(angle)), solarAzimuth + offset + (angle < 0 ? 180.0f : 0.0f));
    // 半球采样：cos(VZA) 在 [cos75°, 1] 上等间隔，方位角按黄金角（约 137.5°）递增，
    // 得到近似均匀的 128 个方向。
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
