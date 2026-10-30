#pragma once

#include <memory>

#include "facetebio.h"

namespace faceteb_model {
class Buffer;
class Command;
class Descriptor;
class Pipeline;
}

class Faceteb {
public:
    Faceteb();

    bool setup(std::shared_ptr<FacetebIO>& modelio);
    bool upload(std::shared_ptr<FacetebIO>& modelio);
    bool create(std::shared_ptr<FacetebIO>& modelio);
    bool run(std::shared_ptr<FacetebIO>& modelio);
    bool destroy(std::shared_ptr<FacetebIO>& modelio);

private:
    std::shared_ptr<faceteb_model::Buffer> m_pBuffer;
    std::shared_ptr<faceteb_model::Descriptor> m_pDescriptor;
    std::shared_ptr<faceteb_model::Pipeline> m_pPipeline;
    std::shared_ptr<faceteb_model::Command> m_pCommand;
};
