// ==========================================================================
// 文件：fileio.cpp
// 功能：FileIO 的实现——读取 StreamStudio 工程 XML（RayTracing / VoxelRT / VoxelEB 模式）并填充 *Xml 配置结构，
//       以及读取气象文件、入射光谱，把计算结果写成 GeoTIFF / ENVI 影像和统计 CSV。
// 主要模块：
//   1. readXml：工程 XML 总入口，按模式分发到各 readXxxXML（空路径时用 XmlExamples 的内置工程）
//   2. 各节点解析：Control(readSettingXML)、Geometry(readLightXML/readSensorXML)、Atmosphere、
//      Scene(readSceneXML，含背景、DEM、地形环境、气象经纬度、场景对象)、
//      Attribute(Spectral/Thermal/Canopy/Biochemistry/Aerodynamics)、Meteorology
//   3. readDefined：读取 defined/optipar.txt（PROSPECT/Fluspect 光学常数）
//   4. readMeteo：读取气象表、建立 400–50000 nm 波长网格并归一化入射直射/散射光谱
//   5. writeTIFData / writeENVIdata：输出影像与逐波段统计
// 数据流：工程 XML → FileIO::readXml → m_pRaytracingXml / m_pVoxelrtXml / m_pVoxelebXml（structs.h）
//       → 各模式的场景构建与求解；计算结果 → writeTIFData → projectDir 下的 .tif 与 result_statistics_*.csv。
// 与其他文件：fileio.h 声明类；structs.h / structs_cg.h 定义各 Xml 与 GPU 结构；xmlexamples.h 提供缺省工程；
//       radiation_spectrum.h 提供入射光谱读取与归一化；shortwave_split.h 提供直射/散射模式解析；
//       myfunction.h 提供字符串拆分工具。
// 约定：角度均为度（VZA/VAA 观测天顶/方位角，SZA/SAA 太阳天顶/方位角）；长度 m；波长 nm（待核对）；
//       时间 t 为儒略日（DOY）加日内小数；必填节点缺失多数会直接解引用空指针（见各函数说明）。
// 注意：只读取 XML，不做物理合理性的完整校验；多数可选字段缺省时保持旧工程行为。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>
#include "fileio.h"
#include "radiation_spectrum.h"
#include "shortwave_split.h"
#pragma once
//#include "tinyxml.h"
#include "structs.h"
#include "myfunction.h"
#include "stdlib.h"
#include <stack>
#include <string>
#include "xmlexamples.h"

using namespace std;

// 模块：工程 XML 总入口 readXml。
// 输入：Path 为工程 XML 路径（空串表示使用内置示例工程）；mode 为运行模式。
// 输出：按模式填充 m_pRaytracingXml / m_pVoxelebXml / m_pVoxelrtXml，并设置 m_inputDirectory。
// 说明：各子节点由 readXxxXML 逐个解析；FacetRT/FacetEB 等其它模式不在此函数处理（待核对）。
// 返回：成功 true；根节点缺失返回 false；文件无法加载时直接 exit(1)。
bool FileIO::readXml(std::string Path, Mode mode) {
    m_mode = mode;
    SAA = 0.0f; // FIXED: SAA 原为未初始化成员，readLightXML 之前若被 BRF 角度使用会读到垃圾值
    // 步骤 1：记录工程文件所在目录，后续相对路径的资源（气象、DEM、LUT 等）据此解析。
    m_inputDirectory = Path.empty() ? std::string() : std::filesystem::path(Path).parent_path().string();
    // 步骤 2：路径为空时不读文件，直接使用 XmlExamples 提供的内置缺省工程。
    if (Path.empty()) {
        if (m_mode == Mode::eRaytracing) {
            m_pRaytracingXml = xmlexamples.raytracing();
        } else if (m_mode == Mode::eVoxelRT) {
            m_pVoxelrtXml = xmlexamples.voxelrt();
        } else {
            m_pVoxelebXml = xmlexamples.voxeleb();
            m_meteoXml = m_pVoxelebXml->meteoxml;
        }
        return true;
    }

    // std::string filePath = Path;
    // DEFERRED[中] 错误处理: 加载失败直接 exit(1)，跳过析构（Vulkan 设备/GDAL 等未释放）且调用方无法捕获；TinyXML 以窄字符 fopen 打开，UTF-8 中文路径失败；建议：抛 std::runtime_error（带路径），用 _wfopen 读入后 Parse。
    // 步骤 3：加载 XML 文档。mydoc 用 static 保存，使 RootElement 指向的节点在函数返回后仍有效。
    static TiXmlDocument mydoc; // tinyxml.h
    mydoc = TiXmlDocument(Path.c_str());
    bool isloadOk = mydoc.LoadFile();
    if (!isloadOk)
    {
        std::cout << "could not load the test file.Error:" << mydoc.ErrorDesc() << std::endl;
        exit(1);
    }
    RootElement = mydoc.RootElement(); // root node of xml
    if (RootElement == nullptr)
    {
        std::cerr << "could not find root element in xml: " << Path << std::endl;
        return false;
    }

    // 步骤 4：按模式分别解析各一级节点（Control/Geometry/Atmosphere/Attribute/Scene/Meteorology）。
    // 各模式需要的节点不同：RayTracing 无 Canopy/Property/Meteorology；VoxelRT 无 Meteorology；
    // VoxelEB 全部读取（含气象与空气动力学）。
    if(m_mode == Mode::eRaytracing)
    {
        m_mode = Mode::eRaytracing;
        m_pRaytracingXml = std::make_shared<RaytracingXml>();
        // m_pRaytracingXml->projectDir = Path;
        m_pRaytracingXml->settingxml = readSettingXML(RootElement->FirstChild("Control"), m_mode);
        m_pRaytracingXml->lightxml = readLightXML(RootElement->FirstChild("Geometry"), m_mode);
        m_pRaytracingXml->sensorxml = readSensorXML(RootElement->FirstChild("Geometry"), m_mode);
        m_pRaytracingXml->atmospherexml = readAtmosphereXML(RootElement->FirstChild("Atmosphere"));
        m_pRaytracingXml->spectralxmls = readSpectralXML(RootElement->FirstChild("Attribute"), m_mode);
        m_pRaytracingXml->thermalxmls = readThermalXML(RootElement->FirstChild("Attribute"), m_mode);
        m_pRaytracingXml->scenexml = readSceneXML(RootElement->FirstChild("Scene"), m_mode);
    }else if(m_mode == Mode::eVoxelEB){

        m_mode = Mode::eVoxelEB;
        m_pVoxelebXml = std::make_shared<VoxelEBXml>();
        // m_pVoxelebXml->projectDir = Path;

        m_pVoxelebXml->settingxml = readSettingXML(RootElement->FirstChild("Control"), m_mode);
        m_pVoxelebXml->lightxml = readLightXML(RootElement->FirstChild("Geometry"), m_mode);
        m_pVoxelebXml->sensorxml = readSensorXML(RootElement->FirstChild("Geometry"), m_mode);
        m_pVoxelebXml->atmospherexml = readAtmosphereXML(RootElement->FirstChild("Atmosphere"));
        m_pVoxelebXml->scenexml = readSceneXML(RootElement->FirstChild("Scene"), m_mode);
        m_pVoxelebXml->spectralxmls = readSpectralXML(RootElement->FirstChild("Attribute"), m_mode);
        m_pVoxelebXml->thermalxmls = readThermalXML(RootElement->FirstChild("Attribute"), m_mode);
        m_pVoxelebXml->canopyxmls = readCanopyXML(RootElement->FirstChild("Attribute"), m_mode);
        m_pVoxelebXml->propxmls = readPropertyXML(RootElement->FirstChild("Attribute"), m_mode);
        // m_pVoxelebXml->atomcondxml = readAtomCondXML(RootElement->FirstChild("Geometry"), m_mode);
        m_pVoxelebXml->meteoxml = readMeteoXML(RootElement->FirstChild("Meteorology"), m_mode);
        m_pVoxelebXml->aerocondxml= readAeroXML(RootElement->FirstChild("Attribute"), m_mode);
    }

    else if(m_mode == Mode::eVoxelRT){
        m_mode = Mode::eVoxelRT;
        m_pVoxelrtXml = std::make_shared<VoxelRTXml>();
        // m_pVoxelrtXml->projectDir = Path;
        m_pVoxelrtXml->settingxml = readSettingXML(RootElement->FirstChild("Control"), m_mode);
        m_pVoxelrtXml->lightxml = readLightXML(RootElement->FirstChild("Geometry"), m_mode);
        m_pVoxelrtXml->sensorxml = readSensorXML(RootElement->FirstChild("Geometry"), m_mode);
        m_pVoxelrtXml->atmospherexml = readAtmosphereXML(RootElement->FirstChild("Atmosphere"));
        m_pVoxelrtXml->scenexml = readSceneXML(RootElement->FirstChild("Scene"), m_mode);
        m_pVoxelrtXml->spectralxmls = readSpectralXML(RootElement->FirstChild("Attribute"), m_mode);
        m_pVoxelrtXml->canopyxmls = readCanopyXML(RootElement->FirstChild("Attribute"), m_mode);
        m_pVoxelrtXml->propxmls = readPropertyXML(RootElement->FirstChild("Attribute"), m_mode);
        m_pVoxelrtXml->thermalxmls = readThermalXML(RootElement->FirstChild("Attribute"), m_mode);
    }
    return true;

}

// 模块：解析 <Atmosphere> 节点（大气订正选项）。
// 输入：node 可为空（缺省返回默认值，enabled=false）。
// 输出：AtmosphereXml；model/aerosol 非法时回退到 midlatitude-summer / rural，
// waterVapor 限幅到 [0.5, 5.0]（单位待核对，疑为 g cm-2），visibility 限幅到 [10, 50] km。
AtmosphereXml FileIO::readAtmosphereXML(TiXmlNode* node) {
    AtmosphereXml atmosphere;
    if (node == nullptr || node->ToElement() == nullptr) return atmosphere;
    TiXmlElement* element = node->ToElement();
    if (sonExists("enabled", element)) {
        const char* value = element->FirstChildElement("enabled")->GetText();
        atmosphere.enabled = value != nullptr && (std::string(value) == "1" || std::string(value) == "true");
    }
    if (sonExists("model", element)) {
        const char* value = element->FirstChildElement("model")->GetText();
        if (value != nullptr) atmosphere.model = value;
    }
    if (atmosphere.model != "tropical" && atmosphere.model != "midlatitude-summer"
        && atmosphere.model != "midlatitude-winter") atmosphere.model = "midlatitude-summer";
    if (sonExists("waterVapor", element)) {
        const char* value = element->FirstChildElement("waterVapor")->GetText();
        if (value != nullptr) atmosphere.waterVapor = std::clamp(std::stof(value), 0.5f, 5.0f);
    }
    if (sonExists("aerosol", element)) {
        const char* value = element->FirstChildElement("aerosol")->GetText();
        if (value != nullptr) atmosphere.aerosol = value;
    }
    if (atmosphere.aerosol != "rural" && atmosphere.aerosol != "urban") atmosphere.aerosol = "rural";
    if (sonExists("visibility", element)) {
        const char* value = element->FirstChildElement("visibility")->GetText();
        if (value != nullptr) atmosphere.visibility = std::clamp(std::stof(value), 10.0f, 50.0f);
    }
    if (sonExists("lutFile", element)) {
        const char* value = element->FirstChildElement("lutFile")->GetText();
        if (value != nullptr) atmosphere.lutFile = value;
    }
    return atmosphere;
}


// 模块：解析 <Attribute>/<Aerodynamics>（空气动力学条件，VoxelEB 使用）。
// 输入：<Attribute> 节点。输出：AeroCondXml（AeroType::ONE，整个场景共用一组 AeroCond）。
// 每个子条目必须含 type/L/ustar/hc_veg/hc_build/lai/leaf_width/stepsizeatmos，否则跳过；
// 存在多个有效条目时以最后一个为准。缺失节点时返回零初始化结果并打印错误。
AeroCondXml FileIO::readAeroXML(TiXmlNode *node, Mode mode) {

    AeroCondXml aeroCondXml{};
    if (node == nullptr || node->ToElement() == nullptr) {
        std::cerr << "Error: <Attribute> element missing, cannot read <Aerodynamics>." << std::endl;
        return aeroCondXml;
    }
    TiXmlElement *AeroNode = node->FirstChildElement("Aerodynamics");
    if (AeroNode == nullptr) {
        std::cerr << "Error: <Aerodynamics> element missing, cannot read aerodynamic parameters." << std::endl;
        return aeroCondXml;
    }
//    aeroCondXml = {
//            AeroType::ONE,
//            {
//
//            }
//    };
//    aeroCondXml = {AeroType::ONE,
//                   {1, 10, 10, 3, 12.}, "", 1000};
    // 辅助 lambda：取子元素文本，父节点或子节点缺失时返回 nullptr。
    const auto childText = [](TiXmlElement *parent, const char *name) -> const char * {
        TiXmlElement *child = parent ? parent->FirstChildElement(name) : nullptr;
        return child ? child->GetText() : nullptr;
    };
    for (TiXmlElement *AeroNode0 = AeroNode->FirstChildElement(); AeroNode0 != NULL; AeroNode0 = AeroNode0->NextSiblingElement()) {

        const char *typeText = childText(AeroNode0, "type");
        const char *lText = childText(AeroNode0, "L");
        const char *ustarText = childText(AeroNode0, "ustar");
        const char *hcVegText = childText(AeroNode0, "hc_veg");
        const char *hcBuildText = childText(AeroNode0, "hc_build");
        const char *laiText = childText(AeroNode0, "lai");
        const char *leafWidthText = childText(AeroNode0, "leaf_width");
        const char *stepText = childText(AeroNode0, "stepsizeatmos");
        if (typeText == nullptr || lText == nullptr || ustarText == nullptr || hcVegText == nullptr
            || hcBuildText == nullptr || laiText == nullptr || leafWidthText == nullptr || stepText == nullptr) {
            std::cerr << "Error: incomplete <Aerodynamics> entry (requires type/L/ustar/hc_veg/hc_build/lai/leaf_width/stepsizeatmos)." << std::endl;
            continue;
        }
        try {
            // 步骤：把文本转为数值并整体赋给 aeroCondXml；转换失败只打印错误，保留上一次的值。
            aeroCondXml = {AeroType::ONE,
                           {stoi(typeText),
                            stof(lText),
                            stof(ustarText),
                            stof(hcVegText),
                            stof(hcBuildText),
                            stof(laiText),
                            stof(leafWidthText)},
                           "1",
                           stof(stepText)
            };
        } catch (const std::exception &e) {
            std::cerr << "Error: invalid number in <Aerodynamics>: " << e.what() << std::endl;
        }

    }
    return aeroCondXml;
}

// 模块：解析 <Meteorology> 节点（VoxelEB 气象强迫的元数据）。
// 输入：必填子元素 startTimeNode/endTimeNode（气象表的起止行号）、z（观测高度）、
// Tsold（初始土壤温度）、SatWater（饱和含水量）、dTime（时间步长，s）、filePath（气象文件）。
// 输出：MeteoXml，同时缓存到成员 m_meteoXml 供 readMeteo 使用；任一必填项缺失则返回未完整填充的结果。
MeteoXml FileIO::readMeteoXML(TiXmlNode *node, Mode mode) {


    MeteoXml meteoxml{};
    MeteoMeta meta{};
    if (node == nullptr || node->ToElement() == nullptr) {
        std::cerr << "Error: <Meteorology> element missing, cannot read meteorological settings." << std::endl;
        return meteoxml;
    }
    const auto readRequiredText = [node](const char *name) -> const char * {
        TiXmlElement *child = node->ToElement()->FirstChildElement(name);
        const char *text = child ? child->GetText() : nullptr;
        if (text == nullptr) {
            throw std::runtime_error(std::string("<Meteorology> is missing required element <") + name + ">");
        }
        return text;
    };
    try {
        meteoxml.startTimeNode = stoi(readRequiredText("startTimeNode"));
        meteoxml.endTimeNode = stoi(readRequiredText("endTimeNode"));
        meta.z = stof(readRequiredText("z"));
        meta.Tsold = stof(readRequiredText("Tsold"));
        meta.SatWater = stof(readRequiredText("SatWater"));
        meta.dTime = stof(readRequiredText("dTime"));
        meteoxml.meteofile = readRequiredText("filePath");
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return meteoxml;
    }
    meteoxml.meta = meta;

    m_meteoXml = meteoxml;
    return meteoxml;

}

// 模块：读取 <Light> 下首个条目的 Esky/Esun 文件名（旧版大气条件入口）。
// 说明：readXml 中对它的调用已被注释，当前基本不使用；该函数不检查空指针（疑似遗留代码）。
AtomCondXml FileIO::readAtomCondXML(TiXmlNode *node, Mode mode) {

    AtomCondXml atomCondXml;
    TiXmlNode* lightNode = node->FirstChildElement("Light");
    for (TiXmlElement* pEle = lightNode->FirstChildElement(); pEle != NULL; pEle = pEle->NextSiblingElement())
    {
        atomCondXml.rlifile = pEle->FirstChildElement("eskyFileName")->GetText();
        atomCondXml.rinfile = pEle->FirstChildElement("esunFileName")->GetText();
        break;

    }


    return atomCondXml;
}

