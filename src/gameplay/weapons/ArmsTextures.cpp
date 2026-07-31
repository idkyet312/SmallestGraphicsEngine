// Pulls the view model's textures out of the FBX itself.
//
// This Mixamo export embeds all eight of its maps inside the .fbx (114 MB of
// PNG) and references them by the exporter's own build-server paths, e.g.
//   ../../../../home/app/mixamo-mini/tmp/skins_<guid>.fbm/Ch49_1002_Diffuse.png
// Nothing like that exists on this machine, so SkinnedFBXImporter's disk-based
// resolution finds nothing and every material falls back to flat white -- the
// untextured character on screen.
//
// The fix is to read the embedded blobs directly. Assimp exposes them as
// aiScene::mTextures, and a material path of "*N" is an index into that array;
// this file also matches by filename so the long authored paths resolve to the
// right blob.
//
// Everything is downscaled on the way in. The source maps are 4K, which at
// ~64 MB each as RGBA8 (plus an equal-sized upload heap) is what exhausted GPU
// memory during level load with the previous arms asset. Diffuse and normal are
// capped at 1024; the specular/glossiness maps are only used to derive a scalar
// roughness, so they are not uploaded at all.

#include "ArmsModel.h"
#include "GLBImporter.h"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::string LowerCase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Trailing filename of an authored texture path, in lower case. The embedded
// entries and the material references both carry the same long prefix, so the
// filename is the reliable key between them.
std::string FileNameKey(const std::string& path) {
    std::string normalised = path;
    std::replace(normalised.begin(), normalised.end(), '\\', '/');
    const size_t slash = normalised.find_last_of('/');
    return LowerCase(slash == std::string::npos ? normalised
                                                : normalised.substr(slash + 1));
}

// Halve until within `maxSize`, with a 2x2 box filter -- the same reduction mip
// generation performs, and plenty for skin and cloth.
void Downscale(std::vector<unsigned char>& pixels, int& width, int& height,
               int maxSize) {
    while ((width > maxSize || height > maxSize) && width >= 2 && height >= 2) {
        const int halfWidth = width / 2;
        const int halfHeight = height / 2;
        std::vector<unsigned char> reduced(
            static_cast<size_t>(halfWidth) * halfHeight * 4);
        for (int y = 0; y < halfHeight; ++y)
            for (int x = 0; x < halfWidth; ++x)
                for (int channel = 0; channel < 4; ++channel) {
                    const size_t topLeft =
                        ((static_cast<size_t>(y) * 2) * width + x * 2) * 4 + channel;
                    const size_t bottomLeft = topLeft + static_cast<size_t>(width) * 4;
                    reduced[(static_cast<size_t>(y) * halfWidth + x) * 4 + channel] =
                        static_cast<unsigned char>(
                            (pixels[topLeft] + pixels[topLeft + 4] +
                             pixels[bottomLeft] + pixels[bottomLeft + 4]) / 4);
                }
        pixels.swap(reduced);
        width = halfWidth;
        height = halfHeight;
    }
}

} // namespace

