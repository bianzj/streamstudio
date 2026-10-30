//
// Created by admin on 2024/1/24.
//

#include "projectjson.h"
#include "geometry.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <fstream>

namespace {

constexpr double kOrthographicScale = 1.0;

glm::vec3 observationCameraUp(float zenithDegrees, float azimuthDegrees)
{
    const float zenith = zenithDegrees * DEG2RAD;
    const float azimuth = azimuthDegrees * DEG2RAD;
    const glm::vec3 cameraOut{
        std::sin(zenith) * std::cos(azimuth),
        std::cos(zenith),
        std::sin(zenith) * std::sin(azimuth)};
    const glm::vec3 forward = -glm::normalize(cameraOut);
    // +X is north. Project geographic north into the image plane so native
    // output rows are already north-up and image columns increase eastward.
    const glm::vec3 north{1.0f, 0.0f, 0.0f};
    glm::vec3 up = north - glm::dot(north, forward) * forward;
    if (glm::dot(up, up) < 1.0e-12f) {
        const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
        up = worldUp - glm::dot(worldUp, forward) * forward;
    }
    up = glm::normalize(up);
    glm::vec3 right = glm::cross(forward, up);
    if (glm::dot(right, right) < 1.0e-12f) right = {0.0f, 0.0f, 1.0f};
    else right = glm::normalize(right);
    return glm::normalize(glm::cross(right, forward));
}

glm::vec3 perspectiveCameraUp(float zenithDegrees, float azimuthDegrees)
{
    const float zenith = zenithDegrees * DEG2RAD;
    const float azimuth = azimuthDegrees * DEG2RAD;
    const glm::vec3 cameraOut{
        std::sin(zenith) * std::cos(azimuth),
        std::cos(zenith),
        std::sin(zenith) * std::sin(azimuth)};
    const glm::vec3 forward = -glm::normalize(cameraOut);
    // Perspective imagery behaves like a forward-looking camera: keep the
    // horizon horizontal. At nadir, vertical-up is degenerate, so use north-up.
    const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
    glm::vec3 up = worldUp - glm::dot(worldUp, forward) * forward;
    if (glm::dot(up, up) < 1.0e-12f) {
        const glm::vec3 north{1.0f, 0.0f, 0.0f};
        up = north - glm::dot(north, forward) * forward;
    }
    up = glm::normalize(up);
    glm::vec3 right = glm::cross(forward, up);
    if (glm::dot(right, right) < 1.0e-12f) right = {0.0f, 0.0f, 1.0f};
    else right = glm::normalize(right);
    return glm::normalize(glm::cross(right, forward));
}

void transferToComputeBarrier(vk::CommandBuffer command, VkBuffer buffer, VkDeviceSize size)
{
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = size;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 1, &barrier, 0, nullptr);
}

void solveNadirMapping(float imageWidth, float imageHeight,
                       float sceneWidth, float sceneDepth,
                       const glm::vec2& leftUpper, const glm::vec2& leftBottom,
                       const glm::vec2& rightUpper, const glm::vec2& rightBottom,
                       Eigen::VectorXd& cx, Eigen::VectorXd& cy)
{
    // Scale factor is fixed at 1.0: preserve the real scene aspect ratio and
    // fit it once into the nadir raster without the legacy 0.707 shrink.
    // Pixel indices address sample centres.  The raster boundaries therefore
    // lie at -0.5 and size-0.5.  Fitting scene corners to 0 and size-1 puts
    // the outer samples exactly on triangle boundaries and creates a ring of
    // false zero/no-hit pixels, most visibly as a nadir-angle dip.
    const double rasterWidth = std::max(1.0, static_cast<double>(imageWidth));
    const double rasterHeight = std::max(1.0, static_cast<double>(imageHeight));
    const double groundPerPixel = std::max(
        static_cast<double>(sceneDepth) / rasterWidth,
        static_cast<double>(sceneWidth) / rasterHeight);
    const double targetWidth = static_cast<double>(sceneDepth) / groundPerPixel
                               * kOrthographicScale;
    const double targetHeight = static_cast<double>(sceneWidth) / groundPerPixel
                                * kOrthographicScale;
    const double x0 = (rasterWidth - targetWidth) * 0.5 - 0.5;
    const double x1 = x0 + targetWidth;
    const double y0 = (rasterHeight - targetHeight) * 0.5 - 0.5;
    const double y1 = y0 + targetHeight;

    Eigen::MatrixXd design(4, 4);
    design << x1, y0, x1 * y0, 1.0,
              x1, y1, x1 * y1, 1.0,
              x0, y0, x0 * y0, 1.0,
              x0, y1, x0 * y1, 1.0;
    Eigen::VectorXd sourceX(4), sourceY(4);
    // Destination raster: top is north, right is east.
    sourceX << rightUpper.x, rightBottom.x, leftUpper.x, leftBottom.x;
    sourceY << rightUpper.y, rightBottom.y, leftUpper.y, leftBottom.y;
    cx = design.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(sourceX);
    cy = design.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(sourceY);
}

} // namespace


LightSet Geometry::createLight(float sza, float saa, float direct, float diffuse,float solarT,float skyT)
{
    LightSet light;
    // DEFERRED[低] 数值: 用浮点精确相等判断 0°/45°，44.9999 之类的值不会被偏移；下方 saa 修正用 int() 向零截断，负方位角（如 -45°）时取到错误的 45° 倍数；且这些修正会静默改变用户给的太阳角；建议：改用 std::fabs(sza-45)<eps 与 std::floor/fmod 处理方位角，并在注释中说明修正原因（避开体元面对齐的退化方向）。
    if (sza == 0.0 || sza == 45.0) sza = sza + ANGLE_COR;

    // if (abs(saa - int((saa +ANGLE_COR)/ 45.0) * 45.0) < 0.05) saa = saa + ANGLE_COR;
    if ((saa - int((saa +ANGLE_COR) / 45.0) * 45.0) < 0.05 && (saa - int((saa +ANGLE_COR) / 45.0) * 45.0) > 0)// saa比45的倍数大一点
        saa = saa + ANGLE_COR * 2;
    if ((saa - int((saa +ANGLE_COR) / 45.0) * 45.0) > -0.05 && (saa - int((saa +ANGLE_COR) / 45.0) * 45.0) < 0)// saa比45的倍数小一点
        saa = saa - ANGLE_COR * 2;


    float r = SENSOR_HEIGHT;
    float rd = DEG2RAD;
    glm::vec3 origin = glm::vec3(0, 0, 0);
    // Azimuth is clockwise from north: +X is north and +Z is east.
    glm::vec3 lightPos = glm::vec3(r * std::sin(sza * rd) * std::cos(saa * rd),
                                   r * std::cos(sza * rd),
                                   r * std::sin(sza * rd) * std::sin(saa * rd));

    light.direction = lightPos;
    light.direct = direct;
    light.diffuse = diffuse;
    light.skyTemperature = skyT;
    light.solarTemperature = solarT;

    return light;
}

