// ==========================================================================
// 文件：fileio_json.cpp
// 功能：FileIO::readJson —— 把 JSON 工程文件转换成各模式（RayTracing / VoxelRT / VoxelEB）
//       使用的 XML 风格配置结构（SettingXml、SensorXml、LightXml、SceneXml、光谱/热力/冠层/
//       材质/气象/流体/空气动力配置等）
// 主要模块：
//   1. 匿名命名空间辅助：对象类型映射、网格绑定名与类型（含木质材质迁移）、尺寸与坐标环绕、
//      UAV 巡航航线生成（直线往返 / 矩形 / 随机）
//   2. readAtmosphere：大气配置（缺省值、限幅、LUT 路径）
//   3. configureCommon：运行设置、传感器与观测角、光源、大气，按模式写入目标配置
//   4. bindSpecularOverride / readScene：场景背景、地形环境、对象实体（含镜面反射私有覆盖）
//   5. FileIO::readJson：入口；依次读光谱、热力、冠层、材质，再装配场景、气象、流体、空气动力
// 数据流：工程 JSON → ProjectJson → 本文件各 read*/configure* → FileIO 的 m_pRaytracingXml /
//         m_pVoxelrtXml / m_pVoxelebXml → Geometry::createGeometry、场景构建、能量平衡模块使用
// 与其他文件的关系：projectjson.h 提供 JSON 访问与路径解析；fileio.h 声明 FileIO；
//       shortwave_split.h 提供短波直/散拆分模式解析；geometry.cpp 使用传感器与光源配置
// 约定：长度单位为米；UAV 位置 x/y 水平、z 高度；航向与方位角单位为度；
//       字段缺失时取缺省值以兼容旧工程，非法取值多数用 clamp 限幅，少数直接抛异常
// ==========================================================================

#include "projectjson.h"
#include "fileio.h"
#include "shortwave_split.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace {
using Json = ProjectJson::Json;

// 作用：把工程里对象的类别字符串（大小写两种写法）映射为内部 Type。
// 说明：人、车、船都按 BUILDING（刚性不透光表面）处理；火焰、雾按 VEGETATION 处理；
//       未知类别归 OTHER。
Type objectType(const std::string& value) {
    if (value == "Soil" || value == "soil") return Type::SOIL;
    if (value == "Building" || value == "building" || value == "Human" || value == "human" ||
        value == "Vehicle" || value == "vehicle" ||
        value == "Ship" || value == "ship") return Type::BUILDING;
    if (value == "Water" || value == "water") return Type::WATER;
    if (value == "Vegetation" || value == "vegetation" || value == "Fire" || value == "Fog") return Type::VEGETATION;
    return Type::OTHER;
}

// 作用：取一个对象下各网格（meshes）的 key 字段（如材质名）列表。
// 说明：无 meshes 或为空时，退化为只含 fallback 的单元素列表，保持与旧工程的兼容。
std::vector<std::string> bindingNames(const Json& item, const char* key,
                                      const std::string& fallback) {
    std::vector<std::string> values;
    if (item.contains("meshes") && item["meshes"].is_array()) {
        for (const auto& mesh : item["meshes"])
            values.push_back(ProjectJson::string(mesh, key, fallback));
    }
    if (values.empty()) values.push_back(fallback);
    return values;
}

// 作用：旧工程没有 energyModel 字段时，按材质名识别木质（树枝/树干）。
bool isWoodMaterialName(const std::string& name) {
    return name == "tree_branch_wood" || name == "tree_trunk_wood";
}

// 作用：判断材质是否为木质：先按历史名称，再查材质表的 energyModel == "wood"。
bool isWoodMaterial(const ProjectJson& project, const std::string& name) {
    if (isWoodMaterialName(name)) return true; // Migrates projects made before energyModel.
    for (const auto& material : project.materials()) {
        if (ProjectJson::string(material, "name") == name)
            return ProjectJson::string(material, "energyModel") == "wood";
    }
    return false;
}

// 作用：为对象的每个网格给出能量平衡类别：木质（网格或材质或对象 energyModel）
//       强制为 BUILDING，其余用 fallback。输出长度与 bindingNames 一致。
std::vector<Type> bindingTypes(const ProjectJson& project, const Json& item,
                               Type fallback, const std::string& fallbackMaterial) {
    std::vector<Type> values;
    if (item.contains("meshes") && item["meshes"].is_array()) {
        for (const auto& mesh : item["meshes"]) {
            const std::string materialName =
                ProjectJson::string(mesh, "materialName", fallbackMaterial);
            const bool wood = ProjectJson::string(mesh, "energyModel") == "wood"
                || isWoodMaterial(project, materialName);
            values.push_back(wood ? Type::BUILDING : fallback);
        }
    }
    if (values.empty()) {
        const bool wood = ProjectJson::string(item, "energyModel") == "wood"
            || isWoodMaterial(project, fallbackMaterial);
        values.push_back(wood ? Type::BUILDING : fallback);
    }
    return values;
}

// 作用：读取对象的三个尺寸；不足 3 个补 1，非正或非数的值也置 1（兼容旧工程）。
std::vector<float> dimensions(const Json& item) {
    auto values = ProjectJson::numbers(item.contains("dimensions") ? item["dimensions"] : Json{});
    while (values.size() < 3) values.push_back(1.0f);
    for (float& value : values) if (!(value > 0.0f)) value = 1.0f;
    return values;
}

// 作用：把坐标周期性环绕到 [0, extent)（负值也折回正区间）；extent 非正时返回 0。
// 说明：对应场景的周期边界，使 UAV 航线越界后从另一侧进入。
float wrapCoordinate(float value, float extent) {
    if (!(extent > 0.0f)) return 0.0f;
    const float wrapped = std::fmod(value, extent);
    return wrapped < 0.0f ? wrapped + extent : wrapped;
}

// UAV 巡航航线：逐点位置（x, y, 高度 z，米）与对应航向（度）。
struct CruiseRoute {
    std::vector<glm::vec3> positions;
    std::vector<float> headings;
};

// 作用：把航向角规范化到 [0, 360)。
float normalizedHeading(float value) {
    value = std::fmod(value, 360.0f);
    return value < 0.0f ? value + 360.0f : value;
}

