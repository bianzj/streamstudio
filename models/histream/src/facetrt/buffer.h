#pragma once

#include <memory>

#include "facetrtio.h"

namespace facetrt_model {

class Buffer {
public:
    bool createBuffer(std::shared_ptr<FacetrtIO>& modelio);
    void destroy(std::shared_ptr<FacetrtIO>& modelio);
};

} // namespace facetrt_model
