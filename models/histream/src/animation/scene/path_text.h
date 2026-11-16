#pragma once
// UTF-8 path helpers shared by the scene-animation modules.
//
// std::filesystem::path::string() returns narrow characters in the local code page on Windows, which is not UTF-8:
// nlohmann::json throws "invalid UTF-8 byte" when such a string is serialised, and GDAL expects UTF-8 file names.
// These helpers keep path text in JSON, receipts, messages and GDAL calls UTF-8 regardless of the code page.
// They are inline so that every module (including the small fixtures that link only part of the animation library)
// can use them without pulling in the mesh-scene translation unit and its OBJ loader dependency.
#include <filesystem>
#include <string>

namespace animation {

inline std::string pathText(const std::filesystem::path& value) {
    const auto text = value.u8string();
    return std::string(text.begin(), text.end());
}

inline std::filesystem::path pathFrom(const std::string& value) {
    return std::filesystem::u8path(value);
}

}  // namespace animation
