#include "forcing_source.h"

#include <algorithm>

namespace stream {

void ForcingReport::warn(const std::string& text)
{
    if (std::find(warnings.begin(), warnings.end(), text) == warnings.end()) warnings.push_back(text);
}

void ForcingReport::info(const std::string& text)
{
    if (std::find(infos.begin(), infos.end(), text) == infos.end()) infos.push_back(text);
}

}  // namespace stream