// 作用：按传感器配置生成 UAV 巡航航线（count 个采样点，相邻点沿航线间隔 step 米）。
// 输入：sensor 的 positionX/Y、height、cruiseRoute（line/rectangle/random）、
//       cruiseStep、cruiseCount（限幅 1–10000）等；scene 的 x、y 为场景水平尺寸。
// 输出：CruiseRoute；所有水平坐标按场景尺寸周期环绕，高度不低于 0.01 m。
// 说明：航向约定为 atan2(Δy, Δx)，0° 沿 +x（相对正北的关系待核对）。
CruiseRoute cruiseRoute(const Json& sensor, const Json& scene) {
    // 步骤 1：读取场景尺寸与起点，缺省 60 m 场景、起点在中心、高度 3000 m、
    //       航向 90°、步长 25 m、20 个点；各量设下限避免除零。
    const float width = std::max(0.0001f, ProjectJson::number(scene, "x", 60.0f));
    const float depth = std::max(0.0001f, ProjectJson::number(scene, "y", 60.0f));
    const float startX = wrapCoordinate(ProjectJson::number(sensor, "positionX", width * 0.5f), width);
    const float startY = wrapCoordinate(ProjectJson::number(sensor, "positionY", depth * 0.5f), depth);
    const float height = std::max(0.01f, ProjectJson::number(sensor, "height", 3000.0f));
    const float fallbackHeading = normalizedHeading(ProjectJson::number(sensor, "cruiseHeading", 90.0f));
    const float step = std::max(0.01f, ProjectJson::number(sensor, "cruiseStep", 25.0f));
    const int count = std::clamp(ProjectJson::integer(sensor, "cruiseCount", 20), 1, 10000);
    const std::string mode = ProjectJson::string(sensor, "cruiseRoute", "line");
    CruiseRoute result;
    result.positions.reserve(static_cast<size_t>(count));
    result.headings.reserve(static_cast<size_t>(count));
    const auto append = [&](float x, float y, float z, float heading) {
        result.positions.emplace_back(wrapCoordinate(x, width), wrapCoordinate(y, depth), std::max(0.01f, z));
        result.headings.push_back(normalizedHeading(heading));
    };

    // 步骤 2a：矩形航线：以起点为中心的矩形（边长缺省取场景的 1/4 且不超过 500 m），
    //       按周长参数 distance 依次走过四条边（航向 0°、90°、180°、270°）。
    if (mode == "rectangle") {
        const float rectangleWidth = std::min(width, std::max(std::min(step, width),
            ProjectJson::number(sensor, "cruiseRectangleWidth", std::min(width * 0.25f, 500.0f))));
        const float rectangleHeight = std::min(depth, std::max(std::min(step, depth),
            ProjectJson::number(sensor, "cruiseRectangleHeight", std::min(depth * 0.25f, 500.0f))));
        const float perimeter = 2.0f * (rectangleWidth + rectangleHeight);
        for (int index = 0; index < count; ++index) {
            const float distance = std::fmod(index * step, perimeter);
            if (distance < rectangleWidth)
                append(startX - rectangleWidth * 0.5f + distance, startY - rectangleHeight * 0.5f, height, 0.0f);
            else if (distance < rectangleWidth + rectangleHeight)
                append(startX + rectangleWidth * 0.5f, startY - rectangleHeight * 0.5f + distance - rectangleWidth, height, 90.0f);
            else if (distance < rectangleWidth * 2.0f + rectangleHeight)
                append(startX + rectangleWidth * 0.5f - (distance - rectangleWidth - rectangleHeight), startY + rectangleHeight * 0.5f, height, 180.0f);
            else
                append(startX - rectangleWidth * 0.5f, startY + rectangleHeight * 0.5f - (distance - rectangleWidth * 2.0f - rectangleHeight), height, 270.0f);
        }
        return result;
    }

    // 步骤 2b：随机航线：用固定种子的线性同余发生器（乘数 1664525、增量 1013904223）
    //       逐点随机取航向，保证同一种子可重复；最后一个点不再更换航向。
    if (mode == "random") {
        std::uint32_t seed = static_cast<std::uint32_t>(std::max(1, ProjectJson::integer(sensor, "cruiseRandomSeed", 1)));
        auto randomUnit = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed) / 4294967296.0f;
        };
        float x = startX, y = startY, heading = fallbackHeading;
        for (int index = 0; index < count; ++index) {
            if (index < count - 1) heading = randomUnit() * 360.0f;
            append(x, y, height, heading);
            const float radians = heading * 3.14159265358979323846f / 180.0f;
            x = wrapCoordinate(x + step * std::cos(radians), width);
            y = wrapCoordinate(y + step * std::sin(radians), depth);
        }
        return result;
    }

    // 步骤 2c：默认直线航线：起点到目标点（缺省为沿 cruiseHeading 前进
    //       step·(count-1) 的位置）之间往返飞行，每折返一次航向转 180°。
    //       起终点重合时，退化为沿 cruiseHeading 方向的水平航线。
    const float fallbackRadians = fallbackHeading * 3.14159265358979323846f / 180.0f;
    const float fallbackSpan = step * std::max(1, count - 1);
    const float targetX = ProjectJson::number(sensor, "cruiseTargetX", startX + std::cos(fallbackRadians) * fallbackSpan);
    const float targetY = ProjectJson::number(sensor, "cruiseTargetY", startY + std::sin(fallbackRadians) * fallbackSpan);
    const float targetZ = std::max(0.01f, ProjectJson::number(sensor, "cruiseTargetZ", height));
    float deltaX = targetX - startX;
    float deltaY = targetY - startY;
    float deltaZ = targetZ - height;
    float length = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
    if (length < 0.000001f) {
        deltaX = std::cos(fallbackRadians) * step;
        deltaY = std::sin(fallbackRadians) * step;
        deltaZ = 0.0f;
        length = step;
    }
    const float horizontalLength = std::hypot(deltaX, deltaY);
    const float forwardHeading = horizontalLength < 0.000001f
        ? fallbackHeading
        : normalizedHeading(std::atan2(deltaY, deltaX) * 180.0f / 3.14159265358979323846f);
    // 往返周期 = 2 倍航线长度；phase 为沿周期的里程，前半程去、后半程回。
    const float period = 2.0f * length;
    for (int index = 0; index < count; ++index) {
        const float phase = std::fmod(index * step, period);
        const bool forward = phase < length;
        const float distance = forward ? phase : period - phase;
        append(startX + deltaX / length * distance, startY + deltaY / length * distance,
               height + deltaZ / length * distance, forward ? forwardHeading : forwardHeading + 180.0f);
    }
    return result;
}

// 作用：读取大气配置并规范化。非法的模型/气溶胶类型回退为 midlatitude-summer / rural；
//       水汽限幅 [0.5, 5]（单位待核对），能见度限幅 [10, 50]（km，待核对）；
//       lutFile 相对工程目录解析为绝对路径，缺省为 assets/atmosphere/simple_modtran_lut.csv。
AtmosphereXml readAtmosphere(const ProjectJson& project) {
    const Json& source = project.atmosphere();
    AtmosphereXml value{};
    value.enabled = ProjectJson::boolean(source, "enabled", false);
    value.model = ProjectJson::string(source, "model", "midlatitude-summer");
    if (value.model != "tropical" && value.model != "midlatitude-summer" && value.model != "midlatitude-winter")
        value.model = "midlatitude-summer";
    value.waterVapor = std::clamp(ProjectJson::number(source, "waterVapor", 2.0f), 0.5f, 5.0f);
    value.aerosol = ProjectJson::string(source, "aerosol", "rural");
    if (value.aerosol != "rural" && value.aerosol != "urban") value.aerosol = "rural";
    value.visibility = std::clamp(ProjectJson::number(source, "visibility", 23.0f), 10.0f, 50.0f);
    const std::string lut = ProjectJson::string(source, "lutFile", "assets/atmosphere/simple_modtran_lut.csv");
    value.lutFile = project.resolve(lut).string();
    return value;
}

