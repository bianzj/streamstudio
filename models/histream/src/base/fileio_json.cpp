#include "projectjson.h"
#include "fileio.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace {
using Json = ProjectJson::Json;

Type objectType(const std::string& value) {
    if (value == "Soil" || value == "soil") return Type::SOIL;
    if (value == "Building" || value == "building" || value == "Human" || value == "human" ||
        value == "Vehicle" || value == "vehicle" ||
        value == "Ship" || value == "ship") return Type::BUILDING;
    if (value == "Water" || value == "water") return Type::WATER;
    if (value == "Vegetation" || value == "vegetation" || value == "Fire" || value == "Fog") return Type::VEGETATION;
    return Type::OTHER;
}

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

bool isWoodMaterialName(const std::string& name) {
    return name == "tree_branch_wood" || name == "tree_trunk_wood";
}

bool isWoodMaterial(const ProjectJson& project, const std::string& name) {
    if (isWoodMaterialName(name)) return true; // Migrates projects made before energyModel.
    for (const auto& material : project.materials()) {
        if (ProjectJson::string(material, "name") == name)
            return ProjectJson::string(material, "energyModel") == "wood";
    }
    return false;
}

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

std::vector<float> dimensions(const Json& item) {
    auto values = ProjectJson::numbers(item.contains("dimensions") ? item["dimensions"] : Json{});
    while (values.size() < 3) values.push_back(1.0f);
    for (float& value : values) if (!(value > 0.0f)) value = 1.0f;
    return values;
}

float wrapCoordinate(float value, float extent) {
    if (!(extent > 0.0f)) return 0.0f;
    const float wrapped = std::fmod(value, extent);
    return wrapped < 0.0f ? wrapped + extent : wrapped;
}

struct CruiseRoute {
    std::vector<glm::vec3> positions;
    std::vector<float> headings;
};

float normalizedHeading(float value) {
    value = std::fmod(value, 360.0f);
    return value < 0.0f ? value + 360.0f : value;
}

