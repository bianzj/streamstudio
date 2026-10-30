#pragma once

#include <memory>

#include "facetebio.h"

namespace faceteb_model {

class Command {
public:
    bool create(std::shared_ptr<FacetebIO>& modelio);
    bool runEB(std::shared_ptr<FacetebIO>& modelio);
    void destroy(std::shared_ptr<FacetebIO>& modelio);
};

} // namespace faceteb_model