SensorMatrix Geometry::createSensor(glm::vec3 size, glm::vec3 origen, float vza, float vaa, float ratio) {

    // auto & sceneio = modelio->m_sceneio;

    SensorMatrix sensor;

    glm::vec3 semi = { size.x / 2.0, 0, size.z / 2.0 };
    glm::vec3 dimensionMin = -semi + glm::vec3{ origen.x, 0, origen.z };
    glm::vec3 dimensionMax = semi + glm::vec3{ origen.x, 0, origen.z };

    //float scale = m_pRaytracingXml->scene.stepSize;
    dimensionMin.y = 0;
    dimensionMax.y = 0;
    float r = SENSOR_HEIGHT;
    float rd = DEG2RAD;
    glm::vec3 origin = glm::vec3(0, 0, 0);
    glm::vec3 sensorPos = glm::vec3(r * std::sin(vza * rd) * std::cos(vaa * rd),
                                    r * std::cos(vza * rd),
                                    r * std::sin(vza * rd) * std::sin(vaa * rd));

    CameraManip.setFov(SENSOR_FOV);
    CameraManip.setLookat(sensorPos, origin, observationCameraUp(vza, vaa));
    float fovv = CameraManip.getFov();




    //CameraManip.fit(dimensionMin * ratio / scale, dimensionMax * ratio / scale); // the sensor position height is changed.
    CameraManip.fit(dimensionMin * ratio, dimensionMax * ratio); // the sensor position height is changed.


    float width = CameraManip.getWidth();
    float height = CameraManip.getHeight();
    const float aspectRatio = CameraManip.getWidth() / static_cast<float>(CameraManip.getHeight());
    glm::mat4 view = CameraManip.getMatrix();
    nvmath::mat4f projj = nvmath::perspectiveVK(CameraManip.getFov(), aspectRatio, 0.0001f, 10000.0f);
    // glm::mat4 proj = projglm::perspective(glm::radians(CameraManip.getFov()), aspectRatio, 0.01f, 1000.0f);
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(CameraManip.getFov()), aspectRatio, 0.01f, 10000.0f);
    proj[1][1] *= -1;
    sensor.viewInverse = glm::inverse(view);
    sensor.projInverse = glm::inverse(proj);
    glm::vec3 eye, center, currentUp;
    CameraManip.getLookat(eye, center, currentUp);                       // get sensor (eye) and center.

    float fov = CameraManip.getFov();
    sensor.focalDist = glm::length(center - eye);
    sensor.aperture = 0.0;
    sensor.direction = eye - center;


    //  sensor.n_wave = modelio->waves.size();





    return sensor;
}

SensorMatrix Geometry::createSensor(glm::vec3 sensorPos_XZY, glm::vec3 center_XZY,
                                    glm::vec3 sceneSize_XZY, float fovDegrees,
                                    glm::vec3 up) {

    // auto & sceneio = modelio->m_sceneio;

    SensorMatrix sensor;

    const float distance = std::max(0.01f, glm::length(sensorPos_XZY - center_XZY));
    const float aspect = std::max(0.01f, CameraManip.getWidth() /
                                           static_cast<float>(CameraManip.getHeight()));
    const float radius = std::max(0.01f, 0.5f * glm::length(sceneSize_XZY));
    const float requiredHalfHeight = radius / std::min(1.0f, aspect);
    const float fittedFov = glm::degrees(2.0f * std::atan(requiredHalfHeight / distance)) * 1.02f;
    CameraManip.setFov(std::clamp(fovDegrees > 0.0f ? fovDegrees : fittedFov, 0.1f, 120.0f));
    CameraManip.setLookat(sensorPos_XZY, center_XZY, up);
    float fovv = CameraManip.getFov();


    //CameraManip.fit(dimensionMin * ratio / scale, dimensionMax * ratio / scale); // the sensor position height is changed.
    //CameraManip.fit(dimensionMin * ratio, dimensionMax * ratio); // the sensor position height is changed.


    float width = CameraManip.getWidth();
    float height = CameraManip.getHeight();
    const float aspectRatio = CameraManip.getWidth() / static_cast<float>(CameraManip.getHeight());
    glm::mat4 view = CameraManip.getMatrix();
    nvmath::mat4f projj = nvmath::perspectiveVK(CameraManip.getFov(), aspectRatio, 0.0001f, 10000.0f);
    //glm::mat4 proj = glm::perspective(CameraManip.getFov(), aspectRatio, 0.0001f, 10000.0f);
    const float farPlane = std::max(10000.0f, distance + radius * 4.0f);
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(CameraManip.getFov()), aspectRatio, 0.01f, farPlane);
    proj[1][1] *= -1;
    sensor.viewInverse = glm::inverse(view);
    sensor.projInverse = glm::inverse(proj);
    glm::vec3 eye, center, currentUp;
    CameraManip.getLookat(eye, center, currentUp);                       // get sensor (eye) and center.

    float fov = CameraManip.getFov();
    sensor.focalDist = glm::length(center - eye);
    sensor.aperture = 0.0;
    sensor.direction = eye - center;


    //  sensor.n_wave = modelio->waves.size();





    return sensor;
}

void Geometry::configureSensor(const SensorXml& sensor, glm::vec3 sceneSize_XYZ,
                               float metresPerUnit) {
    const float scale = std::max(0.0001f, metresPerUnit);
    m_sensorMetresPerUnit = scale;
    m_sensorProjection = sensor.projection;
    m_sensorFov = std::clamp(sensor.sensorFov, 0.1f, 120.0f);
    m_sensorSceneSize_XZY = {sceneSize_XYZ.x / scale, sceneSize_XYZ.z / scale,
                             sceneSize_XYZ.y / scale};
    m_sensorPosition_XZY = {(sensor.position.x - sceneSize_XYZ.x * 0.5f) / scale,
                            sensor.position.z / scale,
                            (sensor.position.y - sceneSize_XYZ.y * 0.5f) / scale};
    m_sensorTarget_XZY = {0.0f, sceneSize_XYZ.z * 0.5f / scale, 0.0f};
}

