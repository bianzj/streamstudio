#include "pipeline.h"

namespace faceteb_model {

bool Pipeline::createPipeline(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->descriptorReady || modelio->shaderDirectory.empty()) {
        return false;
    }
    modelio->pipelineReady = true;
    return true;
}

void Pipeline::destroy(std::shared_ptr<FacetebIO>& modelio)
{
    if (modelio) {
        modelio->pipelineReady = false;
    }
}

} // namespace faceteb_model
