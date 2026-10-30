#pragma once

#include <memory>

#include "facetebio.h"

namespace faceteb_model {

class Descriptor {
public:
    bool createDescriptor(std::shared_ptr<FacetebIO>& modelio);
    void destroy(std::shared_ptr<FacetebIO>& modelio);
};

} // namespace faceteb_model
