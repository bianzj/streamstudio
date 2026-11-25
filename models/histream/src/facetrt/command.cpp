// ==========================================================================
// 文件：command.cpp（facetrt）
// 功能：实现 FacetRT “命令”阶段，并在 runRT 中调用 FacetRT 核心求解器。
// 主要模块：
//   1. Command::create：要求 pipelineReady，置 commandReady
//   2. Command::runRT：调用 runFacetRTCore，退出码为 0 视为成功
//   3. Command::destroy：commandReady = false
// 数据流：inputPath / shaderDirectory / outputPath（FacetrtIO）→ runFacetRTCore → 布尔结果；
//   FacetrtIO 无 exitCode 字段，Engine::run 只据此返回 0 或 1。
// ==========================================================================

#include "command.h"

#include "facetrtcore.h"

namespace facetrt_model {

// 作用：命令阶段；前置条件 pipelineReady，否则返回 false；成功时置 commandReady。
bool Command::create(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio || !modelio->pipelineReady) {
        return false;
    }
    modelio->commandReady = true;
    return true;
}

// 作用：运行 FacetRT 全流程（读取工程、辐射传输、写结果）。
// 输入：modelio 的 inputPath、shaderDirectory、outputPath（可为空）；
// 输出：runFacetRTCore 返回 0 时为 true，否则 false（核心内部把异常转为返回值 1）。
bool Command::runRT(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio || !modelio->commandReady) {
        return false;
    }
    return runFacetRTCore(modelio->inputPath, modelio->shaderDirectory,
                          modelio->outputPath) == 0;
}

// 作用：撤销命令阶段标志（modelio 为空时忽略）。
void Command::destroy(std::shared_ptr<FacetrtIO>& modelio)
{
    if (modelio) {
        modelio->commandReady = false;
    }
}

} // namespace facetrt_model
