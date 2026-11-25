// ==========================================================================
// 文件：descriptor.h（faceteb）
// 功能：FacetEB 后端“描述符”阶段的接口声明（Descriptor 类）。
// 主要模块：
//   1. faceteb_model::Descriptor：createDescriptor / destroy
// 数据流：位于 Buffer 之后、Pipeline 之前；仅在 FacetebIO 上置 descriptorReady 标志。
// 与其他文件的关系：实现见 descriptor.cpp；由 faceteb.cpp 的 Faceteb::create 调用。
// ==========================================================================

#pragma once

#include <memory>

#include "facetebio.h"

namespace faceteb_model {

// 类：Descriptor —— 阶段式生命周期中的“描述符”阶段（仅维护 FacetebIO::descriptorReady）。
class Descriptor {
public:
    bool createDescriptor(std::shared_ptr<FacetebIO>& modelio);
    void destroy(std::shared_ptr<FacetebIO>& modelio);
};

} // namespace faceteb_model
