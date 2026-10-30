#pragma once

#include <memory>

#include "facetrtio.h"

namespace facetrt_model {

class Descriptor {
public:
    bool createDescriptor(std::shared_ptr<FacetrtIO>& modelio);
    void destroy(std::shared_ptr<FacetrtIO>& modelio);
};

} // namespace facetrt_model
