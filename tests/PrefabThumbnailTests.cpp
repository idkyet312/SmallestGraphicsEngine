#include "PrefabThumbnailGenerator.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("sge-thumbnail-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto model = root / "triangle.obj";
    const auto image = root / "thumb.png";
    {
        std::ofstream stream(model);
        stream << "v -1 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }
    PrefabThumbnailMesh mesh;
    CHECK(PrefabThumbnailGenerator::LoadMesh(model, mesh));
    CHECK(mesh.vertices.size() == 3);
    CHECK(mesh.indices.size() == 3);
    CHECK(PrefabThumbnailGenerator::Render128(model, image));
    int width = 0, height = 0, channels = 0;
    CHECK(stbi_info(image.string().c_str(), &width, &height, &channels) != 0);
    CHECK(width == 128);
    CHECK(height == 128);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return failures ? 1 : 0;
}
