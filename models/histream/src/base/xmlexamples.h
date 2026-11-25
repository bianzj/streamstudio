// ==========================================================================
// 文件：xmlexamples.h
// 功能：histream 内置“最小示例工程”的工厂类 XmlExamples。未给工程文件路径时，FileIO 用它
//       直接构造 RaytracingXml / VoxelRTXml / VoxelEBXml（含水体变体 WaterEB）的内存配置。
// 主要模块：
//   1. 对外接口 raytracing() / voxelrt() / voxeleb() / watereb()：首次调用才构造，
//      并创建对应 projectDir；返回的 shared_ptr 与本对象内部成员共享同一份配置
//   2. 路径辅助 rootDirectory() / exampleDirectory(name)（实现见 xmlexamples.cpp）
//   3. createMinimalVoxelebxml / createMinimalVoxelrtxml / createMinimalRaytracingxml /
//      createMinimalWaterEBxml：在头文件内联实现的 16×16 像元、1×1 采样的最小场景
//   4. 旧式 createRaytracingxml / createVoxelebxml / createVoxelrtxml：仅声明，实现在
//      xmlexamples.cpp，含开发机绝对路径，src 中未发现调用（见 .cpp 内已有说明）
// 数据流：FileIO::readXml（路径为空）→ XmlExamples::xxx() → *Xml 结构 → 引擎各模式读取。
// 与其他文件的关系：structs.h / structs_cg.h 定义 *Xml 与 SoilSet/LeafBio/WaterSet 等结构；
//   fileio.h 持有 XmlExamples 成员；defined/meteo.txt、Esky_.dat、Esun_.dat 来自 histream 根目录。
// 约定：最小示例统一为场景 8 m × 8 m × 4 m、体元 1 m、太阳角 {30, 135}（{天顶, 方位}，单位待核对，推测为度）、
//   直射/散射比例 0.8/0.2、天空温度 250 K、太阳温度 6000 K；波段 10500 nm（热红外）；
//   ThermalXml 的两个温度依次为受光/阴影温度（K）。
// ==========================================================================

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

// 类：XmlExamples —— 内置最小示例工程的惰性构造器；配置对象为成员 m_p*Xml，
//   构造过程不做 I/O，目录创建发生在 raytracing()/voxelrt()/voxeleb()/watereb() 中。
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

    // 辅助：rootDirectory() 求 histream 根目录（含 defined/meteo.txt 者，找不到则回落到 cwd）；
    //   exampleDirectory(name) 返回 <根目录>/examples/<name> 字符串，本身不建目录。
    static std::filesystem::path rootDirectory();
    static std::string exampleDirectory(const char* name);

    // 作用：构造 VoxelEB（体元能量平衡）最小示例：裸土背景 + 一个未使用的冠层/叶片占位。
    // 输出：m_pVoxelebXml；projectDir = examples/minimal_voxeleb，definedDir = 根目录。
    // 说明：占位的 unused_canopy / unused_leaf 是因为当前描述符布局要求相应缓冲区非空；
    //   土壤取 method = 1，brdfModel = 1（方向性 BRDF，Hapke 参数），气象取白天节点以覆盖该路径。
    void createMinimalVoxelebxml()
    {
        m_pVoxelebXml = std::make_shared<VoxelEBXml>();
        auto& example = *m_pVoxelebXml;

        // 步骤 1：设定工程/定义目录与采样、递归深度（n_sample = 1，maxDepth = 4，使用 GPU 0）。
        example.projectDir = exampleDirectory("minimal_voxeleb");
        example.definedDir = rootDirectory().string();

        example.settingxml.n_sample = 1;
        example.settingxml.maxDepth = 4;
        example.settingxml.theGPU = 0;

        // 步骤 2：光源（太阳角 {天顶, 方位}（单位待核对，推测为度），直射 0.8 / 散射 0.2）与传感器（16×16，平行投影，
        //   单视角天顶 0°，10500 nm，输出温度）。
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

        // 步骤 3：背景场景：8×8×4 m、水平/垂直体元 1 m，非 DEM，纬度 40°N、经度 116°E；清空实体列表。
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

        // 步骤 4：光谱与热参数：土壤自定义反射率 0.20（谱段）/ 热红外反射率 0.05，
        //   受光/阴影温度 305/295 K；冠层占位参数依次对应 Canopy 的 lai、density、height、width、G、
        //   LIDFa、LIDFb、hspot、leafwidth。
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

        // 步骤 5：材质属性：土壤参数（rss、cs、rhos、lambdas、Tsoil、smc、SatWater 与 Hapke 参数）和占位叶片。
        PropertyXml soilProperty{};
        soilProperty.name = "soil";
        soilProperty.type = Type::SOIL;
        soilProperty.soilset = SoilSet{};
        soilProperty.soilset.method = 1;
        soilProperty.soilset.rss = 2000.0f;
        soilProperty.soilset.cs = 1180.0f;
        soilProperty.soilset.rhos = 1800.0f;
        soilProperty.soilset.lambdas = 1.55f;
        soilProperty.soilset.Tsoil = 25.0f;
        soilProperty.soilset.smc = 0.25f;
        soilProperty.soilset.SatWater = 0.45f;
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

        // 步骤 6：空气动力学条件（AeroCond 字段顺序：type, L, ustar, hc_veg, hc_build, lai, leafwidth, cover）
        //   及气象/大气文件路径（均位于 definedDir\defined 下）；节点 24–25 为一个白天节点。
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

    // 作用：构造 VoxelRT（体元辐射传输）最小示例：与 VoxelEB 示例共用光源/传感器/背景设定，
    //   但 maxDepth = 2，无叶片/材质属性（propxmls 未设置）。
    // 输出：m_pVoxelrtXml；projectDir = examples/minimal_voxelrt。
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

    // 作用：构造 RayTracing（光线追踪）最小示例：同上的光源与背景，传感器多 isOrth = false，
    //   只含土壤光谱与热参数，无冠层/属性。
    // 输出：m_pRaytracingXml；projectDir = examples/minimal_raytracing。
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
    // 作用：在 VoxelEB 最小示例的副本上改为水体背景（水体 EB 回归用例）。
    // 步骤：1 复制 m_pVoxelebXml（必要时先构造）；2 背景类型/光谱/热参数改为 water（反射率 0.06，
    //   热红外 0.02，水温 298.15 K）；3 清空冠层、空气动力学冠层参数置 0；
    //   4 属性仅保留 water：WaterSet{rss = 0, 体积热容 4.186e6 J m-3 K-1, 混合层深 0.5 m,
    //   蒸发系数 1}，Fresnel/Cox–Munk BRDF（brdfModel = 1，折射率 1.333）。
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
    // 缓存的示例配置（空指针表示尚未构造）；WaterEB 示例是 VoxelEB 示例的按值拷贝，互不影响。
    std::shared_ptr<RaytracingXml> m_pRaytracingXml;
    std::shared_ptr<VoxelEBXml>   m_pVoxelebXml;
    std::shared_ptr<VoxelRTXml>   m_pVoxelrtXml;
    std::shared_ptr<VoxelEBXml>   m_pWaterEBXml;


    // 旧式示例（实现见 xmlexamples.cpp，硬编码开发机路径，src 中未发现调用）。
    void createRaytracingxml();
    void createVoxelebxml();
    void createVoxelrtxml();

};





#endif //FIELD_RAYTRACINGXMLEXAMPLE_H
