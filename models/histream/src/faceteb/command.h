// ==========================================================================
// 文件：command.h（faceteb）
// 功能：FacetEB 后端“命令”阶段的接口声明，并提供实际运行入口 runEB。
// 主要模块：
//   1. faceteb_model::Command：create / runEB / destroy
// 数据流：Faceteb::create 末尾调用 create；Faceteb::run 调用 runEB，runEB 转调
//   runFacetEBCore 并把退出码写入 FacetebIO::exitCode。
// 与其他文件的关系：实现见 command.cpp；核心计算在 facetrt/facetrtcore.cpp。
// ==========================================================================

#pragma once

#include <memory>

#include "facetebio.h"

namespace faceteb_model {

// 类：Command —— “命令”阶段；runEB 是 FacetEB 真正执行计算的唯一入口。
class Command {
public:
    bool create(std::shared_ptr<FacetebIO>& modelio);
    bool runEB(std::shared_ptr<FacetebIO>& modelio);
    void destroy(std::shared_ptr<FacetebIO>& modelio);
};

} // namespace faceteb_model
