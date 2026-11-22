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

class FileIO {

public:
    FileIO(){};

    bool readXml(std::string path, Mode mode);
    bool readJson(const std::string& path, Mode mode);

    //bool readRayTracingXML(std::string inputPath){ return true;};
    //void writeTif(std::string outDir,void *images,glm::vec4 angles, std::vector<float> bands, glm::vec2 resolution);



public:

    std::shared_ptr<RaytracingXml> m_pRaytracingXml;
    std::shared_ptr<VoxelEBXml>   m_pVoxelebXml;
    std::shared_ptr<VoxelRTXml>   m_pVoxelrtXml;

//private:
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

    bool sonExists(std::string sonName, TiXmlElement* parentEle);
    void readDefined(std::shared_ptr<DefinedIO> & defineio);
    void readMeteo(std::shared_ptr<DefinedIO> &defineio,int & n_node, std::vector<Meteo> &meteos, std::vector<AtomCond> &wavesets);
    // Filled by readMeteo for light.diffuseFractionMode = erbs: optional
    // columns 8-9 (horizontal direct, diffuse W m-2; NaN when absent) per row,
    // and the VNIR Esun/Esky coefficients normalized to fraction 1 (direct in
    // .direct, diffuse in .diffuse) that a node's split scales.
    std::vector<std::array<float, 2>> m_meteoMeasuredSplit;
    std::vector<AtomCond> m_unitShortwaveShapes;
    void writeENVIdata(std::string projectDir,float * pData, int width, int height, int band, Angle &angle, float t = -1);
    void writeENVIdata(std::string projectDir,float * pData, int width, int height, int band, Angle &angle, float t = -1,int k = -1);
    bool writeTIFData(const std::string& projectDir, const float* pData, int width, int height,
                      int band, const Angle& angle, float t, const std::string& modelSuffix,
                      int positionIndex = -1, bool flipRows = false,
                      const std::vector<std::string>& bandNames = {});

    TiXmlElement *RootElement;
    Mode m_mode;
    float SAA;
    std::string m_inputDirectory;

    std::vector<float> outImage1;
    std::vector<std::vector<float>> outImage;
    std::vector<std::vector<float>> outImage_orth;
    std::vector<float> outImageMeanValue;

    XmlExamples xmlexamples;

private:
    MeteoXml m_meteoXml;
    // A FileIO instance belongs to one simulation run. The first raster for
    // each model replaces stale statistics; subsequent rasters append.
    std::unordered_set<std::string> m_initializedStatisticsPaths;


};


#endif //FIELD_FILEIO_H