// 模块：解析 <Attribute>/<Biochemistry>（叶片/土壤等生化与热学参数，VoxelEB/VoxelRT）。
// 输入：<Attribute> 节点。输出：每个子条目一个 PropertyXml（名称、类型、LeafBio/土壤/水体/建筑参数）。
// 说明：每个子条目对应一个命名属性，leafBio 子节点设置 Type::VEGETATION。
std::vector<PropertyXml> FileIO::readPropertyXML(TiXmlNode *node, Mode mode) {

    std::vector<PropertyXml> propxmls;
    if (node == nullptr || node->ToElement() == nullptr) {
        std::cerr << "Error: <Attribute> element missing, cannot read <Biochemistry>." << std::endl;
        return propxmls;
    }
    TiXmlElement* chemstry_node = node->FirstChildElement("Biochemistry");
    if (chemstry_node == nullptr) {
        std::cerr << "Error: <Biochemistry> element missing, cannot read material properties." << std::endl;
        return propxmls;
    }
    for (TiXmlElement* Node = chemstry_node->FirstChildElement(); Node != NULL; Node = Node->NextSiblingElement()){
        PropertyXml propertyXml;
        if (sonExists("leafBio", Node)){
            TiXmlElement* leafNode = Node->FirstChildElement("leafBio");
            const char* leafNameText = leafNode ? leafNode->Attribute("name") : nullptr;
            if (leafNameText != nullptr) {
                propertyXml.name = leafNameText;
            } else {
                std::cerr << "Error: <leafBio> is missing 'name' attribute." << std::endl;
            }
//            propertyXml.name = "tree";
            if (propertyXml.name == "leafBio"){
                propertyXml.name = "leafbio";
            }
            propertyXml.type = Type::VEGETATION;
            // 步骤 1（leafBio）：读取叶片温度响应参数 Tparam（逗号分隔，需要 5 个值），
            // 不足 5 个时补 0 并报错，保证后面按下标取 5 个值时不越界。
            std::vector<float> tparam;
            if (TiXmlElement* tparamEle = leafNode->FirstChildElement("Tparam")) {
                if (tparamEle->GetText() != nullptr) {
                    tparam = myFunction::mySplitFloat(tparamEle->GetText(), ",");
                }
            }
            if (tparam.size() < 5) {
                std::cerr << "Error: <leafBio><Tparam> requires 5 comma-separated values; missing entries default to 0." << std::endl;
                tparam.resize(5, 0.0f);
            }
            // 步骤 2（leafBio）：按 LeafBio 结构体的字段顺序依次构造：Vcmax、m、BallBerry、Type、kV、
            // Rdparam、Tparam[5]、Tyear、beta、kNPQs、qLs、stressfactor、Tcor。必填项缺失时会解引用空指针。
            propertyXml.leafbio = LeafBio{
                    stof(leafNode->FirstChildElement("Vcmax")->GetText()),
                    stof(leafNode->FirstChildElement("m")->GetText()),
                    stof(leafNode->FirstChildElement("BallBerry")->GetText()),
                    stof(leafNode->FirstChildElement("Type")->GetText()),
                    stof(leafNode->FirstChildElement("kV")->GetText()),
                    stof(leafNode->FirstChildElement("Rdparam")->GetText()),
                    {tparam[0], tparam[1], tparam[2], tparam[3], tparam[4]},
                    stof(leafNode->FirstChildElement("Tyear")->GetText()),
                    stof(leafNode->FirstChildElement("beta")->GetText()),
                    stof(leafNode->FirstChildElement("kNPQs")->GetText()),
                    stof(leafNode->FirstChildElement("qLs")->GetText()),
                    stof(leafNode->FirstChildElement("stressfactor")->GetText()),
                    (stoi(leafNode->FirstChildElement("Tcor")->GetText()))
            };
            propxmls.push_back(propertyXml);
//            continue;
        }
        // 分支：土壤/地表热参数 <soilSet>（生成 Type::SOIL 的 PropertyXml）。
        // 名称 "soil" 会被改写为 "soilset"，与场景中引用的背景/材质名称约定一致（待核对）。
        if (sonExists("soilSet", Node)){
            PropertyXml propertyXml2;
            TiXmlElement* soilNode = Node->FirstChildElement("soilSet");
            const char* soilNameText = soilNode ? soilNode->Attribute("name") : nullptr;
            if (soilNameText != nullptr) {
                propertyXml2.name = soilNameText;
            } else {
                std::cerr << "Error: <soilSet> is missing 'name' attribute." << std::endl;
            }
//            propertyXml2.name = "soilset";
            if (propertyXml2.name == "soil"){
                propertyXml2.name = "soilset";
            }
            propertyXml2.type = Type::SOIL;
            // 步骤 1（soilSet）：必填项依次为 method（导热方法）、rss（蒸发阻抗）、cs（体积热容）、
            // rhos（密度）、lambdas（导热率）、Tsoil（平均温度）、SMC（体积含水量）、Satwater（饱和含水量）。
            propertyXml2.soilset = SoilSet{
                    stoi(soilNode->FirstChildElement("method")->GetText()),
                    stof(soilNode->FirstChildElement("rss")->GetText()),
                    stof(soilNode->FirstChildElement("cs")->GetText()),
                    stof(soilNode->FirstChildElement("rhos")->GetText()),
                    stof(soilNode->FirstChildElement("lambdas")->GetText()),
                    stof(soilNode->FirstChildElement("Tsoil")->GetText()),
                    stof(soilNode->FirstChildElement("SMC")->GetText()),
                    stof(soilNode->FirstChildElement("Satwater")->GetText())
//                    0.45
            };
            // 步骤 2（soilSet）：可选项。brdfModel 取 Hapke/hapke/1 时为 1（Hapke 双向反射），否则 0（Lambert）；
            // hapkeB0/hapkeH/hapkeG 为 Hapke 模型的后向增强幅度、角宽度与不对称参数。
            if (TiXmlElement* model = soilNode->FirstChildElement("brdfModel")) {
                const char* modelText = model->GetText();
                const std::string value = modelText != nullptr ? modelText : std::string();
                propertyXml2.soilset.brdfModel =
                    (value == "Hapke" || value == "hapke" || value == "1") ? 1 : 0;
            }
            if (TiXmlElement* value = soilNode->FirstChildElement("hapkeB0"))
                if (value->GetText() != nullptr) propertyXml2.soilset.hapkeB0 = stof(value->GetText());
            if (TiXmlElement* value = soilNode->FirstChildElement("hapkeH"))
                if (value->GetText() != nullptr) propertyXml2.soilset.hapkeH = stof(value->GetText());
            if (TiXmlElement* value = soilNode->FirstChildElement("hapkeG"))
                if (value->GetText() != nullptr) propertyXml2.soilset.hapkeG = stof(value->GetText());
            // Method-1 history beyond TLASTNUM nodes: <inertiaHistory>full</inertiaHistory>
            // (or 1); absent keeps the legacy 8-node sum.
            // 可选：inertiaHistory 为 full 或 1 时，方法 1 的土壤热惯性计入更早的历史（指数模态求和）；
            // 缺省 0 时只保留最近 TLASTNUM 个节点，与旧工程结果一致。
            if (TiXmlElement* value = soilNode->FirstChildElement("inertiaHistory")) {
                const char* historyText = value->GetText();
                const std::string history = historyText != nullptr ? historyText : std::string();
                propertyXml2.soilset.inertiaHistory = (history == "full" || history == "1") ? 1 : 0;
            }
            propxmls.push_back(propertyXml2);
////            continue;
//
        }
        // 分支：水体参数 <waterSet>（生成 Type::WATER 的 PropertyXml）。
        if (sonExists("waterSet", Node)){
            PropertyXml waterProperty{};
            TiXmlElement* waterNode = Node->FirstChildElement("waterSet");
            const char* waterNameText = waterNode ? waterNode->Attribute("name") : nullptr;
            if (waterNameText != nullptr) {
                waterProperty.name = waterNameText;
            } else {
                std::cerr << "Error: <waterSet> is missing 'name' attribute." << std::endl;
            }
            waterProperty.type = Type::WATER;
            // 必填项：rss（表面阻抗，通常 0 s m-1）、heatCapacity（体积热容 J m-3 K-1）、
            // mixingDepth（有效混合层深度 m）、evaporationCoefficient（潜热通量乘子 0..1）。
            waterProperty.waterset = WaterSet{
                stof(waterNode->FirstChildElement("rss")->GetText()),
                stof(waterNode->FirstChildElement("heatCapacity")->GetText()),
                stof(waterNode->FirstChildElement("mixingDepth")->GetText()),
                stof(waterNode->FirstChildElement("evaporationCoefficient")->GetText())
            };
            // 可选项：brdfModel 为 CoxMunk/coxMunk/cox-munk/1 时用 Fresnel-Cox-Munk 反射（1），否则 Lambert（0）；
            // refractiveIndex 折射率、slopeVariance 波面斜率方差、diffuseFraction 漫反射份额。
            if (TiXmlElement* model = waterNode->FirstChildElement("brdfModel")) {
                const char* modelText = model->GetText();
                const std::string value = modelText != nullptr ? modelText : std::string();
                waterProperty.waterset.brdfModel =
                    (value == "CoxMunk" || value == "coxMunk"
                     || value == "cox-munk" || value == "1") ? 1 : 0;
            }
            if (TiXmlElement* value = waterNode->FirstChildElement("refractiveIndex"))
                if (value->GetText() != nullptr) waterProperty.waterset.refractiveIndex = stof(value->GetText());
            if (TiXmlElement* value = waterNode->FirstChildElement("slopeVariance"))
                if (value->GetText() != nullptr) waterProperty.waterset.slopeVariance = stof(value->GetText());
            if (TiXmlElement* value = waterNode->FirstChildElement("diffuseFraction"))
                if (value->GetText() != nullptr) waterProperty.waterset.diffuseFraction = stof(value->GetText());
            propxmls.push_back(waterProperty);
        }
    }
    return propxmls;

}

// 工具：判断 parentEle 的直接子元素中是否存在名为 sonName 的元素（不递归）。
// parentEle 为空返回 false；返回值用 1/0 表示布尔。
bool FileIO::sonExists(std::string sonName, TiXmlElement* parentEle)
{
    if (parentEle == nullptr) return false;
    //根据节点名称进行查询
    for (TiXmlElement* pEle = parentEle->FirstChildElement(); pEle; pEle = pEle->NextSiblingElement())///循环下面所有节点
    {
        //recursive find sub node return node pointer
        if (!strcmp(sonName.c_str(), pEle->Value()))
        {
            return 1;
        }
    }
    return 0;
}

// 模块：解析 <Attribute>/<Spectral>/<spectral>（光谱参数，反射率与透射率）。
// 输入：<Attribute> 节点。输出：每个 <spectral name=...> 一个 SpectralXml，缺 name 的条目跳过。
// type 属性：custom/file（直接给出 reflectance/transmittance 曲线或外部光谱文件）、
// Prospect（叶片 Cab/Cw/Cdm/Cs/N 参数）、BSM（土壤 SMC/亮度/lat/lon）；缺省按 custom。
// 另可读热红外波段的 tau_TIR、ref_TIR、ref_TIR_wet（单值）。
std::vector<SpectralXml> FileIO::readSpectralXML(TiXmlNode *node, Mode mode) {

    std::vector<SpectralXml> spectralxmls;
    if (node == nullptr || node->FirstChildElement("Spectral") == nullptr) return spectralxmls;
    TiXmlElement *spectralNode = node->FirstChildElement("Spectral");

    for (TiXmlElement *Node = spectralNode->FirstChildElement("spectral");
         Node != NULL; Node = Node->NextSiblingElement("spectral")) {
        SpectralXml spectralXml{};
        const char *nameAttribute = Node->Attribute("name");

        if (!nameAttribute) {
            std::cerr << "Error: <spectral> element is missing 'name' attribute." << std::endl;
            continue;
        }

        spectralXml.spectralName = nameAttribute;

        // 辅助 lambda：把子元素文本按逗号拆成 float 序列；元素缺失时返回空序列。
        const auto parseCurve = [Node](const char *elementName) {
            TiXmlElement *element = Node->FirstChildElement(elementName);
            const char *text = element ? element->GetText() : nullptr;
            if (text == nullptr) return std::vector<float>();
            return myFunction::mySplitFloat(text, ",");
        };

        const std::string type = Node->Attribute("type") ? Node->Attribute("type") : "custom";
        if (type == "custom" || type == "file") {
            // custom/file 类型：曲线与可选外部光谱文件；相对路径按工程文件所在目录解析并规范化。
            TiXmlElement *spectralFileElement = Node->FirstChildElement("spectral_file");
            const char *spectralFile = spectralFileElement ? spectralFileElement->GetText() : nullptr;
            spectralXml.type = type == "file" ? spectralType::OTHER : spectralType::CUSTOM;
            const std::vector<float> reflectanceValues = parseCurve("reflectance");
            const std::vector<float> transmittanceValues = parseCurve("transmittance");
            if (reflectanceValues.empty() || transmittanceValues.empty()) {
                std::cerr << "Error: <spectral> '" << spectralXml.spectralName
                          << "' is missing <reflectance>/<transmittance>." << std::endl;
            }
            if (!reflectanceValues.empty()) spectralXml.reflectances = {reflectanceValues};
            if (!transmittanceValues.empty()) spectralXml.transmittance = {transmittanceValues};

            if (spectralFile && spectralFile[0] != '\0') {
                std::filesystem::path path(spectralFile);
                if (path.is_relative()) path = std::filesystem::path(m_inputDirectory) / path;
                spectralXml.path = path.lexically_normal().string();
            }
        // Prospect 类型：反射/透射曲线可作为红外波段取值（只取一个值），可见-近红外由 PROSPECT 参数计算。
        } else if (type == "Prospect") {
            spectralXml.type = spectralType::PROSPECT;
            spectralXml.reflectances = {parseCurve("reflectance")};
            spectralXml.transmittance = {parseCurve("transmittance")};
//红外波段只取一个值


            spectralXml.fp = {
                    stof(Node->FirstChildElement("Cab")->GetText()),
                    stof(Node->FirstChildElement("Cw")->GetText()),
                    stof(Node->FirstChildElement("Cdm")->GetText()),
                    stof(Node->FirstChildElement("Cs")->GetText()),
                    stof(Node->FirstChildElement("N")->GetText())
            };
        }
        else if (type == "BSM") {
            spectralXml.type = spectralType::BSM;
            spectralXml.reflectances = {parseCurve("reflectance")};
            spectralXml.transmittance = {parseCurve("transmittance")};
//红外波段只取一个值

            // BSM 土壤光谱模型参数：缺省值 SMC=25、BSMBrightness=0.5、BSMlat=25、BSMlon=45（BSM 的亮度与光谱形状参数）。
            const auto bsmValue = [Node](const char* name, float fallback) {
                TiXmlElement* value = Node->FirstChildElement(name);
                return value && value->GetText() ? stof(value->GetText()) : fallback;
            };
            spectralXml.bsm = {
                    bsmValue("SMC", 25.0f),
                    bsmValue("BSMBrightness", 0.5f),
                    bsmValue("BSMlat", 25.0f),
                    bsmValue("BSMlon", 45.0f)
            };
        }
        // 热红外波段的单值透射率/反射率（干、湿）；各类型通用，缺省时保持结构体默认值。
        if (TiXmlElement *value = Node->FirstChildElement("tau_TIR"); value && value->GetText())
            spectralXml.tau_tir = stof(value->GetText());
        if (TiXmlElement *value = Node->FirstChildElement("ref_TIR"); value && value->GetText())
            spectralXml.refl_tir = stof(value->GetText());
        if (TiXmlElement *value = Node->FirstChildElement("ref_TIR_wet"); value && value->GetText())
            spectralXml.refl_tir_wet = stof(value->GetText());
        spectralxmls.push_back(spectralXml);
    }

    return spectralxmls;
}

// 模块：解析 <Attribute>/<Thermal>/<thermal>（各模式共用的表面温度）。
// 输出：每个条目的名称与 sunlitTemperature / shadedTemperature（光照面/阴影面温度，单位待核对，疑为 K）。
// 节点缺失时返回空数组；数值非法只打印错误。
std::vector<ThermalXml> FileIO::readThermalXML(TiXmlNode *node, Mode mode) {
    std::vector<ThermalXml> thermalXmls;
    if (node == nullptr) return thermalXmls;
    TiXmlElement* thermalNode = node->FirstChildElement("Thermal");
    if (thermalNode == nullptr) return thermalXmls;
    for (TiXmlElement* Node = thermalNode->FirstChildElement("thermal"); Node != NULL; Node = Node->NextSiblingElement("thermal")){
        ThermalXml thermalxml{};
        const char* thermalNameText = Node->Attribute("name");
        if (thermalNameText != nullptr) {
            thermalxml.thermalName = thermalNameText;
        } else {
            std::cerr << "Error: <thermal> is missing 'name' attribute." << std::endl;
        }
        TiXmlElement* sunlitEle = Node->FirstChildElement("sunlitTemperature");
        TiXmlElement* shadedEle = Node->FirstChildElement("shadedTemperature");
        try {
            if (sunlitEle != nullptr && sunlitEle->GetText() != nullptr)
                thermalxml.sunlitTemperature = stof(sunlitEle->GetText());
            if (shadedEle != nullptr && shadedEle->GetText() != nullptr)
                thermalxml.shadedTemperature = stof(shadedEle->GetText());
        } catch (const std::exception& e) {
            std::cerr << "Error: invalid temperature in <thermal>: " << e.what() << std::endl;
        }
        thermalXmls.push_back(thermalxml);
    }
    return thermalXmls;
}