SensorMatrix Geometry::createConfiguredSensor(glm::vec3 size_XZY, glm::vec3 origin_XZY,
                                               float vza, float vaa, float ratio) {
    // The Stream3D bridge supplies the exact sensor frame, including orthographic
    // footprint and roll. Existing project cameras retain their normal behaviour.
    if (const char* cameraPath = std::getenv("HISTREAM_SENSOR_CAMERA")) {
        // DEFERRED[低] 安全: getenv 返回 ANSI 代码页字符串，窄字符 ifstream 无法打开含代码页外字符的中文路径；错误信息也未带上路径；JSON 中缺 "viewInverse" 等字段时 at() 抛出的 nlohmann 异常信息难以定位；建议：用 _wgetenv + std::filesystem::path 打开，报错时附带路径与字段名。
        std::ifstream input(cameraPath);
        if (!input) throw std::runtime_error("Cannot open explicit sensor camera");
        ProjectJson::Json camera; input >> camera;
        SensorMatrix sensor{};
        for (const char* field : {"viewInverse", "projInverse"}) {
            const auto& values = camera.at(field);
            if (!values.is_array() || values.size() != 16)
                throw std::runtime_error("Sensor matrices require 16 column-major values");
            glm::mat4 matrix(1.0f);
            for (int i = 0; i < 16; ++i) {
                const float value = values.at(i).get<float>();
                if (!std::isfinite(value)) throw std::runtime_error("Non-finite sensor matrix");
                matrix[i / 4][i % 4] = value;
            }
            if (std::string(field) == "viewInverse") sensor.viewInverse = matrix;
            else sensor.projInverse = matrix;
        }
        sensor.focalDist = camera.value("orthographic", false) ? -1.0f : 1.0f;
        sensor.aperture = 0.0f;
        sensor.direction = glm::vec3(sensor.viewInverse[2]);
        return sensor;
    }
    if (m_sensorProjection == Projection::PERSPECTIVE) {
        SensorMatrix sensor = createPerspectiveSensor(m_sensorPosition_XZY, vza, vaa);
        if (const char* cone = std::getenv("HISTREAM_HEX_CONE")) {
            if (std::string(cone) != "lod") {
                throw std::runtime_error(
                    "HISTREAM_HEX_CONE currently supports lod");
            }
            sensor.aperture = -4.0f;
        }
        return sensor;
    }
    SensorMatrix sensor = createSensor(size_XZY, origin_XZY, vza, vaa, ratio);
    if (const char* cone = std::getenv("HISTREAM_HEX_CONE")) {
        if (std::string(cone) != "lod") {
            throw std::runtime_error(
                "HISTREAM_HEX_CONE currently supports lod");
        }
        sensor.aperture = -4.0f;
    }
    return sensor;
}

SensorMatrix Geometry::createPerspectiveSensor(glm::vec3 position_XZY, float vza, float vaa) {
    const float zenith = std::clamp(vza, 0.0f, 89.999f) * DEG2RAD;
    const float azimuth = vaa * DEG2RAD;
    const glm::vec3 cameraOut{
        std::sin(zenith) * std::cos(azimuth),
        std::cos(zenith),
        std::sin(zenith) * std::sin(azimuth)};
    const float lookDistance = std::max(1.0f, glm::length(m_sensorSceneSize_XZY));
    return createSensor(position_XZY, position_XZY - cameraOut * lookDistance,
                        m_sensorSceneSize_XZY, m_sensorFov,
                        perspectiveCameraUp(vza, vaa));
}

glm::vec3 Geometry::sensorWorldToXzy(glm::vec3 position_XYZ) const {
    return {
        position_XYZ.x / m_sensorMetresPerUnit - m_sensorSceneSize_XZY.x * 0.5f,
        position_XYZ.z / m_sensorMetresPerUnit,
        position_XYZ.y / m_sensorMetresPerUnit - m_sensorSceneSize_XZY.z * 0.5f};
}


bool Geometry::createGeometry(std::shared_ptr<FileIO> &fileio, std::shared_ptr<RaytracingIO> &modelio){

//    auto & sceneio = modelio->m_sceneio;


    //--------------------------------------------------------
    //--- Resolution
    //-------------------------------------------------------
    modelio->imageSize = fileio->m_pRaytracingXml->sensorxml.resolution;
    CameraManip.setWindowSize(modelio->imageSize.x, modelio->imageSize.y );
    //---------------------------------------------------------
    // Angles
    //---------------------------------------------------------
    float vza, vaa, sza, saa;
    LightXml lightxml = fileio->m_pRaytracingXml->lightxml;
    SensorXml sensorxml = fileio->m_pRaytracingXml->sensorxml;
    configureSensor(sensorxml, fileio->m_pRaytracingXml->scenexml.background.sceneSize, 1.0f);


    sza = lightxml.solarAngle[0];
    saa = lightxml.solarAngle[1];
    for (int i = 0; i < sensorxml.viewAngles.size(); i++)
    {
        vza = sensorxml.viewAngles[i][0];
        vaa = sensorxml.viewAngles[i][1];
        modelio->angles.emplace_back(Angle{vza, vaa, sza, saa});
    }
    //modelio->n_angle = sensorxml.viewAngles.size();
    //---------------------------------------------------------
    // Bands
    //---------------------------------------------------------
    modelio->waves = fileio->m_pRaytracingXml->sensorxml.waves;
    modelio->isUAVTrave = fileio->m_pRaytracingXml->settingxml.isUAVtrave
                          && !sensorxml.uavPoses.empty();
    modelio->uavposes = sensorxml.uavPoses;
    modelio->uavViewAzimuths = sensorxml.uavViewAzimuths;
    modelio->n_pos = static_cast<int>(modelio->uavposes.size());

    //---------------------------------------------------------
    // LIGHT AND SENSOR INI with Angle 0 and Band 0
    //---------------------------------------------------------
    // FIXED[高] 内存: sensorxml.viewAngles 为空（配置缺省观测角）时 angles 为空，下方 angles[0] 越界读（UB/崩溃）；已修复：解析完 viewAngles 后若 angles 仍为空，则补一个天底观测 {0,0}（沿用当前太阳角），保证 angles[0] 有效；solarAngle 为 glm::vec2，无“元素个数不足”问题。
    if (modelio->angles.empty())
    {
        modelio->angles.emplace_back(Angle{0.0f, 0.0f, sza, saa});
    }
    vza = modelio->angles[0].vza;
    vaa = modelio->angles[0].vaa;
    sza = modelio->angles[0].sza;
    saa = modelio->angles[0].saa;
    modelio->sensor = createConfiguredSensor(modelio->voxelSize, modelio->voxelOrigin, vza, vaa, 1.0f);


    modelio->light = createLight(sza, saa, fileio->m_pRaytracingXml->lightxml.direct, fileio->m_pRaytracingXml->lightxml.diffuse,
                                      fileio->m_pRaytracingXml->lightxml.solarTemperature,
                                      fileio->m_pRaytracingXml->lightxml.skyTemperature);



    return true;

}

