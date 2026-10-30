#include "facetrt.h"

#include "buffer.h"
#include "command.h"
#include "descriptor.h"
#include "pipeline.h"

Facetrt::Facetrt()
{
    m_pBuffer = std::make_shared<facetrt_model::Buffer>();
    m_pDescriptor = std::make_shared<facetrt_model::Descriptor>();
    m_pPipeline = std::make_shared<facetrt_model::Pipeline>();
    m_pCommand = std::make_shared<facetrt_model::Command>();
}

bool Facetrt::setup(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio || modelio->inputPath.empty() || modelio->shaderDirectory.empty()) {
        return false;
    }
    modelio->setupReady = true;
    return true;
}

bool Facetrt::upload(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio || !modelio->setupReady) {
        return false;
    }
    modelio->uploadReady = true;
    return true;
}

bool Facetrt::create(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio || !modelio->uploadReady) {
        return false;
    }
    return m_pBuffer->createBuffer(modelio) &&
           m_pDescriptor->createDescriptor(modelio) &&
           m_pPipeline->createPipeline(modelio) &&
           m_pCommand->create(modelio);
}

bool Facetrt::run(std::shared_ptr<FacetrtIO>& modelio)
{
    return m_pCommand->runRT(modelio);
}

bool Facetrt::destroy(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio) {
        return true;
    }
    m_pCommand->destroy(modelio);
    m_pPipeline->destroy(modelio);
    m_pDescriptor->destroy(modelio);
    m_pBuffer->destroy(modelio);
    return true;
}
