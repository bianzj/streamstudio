#pragma once

#include <string>

#include "../facetrt/facetrt_vulkan.h"

class FacetebIO {
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
    // Preserve the core result: 2 denotes retained, non-converged output.
    int exitCode{1};

};
