// ==========================================================================
// 文件：facetrt.cpp
// 功能：Facetrt 的实现：把 FacetRT 模式的生命周期调度到 Buffer / Descriptor / Pipeline / Command。
// 主要步骤：
//   1. 构造：创建四个子模块对象（facetrt_model::Buffer/Descriptor/Pipeline/Command）
//   2. setup：检查输入路径与着色器目录非空，置 setupReady
//   3. upload：要求 setup 已完成，置 uploadReady（本文件内不做实际数据上传）
//   4. create：依次创建缓冲、描述符、管线与命令（任一步失败即返回 false）
//   5. run：委托 Command::runRT 执行 FacetRT 计算
//   6. destroy：按创建的逆序释放命令、管线、描述符、缓冲
// 数据流：engine.cpp 持有 Facetrt 与 FacetrtIO，按 setup → upload → create → run → destroy 调用。
// 注意：各阶段用 FacetrtIO 的 setupReady/uploadReady 标志串联先后顺序，前一阶段未完成则返回 false。
// ==========================================================================

#include "facetrt.h"

#include "buffer.h"
#include "command.h"
#include "descriptor.h"
#include "pipeline.h"

// 构造：只创建子模块对象，不分配 Vulkan 资源（资源在 create 阶段创建）。
Facetrt::Facetrt()
{
    m_pBuffer = std::make_shared<facetrt_model::Buffer>();
    m_pDescriptor = std::make_shared<facetrt_model::Descriptor>();
    m_pPipeline = std::make_shared<facetrt_model::Pipeline>();
    m_pCommand = std::make_shared<facetrt_model::Command>();
}

// setup：校验必需输入（inputPath、shaderDirectory）；通过后置 setupReady。
bool Facetrt::setup(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio || modelio->inputPath.empty() || modelio->shaderDirectory.empty()) {
        return false;
    }
    modelio->setupReady = true;
    return true;
}

// upload：仅检查 setup 已完成并置 uploadReady；实际 GPU 数据在 create 的 Buffer 阶段处理（待核对）。
bool Facetrt::upload(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio || !modelio->setupReady) {
        return false;
    }
    modelio->uploadReady = true;
    return true;
}

// create：要求 upload 已完成；按缓冲 → 描述符 → 管线 → 命令的依赖顺序创建，
// && 短路保证前一步失败时不再执行后续步骤。
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

// run：执行一次 FacetRT 计算，由 Command::runRT 完成，返回其成功与否。
bool Facetrt::run(std::shared_ptr<FacetrtIO>& modelio)
{
    return m_pCommand->runRT(modelio);
}

// destroy：modelio 为空时视为无事可做；否则按创建的逆序释放（命令依赖管线/描述符/缓冲），
// 始终返回 true。
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
