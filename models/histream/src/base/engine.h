//
// Created by admin on 2024/1/25.
//

#ifndef FIELD_ENGINE_H
#define FIELD_ENGINE_H

#include "src/raytracing/raytracingio.h"
#include "src/raytracing/raytracing.h"
#include "src/voxeleb/voxeleb.h"
#include "src/voxeleb/voxelebio.h"
#include "src/voxelrt/voxelrt.h"
#include "src/base/appsetting.h"
#include "src/base/fileio.h"
// CMake defines HISTREAM_ENABLE_FACET=0 when the Facet backends are not compiled
// (option HISTREAM_ENABLE_FACET=OFF); other build setups keep them enabled.
#ifndef HISTREAM_ENABLE_FACET
#define HISTREAM_ENABLE_FACET 1
#endif
#if HISTREAM_ENABLE_FACET
#include "../facetrt/facetrt.h"
#include "../faceteb/faceteb.h"
#endif

#include <filesystem>
#include <utility>




class Engine {
public:
    Engine(){};

    void input(std::string path, std::string V, std::string outputPath = {});
    void init(Mode mode);
    bool create();
    int run();
    void destroy();

private:
    std::string facetShaderDirectory(const char* name) const;

    Mode m_mode{};
    std::string m_inputPath;
    std::string m_outputPath;
    AppSetting appSetting;
    std::shared_ptr<FileIO>       m_pFileio;
    std::shared_ptr<Raytracing>   m_pRaytracing;
    std::shared_ptr<RaytracingIO> m_pRaytracingio;
    std::shared_ptr<Voxeleb>    m_pVoxeleb;
    std::shared_ptr<VoxelebIO>  m_pVoxelebio;
    std::shared_ptr<Voxelrt>    m_pVoxelrt;
    std::shared_ptr<VoxelrtIO>  m_pVoxelrtio;
#if HISTREAM_ENABLE_FACET
    std::shared_ptr<Facetrt>    m_pFacetrt;
    std::shared_ptr<FacetrtIO>  m_pFacetrtio;
    std::shared_ptr<Faceteb>    m_pFaceteb;
    std::shared_ptr<FacetebIO>  m_pFacetebio;
#endif

};


#endif //FIELD_ENGINE_H
