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

// The bake slot. BakeTerrainSculptToStamp fits ONE stamp over the bounding box
// of the entire sculpt stack, so unlike a hand-placed stamp its footprint is
// the whole sculpted level -- at 512 a 500 m sculpt bakes to ~1 m/texel, well
// coarser than the mesh can draw, and the stack's fine relief is averaged away.
//
// Raising the shared resolution to fix that is the wrong trade: every one of
// the 64 layers would pay it (4096 across the board is 2.1 GB). Instead the
// baked stamp gets one dedicated high-resolution region appended after the
// atlas, addressed by kTerrainStampBakeLayer. 4096^2 * 2B = 33.5 MB for the
// single slot, which doubles the stamp memory rather than multiplying it by 64.
inline constexpr uint32_t kTerrainStampBakeResolution = 4096;
inline constexpr uint32_t kTerrainStampBakeLayer = kMaxTerrainStampTextures;

// Texel count of the atlas plus the bake slot: the size of the buffer the
// shader indexes. The bake region starts at kMaxTerrainStampTextures * 512^2.
inline constexpr size_t kTerrainStampAtlasTexels =
    static_cast<size_t>(kMaxTerrainStampTextures) *
        kTerrainStampResolution * kTerrainStampResolution +
    static_cast<size_t>(kTerrainStampBakeResolution) *
        kTerrainStampBakeResolution;

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

// Bakes are named HM_Baked_<level>.png by the editor. The prefix is what routes
// a stamp to the dedicated high-resolution slot instead of the shared atlas, so
// it lives here rather than being spelled out at the one call site that mints
// the name.
inline constexpr const char* kTerrainStampBakePrefix = "HM_Baked_";

inline bool IsTerrainStampBakeFilename(const std::string& filename) {
    return filename.rfind(kTerrainStampBakePrefix, 0) == 0;
}

inline std::vector<std::string> DiscoverTerrainStampNames() {
    std::vector<std::string> names;
    std::error_code error;
    for (std::filesystem::directory_iterator it(TerrainStampDirectory(), error), end;
         !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        const std::string filename = it->path().filename().string();
        // Bakes load into the dedicated slot on demand, so they must not take
        // one of the 64 atlas layers here.
        if (IsTerrainStampFilename(filename) &&
            !IsTerrainStampBakeFilename(filename))
            names.push_back(filename);
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