// 模块：各模式共用的配置读取（运行设置、传感器、光源、大气），并按 mode 写入对应的
//       m_pRaytracingXml / m_pVoxelrtXml / m_pVoxelebXml。
// 输入：project 工程 JSON；mode 运行模式；输出：io 中对应模式的配置结构。
// 步骤：1 读运行设置 SettingXml；2 读传感器与观测角（含 UAV 巡航）；
//       3 读光源；4 读大气与输出目录；5 按模式写入目标结构。
// 说明：所有字段缺失时取缺省值，以兼容旧工程。
void configureCommon(FileIO& io, const ProjectJson& project, Mode mode) {
    const Json& control = project.control();
    const Json& sensor = project.sensor();
    const Json& light = project.light();
    const Json& scene = project.scene();

    // 步骤 1：运行设置。各整型参数用 clamp 限定在可支持的范围内，防止配置越界。
    SettingXml settings{};
    settings.maxDepth = std::max(1, ProjectJson::integer(control, "depth", 4));
    settings.theGPU = std::max(0, ProjectJson::integer(control, "gpu", 0));
    settings.computeBackend = ProjectJson::string(control, "computeBackend", "gpu");
    settings.n_sample = std::clamp(ProjectJson::integer(control, "samples", 32), 1, 1024);
    settings.heterogeneousVoxel = ProjectJson::boolean(control, "heterogeneousVoxel", false);
    // 兼容旧工程：旧字段 periodicBoundary + periodicNeighborCount 折算成
    // 新字段 periodicTraversalCount 的缺省值（旧版关闭周期边界即为 0）。
    const int legacyTraversalCount = ProjectJson::boolean(
        control, "periodicBoundary", false)
            ? ProjectJson::integer(control, "periodicNeighborCount", 8) : 0;
    settings.periodicNeighborCount = std::clamp(ProjectJson::integer(
        control, "periodicTraversalCount", legacyTraversalCount), 0, 20);
    settings.skyboxEnabled = ProjectJson::boolean(control, "skyboxEnabled", false);
    settings.acceleratedRadiationSolver =
        ProjectJson::string(control, "radiationSolver", "traditional") == "accelerated";
    settings.spectralAccelerationWidth = std::clamp(
        ProjectJson::integer(control, "spectralAccelerationWidth", 100), 1, 1000);
    settings.vegetationTemperatureMethod = std::clamp(ProjectJson::integer(control, "vegetationTemperatureMethod", 0), 0, 1);
    // Missing field = neutral (same compatibility rule as FacetEB).
    settings.stabilityCorrection = ProjectJson::boolean(control, "stabilityCorrection", false);
    // 短波散射阶数只允许 1~3，超出范围直接报错（不静默截断）。
    settings.shortwaveScatteringOrders = ProjectJson::integer(control, "shortwaveScatteringOrders", 1);
    if (settings.shortwaveScatteringOrders < 1 || settings.shortwaveScatteringOrders > 3)
        throw std::runtime_error("control.shortwaveScatteringOrders must be 1, 2 or 3");
    settings.voxelCollisionOrders = std::clamp(
        ProjectJson::integer(control, "voxelCollisionOrders", 2), 2, 5);
    settings.radiationMaxSteps = std::clamp(
        ProjectJson::integer(control, "radiationMaxSteps", 64), 1, 4096);
    // UAV 巡航遍历仅在透视投影下有效：由 sensor.cruiseEnabled 或 control.uavTraverse 触发。
    const bool perspective = ProjectJson::string(sensor, "projection", "parallel") == "perspective";
    settings.isUAVtrave = perspective &&
        (ProjectJson::boolean(sensor, "cruiseEnabled", false) ||
         ProjectJson::boolean(control, "uavTraverse", false));

    // 步骤 2：传感器。分辨率至少 1×1；传感器位置缺省在场景水平中心、高度 3000 m；
    //       视场角限幅 [0.1°, 120°]。
    SensorXml sensorData{};
    sensorData.name = "MainSensor";
    sensorData.projection = ProjectJson::string(sensor, "projection", "parallel") == "perspective"
        ? Projection::PERSPECTIVE : Projection::PARALLAL;
    sensorData.resolution = {
        static_cast<float>(std::max(1, ProjectJson::integer(sensor, "x", 512))),
        static_cast<float>(std::max(1, ProjectJson::integer(sensor, "y", 512)))
    };
    sensorData.position = {
        ProjectJson::number(sensor, "positionX", ProjectJson::number(scene, "x", 60.0f) * 0.5f),
        ProjectJson::number(sensor, "positionY", ProjectJson::number(scene, "y", 60.0f) * 0.5f),
        std::max(0.01f, ProjectJson::number(sensor, "height", 3000.0f))
    };
    sensorData.sensorFov = std::clamp(ProjectJson::number(sensor, "fov", 60.0f), 0.1f, 120.0f);
    // 巡航模式：由 cruiseRoute 生成逐点位姿 uavPoses，再据航向把观测方位角换算到绝对方位。
    CruiseRoute route{};
    if (settings.isUAVtrave) {
        route = cruiseRoute(sensor, scene);
        sensorData.uavPoses = route.positions;
    }
    sensorData.waves = project.sensorBands();
    // 观测角：以第一个巡航点的航向为基准（无航线时取 sensor.cruiseHeading，缺省 90°）。
    const float cruiseHeading = route.headings.empty()
        ? ProjectJson::number(sensor, "cruiseHeading", 90.0f) : route.headings.front();
    for (const auto& angle : project.viewAngles()) {
        float azimuth = angle[1];
        if (settings.isUAVtrave) {
            // In cruise mode the project azimuth is camera-relative:
            // 0 forward, 90 right, 180 backward, 270 left. Geometry expects
            // the remote-sensing ground-to-sensor azimuth, hence +180 deg.
            azimuth = std::fmod(cruiseHeading + azimuth + 180.0f, 360.0f);
            if (azimuth < 0.0f) azimuth += 360.0f;
        }
        sensorData.viewAngles.emplace_back(angle[0], azimuth);
    }
    // 工程未给观测角时，退回单一天底视角 {0,0} 并给出警告。
    if (sensorData.viewAngles.empty()) {
        std::cerr << "Warning: no viewAngle in project sensor; falling back to a single nadir view {0,0}." << std::endl;
        sensorData.viewAngles.emplace_back(0.0f, 0.0f);
    }
    // 为每个巡航位姿单独计算观测方位角（第一个观测角的相对方位 + 该点航向 + 180°），
    // 供 Geometry::updateSensorPos 逐位姿覆盖 vaa。
    if (settings.isUAVtrave && !project.viewAngles().empty()) {
        const float relativeAzimuth = project.viewAngles().front()[1];
        for (const float heading : route.headings)
            sensorData.uavViewAzimuths.push_back(normalizedHeading(heading + relativeAzimuth + 180.0f));
    }
    sensorData.isImage = ProjectJson::boolean(sensor, "image", true);
    sensorData.isRadiationProcess = ProjectJson::boolean(sensor, "radiationProcess", false);
    sensorData.isEnergyProcess = ProjectJson::boolean(sensor, "energyProcess", false);
    sensorData.isProcess = sensorData.isRadiationProcess || sensorData.isEnergyProcess || ProjectJson::boolean(sensor, "process", false);
    sensorData.isTemperature = ProjectJson::boolean(sensor, "temperature", true);
    sensorData.isAlbedo = ProjectJson::boolean(sensor, "albedo", false);

    // 步骤 3：光源。缺省太阳天顶角 30°、方位角 135°；直射占比 direct 限幅 [0,1]，
    //       散射 = 1 - direct；天空等效温度缺省 250 K；太阳等效温度固定 6000 K；
    //       水平面全球短波 shortwaveIrradiance 限幅 [0, 2000]（单位待核对，应为 W m-2）。
    LightXml lightData{};
    lightData.name = "Solar";
    lightData.solarAngle = {ProjectJson::number(light, "zenith", 30.0f),
                            ProjectJson::number(light, "azimuth", 135.0f)};
    lightData.direct = std::clamp(ProjectJson::number(light, "direct", 0.8f), 0.0f, 1.0f);
    lightData.diffuse = 1.0f - lightData.direct;
    lightData.skyTemperature = ProjectJson::number(light, "skyTemperature", 250.0f);
    lightData.solarTemperature = 6000.0f;
    lightData.shortwaveIrradiance = std::clamp(
        ProjectJson::number(light, "shortwaveIrradiance", 0.0f), 0.0f, 2000.0f);
    // Missing field = legacy fixed split; new projects save "erbs".
    lightData.diffuseFractionMode = shortwave_split::parseMode(
        ProjectJson::string(light, "diffuseFractionMode", "fixed"));
    // 步骤 4：大气配置与输出目录 / 工程定义目录（definedDir 为运行时定义目录的上一级）。
    const AtmosphereXml atmosphere = readAtmosphere(project);

    const std::string output = project.outputDirectory().string();
    // DEFERRED[中] 安全: ProjectJson::resolve 特意用 u8path 保留 UTF-8 路径，但这里及 readScene/readJson 中各处 .string() 又在 Windows 上按 ANSI 代码页转回窄字符串；路径含代码页外字符时 MSVC 抛 std::system_error，代码页内字符则依赖后续所有 fopen/GDALOpen 也用 ANSI；建议：结构体中路径统一存 std::filesystem::path（或 u8string），打开文件时用宽字符 API。
    const std::string defined = project.runtimeDefinedDirectory().parent_path().string();
    // 步骤 5：按模式写入对应配置。RayTracing 单独设置路径追踪深度；
    //       VoxelEB 另需太阳/天空辐照输入文件（Esun_.dat、Esky_.dat，位于运行时定义目录）。
    if (mode == Mode::eRaytracing) {
        io.m_pRaytracingXml->projectDir = output;
        io.m_pRaytracingXml->definedDir = defined;
        io.m_pRaytracingXml->settingxml = settings;
        // Path tracing has its own scattering-order budget. The shared "depth"
        // key sets the VoxelRT DDA step budget (depth x 128); reusing its
        // default of 4 here truncated every path after four bounces and biased
        // NIR canopy reflectance low. Russian roulette (from bounce 3) keeps
        // deep paths cheap, so the default is 16 and the hard cap 64.
        io.m_pRaytracingXml->settingxml.maxDepth = std::clamp(
            ProjectJson::integer(control, "raytracingDepth", 16), 1, 64);
        io.m_pRaytracingXml->sensorxml = sensorData;
        io.m_pRaytracingXml->lightxml = lightData;
        io.m_pRaytracingXml->atmospherexml = atmosphere;
    } else if (mode == Mode::eVoxelRT) {
        io.m_pVoxelrtXml->projectDir = output;
        io.m_pVoxelrtXml->definedDir = defined;
        io.m_pVoxelrtXml->settingxml = settings;
        io.m_pVoxelrtXml->sensorxml = sensorData;
        io.m_pVoxelrtXml->lightxml = lightData;
        io.m_pVoxelrtXml->atmospherexml = atmosphere;
    } else if (mode == Mode::eVoxelEB) {
        io.m_pVoxelebXml->projectDir = output;
        io.m_pVoxelebXml->definedDir = defined;
        io.m_pVoxelebXml->settingxml = settings;
        io.m_pVoxelebXml->sensorxml = sensorData;
        io.m_pVoxelebXml->lightxml = lightData;
        io.m_pVoxelebXml->atmospherexml = atmosphere;
        io.m_pVoxelebXml->atomcondxml.rinfile = (project.runtimeDefinedDirectory() / "Esun_.dat").string();
        io.m_pVoxelebXml->atomcondxml.rlifile = (project.runtimeDefinedDirectory() / "Esky_.dat").string();
    }
}

