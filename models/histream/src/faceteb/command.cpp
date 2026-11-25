// ==========================================================================
// 文件：command.cpp（faceteb）
// 功能：实现 FacetEB “命令”阶段，并在 runEB 中调用 FacetEB 核心求解器。
// 主要模块：
//   1. Command::create：要求 pipelineReady，置 commandReady
//   2. Command::runEB：调用 runFacetEBCore，保存退出码到 FacetebIO::exitCode
//   3. Command::destroy：commandReady = false
// 数据流：inputPath / shaderDirectory / outputPath（FacetebIO）→ runFacetEBCore → exitCode；
//   Engine::run 之后直接读取 exitCode 作为进程退出码。
// 约定：退出码 0 = 全部收敛；2 = 已完成但存在未收敛节点（结果保留）；
//   3 = CPU/GPU 能量审计不一致；1 = 失败（见 facetrtcore.cpp 的 runFacetEBCore 返回值）。
// ==========================================================================

#include "command.h"

#include "../facetrt/facetrtcore.h"

namespace faceteb_model {

// 作用：命令阶段；前置条件 pipelineReady，否则返回 false；成功时置 commandReady。
bool Command::create(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->pipelineReady) {
        return false;
    }
    modelio->commandReady = true;
    return true;
}

// 作用：运行 FacetEB 全流程（读取工程、辐射与能量平衡迭代、写结果）。
// 输入：modelio 的 inputPath（工程文件）、shaderDirectory（着色器目录）、outputPath（可为空）。
// 输出：返回 exitCode == 0；注意退出码 2（未收敛）和 3 会使返回值为 false，
//   但 exitCode 已原样保存，Engine::run 据此向上层报告。
bool Command::runEB(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->commandReady) {
        return false;
    }
    modelio->exitCode = runFacetEBCore(modelio->inputPath, modelio->shaderDirectory,
                                      modelio->outputPath);
    return modelio->exitCode == 0;
}

// 作用：撤销命令阶段标志（modelio 为空时忽略）；不清除 exitCode。
void Command::destroy(std::shared_ptr<FacetebIO>& modelio)
{
    if (modelio) {
        modelio->commandReady = false;
    }
}

} // namespace faceteb_model
