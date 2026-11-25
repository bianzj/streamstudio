// ==========================================================================
// 文件：facetebio.h（faceteb）
// 功能：FacetEB 后端的“输入/输出与阶段状态”载体 FacetebIO。
// 主要模块：
//   1. 路径字段：inputPath（工程文件）、shaderDirectory（着色器目录）、outputPath（结果）
//   2. setting：facetvk::Config（来自 facetrt_vulkan.h 的 Vulkan 配置）
//   3. 阶段标志：setup/upload/buffer/descriptor/pipeline/command 的 *Ready
//   4. exitCode：保存 runFacetEBCore 的返回值
// 数据流：Engine::input 填写三个路径 → Faceteb::setup/upload/create 依次置标志 →
//   Command::runEB 写入 exitCode → Engine::run 返回该值。
// 约定：exitCode 初值 1（失败）；0 = 收敛，2 = 完成但有未收敛节点（结果保留），
//   3 = CPU/GPU 能量审计不一致（见 facetrtcore.cpp 的 runFacetEBCore）。
// 与其他文件的关系：对应 FacetRT 的 facetrt/facetrtio.h（无 exitCode 字段）。
// ==========================================================================

#pragma once

#include <string>

#include "../facetrt/facetrt_vulkan.h"

// 类：FacetebIO —— 在 Faceteb 各阶段对象之间共享的状态与路径；阶段按 setup → upload →
//   buffer → descriptor → pipeline → command 顺序依次将前一个标志作为前置条件。
class FacetebIO {
public:
    std::string inputPath;
    std::string shaderDirectory;
    std::string outputPath;

    facetvk::Config setting{};
    bool setupReady{false};
    bool uploadReady{false};
    bool bufferReady{false};
    bool descriptorReady{false};
    bool pipelineReady{false};
    bool commandReady{false};
    // 说明：exitCode 初值 1 表示尚未成功运行；Command::runEB 会覆盖为核心求解器的返回值。
    // Preserve the core result: 2 denotes retained, non-converged output.
    int exitCode{1};

};