void Geometry::updateAngle(std::shared_ptr<RaytracingIO> &modelio, int kangle){

    Angle &angle = modelio->angles[kangle];
//    std::cout << "Angle Info:"
//              << "    vza_" << std::to_string(angles.x) << "    vaa_" << std::to_string(angles.y)
//              << "    sza_" << std::to_string(angles.z) << "    saa_" << std::to_string(angles.w) << std::endl;

    float ratio = 1.0;
    // ratio = 0.707;
    SensorMatrix sensorMatrix = createConfiguredSensor(modelio->voxelSize, modelio->voxelOrigin,
                                                       angle.vza, angle.vaa, ratio);
    updateSensor(modelio, sensorMatrix);

    LightSet lightSet = createLight(angle.sza, angle.saa,modelio->light.direct,
                                    modelio->light.diffuse,modelio->light.solarTemperature,
                                    modelio->light.skyTemperature);
    updateLight(modelio,lightSet);

}

void Geometry::updateSensorPos(std::shared_ptr<RaytracingIO> &modelio, int kpos) {
    if (kpos < 0 || kpos >= static_cast<int>(modelio->uavposes.size()) || modelio->angles.empty()) return;
    Angle& angle = modelio->angles.front();
    if (kpos < static_cast<int>(modelio->uavViewAzimuths.size())) angle.vaa = modelio->uavViewAzimuths[kpos];
    SensorMatrix sensor = createPerspectiveSensor(sensorWorldToXzy(modelio->uavposes[kpos]),
                                                  angle.vza, angle.vaa);
    updateSensor(modelio, sensor);
}

void Geometry::updateSensor( std::shared_ptr<RaytracingIO> &modelio, SensorMatrix &sensor){


    auto &m_pBufferSensor = modelio->m_pBufferSensor;
    auto &m_device = modelio->m_device;
    auto &m_queueIndex = modelio->m_queueIndex;

    nvvk::CommandPool cmdBufGet(m_device, m_queueIndex);
    vk::CommandBuffer cmdBuf = cmdBufGet.createCommandBuffer();
    vkCmdUpdateBuffer(cmdBuf, (*m_pBufferSensor).buffer, 0, sizeof(SensorMatrix), &sensor);
    transferToComputeBarrier(cmdBuf, (*m_pBufferSensor).buffer, sizeof(SensorMatrix));
    cmdBufGet.submitAndWait(cmdBuf);
}

void Geometry::updateLight(std::shared_ptr<RaytracingIO> &modelio, LightSet &light){


    auto &m_pBufferLight = modelio->m_pBufferLight;
    auto &m_device = modelio->m_device;
    auto &m_queueIndex = modelio->m_queueIndex;

    nvvk::CommandPool cmdBufGet(m_device, m_queueIndex);
    vk::CommandBuffer cmdBuf = cmdBufGet.createCommandBuffer();
    vkCmdUpdateBuffer(cmdBuf, (*m_pBufferLight).buffer, 0, sizeof(LightSet), &light);
    transferToComputeBarrier(cmdBuf, (*m_pBufferLight).buffer, sizeof(LightSet));
    cmdBufGet.submitAndWait(cmdBuf);
}


void Geometry::orthcorrect(std::shared_ptr<RaytracingIO> &modelio,float vza, float vaa,
                           Eigen::VectorXd &cx, Eigen::VectorXd &cy) {

    glm::vec3 size = modelio->voxelSize;
    glm::vec3 origen =modelio->voxelOrigin;


    glm::vec3 semi = { size.x / 2.0, 0, size.z / 2.0 };

    glm::vec3 dimensionMin = -semi + glm::vec3{ origen.x, 0, origen.z };
    glm::vec3 dimensionMax = semi + glm::vec3{ origen.x, 0, origen.z };
    //float scale = m_pRaytracingXml->scene.stepSize;
    dimensionMin.y = 0;
    dimensionMax.y = 0;
    float r = SENSOR_HEIGHT;
    float rd = DEG2RAD;


    float ratio = 1.0;


    //SensorMatrix sensor;
    glm::vec3 origin = glm::vec3(0, 0, 0);
    glm::vec3 sensorPos = glm::vec3(r * std::sin(vza * rd) * std::cos(vaa * rd),
                                    r * std::cos(vza * rd), r * std::sin(vza * rd) * std::sin(vaa * rd));
    CameraManip.setFov(SENSOR_FOV);
    CameraManip.setLookat(sensorPos, origin, observationCameraUp(vza, vaa));
    float fovv = CameraManip.getFov();
    //CameraManip.fit(dimensionMin * ratio / scale, dimensionMax * ratio / scale); // the sensor position height is changed.
    CameraManip.fit(dimensionMin * ratio, dimensionMax * ratio); // the sensor position height is changed.
    float width = CameraManip.getWidth();
    float height = CameraManip.getHeight();
    const float aspectRatio = CameraManip.getWidth() / static_cast<float>(CameraManip.getHeight());
    glm::mat4 view = CameraManip.getMatrix();
    nvmath::mat4f projj = nvmath::perspectiveVK(CameraManip.getFov(), aspectRatio, 0.0001f, 10000.0f);
    //glm::mat4 proj = glm::perspective(CameraManip.getFov(), aspectRatio, 0.0001f, 10000.0f);
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(CameraManip.getFov()), aspectRatio, 0.1f, 1000.0f);
    proj[1][1] *= -1;

    glm::vec4 leftupper,leftbottom,rightupper,rightbottom;
    leftupper = glm::vec4(dimensionMax.x,0,dimensionMin.z,1);
    leftbottom = glm::vec4(dimensionMin.x,0,dimensionMin.z,1);
    rightupper = glm::vec4(dimensionMax.x,0,dimensionMax.z,1);
    rightbottom = glm::vec4(dimensionMin.x,0,dimensionMax.z,1);

    glm::vec4 lu,lb,ru,rb;
    glm::vec2 lun,lbn,run,rbn;
    lu = proj * view * leftupper;
    lu = lu/lu.w;
    lun.x = (lu.x+1.0f)/2.0*width;
    lun.y = (lu.y+1.0f)/2.0*height;

    lb = proj * view * leftbottom;
    lb = lb/lb.w;
    lbn.x = (lb.x+1.0f)/2.0*width;
    lbn.y = (1.0f + lb.y)/2.0*height;

    ru = proj * view * rightupper;
    ru = ru/ru.w;
    run.x = (ru.x+1.0f)/2.0*width;
    run.y = (1.0f + ru.y)/2.0*height;


    rb = proj * view * rightbottom;
    rb = rb/rb.w;
    rbn.x = (rb.x+1.0f)/2.0*width;
    rbn.y = (1.0f + rb.y)/2.0*height;


    solveNadirMapping(width, height,
                      dimensionMax.x - dimensionMin.x,
                      dimensionMax.z - dimensionMin.z,
                      lun, lbn, run, rbn, cx, cy);
}



