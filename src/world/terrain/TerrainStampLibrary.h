#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

inline constexpr size_t kMaxTerrainStampTextures = 64;
// 512 gives a 64 m stamp a sample every 12.5 cm, comfortably finer than the
// 0.5 m/vertex the clipmap's innermost ring can render, so the atlas is no
// longer what limits stamp detail. Costs 64 * 512^2 * 2B = 33.5 MB of upload
// heap. At 256 a large stamp was resolved more coarsely than the terrain mesh
// could draw it, which is what made authored ridges read as blobs.
inline constexpr uint32_t kTerrainStampResolution = 512;

inline const std::filesystem::path& TerrainStampDirectory() {
    static const std::filesystem::path directory =
        "Content/Textures/Stamps/Game/StampIt/Examples";
    return directory;
}

inline bool IsTerrainStampFilename(const std::string& filename) {
    if (filename.empty() || filename.size() > 128 ||
        filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos ||
        filename.find("..") != std::string::npos)
        return false;
    std::string extension = std::filesystem::path(filename).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".png";
}

inline std::vector<std::string> DiscoverTerrainStampNames() {
    std::vector<std::string> names;
    std::error_code error;
    for (std::filesystem::directory_iterator it(TerrainStampDirectory(), error), end;
         !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        const std::string filename = it->path().filename().string();
        if (IsTerrainStampFilename(filename)) names.push_back(filename);
    }
    std::sort(names.begin(), names.end());
    if (names.size() > kMaxTerrainStampTextures)
        names.resize(kMaxTerrainStampTextures);
    return names;
}

inline std::string TerrainStampDisplayName(const std::string& filename) {
    std::string name = std::filesystem::path(filename).stem().string();
    if (name.rfind("HM_", 0) == 0) name.erase(0, 3);
    if (name.size() >= 3 && name.compare(name.size() - 3, 3, "_Ex") == 0)
        name.resize(name.size() - 3);
    std::replace(name.begin(), name.end(), '_', ' ');
    return name;
}
