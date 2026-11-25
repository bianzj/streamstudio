// ==========================================================================
// 文件：forcing_source.cpp
// 功能：ForcingReport 的去重写入实现（warn / info）
// 说明：同一条文本只记录一次，避免逐像元/逐变量重复产生相同警告刷屏
// 与其他文件：接口定义见 forcing_source.h；各强迫源（era5.cpp、station_source.cpp）调用
// ==========================================================================

#include "forcing_source.h"

#include <algorithm>

namespace stream {

// 作用：加入一条用户可见警告（已存在则忽略）。线性查找，警告条数很少故无需哈希。
void ForcingReport::warn(const std::string& text)
{
    if (std::find(warnings.begin(), warnings.end(), text) == warnings.end()) warnings.push_back(text);
}

void ForcingReport::info(const std::string& text)
{
    if (std::find(infos.begin(), infos.end(), text) == infos.end()) infos.push_back(text);
}

}  // namespace stream
