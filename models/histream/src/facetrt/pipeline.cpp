#include "pipeline.h"

namespace facetrt_model {

bool Pipeline::createPipeline(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio || !modelio->descriptorReady || modelio->shaderDirectory.empty()) {
        return false;
    }
    modelio->pipelineReady = true;
    return true;
}

void Pipeline::destroy(std::shared_ptr<FacetrtIO>& modelio)
{
    if (modelio) {
        modelio->pipelineReady = false;
    }
}

} // namespace facetrt_model
