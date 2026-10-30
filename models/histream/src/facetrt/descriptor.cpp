#include "descriptor.h"

namespace facetrt_model {

bool Descriptor::createDescriptor(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio || !modelio->bufferReady) {
        return false;
    }
    modelio->descriptorReady = true;
    return true;
}

void Descriptor::destroy(std::shared_ptr<FacetrtIO>& modelio)
{
    if (modelio) {
        modelio->descriptorReady = false;
    }
}

} // namespace facetrt_model
