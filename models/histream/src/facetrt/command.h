// ==========================================================================
// 文件：command.h（facetrt）
// 功能：FacetRT 后端“命令”阶段的接口声明，并提供实际运行入口 runRT。
// 主要模块：
//   1. facetrt_model::Command：create / runRT / destroy
// 数据流：Facetrt::create 末尾调用 create；Facetrt::run 调用 runRT，runRT 转调 runFacetRTCore。
// 与其他文件的关系：实现见 command.cpp；核心计算在 facetrtcore.cpp；FacetEB 对应版本为
//   faceteb/command.h（多保存一个 exitCode）。
// ==========================================================================

#pragma once

#include <memory>

#include "facetrtio.h"

namespace facetrt_model {

// 类：Command —— “命令”阶段；runRT 是 FacetRT 真正执行计算的唯一入口。
class Command {
public:
    bool create(std::shared_ptr<FacetrtIO>& modelio);
    bool runRT(std::shared_ptr<FacetrtIO>& modelio);
    void destroy(std::shared_ptr<FacetrtIO>& modelio);
};

} // namespace facetrt_model