bool Geometry::createGeometry(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &modelio){

//    auto & sceneio = modelio->m_sceneio;

    //--------------------------------------------------------
    //--- Resolution
    //-------------------------------------------------------
    modelio->imageSize = fileio->m_pVoxelebXml->sensorxml.resolution;
    CameraManip.setWindowSize(modelio->imageSize.x, modelio->imageSize.y );
    //---------------------------------------------------------
    // Angles
    //---------------------------------------------------------
    float vza, vaa, sza, saa;
    LightXml lightxml = fileio->m_pVoxelebXml->lightxml;
    SensorXml sensorxml = fileio->m_pVoxelebXml->sensorxml;
    configureSensor(sensorxml, fileio->m_pVoxelebXml->scenexml.background.sceneSize,
                    fileio->m_pVoxelebXml->scenexml.background.stepsize_surface);




    sza = lightxml.solarAngle[0];
    saa = lightxml.solarAngle[1];
    for (int i = 0; i < sensorxml.viewAngles.size(); i++)
    {
        vza = sensorxml.viewAngles[i][0];
        vaa = sensorxml.viewAngles[i][1];
        modelio->angles.emplace_back(Angle{vza, vaa, sza, saa});
    }
    //modelio->n_angle = sensorxml.viewAngles.size();
    //---------------------------------------------------------
    // Bands
    //---------------------------------------------------------
    modelio->waves = fileio->m_pVoxelebXml->sensorxml.waves;
    modelio->isUAVTrave = fileio->m_pVoxelebXml->settingxml.isUAVtrave
                          && !sensorxml.uavPoses.empty();
    modelio->uavposes = sensorxml.uavPoses;
    modelio->uavViewAzimuths = sensorxml.uavViewAzimuths;
    modelio->n_pos = static_cast<int>(modelio->uavposes.size());

    //---------------------------------------------------------
    // LIGHT AND SENSOR INI with Angle 0 and Band 0
    //---------------------------------------------------------
    // FIXED[高] 内存: viewAngles 为空时 angles[0] 越界读（同 RaytracingIO 重载）；已修复：解析完 viewAngles 后若 angles 为空，则补一个天底观测 {0,0}（沿用当前太阳角）。
    if (modelio->angles.empty())
    {
        modelio->angles.emplace_back(Angle{0.0f, 0.0f, sza, saa});
    }
    vza = modelio->angles[0].vza;
    vaa = modelio->angles[0].vaa;
    sza = modelio->angles[0].sza;
    saa = modelio->angles[0].saa;
    modelio->sensor = createConfiguredSensor(modelio->voxelSize_XZY, modelio->voxelOrigin_XZY, vza, vaa, 1.0f);
    modelio->light = createLight(sza, saa, fileio->m_pVoxelebXml->lightxml.direct, fileio->m_pVoxelebXml->lightxml.diffuse,
                                 fileio->m_pVoxelebXml->lightxml.solarTemperature,
                                 fileio->m_pVoxelebXml->lightxml.skyTemperature);



    return true;

}

void Geometry::updateAngle(std::shared_ptr<VoxelebIO> &modelio, int kangle){

    Angle &angle = modelio->angles[kangle];
//    std::cout << "Angle Info:"
//              << "    vza_" << std::to_string(angles.x) << "    vaa_" << std::to_string(angles.y)
//              << "    sza_" << std::to_string(angles.z) << "    saa_" << std::to_string(angles.w) << std::endl;

    updateSolarAngle(modelio,angle);

    float ratio = 1.0;
    //ratio = 0.707;
    SensorMatrix sensorMatrix = createConfiguredSensor(modelio->voxelSize_XZY, modelio->voxelOrigin_XZY,
                                                       angle.vza, angle.vaa, ratio);
    updateSensor(modelio, sensorMatrix);

    // if (angle.sza >= 85) angle.sza = 85;
    LightSet lightSet = createLight(angle.sza, angle.saa,modelio->light.direct,
                                    modelio->light.diffuse,modelio->light.solarTemperature,
                                    modelio->light.skyTemperature);
    updateLight(modelio,lightSet);

}

void Geometry::updateSensorPos(std::shared_ptr<VoxelebIO> &modelio, int kpos) {
    if (kpos < 0 || kpos >= static_cast<int>(modelio->uavposes.size()) || modelio->angles.empty()) return;
    Angle& angle = modelio->angles.front();
    if (kpos < static_cast<int>(modelio->uavViewAzimuths.size())) angle.vaa = modelio->uavViewAzimuths[kpos];
    SensorMatrix sensor = createPerspectiveSensor(sensorWorldToXzy(modelio->uavposes[kpos]),
                                                  angle.vza, angle.vaa);
    updateSensor(modelio, sensor);
}

