// ==========================================================================
// 文件：facetrt.h
// 功能：声明 FacetRT/FacetEB（面元辐射/能量平衡）模式的顶层模型类 Facetrt。
// 主要模块：
//   1. 前置声明 facetrt_model 命名空间下的 Buffer、Descriptor、Pipeline、Command
//   2. Facetrt 类：setup → upload → create → run → destroy 五个生命周期接口
// 数据流：所有接口共用同一个 FacetrtIO（facetrtio.h）：输入路径、着色器目录等由调用方填入，阶段标志
//   setupReady/uploadReady 由本类写回。
// 与其他文件：facetrt.cpp 实现本类；engine.cpp 在 Mode::eFacetRT 下创建并依次调用这些接口。
// ==========================================================================

#pragma once

#include <memory>

#include "facetrtio.h"

namespace facetrt_model {
class Buffer;
class Command;
class Descriptor;
class Pipeline;
}

// 类：FacetRT 模式门面。持有 Vulkan 资源四件套（缓冲、描述符、管线、命令）的共享指针，
// 接口均以 FacetrtIO 为输入/输出，返回 true 表示成功。
class Facetrt {
public:
    Facetrt();

    bool setup(std::shared_ptr<FacetrtIO>& modelio);
    bool upload(std::shared_ptr<FacetrtIO>& modelio);
    bool create(std::shared_ptr<FacetrtIO>& modelio);
    bool run(std::shared_ptr<FacetrtIO>& modelio);
    bool destroy(std::shared_ptr<FacetrtIO>& modelio);

private:
    std::shared_ptr<facetrt_model::Buffer> m_pBuffer;
    std::shared_ptr<facetrt_model::Descriptor> m_pDescriptor;
    std::shared_ptr<facetrt_model::Pipeline> m_pPipeline;
    std::shared_ptr<facetrt_model::Command> m_pCommand;
};
