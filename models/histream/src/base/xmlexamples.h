//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_RAYTRACINGXMLEXAMPLE_H
#define FIELD_RAYTRACINGXMLEXAMPLE_H

#include "structs.h"
#include <iostream>
#include <string>
#include <fstream>
#include <memory>
#include <numbers>
#include <algorithm>
#include <vector>
#include <filesystem>
#include "utils.h"

class XmlExamples{

public:
    // Minimal built-in configurations, used when histream runs without a
    // project file. They are built on first use; construction does no I/O.
    // Paths derive from the histream directory containing defined/meteo.txt
    // (<cwd>/models/histream, <cwd> or the executable directory).
    std::shared_ptr<RaytracingXml> raytracing();
    std::shared_ptr<VoxelRTXml> voxelrt();
    std::shared_ptr<VoxelEBXml> voxeleb();
    std::shared_ptr<VoxelEBXml> watereb();

    static std::filesystem::path rootDirectory();
    static std::string exampleDirectory(const char* name);

    void createMinimalVoxelebxml()
    {
        m_pVoxelebXml = std::make_shared<VoxelEBXml>();
        auto& example = *m_pVoxelebXml;

        example.projectDir = exampleDirectory("minimal_voxeleb");
        example.definedDir = rootDirectory().string();

        example.settingxml.n_sample = 1;
        example.settingxml.maxDepth = 4;
        example.settingxml.theGPU = 0;

        example.lightxml.name = "Solar";
        example.lightxml.solarAngle = {30.0f, 135.0f};
        example.lightxml.direct = 0.8f;
        example.lightxml.diffuse = 0.2f;
        example.lightxml.skyTemperature = 250.0f;
        example.lightxml.solarTemperature = 6000.0f;

        example.sensorxml.name = "MinimalSensor";
        example.sensorxml.resolution = {16, 16};
        example.sensorxml.viewAngles = {{0.0f, 0.0f}};
        example.sensorxml.waves = {10500.0f};
        example.sensorxml.isImage = true;
        example.sensorxml.isAlbedo = false;
        example.sensorxml.isDisplay = false;
        example.sensorxml.isTemperature = true;
        example.sensorxml.projection = Projection::PARALLAL;

        auto& background = example.scenexml.background;
        background.sceneSize = {8.0f, 8.0f, 4.0f};
        background.sceneOrigin = {0.0f, 0.0f, 0.0f};
        background.stepsize_surface = 1.0f;
        background.stepsize_height = 1.0f;
        background.bgSpectralName = "soil";
        background.bgThermalName = "soil_temperature";
        background.bgPropName = "soil";
        background.isDEM = false;
        background.lat = 40.0f;
        background.lon = 116.0f;

        example.scenexml.objEntities.clear();
        example.scenexml.primEntities.clear();

        SpectralXml soilOptical{};
        soilOptical.spectralName = "soil";
        soilOptical.type = spectralType::CUSTOM;
        soilOptical.reflectances = {0.20f};
        soilOptical.transmittance = {0.0f};
        soilOptical.refl_tir = 0.05f;
        soilOptical.tau_tir = 0.0f;
        example.spectralxmls = {soilOptical};

        example.thermalxmls = {{"soil_temperature", 305.0f, 295.0f}};
        example.canopyxmls = {
            {"unused_canopy", {0.0f, 0.0f, 1.0f, 1.0f, 0.5f,
                                -0.35f, -0.15f, 0.2f, 0.1f}}
        };

        PropertyXml soilProperty{};
        soilProperty.name = "soil";
        soilProperty.type = Type::SOIL;
        // DEFERRED[高] Bug: SoilSet 字段顺序为 method, rss, cs, rhos, lambdas, Tsoil, smc, SatWater，这里按位置初始化后 Tsoil=0.25、smc=25.0，两值互换（注释里 Tsoil≈25、smc≈0.25）；土壤温度初值/含水量严重错误（smc=25 超过饱和含水量 0.45），影响土壤蒸发阻抗与热传导；建议：改为指定成员赋值（soilset.Tsoil=25, soilset.smc=0.25），避免位置聚合初始化。
        soilProperty.soilset = SoilSet{1, 2000.0f, 1180.0f, 1800.0f,
                                       1.55f, 0.25f, 25.0f, 0.45f};
        soilProperty.soilset.brdfModel = 1;
        soilProperty.soilset.hapkeB0 = 1.0f;
        soilProperty.soilset.hapkeH = 0.1f;
        soilProperty.soilset.hapkeG = 0.0f;

        // The current descriptor layout always requires a non-empty leaf buffer.
        PropertyXml fallbackLeaf{};
        fallbackLeaf.name = "unused_leaf";
        fallbackLeaf.type = Type::VEGETATION;
        fallbackLeaf.leafbio = LeafBio{80.0f, 9.0f, 0.01f, 3.0f, 0.6396f, 0.015f,
                                       {0.2f, 0.3f, 288.0f, 313.0f, 328.0f},
                                       25.0f, 0.507f, 0.0f, 1.0f, 1.0f, 0};
        example.propxmls = {soilProperty, fallbackLeaf};

        example.aerocondxml = {
            AeroType::ONE,
            // DEFERRED[低] 数值: 位置聚合初始化依赖 AeroCond 字段顺序（type, L, ustar, hc_veg, hc_build, lai, leafwidth, cover），L(Monin-Obukhov 长度)=0；若空气动力学阻抗计算直接用 z/L 稳定度函数而未对 L=0 特判，将除零得到 inf/NaN；建议：用指定成员赋值并给 L 一个中性层结大值（如 -1e6），或确认着色器对 L=0 有保护。
            AeroCond{0, 0.0f, 0.3f, 1.0f, 1.0f, 0.0f, 0.1f, 1.0f},
            "",
            1000.0f
        };

        // Daytime node: exercises directional surface BRDF in the regression case.
        example.meteoxml.startTimeNode = 24;
        example.meteoxml.endTimeNode = 25;
        example.meteoxml.meteofile = example.definedDir + R"(\defined\meteo.txt)";
        example.atomcondxml.rlifile = example.definedDir + R"(\defined\Esky_.dat)";
        example.atomcondxml.rinfile = example.definedDir + R"(\defined\Esun_.dat)";
    }

