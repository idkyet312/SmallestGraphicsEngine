#include "PrefabThumbnailGenerator.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <vector>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <stb_image_write.h>

namespace {
constexpr int kSize = 128;
struct Vertex { float x, y, z; };
float Edge(const Vertex& a, const Vertex& b, float x, float y) {
    return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}
}

bool PrefabThumbnailGenerator::Render128(const std::filesystem::path& modelPath,
                                         const std::filesystem::path& pngPath) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(modelPath.string(),
        aiProcess_Triangulate | aiProcess_PreTransformVertices |
        aiProcess_GenSmoothNormals);
    if (!scene || !scene->HasMeshes()) return false;
    aiVector3D minimum(FLT_MAX), maximum(-FLT_MAX);
    for (unsigned m = 0; m < scene->mNumMeshes; ++m)
        for (unsigned v = 0; v < scene->mMeshes[m]->mNumVertices; ++v) {
            const aiVector3D& p = scene->mMeshes[m]->mVertices[v];
            minimum.x = (std::min)(minimum.x, p.x); minimum.y = (std::min)(minimum.y, p.y);
            minimum.z = (std::min)(minimum.z, p.z); maximum.x = (std::max)(maximum.x, p.x);
            maximum.y = (std::max)(maximum.y, p.y); maximum.z = (std::max)(maximum.z, p.z);
        }
    const aiVector3D center = (minimum + maximum) * 0.5f;
    const float span = (std::max)({ maximum.x - minimum.x, maximum.y - minimum.y,
                                    maximum.z - minimum.z, 0.001f });
    const aiVector3D forward = aiVector3D(1.0f, -0.65f, 1.0f).Normalize();
    const aiVector3D right = aiVector3D(0.0f, 1.0f, 0.0f) ^ forward;
    const aiVector3D up = forward ^ right;
    std::vector<unsigned char> pixels(kSize * kSize * 4, 0);
    std::vector<float> depth(kSize * kSize, FLT_MAX);
    for (int y = 0; y < kSize; ++y) for (int x = 0; x < kSize; ++x) {
        const size_t pixel = static_cast<size_t>(y * kSize + x) * 4;
        const float checker = ((x / 16 + y / 16) & 1) ? 34.0f : 42.0f;
        pixels[pixel] = static_cast<unsigned char>(checker);
        pixels[pixel + 1] = static_cast<unsigned char>(checker + 4.0f);
        pixels[pixel + 2] = static_cast<unsigned char>(checker + 8.0f);
        pixels[pixel + 3] = 255;
    }
    const auto project = [&](const aiVector3D& source) {
        const aiVector3D p = source - center;
        return Vertex { 64.0f + (p * right) / span * 105.0f,
            66.0f - (p * up) / span * 105.0f, p * forward };
    };
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        aiColor4D diffuse(0.55f, 0.58f, 0.62f, 1.0f);
        if (mesh->mMaterialIndex < scene->mNumMaterials)
            aiGetMaterialColor(scene->mMaterials[mesh->mMaterialIndex],
                               AI_MATKEY_COLOR_DIFFUSE, &diffuse);
        for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices != 3) continue;
            Vertex p[3] = { project(mesh->mVertices[face.mIndices[0]]),
                            project(mesh->mVertices[face.mIndices[1]]),
                            project(mesh->mVertices[face.mIndices[2]]) };
            const float area = Edge(p[0], p[1], p[2].x, p[2].y);
            if (std::abs(area) < 1e-5f) continue;
            const int minX = (std::max)(0, static_cast<int>(std::floor(
                (std::min)({p[0].x, p[1].x, p[2].x}))));
            const int maxX = (std::min)(kSize - 1, static_cast<int>(std::ceil(
                (std::max)({p[0].x, p[1].x, p[2].x}))));
            const int minY = (std::max)(0, static_cast<int>(std::floor(
                (std::min)({p[0].y, p[1].y, p[2].y}))));
            const int maxY = (std::min)(kSize - 1, static_cast<int>(std::ceil(
                (std::max)({p[0].y, p[1].y, p[2].y}))));
            aiVector3D normal = (mesh->mVertices[face.mIndices[1]] -
                mesh->mVertices[face.mIndices[0]]) ^
                (mesh->mVertices[face.mIndices[2]] - mesh->mVertices[face.mIndices[0]]);
            normal.Normalize();
            const float lighting = 0.28f + 0.72f * std::abs(normal *
                aiVector3D(-0.4f, 0.8f, -0.45f).Normalize());
            for (int y = minY; y <= maxY; ++y) for (int x = minX; x <= maxX; ++x) {
                const float w0 = Edge(p[1], p[2], x + 0.5f, y + 0.5f) / area;
                const float w1 = Edge(p[2], p[0], x + 0.5f, y + 0.5f) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                const float z = w0 * p[0].z + w1 * p[1].z + w2 * p[2].z;
                const size_t index = static_cast<size_t>(y * kSize + x);
                if (z >= depth[index]) continue;
                depth[index] = z;
                pixels[index * 4] = static_cast<unsigned char>((std::min)(255.0f,
                    255.0f * diffuse.r * lighting));
                pixels[index * 4 + 1] = static_cast<unsigned char>((std::min)(255.0f,
                    255.0f * diffuse.g * lighting));
                pixels[index * 4 + 2] = static_cast<unsigned char>((std::min)(255.0f,
                    255.0f * diffuse.b * lighting));
                pixels[index * 4 + 3] = 255;
            }
        }
    }
    std::error_code error;
    std::filesystem::create_directories(pngPath.parent_path(), error);
    return stbi_write_png(pngPath.string().c_str(), kSize, kSize, 4,
                          pixels.data(), kSize * 4) != 0;
}
