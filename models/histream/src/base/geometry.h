// ==========================================================================
// 文件：geometry.h
// 功能：声明 Geometry 类：把工程配置转成相机（SensorMatrix）与光源（LightSet）并更新 GPU 缓冲
// 主要模块：
//   1. 公共的 createSensor / createLight 构造函数（平行传感器、透视传感器、光源）
//   2. RayTracing / VoxelEB / VoxelRT 三套重载：createGeometry、updateAngle、updateSensorPos、
//      updateSensor、updateLight、orthcorrect（VoxelEB 另有静态 updateSolarAngle）
//   3. 私有的传感器配置缓存（投影类型、位置、注视点、场景尺寸、米/单位、视场）
// 数据流：FileIO → createGeometry 填充 modelio 的角度表/初始传感器/光源 →
//         逐角度 updateAngle、UAV 逐位姿 updateSensorPos → 渲染 → orthcorrect 取校正系数
// 与其他文件的关系：geometry.cpp 实现；raytracingio.h / voxelebio.h / voxelrtio.h 提供各模型 IO
// 约定：XZY 后缀的向量按 X、Z（高度）、Y 排列；角度单位为度，方位角自北顺时针
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_GEOMETRY_H
#define FIELD_GEOMETRY_H

#include "src/raytracing/raytracingio.h"
#include "src/voxeleb/voxelebio.h"
#include "src/voxelrt/voxelrtio.h"
#include "nvh/cameramanipulator.hpp"
#include "nvmath/nvmath.h"
#include "utils.h"
#include "thirdparty/spa.h"
#include <Eigen/Dense>

// 类：Geometry —— 相机/光源/角度管理器，无需实例状态之外的资源；
// 三类模型使用同名重载，只在 modelio 类型上不同。
class Geometry {
public:
    Geometry(){};



    // 平行（正射）传感器：按场景大小与观测角拟合相机。
    SensorMatrix createSensor(glm::vec3 size, glm::vec3 origin, float vza, float vaa, float ratio = 1.0);
    // 由太阳天顶/方位角与辐射量构造光源（未命名的两个 float 实为 solarT、skyT，见 cpp）。
    LightSet createLight(float sza, float saa, float direct, float diffuse,float,float, float shortwave = 0.0f);
    // 透视传感器：指定相机位置、注视点、场景尺寸、视场角（≤0 时自动拟合）与 up。
    SensorMatrix createSensor(glm::vec3 sensorPos_XZY, glm::vec3 center_XZY,
                              glm::vec3 sceneSize_XZY, float fovDegrees = 0.0f,
                              glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f));

    // —— RayTracing 版本 ——
    bool createGeometry(std::shared_ptr<FileIO> &fileio, std::shared_ptr<RaytracingIO> &modelio);
    void updateAngle(std::shared_ptr<RaytracingIO> &modelio, int kangle);
    void updateSensorPos(std::shared_ptr<RaytracingIO> &modelio, int kPos);
    void updateSensor(std::shared_ptr<RaytracingIO> &modelio, SensorMatrix &sensor);
    void updateLight(std::shared_ptr<RaytracingIO> &modelio, LightSet &light);
    void orthcorrect(std::shared_ptr<RaytracingIO> &modelio,float vza, float vaa,
                     Eigen::VectorXd & cx, Eigen::VectorXd & cy);


    // —— VoxelEB 版本 ——
    bool createGeometry(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &modelio);
    void updateAngle(std::shared_ptr<VoxelebIO> &modelio, int kangle);
    void updateSensorPos(std::shared_ptr<VoxelebIO> &modelio, int kPos);
    void updateSensor(std::shared_ptr<VoxelebIO> &modelio, SensorMatrix &sensor);
    void updateLight(std::shared_ptr<VoxelebIO> &modelio, LightSet &light);
    void orthcorrect(std::shared_ptr<VoxelebIO> &modelio,float vza, float vaa,
                               Eigen::VectorXd &cx, Eigen::VectorXd &cy);
    // 按 modelio 中的气象时间、经纬度用 SPA 算法重算太阳角。
    static void updateSolarAngle(std::shared_ptr<VoxelebIO> &modelio, Angle &angle);


    // —— VoxelRT 版本 ——
    bool createGeometry(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &modelio);
    void updateAngle(std::shared_ptr<VoxelrtIO> &modelio, int kangle);
    void updateSensorPos(std::shared_ptr<VoxelrtIO> &modelio, int kPos);

    void updateSensor(std::shared_ptr<VoxelrtIO> &modelio, SensorMatrix &sensor);
    void updateLight(std::shared_ptr<VoxelrtIO> &modelio, LightSet &light);
    void orthcorrect(std::shared_ptr<VoxelrtIO> &modelio,float vza, float vaa,
                     Eigen::VectorXd & cx, Eigen::VectorXd & cy);


private:
    // 私有：缓存传感器配置（换算到场景单位）；其余私有函数见 geometry.cpp。
    void configureSensor(const SensorXml& sensor, glm::vec3 sceneSize_XYZ, float metresPerUnit);
    SensorMatrix createConfiguredSensor(glm::vec3 size_XZY, glm::vec3 origin_XZY,
                                        float vza, float vaa, float ratio = 1.0f);
    SensorMatrix createPerspectiveSensor(glm::vec3 position_XZY, float vza, float vaa);
    glm::vec3 sensorWorldToXzy(glm::vec3 position_XYZ) const;

    // 以下成员由 configureSensor 填写，默认值只在未配置时生效（缺省：平行投影，
    // 高 3000 处俯视，场景尺寸 1，视场 60°）。
    Projection m_sensorProjection{Projection::PARALLAL};
    glm::vec3 m_sensorPosition_XZY{0.0f, 3000.0f, 0.0f};
    glm::vec3 m_sensorTarget_XZY{0.0f, 0.0f, 0.0f};
    glm::vec3 m_sensorSceneSize_XZY{1.0f, 1.0f, 1.0f};
    float m_sensorMetresPerUnit{1.0f};
    float m_sensorFov{60.0f};
};


#endif //FIELD_GEOMETRY_H
