#include "buffer.h"

namespace facetrt_model {

bool Buffer::createBuffer(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio || !modelio->uploadReady) {
        return false;
    }
    modelio->bufferReady = true;
    return true;
}

void Buffer::destroy(std::shared_ptr<FacetrtIO>& modelio)
{
    if (modelio) {
        modelio->bufferReady = false;
    }
}

} // namespace facetrt_model