// 模块：解析 <Attribute>/<Canopy>/<canopy>（冠层结构参数）。
// 输入：<Attribute> 节点。输出：每个 <canopy name=...> 一个 CanopyXml，其 canopy 字段按 Canopy 结构体顺序填充。
// 必填：lai、density、hc（高度）、G、LIDFa、LIDFb、hspot（热点）、leafwidth；
// 可选：structureType、extinction、scatteringAlbedo、asymmetry、emissionScale、fixedTemperature。
std::vector<CanopyXml> FileIO::readCanopyXML(TiXmlNode *node, Mode mode) {

    std::vector<CanopyXml> CanopyXmls;
    if (node == nullptr || node->ToElement() == nullptr) {
        std::cerr << "Error: <Attribute> element missing, cannot read <Canopy>." << std::endl;
        return CanopyXmls;
    }
    TiXmlElement* canopyNode = node->FirstChildElement("Canopy");
    if (canopyNode == nullptr) {
        std::cerr << "Error: <Canopy> element missing, cannot read canopy parameters." << std::endl;
        return CanopyXmls;
    }
    for (TiXmlElement* Node = canopyNode->FirstChildElement("canopy"); Node != NULL; Node = Node->NextSiblingElement("canopy")){
        CanopyXml canopyXml;
        const char* canopyNameText = Node->Attribute("name");
        if (canopyNameText != nullptr) {
            canopyXml.canopyName = canopyNameText;
        } else {
            std::cerr << "Error: <canopy> is missing 'name' attribute." << std::endl;
        }
        const TiXmlElement* structureNode = Node->FirstChildElement("structureType");
        const std::string structureType = structureNode != nullptr && structureNode->GetText() != nullptr
            ? structureNode->GetText() : "canopy";
        // 辅助 lambda：读取可选浮点子元素，缺失时用 fallback（兼容旧工程）。
        const auto optionalFloat = [Node](const char* name, float fallback) {
            const TiXmlElement* valueNode = Node->FirstChildElement(name);
            return valueNode != nullptr && valueNode->GetText() != nullptr
                ? std::stof(valueNode->GetText()) : fallback;
        };
        // DEFERRED[低] 错误处理: 未识别的 structureType（如拼写为 "Fire"）静默落为 0（普通冠层）而不告警；lai/density/hc/leafwidth 未校验 >0（leafwidth=0 时空气动力学阻抗除零）；建议：未知类型报错，数值做范围检查。
        // structureType 映射：canopy/缺省=0（普通叶冠层）、rigid=1（刚性体）、fire=2（火/烟）、fog=3（雾）；
        // 名称与数字都接受，未识别的值落为 0。
        const int structureTypeId = structureType == "rigid" || structureType == "1" ? 1
            : structureType == "fire" || structureType == "2" ? 2
            : structureType == "fog" || structureType == "3" ? 3 : 0;
        // 按 Canopy 结构体字段顺序构造；第 4 个字段 width 固定为 1（XML 不提供）。
        // 介质参数做范围限制：extinction/emissionScale/fixedTemperature >= 0，scatteringAlbedo 限幅到 [0,1]，
        // asymmetry 限幅到 [-0.99, 0.99]（避免 Henyey-Greenstein 相函数奇异）。
        canopyXml.canopy = {
                stof(Node ->FirstChildElement("lai")->GetText()),
                stof(Node ->FirstChildElement("density")->GetText()),
                stof(Node ->FirstChildElement("hc")->GetText()),
                1,
                stof(Node ->FirstChildElement("G")->GetText()),
                stof(Node ->FirstChildElement("LIDFa")->GetText()),
                stof(Node ->FirstChildElement("LIDFb")->GetText()),
                stof(Node ->FirstChildElement("hspot")->GetText()),
                stof(Node ->FirstChildElement("leafwidth")->GetText()),
                structureTypeId,
                std::max(0.0f, optionalFloat("extinction", 0.0f)),
                std::clamp(optionalFloat("scatteringAlbedo", 0.0f), 0.0f, 1.0f),
                std::clamp(optionalFloat("asymmetry", 0.0f), -0.99f, 0.99f),
                std::max(0.0f, optionalFloat("emissionScale", 0.0f)),
                std::max(0.0f, optionalFloat("fixedTemperature", 0.0f))
        };
        CanopyXmls.push_back(canopyXml);
    }
//    CanopyXml canopyXml = {"crown", {1, 1, 1, 1, 0.5, -0.35, -0.15, 0.2, 0.2}};
//    CanopyXmls ={canopyXml};

    return CanopyXmls;
}


