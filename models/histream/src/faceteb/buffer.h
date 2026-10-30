#pragma once

#include <memory>

#include "facetebio.h"

namespace faceteb_model {

class Buffer {
public:
    bool createBuffer(std::shared_ptr<FacetebIO>& modelio);
    void destroy(std::shared_ptr<FacetebIO>& modelio);
};

} // namespace faceteb_model