void Geometry::updateSensor( std::shared_ptr<VoxelebIO> &modelio, SensorMatrix &sensor){


    auto &m_pBufferSensor = modelio->m_pBufferSensor;
    auto &m_device = modelio->m_device;
    auto &m_queueIndex = modelio->m_queueIndex;

    nvvk::CommandPool cmdBufGet(m_device, m_queueIndex);
    vk::CommandBuffer cmdBuf = cmdBufGet.createCommandBuffer();
    vkCmdUpdateBuffer(cmdBuf, (*m_pBufferSensor).buffer, 0, sizeof(SensorMatrix), &sensor);
    transferToComputeBarrier(cmdBuf, (*m_pBufferSensor).buffer, sizeof(SensorMatrix));
    cmdBufGet.submitAndWait(cmdBuf);
}

void Geometry::updateLight(std::shared_ptr<VoxelebIO> &modelio, LightSet &light){


    auto &m_pBufferLight = modelio->m_pBufferLight;
    auto &m_device = modelio->m_device;
    auto &m_queueIndex = modelio->m_queueIndex;

    nvvk::CommandPool cmdBufGet(m_device, m_queueIndex);
    vk::CommandBuffer cmdBuf = cmdBufGet.createCommandBuffer();
    vkCmdUpdateBuffer(cmdBuf, (*m_pBufferLight).buffer, 0, sizeof(LightSet), &light);
    transferToComputeBarrier(cmdBuf, (*m_pBufferLight).buffer, sizeof(LightSet));
    cmdBufGet.submitAndWait(cmdBuf);
}

void Geometry::updateSolarAngle(std::shared_ptr<VoxelebIO> &modelio, Angle &angle) {
    SPACalc spa;
    spa_data data{};


    int kmonth,kday;
    const double t = modelio->meteo.t;
    const int kdoy = static_cast<int>(std::floor(t));
    // Round the day fraction to the nearest second; the last second of the
    // day is kept so rounding never moves past the end of the year.
    const long long secondOfDay = std::min(86399LL, std::llround((t - kdoy) * 86400.0));
    const int h = static_cast<int>(secondOfDay / 3600);
    const int m = static_cast<int>((secondOfDay % 3600) / 60);
    const int s = static_cast<int>(secondOfDay % 60);
    Utils::calculateMonthAndDay(modelio->m_year, kdoy, &kmonth, &kday);

    data.year = modelio->m_year;
    data.month = kmonth;
    data.day = kday;
    data.hour = h;
    data.minute = m;
    data.second = s;
    data.timezone = modelio->m_timezone;
    data.pressure = std::max(100.0f, modelio->meteo.p);
    data.temperature = modelio->meteo.Ta;
    data.delta_t = Utils::calculateDeltaT(data.year, data.month);
    data.longitude = modelio->lon;
    data.latitude = modelio->lat;
    data.atmos_refract = 0.5667;
    // DEFERRED[中] 物理: 观测点海拔硬编码为 100 m，忽略场景实际海拔/DEM；更重要的是 zenith>90°（夜间/日出前）时没有任何处理，createLight 会把太阳放到地平面以下并照常施加 direct 辐照，能量平衡在夜间得到非物理的直射加热；另 kdoy 为 0 或超出当年天数时 calculateMonthAndDay 的行为未校验；建议：elevation 取配置值，sza>=90 时将 direct 置 0，并校验 1<=kdoy<=365/366。
    data.elevation = 100;
    data.slope = 0;
    data.azm_rotation = 0;
    data.function = SPA_ZA;

    if (spa.spa_calculate(&data) != 0) {
        throw std::runtime_error("VoxelEB solar-position calculation failed; check meteo time, latitude and pressure");
    }
    angle.sza = data.zenith;
    angle.saa = data.azimuth;

}

void Geometry::orthcorrect(std::shared_ptr<VoxelebIO> &modelio,float vza, float vaa,
                           Eigen::VectorXd &cx, Eigen::VectorXd &cy) {

    glm::vec3 size = modelio->voxelSize_XZY;
    glm::vec3 origen =modelio->voxelOrigin_XZY;
    glm::vec3 semi = { size.x / 2.0, 0, size.z / 2.0 };
    glm::vec3 dimensionMin = -semi + glm::vec3{ origen.x, 0, origen.z };
    glm::vec3 dimensionMax = semi + glm::vec3{ origen.x, 0, origen.z };
    //float scale = m_pRaytracingXml->scene.stepSize;
    dimensionMin.y = 0;
    dimensionMax.y = 0;
    float r = SENSOR_HEIGHT;
    float rd = DEG2RAD;


    float ratio = 1.0;


    //SensorMatrix sensor;
    glm::vec3 origin = glm::vec3(0, 0, 0);
    glm::vec3 sensorPos = glm::vec3(r * std::sin(vza * rd) * std::cos(vaa * rd),
                                    r * std::cos(vza * rd),
                                    r * std::sin(vza * rd) * std::sin(vaa * rd));
    CameraManip.setFov(SENSOR_FOV);
    CameraManip.setLookat(sensorPos, origin, observationCameraUp(vza, vaa));
    float fovv = CameraManip.getFov();
    //CameraManip.fit(dimensionMin * ratio / scale, dimensionMax * ratio / scale); // the sensor position height is changed.
    CameraManip.fit(dimensionMin * ratio, dimensionMax * ratio); // the sensor position height is changed.
    float width = CameraManip.getWidth();
    float height = CameraManip.getHeight();
    const float aspectRatio = CameraManip.getWidth() / static_cast<float>(CameraManip.getHeight());
    glm::mat4 view = CameraManip.getMatrix();
    nvmath::mat4f projj = nvmath::perspectiveVK(CameraManip.getFov(), aspectRatio, 0.0001f, 10000.0f);
    //glm::mat4 proj = glm::perspective(CameraManip.getFov(), aspectRatio, 0.0001f, 10000.0f);
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(CameraManip.getFov()), aspectRatio, 0.1f, 1000.0f);
    proj[1][1] *= -1;

    glm::vec4 leftupper,leftbottom,rightupper,rightbottom;
    leftupper = glm::vec4(dimensionMax.x,0,dimensionMin.z,1);
    leftbottom = glm::vec4(dimensionMin.x,0,dimensionMin.z,1);
    rightupper = glm::vec4(dimensionMax.x,0,dimensionMax.z,1);
    rightbottom = glm::vec4(dimensionMin.x,0,dimensionMax.z,1);

    glm::vec4 lu,lb,ru,rb;
    glm::vec2 lun,lbn,run,rbn;
    lu = proj * view * leftupper;
    lu = lu/lu.w;
    lun.x = (lu.x+1.0f)/2.0*width;
    lun.y = (lu.y+1.0f)/2.0*height;

    lb = proj * view * leftbottom;
    lb = lb/lb.w;
    lbn.x = (lb.x+1.0f)/2.0*width;
    lbn.y = (1.0f + lb.y)/2.0*height;

    ru = proj * view * rightupper;
    ru = ru/ru.w;
    run.x = (ru.x+1.0f)/2.0*width;
    run.y = (1.0f + ru.y)/2.0*height;


    rb = proj * view * rightbottom;
    rb = rb/rb.w;
    rbn.x = (rb.x+1.0f)/2.0*width;
    rbn.y = (1.0f + rb.y)/2.0*height;


    solveNadirMapping(width, height,
                      dimensionMax.x - dimensionMin.x,
                      dimensionMax.z - dimensionMin.z,
                      lun, lbn, run, rbn, cx, cy);
}


