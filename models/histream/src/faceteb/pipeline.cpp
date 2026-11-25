// ==========================================================================
// 文件：pipeline.cpp（faceteb）
// 功能：实现 FacetEB “管线”阶段：校验描述符阶段已完成且着色器目录非空，并置 pipelineReady。
// 主要模块：
//   1. Pipeline::createPipeline：要求 descriptorReady 且 shaderDirectory 非空
//   2. Pipeline::destroy：pipelineReady = false
// 说明：这里并不编译或加载着色器，只检查路径已设置；着色器由 runFacetEBCore 在运行时使用
//   （待核对）。
// ==========================================================================

#include "pipeline.h"

namespace faceteb_model {

// 作用：管线阶段；前置条件 descriptorReady 且 shaderDirectory 非空，否则返回 false。
// 输出：成功时置 modelio->pipelineReady = true。
bool Pipeline::createPipeline(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->descriptorReady || modelio->shaderDirectory.empty()) {
        return false;
    }
    modelio->pipelineReady = true;
    return true;
}

// 作用：撤销管线阶段标志（modelio 为空时忽略）。
void Pipeline::destroy(std::shared_ptr<FacetebIO>& modelio)
{
    if (modelio) {
        modelio->pipelineReady = false;
    }
}

} // namespace faceteb_model
