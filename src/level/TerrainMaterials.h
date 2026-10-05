#pragma once

#include <array>
#include <string>

enum class LevelMapType { Tropical, Snowy, Custom, GrassGround };

inline const char* LevelMapTypeName(LevelMapType type) {
    switch (type) {
    case LevelMapType::Tropical: return "tropical";
    case LevelMapType::Snowy: return "snowy";
    case LevelMapType::Custom: return "custom";
    case LevelMapType::GrassGround: return "grass_ground";
    }
    return "unknown";
}

struct TerrainTextureLayer {
    bool custom = false;
    std::string albedo;
    std::string normal;
    std::string roughness;
    std::string ambientOcclusion;
    std::string height;

    bool operator==(const TerrainTextureLayer& other) const {
        return custom == other.custom && albedo == other.albedo &&
            normal == other.normal && roughness == other.roughness &&
            ambientOcclusion == other.ambientOcclusion && height == other.height;
    }
    bool operator!=(const TerrainTextureLayer& other) const { return !(*this == other); }
};

using TerrainTextureLayers = std::array<TerrainTextureLayer, 4>;

inline TerrainTextureLayers TerrainTexturePreset(LevelMapType type) {
    TerrainTextureLayers layers = {{
        { false, "Content/Models/Grass3/Grass004_2K-JPG/Grass004_2K-JPG_Color.jpg",
          "Content/Models/Grass3/Grass004_2K-JPG/Grass004_2K-JPG_NormalGL.jpg",
          "Content/Models/Grass3/Grass004_2K-JPG/Grass004_2K-JPG_Roughness.jpg",
          "Content/Models/Grass3/Grass004_2K-JPG/Grass004_2K-JPG_AmbientOcclusion.jpg",
          "Content/Models/Grass3/Grass004_2K-JPG/Grass004_2K-JPG_Displacement.jpg" },
        { false, "Content/Models/terrain/dirt_floor/dirt_floor_diff_1k.png",
          "Content/Models/terrain/dirt_floor/dirt_floor_nor_gl_1k.png",
          "Content/Models/terrain/dirt_floor/dirt_floor_rough_1k.png", "", "" },
        { false, "Content/Models/terrain/aerial_beach_01/aerial_beach_01_diff_2k.png",
          "Content/Models/terrain/aerial_beach_01/aerial_beach_01_nor_gl_2k.png",
          "Content/Models/terrain/aerial_beach_01/aerial_beach_01_rough_2k.png",
          "Content/Models/terrain/aerial_beach_01/aerial_beach_01_ao_2k.png", "" },
        { false, "Content/Models/terrain/dark_rock/dark_rock_diff_1k.png",
          "Content/Models/terrain/dark_rock/dark_rock_nor_gl_1k.png",
          "Content/Models/terrain/dark_rock/dark_rock_rough_1k.png", "", "" }
    }};
    if (type == LevelMapType::Snowy) {
        for (size_t i = 0; i < 3; ++i) {
            const std::string prefix =
                "Content/Models/terrain/snow_02_2k/snow_02";
            layers[i] = { false, prefix + "_diff_2k.jpg",
                prefix + "_nor_gl_2k.png", prefix + "_rough_2k.png",
                prefix + "_ao_2k.jpg", prefix + "_disp_2k.png" };
        }
    } else if (type == LevelMapType::GrassGround) {
        const std::string prefix =
            "Content/Models/terrain/grass_ground_2k/grass_ground";
        layers[0] = { false, prefix + "_diff_2k.jpg",
            prefix + "_nor_gl_2k.png", prefix + "_rough_2k.png",
            prefix + "_ao_2k.jpg", prefix + "_disp_2k.png" };
        const std::string sandPrefix =
            "Content/Models/terrain/coast_sand_05_2k/coast_sand_05";
        layers[2] = { false, sandPrefix + "_diff_2k.jpg",
            sandPrefix + "_nor_gl_2k.png", sandPrefix + "_rough_2k.png",
            sandPrefix + "_ao_2k.jpg", sandPrefix + "_disp_2k.png" };
    }
    return layers;
}

inline TerrainTextureLayers ResolveTerrainTextureLayers(
        LevelMapType type, const TerrainTextureLayers& overrides) {
    TerrainTextureLayers layers = TerrainTexturePreset(type);
    for (size_t i = 0; i < layers.size(); ++i)
        if (overrides[i].custom) layers[i] = overrides[i];
    return layers;
}

inline bool IsTerrainTexturePath(const std::string& path) {
    if (path.empty()) return true;
    // Portable level files always refer to packaged Content, never an author's drive.
    if (path.size() > 1024 || path.compare(0, 8, "Content/") != 0 ||
        path.find('\\') != std::string::npos || path.find(':') != std::string::npos)
        return false;
    size_t start = 0;
    while (start < path.size()) {
        const size_t end = path.find('/', start);
        const std::string part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return path.back() != '/';
}
