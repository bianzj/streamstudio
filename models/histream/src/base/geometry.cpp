// ==========================================================================
// 文件：geometry.cpp
// 功能：Geometry 类实现。把工程配置（FileIO）里的场景、太阳、传感器参数转成 GPU 渲染所需的
//       相机矩阵（SensorMatrix）与光源（LightSet），并写入 RayTracing / VoxelEB / VoxelRT
//       三类模型 IO 对象；同时提供正射（天底）影像的像素—场景坐标校正系数
// 主要模块：
//   1. 匿名命名空间工具：相机 up 向量（正射 / 透视）、缓冲区屏障、天底映射系数求解
//   2. createLight：由太阳天顶角/方位角构造 LightSet
//   3. createSensor（两个重载）：平行（正射）传感器与透视传感器的视图/投影逆矩阵
//   4. 传感器配置（configureSensor / createConfiguredSensor / createPerspectiveSensor）
//   5. 三套模型的 createGeometry / updateAngle / updateSensorPos / updateSensor /
//      updateLight / orthcorrect（RayTracing、VoxelEB、VoxelRT 各一份，逻辑相近）
// 数据流：FileIO（projectjson 解析结果）→ createGeometry 取角度表与传感器位置 →
//         update* 写入各模型 IO 的 sensor / light → 渲染 → orthcorrect 给出输出栅格校正
// 与其他文件的关系：geometry.h 声明；fileio_json.cpp 负责读取工程 JSON；structs.h 定义常量
// 约定：场景坐标系 +X 为正北、+Z 为正东、+Y 向上（XZY 后缀表示 X/Z/Y 顺序的向量）；
//       方位角自北顺时针；角度单位为度（内部乘 DEG2RAD 转弧度）；
//       SENSOR_HEIGHT=3000（传感器距原点的距离，场景单位）、ANGLE_COR=0.1（度）
// ==========================================================================

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

// 天底正射映射的缩放系数，固定为 1.0（保持真实场景长宽比，不再使用旧版 0.707 缩小）。
constexpr double kOrthographicScale = 1.0;

// 作用：正射/观测相机的 up 向量，使输出影像“上北右东”。
// 输入：观测天顶角、方位角（度）；输出：与视线正交的单位 up 向量。
// 说明：把正北（+X）投影到像面作 up；视线几乎沿北方向（退化）时改用世界 +Y；
//       right 退化时退回 +Z。
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

// 作用：透视相机的 up 向量，保持地平线水平（类似前视相机）。
// 说明：优先把世界 +Y 投影到像面；天底（视线与 +Y 平行）时 up 退化，改用正北。
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

// 作用：在命令缓冲中插入缓冲区内存屏障，保证传输（拷贝）写入对随后
//       compute 着色器读取可见。
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

// 作用：求天底正射输出栅格的像素坐标 (x, y) → 源坐标的双线性映射系数。
// 输入：影像宽高（像素）、场景宽/深、源图上场景四角的坐标（左上/左下/右上/右下）；
// 输出：cx、cy（各 4 项），源坐标 = c0·x + c1·y + c2·x·y + c3。
// 说明：由 4 个角点解 4×4 线性方程（SVD 最小二乘）；栅格边界取 -0.5 与 size-0.5。
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
    // 步骤 1：按场景长宽比等比缩放进栅格，求每像素对应的地面尺寸 groundPerPixel，
    //       并得到场景在栅格中居中的目标矩形 [x0, x1] × [y0, y1]。
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

    // 步骤 2：用四个角点建立设计矩阵（行：x, y, x·y, 1），分别对 X、Y 方向求解系数。
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


// 作用：由太阳角度与辐射量构造光源。
// 输入：sza/saa 太阳天顶角/方位角（度）；direct/diffuse 直射/散射辐射；
//       solarT/skyT 太阳/天空等效温度；shortwave 水平面全球短波（RayTracing/VoxelRT 的 Rin）。
// 输出：LightSet，direction 为指向太阳、长度 SENSOR_HEIGHT 的向量。
// 说明：sza 恰为 0° 或 45°、saa 贴近 45° 倍数（<0.05°）时做 ANGLE_COR 偏移，
//       避开与体元面对齐的退化方向（原因待核对）。
LightSet Geometry::createLight(float sza, float saa, float direct, float diffuse,float solarT,float skyT, float shortwave)
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
    // 步骤：球坐标转笛卡尔，Y 为天顶方向。
    // Azimuth is clockwise from north: +X is north and +Z is east.
    glm::vec3 lightPos = glm::vec3(r * std::sin(sza * rd) * std::cos(saa * rd),
                                   r * std::cos(sza * rd),
                                   r * std::sin(sza * rd) * std::sin(saa * rd));

    light.direction = lightPos;
    light.direct = direct;
    light.diffuse = diffuse;
    light.skyTemperature = skyT;
    light.solarTemperature = solarT;
    // RayTracing/VoxelRT carry the configured horizontal global shortwave in
    // Rin (thermal 2.5-6 um solar term, mwir_solar.glsl; 0 = clear sky).
    // VoxelEB uses the meteorological ubom.meteo values, not these fields.
    light.Rin = std::max(0.0f, shortwave);
    light.Rli = 0.0f;

    return light;
}

