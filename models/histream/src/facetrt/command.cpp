#include "command.h"

#include "facetrtcore.h"

namespace facetrt_model {

bool Command::create(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio || !modelio->pipelineReady) {
        return false;
    }
    modelio->commandReady = true;
    return true;
}

bool Command::runRT(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio || !modelio->commandReady) {
        return false;
    }
    return runFacetRTCore(modelio->inputPath, modelio->shaderDirectory,
                          modelio->outputPath) == 0;
}

void Command::destroy(std::shared_ptr<FacetrtIO>& modelio)
{
    if (modelio) {
        modelio->commandReady = false;
    }
}

} // namespace facetrt_model
