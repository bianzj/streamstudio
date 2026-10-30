#pragma once

#include <memory>

#include "facetebio.h"

namespace faceteb_model {

class Pipeline {
public:
    bool createPipeline(std::shared_ptr<FacetebIO>& modelio);
    void destroy(std::shared_ptr<FacetebIO>& modelio);
};

} // namespace faceteb_model