    void createMinimalVoxelrtxml()
    {
        m_pVoxelrtXml = std::make_shared<VoxelRTXml>();
        auto& example = *m_pVoxelrtXml;

        example.projectDir = exampleDirectory("minimal_voxelrt");
        example.definedDir = rootDirectory().string();

        example.settingxml.n_sample = 1;
        example.settingxml.maxDepth = 2;
        example.settingxml.theGPU = 0;
        example.settingxml.isUAVtrave = false;

        example.lightxml = {"Solar", {30.0f, 135.0f}, 0.8f, 0.2f, 250.0f, 6000.0f};
        example.sensorxml.name = "MinimalVoxelRTSensor";
        example.sensorxml.resolution = {16, 16};
        example.sensorxml.viewAngles = {{0.0f, 0.0f}};
        example.sensorxml.waves = {10500.0f};
        example.sensorxml.isImage = true;
        example.sensorxml.isAlbedo = false;
        example.sensorxml.isDisplay = false;
        example.sensorxml.isTemperature = true;
        example.sensorxml.projection = Projection::PARALLAL;

        auto& background = example.scenexml.background;
        background.sceneSize = {8.0f, 8.0f, 4.0f};
        background.sceneOrigin = {0.0f, 0.0f, 0.0f};
        background.stepsize_surface = 1.0f;
        background.stepsize_height = 1.0f;
        background.bgSpectralName = "soil";
        background.bgThermalName = "soil_temperature";
        background.bgPropName = "soil";
        background.isDEM = false;
        background.lat = 40.0f;
        background.lon = 116.0f;

        example.scenexml.objEntities.clear();
        example.scenexml.primEntities.clear();

        SpectralXml soil{};
        soil.spectralName = "soil";
        soil.type = spectralType::CUSTOM;
        soil.reflectances = {0.20f};
        soil.transmittance = {0.0f};
        soil.refl_tir = 0.05f;
        soil.tau_tir = 0.0f;
        example.spectralxmls = {soil};
        example.thermalxmls = {{"soil_temperature", 305.0f, 295.0f}};

        // VoxelRT 的描述符布局要求 canopy 缓冲区非空。
        example.canopyxmls = {
            {"unused_canopy", {0.0f, 0.0f, 1.0f, 1.0f, 0.5f,
                                -0.35f, -0.15f, 0.2f, 0.1f}}
        };
    }