bool Geometry::createGeometry(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &modelio){

//    auto & sceneio = modelio->m_sceneio;

    //--------------------------------------------------------
    //--- Resolution
    //-------------------------------------------------------
    modelio->imageSize = fileio->m_pVoxelrtXml->sensorxml.resolution;
    CameraManip.setWindowSize(modelio->imageSize.x, modelio->imageSize.y );
    //---------------------------------------------------------
    // Angles
    //---------------------------------------------------------
    float vza, vaa, sza, saa;
    LightXml lightxml = fileio->m_pVoxelrtXml->lightxml;
    SensorXml sensorxml = fileio->m_pVoxelrtXml->sensorxml;
    configureSensor(sensorxml, fileio->m_pVoxelrtXml->scenexml.background.sceneSize,
                    fileio->m_pVoxelrtXml->scenexml.background.stepsize_surface);




    sza = lightxml.solarAngle[0];
    saa = lightxml.solarAngle[1];
    for (int i = 0; i < sensorxml.viewAngles.size(); i++)
    {
        vza = sensorxml.viewAngles[i][0];
        vaa = sensorxml.viewAngles[i][1];
        modelio->angles.emplace_back(Angle{vza, vaa, sza, saa});
    }
    //modelio->n_angle = sensorxml.viewAngles.size();
    //---------------------------------------------------------
    // Bands
    //---------------------------------------------------------
    modelio->waves = fileio->m_pVoxelrtXml->sensorxml.waves;

    //---------------------------------------------------------
    // LIGHT AND SENSOR INI with Angle 0 and Band 0
    //---------------------------------------------------------
    // FIXED[高] 内存: viewAngles 为空时 angles[0] 越界读（同 RaytracingIO 重载）；已修复：解析完 viewAngles 后若 angles 为空，则补一个天底观测 {0,0}（沿用当前太阳角）。
    if (modelio->angles.empty())
    {
        modelio->angles.emplace_back(Angle{0.0f, 0.0f, sza, saa});
    }
    vza = modelio->angles[0].vza;
    vaa = modelio->angles[0].vaa;
    sza = modelio->angles[0].sza;
    saa = modelio->angles[0].saa;

    modelio->isUAVTrave = fileio->m_pVoxelrtXml->settingxml.isUAVtrave
                          && !fileio->m_pVoxelrtXml->sensorxml.uavPoses.empty();
    modelio->n_pos = fileio->m_pVoxelrtXml->sensorxml.uavPoses.size();
    modelio->uavposes = fileio->m_pVoxelrtXml->sensorxml.uavPoses;
    modelio->uavViewAzimuths = fileio->m_pVoxelrtXml->sensorxml.uavViewAzimuths;

//    if(modelio->isUAVTrave == true){
//        modelio->sensor = createSensor(glm::vec3(0,10,1),glm::vec3(0,0,0));}
//    else {
//        modelio->sensor = createSensor(modelio->voxelSize_XZY, modelio->voxelOrigin_XZY, vza, vaa, 1.0);
//    }
    modelio->sensor = createConfiguredSensor(modelio->voxelSize_XZY, modelio->voxelOrigin_XZY, vza, vaa, 1.0f);
    modelio->light = createLight(sza, saa, fileio->m_pVoxelrtXml->lightxml.direct,
                                 fileio->m_pVoxelrtXml->lightxml.diffuse,
                                 fileio->m_pVoxelrtXml->lightxml.solarTemperature,
                                 fileio->m_pVoxelrtXml->lightxml.skyTemperature);



    return true;

}

void Geometry::updateAngle(std::shared_ptr<VoxelrtIO> &modelio, int kangle){

    Angle &angle = modelio->angles[kangle];
//    std::cout << "Angle Info:"
//              << "    vza_" << std::to_string(angles.x) << "    vaa_" << std::to_string(angles.y)
//              << "    sza_" << std::to_string(angles.z) << "    saa_" << std::to_string(angles.w) << std::endl;

    float ratio = 1.0;
    //ratio = 0.707;
    SensorMatrix sensorMatrix;
    if(modelio->isUAVTrave == false) {

        sensorMatrix = createConfiguredSensor(modelio->voxelSize_XZY, modelio->voxelOrigin_XZY,
                                                   angle.vza, angle.vaa, ratio);
    }else {
        // DEFERRED[低] Bug: UAV 模式下切换角度时用硬编码相机（位于 (0,1000,1) 看向 (0,900,0)），既不对应 uavposes 也不随 vza/vaa 变化，与 RaytracingIO/VoxelebIO 版本不一致；若 updateAngle 在 updateSensorPos 之后调用会覆盖正确的 UAV 相机；建议：UAV 模式下跳过传感器更新或使用当前 kpos 的 createPerspectiveSensor。
        sensorMatrix = createSensor(glm::vec3(0, 1000, 1), glm::vec3(0, 900, 0), m_sensorSceneSize_XZY);
    }
    updateSensor(modelio, sensorMatrix);

    LightSet lightSet = createLight(angle.sza, angle.saa,modelio->light.direct,
                                    modelio->light.diffuse,modelio->light.solarTemperature,
                                    modelio->light.skyTemperature);
    updateLight(modelio,lightSet);

}

