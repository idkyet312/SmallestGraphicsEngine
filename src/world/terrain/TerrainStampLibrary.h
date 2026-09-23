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

// Where each map keeps its own images: Content/Levels/<Map>/ beside
// Content/Levels/<Map>.json. A stamp that belongs to one map lives there rather
// than in the shared library, so copying a map's folder carries its heightmap
// with it. The editor saves relative to the working directory, and a bake
// written into the shared library under build/ was exactly the file that never
// made it into a shipped build.
inline const std::filesystem::path& TerrainLevelsDirectory() {
    static const std::filesystem::path directory = "Content/Levels";
    return directory;
}

// A stamp texture is one of two shapes:
//   "HM_Islands_07_Ex.PNG"            -- the shared stamp library
//   "BigIslandv33/HM_Baked_mil.png"   -- that map's folder under Content/Levels
// Exactly one folder deep, so a level can name a map folder and nothing else:
// no absolute paths, no backslashes, no "..", no drive letters.
inline bool IsTerrainStampFilename(const std::string& filename) {
    if (filename.empty() || filename.size() > 128 ||
        filename.find('\\') != std::string::npos ||
        filename.find(':') != std::string::npos ||
        filename.find("..") != std::string::npos)
        return false;
    const size_t slash = filename.find('/');
    if (slash != std::string::npos) {
        // A folder and a file, both non-empty, and no second slash.
        if (slash == 0 || slash + 1 >= filename.size() ||
            filename.find('/', slash + 1) != std::string::npos)
            return false;
    }
    std::string extension = std::filesystem::path(filename).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".png";
}

// The file on disk a stamp name refers to. Every reader and the bake writer go
// through this, so the two shapes above resolve the same way everywhere.
inline std::filesystem::path ResolveTerrainStampPath(const std::string& texture) {
    return texture.find('/') != std::string::npos
        ? TerrainLevelsDirectory() / texture
        : TerrainStampDirectory() / texture;
}

// The folder a map's own files go under: the level file's stem, so
// Content/Levels/BigIslandv33.json owns Content/Levels/BigIslandv33/. Folded to
// the characters the stamp name check accepts.
inline std::string TerrainLevelFolderName(const std::string& levelStem) {
    std::string folder;
    for (char c : levelStem) {
        const unsigned char u = static_cast<unsigned char>(c);
        folder.push_back((std::isalnum(u) || c == '-' || c == '_') ? c : '_');
    }
    if (folder.empty()) folder = "level";
    if (folder.size() > 64) folder.resize(64);
    return folder;
}

// The editor names a bake after its map: <Map>/<Map>_baked.png, beside
// <Map>/<Map>_splat.png. The suffix is what routes a stamp to the dedicated
// high-resolution slot instead of the shared atlas. Bakes made before maps had
// folders were named HM_Baked_<level>.png, and that prefix still routes, so a
// level saved then keeps its detail. Both are tested on the file part.
inline constexpr const char* kTerrainStampBakeSuffix = "_baked.png";
inline constexpr const char* kTerrainStampLegacyBakePrefix = "HM_Baked_";

inline bool IsTerrainStampBakeFilename(const std::string& filename) {
    const size_t slash = filename.find('/');
    std::string file =
        slash == std::string::npos ? filename : filename.substr(slash + 1);
    if (file.rfind(kTerrainStampLegacyBakePrefix, 0) == 0) return true;
    std::transform(file.begin(), file.end(), file.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string suffix = kTerrainStampBakeSuffix;
    return file.size() > suffix.size() &&
           file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The stamp name a map's bake is written under, e.g.
// "BigIslandv33/BigIslandv33_baked.png". `folder` is already folded by
// TerrainLevelFolderName, so the result passes IsTerrainStampFilename.
inline std::string TerrainLevelBakeName(const std::string& folder) {
    return folder + "/" + folder + kTerrainStampBakeSuffix;
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
