// ==========================================================================
// 文件：image.h
// 功能：STREAM 影像引擎计算链路（image.cpp）的对外入口声明
// 主要模块：
//   1. runImage：读取 stream-image-config v2 配置并完成一次过境时刻的整景模拟
// 数据流：main.cpp 的 stream run <config.json> 调用 runImage → image.cpp 完成成像/EB/同化/反演
// 注意：返回值作为进程退出码（0 表示成功，具体含义见 image.cpp 中 runImage 的实现）
// ==========================================================================

#pragma once
#include <string>

namespace stream {

// 作用：执行一次 STREAM 影像运行（配置路径为 UTF-8 的 JSON 文件）。
// 输入：stream-image-config v2 配置文件路径；输出：写出各 GeoTIFF 与 scene_summary.json，返回退出码。
int runImage(const std::string& configPath);

}  // namespace stream
