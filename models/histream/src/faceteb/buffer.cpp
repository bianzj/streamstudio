#include "buffer.h"

namespace faceteb_model {

bool Buffer::createBuffer(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->uploadReady) {
        return false;
    }
    modelio->bufferReady = true;
    return true;
}

void Buffer::destroy(std::shared_ptr<FacetebIO>& modelio)
{
    if (modelio) {
        modelio->bufferReady = false;
    }
}

} // namespace faceteb_model
