// ==========================================================================
// 文件：pipeline.h（facetrt）
// 功能：FacetRT 后端“管线”阶段的接口声明（Pipeline 类）。
// 主要模块：
//   1. facetrt_model::Pipeline：createPipeline / destroy
// 数据流：位于 Descriptor 之后、Command 之前；仅在 FacetrtIO 上置 pipelineReady 标志。
// 与其他文件的关系：实现见 pipeline.cpp；由 facetrt.cpp 的 Facetrt::create 调用。
// ==========================================================================

#pragma once

#include <memory>

#include "facetrtio.h"

namespace facetrt_model {

// 类：Pipeline —— 阶段式生命周期中的“管线”阶段（仅维护 FacetrtIO::pipelineReady）。
class Pipeline {
public:
    bool createPipeline(std::shared_ptr<FacetrtIO>& modelio);
    void destroy(std::shared_ptr<FacetrtIO>& modelio);
};

} // namespace facetrt_model
