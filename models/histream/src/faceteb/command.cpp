#include "command.h"

#include "../facetrt/facetrtcore.h"

namespace faceteb_model {

bool Command::create(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->pipelineReady) {
        return false;
    }
    modelio->commandReady = true;
    return true;
}

bool Command::runEB(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->commandReady) {
        return false;
    }
    modelio->exitCode = runFacetEBCore(modelio->inputPath, modelio->shaderDirectory,
                                      modelio->outputPath);
    return modelio->exitCode == 0;
}

void Command::destroy(std::shared_ptr<FacetebIO>& modelio)
{
    if (modelio) {
        modelio->commandReady = false;
    }
}

} // namespace faceteb_model
