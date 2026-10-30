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
#pragma once
//#include "tinyxml.h"
#include "structs.h"
#include "myfunction.h"
#include "stdlib.h"
#include <stack>
#include <string>
#include "xmlexamples.h"

using namespace std;

bool FileIO::readXml(std::string Path, Mode mode) {
    m_mode = mode;
    SAA = 0.0f; // FIXED: SAA 原为未初始化成员，readLightXML 之前若被 BRF 角度使用会读到垃圾值
    m_inputDirectory = Path.empty() ? std::string() : std::filesystem::path(Path).parent_path().string();
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
    static TiXmlDocument mydoc; // tinyxml.h
    mydoc = TiXmlDocument(Path.c_str());
    bool isloadOk = mydoc.LoadFile();
    if (!isloadOk)
    {
        std::cout << "could not load the test file.Error:" << mydoc.ErrorDesc() << std::endl;
        exit(1);
    }
    // FIXED[中] 内存: 成员 RootElement 曾指向局部 mydoc，函数返回后成为悬垂指针（UAF），且空文档时 RootElement 为 nullptr 仍被解引用；已修复：文档改为函数内 static 对象（生命周期覆盖 RootElement 的全部使用），RootElement 判空，为空时报错并返回 false。
    RootElement = mydoc.RootElement(); // root node of xml
    if (RootElement == nullptr)
    {
        std::cerr << "could not find root element in xml: " << Path << std::endl;
        return false;
    }

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


AeroCondXml FileIO::readAeroXML(TiXmlNode *node, Mode mode) {

    AeroCondXml aeroCondXml{};
    // FIXED[高] 错误处理: node/AeroNode 未判空（缺 <Attribute> 或 <Aerodynamics> 时空指针崩溃），各子元素 FirstChildElement(...)->GetText() 未判空，stoi/stof 对非法文本抛未捕获异常；多个子节点时只保留最后一个；无子节点时返回未初始化的 aeroCondXml；已修复：node 与 <Aerodynamics> 判空后返回值初始化的默认结构，循环内逐项判空（缺项时打印元素名并跳过该条），stoi/stof 用 try/catch 包裹，非法文本不再终止进程。本文件 readMeteoXML/readPropertyXML/readThermalXML/readCanopyXML/readSceneXML 的同类链式解引用另行标注。
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

MeteoXml FileIO::readMeteoXML(TiXmlNode *node, Mode mode) {


    MeteoXml meteoxml{};
    MeteoMeta meta{};
    // FIXED[高] 错误处理: node 为空（XML 缺 <Meteorology>，VoxelEB 模式必经）时空指针崩溃，GetText() 返回 nullptr 时构造 std::string 为 UB；已修复：node 判空、每项先取文本再判空（缺失时抛出带字段名的异常并在本函数内捕获打印），数值解析失败同样捕获，返回默认结构而不是崩溃。
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

std::vector<PropertyXml> FileIO::readPropertyXML(TiXmlNode *node, Mode mode) {

    std::vector<PropertyXml> propxmls;
    // FIXED[高] 空指针: 缺 <Attribute> 或 <Biochemistry> 时 node/chemstry_node 为空立即崩溃；已修复：两级均判空后返回空列表。
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
            // FIXED[高] 内存: Tparam 被解析 5 次（每次 mySplitFloat 都泄漏 new[]），且未检查分割结果至少 5 个元素，少于 5 个时 [3]/[4] 越界读；Attribute("name") 返回 nullptr 时赋给 std::string 崩溃（soilSet/waterSet 同）；已修复：Tparam 只解析一次存入局部 vector 并校验 size()>=5（不足时补 0 并告警），leafBio/soilSet/waterSet 的 name 均判空后赋值。
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
            if (TiXmlElement* model = soilNode->FirstChildElement("brdfModel")) {
                // FIXED[中] Bug: 空元素 <brdfModel/> 时 GetText() 返回 nullptr，用其构造 std::string 为 UB/崩溃（下方 waterSet 的 brdfModel 及 hapke*/refractiveIndex 等 stof(GetText()) 同理）；已修复：GetText() 先判空（空文本按非 Hapke 处理），下方 hapke*/refractiveIndex/slopeVariance/diffuseFraction 均先检查文本非空再 stof。
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
            propxmls.push_back(propertyXml2);
////            continue;
//
        }
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
            waterProperty.waterset = WaterSet{
                stof(waterNode->FirstChildElement("rss")->GetText()),
                stof(waterNode->FirstChildElement("heatCapacity")->GetText()),
                stof(waterNode->FirstChildElement("mixingDepth")->GetText()),
                stof(waterNode->FirstChildElement("evaporationCoefficient")->GetText())
            };
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

bool FileIO::sonExists(std::string sonName, TiXmlElement* parentEle)
{
    // FIXED[中] 空指针: 父节点为空（如缺少 <Geometry>/<Control> 等父元素）时直接解引用崩溃；已修复：parentEle 为 nullptr 时返回 false。
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

        const auto parseCurve = [Node](const char *elementName) {
            TiXmlElement *element = Node->FirstChildElement(elementName);
            const char *text = element ? element->GetText() : nullptr;
            if (text == nullptr) return std::vector<float>();
            return myFunction::mySplitFloat(text, ",");
        };

        const std::string type = Node->Attribute("type") ? Node->Attribute("type") : "custom";
        if (type == "custom" || type == "file") {
            TiXmlElement *spectralFileElement = Node->FirstChildElement("spectral_file");
            const char *spectralFile = spectralFileElement ? spectralFileElement->GetText() : nullptr;
            spectralXml.type = type == "file" ? spectralType::OTHER : spectralType::CUSTOM;
            // FIXED[中] 错误处理: type=file 时通常只给 spectral_file，但这里无条件解引用 <reflectance>/<transmittance>，缺失即空指针崩溃（Prospect/BSM 分支同理）；已修复：三条分支统一通过 parseCurve 先判元素与文本是否存在，缺失时保留空曲线并打印波段名告警；path.string() 的 ANSI 转换问题保留待确认。
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

std::vector<ThermalXml> FileIO::readThermalXML(TiXmlNode *node, Mode mode) {
    std::vector<ThermalXml> thermalXmls;
    if (node == nullptr) return thermalXmls;
    TiXmlElement* thermalNode = node->FirstChildElement("Thermal");
    if (thermalNode == nullptr) return thermalXmls;
    // FIXED[中] Bug: 起点按名字 "thermal" 查找，但迭代用无名 NextSiblingElement()，会把 <Thermal> 下其他任意元素当作 thermal 解析；Attribute("name") 为空时构造 std::string 崩溃，子元素缺失时链式解引用崩溃；已修复：迭代改为 NextSiblingElement("thermal")，name 与子元素文本均判空（thermalxml 值初始化，数值解析包 try/catch）。
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

std::vector<CanopyXml> FileIO::readCanopyXML(TiXmlNode *node, Mode mode) {

    std::vector<CanopyXml> CanopyXmls;
    // FIXED[中] 空指针/Bug: node 或 <Canopy> 缺失时空指针崩溃，迭代用无名 NextSiblingElement() 会把 <Canopy> 下其他元素当冠层，Attribute("name") 为空时构造 std::string 崩溃；已修复：两级判空后返回空列表，迭代改为 NextSiblingElement("canopy")，name 判空。
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
        const auto optionalFloat = [Node](const char* name, float fallback) {
            const TiXmlElement* valueNode = Node->FirstChildElement(name);
            return valueNode != nullptr && valueNode->GetText() != nullptr
                ? std::stof(valueNode->GetText()) : fallback;
        };
        // DEFERRED[低] 错误处理: 未识别的 structureType（如拼写为 "Fire"）静默落为 0（普通冠层）而不告警；lai/density/hc/leafwidth 未校验 >0（leafwidth=0 时空气动力学阻抗除零）；建议：未知类型报错，数值做范围检查。
        const int structureTypeId = structureType == "rigid" || structureType == "1" ? 1
            : structureType == "fire" || structureType == "2" ? 2
            : structureType == "fog" || structureType == "3" ? 3 : 0;
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


SensorXml FileIO::readSensorXML(TiXmlNode *node, Mode mode){

    SensorXml sensorxml;
    sensorxml.projection = Projection::PARALLAL;
    sensorxml.position = {0.0f, 0.0f, 3000.0f};
    // FIXED[中] 错误处理: node（<Geometry>）与 sensorEle 未判空，缺失即空指针崩溃；pixelResolutionX/Y、controlBand、viewAngle 均为链式解引用；Attribute("name") 为 nullptr 时赋给 std::string 崩溃；已修复：node/<Sensor> 判空后返回默认 sensorxml，name/projection/分辨率/波段文本与 viewAngle 元素均判空，数值解析包 try/catch（SPP/CSPP/enabled/angleNum 用 readInt 取默认值），缺失项打印带元素名的告警而不是崩溃。
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
    const auto readInt = [&elementText](TiXmlElement *parent, const char *name, int fallback) {
        const char *text = elementText(parent, name);
        if (text == nullptr) return fallback;
        try { return std::stoi(text); } catch (const std::exception &) { return fallback; }
    };

//赋值
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

        if (sensorType.empty() || sensorType == "Multispectral sensor")
        {
            const char* sensorNameText = pEle->Attribute("name");
            if (sensorNameText != nullptr) {
                sensorxml.name = sensorNameText;
            } else {
                std::cerr << "Error: <Sensor> is missing 'name' attribute." << std::endl;
            }
            if (sonExists("projection", pEle))
            {
                // FIXED[低] Bug: <projection/> 为空时 GetText() 返回 nullptr，构造 std::string 崩溃；已修复：GetText() 判空后再构造（空文本保持默认投影）。
                const char* projectionText = pEle->FirstChildElement("projection")->GetText();
                std::string projection = projectionText != nullptr ? projectionText : std::string();
                std::transform(projection.begin(), projection.end(), projection.begin(),
                               [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
                if (projection == "perspective" || projection == "central" || projection == "center")
                {
                    sensorxml.projection = Projection::PERSPECTIVE;
                }
            }
            if (sonExists("sensorPosition", pEle))
            {
                const std::vector<float> position = myFunction::mySplitFloat(
                        pEle->FirstChildElement("sensorPosition")->GetText(), ",");
                if (position.size() >= 3)
                {
                    sensorxml.position = {position[0], position[1], std::max(0.01f, position[2])};
                }
            }
            if (sonExists("FOV", pEle))
            {
                sensorxml.sensorFov = std::clamp(std::stof(pEle->FirstChildElement("FOV")->GetText()), 0.1f, 120.0f);
            }
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

            // FIXED[中] 空指针: 缺 <controlBand> 时 FirstChildElement 返回空指针、<controlBand/> 空文本时 GetText() 返回 nullptr，而 mySplitFloat(const std::string&) 由 nullptr 构造 std::string 即崩溃（上方 587 行的"已判空"注释此前并未落实到本行）；已修复：用同函数已有的 elementText 取文本并判空，缺失/空文本时打印告警、波段保持默认（空）。
            const char *controlBandText = elementText(pEle, "controlBand");
            if (controlBandText != nullptr) {
                sensorxml.waves = myFunction::mySplitFloat(controlBandText, ",");
            } else {
                std::cerr << "Error: <controlBand> is missing or empty for a sensor; spectral bands stay empty." << std::endl;
            }


            TiXmlElement* allViewAngleEle = pEle->FirstChildElement("viewAngle");
            // FIXED[中] 错误处理: subEle->Attribute("type") 为 nullptr 时构造 std::string 崩溃；自定义角度用 mySplitFloat(...)[0]/[1] 未检查元素个数（"30" 只有一个数时 [1] 越界读）且同一文本解析两次；已修复：type 判空后构造（缺失时按空类型跳过），自定义角度只解析一次并校验 size()>=2（不足时打印告警并跳过该组），viewAngle 元素缺失时循环直接不执行。
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
                    // FIXED[高] Bug: vzaStep<=0 时 for (k=0; k<=VzaMax; k+=vzaStep) 死循环并无限 push_back 直至内存耗尽；另外 SAA 是未初始化成员；已修复：SPP/CSPP/vzaMax/vzaStep 改用 readInt（缺失或非法时取 0，不再 stoi(nullptr)），并校验 VzaMax>0 且 vzaStep>0，非法时打印错误并跳过该 BRF 角度集（两个分支均已处理）；SAA 在 readXml 入口初始化为 0，readLightXML 会写入真实值。
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
                        // FIXED[中] 数值: hemiAngleNum<=0 时 sqrt(pi/k0)、360/k00 除零；迭代中 k1 由 float 截断为 int，环数增加后 k1 先变为 0 而不是 <0，下一轮 k0=0 同样除零得到 inf/NaN 角度并静默写入 viewAngles，break 条件 k1<0 几乎不会触发；pi 仅取 7 位；已修复：仅在该角度集 hemiAngleNum>0（且 enabled 解析成功）时才生成半球角度，否则跳过该角度集；break 条件由 k1<0 改为 k1<=0，避免 k0 归零后继续除零；enabled/angleNum 改用 readInt（缺失/非法不再崩溃）。pi 取值精度属数值常量，未改动（待确认）。下方高光谱分支同一段代码同样修复。
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
            // FIXED[中] 错误处理: bandsFilePath 未按 m_inputDirectory 解析相对路径（spectral_file 却会），且用窄字符 ifstream 打开；文件打不开时只打印一行仍继续，waves 为空，后续按波段数分配/循环为 0 或越界；已修复：相对路径先拼接输入目录，路径按 UTF-8（u8path）解释后交给 ifstream（Windows 下最终走宽字符 API），元素或文本缺失、文件打不开、读完 waves 仍为空时打印带路径的错误并跳过该传感器，避免空波段继续传播。
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
                        // FIXED[中] 数值: hemiAngleNum<=0 时 sqrt(pi/k0)、360/k00 除零；迭代中 k1 由 float 截断为 int，环数增加后 k1 先变为 0 而不是 <0，下一轮 k0=0 同样除零得到 inf/NaN 角度并静默写入 viewAngles，break 条件 k1<0 几乎不会触发；pi 仅取 7 位；已修复：仅在该角度集 hemiAngleNum>0（且 enabled 解析成功）时才生成半球角度，否则跳过该角度集；break 条件由 k1<0 改为 k1<=0，避免 k0 归零后继续除零；enabled/angleNum 改用 readInt（缺失/非法不再崩溃）。pi 取值精度属数值常量，未改动（待确认）。
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

    // FIXED[中] 错误处理: 成员 RootElement 依赖 readXml 中局部 mydoc 存活，且 controlnode 为空（缺 <Control>）时 ->ToElement() 空指针崩溃；已修复：readXml 的文档改为 static 保证生命周期，此处 RootElement/controlnode 均判空（sonExists 也已内部判空），缺 <Control> 时跳过这些开关项。
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
LightXml FileIO::readLightXML(TiXmlNode *geometryNode, Mode mode){

    LightXml lightxml;
    AtomCondXml atomCondXml;
    // FIXED[中] 空指针: geometryNode（缺 <Geometry>）与 lightEle（缺 <Light>）均未判空即链式解引用（静态复核发现，原文件未标注）；已修复：两级均判空并打印告警后返回默认 lightxml，不再崩溃。
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

        lightxml.name = pEle->Attribute("name");

//        temp.isSun = stoi(pEle->FirstChildElement("isSun")->GetText());
//        glm::vec2 sunAnglesk;
        TiXmlElement* lightAngleEle = pEle->FirstChildElement("lightAngle");
        for (TiXmlElement* lightAngleIter = lightAngleEle->FirstChildElement(); lightAngleIter != NULL; lightAngleIter = lightAngleIter->NextSiblingElement())
        {

            nvmath::vec2f lightAngleTemp = { myFunction::mySplitFloat(lightAngleIter->GetText(), ",")[0],
                                             myFunction::mySplitFloat(lightAngleIter->GetText(), ",")[1] };
            lightxml.solarAngle = {lightAngleTemp[0], lightAngleTemp[1]};
            SAA = lightAngleTemp[1];

        }

        lightxml.skyTemperature = stof(pEle->FirstChildElement("skyTemperature")->GetText());
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



SettingXml FileIO::readSettingXML(TiXmlNode *controlNode, Mode mode){

    SettingXml settingxml;
    settingxml.maxDepth = stoi(controlNode->FirstChildElement("rayTracingDepth")->GetText());
    settingxml.theGPU = stoi(controlNode->FirstChildElement("GPU")->GetText());
    if (sonExists("computeBackend", controlNode->ToElement())) {
        const char* backend = controlNode->FirstChildElement("computeBackend")->GetText();
        if (backend) settingxml.computeBackend = backend;
    }
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

    if (sonExists("voxelFillThreshold", sceneNode->ToElement())) {
        sceneXml.background.voxelFillThreshold = std::clamp(
            stof(sceneNode->FirstChildElement("voxelFillThreshold")->GetText()),
            0.0f, 1.0f);
    }

    // FIXED[中] Bug: 判断的是 bgBioType 是否存在，却无条件解引用 bgBioName；只给 bgBioType 而缺 bgBioName 时空指针崩溃；两个 GetText() 也未判空；已修复：bgBioName 与 bgBioType 分别查元素并判空文本，缺 bgBioName 时沿用默认背景材质名 "soilset"，缺 bgBioType 时保持默认背景类型。
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

    // FIXED[中] 空指针: sonExists 只判断元素是否存在，<bgSpectral/> 为空文本时 GetText() 返回 nullptr，赋给 std::string 成员 bgSpectralName 即崩溃（静态复核发现，原文件未标注）；已修复：照下方 bgThermal 的既有写法先判 GetText() 非空再赋值，空文本保持默认背景光谱名。
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
    TiXmlElement* controlElement = RootElement
        ? RootElement->FirstChildElement("Control") : nullptr;
    if (controlElement && sonExists("isDEM", controlElement)) {
        const char* enabledText =
            controlElement->FirstChildElement("isDEM")->GetText();
        demEnabled = enabledText && std::atoi(enabledText) != 0;
    }

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

void FileIO::readMeteo(std::shared_ptr<DefinedIO> &defineio,int & n_node,
                       std::vector<Meteo> &meteos, std::vector<AtomCond> &wavesets) {


    auto & meteofile = m_pVoxelebXml->meteoxml.meteofile;



    // DEFERRED[低] 错误处理: 窄字符 ifstream 打开（中文路径失败），且未区分"打不开"与"空文件"，打不开时报 "Meteorology file is empty" 误导排查；meteofile 相对路径未拼接 m_inputDirectory；建议：u8path 打开并先检查 is_open()，相对路径按输入目录解析。
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

        meteos.emplace_back(mi);
        ++rowIndex;
        //m_meteoParams.emplace_back(mi);
    }
    if (meteos.size() < static_cast<size_t>(fileNodeCount)) {
        throw std::runtime_error("Meteorology file contains fewer rows than its header");
    }
    n_node = static_cast<int>(meteos.size());

    ///-----------------------------------------------------------------------------
    ///   WaveLength and atmospheric condition
    ///----------------------------------------------------------------------------

    // DEFERRED[低] 可维护性: 波长网格以字面量 2001/126/35 硬编码，只在 N1=2001、N2=161 时恰好填满 wavesets；N1/N2 宏一旦调整即越界写或留下未初始化波长；建议：以 N1、N2 推导循环上界并 static_assert(N1==2001 && N2==126+35)。
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
    const auto esun_ = readIncidentSpectrum(m_pVoxelebXml->atomcondxml.rinfile, N1 + N2);
    const auto esky_ = readIncidentSpectrum(m_pVoxelebXml->atomcondxml.rlifile, N1 + N2);
    std::vector<float> fesky_(N1 + N2), fesun_(N1 + N2);

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
    const float directFraction = m_pVoxelebXml->lightxml.direct;
    for (int i = 0; i < b1; i++)
    {
        const float endpointWeight = i == 0 || i == b1 - 1 ? 0.5f : 1.0f;
        fesky_[i] = endpointWeight * normalizedShortwaveCoefficient(esky_[i], TsEsky, 1.0f - directFraction);
        fesun_[i] = endpointWeight * normalizedShortwaveCoefficient(esun_[i], TsEsun, directFraction);
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

bool FileIO::writeTIFData(const std::string& projectDir, const float* pData, int width, int height,
                          int band, const Angle& angle, float t, const std::string& modelSuffix,
                          int positionIndex, bool flipRows,
                          const std::vector<std::string>& bandNames) {
    if (pData == nullptr || width <= 0 || height <= 0 || band <= 0) {
        std::cerr << "Invalid GeoTIFF output parameters." << std::endl;
        return false;
    }

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