// 作用：处理网格/背景上对镜面反射开关的局部覆盖，而不改动共享的光谱材质。
// 输入：binding 绑定（specularEnabled 或 specular.enabled）；spectrum 原光谱名；
//       materials 光学材质表（可能新增条目）；privateName 私有副本名。
// 输出：实际应绑定的光谱名。无覆盖或与原值一致时返回原名；否则复制一份光谱，
//       设置 specularEnabled，名称冲突时在末尾补 "_"，并追加到 materials。
std::string bindSpecularOverride(const Json& binding, const std::string& spectrum,
                                std::vector<SpectralXml>& materials, const std::string& privateName) {
    if (!binding.contains("specularEnabled") &&
        (!binding.contains("specular") || !binding["specular"].is_object())) return spectrum;
    const auto found = std::find_if(materials.begin(), materials.end(), [&](const auto& value) {
        return value.spectralName == spectrum;
    });
    if (found == materials.end()) throw std::runtime_error("Surface spectrum not found: " + spectrum);
    const bool enabled = binding.contains("specularEnabled")
        ? ProjectJson::boolean(binding, "specularEnabled", false)
        : ProjectJson::boolean(binding["specular"], "enabled", false);
    if (found->specularEnabled == enabled) return spectrum;
    SpectralXml value = *found;
    if (value.textureSourceName.empty()) value.textureSourceName = found->spectralName;
    value.specularEnabled = enabled;
    value.spectralName = privateName;
    while (std::any_of(materials.begin(), materials.end(), [&](const auto& candidate) {
        return candidate.spectralName == value.spectralName;
    })) value.spectralName += "_";
    const std::string name = value.spectralName;
    materials.push_back(std::move(value));
    return name;
}

