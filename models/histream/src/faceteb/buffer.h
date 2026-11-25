// ==========================================================================
// 文件：buffer.h（faceteb）
// 功能：FacetEB 后端“缓冲区”阶段的接口声明（Buffer 类）。
// 主要模块：
//   1. faceteb_model::Buffer：createBuffer / destroy
// 数据流：Faceteb::create() 依次调用 createBuffer → createDescriptor → createPipeline → create，
//   各阶段只在 FacetebIO 上置对应的 *Ready 标志，不分配真实 GPU 资源。
// 与其他文件的关系：实现见 buffer.cpp；状态载体为 facetebio.h 的 FacetebIO；真正的计算
//   在 facetrt/facetrtcore.cpp 的 runFacetEBCore 中自行创建 Vulkan 资源。
// ==========================================================================

#pragma once

#include <memory>

#include "facetebio.h"

namespace faceteb_model {

// 类：Buffer —— 阶段式生命周期中的“缓冲区”阶段（仅维护 FacetebIO::bufferReady 标志）。
class Buffer {
public:
    bool createBuffer(std::shared_ptr<FacetebIO>& modelio);
    void destroy(std::shared_ptr<FacetebIO>& modelio);
};

} // namespace faceteb_model
