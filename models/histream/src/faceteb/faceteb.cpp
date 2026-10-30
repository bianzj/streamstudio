#include "faceteb.h"

#include "buffer.h"
#include "command.h"
#include "descriptor.h"
#include "pipeline.h"

Faceteb::Faceteb()
{
    m_pBuffer = std::make_shared<faceteb_model::Buffer>();
    m_pDescriptor = std::make_shared<faceteb_model::Descriptor>();
    m_pPipeline = std::make_shared<faceteb_model::Pipeline>();
    m_pCommand = std::make_shared<faceteb_model::Command>();
}

bool Faceteb::setup(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || modelio->inputPath.empty() || modelio->shaderDirectory.empty()) {
        return false;
    }
    modelio->setupReady = true;
    return true;
}

bool Faceteb::upload(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->setupReady) {
        return false;
    }
    modelio->uploadReady = true;
    return true;
}

bool Faceteb::create(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->uploadReady) {
        return false;
    }
    return m_pBuffer->createBuffer(modelio) &&
           m_pDescriptor->createDescriptor(modelio) &&
           m_pPipeline->createPipeline(modelio) &&
           m_pCommand->create(modelio);
}

bool Faceteb::run(std::shared_ptr<FacetebIO>& modelio)
{
    return m_pCommand->runEB(modelio);
}

bool Faceteb::destroy(std::shared_ptr<FacetebIO>& modelio)
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
