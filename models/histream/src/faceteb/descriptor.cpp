#include "descriptor.h"

namespace faceteb_model {

bool Descriptor::createDescriptor(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->bufferReady) {
        return false;
    }
    modelio->descriptorReady = true;
    return true;
}

void Descriptor::destroy(std::shared_ptr<FacetebIO>& modelio)
{
    if (modelio) {
        modelio->descriptorReady = false;
    }
}

} // namespace faceteb_model
