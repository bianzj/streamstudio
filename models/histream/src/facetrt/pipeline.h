#pragma once

#include <memory>

#include "facetrtio.h"

namespace facetrt_model {

class Pipeline {
public:
    bool createPipeline(std::shared_ptr<FacetrtIO>& modelio);
    void destroy(std::shared_ptr<FacetrtIO>& modelio);
};

} // namespace facetrt_model
