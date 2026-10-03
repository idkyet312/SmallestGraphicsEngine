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

static const std::array<TravelDestination, 2> kTravelDestinations = { {
    { "ISLAND 1", "Campaign - hostile territory",
      { "Content/Levels/Islandv10.json",
        "levels/Islandv10.json",
        "build/Content/Levels/Islandv10.json" },
      "Content/Levels/Islandv10/preview.png" },
    { "MILITARY AIRFIELD", "Strike - aircraft on the ground",
      { "Content/Levels/BigIslandv34.json",
        "levels/BigIslandv34.json",
        "build/Content/Levels/BigIslandv34.json" },
      "Content/Levels/BigIslandv34/preview.png" },
} };

static const TravelDestination* TravelDestinationForLevel(
    const std::filesystem::path& levelPath) {
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
