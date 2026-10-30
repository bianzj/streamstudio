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

class Geometry {
public:
    Geometry(){};



    SensorMatrix createSensor(glm::vec3 size, glm::vec3 origin, float vza, float vaa, float ratio = 1.0);
    LightSet createLight(float sza, float saa, float direct, float diffuse,float,float);
    SensorMatrix createSensor(glm::vec3 sensorPos_XZY, glm::vec3 center_XZY,
                              glm::vec3 sceneSize_XZY, float fovDegrees = 0.0f,
                              glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f));

    bool createGeometry(std::shared_ptr<FileIO> &fileio, std::shared_ptr<RaytracingIO> &modelio);
    void updateAngle(std::shared_ptr<RaytracingIO> &modelio, int kangle);
    void updateSensorPos(std::shared_ptr<RaytracingIO> &modelio, int kPos);
    void updateSensor(std::shared_ptr<RaytracingIO> &modelio, SensorMatrix &sensor);
    void updateLight(std::shared_ptr<RaytracingIO> &modelio, LightSet &light);
    void orthcorrect(std::shared_ptr<RaytracingIO> &modelio,float vza, float vaa,
                     Eigen::VectorXd & cx, Eigen::VectorXd & cy);


    bool createGeometry(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &modelio);
    void updateAngle(std::shared_ptr<VoxelebIO> &modelio, int kangle);
    void updateSensorPos(std::shared_ptr<VoxelebIO> &modelio, int kPos);
    void updateSensor(std::shared_ptr<VoxelebIO> &modelio, SensorMatrix &sensor);
    void updateLight(std::shared_ptr<VoxelebIO> &modelio, LightSet &light);
    void orthcorrect(std::shared_ptr<VoxelebIO> &modelio,float vza, float vaa,
                               Eigen::VectorXd &cx, Eigen::VectorXd &cy);
    static void updateSolarAngle(std::shared_ptr<VoxelebIO> &modelio, Angle &angle);


    bool createGeometry(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &modelio);
    void updateAngle(std::shared_ptr<VoxelrtIO> &modelio, int kangle);
    void updateSensorPos(std::shared_ptr<VoxelrtIO> &modelio, int kPos);

    void updateSensor(std::shared_ptr<VoxelrtIO> &modelio, SensorMatrix &sensor);
    void updateLight(std::shared_ptr<VoxelrtIO> &modelio, LightSet &light);
    void orthcorrect(std::shared_ptr<VoxelrtIO> &modelio,float vza, float vaa,
                     Eigen::VectorXd & cx, Eigen::VectorXd & cy);


private:
    void configureSensor(const SensorXml& sensor, glm::vec3 sceneSize_XYZ, float metresPerUnit);
    SensorMatrix createConfiguredSensor(glm::vec3 size_XZY, glm::vec3 origin_XZY,
                                        float vza, float vaa, float ratio = 1.0f);
    SensorMatrix createPerspectiveSensor(glm::vec3 position_XZY, float vza, float vaa);
    glm::vec3 sensorWorldToXzy(glm::vec3 position_XYZ) const;

    Projection m_sensorProjection{Projection::PARALLAL};
    glm::vec3 m_sensorPosition_XZY{0.0f, 3000.0f, 0.0f};
    glm::vec3 m_sensorTarget_XZY{0.0f, 0.0f, 0.0f};
    glm::vec3 m_sensorSceneSize_XZY{1.0f, 1.0f, 1.0f};
    float m_sensorMetresPerUnit{1.0f};
    float m_sensorFov{60.0f};
};


#endif //FIELD_GEOMETRY_H