// 模块：解析 <Geometry>/<Sensor>（传感器位置、投影、视场、分辨率、观测波段与观测角）。
// 输入：<Geometry> 节点。输出：SensorXml；缺省投影为 PARALLAL（平行投影），位置 (0,0,3000) m。
// 只处理 type 缺省或为 "Multispectral sensor" 的条目。
SensorXml FileIO::readSensorXML(TiXmlNode *node, Mode mode){

    SensorXml sensorxml;
    sensorxml.projection = Projection::PARALLAL;
    sensorxml.position = {0.0f, 0.0f, 3000.0f};
    if (node == nullptr || node->ToElement() == nullptr) {
        std::cerr << "Error: <Geometry> element missing, cannot read <Sensor>." << std::endl;
        return sensorxml;
    }
    TiXmlElement* sensorEle = node->FirstChildElement("Sensor");
    if (sensorEle == nullptr) {
        std::cerr << "Error: <Sensor> element missing under <Geometry>." << std::endl;
        return sensorxml;
    }
    const auto elementText = [](TiXmlElement *parent, const char *name) -> const char * {
        TiXmlElement *child = parent ? parent->FirstChildElement(name) : nullptr;
        return child ? child->GetText() : nullptr;
    };
    // 辅助 lambda：读取整型子元素，缺失或转换失败时返回 fallback。
    const auto readInt = [&elementText](TiXmlElement *parent, const char *name, int fallback) {
        const char *text = elementText(parent, name);
        if (text == nullptr) return fallback;
        try { return std::stoi(text); } catch (const std::exception &) { return fallback; }
    };

//赋值
    // 步骤 1：遍历 <Sensor> 下的每个传感器条目，按 type 属性分为多光谱（缺省）与高光谱两类；
    // 同名字段多次出现时后面的条目覆盖前面的（sensorxml 只有一份）。
    for (TiXmlElement* pEle = sensorEle->FirstChildElement(); pEle != NULL; pEle = pEle->NextSiblingElement())
    {
        // const char* typeAttr = pEle->Attribute("type");
        std::string sensorType;
        if (pEle->Attribute("type") == NULL)
        {
            sensorType = "Multispectral sensor";
        }
        else
        {
            sensorType = pEle->Attribute("type");
        }

        // 多光谱传感器：读投影、位置、视场角、像元分辨率、波段（controlBand）与观测角集合。
        if (sensorType.empty() || sensorType == "Multispectral sensor")
        {
            const char* sensorNameText = pEle->Attribute("name");
            if (sensorNameText != nullptr) {
                sensorxml.name = sensorNameText;
            } else {
                std::cerr << "Error: <Sensor> is missing 'name' attribute." << std::endl;
            }
            // 投影类型：perspective/central/center 为透视（中心）投影，其余保持缺省的平行投影。
            if (sonExists("projection", pEle))
            {
                const char* projectionText = pEle->FirstChildElement("projection")->GetText();
                std::string projection = projectionText != nullptr ? projectionText : std::string();
                std::transform(projection.begin(), projection.end(), projection.begin(),
                               [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
                if (projection == "perspective" || projection == "central" || projection == "center")
                {
                    sensorxml.projection = Projection::PERSPECTIVE;
                }
            }
            // 传感器位置 x,y,z（m）；高度 z 下限取 0.01 m，避免传感器落在地面上。
            if (sonExists("sensorPosition", pEle))
            {
                const std::vector<float> position = myFunction::mySplitFloat(
                        pEle->FirstChildElement("sensorPosition")->GetText(), ",");
                if (position.size() >= 3)
                {
                    sensorxml.position = {position[0], position[1], std::max(0.01f, position[2])};
                }
            }
            // 视场角 FOV（度），限幅到 [0.1, 120]，用于透视投影。
            if (sonExists("FOV", pEle))
            {
                sensorxml.sensorFov = std::clamp(std::stof(pEle->FirstChildElement("FOV")->GetText()), 0.1f, 120.0f);
            }
            // 无人机飞行模式：多个 <position>x,y,z</position> 逐个读入 uavPoses，高度下限 0.01 m。
            if (TiXmlElement* positions = pEle->FirstChildElement("uavPositions"))
            {
                for (TiXmlElement* position = positions->FirstChildElement("position");
                     position != nullptr; position = position->NextSiblingElement("position"))
                {
                    if (position->GetText() == nullptr) continue;
                    const std::vector<float> values = myFunction::mySplitFloat(position->GetText(), ",");
                    if (values.size() >= 3)
                        sensorxml.uavPoses.emplace_back(values[0], values[1], std::max(0.01f, values[2]));
                }
            }
            // 像元分辨率 pixelResolutionX/Y 为必填项；缺失或非法时只打印错误，resolution 保持原值。
            TiXmlElement* resolutionXEle = pEle->FirstChildElement("pixelResolutionX");
            TiXmlElement* resolutionYEle = pEle->FirstChildElement("pixelResolutionY");
            if (resolutionXEle != nullptr && resolutionXEle->GetText() != nullptr
                && resolutionYEle != nullptr && resolutionYEle->GetText() != nullptr) {
                try {
                    sensorxml.resolution = {std::stof(resolutionXEle->GetText()),
                                            std::stof(resolutionYEle->GetText())};
                } catch (const std::exception& e) {
                    std::cerr << "Error: invalid pixelResolutionX/Y: " << e.what() << std::endl;
                }
            } else {
                std::cerr << "Error: <pixelResolutionX>/<pixelResolutionY> is required for a sensor." << std::endl;
            }
    //        temp.pixelResolutionX = std::stof(pEle->FirstChildElement("pixelResolutionX")->GetText());
    //        temp.pixelResolutionY = std::stof(pEle->FirstChildElement("pixelResolutionY")->GetText());

            // 多光谱波段：controlBand 为逗号分隔的波长列表（单位待核对）。
            const char *controlBandText = elementText(pEle, "controlBand");
            if (controlBandText != nullptr) {
                sensorxml.waves = myFunction::mySplitFloat(controlBandText, ",");
            } else {
                std::cerr << "Error: <controlBand> is missing or empty for a sensor; spectral bands stay empty." << std::endl;
            }


            // 观测角集合 <viewAngle>：每个子元素按 type 生成一组 (VZA, VAA) 写入 sensorxml.viewAngles（度）。
            // type=custom 逐条给定；BRF 生成主平面/垂直主平面角度；albedo 生成半球等面积采样角度。
            TiXmlElement* allViewAngleEle = pEle->FirstChildElement("viewAngle");
            for (TiXmlElement* subEle = allViewAngleEle ? allViewAngleEle->FirstChildElement() : nullptr; subEle != NULL; subEle = subEle->NextSiblingElement())
            {
                const char* viewAngleTypeText = subEle->Attribute("type");
                std::string type = viewAngleTypeText != nullptr ? viewAngleTypeText : std::string();
                if (type == "custom")
                {
                    glm::vec2 viewanglesk;
                    for (TiXmlElement* viewAngleIter = subEle->FirstChildElement(); viewAngleIter != NULL; viewAngleIter = viewAngleIter->NextSiblingElement())
                    {
                        const std::vector<float> viewAngleValues = myFunction::mySplitFloat(
                                viewAngleIter->GetText() != nullptr ? viewAngleIter->GetText() : "", ",");
                        if (viewAngleValues.size() < 2)
                        {
                            std::cerr << "Error: <viewAngle> entry needs two comma-separated values (vza,vaa)." << std::endl;
                            continue;
                        }
                        nvmath::vec2f viewAngleTemp = { viewAngleValues[0], viewAngleValues[1] };
                        viewanglesk = {viewAngleTemp[0],
                                       viewAngleTemp[1]};
                        sensorxml.viewAngles.push_back(viewanglesk);
    //                    temp.viewZenith.push_back(viewAngleTemp[0]);
    //                    temp.viewAzimuth.push_back(viewAngleTemp[1]);
                    }
                }
                // BRF 观测角：以太阳方位角 SAA（readLightXML 读入）为基准，VZA 从 0 到 vzaMax 按 vzaStep 取值。
                // SPP=1：太阳主平面，两侧方位角为 SAA 与 SAA±180（折回 [0,360]）；
                // CSPP=1：垂直主平面，方位角为 SAA+90 与 SAA+270（折回 [0,360]）。vzaMax/vzaStep 必须为正。
                else if (type == "BRF")
                {
                    if (readInt(subEle, "SPP", 0) == 1){
                        const int VzaMax = readInt(subEle, "vzaMax", 0);
                        const int vzaStep = readInt(subEle, "vzaStep", 0);
                        if (VzaMax <= 0 || vzaStep <= 0) {
                            std::cerr << "Error: BRF <vzaMax>/<vzaStep> must be positive; skipping this BRF angle set." << std::endl;
                            continue;
                        }
                        for (int k = 0; k<=VzaMax; k += vzaStep){
                            nvmath::vec2f viewAngleTemp = {k, SAA};
                            glm::vec2 viewanglesk = {viewAngleTemp[0],
                                                     viewAngleTemp[1]};
                            sensorxml.viewAngles.push_back(viewanglesk);
                        }
                        for (int k = 0; k<=VzaMax; k += vzaStep){
                            nvmath::vec2f viewAngleTemp;
                            if (SAA > 180){
                                viewAngleTemp = {k, SAA-180};
                            }
                            else{
                                viewAngleTemp = {k, SAA+180};
                            }
                            glm::vec2 viewanglesk = {viewAngleTemp[0],
                                                     viewAngleTemp[1]};
                            sensorxml.viewAngles.push_back(viewanglesk);
                        }

                    }
                    if (readInt(subEle, "CSPP", 0) == 1){
                        const int VzaMax = readInt(subEle, "vzaMax", 0);
                        const int vzaStep = readInt(subEle, "vzaStep", 0);
                        if (VzaMax <= 0 || vzaStep <= 0) {
                            std::cerr << "Error: BRF <vzaMax>/<vzaStep> must be positive; skipping this BRF angle set." << std::endl;
                            continue;
                        }
                        for (int k = 0; k<=VzaMax; k += vzaStep){
                            nvmath::vec2f viewAngleTemp;
                            if (SAA < 270){
                                viewAngleTemp = {k, SAA+90};
                            }
                            else{
                                viewAngleTemp = {k, SAA-270};
                            }

                            glm::vec2 viewanglesk = {viewAngleTemp[0],
                                                     viewAngleTemp[1]};
                            sensorxml.viewAngles.push_back(viewanglesk);
                        }
                        for (int k = 0; k<=VzaMax; k += vzaStep){
                            nvmath::vec2f viewAngleTemp;
                            if (SAA < 90){
                                viewAngleTemp = {k, SAA+270};
                            }
                            else{
                                viewAngleTemp = {k, SAA-90};
                            }

                            glm::vec2 viewanglesk = {viewAngleTemp[0],
                                                     viewAngleTemp[1]};
                            sensorxml.viewAngles.push_back(viewanglesk);
                        }

                    }
                }
                // albedo 观测角：把上半球按近似等面积分成若干环，每个观测方向代表相等的立体角，用于对半球积分求反照率。
                // enabled=1 且 angleNum>0 时生效；angleNum 是最外环（VZA≈90° 附近）的方向数。
                else if (type == "albedo")
                {
                    const int isHemisphere = readInt(subEle, "enabled", 0);
                    const int hemiAngleNum = readInt(subEle, "angleNum", 0);
                    if (isHemisphere == 1 && hemiAngleNum > 0){
                        // 算法（待核对）：theta0 为当前环外缘天顶角，r=2sin(theta/2) 是等面积投影下的半径；
                        // 环宽取 r0·sqrt(pi/k0)，内环方向数 k1 按面积比 (r1/r0)^2 缩小；
                        // 每环取中间天顶角 theta、方位角在 [0,360) 内均分（起点为半个间隔），最多 10 环；
                        // k1<=0 时补一个天顶方向 (0,0) 并结束。
                        float theta0, theta1, theta;
                        float r0, r1;
                        int k0, k1, k00;
                        float pi = 3.1415926, dphi;

                        theta0 = pi / 2.0;
                        k0 = hemiAngleNum;
                        r0 = 2.0 * sin(theta0 / 2.0);

                        for (int k = 0; k < 10; k++) {
                            theta1 = theta0 - 2.0 * sin(theta0 / 2.0) * sqrt(pi / k0);
                            r1 = 2.0 * sin(theta1 / 2.0);
                            k1 = k0 * pow(r1 / r0, 2);

                            theta = (theta0 + theta1) / 2.0 * 180.0 / pi;

                            k00 = k0;
                            dphi = 360.0 / k00;
                            for (int kk = 0; kk < k00; kk++) {
                                // 生成每个 viewAngleTemp 值
                                glm::vec2 viewanglesk = {
                                        theta,                          // 每一个 VZA
                                        dphi / 2.0 + dphi * kk          // 每一个 VAA
                                };

                                // 插入到 sensorxml.viewAngles 中
                                sensorxml.viewAngles.push_back(viewanglesk);
                            }

                            if (k1 <= 0) {
                                // 如果 k1 小于 0，插入 VZA = 0 和 VAA = 0 的角度
                                glm::vec2 viewanglesk = {0.0f, 0.0f};
                                sensorxml.viewAngles.push_back(viewanglesk);
                                break;
                            }

                            theta0 = theta1;
                            r0 = r1;
                            k0 = k1;


                        }
                    }
                }
            }
        }
        // 高光谱传感器：除波段来自 bandsFilePath 文件外，逻辑与多光谱相同（投影固定为平行投影，
        // 不读位置与 FOV）；下面的观测角生成代码与多光谱分支逐行重复。
        else if (sensorType == "hyperspectral sensor")
        {
            const char* sensorNameText = pEle->Attribute("name");
            if (sensorNameText != nullptr) {
                sensorxml.name = sensorNameText;
            } else {
                std::cerr << "Error: <Sensor> is missing 'name' attribute." << std::endl;
            }
            sensorxml.projection = Projection::PARALLAL;
            if (sonExists("FOV", pEle))
            {
    // temp.FOV = std::stoi(pEle->FirstChildElement("FOV")->GetText());
            }
            else
            {
    //            temp.FOV = -1;
            }

            // 同多光谱分支：读取像元分辨率。
            TiXmlElement* resolutionXEle = pEle->FirstChildElement("pixelResolutionX");
            TiXmlElement* resolutionYEle = pEle->FirstChildElement("pixelResolutionY");
            if (resolutionXEle != nullptr && resolutionXEle->GetText() != nullptr
                && resolutionYEle != nullptr && resolutionYEle->GetText() != nullptr) {
                try {
                    sensorxml.resolution = {std::stof(resolutionXEle->GetText()),
                                            std::stof(resolutionYEle->GetText())};
                } catch (const std::exception& e) {
                    std::cerr << "Error: invalid pixelResolutionX/Y: " << e.what() << std::endl;
                }
            } else {
                std::cerr << "Error: <pixelResolutionX>/<pixelResolutionY> is required for a sensor." << std::endl;
            }

            // sensorxml.waves = myFunction::mySplitFloat(pEle->FirstChildElement("controlBand")->GetText(), ",");
            // 步骤：读取波段文件，每行一个波长（空行跳过，非法行告警后忽略）；
            // 相对路径按工程文件所在目录解析。文件缺失或读不到波长时跳过该传感器条目。
            TiXmlElement* bandsFileEle = pEle->FirstChildElement("bandsFilePath");
            const char* bandsFileText = bandsFileEle ? bandsFileEle->GetText() : nullptr;
            if (bandsFileText == nullptr || bandsFileText[0] == '\0') {
                std::cerr << "Error: <bandsFilePath> is missing for the hyperspectral sensor." << std::endl;
                continue;
            }
            std::filesystem::path bandsPath = std::filesystem::u8path(bandsFileText);
            if (bandsPath.is_relative() && !m_inputDirectory.empty()) {
                bandsPath = std::filesystem::path(m_inputDirectory) / bandsPath;
            }
            std::ifstream file(bandsPath);

            if (!file.is_open()) {
                std::cerr << "Can not open file: " << bandsPath.string() << std::endl;
                continue;
            }
            std::string line;
            while (std::getline(file, line)) {
                if (!line.empty()) {
                    try {
                        // 将字符串转换为float并添加到vector中
                        float num = std::stof(line);
                        sensorxml.waves.push_back(num);
                    }
                    catch (const std::invalid_argument& e) {
                        std::cerr << "无效的数字格式: " << line << std::endl;
                    }
                    catch (const std::out_of_range& e) {
                        std::cerr << "数字超出范围: " << line << std::endl;
                    }
                }
            }
            file.close();
            if (sensorxml.waves.empty()) {
                std::cerr << "Error: no wavelength read from " << bandsPath.string() << std::endl;
                continue;
            }




            // 观测角集合（custom / BRF / albedo 三种生成方式，与多光谱分支相同）。
            TiXmlElement* allViewAngleEle = pEle->FirstChildElement("viewAngle");
            for (TiXmlElement* subEle = allViewAngleEle ? allViewAngleEle->FirstChildElement() : nullptr; subEle != NULL; subEle = subEle->NextSiblingElement())
            {
                const char* viewAngleTypeText = subEle->Attribute("type");
                std::string type = viewAngleTypeText != nullptr ? viewAngleTypeText : std::string();
                if (type == "custom")
                {
                    glm::vec2 viewanglesk;
                    for (TiXmlElement* viewAngleIter = subEle->FirstChildElement(); viewAngleIter != NULL; viewAngleIter = viewAngleIter->NextSiblingElement())
                    {
                        const std::vector<float> viewAngleValues = myFunction::mySplitFloat(
                                viewAngleIter->GetText() != nullptr ? viewAngleIter->GetText() : "", ",");
                        if (viewAngleValues.size() < 2)
                        {
                            std::cerr << "Error: <viewAngle> entry needs two comma-separated values (vza,vaa)." << std::endl;
                            continue;
                        }
                        nvmath::vec2f viewAngleTemp = { viewAngleValues[0], viewAngleValues[1] };
                        viewanglesk = {viewAngleTemp[0],
                                       viewAngleTemp[1]};
                        sensorxml.viewAngles.push_back(viewanglesk);
    //                    temp.viewZenith.push_back(viewAngleTemp[0]);
    //                    temp.viewAzimuth.push_back(viewAngleTemp[1]);
                    }
                }
                else if (type == "BRF")
                {
                    if (readInt(subEle, "SPP", 0) == 1){
                        const int VzaMax = readInt(subEle, "vzaMax", 0);
                        const int vzaStep = readInt(subEle, "vzaStep", 0);
                        if (VzaMax <= 0 || vzaStep <= 0) {
                            std::cerr << "Error: BRF <vzaMax>/<vzaStep> must be positive; skipping this BRF angle set." << std::endl;
                            continue;
                        }
                        for (int k = 0; k<=VzaMax; k += vzaStep){
                            nvmath::vec2f viewAngleTemp = {k, SAA};
                            glm::vec2 viewanglesk = {viewAngleTemp[0],
                                                     viewAngleTemp[1]};
                            sensorxml.viewAngles.push_back(viewanglesk);
                        }
                        for (int k = 0; k<=VzaMax; k += vzaStep){
                            nvmath::vec2f viewAngleTemp;
                            if (SAA > 180){
                                viewAngleTemp = {k, SAA-180};
                            }
                            else{
                                viewAngleTemp = {k, SAA+180};
                            }
                            glm::vec2 viewanglesk = {viewAngleTemp[0],
                                                     viewAngleTemp[1]};
                            sensorxml.viewAngles.push_back(viewanglesk);
                        }

                    }
                    if (readInt(subEle, "CSPP", 0) == 1){
                        const int VzaMax = readInt(subEle, "vzaMax", 0);
                        const int vzaStep = readInt(subEle, "vzaStep", 0);
                        if (VzaMax <= 0 || vzaStep <= 0) {
                            std::cerr << "Error: BRF <vzaMax>/<vzaStep> must be positive; skipping this BRF angle set." << std::endl;
                            continue;
                        }
                        for (int k = 0; k<=VzaMax; k += vzaStep){
                            nvmath::vec2f viewAngleTemp;
                            if (SAA < 270){
                                viewAngleTemp = {k, SAA+90};
                            }
                            else{
                                viewAngleTemp = {k, SAA-270};
                            }

                            glm::vec2 viewanglesk = {viewAngleTemp[0],
                                                     viewAngleTemp[1]};
                            sensorxml.viewAngles.push_back(viewanglesk);
                        }
                        for (int k = 0; k<=VzaMax; k += vzaStep){
                            nvmath::vec2f viewAngleTemp;
                            if (SAA < 90){
                                viewAngleTemp = {k, SAA+270};
                            }
                            else{
                                viewAngleTemp = {k, SAA-90};
                            }

                            glm::vec2 viewanglesk = {viewAngleTemp[0],
                                                     viewAngleTemp[1]};
                            sensorxml.viewAngles.push_back(viewanglesk);
                        }

                    }
                }
                else if (type == "albedo")
                {
                    const int isHemisphere = readInt(subEle, "enabled", 0);
                    const int hemiAngleNum = readInt(subEle, "angleNum", 0);
                    if (isHemisphere == 1 && hemiAngleNum > 0){
                        float theta0, theta1, theta;
                        float r0, r1;
                        int k0, k1, k00;
                        float pi = 3.1415926, dphi;

                        theta0 = pi / 2.0;
                        k0 = hemiAngleNum;
                        r0 = 2.0 * sin(theta0 / 2.0);

                        for (int k = 0; k < 10; k++) {
                            theta1 = theta0 - 2.0 * sin(theta0 / 2.0) * sqrt(pi / k0);
                            r1 = 2.0 * sin(theta1 / 2.0);
                            k1 = k0 * pow(r1 / r0, 2);

                            theta = (theta0 + theta1) / 2.0 * 180.0 / pi;

                            k00 = k0;
                            dphi = 360.0 / k00;
                            for (int kk = 0; kk < k00; kk++) {
                                // 生成每个 viewAngleTemp 值
                                glm::vec2 viewanglesk = {
                                        theta,                          // 每一个 VZA
                                        dphi / 2.0 + dphi * kk          // 每一个 VAA
                                };

                                // 插入到 sensorxml.viewAngles 中
                                sensorxml.viewAngles.push_back(viewanglesk);
                            }

                            if (k1 <= 0) {
                                // 如果 k1 小于 0，插入 VZA = 0 和 VAA = 0 的角度
                                glm::vec2 viewanglesk = {0.0f, 0.0f};
                                sensorxml.viewAngles.push_back(viewanglesk);
                                break;
                            }

                            theta0 = theta1;
                            r0 = r1;
                            k0 = k1;


                        }
                    }
                }
            }
        }

    }
// 补充，sensorxml里面的bool isImage{true};
//    bool isAlbedo{false};
//    bool isTemperature{true};
//    bool isDisplay{false};属性------------------------------------------------------------------------------

//    sensorxml.viewAngles = {{0, 0}};

    // 步骤 2：从 <Control> 读取输出开关（isAlbedo/isImage/isProcess/isRadiationProcess/
    // isEnergyProcess/isOrth/isTemperature/isDisplay），整数 0/1；存在即解析，缺省保持结构体默认值。
    TiXmlNode *controlnode = RootElement ? RootElement->FirstChild("Control") : nullptr;
    TiXmlElement *controlElement = controlnode ? controlnode->ToElement() : nullptr;
    if (sonExists("isAlbedo", controlElement))
    {
        sensorxml.isAlbedo = stoi(controlnode->FirstChildElement("isAlbedo")->GetText());
    }
    if (sonExists("isImage", controlElement))
    {
        sensorxml.isImage = stoi(controlnode->FirstChildElement("isImage")->GetText());
    }
    if (sonExists("isProcess", controlElement))
    {
        sensorxml.isProcess = stoi(controlnode->FirstChildElement("isProcess")->GetText());
    }
    if (sonExists("isRadiationProcess", controlElement))
    {
        sensorxml.isRadiationProcess = stoi(controlnode->FirstChildElement("isRadiationProcess")->GetText());
    }
    if (sonExists("isEnergyProcess", controlElement))
    {
        sensorxml.isEnergyProcess = stoi(controlnode->FirstChildElement("isEnergyProcess")->GetText());
    }
    if (sonExists("isOrth", controlElement))
    {
        sensorxml.isOrth = stoi(controlnode->FirstChildElement("isOrth")->GetText());
    }
    if (sonExists("isTemperature", controlElement))
    {
        sensorxml.isTemperature = stoi(controlnode->FirstChildElement("isTemperature")->GetText());
    }

    if (sonExists("isDisplay", controlElement))
    {
        sensorxml.isDisplay = stoi(controlnode->FirstChildElement("isDisplay")->GetText());
    }

    return sensorxml;


}
// 模块：解析 <Geometry>/<Light>（太阳与天空光照条件）。
// 输入：<Geometry> 节点。输出：LightXml；并把太阳方位角记入成员 SAA，供 readSensorXML 生成 BRF 观测角。
LightXml FileIO::readLightXML(TiXmlNode *geometryNode, Mode mode){

    LightXml lightxml;
    AtomCondXml atomCondXml;
    if (geometryNode == nullptr || geometryNode->ToElement() == nullptr) {
        std::cerr << "Error: <Geometry> element missing, cannot read <Light>." << std::endl;
        return lightxml;
    }
    TiXmlElement* lightEle = geometryNode->FirstChildElement("Light");
    if (lightEle == nullptr) {
        std::cerr << "Error: <Light> element missing under <Geometry>." << std::endl;
        return lightxml;
    }
    for (TiXmlElement* pEle = lightEle->FirstChildElement(); pEle != NULL; pEle = pEle->NextSiblingElement())
    {

//        LightStruct temp;

        // 步骤 1：逐个 <Light> 子条目解析（多个条目时后者覆盖前者）；name 属性缺失会得到空指针（未检查）。
        lightxml.name = pEle->Attribute("name");

//        temp.isSun = stoi(pEle->FirstChildElement("isSun")->GetText());
//        glm::vec2 sunAnglesk;
        // 步骤 2：太阳角度 <lightAngle> 下每个子元素为 "天顶角,方位角"（度）；
        // 最后一个子元素生效，方位角同时记入成员 SAA，供 BRF 观测角使用。
        TiXmlElement* lightAngleEle = pEle->FirstChildElement("lightAngle");
        for (TiXmlElement* lightAngleIter = lightAngleEle->FirstChildElement(); lightAngleIter != NULL; lightAngleIter = lightAngleIter->NextSiblingElement())
        {

            nvmath::vec2f lightAngleTemp = { myFunction::mySplitFloat(lightAngleIter->GetText(), ",")[0],
                                             myFunction::mySplitFloat(lightAngleIter->GetText(), ",")[1] };
            lightxml.solarAngle = {lightAngleTemp[0], lightAngleTemp[1]};
            SAA = lightAngleTemp[1];

        }

        // 步骤 3：天空等效温度 skyTemperature（必填，用于热红外天空辐射，单位待核对，疑为 K）；
        // shortwaveIrradiance 为可选的实测短波辐照度（W m-2），限幅到 [0, 2000]，缺省 0 表示不使用。
        lightxml.skyTemperature = stof(pEle->FirstChildElement("skyTemperature")->GetText());
        lightxml.shortwaveIrradiance = 0.0f;
        if (sonExists("shortwaveIrradiance", pEle->ToElement())) {
            const char* text = pEle->FirstChildElement("shortwaveIrradiance")->GetText();
            if (text) lightxml.shortwaveIrradiance = std::clamp(
                static_cast<float>(std::atof(text)), 0.0f, 2000.0f);
        }
        // Missing element = legacy fixed split (old projects stay unchanged).
        // 步骤 4：散射比例模式 diffuseFractionMode（解析见 shortwave_split::parseMode）；
        // 缺省为固定比例，保证旧工程结果不变。
        lightxml.diffuseFractionMode = shortwave_split::kModeFixed;
        if (sonExists("diffuseFractionMode", pEle->ToElement())) {
            const char* text = pEle->FirstChildElement("diffuseFractionMode")->GetText();
            if (text) lightxml.diffuseFractionMode = shortwave_split::parseMode(text);
        }
        // 步骤 5：直射/散射分配。给出 directScatteringRatio 时，direct=k、diffuse=1-k，太阳温度固定为 6000 K；
        // VoxelEB 同时记录 Esun/Esky 文件名（esunFileName/eskyFileName）。
        // 未给出该比例时，VoxelEB 仍要读取 Esun/Esky 文件名，RayTracing 无需额外处理。
        if (sonExists("directScatteringRatio", pEle->ToElement())){
            if (m_mode == Mode::eVoxelEB) {
                m_pVoxelebXml->atomcondxml.rinfile = pEle->FirstChildElement("esunFileName")->GetText();
                m_pVoxelebXml->atomcondxml.rlifile = pEle->FirstChildElement("eskyFileName")->GetText();
            }
            float k;
            k = stof(pEle->FirstChildElement(("directScatteringRatio"))->GetText());
            lightxml.direct = k;
            lightxml.diffuse = 1 - lightxml.direct;
            lightxml.solarTemperature = 6000;
        }
        else{
            if (m_mode == Mode::eRaytracing){
//                break;
            }
            else if (m_mode == Mode::eVoxelEB){
                m_pVoxelebXml->atomcondxml.rinfile = pEle->FirstChildElement("esunFileName")->GetText();
                m_pVoxelebXml->atomcondxml.rlifile = pEle->FirstChildElement("eskyFileName")->GetText();
            }

        }
    }
//-----------------------------------------x+y = 1, x/y = k,y = x=k-kx,x/(1+k) =k,x=k(1+k)
//-----------------------------------------


    return lightxml;
}



// 模块：解析 <Control>（计算控制参数）。
// 输入：<Control> 节点（不判空）；rayTracingDepth 与 GPU 为必填项。
// 输出：SettingXml；同时向当前模式的 Xml 对象写入 definedDir（可执行文件所在目录）与 projectDir（outDir）。
// 可选项均带限幅：sampleCount [1,1024]、periodicNeighborCount [0,20]、
// spectralAccelerationWidth [1,1000]、radiationMaxSteps [1,4096]、vegetationTemperatureMethod [0,1]；
// shortwaveScatteringOrders 必须为 1/2/3，否则抛出异常。
SettingXml FileIO::readSettingXML(TiXmlNode *controlNode, Mode mode){

    SettingXml settingxml;
    settingxml.maxDepth = stoi(controlNode->FirstChildElement("rayTracingDepth")->GetText());
    settingxml.theGPU = stoi(controlNode->FirstChildElement("GPU")->GetText());
    if (sonExists("computeBackend", controlNode->ToElement())) {
        const char* backend = controlNode->FirstChildElement("computeBackend")->GetText();
        if (backend) settingxml.computeBackend = backend;
    }
    // n_sample（采样数）缺省 32；sampleCount 存在时限幅到 [1,1024]。
    settingxml.n_sample = 32;
    if (sonExists("sampleCount", controlNode->ToElement())) {
        settingxml.n_sample = std::clamp(
            stoi(controlNode->FirstChildElement("sampleCount")->GetText()), 1, 1024);
    }
    if (sonExists("periodicNeighborCount", controlNode->ToElement())) {
        settingxml.periodicNeighborCount = std::clamp(
            stoi(controlNode->FirstChildElement("periodicNeighborCount")->GetText()), 0, 20);
    }
    if (sonExists("skyboxEnabled", controlNode->ToElement())) {
        settingxml.skyboxEnabled =
            stoi(controlNode->FirstChildElement("skyboxEnabled")->GetText()) != 0;
    }
    if (sonExists("spectralAccelerationWidth", controlNode->ToElement())) {
        settingxml.spectralAccelerationWidth = std::clamp(
            stoi(controlNode->FirstChildElement("spectralAccelerationWidth")->GetText()), 1, 1000);
    }
    if (sonExists("stabilityCorrection", controlNode->ToElement())) {
        const char* text = controlNode->FirstChildElement("stabilityCorrection")->GetText();
        settingxml.stabilityCorrection = text != nullptr && std::atof(text) > 0.5;
    }
    if (sonExists("vegetationTemperatureMethod", controlNode->ToElement())) {
        settingxml.vegetationTemperatureMethod = std::clamp(
            stoi(controlNode->FirstChildElement("vegetationTemperatureMethod")->GetText()), 0, 1);
    }
    if (sonExists("shortwaveScatteringOrders", controlNode->ToElement())) {
        settingxml.shortwaveScatteringOrders =
            stoi(controlNode->FirstChildElement("shortwaveScatteringOrders")->GetText());
        if (settingxml.shortwaveScatteringOrders < 1 || settingxml.shortwaveScatteringOrders > 3)
            throw std::runtime_error("shortwaveScatteringOrders must be 1, 2 or 3");
    }
    if (sonExists("radiationMaxSteps", controlNode->ToElement())) {
        settingxml.radiationMaxSteps = std::clamp(
            stoi(controlNode->FirstChildElement("radiationMaxSteps")->GetText()), 1, 4096);
    }
    if (sonExists("isUAVtrave", controlNode->ToElement())){
        settingxml.isUAVtrave = stoi(controlNode->FirstChildElement("isUAVtrave")->GetText());
    }



    // 步骤：取得可执行文件所在目录，作为 definedDir（Windows 专用 API）。
    char szFilePath[MAX_PATH + 1] = { 0 };
    GetModuleFileNameA(NULL, szFilePath, MAX_PATH);
    /*
    strrchr:函数功能：查找一个字符c在另一个字符串str中末次出现的位置（也就是从str的右侧开始查找字符c首次出现的位置），
    并返回这个位置的地址。如果未能找到指定字符，那么函数将返回NULL。
    使用这个地址返回从最后一个字符c到str末尾的字符串。
    */
    // DEFERRED[中] 安全: GetModuleFileNameA 在路径超过 MAX_PATH 时截断、按 ANSI 代码页返回（中文安装目录变成 '?'），strrchr 找不到 '\\' 时返回 NULL 被解引用；且该段仅 Windows 可编译；下方 outDir 的 GetText() 未判空，缺 <outDir> 即崩溃；建议：使用 GetModuleFileNameW + std::filesystem::path::parent_path，并对 outDir 判空。
    (strrchr(szFilePath, '\\'))[0] = 0; // 删除文件名，只获得路径字串//
    std::string exe_path = szFilePath;
    // 按模式把 definedDir / projectDir 写入对应的 Xml 对象（输出目录 outDir 必填）。
    if (mode == Mode::eRaytracing)
    {
        m_pRaytracingXml->definedDir = exe_path;
        //controlNode->FirstChildElement("defineDir")->GetText();
        m_pRaytracingXml->projectDir = controlNode->FirstChildElement("outDir")->GetText();
    }
    if (mode == Mode::eVoxelRT)
    {
        m_pVoxelrtXml->definedDir = exe_path;//controlNode->FirstChildElement("defineDir")->GetText();
        m_pVoxelrtXml->projectDir = controlNode->FirstChildElement("outDir")->GetText();
    }
    if (mode == Mode::eVoxelEB)
    {
        m_pVoxelebXml->definedDir = exe_path;//controlNode->FirstChildElement("defineDir")->GetText();
        m_pVoxelebXml->projectDir = controlNode->FirstChildElement("outDir")->GetText();
    }

    return settingxml;
}

// 模块：解析 <Scene>（场景大小、体元尺寸、背景、DEM、地形环境、气象经纬度与场景对象）。
// 输入：<Scene> 节点（不判空）；必填 sceneSizeX、sceneSizeY、Height（m）。
// 输出：SceneXml，包含 background 与 VoxelEB/VoxelRT 的对象列表（objEntities 等）。
// 约定：sceneOrigin 固定为 (0,0,0)；voxelSize 缺省 1 m，水平与垂直体元尺寸取同一值。
SceneXml FileIO::readSceneXML(TiXmlNode *sceneNode, Mode mode) {

    SceneXml sceneXml;
    sceneXml.background.sceneSize = {
            stof(sceneNode->FirstChildElement("sceneSizeX")->GetText()),
            stof(sceneNode->FirstChildElement("sceneSizeY")->GetText()),
            stof(sceneNode->FirstChildElement("Height")->GetText())
    };
//    sceneSizeDatasets.sceneSizeX = stof(sceneNode->FirstChildElement("sceneSizeX")->GetText());
//    sceneSizeDatasets.sceneSizeY = stof(sceneNode->FirstChildElement("sceneSizeY")->GetText());
// ------------------------------------------------------
    sceneXml.background.sceneOrigin = {0, 0, 0};
// ------------------------------------------------------
    // DEFERRED[中] 数值: voxelSize 未校验 >0 且有限，0/负值会在体元化时 sceneSize/stepsize 除零或得到负格数（UB 的 float→int 转换）；sceneSizeX/Y/Height 同样未校验 >0；sceneNode 本身也未判空；建议：解析后校验所有尺寸与步长为正有限值，否则抛异常。
    if (sonExists("voxelSize", sceneNode->ToElement()))
    {
        sceneXml.background.stepsize_surface = stof(sceneNode->FirstChildElement("voxelSize")->GetText());
        sceneXml.background.stepsize_height = stof(sceneNode->FirstChildElement("voxelSize")->GetText());
    }
    else
    {
        sceneXml.background.stepsize_surface = 1;
        sceneXml.background.stepsize_height = 1;
    }

    // voxelFillThreshold：OBJ 体元化时，体元被几何占据的最小比例，限幅到 [0,1]。
    if (sonExists("voxelFillThreshold", sceneNode->ToElement())) {
        sceneXml.background.voxelFillThreshold = std::clamp(
            stof(sceneNode->FirstChildElement("voxelFillThreshold")->GetText()),
            0.0f, 1.0f);
    }

    // 背景材质：bgBioName 缺省为 "soilset"，写作 "soilSet" 时统一为 "soilset"；
    // bgBioType 为 Water/water 时背景类型改为水体（缺省为土壤）。
    TiXmlElement* bgNameEle = sceneNode->FirstChildElement("bgBioName");
    if (bgNameEle != nullptr && bgNameEle->GetText() != nullptr) {
        sceneXml.background.bgPropName = bgNameEle->GetText();
    } else {
        sceneXml.background.bgPropName = "soilset";
    }
    TiXmlElement* bgTypeEle = sceneNode->FirstChildElement("bgBioType");
    if (bgTypeEle != nullptr && bgTypeEle->GetText() != nullptr) {
        const std::string bgType = bgTypeEle->GetText();
        if (bgType == "Water" || bgType == "water") {
            sceneXml.background.type = Type::WATER;
        }
    }


    if (sceneXml.background.bgPropName == "soilSet") {
        sceneXml.background.bgPropName = "soilset";
    }

    if (sonExists("bgSpectral", sceneNode->ToElement())) {
        if (sceneNode->FirstChildElement("bgSpectral")->GetText() != NULL) {
            sceneXml.background.bgSpectralName = sceneNode->FirstChildElement("bgSpectral")->GetText();
        }
    }

//-------------------------------------------------------
    if (sonExists("bgThermal", sceneNode->ToElement())) {
        if(sceneNode->FirstChildElement("bgThermal")->GetText() != NULL){
            sceneXml.background.bgThermalName = sceneNode->FirstChildElement("bgThermal")->GetText();
        }
    }
    // 背景角度效应强度（0..1，作用于背景光学与热学 Hapke 效应）；
    // 若 backgroundHeterogeneity 不是 Hapke，则强度强制置 0。
    if (sonExists("backgroundAngularStrength", sceneNode->ToElement())) {
        const char* value = sceneNode->FirstChildElement("backgroundAngularStrength")->GetText();
        if (value != nullptr) {
            sceneXml.background.angularEffectStrength = std::clamp(stof(value), 0.0f, 1.0f);
        }
    }
    if (sonExists("backgroundHeterogeneity", sceneNode->ToElement())) {
        const char* model = sceneNode->FirstChildElement("backgroundHeterogeneity")->GetText();
        if (model == nullptr || (std::string(model) != "Hapke" && std::string(model) != "hapke")) {
            sceneXml.background.angularEffectStrength = 0.0f;
        }
    }
//-------------------------------------------------------

    // 步骤：<TerrainEnvironment> 属性（空气/土壤环境开关、参考高度、温度直减率、水汽/土壤/CO2 标高等）；
    // 任何属性非法或开关不是 0/1 都会抛异常，且目前只支持 VoxelEB。
    bool demEnabled = true;
    if (const TiXmlElement* env = sceneNode->FirstChildElement("TerrainEnvironment")) {
        auto& e = sceneXml.background.terrainEnvironment;
        int air = 0, soil = 0;
        const auto readInt = [&](const char* key, int& value) {
            if (env->Attribute(key) && env->QueryIntAttribute(key, &value) != TIXML_SUCCESS)
                throw std::runtime_error(std::string("Invalid TerrainEnvironment attribute: ") + key);
            if (value != 0 && value != 1) throw std::runtime_error("TerrainEnvironment switches must be 0 or 1");
        };
        const auto readFloat = [&](const char* key, float& value) {
            if (env->Attribute(key) && env->QueryFloatAttribute(key, &value) != TIXML_SUCCESS)
                throw std::runtime_error(std::string("Invalid TerrainEnvironment attribute: ") + key);
        };
        readInt("airEnabled", air); readInt("soilEnabled", soil);
        e.airEnabled = air != 0; e.soilEnabled = soil != 0;
        readFloat("referenceHeight", e.referenceHeight);
        readFloat("temperatureLapseRate", e.temperatureLapseRate);
        readFloat("vaporScaleHeight", e.vaporScaleHeight);
        readFloat("drainageHeight", e.drainageHeight);
        readFloat("soilDry", e.soilDry); readFloat("soilValley", e.soilValley);
        readFloat("soilScaleHeight", e.soilScaleHeight);
        readFloat("co2ValleyExcess", e.co2ValleyExcess);
        readFloat("co2ScaleHeight", e.co2ScaleHeight);
        e.validate();
        if ((e.airEnabled || e.soilEnabled || e.co2ValleyExcess != 0) && mode != Mode::eVoxelEB)
            throw std::runtime_error("TerrainEnvironment currently requires eVoxelEB");
    }
    // 步骤：<Control>/<isDEM> 为 0 时忽略场景里的 DEM 文件；缺省启用。
    TiXmlElement* controlElement = RootElement
        ? RootElement->FirstChildElement("Control") : nullptr;
    if (controlElement && sonExists("isDEM", controlElement)) {
        const char* enabledText =
            controlElement->FirstChildElement("isDEM")->GetText();
        demEnabled = enabledText && std::atoi(enabledText) != 0;
    }

    // 只有 DEM 开关打开且 <DEM> 文本非空时才记为使用 DEM，并保存 DEM 文件路径。
    sceneXml.background.isDEM = {false};
    if (demEnabled && sonExists("DEM", sceneNode->ToElement()))
    {
        if (sceneNode->FirstChildElement("DEM")->GetText() != NULL){
            sceneXml.background.isDEM = {true};
            sceneXml.background.DEMFile = sceneNode->FirstChildElement("DEM")->GetText();
        }
    }



// 添加一个meteo的经纬度信息-----------------------------------------
    // DEFERRED[低] 物理: Latitude/Longitude/TimeZone 未做范围校验（|lat|<=90、|lon|<=180、|tz|<=14），越界值会让 SPA 计算失败并抛出含糊的异常；且这里假设经纬度单位为十进制度、东经为正，未在配置中注明；建议：解析时校验范围并报出字段名。
    // 步骤：从 <Meteorology> 读取经纬度（度）、年份与时区（东区为正，小时），用于太阳位置计算；
    // 缺省 year=2019、timezone=8（见 Background 结构体）；lat/lon 无缺省值。
    if (sonExists("Meteorology", RootElement->ToElement()))
    {
        TiXmlNode *MeteorologyNode = RootElement->FirstChild("Meteorology");
        if (sonExists("Latitude", MeteorologyNode->ToElement())) {
            sceneXml.background.lat = stof(MeteorologyNode->FirstChildElement("Latitude")->GetText());
        }
        if (sonExists("Longitude", MeteorologyNode->ToElement())) {
            sceneXml.background.lon = stof(MeteorologyNode->FirstChildElement("Longitude")->GetText());
        }
        if (sonExists("Year", MeteorologyNode->ToElement())) {
            sceneXml.background.year = stoi(MeteorologyNode->FirstChildElement("Year")->GetText());
        }
        if (sonExists("TimeZone", MeteorologyNode->ToElement())) {
            sceneXml.background.timezone = stof(MeteorologyNode->FirstChildElement("TimeZone")->GetText());
        }
    }
//// obj文件和位置-----------------------
    // 步骤：VoxelEB 场景对象 <Object>。每个子元素是一个 PrimEntity：
    // types 取 Building / Vegetation / Water，shapeTypes 取 ellipsoid / cube，
    // shapes 为三个尺寸（会被重排为 (Y,Z,X) 存入 shape）；
    // 若给出 fileName/objectfile 则改为从 OBJ 文件体元化。
    if (m_mode == Mode::eVoxelEB)
    {
        sceneXml.objEntities = {};
//    PrimEntity treeEntity;
//    PrimEntity buildingEntity;
        std::vector<PrimEntity> PrimEntitys;
// 获取position
        if (sonExists("Object", sceneNode->ToElement())) {
            TiXmlElement *obj = sceneNode->FirstChildElement("Object");
            for (TiXmlElement *node = obj->FirstChildElement(); node != NULL; node = node->NextSiblingElement()) {
                PrimEntity entity;
                const bool voxelizeFromObj =
                    sonExists("fileName", node) || sonExists("objectfile", node);
                const char* objFileText = voxelizeFromObj
                    ? (sonExists("fileName", node)
                        ? node->FirstChildElement("fileName")->GetText()
                        : node->FirstChildElement("objectfile")->GetText())
                    : nullptr;
                // DEFERRED[中] 错误处理: meshNames/types/shapeTypes/shapes 以及各分支内的 canopyNames/bioNames/spectralNames/objectPosition 均无判空即解引用，任一缺失（例如 OBJ 体元化对象不写 shapes）就空指针崩溃；GetText() 为 nullptr 时赋给 std::string 同样崩溃；未识别的 types 值（如 "Soil"）静默留下未初始化的 entity.type；建议：用统一的 requireText(node, name) 辅助函数并对未知类型报错（VoxelRT 分支同理）。
                entity.primitiveName = node->FirstChildElement("meshNames")->GetText();
                entity.meshNames = {node->FirstChildElement("meshNames")->GetText()};
                std::string name = node->FirstChildElement("types")->GetText();

                // Building：spectralNames 的第 1、2 项分别作墙与屋顶光谱（只给一项则两者相同，缺省 "soil"），
                // meshNames 固定为 wall/roof，形状与高度不来自文件。
                if (name == "Building") {
                    const auto spectra = myFunction::mySplitStr(node->FirstChildElement("spectralNames")->GetText(), ",");
                    const std::string wallSpectrum = spectra.empty() ? "soil" : spectra.front();
                    const std::string roofSpectrum = spectra.size() > 1 ? spectra[1] : wallSpectrum;
                    entity.meshNames = {"wall", "roof"};
                    entity.spectralNames = {wallSpectrum, roofSpectrum};
//                    entity.thermalNames = {"wall", "roof"};
                    entity.canopyNames = {node->FirstChildElement("canopyNames")->GetText(),
                                          node->FirstChildElement("canopyNames")->GetText()};
                    entity.propNames = {node->FirstChildElement("bioNames")->GetText(),
                                        node->FirstChildElement("bioNames")->GetText()};
                    entity.type = Type::BUILDING;
                    entity.isshapeFromFile = false;
                    entity.shapefile = " ";
                    entity.isheightFromFile = false;
                    entity.heightfile = "";
//                entity.isdisFromFile = false;
//                entity.distributefile = " ";
                // Vegetation：冠层/材质/光谱来自 XML；位置分布文件 objectPosition 必填（逐株位置）。
                } else if (name == "Vegetation") {
                    entity.canopyNames = {node->FirstChildElement("canopyNames")->GetText()};
                    entity.propNames = {node->FirstChildElement("bioNames")->GetText()};
                    entity.spectralNames = {node->FirstChildElement("spectralNames")->GetText()};

                    if (sonExists("thermalNames", node)){
                        entity.thermalNames = {node->FirstChildElement("thermalNames")->GetText()};
                    }
                    entity.type = Type::VEGETATION;
                    entity.isshapeFromFile = false;
                    entity.shapefile = " ";
                    entity.isheightFromFile = false;
                    entity.heightfile = "";
                    entity.isdisFromFile = true;
                    entity.distributefile = node->FirstChildElement("objectPosition")->GetText();
                // Water：无冠层；objectPosition 可选，缺失表示不按文件分布。
                } else if (name == "Water" || name == "water") {
                    entity.canopyNames = {""};
                    entity.propNames = {node->FirstChildElement("bioNames")->GetText()};
                    entity.spectralNames = {node->FirstChildElement("spectralNames")->GetText()};
                    entity.type = Type::WATER;
                    entity.isshapeFromFile = false;
                    entity.isheightFromFile = false;
                    entity.isdisFromFile = sonExists("objectPosition", node);
                    if (entity.isdisFromFile) {
                        entity.distributefile = node->FirstChildElement("objectPosition")->GetText();
                    }
                }


                std::string shapetype = node->FirstChildElement("shapeTypes")->GetText();
                cout << shapetype << endl;
                const auto shapeDimensions = myFunction::mySplitFloat(
                    node->FirstChildElement("shapes")->GetText(), ",");
                const float shapeX = shapeDimensions.size() > 0 ? shapeDimensions[0] : 1.0f;
                const float shapeY = shapeDimensions.size() > 1 ? shapeDimensions[1] : 1.0f;
                const float shapeZ = shapeDimensions.size() > 2 ? shapeDimensions[2] : 1.0f;
                // DEFERRED[中] Bug: shapeTypes 只识别 ellipsoid/cube，其他值（如 "cylinder"、"Cone"）时 entity.shape 保持未初始化（ShapeXml 无默认值），体元化按垃圾尺寸生成；且 shapes 的 (X,Y,Z) 被重排为 (Y,Z,X)，含义（东/北/高？）未注明，易与 JSON 路径不一致；建议：未知形状报错，并注明轴映射。
                if (shapetype == "ellipsoid" || shapetype == "Ellipsoid") {
                    entity.shape = {ShapeType::ELLIPSOID,
                                    shapeY, shapeZ, shapeX,
                                    glm::vec3(0, 0, 0)};
                } else if (shapetype == "cube" || shapetype == "Cube") {
                    entity.shape = {ShapeType::CUBE,
                                    shapeY, shapeZ, shapeX,
                                    glm::vec3(0, 0, 0)};
                }
                // OBJ 体元化对象：相对路径按工程目录解析，meshNames/spectralNames/thermalNames/canopyNames/bioNames
                // 改按逗号拆分为列表（与网格一一对应），体元填充阈值沿用背景的 voxelFillThreshold。
                if (voxelizeFromObj && objFileText && *objFileText) {
                    entity.voxelizeFromObj = true;
                    std::filesystem::path objPath(objFileText);
                    if (objPath.is_relative() && !m_inputDirectory.empty()) {
                        objPath = std::filesystem::path(m_inputDirectory) / objPath;
                    }
                    entity.objFile = objPath.lexically_normal().string();
                    entity.voxelFillThreshold = sceneXml.background.voxelFillThreshold;

                    entity.meshNames = myFunction::mySplitStr(
                        node->FirstChildElement("meshNames")->GetText(), ",");
                    entity.spectralNames = myFunction::mySplitStr(
                        node->FirstChildElement("spectralNames")->GetText(), ",");
                    if (sonExists("thermalNames", node)) {
                        entity.thermalNames = myFunction::mySplitStr(
                            node->FirstChildElement("thermalNames")->GetText(), ",");
                    }

                    entity.canopyNames = sonExists("canopyNames", node)
                        ? myFunction::mySplitStr(
                            node->FirstChildElement("canopyNames")->GetText(), ",")
                        : std::vector<std::string>{""};
                    entity.propNames = sonExists("bioNames", node)
                        ? myFunction::mySplitStr(
                            node->FirstChildElement("bioNames")->GetText(), ",")
                        : std::vector<std::string>{""};

                    entity.isdisFromFile = sonExists("objectPosition", node);
                    if (entity.isdisFromFile) {
                        std::filesystem::path distributionPath(
                            node->FirstChildElement("objectPosition")->GetText());
                        if (distributionPath.is_relative() && !m_inputDirectory.empty()) {
                            distributionPath =
                                std::filesystem::path(m_inputDirectory) / distributionPath;
                        }
                        entity.distributefile =
                            distributionPath.lexically_normal().string();
                    }
                }

                PrimEntitys.push_back(entity);
            }
        }
        sceneXml.primEntities = PrimEntitys;
    }



    if (mode == Mode::eRaytracing)
    {
        // 模块：RayTracing 模式的场景对象（仅此模式用 ObjEntity，而非体元的 PrimEntity）。
        // 每个 <Object> 子元素给出 OBJ 文件名、网格名、光谱/热学名称与可选的位置分布文件 objectPosition；
        // 所有网格一律按植被（Type::VEGETATION）处理。缩放/旋转/分布列表留空，由位置文件决定。
        std::vector<ObjEntity> ObjEntities;
        // DEFERRED[中] 错误处理: objNode 未判空（场景无 <Object> 时空指针崩溃，而 VoxelEB/RT 分支有 sonExists 保护）；Attribute("objName")/fileName/meshNames/spectralNames 的 GetText() 均未判空；filePath/objectPosition 未像其他分支那样拼接 m_inputDirectory，相对路径按进程当前目录解析；建议：判空并统一做相对路径解析。
        TiXmlElement* objNode = sceneNode->FirstChildElement("Object");
        for (TiXmlElement* node = objNode->FirstChildElement(); node != NULL; node = node->NextSiblingElement())
        {
            ObjEntity objEntity;
            objEntity.objName = node->Attribute("objName");
            objEntity.filePath = node->FirstChildElement("fileName")->GetText();
//            objEntity.filePath = "D:\\work\\data\\field_aoyunlst\\field_data\\aoyun\\single_tree_a2_b3_q0.3_lai2.7.obj";
            // objEntity.meshNames = {"Crown"};
            objEntity.meshNames ={myFunction::mySplitStr(node->FirstChildElement("meshNames")->GetText(), ",")};
//            objEntity.types = {Type::SOIL, Type::VEGETATION, Type::VEGETATION};
            for (int i = 0; i < objEntity.meshNames.size(); i++)
            {
                objEntity.types.push_back(Type::VEGETATION);
            }
            // objEntity.types = {Type::VEGETATION};
            objEntity.spectralNames = {myFunction::mySplitStr(node->FirstChildElement("spectralNames")->GetText(), ",")};
//            objEntity.spectralNames = {"leaf"};
            if (sonExists("thermalNames", node)){
                objEntity.thermalNames = {myFunction::mySplitStr(node->FirstChildElement("thermalNames")->GetText(), ",")};
            }

//            objEntity.thermalNames = {"leaf"};
            if(sonExists("objectPosition", node))
            {
                objEntity.isFromFile = {true};
                objEntity.file = node->FirstChildElement("objectPosition")->GetText();
            }

//            objEntity.isFromFile = true;
//            objEntity.file = "D:/work/gray_rt/1/entity_0_position.txt";
            objEntity.objDistributions = {};
            objEntity.scales = {};
            objEntity.rotations = {};
            ObjEntities.push_back(objEntity);
        }
        sceneXml.objEntities = ObjEntities;
    }

    if (m_mode == Mode::eVoxelRT)
    {
        // 模块：VoxelRT 的场景对象 <Object>，逻辑与上面 VoxelEB 分支基本相同，主要区别：
        // types 名称大小写均接受；缺省 meshNames 取自 canopyNames；Water 也要求提供 canopyNames/bioNames。
        sceneXml.objEntities = {};
//    PrimEntity treeEntity;
//    PrimEntity buildingEntity;
        std::vector<PrimEntity> PrimEntitys;
// 获取position
        if (sonExists("Object", sceneNode->ToElement())) {
            TiXmlElement *obj = sceneNode->FirstChildElement("Object");
            for (TiXmlElement *node = obj->FirstChildElement(); node != NULL; node = node->NextSiblingElement()) {
                PrimEntity entity;
                const bool voxelizeFromObj =
                    sonExists("fileName", node) || sonExists("objectfile", node);
                const char* objFileText = voxelizeFromObj
                    ? (sonExists("fileName", node)
                        ? node->FirstChildElement("fileName")->GetText()
                        : node->FirstChildElement("objectfile")->GetText())
                    : nullptr;
                entity.primitiveName = node->FirstChildElement("meshNames")->GetText();
                entity.meshNames = {node->FirstChildElement("canopyNames")->GetText()};
                std::string name = node->FirstChildElement("types")->GetText();

                if (name == "building" || name == "Building") {
                    const auto spectra = myFunction::mySplitStr(node->FirstChildElement("spectralNames")->GetText(), ",");
                    const std::string wallSpectrum = spectra.empty() ? "soil" : spectra.front();
                    const std::string roofSpectrum = spectra.size() > 1 ? spectra[1] : wallSpectrum;
                    entity.meshNames = {"wall", "roof"};
                    entity.spectralNames = {wallSpectrum, roofSpectrum};
//                    entity.thermalNames = {"wall", "roof"};
                    entity.canopyNames = {node->FirstChildElement("canopyNames")->GetText(),
                                          node->FirstChildElement("canopyNames")->GetText()};
                    entity.propNames = {node->FirstChildElement("bioNames")->GetText(),
                                        node->FirstChildElement("bioNames")->GetText()};
                    entity.type = Type::BUILDING;
                    entity.isshapeFromFile = false;
                    entity.shapefile = " ";
                    entity.isheightFromFile = false;
                    entity.heightfile = "";
//                entity.isdisFromFile = false;
//                entity.distributefile = " ";
                } else if (name == "vegetation" || name == "Vegetation") {
                    entity.canopyNames = {node->FirstChildElement("canopyNames")->GetText()};
                    entity.propNames = {node->FirstChildElement("bioNames")->GetText()};
                    entity.spectralNames = {node->FirstChildElement("spectralNames")->GetText()};

                    if (sonExists("thermalNames", node)){
                        entity.thermalNames = {node->FirstChildElement("thermalNames")->GetText()};
                    }
                    entity.type = Type::VEGETATION;
                    entity.isshapeFromFile = false;
                    entity.shapefile = " ";
                    entity.isheightFromFile = false;
                    entity.heightfile = "";
                    entity.isdisFromFile = true;
                    entity.distributefile = node->FirstChildElement("objectPosition")->GetText();
                } else if (name == "water" || name == "Water") {
                    entity.canopyNames = {node->FirstChildElement("canopyNames")->GetText()};
                    entity.propNames = {node->FirstChildElement("bioNames")->GetText()};
                    entity.spectralNames = {node->FirstChildElement("spectralNames")->GetText()};
                    entity.type = Type::WATER;
                    entity.isshapeFromFile = false;
                    entity.isheightFromFile = false;
                    entity.isdisFromFile = sonExists("objectPosition", node);
                    if (entity.isdisFromFile) {
                        entity.distributefile = node->FirstChildElement("objectPosition")->GetText();
                    }
                }


                std::string shapetype = node->FirstChildElement("shapeTypes")->GetText();
                cout << shapetype << endl;
                const auto shapeDimensions = myFunction::mySplitFloat(
                    node->FirstChildElement("shapes")->GetText(), ",");
                const float shapeX = shapeDimensions.size() > 0 ? shapeDimensions[0] : 1.0f;
                const float shapeY = shapeDimensions.size() > 1 ? shapeDimensions[1] : 1.0f;
                const float shapeZ = shapeDimensions.size() > 2 ? shapeDimensions[2] : 1.0f;
                if (shapetype == "ellipsoid" || shapetype == "Ellipsoid") {
                    entity.shape = {ShapeType::ELLIPSOID,
                                    shapeY, shapeZ, shapeX,
                                    glm::vec3(0, 0, 0)};
                } else if (shapetype == "cube" || shapetype == "Cube") {
                    entity.shape = {ShapeType::CUBE,
                                    shapeY, shapeZ, shapeX,
                                    glm::vec3(0, 0, 0)};
                }
                if (voxelizeFromObj && objFileText && *objFileText) {
                    entity.voxelizeFromObj = true;
                    std::filesystem::path objPath(objFileText);
                    if (objPath.is_relative() && !m_inputDirectory.empty()) {
                        objPath = std::filesystem::path(m_inputDirectory) / objPath;
                    }
                    entity.objFile = objPath.lexically_normal().string();
                    entity.voxelFillThreshold = sceneXml.background.voxelFillThreshold;

                    entity.meshNames = myFunction::mySplitStr(
                        node->FirstChildElement("meshNames")->GetText(), ",");
                    entity.spectralNames = myFunction::mySplitStr(
                        node->FirstChildElement("spectralNames")->GetText(), ",");
                    if (sonExists("thermalNames", node)) {
                        entity.thermalNames = myFunction::mySplitStr(
                            node->FirstChildElement("thermalNames")->GetText(), ",");
                    }

                    entity.canopyNames = sonExists("canopyNames", node)
                        ? myFunction::mySplitStr(
                            node->FirstChildElement("canopyNames")->GetText(), ",")
                        : std::vector<std::string>{""};
                    entity.propNames = sonExists("bioNames", node)
                        ? myFunction::mySplitStr(
                            node->FirstChildElement("bioNames")->GetText(), ",")
                        : std::vector<std::string>{""};

                    entity.isdisFromFile = sonExists("objectPosition", node);
                    if (entity.isdisFromFile) {
                        std::filesystem::path distributionPath(
                            node->FirstChildElement("objectPosition")->GetText());
                        if (distributionPath.is_relative() && !m_inputDirectory.empty()) {
                            distributionPath =
                                std::filesystem::path(m_inputDirectory) / distributionPath;
                        }
                        entity.distributefile =
                            distributionPath.lexically_normal().string();
                    }
                }

                PrimEntitys.push_back(entity);
            }
        }
        sceneXml.primEntities = PrimEntitys;
    }
    return sceneXml;
}


// 模块：读取 definedDir/defined/optipar.txt（PROSPECT/Fluspect 的光学常数表）。
// 输入：当前模式的 Xml 对象中的 definedDir；输出：definedio->m_fluspectCoeff 的各列系数。
// 说明：列顺序为 0 波长 wl、1 折射率 nr、2 kab、3 kca、4 ks、5 kw、6 kdm、7 phiI、8 phiII、
// 9-12 类胡萝卜素相关系数（kcaV/kcaZ/kcant/kcaV2）、13 phi、14-16 gsv1-3、17 nw（待核对）。
void FileIO::readDefined(std::shared_ptr<DefinedIO> & definedio) {

    if (m_mode == Mode::eRaytracing){
        definedio->definedDir = m_pRaytracingXml->definedDir;
    }
    if (m_mode == Mode::eVoxelRT){
        definedio->definedDir = m_pVoxelrtXml->definedDir;
    }
    if (m_mode == Mode::eVoxelEB) {
        definedio->definedDir = m_pVoxelebXml->definedDir;
    }
    std::string predifineDir = definedio->definedDir + "\\defined\\";
    std::string infileName = predifineDir + "optipar.txt";
    // DEFERRED[中] 错误处理: optipar.txt 被完整读取 18 次（每列一次），且 readascfileinout 打不开文件时 num 保持旧值、不报错（见 utils.cpp 已有注释），各系数向量为空或长度不一，fluspect 随后越界；也未校验各列长度一致（应为 2001）；建议：一次读入全部列，校验行数与列数，失败时抛出带路径的异常；路径分隔符用 std::filesystem::path 拼接。
    int num = 1;
    Utils::readascfileinout(infileName,0,0,definedio->m_fluspectCoeff.wl_,num);
    Utils::readascfileinout(infileName,0,1,definedio->m_fluspectCoeff.nr_,num);
    Utils::readascfileinout(infileName,0,2,definedio->m_fluspectCoeff.kab_,num);
    Utils::readascfileinout(infileName,0,3,definedio->m_fluspectCoeff.kca_,num);
    Utils::readascfileinout(infileName,0,4,definedio->m_fluspectCoeff.ks_,num);
    Utils::readascfileinout(infileName,0,5,definedio->m_fluspectCoeff.kw_,num);
    Utils::readascfileinout(infileName,0,6,definedio->m_fluspectCoeff.kdm_,num);
    Utils::readascfileinout(infileName,0,7,definedio->m_fluspectCoeff.phiI_,num);
    Utils::readascfileinout(infileName,0,8,definedio->m_fluspectCoeff.phiII_,num);
    Utils::readascfileinout(infileName,0,9,definedio->m_fluspectCoeff.kcaV_,num);
    Utils::readascfileinout(infileName,0,10,definedio->m_fluspectCoeff.kcaZ_,num);
    Utils::readascfileinout(infileName,0,11,definedio->m_fluspectCoeff.kcant_,num);
    Utils::readascfileinout(infileName,0,12,definedio->m_fluspectCoeff.kcaV2_,num);
    Utils::readascfileinout(infileName,0,13,definedio->m_fluspectCoeff.phi_,num);
    Utils::readascfileinout(infileName,0,14,definedio->m_fluspectCoeff.gsv1_,num);
    Utils::readascfileinout(infileName,0,15,definedio->m_fluspectCoeff.gsv2_,num);
    Utils::readascfileinout(infileName,0,16,definedio->m_fluspectCoeff.gsv3_,num);
    Utils::readascfileinout(infileName,0,17,definedio->m_fluspectCoeff.nw_,num);
    // return false;
}

// 模块：读取 VoxelEB 气象文件并准备入射光谱（Esun/Esky）。
// 输入：m_pVoxelebXml->meteoxml.meteofile（首行为节点数，其后每行一个节点：
// t Ta ea p u Rin Rli，可选第 8、9 列为水平面直射/散射短波 W m-2）；
// 气象元数据 m_meteoXml.meta（z、sm、ea、Ca、Oa、dTime 等）补入每个节点。
// 输出：meteos（逐节点气象）、n_node（节点数）、wavesets（波长网格 + 入射直射/散射系数）、
// m_meteoMeasuredSplit（各节点实测直射/散射，缺失为 NaN）与 m_unitShortwaveShapes。
// 校验：文件为空、头部非法、节点数 <=0、dTime 非正、数值非有限或物理上不合理都会抛异常。
void FileIO::readMeteo(std::shared_ptr<DefinedIO> &defineio,int & n_node,
                       std::vector<Meteo> &meteos, std::vector<AtomCond> &wavesets) {


    auto & meteofile = m_pVoxelebXml->meteoxml.meteofile;



    // DEFERRED[低] 错误处理: 窄字符 ifstream 打开（中文路径失败），且未区分"打不开"与"空文件"，打不开时报 "Meteorology file is empty" 误导排查；meteofile 相对路径未拼接 m_inputDirectory；建议：u8path 打开并先检查 is_open()，相对路径按输入目录解析。
    // 步骤 1：打开文件，读首行节点数 fileNodeCount 并预留空间；清空上一次的实测直射/散射记录。
    std::ifstream infile(meteofile);
    std::vector<std::string> fields;
    std::string deli(" "), line;

    if (!std::getline(infile, line)) {
        throw std::runtime_error("Meteorology file is empty: " + meteofile);
    }
    std::istringstream header(line);
    int fileNodeCount = 0;
    if (!(header >> fileNodeCount) || (!header.eof() && !std::isspace(static_cast<unsigned char>(header.peek())))) {
        throw std::runtime_error("Meteorology header is invalid: " + meteofile);
    }
    if (fileNodeCount <= 0) {
        throw std::runtime_error("Meteorology node count must be positive");
    }
    meteos.clear();
    meteos.reserve(static_cast<size_t>(fileNodeCount));
    m_meteoMeasuredSplit.clear();

    // 步骤 2：取全局元数据作为每个节点的缺省值（观测高度 z、土壤湿度 sm、CO2 Ca、O2 Oa、时间步 dTime）。
    int meteoNum = 0;
    float z = m_meteoXml.meta.z;//15;
    float sm = m_meteoXml.meta.sm;//0.25;
    float ea = m_meteoXml.meta.ea;//15;
    float Ca = m_meteoXml.meta.Ca;//380
    float Oa = m_meteoXml.meta.Oa;//209
    float Tsold = m_meteoXml.meta.Tsold;//
    float SatWater = m_meteoXml.meta.SatWater;//0.45;
    float dTime = m_meteoXml.meta.dTime;//1800;
    if (!std::isfinite(dTime) || !(dTime > 0.0f)) {
        throw std::runtime_error("Meteorology time step dTime must be a positive number of seconds");
    }

    int rowIndex = 0;
    // 步骤 3：逐行读节点：必须有 7 个数值，且 p>0、ea/u/Rin/Rli >= 0，否则抛异常；空行跳过。
    while (std::getline(infile, line))
    {
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        std::istringstream row(line);
        Meteo mi;
        if (!(row >> mi.t >> mi.Ta >> mi.ea >> mi.p >> mi.u >> mi.Rin >> mi.Rli) ||
            (!row.eof() && !std::isspace(static_cast<unsigned char>(row.peek())))) {
            throw std::runtime_error("Meteorology node " + std::to_string(rowIndex) +
                                     " requires seven numeric fields");
        }
        if (!std::isfinite(mi.t) || !std::isfinite(mi.Ta) ||
            !std::isfinite(mi.ea) || !std::isfinite(mi.p) ||
            !std::isfinite(mi.u) || !std::isfinite(mi.Rin) || !std::isfinite(mi.Rli) ||
            mi.p <= 0.0f || mi.ea < 0.0f || mi.u < 0.0f || mi.Rin < 0.0f || mi.Rli < 0.0f) {
            throw std::runtime_error("Meteorology node " + std::to_string(rowIndex) +
                                     " has non-finite or invalid physical values");
        }
        mi.sm = sm;
        mi.z = z;
//        mi.ea = ea;
        mi.Ca = Ca;
        mi.Oa = Oa;
        mi.dTime = dTime;

        // Optional columns 8-9: horizontal direct and diffuse shortwave
        // (W m-2), read only by light.diffuseFractionMode = erbs.
        {
            // 可选的第 8、9 列：读不到或非法时记为 NaN，表示该节点没有实测直射/散射。
            float measuredDirect = 0.0f, measuredDiffuse = 0.0f;
            std::array<float, 2> measured{std::numeric_limits<float>::quiet_NaN(),
                                          std::numeric_limits<float>::quiet_NaN()};
            if (row >> measuredDirect >> measuredDiffuse &&
                std::isfinite(measuredDirect) && std::isfinite(measuredDiffuse) &&
                measuredDirect >= 0.0f && measuredDiffuse >= 0.0f) {
                measured = {measuredDirect, measuredDiffuse};
            }
            m_meteoMeasuredSplit.push_back(measured);
        }

        meteos.emplace_back(mi);
        ++rowIndex;
        //m_meteoParams.emplace_back(mi);
    }
    if (meteos.size() < static_cast<size_t>(fileNodeCount)) {
        throw std::runtime_error("Meteorology file contains fewer rows than its header");
    }
    // 节点数取实际读到的行数：少于首行声明时已在上面抛异常，多于声明时全部使用。
    n_node = static_cast<int>(meteos.size());

    ///-----------------------------------------------------------------------------
    ///   WaveLength and atmospheric condition
    ///----------------------------------------------------------------------------

    // DEFERRED[低] 可维护性: 波长网格以字面量 2001/126/35 硬编码，只在 N1=2001、N2=161 时恰好填满 wavesets；N1/N2 宏一旦调整即越界写或留下未初始化波长；建议：以 N1、N2 推导循环上界并 static_assert(N1==2001 && N2==126+35)。
    // 步骤 4：建立波长网格（nm）：400–2400 步长 1（2001 点，可见-近红外 VNIR）、
    // 2500–15000 步长 100（126 点）、16000–50000 步长 1000（35 点），后两段属于热红外/长波段，
    // 共 N1+N2=2162 点；N1 为短波点数 2001。
    wavesets.resize(N1+N2);
    for (int i = 0; i <  2001 ; i++)
    {
        wavesets[i].wavelength = 400 + i;
    }
    for (int i = 0; i < 126; i++)
    {
        wavesets[i+2001].wavelength = 2500 + i * 100;
    }
    for (int i = 0; i < 35; i++)
    {
        wavesets[i+2127].wavelength = 16000 + i * 1000;
    }


    ///-----------------------------------------------------------------------------
    ///   Atmospheric condition in radiative transfer domain
    ///----------------------------------------------------------------------------
    // 步骤 5：读入太阳直射 Esun 与天空散射 Esky 的入射光谱（每行一个样本，需覆盖全部 N1+N2 个波长点）。
    const auto esun_ = readIncidentSpectrum(m_pVoxelebXml->atomcondxml.rinfile, N1 + N2);
    const auto esky_ = readIncidentSpectrum(m_pVoxelebXml->atomcondxml.rlifile, N1 + N2);
    std::vector<float> fesky_(N1 + N2), fesun_(N1 + N2);

    // 步骤 6：用梯形法分别对短波（前 N1 点）与长波（其余点）入射光谱积分，用于归一化；
    // Ts* 为短波积分，Tl* 为长波积分。
    float TsEsky = 0, TlEsky = 0, TlEsun = 0, TsEsun = 0, tltot = 0, temp1, temp2, step;
    int b1 = N1;
    int b2 = N1+N2;

    // �̲�
    for (int i = 0; i < b1 - 1; i++)
    {
        temp1 = (esky_[i] + esky_[i + 1]) / 2.0;
        step =  wavesets[i+1].wavelength -  wavesets[i].wavelength;
        temp2 = (esun_[i] + esun_[i + 1]) / 2.0;
        TsEsky += temp1 * step;
        TsEsun += temp2 * step;
    }
    // Rin is total shortwave irradiance. Each retained spectral shape receives
    // its configured fraction, including the pure-direct/pure-diffuse limits.
    // 步骤 7：短波系数 = 光谱形状/积分 × 能量份额 × 1000（GPU 端的缩放，见 normalizedShortwaveCoefficient）；
    // 首尾点权重为 0.5（梯形法端点）。fesun_/fesky_ 按 light.direct 分配直射/散射份额，
    // m_unitShortwaveShapes 保存份额为 1 的形状，供按节点改变直射/散射比例时缩放。
    const float directFraction = m_pVoxelebXml->lightxml.direct;
    m_unitShortwaveShapes.assign(b1, AtomCond{});
    for (int i = 0; i < b1; i++)
    {
        const float endpointWeight = i == 0 || i == b1 - 1 ? 0.5f : 1.0f;
        fesky_[i] = endpointWeight * normalizedShortwaveCoefficient(esky_[i], TsEsky, 1.0f - directFraction);
        fesun_[i] = endpointWeight * normalizedShortwaveCoefficient(esun_[i], TsEsun, directFraction);
        // Fraction-1 shapes for a per-node split (light.diffuseFractionMode).
        m_unitShortwaveShapes[i].wavelength = wavesets[i].wavelength;
        m_unitShortwaveShapes[i].direct = endpointWeight * normalizedShortwaveCoefficient(esun_[i], TsEsun, 1.0f);
        m_unitShortwaveShapes[i].diffuse = endpointWeight * normalizedShortwaveCoefficient(esky_[i], TsEsky, 1.0f);
    }
    // ����
    for (int j = b1; j < b2 - 1; j++)
    {
        temp1 = (esky_[j] + esky_[j + 1]) / 2.0;
        step =   wavesets[j+1].wavelength -  wavesets[j].wavelength;
        temp2 = (esun_[j] + esun_[j + 1]) / 2.0;
        TlEsky += temp1 * step;
        TlEsun += temp2 * step;
    }
    // 步骤 8：长波不区分份额：直射+散射积分之和除以 1000 作为归一化因子，
    // 使长波段两项积分之和等于 1000（与短波系数同一缩放，待核对）；积分非正则抛异常。
    tltot = (TlEsky + TlEsun) * 0.001;
    if (!std::isfinite(tltot) || tltot <= 0.0f) {
        throw std::runtime_error("Incident longwave spectrum has no finite positive integral");
    }
    for (int i = b1; i < b2; i++)
    {
        fesky_[i] = esky_[i] / tltot;
        fesun_[i] = esun_[i] / tltot;
    }

    for (int i = 0; i < b2; i++)
    {
        // 步骤 9：把归一化后的直射/散射系数写入 wavesets。
        wavesets[i].direct = fesun_[i];
        wavesets[i].diffuse = fesky_[i];
    }

}



//void FileIO::writeENVIdata(std::string projectDir, float *pData, int width, int height, int band,
//                           Angle &angle, float t) {
//
//    std::ostringstream  oss_x;
//    oss_x << std::fixed << std::setprecision(2)<<std::setfill('0')<<angle.vza;
//    std::ostringstream  oss_y;
//    oss_y << std::fixed << std::setprecision(2)<<std::setfill('0')<<angle.vaa;
//    std::ostringstream  oss_z;
//    oss_z << std::fixed << std::setprecision(2)<<std::setfill('0')<<angle.sza;
//    std::ostringstream  oss_w;
//    oss_w << std::fixed << std::setprecision(2)<<std::setfill('0')<<angle.saa;
//    std::ostringstream  oss_t;
//    oss_t << std::fixed << std::setprecision(2)<<std::setfill('0')<<t;
//
////    std::string outPath = projectDir + "/results/VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
////                          "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".tif";
//    std::string tifName,hdrName;
//    if(t > 0){
//        tifName = projectDir + "/results/t=" + oss_t.str() +"_VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
//                  "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".img";
//        hdrName = projectDir + "/results/t=" + oss_t.str() +"_VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
//                  "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".hdr";
//    }else {
//        tifName = projectDir +"/results/VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
//                  "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".img";
//        hdrName = projectDir +"/results/VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
//                  "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".hdr";
//    }
//    std::ofstream outfilet1(tifName.c_str(), std::ios::binary);
//    outfilet1.write(reinterpret_cast<const char*>(pData), sizeof(float) * width * height * band);
//    outfilet1.close();
//
//    std::ofstream outfile(hdrName);
//    if (outfile.is_open())
//    {
//        outfile << "ENVI" << std::endl;
//        outfile << "description = {" << std::endl;
//        outfile << " File Imported into ENVI.} " << std::endl;
//        outfile << "samples = " << width << std::endl;
//        outfile << "lines   = " << height << std::endl;
//        outfile << "bands   =  " << band << std::endl;
//        outfile << "header offset = 0" << std::endl;
//        outfile << "file type = ENVI Standard" << std::endl;
//        outfile << "data type = 4" << std::endl;
//        outfile << "interleave = bsp" << std::endl;
//        outfile << "sensor type = unknown" << std::endl;
//        outfile << "byte order = 0" << std::endl;
//        outfile << "wavelength units = Unknown" << std::endl;
//        outfile.close();
//    }
//    outfile.close();
//
//
//}

//void FileIO::readCustomSpectral(TiXmlElement* pEle)
//{
//    ManualSpectralStruct temp;
//    temp.name = pEle->Attribute("name");;
//
//    std::vector<float> reflectanceVec = myFunction::mySplitFloat(pEle->FirstChildElement("reflectance")->GetText(), ",");
//    std::vector<float> transmittanceVec = myFunction::mySplitFloat(pEle->FirstChildElement("transmittance")->GetText(), ",");
//    for (int iter = 0; iter < reflectanceVec.size(); iter++)
//    {
//        OpticalMaterial optical;
//        optical.band = sensorDatasets[0].controlBand[iter];
//        optical.reflectance = reflectanceVec[iter];
//        optical.transmittance = transmittanceVec[iter];
//        temp.m_opticalMaterials.push_back(optical);
//    }
//    manualSpectralDatasets.push_back(temp);
//}






//void FileIO::writeENVIdata(std::string projectDir, float *pData, int width, int height, int band,
//                           Angle &angle, float t) {
//
//    std::ostringstream  oss_x;
//    oss_x << std::fixed << std::setprecision(2)<<std::setfill('0')<<angle.vza;
//    std::ostringstream  oss_y;
//    oss_y << std::fixed << std::setprecision(2)<<std::setfill('0')<<angle.vaa;
//    std::ostringstream  oss_z;
//    oss_z << std::fixed << std::setprecision(2)<<std::setfill('0')<<angle.sza;
//    std::ostringstream  oss_w;
//    oss_w << std::fixed << std::setprecision(2)<<std::setfill('0')<<angle.saa;
//    std::ostringstream  oss_t;
//    oss_t << std::fixed << std::setprecision(2)<<std::setfill('0')<<t;
//
////    std::string outPath = projectDir + "/results/VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
////                          "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".tif";
//    std::string tifName,hdrName;
//    if(t > 0){
//        tifName = projectDir + "/results/t=" + oss_t.str() +"_VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
//                              "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".img";
//        hdrName = projectDir + "/results/t=" + oss_t.str() +"_VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
//                               "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".hdr";
//    }else {
//        tifName = projectDir +"/results/VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
//                              "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".img";
//        hdrName = projectDir +"/results/VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
//                              "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".hdr";
//    }
//    std::ofstream outfilet1(tifName.c_str(), std::ios::binary);
//    outfilet1.write(reinterpret_cast<const char*>(pData), sizeof(float) * width * height * band);
//    outfilet1.close();
//
//    std::ofstream outfile(hdrName);
//    if (outfile.is_open())
//    {
//        outfile << "ENVI" << std::endl;
//        outfile << "description = {" << std::endl;
//        outfile << " File Imported into ENVI.} " << std::endl;
//        outfile << "samples = " << width << std::endl;
//        outfile << "lines   = " << height << std::endl;
//        outfile << "bands   =  " << band << std::endl;
//        outfile << "header offset = 0" << std::endl;
//        outfile << "file type = ENVI Standard" << std::endl;
//        outfile << "data type = 4" << std::endl;
//        outfile << "interleave = bsp" << std::endl;
//        outfile << "sensor type = unknown" << std::endl;
//        outfile << "byte order = 0" << std::endl;
//        outfile << "wavelength units = Unknown" << std::endl;
//        outfile.close();
//    }
//    outfile.close();


//}

// 模块：把多波段 float 影像写成 GeoTIFF，并同步追加统计 CSV（每波段 count/min/max/mean/stddev）。
// 输入：pData 为按波段连续存放的 band×height×width 数组（float，逐波段 BSQ，行优先）；
// angle 为 sza/saa/vza/vaa（度）；t 为时间（儒略日 DOY 加日内小数，<0 表示非时间序列）；
// modelSuffix 为文件名后缀（以 "_v" 开头视为体元模式输出）；positionIndex>=0 时文件名附加 _P=序号；
// flipRows 为真时输出前上下翻转行；bandNames 为各波段名称（写入描述与统计）。
// 输出：projectDir 下的 .tif 与 result_statistics_*.csv；成功返回 true，参数非法或 GDAL 失败返回 false。
// 说明：影像方位用元数据标注（NORTH_UP_EAST_RIGHT、场景轴 X 北/Y 上/Z 东），没有写地理参考。
bool FileIO::writeTIFData(const std::string& projectDir, const float* pData, int width, int height,
                          int band, const Angle& angle, float t, const std::string& modelSuffix,
                          int positionIndex, bool flipRows,
                          const std::vector<std::string>& bandNames) {
    if (pData == nullptr || width <= 0 || height <= 0 || band <= 0) {
        std::cerr << "Invalid GeoTIFF output parameters." << std::endl;
        return false;
    }

    // 步骤 1：组装输出文件名。有时间（t>=0）：T=DOYddd_hh-mm_VZA=_VAA=（t 的小数部分转为分钟，
    // 进位到 1440 分钟时 DOY 加 1）；无时间：SZA=_SAA=_VZA=_VAA=（角度保留 2 位小数）。
    std::ostringstream vzaText;
    vzaText << std::fixed << std::setprecision(2) << angle.vza;
    std::ostringstream vaaText;
    vaaText << std::fixed << std::setprecision(2) << angle.vaa;
    std::string outputPath;
    if (t >= 0.0f) {
        int doy = static_cast<int>(std::floor(t));
        int minutesOfDay = static_cast<int>(std::lround(
            (t - static_cast<float>(doy)) * 1440.0f));
        if (minutesOfDay >= 1440) {
            minutesOfDay -= 1440;
            ++doy;
        }
        minutesOfDay = std::max(0, minutesOfDay);
        std::ostringstream timeText;
        timeText << "DOY" << doy << "_"
                 << std::setw(2) << std::setfill('0') << minutesOfDay / 60 << "-"
                 << std::setw(2) << std::setfill('0') << minutesOfDay % 60;
        outputPath = projectDir + "/T=" + timeText.str()
            + "_VZA=" + vzaText.str() + "_VAA=" + vaaText.str()
            + (positionIndex >= 0 ? "_P=" + std::to_string(positionIndex) : "")
            + modelSuffix + ".tif";
    } else {
        std::ostringstream szaText;
        szaText << std::fixed << std::setprecision(2) << angle.sza;
        std::ostringstream saaText;
        saaText << std::fixed << std::setprecision(2) << angle.saa;
        outputPath = projectDir + "/SZA=" + szaText.str()
            + "_SAA=" + saaText.str() + "_VZA=" + vzaText.str()
            + "_VAA=" + vaaText.str();
        if (positionIndex >= 0) {
            outputPath += "_P=" + std::to_string(positionIndex);
        }
        outputPath += modelSuffix + ".tif";
    }

    // DEFERRED[低] 错误处理: projectDir 不存在时 Create 失败只打印并返回 false（调用方多数忽略返回值）；outputPath 为窄字符串，进程设置 GDAL_FILENAME_IS_UTF8=NO 时中文输出目录依赖 ANSI 代码页；文件名中的角度含 '=' 与 '.'，在部分工具中不友好；建议：写出前 create_directories 并让调用方检查返回值。
    // 步骤 2：用 GDAL 的 GTiff 驱动创建 Float32 多波段文件（按波段交错，BIGTIFF=IF_SAFER 在大文件时自动启用）。
    GDALAllRegister();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (driver == nullptr) {
        std::cerr << "GDAL GTiff driver is unavailable." << std::endl;
        return false;
    }
    char* creationOptions[] = {
        const_cast<char*>("INTERLEAVE=BAND"),
        const_cast<char*>("BIGTIFF=IF_SAFER"),
        nullptr
    };
    GDALDataset* dataset = driver->Create(
        outputPath.c_str(), width, height, band, GDT_Float32, creationOptions);
    if (dataset == nullptr) {
        std::cerr << "Failed to create GeoTIFF: " << outputPath << std::endl;
        return false;
    }
    // 步骤 3：写元数据：图像描述（波段名列表，缺名的用 "Band n"）、方位约定、太阳天顶/方位角（度）。
    if (!bandNames.empty()) {
        std::ostringstream description;
        description << "HiStream image bands: {";
        for (int index = 0; index < band; ++index) {
            if (index > 0) description << ", ";
            description << (index < static_cast<int>(bandNames.size())
                ? bandNames[index] : "Band " + std::to_string(index + 1));
        }
        description << '}';
        dataset->SetMetadataItem("TIFFTAG_IMAGEDESCRIPTION", description.str().c_str());
    }
    dataset->SetMetadataItem("HISTREAM_IMAGE_ORIENTATION", "NORTH_UP_EAST_RIGHT");
    dataset->SetMetadataItem("HISTREAM_SCENE_AXES", "X_NORTH_Y_UP_Z_EAST");
    dataset->SetMetadataItem("HISTREAM_OBJ_AXES", "X_NORTH_Y_UP_Z_EAST");
    dataset->SetMetadataItem("HISTREAM_SOLAR_ZENITH_DEG", std::to_string(angle.sza).c_str());
    dataset->SetMetadataItem("HISTREAM_SOLAR_AZIMUTH_DEG", std::to_string(angle.saa).c_str());

    // 步骤 4：逐波段写像素；flipRows 时先把该波段按行倒序拷贝到临时缓冲再写入。任一波段写失败则终止。
    const size_t pixelsPerBand = static_cast<size_t>(width) * height;
    std::vector<float> flippedBand;
    if (flipRows) {
        flippedBand.resize(pixelsPerBand);
    }
    bool succeeded = true;
    for (int bandIndex = 0; bandIndex < band; ++bandIndex) {
        const float* bandData = pData + static_cast<size_t>(bandIndex) * pixelsPerBand;
        if (flipRows) {
            for (int row = 0; row < height; ++row) {
                std::copy_n(
                    bandData + static_cast<size_t>(height - 1 - row) * width,
                    width,
                    flippedBand.data() + static_cast<size_t>(row) * width);
            }
            bandData = flippedBand.data();
        }
        GDALRasterBand* rasterBand = dataset->GetRasterBand(bandIndex + 1);
        if (bandIndex < static_cast<int>(bandNames.size()))
            rasterBand->SetDescription(bandNames[bandIndex].c_str());
        const CPLErr result = rasterBand->RasterIO(
            GF_Write, 0, 0, width, height,
            const_cast<float*>(bandData),
            width, height, GDT_Float32, 0, 0, nullptr);
        if (result != CE_None) {
            succeeded = false;
            break;
        }
    }
    dataset->FlushCache();
    GDALClose(dataset);
    if (!succeeded) {
        std::cerr << "Failed to write GeoTIFF pixels: " << outputPath << std::endl;
        return false;
    }
    // 步骤 5：追加统计 CSV。文件名由模式决定：_v* 后缀且有时间为 voxeleb，无时间为 voxelrt，其余为 raytracing；
    // 同一 FileIO 实例内首次写某个 CSV 时覆盖旧文件并写表头，之后追加。
    const bool voxelOutput = modelSuffix.rfind("_v", 0) == 0;
    const std::string statisticsSuffix = voxelOutput
        ? (t >= 0.0f ? "voxeleb" : "voxelrt")
        : "raytracing";
    const std::filesystem::path statisticsPath =
        std::filesystem::path(projectDir)
        / ("result_statistics_" + statisticsSuffix + ".csv");
    const bool firstStatisticsWrite = m_initializedStatisticsPaths
        .insert(statisticsPath.lexically_normal().string()).second;
    const bool writeHeader = firstStatisticsWrite;
    std::ofstream statistics(
        statisticsPath,
        firstStatisticsWrite ? std::ios::trunc : std::ios::app);
    if (statistics) {
        if (writeHeader) {
            statistics << "file,mode,time,vza,vaa,sza,saa,band_index,band_name,count,min,max,mean,stddev\n";
        }
        const std::string fileName = std::filesystem::path(outputPath).filename().string();
        const std::string modeName = voxelOutput
            ? (t >= 0.0f ? "VoxelEB" : "VoxelRT") : "Raytracing";
        // 辅助 lambda：CSV 字段加双引号并把内部的双引号转义为两个双引号。
        auto csv = [](const std::string& value) {
            std::string escaped;
            escaped.reserve(value.size() + 2);
            escaped.push_back('"');
            for (const char character : value) {
                escaped.push_back(character);
                if (character == '"') escaped.push_back('"');
            }
            escaped.push_back('"');
            return escaped;
        };
        // 逐波段统计：忽略非有限值（NaN/Inf）；用 Welford 在线算法累计均值与二阶矩，
        // 标准差为总体标准差 sqrt(moment/count)；无有效像元时 min/max/mean/stddev 留空。
        const size_t pixelCount = static_cast<size_t>(width) * height;
        statistics << std::setprecision(12);
        for (int bandIndex = 0; bandIndex < band; ++bandIndex) {
            const float* values = pData + static_cast<size_t>(bandIndex) * pixelCount;
            size_t count = 0;
            double mean = 0.0;
            double moment = 0.0;
            double minimum = std::numeric_limits<double>::infinity();
            double maximum = -std::numeric_limits<double>::infinity();
            for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
                const double value = values[pixel];
                if (!std::isfinite(value)) continue;
                ++count;
                const double delta = value - mean;
                mean += delta / static_cast<double>(count);
                moment += delta * (value - mean);
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
            const std::string name = bandIndex < static_cast<int>(bandNames.size())
                ? bandNames[bandIndex] : "Band " + std::to_string(bandIndex + 1);
            statistics << csv(fileName) << ',' << modeName << ',';
            if (t >= 0.0f) statistics << t;
            statistics << ',' << angle.vza << ',' << angle.vaa << ',' << angle.sza << ','
                       << angle.saa << ',' << bandIndex << ','
                       << csv(name) << ',' << count << ',';
            if (count > 0) {
                statistics << minimum << ',' << maximum << ',' << mean << ','
                           << std::sqrt(moment / static_cast<double>(count));
            } else {
                statistics << ",,,";
            }
            statistics << '\n';
        }
        std::cout << "Statistics output: " << statisticsPath.string() << std::endl;
    }
    std::cout << "GeoTIFF output: " << outputPath << std::endl;
    return true;
}

// 模块：把 float 影像写成 ENVI 格式（.img 原始二进制 + .hdr 头文件，BSQ，小端，data type=4 即 float32）。
// 输入：同 writeTIFData 的 pData 布局；t>=0 时文件名含 t=（3 位小数），否则 k>=0 时附加 _k 序号。
// 说明：.img 为直接 fwrite，不检查写入是否成功，也不创建目录。
void FileIO::writeENVIdata(std::string projectDir, float *pData, int width, int height, int band,
                           Angle &angle, float t, int k) {

    std::ostringstream  oss_x;
    oss_x << std::fixed << std::setprecision(2)<<std::setfill('0')<<angle.vza;
    std::ostringstream  oss_y;
    oss_y << std::fixed << std::setprecision(2)<<std::setfill('0')<<angle.vaa;
    std::ostringstream  oss_z;
    oss_z << std::fixed << std::setprecision(2)<<std::setfill('0')<<angle.sza;
    std::ostringstream  oss_w;
    oss_w << std::fixed << std::setprecision(2)<<std::setfill('0')<<angle.saa;
    std::ostringstream  oss_t;
    oss_t << std::fixed << std::setprecision(3)<<std::setfill('0')<<t;
    std::ostringstream  oss_k;
    oss_k << std::fixed << std::setprecision(0)<<std::setfill('0')<<k;

//    std::string outPath = projectDir + "/results/VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
//                          "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".tif";
    std::string tifName,hdrName;
    if(t >= 0){
        tifName = projectDir + "/t=" + oss_t.str() +"_VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
                  "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".img";
        hdrName = projectDir + "/t=" + oss_t.str() +"_VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
                  "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".hdr";
    }else if(k >= 0) {
        tifName = projectDir +"/VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
                  "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + "_"+oss_k.str()+".img";
        hdrName = projectDir +"/VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
                  "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + "_"+oss_k.str()+".hdr";
    }else
    {
        tifName = projectDir +"/VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
                  "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".img";
        hdrName = projectDir +"/VZA=" + oss_x.str() + "_VAA=" + oss_y.str() +
                  "_SZA=" + oss_z.str() + "_SAA=" + oss_w.str() + ".hdr";
    }
    // DEFERRED[中] Bug: t>=0 分支的文件名不含 k，同一时刻多个位置/序号的输出互相覆盖；pData 未判空、width/height/band 未校验 >0；写入失败（目录不存在、磁盘满）不检查流状态，静默产生空/截断 .img；与 writeTIFData 不同，未创建输出目录；建议：t>=0 时同样附加 k，写后检查 outfilet1.good() 并返回 bool。
    std::ofstream outfilet1(tifName.c_str(), std::ios::binary);
    outfilet1.write(reinterpret_cast<const char*>(pData), sizeof(float) * width * height * band);
    outfilet1.close();

    std::ofstream outfile(hdrName);
    if (outfile.is_open())
    {
        outfile << "ENVI" << std::endl;
        outfile << "description = {" << std::endl;
        outfile << " File Imported into ENVI.} " << std::endl;
        outfile << "samples = " << width << std::endl;
        outfile << "lines   = " << height << std::endl;
        outfile << "bands   =  " << band << std::endl;
        outfile << "header offset = 0" << std::endl;
        outfile << "file type = ENVI Standard" << std::endl;
        outfile << "data type = 4" << std::endl;
        outfile << "interleave = bsq" << std::endl;
        outfile << "sensor type = unknown" << std::endl;
        outfile << "byte order = 0" << std::endl;
        outfile << "wavelength units = Unknown" << std::endl;
        outfile.close();
    }
    outfile.close();


}
