#pragma once

#include <array>
#include <filesystem>
#include <string>

// One art configuration serves the boarding cards and the loading screen.
struct TravelDestination {
    const char* name;
    const char* subtitle;
    std::array<const char*, 3> levelCandidates;
    const char* imagePath;
};

static const std::array<TravelDestination, 3> kTravelDestinations = { {
    { "ISLAND 1", "Campaign: hostile territory",
      { "Content/Levels/Islandv15.json",
        "levels/Islandv15.json",
        "build/Content/Levels/Islandv15.json" },
      "Content/Levels/Islandv15/preview.png" },
    { "MILITARY AIRFIELD", "Strike: aircraft on the ground",
      { "Content/Levels/BigIslandv34.json",
        "levels/BigIslandv34.json",
        "build/Content/Levels/BigIslandv34.json" },
      "Content/Levels/BigIslandv34/preview.png" },
    { "LEVEL 3", "Mission: island outpost",
      { "Content/Levels/level3v4.json",
        "levels/level3v4.json",
        "build/Content/Levels/level3v4.json" },
      "Content/Levels/level3v4/preview.png" },
} };

// The hub has loading art but is not a mission on the boarding board.
static const TravelDestination kBaseLoadingDestination = {
    "BASE", "Home base: rearm and prepare",
    { "Content/Levels/Base.json", "levels/Base.json",
      "build/Content/Levels/Base.json" },
    "Content/Levels/Base/preview.png"
};

static const TravelDestination* TravelDestinationForLevel(
    const std::filesystem::path& levelPath) {
    if (levelPath.filename() == "Base.json") return &kBaseLoadingDestination;
    for (const TravelDestination& destination : kTravelDestinations) {
        for (const char* candidate : destination.levelCandidates) {
            if (levelPath.filename() == std::filesystem::path(candidate).filename())
                return &destination;
        }
    }
    return nullptr;
}

static std::string TravelPreviewImagePath(const TravelDestination& destination) {
    if (!destination.imagePath || !*destination.imagePath) return {};
    std::error_code error;
    if (std::filesystem::is_regular_file(destination.imagePath, error))
        return destination.imagePath;
    const auto buildPath = std::filesystem::path("build") / destination.imagePath;
    if (std::filesystem::is_regular_file(buildPath, error))
        return buildPath.generic_string();
    return destination.imagePath;
}
