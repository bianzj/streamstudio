// ==========================================================================
// 文件：faceteb.h
// 功能：FacetEB（面元能量平衡）后端的对外门面类 Faceteb 的声明。
// 主要模块：
//   1. 前置声明 faceteb_model::Buffer / Descriptor / Pipeline / Command
//   2. Faceteb：setup / upload / create / run / destroy 五个生命周期接口
// 数据流：Engine（base/engine.cpp）在 eFacetEB 模式下持有 Faceteb 与 FacetebIO，
//   init 阶段依次调用 setup → upload → create，run 阶段调用 run，结束时调用 destroy。
// 与其他文件的关系：实现见 faceteb.cpp；各阶段子对象见同目录 buffer/descriptor/pipeline/command；
//   真正的物理计算在 facetrt/facetrtcore.cpp（runFacetEBCore）。
// ==========================================================================

#pragma once

#include <memory>

#include "facetebio.h"

namespace faceteb_model {
class Buffer;
class Command;
class Descriptor;
class Pipeline;
}

// 类：Faceteb —— FacetEB 后端门面；沿用 VoxelEB/VoxelRT 等后端的阶段式接口，
//   以便 Engine 统一调度。所有接口接收共享的 FacetebIO 并返回是否成功。
class Faceteb {
public:
    Faceteb();

    bool setup(std::shared_ptr<FacetebIO>& modelio);
    bool upload(std::shared_ptr<FacetebIO>& modelio);
    bool create(std::shared_ptr<FacetebIO>& modelio);
    bool run(std::shared_ptr<FacetebIO>& modelio);
    bool destroy(std::shared_ptr<FacetebIO>& modelio);

private:
    std::shared_ptr<faceteb_model::Buffer> m_pBuffer;
    std::shared_ptr<faceteb_model::Descriptor> m_pDescriptor;
    std::shared_ptr<faceteb_model::Pipeline> m_pPipeline;
    std::shared_ptr<faceteb_model::Command> m_pCommand;
};
