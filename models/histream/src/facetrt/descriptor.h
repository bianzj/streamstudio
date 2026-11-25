// ==========================================================================
// 文件：descriptor.h（facetrt）
// 功能：FacetRT 后端“描述符”阶段的接口声明（Descriptor 类）。
// 主要模块：
//   1. facetrt_model::Descriptor：createDescriptor / destroy
// 数据流：位于 Buffer 之后、Pipeline 之前；仅在 FacetrtIO 上置 descriptorReady 标志。
// 与其他文件的关系：实现见 descriptor.cpp；由 facetrt.cpp 的 Facetrt::create 调用。
// ==========================================================================

#pragma once

#include <memory>

#include "facetrtio.h"

namespace facetrt_model {

// 类：Descriptor —— 阶段式生命周期中的“描述符”阶段（仅维护 FacetrtIO::descriptorReady）。
class Descriptor {
public:
    bool createDescriptor(std::shared_ptr<FacetrtIO>& modelio);
    void destroy(std::shared_ptr<FacetrtIO>& modelio);
};

} // namespace facetrt_model
