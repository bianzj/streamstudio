#pragma once

#include <memory>

#include "facetrtio.h"

namespace facetrt_model {

class Command {
public:
    bool create(std::shared_ptr<FacetrtIO>& modelio);
    bool runRT(std::shared_ptr<FacetrtIO>& modelio);
    void destroy(std::shared_ptr<FacetrtIO>& modelio);
};

} // namespace facetrt_model
