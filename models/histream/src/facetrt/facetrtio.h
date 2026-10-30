#pragma once

#include <string>

#include "facetrt_vulkan.h"

class FacetrtIO {
public:
    std::string inputPath;
    std::string shaderDirectory;
    std::string outputPath;

    facetvk::Config setting{};
    bool setupReady{false};
    bool uploadReady{false};
    bool bufferReady{false};
    bool descriptorReady{false};
    bool pipelineReady{false};
    bool commandReady{false};

};