void Geometry::updateSensorPos(std::shared_ptr<VoxelrtIO> &modelio, int kpos){
    if (kpos < 0 || kpos >= static_cast<int>(modelio->uavposes.size()) || modelio->angles.empty()) return;
    Angle& angle = modelio->angles.front();
    if (kpos < static_cast<int>(modelio->uavViewAzimuths.size())) angle.vaa = modelio->uavViewAzimuths[kpos];
    SensorMatrix sensorMatrix = createPerspectiveSensor(sensorWorldToXzy(modelio->uavposes[kpos]),
                                                        angle.vza, angle.vaa);

    updateSensor(modelio, sensorMatrix);

    LightSet lightSet = createLight(angle.sza, angle.saa,modelio->light.direct,
                                    modelio->light.diffuse,modelio->light.solarTemperature,
                                    modelio->light.skyTemperature);
    updateLight(modelio,lightSet);

}

void Geometry::updateSensor( std::shared_ptr<VoxelrtIO> &modelio, SensorMatrix &sensor){


    auto &m_pBufferSensor = modelio->m_pBufferSensor;
    auto &m_device = modelio->m_device;
    auto &m_queueIndex = modelio->m_queueIndex;

    nvvk::CommandPool cmdBufGet(m_device, m_queueIndex);
    vk::CommandBuffer cmdBuf = cmdBufGet.createCommandBuffer();
    // DEFERRED[低] 并发: 与 RaytracingIO/VoxelebIO 版本不同，这里（及下方 updateLight）缺少 transferToComputeBarrier；fence 等待只保证主机侧可见，后续提交的着色器读取该缓冲在 Vulkan 内存模型下没有 transfer→shader 的可用性/可见性依赖（校验层同步检查会报错）；建议：与其他重载一致加上屏障。
    vkCmdUpdateBuffer(cmdBuf, (*m_pBufferSensor).buffer, 0, sizeof(SensorMatrix), &sensor);
    cmdBufGet.submitAndWait(cmdBuf);
}

void Geometry::updateLight(std::shared_ptr<VoxelrtIO> &modelio, LightSet &light){


    auto &m_pBufferLight = modelio->m_pBufferLight;
    auto &m_device = modelio->m_device;
    auto &m_queueIndex = modelio->m_queueIndex;

    nvvk::CommandPool cmdBufGet(m_device, m_queueIndex);
    vk::CommandBuffer cmdBuf = cmdBufGet.createCommandBuffer();
    vkCmdUpdateBuffer(cmdBuf, (*m_pBufferLight).buffer, 0, sizeof(LightSet), &light);
    cmdBufGet.submitAndWait(cmdBuf);
}

void Geometry::orthcorrect(std::shared_ptr<VoxelrtIO> &modelio,float vza, float vaa,
                           Eigen::VectorXd &cx, Eigen::VectorXd &cy) {

    glm::vec3 size = modelio->voxelSize_XZY;
    glm::vec3 origen =modelio->voxelOrigin_XZY;
    glm::vec3 semi = { size.x / 2.0, 0, size.z / 2.0 };
    glm::vec3 dimensionMin = -semi + glm::vec3{ origen.x, 0, origen.z };
    glm::vec3 dimensionMax = semi + glm::vec3{ origen.x, 0, origen.z };
    //float scale = m_pRaytracingXml->scene.stepSize;
    dimensionMin.y = 0;
    dimensionMax.y = 0;
    float r = SENSOR_HEIGHT;
    float rd = DEG2RAD;


    float ratio = 1.0;


    //SensorMatrix sensor;
    glm::vec3 origin = glm::vec3(0, 0, 0);
    glm::vec3 sensorPos = glm::vec3(r * std::sin(vza * rd) * std::cos(vaa * rd),
                                    r * std::cos(vza * rd),
                                    r * std::sin(vza * rd) * std::sin(vaa * rd));
    CameraManip.setFov(SENSOR_FOV);
    CameraManip.setLookat(sensorPos, origin, observationCameraUp(vza, vaa));
    float fovv = CameraManip.getFov();
    //CameraManip.fit(dimensionMin * ratio / scale, dimensionMax * ratio / scale); // the sensor position height is changed.
    CameraManip.fit(dimensionMin * ratio, dimensionMax * ratio); // the sensor position height is changed.
    float width = CameraManip.getWidth();
    float height = CameraManip.getHeight();
    const float aspectRatio = CameraManip.getWidth() / static_cast<float>(CameraManip.getHeight());
    glm::mat4 view = CameraManip.getMatrix();
    nvmath::mat4f projj = nvmath::perspectiveVK(CameraManip.getFov(), aspectRatio, 0.0001f, 10000.0f);
    //glm::mat4 proj = glm::perspective(CameraManip.getFov(), aspectRatio, 0.0001f, 10000.0f);
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(CameraManip.getFov()), aspectRatio, 0.1f, 1000.0f);
    proj[1][1] *= -1;

    glm::vec4 leftupper,leftbottom,rightupper,rightbottom;
    leftupper = glm::vec4(dimensionMax.x,0,dimensionMin.z,1);
    leftbottom = glm::vec4(dimensionMin.x,0,dimensionMin.z,1);
    rightupper = glm::vec4(dimensionMax.x,0,dimensionMax.z,1);
    rightbottom = glm::vec4(dimensionMin.x,0,dimensionMax.z,1);

    glm::vec4 lu,lb,ru,rb;
    glm::vec2 lun,lbn,run,rbn;
    lu = proj * view * leftupper;
    lu = lu/lu.w;
    lun.x = (lu.x+1.0f)/2.0*width;
    lun.y = (lu.y+1.0f)/2.0*height;

    lb = proj * view * leftbottom;
    lb = lb/lb.w;
    lbn.x = (lb.x+1.0f)/2.0*width;
    lbn.y = (1.0f + lb.y)/2.0*height;

    ru = proj * view * rightupper;
    ru = ru/ru.w;
    run.x = (ru.x+1.0f)/2.0*width;
    run.y = (1.0f + ru.y)/2.0*height;


    rb = proj * view * rightbottom;
    rb = rb/rb.w;
    rbn.x = (rb.x+1.0f)/2.0*width;
    rbn.y = (1.0f + rb.y)/2.0*height;


    solveNadirMapping(width, height,
                      dimensionMax.x - dimensionMin.x,
                      dimensionMax.z - dimensionMin.z,
                      lun, lbn, run, rbn, cx, cy);
}
