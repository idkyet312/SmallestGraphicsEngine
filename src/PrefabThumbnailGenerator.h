#ifndef PREFAB_THUMBNAIL_GENERATOR_H
#define PREFAB_THUMBNAIL_GENERATOR_H

#include <filesystem>

class PrefabThumbnailGenerator {
public:
    // Software offscreen target used on a worker thread. GPU upload happens on
    // the render thread after this cached PNG is complete.
    static bool Render128(const std::filesystem::path& modelPath,
                          const std::filesystem::path& pngPath);
};

#endif