// 作用：创建平行（正射）传感器：相机置于观测方向 SENSOR_HEIGHT 处，窄视场（SENSOR_FOV），
//       并把相机拟合到整个场景范围（乘 ratio）。
// 输入：size 场景尺寸，origen 场景中心，vza/vaa 观测天顶/方位角（度），ratio 范围放大系数。
// 输出：SensorMatrix（视图/投影逆矩阵、焦距、视线方向）。aperture 恒为 0（针孔）。
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

    // 步骤 1：设置极小视场角以近似平行投影，并看向原点；up 取使影像上北的方向。
    CameraManip.setFov(SENSOR_FOV);
    CameraManip.setLookat(sensorPos, origin, observationCameraUp(vza, vaa));
    float fovv = CameraManip.getFov();




    //CameraManip.fit(dimensionMin * ratio / scale, dimensionMax * ratio / scale); // the sensor position height is changed.
    // 步骤 2：按场景包围盒拟合相机（会改变传感器高度）。
    CameraManip.fit(dimensionMin * ratio, dimensionMax * ratio); // the sensor position height is changed.


    float width = CameraManip.getWidth();
    float height = CameraManip.getHeight();
    const float aspectRatio = CameraManip.getWidth() / static_cast<float>(CameraManip.getHeight());
    // 步骤 3：由相机矩阵与 Vulkan 约定的投影矩阵求逆（proj[1][1] 取反以翻转 Y）。
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

// 作用：创建透视传感器。
// 输入：sensorPos_XZY 相机位置，center_XZY 注视点，sceneSize_XZY 场景尺寸，
//       fovDegrees 视场角（≤0 时自动拟合到场景外接球），up 相机上方向。
// 输出：SensorMatrix。说明：自动视场 = 2·atan(半高/距离)·1.02，限幅 [0.1°, 120°]；
//       远裁剪面取 max(10000, 距离 + 4·半径)。
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

// 作用：把工程配置里的传感器（投影类型、视场、位置）换算到渲染场景单位并缓存到成员变量。
// 输入：sensor 配置；sceneSize_XYZ 场景尺寸（配置坐标，Z 为高度）；
//       metresPerUnit 每个场景单位对应的米数（RayTracing 传 1，体元模型传体元步长）。
// 输出：写 m_sensorProjection / m_sensorFov / m_sensorSceneSize_XZY /
//       m_sensorPosition_XZY / m_sensorTarget_XZY（均为 X、Z(高)、Y 顺序的 XZY 坐标）。
// 说明：水平位置以场景中心为原点；注视点取场景中心的高度一半处；
//       视场限幅 [0.1°, 120°]，米/单位下限 0.0001 以防除零。
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

// 作用：按工程配置选择传感器构造方式（外部相机文件 / 透视 / 平行），
//       供 createGeometry 与 updateAngle 统一调用。
// 输入：场景尺寸与原点（XZY）、观测天顶角 vza / 方位角 vaa（度）、拟合范围系数 ratio。
// 输出：SensorMatrix。
SensorMatrix Geometry::createConfiguredSensor(glm::vec3 size_XZY, glm::vec3 origin_XZY,
                                               float vza, float vaa, float ratio) {
    // The Stream3D bridge supplies the exact sensor frame, including orthographic
    // footprint and roll. Existing project cameras retain their normal behaviour.
    // 分支 1：环境变量 HISTREAM_SENSOR_CAMERA 指向 JSON 时，直接使用其中给出的
    //       viewInverse / projInverse（各 16 个列主序数），忽略工程里的传感器角度。
    //       focalDist 用 ∓1 作为标记：orthographic 为真时取 -1，否则 +1（含义待核对）；
    //       direction 取 viewInverse 第 3 列。
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
    // 分支 2：透视投影，相机位置为配置的传感器位置。
    //       HISTREAM_HEX_CONE=lod 时把 aperture 置为 -4 作为 HEX LOD 锥体追踪的标记
    //       （含义待核对，仅支持 lod）。
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
    // 分支 3：默认的平行（正射）传感器，同样受 HISTREAM_HEX_CONE 标记影响。
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

// 作用：以 position_XZY 为相机位置、vza/vaa 为视线方向（相机看向 -cameraOut）创建透视传感器。
// 说明：vza 限幅到 [0, 89.999]° 避免水平视线；注视点距离取场景对角线长度（至少 1）。
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

// 作用：把 UAV 位姿（配置坐标 X/Y 水平、Z 高度，单位米）换算为场景 XZY 坐标，
//       水平原点移到场景中心，并除以米/单位。依赖 configureSensor 已调用。
glm::vec3 Geometry::sensorWorldToXzy(glm::vec3 position_XYZ) const {
    return {
        position_XYZ.x / m_sensorMetresPerUnit - m_sensorSceneSize_XZY.x * 0.5f,
        position_XYZ.z / m_sensorMetresPerUnit,
        position_XYZ.y / m_sensorMetresPerUnit - m_sensorSceneSize_XZY.z * 0.5f};
}


// 模块：RayTracing 的几何初始化（影像尺寸、角度表、UAV 位姿、初始传感器与光源）。
// 输入：fileio 工程配置；modelio 输出对象。结果写入 modelio 的 imageSize/angles/waves/
//       sensor/light 等。返回 true。
// 步骤：1 设置分辨率；2 读取太阳角与观测角表；3 读取波段与 UAV 信息；
//       4 用第 0 个角度创建初始传感器和光源（角度表为空时用天顶观测）。
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
                                      fileio->m_pRaytracingXml->lightxml.skyTemperature,
                                      fileio->m_pRaytracingXml->lightxml.shortwaveIrradiance);



    return true;

}

