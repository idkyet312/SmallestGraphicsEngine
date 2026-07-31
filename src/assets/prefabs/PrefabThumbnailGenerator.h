#ifndef PREFAB_THUMBNAIL_GENERATOR_H
#define PREFAB_THUMBNAIL_GENERATOR_H

#include <filesystem>
#include <vector>

struct PrefabThumbnailVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    float r = 0.65f, g = 0.68f, b = 0.72f;
};

struct PrefabThumbnailMesh {
    std::vector<PrefabThumbnailVertex> vertices;
    std::vector<unsigned int> indices;
    float minimum[3] = {};
    float maximum[3] = {};
};

class PrefabThumbnailGenerator {
public:
    // CPU import stage. Safe to run on a worker; DX12 upload/render stays on the
    // render thread.
    static bool LoadMesh(const std::filesystem::path& modelPath,
                         PrefabThumbnailMesh& output);

    // Headless fallback used by tests and systems without a graphics device.
    static bool Render128(const std::filesystem::path& modelPath,
                          const std::filesystem::path& pngPath);
};

#endif