// 模块：读取场景：背景（地面）、地形环境、DEM、地理位置与时间，以及对象列表。
// 输入：project、mode、opticalMaterials（RayTracing 的镜面覆盖会向其追加条目）。
// 输出：SceneXml（background + objEntities / primEntities 等）。
// 步骤：1 地形环境参数；2 场景尺寸与体元步长；3 背景材质与 DEM；
//       4 地理位置与年/时区；5 逐对象生成实体（RayTracing 走 ObjEntity，
//       其余模式走 PrimEntity）。
SceneXml readScene(const ProjectJson& project, Mode mode, std::vector<SpectralXml>& opticalMaterials) {
    const Json& source = project.scene();
    SceneXml scene{};
    // 步骤 1：地形环境（气温递减率、水汽/土壤/CO2 随高度的标高参数）。
    //       字段必须是数值；启用后只支持 eVoxelEB，其他模式报错。
    if (source.contains("terrainEnvironment")) {
        const Json& env = source["terrainEnvironment"];
        if (!env.is_object()) throw std::runtime_error("scene.terrainEnvironment must be an object");
        auto& e = scene.background.terrainEnvironment;
        e.airEnabled = ProjectJson::boolean(env, "airEnabled", false);
        e.soilEnabled = ProjectJson::boolean(env, "soilEnabled", false);
        const auto read = [&](const char* key, float fallback) {
            if (!env.contains(key)) return fallback;
            if (!env[key].is_number()) throw std::runtime_error(std::string("terrainEnvironment.") + key + " must be numeric");
            return env[key].get<float>();
        };
        e.referenceHeight = read("referenceHeight", e.referenceHeight);
        e.temperatureLapseRate = read("temperatureLapseRate", e.temperatureLapseRate);
        e.vaporScaleHeight = read("vaporScaleHeight", e.vaporScaleHeight);
        e.drainageHeight = read("drainageHeight", e.drainageHeight);
        e.soilDry = read("soilDry", e.soilDry);
        e.soilValley = read("soilValley", e.soilValley);
        e.soilScaleHeight = read("soilScaleHeight", e.soilScaleHeight);
        e.co2ValleyExcess = read("co2ValleyExcess", e.co2ValleyExcess);
        e.co2ScaleHeight = read("co2ScaleHeight", e.co2ScaleHeight);
        e.validate();
        if ((e.airEnabled || e.soilEnabled || e.co2ValleyExcess != 0) && mode != Mode::eVoxelEB)
            throw std::runtime_error("TerrainEnvironment currently requires eVoxelEB");
    }
    // 步骤 2：场景尺寸 x/y/高度（米，缺省 60×60×15）；体元步长（水平与垂直相同）缺省 1 m；
    //       voxelFillThreshold 缺省 0.05（体元被判为填充的占比阈值，含义待核对）。
    scene.background.sceneSize = {
        std::max(0.01f, ProjectJson::number(source, "x", 60.0f)),
        std::max(0.01f, ProjectJson::number(source, "y", 60.0f)),
        std::max(0.0f, ProjectJson::number(source, "height", 15.0f))
    };
    scene.background.sceneOrigin = {0, 0, 0};
    scene.background.stepsize_surface = scene.background.stepsize_height =
        std::max(0.01f, ProjectJson::number(source, "voxel", 1.0f));
    scene.background.voxelFillThreshold = std::clamp(ProjectJson::number(source, "voxelFillThreshold", 0.05f), 0.0f, 1.0f);
    // 步骤 3：背景（地面）的光谱、热力、属性名与类型；RayTracing 下处理镜面覆盖；
    //       地形开启且给出 demFile 时标记为 DEM，并解析为绝对路径。
    //       heterogeneityEnabled 关闭时角度效应强度置 0。
    const Json background = source.contains("background") ? source["background"] : Json::object();
    scene.background.bgSpectralName = ProjectJson::string(background, "spectralName", "soil");
    if (mode == Mode::eRaytracing)
        scene.background.bgSpectralName = bindSpecularOverride(background,
            scene.background.bgSpectralName, opticalMaterials, "__background_specular");
    scene.background.bgThermalName = ProjectJson::string(background, "thermalName", "soil_temperature");
    scene.background.bgPropName = ProjectJson::string(background, "materialName", "soilset");
    scene.background.type = objectType(ProjectJson::string(background, "materialType", "Soil"));
    scene.background.angularEffectStrength = ProjectJson::boolean(background, "heterogeneityEnabled", false)
        ? std::clamp(ProjectJson::number(background, "angularEffectStrength", 0.5f), 0.0f, 1.0f) : 0.0f;
    scene.background.isDEM = ProjectJson::boolean(source, "terrain", false) && !ProjectJson::string(source, "demFile").empty();
    if (scene.background.isDEM) scene.background.DEMFile = project.resolve(ProjectJson::string(source, "demFile")).string();
    // 步骤 4：地理位置与时间（缺省北京：40°N、116°E、2019 年、东八区），用于 SPA 太阳位置计算。
    scene.background.lat = std::clamp(ProjectJson::number(project.meteorology(), "latitude", 40.0f), -90.0f, 90.0f);
    scene.background.lon = std::clamp(ProjectJson::number(project.meteorology(), "longitude", 116.0f), -180.0f, 180.0f);
    scene.background.year = std::clamp(ProjectJson::integer(project.meteorology(), "year", 2019), -2000, 6000);
    scene.background.timezone = std::clamp(ProjectJson::number(project.meteorology(), "timezone", 8.0f), -18.0f, 18.0f);

    // 步骤 5：逐对象生成实体。火/雾等介质对象只在 VoxelRT / VoxelEB 中保留；
    //       各绑定（光谱、热力、冠层、材质）按网格给出，缺省回落到对象级字段。
    for (const Json& item : project.objects()) {
        const std::string rawType = ProjectJson::string(item, "type", "Vegetation");
        const bool medium = rawType == "Fire" || rawType == "Fog" || item.contains("medium");
        if (medium && mode != Mode::eVoxelRT && mode != Mode::eVoxelEB) continue;
        const std::string name = ProjectJson::string(item, "name", "object");
        const std::string defaultSpectrum = ProjectJson::string(item, "spectralName", "soil");
        const std::string defaultThermal = ProjectJson::string(item, "thermalName", "soil_temperature");
        const std::string defaultCanopy = ProjectJson::string(item, "canopyName",
            rawType == "Fire" ? "fire_medium" : rawType == "Fog" ? "fog_medium" : rawType == "Vegetation" ? "canopy_default" : "rigid_body");
        const std::string defaultProperty = ProjectJson::string(item, "materialName",
            rawType == "Vegetation" ? "leaf_c3" : rawType == "Water" ? "water_set" : "soilset");
        const auto meshNames = bindingNames(item, "name", name);
        const auto spectra = bindingNames(item, "spectralName", defaultSpectrum);
        const auto thermals = bindingNames(item, "thermalName", defaultThermal);
        const auto canopies = bindingNames(item, "canopyName", defaultCanopy);
        const auto properties = bindingNames(item, "materialName", defaultProperty);
        const auto types = bindingTypes(project, item, objectType(rawType), defaultProperty);
        const auto shape = dimensions(item);
        // 介质数据的坐标顺序标记为 "XYZ" 时视为标准轴顺序（否则按旧顺序，待核对）。
        const bool standardMediumAxes = medium && item.contains("medium")
            && item["medium"].is_object()
            && ProjectJson::string(item["medium"], "coordinateOrder", "") == "XYZ";
        const std::string model = medium ? std::string{} : project.resolve(ProjectJson::string(item, "fileName")).string();
        const std::string positions = project.resolve(ProjectJson::string(item, "positionFile")).string();

        if (mode == Mode::eRaytracing) {
            // RayTracing：对象为网格文件实体 ObjEntity。镜面覆盖按网格生成私有光谱副本，
            //       不影响共享光谱、背景或其它使用同一光谱的网格。
            ObjEntity entity{};
            entity.objName = name;
            entity.filePath = model;
            entity.meshNames = meshNames;
            entity.spectralNames = spectra;
            // Private optical records bind the Mesh override without changing the
            // shared spectrum, background, or any other Mesh using that spectrum.
            for (size_t i = 0; i < entity.spectralNames.size(); ++i) {
                const Json& mesh = item.contains("meshes") && item["meshes"].is_array() && !item["meshes"].empty()
                    ? item["meshes"][i] : item;
                entity.spectralNames[i] = bindSpecularOverride(mesh, entity.spectralNames[i],
                    opticalMaterials, "__mesh_specular_" + std::to_string(scene.objEntities.size()) + "_" + std::to_string(i));
            }
            entity.thermalNames = thermals;
            entity.types = types;
            entity.isFromFile = !positions.empty();
            entity.file = positions;
            scene.objEntities.push_back(std::move(entity));
            continue;
        }

        // 体元类模式（VoxelRT / VoxelEB 等）：对象为 PrimEntity。
        PrimEntity entity{};
        entity.primitiveName = name;
        entity.meshNames = meshNames;
        entity.types = types;
        entity.spectralNames = spectra;
        entity.thermalNames = thermals;
        entity.canopyNames = canopies;
        entity.propNames = properties;
        entity.type = objectType(rawType);
        entity.shape = {
            ProjectJson::string(item, "shape", "cube") == "ellipsoid" ? ShapeType::ELLIPSOID : ShapeType::CUBE,
            standardMediumAxes ? shape[2] : shape[1],
            standardMediumAxes ? shape[1] : shape[2],
            shape[0], glm::vec3(0, 0, 0)
        };
        entity.isdisFromFile = !positions.empty();
        entity.distributefile = positions;
        entity.voxelizeFromObj = !model.empty();
        entity.objFile = model;
        entity.voxelFillThreshold = scene.background.voxelFillThreshold;
        entity.leafDensityFromGeometry =
            ProjectJson::string(item, "leafDensitySource", "parameter") == "geometry";
        scene.primEntities.push_back(std::move(entity));
    }
    return scene;
}
}