// 作用：切换到第 kangle 个角度：重建传感器并更新 GPU 缓冲，同时按该角度的太阳角重建光源。
// 说明：光源的直射/散射/温度/短波沿用 modelio->light 中的现有值。
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
                                    modelio->light.skyTemperature, modelio->light.Rin);
    updateLight(modelio,lightSet);

}

// 作用：UAV 飞行模式下切换到第 kpos 个位姿，用透视传感器更新 GPU 缓冲。
// 说明：越界或无角度表时直接返回；若给出每位姿的视方位角则覆盖第 0 个角度的 vaa。
void Geometry::updateSensorPos(std::shared_ptr<RaytracingIO> &modelio, int kpos) {
    if (kpos < 0 || kpos >= static_cast<int>(modelio->uavposes.size()) || modelio->angles.empty()) return;
    Angle& angle = modelio->angles.front();
    if (kpos < static_cast<int>(modelio->uavViewAzimuths.size())) angle.vaa = modelio->uavViewAzimuths[kpos];
    SensorMatrix sensor = createPerspectiveSensor(sensorWorldToXzy(modelio->uavposes[kpos]),
                                                  angle.vza, angle.vaa);
    updateSensor(modelio, sensor);
}

// 作用：同步把传感器矩阵写入 GPU 传感器缓冲（vkCmdUpdateBuffer）并插入 transfer→compute 屏障。
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

// 作用：同步把光源写入 GPU 光源缓冲，并插入 transfer→compute 屏障。
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


// 作用：计算 RayTracing 天底/斜视正射输出的栅格校正系数 cx、cy。
// 输入：vza/vaa 观测角（度）；输出：cx、cy（见 solveNadirMapping）。
// 步骤：1 求场景水平范围；2 用与 createSensor 相同的相机拟合；
//       3 把场景地面四角投影到像素坐标；4 solveNadirMapping 求双线性系数。
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

    // 步骤 3：场景地面（y=0）四角的世界坐标，经 proj·view 后做透视除法，
    //       再由 NDC 换算为像素坐标（宽 width、高 height）。
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



// 模块：VoxelEB 的几何初始化，流程同 RayTracing 版本。
// 说明：configureSensor 使用体元步长作米/单位；createLight 不传短波，
//       因为 VoxelEB 使用气象强迫 meteo 中的辐射，而非 LightSet.Rin。
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

// 作用：切换到第 kangle 个角度。VoxelEB 的太阳角由气象时间经 SPA 算法重新求得
//       （updateSolarAngle 覆盖 angle.sza/saa），再更新传感器与光源。
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
                                    modelio->light.skyTemperature, modelio->light.Rin);
    updateLight(modelio,lightSet);

}

// 作用：UAV 模式切换到第 kpos 个位姿（同 RayTracing 版本）。
void Geometry::updateSensorPos(std::shared_ptr<VoxelebIO> &modelio, int kpos) {
    if (kpos < 0 || kpos >= static_cast<int>(modelio->uavposes.size()) || modelio->angles.empty()) return;
    Angle& angle = modelio->angles.front();
    if (kpos < static_cast<int>(modelio->uavViewAzimuths.size())) angle.vaa = modelio->uavViewAzimuths[kpos];
    SensorMatrix sensor = createPerspectiveSensor(sensorWorldToXzy(modelio->uavposes[kpos]),
                                                  angle.vza, angle.vaa);
    updateSensor(modelio, sensor);
}

