// ==========================================================================
// 文件：facetrtcore.h（facetrt）
// 功能：FacetRT / FacetEB 面元求解核心的对外入口声明（实现见 facetrtcore.cpp）。
// 主要模块：
//   1. runFacetRTCore：面元辐射传输（FacetRT）整次运行
//   2. runFacetEBCore：面元辐射-能量平衡耦合（FacetEB）整次运行
// 数据流：调用方（facetrt/command.cpp、faceteb/command.cpp）传入工程文件路径、着色器目录与
//   输出文件路径，核心读场景、求解并写结果文件；返回值为退出码（0 成功；失败/未收敛的取值见
//   facetrtcore.cpp 中各函数的返回语句，FacetEB 另有 2、3 等值）。
// 注意：outputFile 缺省为空串，此时输出位置由核心按工程约定决定（待核对）。
// ==========================================================================

#pragma once

#include <string>

// 作用：FacetRT 入口。inputPath：工程/场景文件；shaderDirectory：着色器目录；
// outputFile：结果文件路径（可空）。返回退出码，0 表示成功。
int runFacetRTCore(const std::string& inputPath,
                   const std::string& shaderDirectory,
                   const std::string& outputFile = {});

// 作用：FacetEB 入口（辐射传输 + 能量平衡逐时间节点耦合）。参数同上。
// 返回退出码：0 全部节点收敛；2 存在未收敛节点；3 CPU/GPU 能量审计不一致；1 失败。
int runFacetEBCore(const std::string& inputPath,
                   const std::string& shaderDirectory,
                   const std::string& outputFile = {});