CruiseRoute cruiseRoute(const Json& sensor, const Json& scene) {
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

void configureCommon(FileIO& io, const ProjectJson& project, Mode mode) {
    const Json& control = project.control();
    const Json& sensor = project.sensor();
    const Json& light = project.light();
    const Json& scene = project.scene();

    SettingXml settings{};
    settings.maxDepth = std::max(1, ProjectJson::integer(control, "depth", 4));
    settings.theGPU = std::max(0, ProjectJson::integer(control, "gpu", 0));
    settings.computeBackend = ProjectJson::string(control, "computeBackend", "gpu");
    settings.n_sample = std::clamp(ProjectJson::integer(control, "samples", 32), 1, 1024);
    settings.heterogeneousVoxel = ProjectJson::boolean(control, "heterogeneousVoxel", false);
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
    settings.shortwaveScatteringOrders = ProjectJson::integer(control, "shortwaveScatteringOrders", 1);
    if (settings.shortwaveScatteringOrders < 1 || settings.shortwaveScatteringOrders > 3)
        throw std::runtime_error("control.shortwaveScatteringOrders must be 1, 2 or 3");
    settings.voxelCollisionOrders = std::clamp(
        ProjectJson::integer(control, "voxelCollisionOrders", 2), 2, 5);
    settings.radiationMaxSteps = std::clamp(
        ProjectJson::integer(control, "radiationMaxSteps", 64), 1, 4096);
    const bool perspective = ProjectJson::string(sensor, "projection", "parallel") == "perspective";
    settings.isUAVtrave = perspective &&
        (ProjectJson::boolean(sensor, "cruiseEnabled", false) ||
         ProjectJson::boolean(control, "uavTraverse", false));

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
    CruiseRoute route{};
    if (settings.isUAVtrave) {
        route = cruiseRoute(sensor, scene);
        sensorData.uavPoses = route.positions;
    }
    sensorData.waves = project.sensorBands();
    const float cruiseHeading = route.headings.empty()
        ? ProjectJson::number(sensor, "cruiseHeading", 90.0f) : route.headings.front();
    // FIXED[中] 错误处理: project.viewAngles() 为空时 sensorData.viewAngles 为空，Geometry::createGeometry 随后 angles[0] 越界；已修复：解析完成仍为空时补一个天底观测 {0,0}（与 Geometry 侧的回退一致）；角度范围（0<=vza<90）、zenith/azimuth/skyTemperature 范围及 sza>=90 时直射处理属于物理约定，未改动（见报告）。
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
    if (sensorData.viewAngles.empty()) {
        std::cerr << "Warning: no viewAngle in project sensor; falling back to a single nadir view {0,0}." << std::endl;
        sensorData.viewAngles.emplace_back(0.0f, 0.0f);
    }
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

    LightXml lightData{};
    lightData.name = "Solar";
    lightData.solarAngle = {ProjectJson::number(light, "zenith", 30.0f),
                            ProjectJson::number(light, "azimuth", 135.0f)};
    lightData.direct = std::clamp(ProjectJson::number(light, "direct", 0.8f), 0.0f, 1.0f);
    lightData.diffuse = 1.0f - lightData.direct;
    lightData.skyTemperature = ProjectJson::number(light, "skyTemperature", 250.0f);
    lightData.solarTemperature = 6000.0f;
    const AtmosphereXml atmosphere = readAtmosphere(project);

    const std::string output = project.outputDirectory().string();
    // DEFERRED[中] 安全: ProjectJson::resolve 特意用 u8path 保留 UTF-8 路径，但这里及 readScene/readJson 中各处 .string() 又在 Windows 上按 ANSI 代码页转回窄字符串；路径含代码页外字符时 MSVC 抛 std::system_error，代码页内字符则依赖后续所有 fopen/GDALOpen 也用 ANSI；建议：结构体中路径统一存 std::filesystem::path（或 u8string），打开文件时用宽字符 API。
    const std::string defined = project.runtimeDefinedDirectory().parent_path().string();
    if (mode == Mode::eRaytracing) {
        io.m_pRaytracingXml->projectDir = output;
        io.m_pRaytracingXml->definedDir = defined;
        io.m_pRaytracingXml->settingxml = settings;
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

SceneXml readScene(const ProjectJson& project, Mode mode, std::vector<SpectralXml>& opticalMaterials) {
    const Json& source = project.scene();
    SceneXml scene{};
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
    scene.background.sceneSize = {
        std::max(0.01f, ProjectJson::number(source, "x", 60.0f)),
        std::max(0.01f, ProjectJson::number(source, "y", 60.0f)),
        std::max(0.0f, ProjectJson::number(source, "height", 15.0f))
    };
    scene.background.sceneOrigin = {0, 0, 0};
    scene.background.stepsize_surface = scene.background.stepsize_height =
        std::max(0.01f, ProjectJson::number(source, "voxel", 1.0f));
    scene.background.voxelFillThreshold = std::clamp(ProjectJson::number(source, "voxelFillThreshold", 0.05f), 0.0f, 1.0f);
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
    scene.background.lat = std::clamp(ProjectJson::number(project.meteorology(), "latitude", 40.0f), -90.0f, 90.0f);
    scene.background.lon = std::clamp(ProjectJson::number(project.meteorology(), "longitude", 116.0f), -180.0f, 180.0f);
    scene.background.year = std::clamp(ProjectJson::integer(project.meteorology(), "year", 2019), -2000, 6000);
    scene.background.timezone = std::clamp(ProjectJson::number(project.meteorology(), "timezone", 8.0f), -18.0f, 18.0f);

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
        const bool standardMediumAxes = medium && item.contains("medium")
            && item["medium"].is_object()
            && ProjectJson::string(item["medium"], "coordinateOrder", "") == "XYZ";
        const std::string model = medium ? std::string{} : project.resolve(ProjectJson::string(item, "fileName")).string();
        const std::string positions = project.resolve(ProjectJson::string(item, "positionFile")).string();

        if (mode == Mode::eRaytracing) {
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

bool FileIO::readJson(const std::string& path, Mode mode) {
    if (path.empty()) return readXml(path, mode);  // built-in minimal example
    m_mode = mode;
    m_inputDirectory = std::filesystem::path(path).parent_path().string();
    const ProjectJson project = ProjectJson::load(path);
    if (mode == Mode::eRaytracing) m_pRaytracingXml = std::make_shared<RaytracingXml>();
    else if (mode == Mode::eVoxelRT) m_pVoxelrtXml = std::make_shared<VoxelRTXml>();
    else if (mode == Mode::eVoxelEB) m_pVoxelebXml = std::make_shared<VoxelEBXml>();
    else return false;
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
    configureCommon(*this, project, mode);

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
        if (item.contains("specular") && item["specular"].is_object()) {
            const Json& specular = item["specular"];
            value.specularEnabled = ProjectJson::boolean(project.control(), "specularEnabled", true)
                && ProjectJson::boolean(specular, "enabled", false);
            value.specularFraction = std::clamp(ProjectJson::number(specular, "fraction", 0.5f), 0.0f, 1.0f);
            value.specularRoughness = std::clamp(ProjectJson::number(specular, "roughness", 0.25f), 0.05f, 1.0f);
        }
        const Json params = item.contains("params") ? item["params"] : Json::object();
        value.fp = {ProjectJson::number(params, "Cab", 40), ProjectJson::number(params, "Cw", .01f), ProjectJson::number(params, "Cdm", .01f), ProjectJson::number(params, "Cs", 0), ProjectJson::number(params, "N", 1.5f)};
        value.bsm = {ProjectJson::number(params, "SMC", 25), ProjectJson::number(params, "BSMBrightness", .5f), ProjectJson::number(params, "BSMlat", 25), ProjectJson::number(params, "BSMlon", 45)};
        if (value.type == spectralType::OTHER) value.path = project.resolve(ProjectJson::string(item, "fileName")).string();
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

    std::vector<ThermalXml> thermals;
    for (const Json& item : project.thermals()) thermals.push_back({
        ProjectJson::string(item, "name", "temperature"),
        ProjectJson::number(item, "sunlitTemperature", 305),
        ProjectJson::number(item, "shadedTemperature", 295)
    });

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
            auto t = ProjectJson::numbers(params.contains("Tparam") ? params["Tparam"] : Json("0.2,0.3,288,313,328"));
            // FIXED[低] 数值: Tparam 不足 5 个时原先用 0 补齐，后三项是温度（K），补 0 会使光合温度响应函数出现 1/T 或 exp(…/(R·T)) 的除零/溢出；已修复：缺失项改用同一分支已内置的默认 Tparam 值（0.2,0.3,288,313,328）逐位补齐并告警，不再补 0。
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
        }
        properties.push_back(std::move(value));
    }

    const SceneXml scene = readScene(project, mode, spectra);
    if (mode == Mode::eRaytracing) {
        m_pRaytracingXml->scenexml = scene; m_pRaytracingXml->spectralxmls = spectra; m_pRaytracingXml->thermalxmls = thermals;
    } else if (mode == Mode::eVoxelRT) {
        m_pVoxelrtXml->scenexml = scene; m_pVoxelrtXml->spectralxmls = spectra; m_pVoxelrtXml->thermalxmls = thermals; m_pVoxelrtXml->canopyxmls = canopies; m_pVoxelrtXml->propxmls = properties;
    } else {
        m_pVoxelebXml->scenexml = scene; m_pVoxelebXml->spectralxmls = spectra; m_pVoxelebXml->thermalxmls = thermals; m_pVoxelebXml->canopyxmls = canopies; m_pVoxelebXml->propxmls = properties;
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
        m_meteoXml = m_pVoxelebXml->meteoxml;
        // Zero height/leaf width: aeresist uses each canopy's own parameters.
        m_pVoxelebXml->aerocondxml = {AeroType::ONE,{0,0,.3f,0,1,0,0,0},"",1000};
    }
    return true;
}
