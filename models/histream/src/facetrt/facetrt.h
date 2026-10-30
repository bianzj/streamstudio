#pragma once

#include <memory>

#include "facetrtio.h"

namespace facetrt_model {
class Buffer;
class Command;
class Descriptor;
class Pipeline;
}

class Facetrt {
public:
    Facetrt();

    bool setup(std::shared_ptr<FacetrtIO>& modelio);
    bool upload(std::shared_ptr<FacetrtIO>& modelio);
    bool create(std::shared_ptr<FacetrtIO>& modelio);
    bool run(std::shared_ptr<FacetrtIO>& modelio);
    bool destroy(std::shared_ptr<FacetrtIO>& modelio);

private:
    std::shared_ptr<facetrt_model::Buffer> m_pBuffer;
    std::shared_ptr<facetrt_model::Descriptor> m_pDescriptor;
    std::shared_ptr<facetrt_model::Pipeline> m_pPipeline;
    std::shared_ptr<facetrt_model::Command> m_pCommand;
};
