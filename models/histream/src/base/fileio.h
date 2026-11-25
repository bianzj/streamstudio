// ==========================================================================
// 文件：fileio.h
// 功能：声明 FileIO——工程输入的读取（XML / JSON）与计算结果的写出（GeoTIFF / ENVI）。
// 主要模块：
//   1. 入口：readXml（工程 XML，fileio.cpp）、readJson（工程 JSON，fileio_json.cpp）
//   2. 按节点解析的 readXxxXML 与工具 sonExists
//   3. 外部数据读取：readDefined（optipar.txt）、readMeteo（气象表与入射光谱）
//   4. 影像输出：writeTIFData、writeENVIdata
//   5. 解析结果成员：各模式的 Xml 对象、气象元数据、统计初始化记录
// 数据流：engine 创建 FileIO → readXml/readJson 填充 m_pRaytracingXml / m_pVoxelrtXml / m_pVoxelebXml
//       → 各模式从这些对象取配置；计算结束后调用 write* 输出结果。
// 与其他文件：fileio.cpp / fileio_json.cpp 为实现；structs.h 定义 *Xml 结构；xmlexamples.h 提供内置工程。
// 约定：角度为度；长度 m；一个 FileIO 实例对应一次仿真运行。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#ifndef FIELD_FILEIO_H
#define FIELD_FILEIO_H

#include "structs.h"
#include "src/thirdparty/tinyxml.h"
#include "gdal.h"
#include <iostream>
#include <array>
#include <string>
#include <fstream>
#include <memory>
#include <numbers>
#include <algorithm>
#include <unordered_set>
#include "xmlexamples.h"
#include "src/base/utils.h"
#include "src/base/defined.h"

// 类：FileIO。解析工程输入并保存各模式的配置，同时负责结果影像输出。
// 说明：多数 readXxxXML 成员为解析细节（被注释的 private: 表明原意是私有，目前公开）。
class FileIO {

public:
    FileIO(){};

    // 读取工程 XML 并填充对应模式的 *Xml 成员；path 为空时使用内置示例工程。成功返回 true。
    bool readXml(std::string path, Mode mode);
    // 读取工程 JSON（实现在 fileio_json.cpp），结果同样写入各模式的 *Xml 成员。
    bool readJson(const std::string& path, Mode mode);

    //bool readRayTracingXML(std::string inputPath){ return true;};
    //void writeTif(std::string outDir,void *images,glm::vec4 angles, std::vector<float> bands, glm::vec2 resolution);



public:

    // 解析结果：按当前模式只有对应的一个对象被填充（其余为空指针）。
    std::shared_ptr<RaytracingXml> m_pRaytracingXml;
    std::shared_ptr<VoxelEBXml>   m_pVoxelebXml;
    std::shared_ptr<VoxelRTXml>   m_pVoxelrtXml;

//private:
    // 各节点解析函数：node 为 XML 中对应的父节点，缺失时多返回缺省值并打印错误。
    std::vector<SpectralXml> readSpectralXML(TiXmlNode *node, Mode mode);
    std::vector<ThermalXml> readThermalXML(TiXmlNode *node, Mode mode);
    SensorXml readSensorXML(TiXmlNode *node, Mode mode);
    LightXml readLightXML(TiXmlNode *node, Mode mode);
    AtmosphereXml readAtmosphereXML(TiXmlNode *node);
    SettingXml readSettingXML(TiXmlNode *node, Mode mode);
    SceneXml readSceneXML(TiXmlNode *node, Mode mode);
    std::vector<CanopyXml> readCanopyXML(TiXmlNode *node, Mode mode);
    std::vector<PropertyXml> readPropertyXML(TiXmlNode *node, Mode mode);
    AtomCondXml readAtomCondXML(TiXmlNode *node, Mode mode);
    MeteoXml readMeteoXML(TiXmlNode *node, Mode mode);
    AeroCondXml readAeroXML(TiXmlNode *node, Mode mode);

    // 判断 parentEle 是否有名为 sonName 的直接子元素。
    bool sonExists(std::string sonName, TiXmlElement* parentEle);
    // 读取 defined 目录下的光学常数表（optipar.txt）到 defineio。
    void readDefined(std::shared_ptr<DefinedIO> & defineio);
    // 读取 VoxelEB 气象表：输出节点数 n_node、逐节点气象 meteos，以及波长网格与入射直射/散射系数 wavesets。
    void readMeteo(std::shared_ptr<DefinedIO> &defineio,int & n_node, std::vector<Meteo> &meteos, std::vector<AtomCond> &wavesets);
    // Filled by readMeteo for light.diffuseFractionMode = erbs: optional
    // columns 8-9 (horizontal direct, diffuse W m-2; NaN when absent) per row,
    // and the VNIR Esun/Esky coefficients normalized to fraction 1 (direct in
    // .direct, diffuse in .diffuse) that a node's split scales.
    std::vector<std::array<float, 2>> m_meteoMeasuredSplit;
    std::vector<AtomCond> m_unitShortwaveShapes;
    // 写 ENVI 影像（.img + .hdr）。注意：这个无 k 参数的重载在 fileio.cpp 中没有定义（实现已被注释），
    // 仅有带 k 的重载；调用时若省略 k 会出现重载歧义或链接错误（疑似遗留声明，待核对）。
    void writeENVIdata(std::string projectDir,float * pData, int width, int height, int band, Angle &angle, float t = -1);
    void writeENVIdata(std::string projectDir,float * pData, int width, int height, int band, Angle &angle, float t = -1,int k = -1);
    // 写多波段 GeoTIFF 并追加统计 CSV；参数含义见 fileio.cpp 中的实现说明。
    bool writeTIFData(const std::string& projectDir, const float* pData, int width, int height,
                      int band, const Angle& angle, float t, const std::string& modelSuffix,
                      int positionIndex = -1, bool flipRows = false,
                      const std::vector<std::string>& bandNames = {});

    // 成员状态：XML 根元素、当前模式、太阳方位角 SAA（度，供 BRF 观测角用）、工程文件所在目录。
    TiXmlElement *RootElement;
    Mode m_mode;
    float SAA;
    std::string m_inputDirectory;

    // 输出缓存：影像与统计量（由引擎在计算过程中填写，用途待核对）。
    std::vector<float> outImage1;
    std::vector<std::vector<float>> outImage;
    std::vector<std::vector<float>> outImage_orth;
    std::vector<float> outImageMeanValue;

    // 内置示例工程生成器（readXml 路径为空时使用）。
    XmlExamples xmlexamples;

private:
    MeteoXml m_meteoXml;
    // A FileIO instance belongs to one simulation run. The first raster for
    // each model replaces stale statistics; subsequent rasters append.
    std::unordered_set<std::string> m_initializedStatisticsPaths;


};


#endif //FIELD_FILEIO_H
