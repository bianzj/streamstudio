// ==========================================================================
// 文件：buffer.h（facetrt）
// 功能：FacetRT 后端“缓冲区”阶段的接口声明（Buffer 类）。
// 主要模块：
//   1. facetrt_model::Buffer：createBuffer / destroy
// 数据流：Facetrt::create 依次调用 createBuffer → createDescriptor → createPipeline → create；
//   本阶段只在 FacetrtIO 上置 bufferReady，不分配真实 GPU 资源。
// 与其他文件的关系：实现见 buffer.cpp；faceteb/buffer.h 是 FacetEB 的对应版本。
// ==========================================================================

#pragma once

#include <memory>

#include "facetrtio.h"

namespace facetrt_model {

// 类：Buffer —— 阶段式生命周期中的“缓冲区”阶段（仅维护 FacetrtIO::bufferReady）。
class Buffer {
public:
    bool createBuffer(std::shared_ptr<FacetrtIO>& modelio);
    void destroy(std::shared_ptr<FacetrtIO>& modelio);
};

} // namespace facetrt_model