// 作用：把传感器矩阵写入 VoxelEB 的 GPU 传感器缓冲，并加 transfer→compute 屏障。
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

// 作用：把光源写入 VoxelEB 的 GPU 光源缓冲，并加 transfer→compute 屏障。
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

// 作用：由气象时间 meteo.t（年内日序，含日小数）、年份、时区、经纬度、气压和气温，
//       用 SPA 太阳位置算法求太阳天顶角和方位角（度），写入 angle。
// 说明：时间化整到秒（最多 86399 秒，防止进位到下一年）；气压下限 100；
//       大气折射 0.5667°；失败时抛异常。方位角约定见 SPA 输出（待核对是否自北顺时针）。
void Geometry::updateSolarAngle(std::shared_ptr<VoxelebIO> &modelio, Angle &angle) {
    SPACalc spa;
    spa_data data{};


    int kmonth,kday;
    const double t = modelio->meteo.t;
    const int kdoy = static_cast<int>(std::floor(t));
    // Round the day fraction to the nearest second; the last second of the
    // day is kept so rounding never moves past the end of the year.
    // 步骤 1：把日序小数拆成日序号、时、分、秒，再换算月、日。
    const long long secondOfDay = std::min(86399LL, std::llround((t - kdoy) * 86400.0));
    const int h = static_cast<int>(secondOfDay / 3600);
    const int m = static_cast<int>((secondOfDay % 3600) / 60);
    const int s = static_cast<int>(secondOfDay % 60);
    Utils::calculateMonthAndDay(modelio->m_year, kdoy, &kmonth, &kday);

    // 步骤 2：填充 SPA 输入，求 SPA_ZA（仅天顶角与方位角）。
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

// 作用：VoxelEB 的正射校正系数 cx、cy，算法同 RayTracing 版本（场景范围取 XZY 尺寸）。
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

    // 步骤 3：场景四角投影到像素坐标（同 RayTracing 版本）。
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


// 模块：VoxelRT 的几何初始化，流程同 RayTracing 版本，差别：
//       体元尺寸/原点用 XZY 坐标；createLight 传入 shortwaveIrradiance 作为 Rin。
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
                                 fileio->m_pVoxelrtXml->lightxml.skyTemperature,
                                 fileio->m_pVoxelrtXml->lightxml.shortwaveIrradiance);



    return true;

}

// 作用：切换到第 kangle 个角度。UAV 模式下使用固定相机（见下方 DEFERRED 说明），
//       否则按配置创建传感器；再按该角度太阳角更新光源。
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
                                    modelio->light.skyTemperature, modelio->light.Rin);
    updateLight(modelio,lightSet);

}

// 作用：UAV 模式切换到第 kpos 个位姿，并同时按当前角度重建光源（其余同 RayTracing 版本）。
void Geometry::updateSensorPos(std::shared_ptr<VoxelrtIO> &modelio, int kpos){
    if (kpos < 0 || kpos >= static_cast<int>(modelio->uavposes.size()) || modelio->angles.empty()) return;
    Angle& angle = modelio->angles.front();
    if (kpos < static_cast<int>(modelio->uavViewAzimuths.size())) angle.vaa = modelio->uavViewAzimuths[kpos];
    SensorMatrix sensorMatrix = createPerspectiveSensor(sensorWorldToXzy(modelio->uavposes[kpos]),
                                                        angle.vza, angle.vaa);

    updateSensor(modelio, sensorMatrix);

    LightSet lightSet = createLight(angle.sza, angle.saa,modelio->light.direct,
                                    modelio->light.diffuse,modelio->light.solarTemperature,
                                    modelio->light.skyTemperature, modelio->light.Rin);
    updateLight(modelio,lightSet);

}

// 作用：把传感器矩阵写入 VoxelRT 的 GPU 传感器缓冲（本版本不加屏障，见下方 DEFERRED 说明）。
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

// 作用：把光源写入 VoxelRT 的 GPU 光源缓冲（同样不加屏障）。
void Geometry::updateLight(std::shared_ptr<VoxelrtIO> &modelio, LightSet &light){


    auto &m_pBufferLight = modelio->m_pBufferLight;
    auto &m_device = modelio->m_device;
    auto &m_queueIndex = modelio->m_queueIndex;

    nvvk::CommandPool cmdBufGet(m_device, m_queueIndex);
    vk::CommandBuffer cmdBuf = cmdBufGet.createCommandBuffer();
    vkCmdUpdateBuffer(cmdBuf, (*m_pBufferLight).buffer, 0, sizeof(LightSet), &light);
    cmdBufGet.submitAndWait(cmdBuf);
}

// 作用：VoxelRT 的正射校正系数 cx、cy，算法同 RayTracing 版本。
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

    // 步骤 3：场景四角投影到像素坐标（同 RayTracing 版本）。
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