void ArmsModel::ApplyEmbeddedTextures() {
    SkinnedModel& model = Source();
    if (!model.node || !model.node->mesh) return;

    const std::string path =
        ResolvePath("Content/Models/MainPlayer/Rifle Idle(1).fbx");

    // Re-open the file purely for its texture blobs and material references.
    // Only structure is read here -- no post-processing, so this is far cheaper
    // than the skinned import and cannot disturb the geometry already loaded.
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path, 0);
    if (!scene) {
        std::fprintf(stderr, "View model textures: reopen failed (%s)\n",
                     importer.GetErrorString());
        return;
    }

    // Index every embedded blob by filename so a material's authored path can
    // find it. Assimp stores an encoded PNG with mHeight == 0 and the byte
    // count in mWidth.
    std::unordered_map<std::string, const aiTexture*> blobs;
    for (unsigned t = 0; t < scene->mNumTextures; ++t)
        blobs.emplace(FileNameKey(scene->mTextures[t]->mFilename.C_Str()),
                      scene->mTextures[t]);

    // Decode each blob at most once: the body materials and the eyelashes all
    // reference the same maps, and a 4K PNG is expensive to decode.
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D12Resource>> uploaded;

    auto resolve = [&](const aiMaterial* material, aiTextureType type,
                       int maxSize,
                       std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploads)
        -> Microsoft::WRL::ComPtr<ID3D12Resource> {
        aiString texturePath;
        if (material->GetTexture(type, 0, &texturePath) != AI_SUCCESS ||
            texturePath.length == 0)
            return nullptr;

        const std::string reference = texturePath.C_Str();
        const aiTexture* blob = nullptr;
        if (!reference.empty() && reference[0] == '*') {
            // "*N" indexes mTextures directly.
            const int index = std::atoi(reference.c_str() + 1);
            if (index >= 0 && static_cast<unsigned>(index) < scene->mNumTextures)
                blob = scene->mTextures[index];
        }
        if (!blob) {
            const auto found = blobs.find(FileNameKey(reference));
            if (found != blobs.end()) blob = found->second;
        }
        if (!blob || blob->mHeight != 0) return nullptr;   // absent, or already raw

        const std::string key = FileNameKey(blob->mFilename.C_Str()) + "@" +
                                std::to_string(maxSize);
        const auto cached = uploaded.find(key);
        if (cached != uploaded.end()) return cached->second;

        std::vector<unsigned char> pixels;
        int width = 0, height = 0;
        if (!GLBImporter::LoadPixelsRGBAFromMemory(
                reinterpret_cast<const unsigned char*>(blob->pcData),
                blob->mWidth, pixels, width, height))
            return nullptr;
        Downscale(pixels, width, height, maxSize);

        auto texture = GLBImporter::CreateTextureFromRGBA(
            g_dx12.device.Get(), g_dx12.commandList.Get(), pixels, width, height,
            uploads);
        uploaded.emplace(key, texture);
        return texture;
    };

    // Match the loaded primitives to their source materials by name: the
    // importer merges meshes per material but preserves the authored name.
    int textured = 0;
    for (MeshPrimitive& primitive : model.node->mesh->primitives) {
        if (!primitive.material) continue;
        const std::string wanted = LowerCase(primitive.material->name);

        const aiMaterial* source = nullptr;
        for (unsigned m = 0; m < scene->mNumMaterials; ++m) {
            aiString name;
            if (scene->mMaterials[m]->Get(AI_MATKEY_NAME, name) != AI_SUCCESS)
                continue;
            if (LowerCase(name.C_Str()) == wanted) {
                source = scene->mMaterials[m];
                break;
            }
        }
        if (!source) continue;

        auto& uploads = primitive.material->uploadHeaps;
        if (auto diffuse = resolve(source, aiTextureType_DIFFUSE,
                                   kAlbedoTextureSize, uploads)) {
            primitive.material->baseColorTexture = diffuse;
            // The white fallback tint would multiply the map down; let the
            // texture carry the colour.
            primitive.material->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
            ++textured;
        }
        if (auto normal = resolve(source, aiTextureType_NORMALS,
                                  kNormalTextureSize, uploads))
            primitive.material->normalTexture = normal;

        // Mixamo ships specular/glossiness, not metal/rough. Rather than
        // convert workflows for a model this size on screen, treat it as the
        // dielectric it is: cloth, webbing and skin are all non-metal.
        primitive.material->metallicRoughnessTexture.Reset();
        primitive.material->roughnessOnlyTexture = false;
        primitive.material->metallicFactor = 0.0f;
        primitive.material->roughnessFactor = 0.62f;
    }

    if (FILE* file = std::fopen("arms_textures.log", "w")) {
        std::fprintf(file, "embedded=%u materials=%u textured=%d albedo=%d normal=%d\n",
                     scene->mNumTextures, scene->mNumMaterials, textured,
                     kAlbedoTextureSize, kNormalTextureSize);
        for (const MeshPrimitive& primitive : model.node->mesh->primitives)
            if (primitive.material)
                std::fprintf(file, "  %-20s albedo=%d normal=%d\n",
                             primitive.material->name.c_str(),
                             primitive.material->baseColorTexture ? 1 : 0,
                             primitive.material->normalTexture ? 1 : 0);
        std::fclose(file);
    }
}