    void createMinimalRaytracingxml()
    {
        m_pRaytracingXml = std::make_shared<RaytracingXml>();
        auto& example = *m_pRaytracingXml;

        example.projectDir = exampleDirectory("minimal_raytracing");
        example.definedDir = rootDirectory().string();

        example.settingxml.n_sample = 1;
        example.settingxml.maxDepth = 2;
        example.settingxml.theGPU = 0;

        example.lightxml = {"Solar", {30.0f, 135.0f}, 0.8f, 0.2f, 250.0f, 6000.0f};
        example.sensorxml.name = "MinimalRaytracingSensor";
        example.sensorxml.resolution = {16, 16};
        example.sensorxml.viewAngles = {{0.0f, 0.0f}};
        example.sensorxml.waves = {10500.0f};
        example.sensorxml.isImage = true;
        example.sensorxml.isOrth = false;
        example.sensorxml.isAlbedo = false;
        example.sensorxml.isDisplay = false;
        example.sensorxml.isTemperature = true;
        example.sensorxml.projection = Projection::PARALLAL;

        auto& background = example.scenexml.background;
        background.sceneSize = {8.0f, 8.0f, 4.0f};
        background.sceneOrigin = {0.0f, 0.0f, 0.0f};
        background.stepsize_surface = 1.0f;
        background.stepsize_height = 1.0f;
        background.bgSpectralName = "soil";
        background.bgThermalName = "soil_temperature";
        background.isDEM = false;

        example.scenexml.objEntities.clear();
        example.scenexml.primEntities.clear();

        SpectralXml soil{};
        soil.spectralName = "soil";
        soil.type = spectralType::CUSTOM;
        soil.reflectances = {0.20f};
        soil.transmittance = {0.0f};
        soil.refl_tir = 0.05f;
        soil.tau_tir = 0.0f;
        example.spectralxmls = {soil};
        example.thermalxmls = {{"soil_temperature", 305.0f, 295.0f}};
    }
    void createMinimalWaterEBxml()
    {
        if (!m_pVoxelebXml) createMinimalVoxelebxml();
        m_pWaterEBXml = std::make_shared<VoxelEBXml>(*m_pVoxelebXml);
        auto& example = *m_pWaterEBXml;
        example.projectDir = exampleDirectory("minimal_watereb");

        auto& background = example.scenexml.background;
        background.type = Type::WATER;
        background.bgSpectralName = "water";
        background.bgThermalName = "water_temperature";
        background.bgPropName = "water";

        SpectralXml waterOptical{};
        waterOptical.spectralName = "water";
        waterOptical.type = spectralType::CUSTOM;
        waterOptical.reflectances = {0.06f};
        waterOptical.transmittance = {0.0f};
        waterOptical.refl_tir = 0.02f;
        waterOptical.tau_tir = 0.0f;
        example.spectralxmls = {waterOptical};
        example.thermalxmls = {{"water_temperature", 298.15f, 298.15f}};
        example.canopyxmls.clear();
        example.aerocondxml.aerocond.hc_veg = 0.0f;
        example.aerocondxml.aerocond.lai = 0.0f;

        PropertyXml water{};
        water.name = "water";
        water.type = Type::WATER;
        water.waterset = WaterSet{0.0f, 4.186e6f, 0.5f, 1.0f};
        water.waterset.brdfModel = 1;
        water.waterset.refractiveIndex = 1.333f;
        water.waterset.slopeVariance = 0.0f;
        water.waterset.diffuseFraction = 0.02f;
        example.propxmls = {water};
    }
    std::shared_ptr<RaytracingXml> m_pRaytracingXml;
    std::shared_ptr<VoxelEBXml>   m_pVoxelebXml;
    std::shared_ptr<VoxelRTXml>   m_pVoxelrtXml;
    std::shared_ptr<VoxelEBXml>   m_pWaterEBXml;


    void createRaytracingxml();
    void createVoxelebxml();
    void createVoxelrtxml();

};





#endif //FIELD_RAYTRACINGXMLEXAMPLE_H