// 模块：JSON 工程读取入口。
// 输入：path 工程文件（空串走内置最小示例 readXml）；mode 运行模式
//       （仅支持 eRaytracing / eVoxelRT / eVoxelEB，其他返回 false）。
// 输出：填充 m_pRaytracingXml / m_pVoxelrtXml / m_pVoxelebXml 之一；成功返回 true。
// 步骤：1 创建模式对应的配置对象；2 光伏材质与模式的匹配检查；3 公共配置；
//       4 光谱 spectra；5 热力 thermals；6 冠层 canopies；7 材质 properties；
//       8 场景 readScene 并按模式写入；9 VoxelEB 另读气象、流体、空气动力。
bool FileIO::readJson(const std::string& path, Mode mode) {
    if (path.empty()) return readXml(path, mode);  // built-in minimal example
    m_mode = mode;
    m_inputDirectory = std::filesystem::path(path).parent_path().string();
    const ProjectJson project = ProjectJson::load(path);
    if (mode == Mode::eRaytracing) m_pRaytracingXml = std::make_shared<RaytracingXml>();
    else if (mode == Mode::eVoxelRT) m_pVoxelrtXml = std::make_shared<VoxelRTXml>();
    else if (mode == Mode::eVoxelEB) m_pVoxelebXml = std::make_shared<VoxelEBXml>();
    else return false;
    // 步骤 2：光伏（photovoltaic）材质需要面元能量平衡（eFacetEB）：
    //       若 VoxelEB 工程中背景或对象使用了该类材质，则报错而不是静默忽略。
    if(mode == Mode::eVoxelEB) {
        for(const Json& material:project.materials()) {
            if(ProjectJson::string(material,"energyModel")!="photovoltaic") continue;
            const auto name=ProjectJson::string(material,"name");
            if(project.scene().contains("background") && ProjectJson::string(project.scene()["background"],"materialName")==name)
                throw std::runtime_error("Photovoltaic panels must be scene objects in eFacetEB");
            for(const Json& object:project.objects()) {
                const auto bindings=bindingNames(object,"materialName",ProjectJson::string(object,"materialName"));
                if(std::find(bindings.begin(),bindings.end(),name)!=bindings.end())
                    throw std::runtime_error("Photovoltaic energy balance requires eFacetEB");
            }
        }
    }
    // 步骤 3：公共配置（设置、传感器、光源、大气）。
    configureCommon(*this, project, mode);

    // 步骤 4：光谱材质。model 为 Prospect / BSM / file 时分别对应 PROSPECT / BSM / OTHER，
    //       其余按 CUSTOM（直接给反射率/透过率）。缺省反射率 0.20、透过率 0、
    //       热红外反射 0.05、热红外透过 0。
    std::vector<SpectralXml> spectra;
    for (const Json& item : project.spectra()) {
        SpectralXml value{};
        value.spectralName = ProjectJson::string(item, "name", "spectral");
        const std::string model = ProjectJson::string(item, "model", "custom");
        value.type = model == "Prospect" ? spectralType::PROSPECT : model == "BSM" ? spectralType::BSM : model == "file" ? spectralType::OTHER : spectralType::CUSTOM;
        value.reflectances = ProjectJson::numbers(item.contains("reflectance") ? item["reflectance"] : Json("0.20"));
        value.transmittance = ProjectJson::numbers(item.contains("transmittance") ? item["transmittance"] : Json("0.0"));
        value.refl_tir = ProjectJson::number(item, "refTir", 0.05f);
        value.tau_tir = ProjectJson::number(item, "tauTir", 0.0f);
        // Negative (or absent) keeps the built-in dry-to-wet emissivity gain for BSM soil.
        value.refl_tir_wet = ProjectJson::number(item, "refTirWet", -1.0f);
        // 镜面反射：control.specularEnabled（缺省开）与材质 specular.enabled 同时为真才启用；
        //       fraction 缺省 0.5，限幅 [0,1]；粗糙度缺省 0.25，限幅 [0.05,1]。
        if (item.contains("specular") && item["specular"].is_object()) {
            const Json& specular = item["specular"];
            value.specularEnabled = ProjectJson::boolean(project.control(), "specularEnabled", true)
                && ProjectJson::boolean(specular, "enabled", false);
            value.specularFraction = std::clamp(ProjectJson::number(specular, "fraction", 0.5f), 0.0f, 1.0f);
            value.specularRoughness = std::clamp(ProjectJson::number(specular, "roughness", 0.25f), 0.05f, 1.0f);
        }
        // PROSPECT 叶片参数 fp = {Cab, Cw, Cdm, Cs, N}，BSM 土壤参数 bsm = {SMC, 亮度, 纬度, 经度}
        // （含义以 PROSPECT / BSM 模型为准，缺省值见下）。
        const Json params = item.contains("params") ? item["params"] : Json::object();
        value.fp = {ProjectJson::number(params, "Cab", 40), ProjectJson::number(params, "Cw", .01f), ProjectJson::number(params, "Cdm", .01f), ProjectJson::number(params, "Cs", 0), ProjectJson::number(params, "N", 1.5f)};
        value.bsm = {ProjectJson::number(params, "SMC", 25), ProjectJson::number(params, "BSMBrightness", .5f), ProjectJson::number(params, "BSMlat", 25), ProjectJson::number(params, "BSMlon", 45)};
        if (value.type == spectralType::OTHER) value.path = project.resolve(ProjectJson::string(item, "fileName")).string();
        // 物理纹理：启用并给出文件时才生效；strength 限幅 [0,1]，相对标准差限幅 [0,0.5]，
        //       重复尺寸至少 0.01（单位待核对，应为米）。
        if (item.contains("physicalTexture") && item["physicalTexture"].is_object()) {
            const Json& texture = item["physicalTexture"];
            const std::string textureFile = ProjectJson::string(texture, "fileName");
            value.physicalTextureEnabled = ProjectJson::boolean(texture, "enabled", false) && !textureFile.empty();
            if (value.physicalTextureEnabled) value.physicalTexturePath = project.resolve(textureFile).string();
            value.physicalTextureStrength = std::clamp(ProjectJson::number(texture, "strength", 0.2f), 0.0f, 1.0f);
            value.physicalTextureRelativeStd = std::clamp(ProjectJson::number(texture, "relativeStd", 0.0f), 0.0f, 0.5f);
            value.physicalTextureRepeatSize = std::max(0.01f, ProjectJson::number(texture, "repeatSize", 2.0f));
        }
        spectra.push_back(std::move(value));
    }

    // 步骤 5：热力配置：光照/阴影表面温度，缺省 305 K / 295 K（温度类材质用，待核对）。
    std::vector<ThermalXml> thermals;
    for (const Json& item : project.thermals()) thermals.push_back({
        ProjectJson::string(item, "name", "temperature"),
        ProjectJson::number(item, "sunlitTemperature", 305),
        ProjectJson::number(item, "shadedTemperature", 295)
    });

    // 步骤 6：冠层结构：LAI、密度、冠高 hc、G 函数、叶倾角分布参数 LIDFa/LIDFb、热点参数 hspot、
    //       叶宽，以及结构类型编码（0 冠层，1 rigid，2 fire，3 fog）；
    //       后几项为介质（火/雾）的消光、单次散射反照率 [0,1]、不对称因子 [-0.99,0.99]、
    //       发射强度、固定温度（均非负）。
    std::vector<CanopyXml> canopies;
    for (const Json& item : project.canopies()) {
        const std::string structure = ProjectJson::string(item, "structureType", "canopy");
        canopies.push_back({ProjectJson::string(item, "name", "canopy_default"), {
            ProjectJson::number(item, "lai", 3), ProjectJson::number(item, "density", 1),
            ProjectJson::number(item, "hc", 2), 1, ProjectJson::number(item, "G", .5f),
            ProjectJson::number(item, "LIDFa", -.35f), ProjectJson::number(item, "LIDFb", -.15f),
            ProjectJson::number(item, "hspot", .2f), ProjectJson::number(item, "leafwidth", .1f),
            structure == "rigid" ? 1 : structure == "fire" ? 2 : structure == "fog" ? 3 : 0,
            std::max(0.0f, ProjectJson::number(item, "extinction", 0.0f)),
            std::clamp(ProjectJson::number(item, "scatteringAlbedo", 0.0f), 0.0f, 1.0f),
            std::clamp(ProjectJson::number(item, "asymmetry", 0.0f), -0.99f, 0.99f),
            std::max(0.0f, ProjectJson::number(item, "emissionScale", 0.0f)),
            std::max(0.0f, ProjectJson::number(item, "fixedTemperature", 0.0f))
        }});
    }

    // 步骤 7：材质属性（按类型分支）：
    //       木质 → 按 BUILDING 的半无限固体热存储求解，用木材热物性；
    //       植被 → 叶片光合/气孔参数 leafbio；
    //       水体 → 水体热容、混合深度、反射模型；
    //       其余（土壤、建筑、船）→ 土壤热参数 soilset，船体另有可选参数。
    std::vector<PropertyXml> properties;
    for (const Json& item : project.materials()) {
        PropertyXml value{};
        value.name = ProjectJson::string(item, "name", "material");
        const Json params = item.contains("params") ? item["params"] : Json::object();
        value.type = objectType(ProjectJson::string(item, "type", "Soil"));
        const bool wood = ProjectJson::string(item, "energyModel") == "wood"
            || isWoodMaterialName(value.name);
        if (wood) {
            // Wood is an impermeable solid: no photosynthesis or transpiration.
            // Reuse the mature semi-infinite solid heat-storage solver used by
            // BUILDING, with wood-specific thermal properties.
            value.type = Type::BUILDING;
            const float initialTemperature = ProjectJson::number(
                params, "Tsoil", ProjectJson::number(params, "initialTemperature", 25.0f));
            value.soilset = {
                1,
                ProjectJson::number(params, "rss", 1.0e9f),
                ProjectJson::number(params, "cs", ProjectJson::number(params, "specificHeat", 1700.0f)),
                ProjectJson::number(params, "rhos", ProjectJson::number(params, "density", 600.0f)),
                ProjectJson::number(params, "lambdas", ProjectJson::number(params, "thermalConductivity", 0.15f)),
                initialTemperature > 150.0f ? initialTemperature - 273.15f : initialTemperature,
                ProjectJson::number(params, "SMC", ProjectJson::number(params, "moisture", 0.0f)),
                ProjectJson::number(params, "Satwater", 0.0f)
            };
            value.soilset.thermalClass = 1;
            value.soilset.convectiveScale = std::max(
                0.1f, ProjectJson::number(params, "convectiveScale",
                    value.name == "tree_branch_wood" ? 1.5f : 1.2f));
        } else if (value.type == Type::VEGETATION) {
            // 植被温度响应参数 Tparam 需要 5 个值；不足时用内置缺省值补齐并给出警告。
            // 含义与 Vcmax、BallBerry 等叶片参数同属光合模型参数（各项含义待核对）。
            auto t = ProjectJson::numbers(params.contains("Tparam") ? params["Tparam"] : Json("0.2,0.3,288,313,328"));
            if (t.size() < 5) {
                std::cerr << "Warning: Tparam has " << t.size()
                          << " value(s); missing entries use the built-in default Tparam." << std::endl;
                const std::vector<float> tparamDefaults = ProjectJson::numbers(Json("0.2,0.3,288,313,328"));
                for (std::size_t i = t.size(); i < 5 && i < tparamDefaults.size(); ++i) {
                    t.push_back(tparamDefaults[i]);
                }
            }
            value.leafbio = {ProjectJson::number(params,"Vcmax",80),ProjectJson::number(params,"m",9),ProjectJson::number(params,"BallBerry",.01f),ProjectJson::number(params,"Type",3),ProjectJson::number(params,"kV",.6396f),ProjectJson::number(params,"Rdparam",.015f),{t[0],t[1],t[2],t[3],t[4]},ProjectJson::number(params,"Tyear",25),ProjectJson::number(params,"beta",.507f),ProjectJson::number(params,"kNPQs",0),ProjectJson::number(params,"qLs",1),ProjectJson::number(params,"stressfactor",1),ProjectJson::integer(params,"Tcor",0)};
        } else if (value.type == Type::WATER) {
            // 水体：热容缺省 4.186e6（J m-3 K-1，待核对）、混合深度 0.5 m；
            //       brdfModel 仅 1 有效否则置 0；折射率下限 1，缺省 1.333；
            //       slopeVariance 非负；diffuseFraction 限幅 [0,1]，缺省 0.02。
            value.waterset = {ProjectJson::number(params,"rss",0),ProjectJson::number(params,"heatCapacity",4186000),ProjectJson::number(params,"mixingDepth",.5f),ProjectJson::number(params,"evaporationCoefficient",1)};
            value.waterset.brdfModel = ProjectJson::integer(params, "brdfModel", 1) == 1 ? 1 : 0;
            value.waterset.refractiveIndex = std::max(1.0f, ProjectJson::number(params, "refractiveIndex", 1.333f));
            value.waterset.slopeVariance = std::max(0.0f, ProjectJson::number(params, "slopeVariance", 0.0f));
            value.waterset.diffuseFraction = std::clamp(ProjectJson::number(params, "diffuseFraction", 0.02f), 0.0f, 1.0f);
        } else {
            // Buildings and mobile solids share the soil thermal parameter
            // layout, but retain their impermeable-solid physical type.
            if (value.type != Type::BUILDING) value.type = Type::SOIL;
            // DEFERRED[低] 物理: 土壤/建筑的 Tsoil 默认 25（摄氏）且原样传入，而上面的木材分支对 >150 的值自动做 K→°C 转换；用户按开尔文填写 Tsoil=300 时土壤初温变为 300 °C；SMC 默认 25（百分比）与 Satwater 0.45（体积分数）单位也不一致；建议：与木材分支统一温度单位处理，并统一含水量为体积分数。
            value.soilset = {std::clamp(ProjectJson::integer(project.control(),"soilTemperatureMethod",ProjectJson::integer(params,"method",1)),0,2),ProjectJson::number(params,"rss",2000),ProjectJson::number(params,"cs",1180),ProjectJson::number(params,"rhos",1800),ProjectJson::number(params,"lambdas",1.55f),ProjectJson::number(params,"Tsoil",25),ProjectJson::number(params,"SMC",25),ProjectJson::number(params,"Satwater",.45f)};
            // Method 1 (semi-infinite conduction): "full" adds the history older
            // than TLASTNUM nodes as exponential modes; absent/"truncated" keeps
            // the legacy 8-node Duhamel sum (bitwise).  Wood (thermalClass 1,
            // finite branches) always keeps the 8-node sum.
            value.soilset.inertiaHistory =
                ProjectJson::string(project.control(), "soilInertiaHistory", "truncated") == "full" ? 1 : 0;
            // Ship hull options (minimal VoxelEB version, all default off):
            // speed through the air, course vs. wind-from direction, and a
            // fixed waterline below which hull voxels exchange heat with the
            // scene water. Same field names as the FacetEB Ship material.
            const std::string typeName = ProjectJson::string(item, "type", "Soil");
            // 船体可选参数（仅 BUILDING 且类型名为 Ship）：航速、航向与风向夹角余弦、
            //       吃水线高度及水热交换系数、内热源，以及 12 mm 钢板的等效单层壁；
            //       各项都校验范围，越界直接报错。
            if (value.type == Type::BUILDING && (typeName == "Ship" || typeName == "ship")) {
                SoilSet& hull = value.soilset;
                const float speed = ProjectJson::number(params, "speed", 0.0f);
                if (!std::isfinite(speed) || speed < 0.0f || speed > 100.0f)
                    throw std::runtime_error("Ship speed must be 0-100 m/s: " + value.name);
                hull.shipSpeed = speed;
                const float heading = ProjectJson::number(params, "heading", -1.0f);
                const float windFrom = ProjectJson::number(params, "windDirection", -1.0f);
                if (heading >= 0.0f && windFrom >= 0.0f) {
                    if (!std::isfinite(heading) || !std::isfinite(windFrom) || heading > 360.0f || windFrom > 360.0f)
                        throw std::runtime_error("Ship heading and wind direction must be 0-360 degrees: " + value.name);
                    hull.shipWindCosine = static_cast<float>(
                        std::cos((double(heading) - double(windFrom)) * 3.14159265358979323846 / 180.0));
                }
                if (params.contains("waterlineHeight") && params["waterlineHeight"].is_number()) {
                    hull.waterlineHeight = ProjectJson::number(params, "waterlineHeight", 0.0f);
                    hull.waterHeatTransferCoefficient = ProjectJson::number(params, "waterHeatTransferCoefficient", 500.0f);
                    if (!std::isfinite(hull.waterlineHeight) || !std::isfinite(hull.waterHeatTransferCoefficient) ||
                        !(hull.waterHeatTransferCoefficient > 0.0f))
                        throw std::runtime_error("Invalid ship waterline parameters: " + value.name);
                }
                // Internal heat source (W m-2 of hull surface; same field as
                // the FacetEB Ship material).
                hull.internalHeatFlux = ProjectJson::number(params, "internalHeatFlux", 0.0f);
                if (!std::isfinite(hull.internalHeatFlux) || std::abs(hull.internalHeatFlux) > 1.0e5f)
                    throw std::runtime_error("Invalid internal heat source (|flux| <= 1e5 W/m2): " + value.name);
                // 12 mm steel plate (wallMode "wall"): VoxelEB only switches it
                // on with the explicit voxelWall flag, so projects saved
                // before it existed keep their semi-infinite solid hull.
                if (ProjectJson::string(params, "wallMode", "shell") == "wall" &&
                    ProjectJson::boolean(params, "voxelWall", false)) {
                    // 多层壁：总厚度、总热阻 Σ t/k、总热容 Σ t·rho·cp；至多 6 层；
                    //       无 layers 时用单层的 thickness / lambdas / rhos / cs。
                    double thickness = ProjectJson::number(params, "thickness", 0.2f);
                    double resistance = 0.0, capacity = 0.0;
                    if (params.contains("layers") && params["layers"].is_array() && !params["layers"].empty()) {
                        if (params["layers"].size() > 6)
                            throw std::runtime_error("Ship wall has more than 6 layers: " + value.name);
                        thickness = 0.0;
                        for (const auto& layer : params["layers"]) {
                            const double t = layer.value("thickness", 0.0), k = layer.value("lambda", 0.0);
                            const double rho = layer.value("rho", 0.0), cp = layer.value("cp", 0.0);
                            if (!(t > 0.0 && k > 0.0 && rho > 0.0 && cp > 0.0))
                                throw std::runtime_error("Invalid ship wall layer (thickness lambda rho cp): " + value.name);
                            thickness += t;
                            resistance += t / k;
                            capacity += t * rho * cp;
                        }
                    } else {
                        resistance = thickness / hull.lambdas;
                        capacity = thickness * double(hull.rhos) * double(hull.cs);
                    }
                    if (!(thickness > 0.0 && thickness <= 5.0) || !(resistance > 0.0) || !(capacity > 0.0))
                        throw std::runtime_error("Invalid ship wall (0 < thickness <= 5 m, lambda, rho, cp > 0): " + value.name);
                    // One equivalent homogeneous plate: same total thickness,
                    // thermal resistance and heat capacity as the layers.
                    hull.plateThickness = static_cast<float>(thickness);
                    hull.plateLambda = static_cast<float>(thickness / resistance);
                    hull.plateCapacity = static_cast<float>(capacity / thickness);
                    hull.plateIndoorC = ProjectJson::number(params, "indoorTemperature", 22.0f);
                    hull.plateIndoorH = ProjectJson::number(params, "indoorHeatTransferCoefficient", 8.0f);
                    if (!(hull.plateIndoorC > -70.0f && hull.plateIndoorC < 120.0f) || !(hull.plateIndoorH > 0.0f))
                        throw std::runtime_error("Invalid ship wall indoor side (-70 < T < 120 C, h > 0): " + value.name);
                }
            }
        }
        properties.push_back(std::move(value));
    }

    // 步骤 8：读取场景（RayTracing 的镜面覆盖会向 spectra 追加私有光谱），
    //       再按模式写入：RayTracing 只需场景、光谱、热力；VoxelRT 另需冠层与材质；
    //       其余（VoxelEB）还要读气象、流体与空气动力配置。
    const SceneXml scene = readScene(project, mode, spectra);
    if (mode == Mode::eRaytracing) {
        m_pRaytracingXml->scenexml = scene; m_pRaytracingXml->spectralxmls = spectra; m_pRaytracingXml->thermalxmls = thermals;
    } else if (mode == Mode::eVoxelRT) {
        m_pVoxelrtXml->scenexml = scene; m_pVoxelrtXml->spectralxmls = spectra; m_pVoxelrtXml->thermalxmls = thermals; m_pVoxelrtXml->canopyxmls = canopies; m_pVoxelrtXml->propxmls = properties;
    } else {
        m_pVoxelebXml->scenexml = scene; m_pVoxelebXml->spectralxmls = spectra; m_pVoxelebXml->thermalxmls = thermals; m_pVoxelebXml->canopyxmls = canopies; m_pVoxelebXml->propxmls = properties;
        // 步骤 9a：气象时间窗 [start, end)（end 至少比 start 大 1）、气象文件路径、
        //       参考高度 z（缺省 15 m）、初始土壤温度 Tsold、饱和含水量 SatWater、
        //       时间步 dTime（秒，必须为正，缺省 1800）、大气 CO2 浓度 Ca（限幅 100–2000，
        //       单位待核对，应为 ppm）。
        const Json& meteo = project.meteorology();
        m_pVoxelebXml->meteoxml.startTimeNode = std::max(0, ProjectJson::integer(meteo,"start",0));
        m_pVoxelebXml->meteoxml.endTimeNode = std::max(m_pVoxelebXml->meteoxml.startTimeNode+1, ProjectJson::integer(meteo,"end",1));
        m_pVoxelebXml->meteoxml.meteofile = project.meteorologyPath().string();
        m_pVoxelebXml->meteoxml.meta.z = ProjectJson::number(meteo,"z",15);
        m_pVoxelebXml->meteoxml.meta.Tsold = ProjectJson::number(meteo,"Tsold",300);
        m_pVoxelebXml->meteoxml.meta.SatWater = ProjectJson::number(meteo,"SatWater",.45f);
        m_pVoxelebXml->meteoxml.meta.dTime = ProjectJson::number(meteo,"dTime",1800);
        if (!(m_pVoxelebXml->meteoxml.meta.dTime > 0.0f))
            throw std::runtime_error("meteo.dTime must be a positive number of seconds");
        m_pVoxelebXml->meteoxml.meta.Ca = std::clamp(ProjectJson::number(meteo,"Ca",380), 100.0f, 2000.0f);
        // 步骤 9b：流体（风场）配置：开关、时间步（≥0.1 s）、压力迭代次数（限幅 2–100）、
        //       风向、浮力、拖曳、热耦合、扩散、烟雾发射、最大风速等，均做非负/下限处理；
        //       输出高度非正时回退 2 m；流体体元尺寸缺省取辐射体元尺寸。
        const Json& fluid = project.fluid();
        const float configuredFluidOutputHeight =
            ProjectJson::number(fluid, "outputHeight", 2.0f);
        const float radiativeVoxelSize = std::max(
            0.01f, ProjectJson::number(project.scene(), "voxel", 1.0f));
        m_pVoxelebXml->fluidxml = {
            ProjectJson::boolean(fluid, "enabled", false),
            std::max(0.1f, ProjectJson::number(fluid, "timeStep", 5.0f)),
            std::clamp(ProjectJson::integer(fluid, "pressureIterations", 20), 2, 100),
            ProjectJson::number(fluid, "windDirection", 0.0f),
            std::max(0.0f, ProjectJson::number(fluid, "buoyancy", 0.033f)),
            std::max(0.0f, ProjectJson::number(fluid, "dragCoefficient", 0.3f)),
            std::max(0.0f, ProjectJson::number(fluid, "thermalCoupling", 1.0f)),
            std::max(0.0f, ProjectJson::number(fluid, "diffusivity", 0.1f)),
            std::max(0.0f, ProjectJson::number(fluid, "smokeEmission", 0.02f)),
            std::max(0.1f, ProjectJson::number(fluid, "maxVelocity", 30.0f)),
            ProjectJson::boolean(fluid, "outputWind", false),
            ProjectJson::boolean(fluid, "outputAirTemperature", false),
            configuredFluidOutputHeight > 0.0f ? configuredFluidOutputHeight : 2.0f,
            std::max(0.1f, ProjectJson::number(fluid, "voxelSize", radiativeVoxelSize)),
            ProjectJson::boolean(fluid, "localExchange", false)
        };
        // 步骤 9c：缓存气象配置副本，并设置空气动力配置（见下方说明）。
        m_meteoXml = m_pVoxelebXml->meteoxml;
        // Zero height/leaf width: aeresist uses each canopy's own parameters.
        // L=0 is the neutral default; control.stabilityCorrection replaces it
        // by the scene 1/L of the Monin-Obukhov iteration (command.cpp).
        m_pVoxelebXml->aerocondxml = {AeroType::ONE,{0,0,.3f,0,1,0,0,0},"",1000};
    }
    return true;
}
