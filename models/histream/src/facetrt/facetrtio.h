// ==========================================================================
// 文件：facetrtio.h（facetrt）
// 功能：FacetRT 后端各阶段共享的输入/状态结构 FacetrtIO。
// 主要模块：
//   1. 输入路径：inputPath / shaderDirectory / outputPath
//   2. 求解配置 setting（facetvk::Config）
//   3. 阶段完成标志：setup → upload → buffer → descriptor → pipeline → command
// 数据流：Engine 创建 FacetrtIO 并填入路径；Facetrt 与 Buffer/Descriptor/Pipeline/Command 各阶段
//   依次检查前一阶段标志、置本阶段标志；Command 阶段把路径交给 runFacetRTCore。
// 注意：各 *Ready 标志只是阶段状态机，并不表示已创建真实的 Vulkan 资源。
// ==========================================================================

#pragma once

#include <string>

#include "facetrt_vulkan.h"

// 类：FacetrtIO —— FacetRT 一次运行的输入与阶段状态。
// inputPath：工程/场景文件；shaderDirectory：着色器目录；outputPath：结果文件路径；
// setting：Vulkan 求解配置，默认值初始化；*Ready：各阶段是否已完成。
class FacetrtIO {
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

};
