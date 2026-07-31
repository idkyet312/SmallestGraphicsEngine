#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "CookedAssetFormat.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/GltfMaterial.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace Cooked = SGE::Cooked;

namespace {

uint64_t HashBytes(const void* data, size_t size,
                   uint64_t hash = 1469598103934665603ull) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t HashFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::array<char, 64 * 1024> block{};
    uint64_t hash = 1469598103934665603ull;
    while (stream) {
        stream.read(block.data(), block.size());
        hash = HashBytes(block.data(), static_cast<size_t>(stream.gcount()), hash);
    }
    return hash;
}

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

struct BlobBuilder {
    std::vector<uint8_t> bytes;

    explicit BlobBuilder(size_t initial) : bytes(initial, 0) {}

    uint64_t Append(const void* data, size_t size, size_t alignment = 16) {
        const uint64_t offset = AlignUp(bytes.size(), alignment);
        bytes.resize(static_cast<size_t>(offset), 0);
        const size_t oldSize = bytes.size();
        bytes.resize(oldSize + size);
        if (size && data) std::memcpy(bytes.data() + oldSize, data, size);
        return offset;
    }

    template<class T>
    uint64_t Append(const std::vector<T>& values, size_t alignment = 16) {
        return Append(values.data(), values.size() * sizeof(T), alignment);
    }

    template<class T>
    void Patch(uint64_t offset, const std::vector<T>& values) {
        if (!values.empty())
            std::memcpy(bytes.data() + offset, values.data(),
                        values.size() * sizeof(T));
    }
};

struct Strings {
    std::vector<char> data{'\0'};
    std::unordered_map<std::string, uint32_t> offsets;

    uint32_t Add(const std::string& text) {
        if (text.empty()) return 0;
        auto found = offsets.find(text);
        if (found != offsets.end()) return found->second;
        const uint32_t result = static_cast<uint32_t>(data.size());
        data.insert(data.end(), text.begin(), text.end());
        data.push_back('\0');
        offsets.emplace(text, result);
        return result;
    }
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

struct EncodedTexture {
    Cooked::Texture record;
    std::vector<uint8_t> data;
};

uint16_t Pack565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r * 31 + 127) / 255) << 11 |
                                 ((g * 63 + 127) / 255) << 5 |
                                 ((b * 31 + 127) / 255));
}

std::array<uint8_t, 3> Unpack565(uint16_t value) {
    return {
        static_cast<uint8_t>(((value >> 11) & 31) * 255 / 31),
        static_cast<uint8_t>(((value >> 5) & 63) * 255 / 63),
        static_cast<uint8_t>((value & 31) * 255 / 31)
    };
}

void EncodeBC1Color(const uint8_t block[16][4], uint8_t out[8]) {
    uint8_t minimum[3] = {255, 255, 255};
    uint8_t maximum[3] = {0, 0, 0};
    for (uint32_t i = 0; i < 16; ++i) {
        for (uint32_t c = 0; c < 3; ++c) {
            minimum[c] = std::min(minimum[c], block[i][c]);
            maximum[c] = std::max(maximum[c], block[i][c]);
        }
    }
    uint16_t c0 = Pack565(maximum[0], maximum[1], maximum[2]);
    uint16_t c1 = Pack565(minimum[0], minimum[1], minimum[2]);
    if (c0 <= c1) {
        if (c0 < 0xffff) ++c0;
        else if (c1 > 0) --c1;
    }
    const auto p0 = Unpack565(c0);
    const auto p1 = Unpack565(c1);
    uint8_t palette[4][3] = {};
    for (uint32_t c = 0; c < 3; ++c) {
        palette[0][c] = p0[c];
        palette[1][c] = p1[c];
        palette[2][c] = static_cast<uint8_t>((2 * p0[c] + p1[c]) / 3);
        palette[3][c] = static_cast<uint8_t>((p0[c] + 2 * p1[c]) / 3);
    }
    uint32_t selectors = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t best = 0;
        uint32_t bestError = std::numeric_limits<uint32_t>::max();
        for (uint32_t p = 0; p < 4; ++p) {
            int dr = int(block[i][0]) - int(palette[p][0]);
            int dg = int(block[i][1]) - int(palette[p][1]);
            int db = int(block[i][2]) - int(palette[p][2]);
            uint32_t error = uint32_t(dr * dr + dg * dg + db * db);
            if (error < bestError) {
                bestError = error;
                best = p;
            }
        }
        selectors |= best << (2 * i);
    }
    std::memcpy(out, &c0, 2);
    std::memcpy(out + 2, &c1, 2);
    std::memcpy(out + 4, &selectors, 4);
}

void EncodeBC4(const uint8_t values[16], uint8_t out[8]) {
    uint8_t minimum = 255;
    uint8_t maximum = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        minimum = std::min(minimum, values[i]);
        maximum = std::max(maximum, values[i]);
    }
    out[0] = maximum;
    out[1] = minimum;
    uint8_t palette[8] = { maximum, minimum };
    if (maximum > minimum) {
        for (uint32_t i = 1; i <= 6; ++i)
            palette[i + 1] = static_cast<uint8_t>(
                ((7 - i) * maximum + i * minimum) / 7);
    } else {
        for (uint32_t i = 1; i <= 4; ++i)
            palette[i + 1] = static_cast<uint8_t>(
                ((5 - i) * maximum + i * minimum) / 5);
        palette[6] = 0;
        palette[7] = 255;
    }
    uint64_t selectors = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t best = 0;
        uint32_t bestError = 256;
        for (uint32_t p = 0; p < 8; ++p) {
            uint32_t error = static_cast<uint32_t>(
                std::abs(int(values[i]) - int(palette[p])));
            if (error < bestError) {
                bestError = error;
                best = p;
            }
        }
        selectors |= uint64_t(best) << (3 * i);
    }
    for (uint32_t byte = 0; byte < 6; ++byte)
        out[2 + byte] = static_cast<uint8_t>(selectors >> (8 * byte));
}

std::vector<uint8_t> EncodeMip(const Image& image,
                               Cooked::TextureFormat format) {
    const int blocksX = std::max(1, (image.width + 3) / 4);
    const int blocksY = std::max(1, (image.height + 3) / 4);
    std::vector<uint8_t> result(static_cast<size_t>(blocksX) * blocksY * 16);
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            uint8_t block[16][4] = {};
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const int sx = std::min(image.width - 1, bx * 4 + x);
                    const int sy = std::min(image.height - 1, by * 4 + y);
                    std::memcpy(block[y * 4 + x],
                        &image.rgba[(static_cast<size_t>(sy) * image.width + sx) * 4],
                        4);
                }
            }
            uint8_t* destination =
                result.data() + (static_cast<size_t>(by) * blocksX + bx) * 16;
            if (format == Cooked::TextureFormat::BC5) {
                uint8_t red[16], green[16];
                for (uint32_t i = 0; i < 16; ++i) {
                    red[i] = block[i][0];
                    green[i] = block[i][1];
                }
                EncodeBC4(red, destination);
                EncodeBC4(green, destination + 8);
            } else {
                uint8_t alpha[16];
                for (uint32_t i = 0; i < 16; ++i) alpha[i] = block[i][3];
                EncodeBC4(alpha, destination);
                EncodeBC1Color(block, destination + 8);
            }
        }
    }
    return result;
}

Image NextMip(const Image& source) {
    Image result;
    result.width = std::max(1, source.width / 2);
    result.height = std::max(1, source.height / 2);
    result.rgba.resize(static_cast<size_t>(result.width) * result.height * 4);
    for (int y = 0; y < result.height; ++y) {
        for (int x = 0; x < result.width; ++x) {
            uint32_t sum[4] = {};
            uint32_t count = 0;
            for (int oy = 0; oy < 2; ++oy) {
                for (int ox = 0; ox < 2; ++ox) {
                    const int sx = std::min(source.width - 1, x * 2 + ox);
                    const int sy = std::min(source.height - 1, y * 2 + oy);
                    const uint8_t* pixel =
                        &source.rgba[(static_cast<size_t>(sy) * source.width + sx) * 4];
                    for (uint32_t c = 0; c < 4; ++c) sum[c] += pixel[c];
                    ++count;
                }
            }
            uint8_t* destination =
                &result.rgba[(static_cast<size_t>(y) * result.width + x) * 4];
            for (uint32_t c = 0; c < 4; ++c)
                destination[c] = static_cast<uint8_t>((sum[c] + count / 2) / count);
        }
    }
    return result;
}

EncodedTexture EncodeTexture(const Image& source,
                             Cooked::TextureFormat format) {
    EncodedTexture result;
    result.record.width = static_cast<uint32_t>(source.width);
    result.record.height = static_cast<uint32_t>(source.height);
    result.record.format = format;
    Image mip = source;
    while (result.record.mipCount < Cooked::kMaxTextureMips) {
        const uint32_t level = result.record.mipCount++;
        result.record.mipOffsets[level] =
            static_cast<uint32_t>(result.data.size());
        std::vector<uint8_t> encoded = EncodeMip(mip, format);
        result.record.mipSizes[level] =
            static_cast<uint32_t>(encoded.size());
        result.data.insert(result.data.end(), encoded.begin(), encoded.end());
        if (mip.width == 1 && mip.height == 1) break;
        mip = NextMip(mip);
    }
    result.record.dataSize = static_cast<uint32_t>(result.data.size());
    return result;
}

bool LoadExternalImage(const fs::path& path, Image& image) {
    int channels = 0;
    unsigned char* pixels = stbi_load(path.string().c_str(), &image.width,
                                     &image.height, &channels, 4);
    if (!pixels) return false;
    image.rgba.assign(pixels, pixels +
        static_cast<size_t>(image.width) * image.height * 4);
    stbi_image_free(pixels);
    return true;
}

bool LoadEmbeddedImage(const aiTexture* texture, Image& image) {
    if (!texture) return false;
    if (texture->mHeight == 0) {
        int channels = 0;
        unsigned char* pixels = stbi_load_from_memory(
            reinterpret_cast<const unsigned char*>(texture->pcData),
            static_cast<int>(texture->mWidth), &image.width, &image.height,
            &channels, 4);
        if (!pixels) return false;
        image.rgba.assign(pixels, pixels +
            static_cast<size_t>(image.width) * image.height * 4);
        stbi_image_free(pixels);
        return true;
    }
    image.width = static_cast<int>(texture->mWidth);
    image.height = static_cast<int>(texture->mHeight);
    image.rgba.resize(static_cast<size_t>(image.width) * image.height * 4);
    for (size_t i = 0; i < static_cast<size_t>(image.width) * image.height; ++i) {
        image.rgba[i * 4 + 0] = texture->pcData[i].r;
        image.rgba[i * 4 + 1] = texture->pcData[i].g;
        image.rgba[i * 4 + 2] = texture->pcData[i].b;
        image.rgba[i * 4 + 3] = texture->pcData[i].a;
    }
    return true;
}

struct MeshletPayload {
    std::vector<Cooked::MeshletDesc> descriptors;
    std::vector<Cooked::MeshletBounds> bounds;
    std::vector<uint32_t> vertices;
    std::vector<uint32_t> triangles;
};

MeshletPayload BuildMeshlets(const std::vector<Cooked::Vertex>& vertices,
                             const std::vector<uint32_t>& indices,
                             bool doubleSided) {
    MeshletPayload result;
    if (vertices.empty() || indices.size() < 3) return result;
    constexpr size_t maxVertices = 64;
    constexpr size_t maxTriangles = 124;
    const size_t bound = meshopt_buildMeshletsBound(
        indices.size(), maxVertices, maxTriangles);
    std::vector<meshopt_Meshlet> meshlets(bound);
    std::vector<uint32_t> vertexIndices(bound * maxVertices);
    std::vector<uint8_t> triangleBytes(bound * maxTriangles * 3);
    const size_t count = meshopt_buildMeshlets(
        meshlets.data(), vertexIndices.data(), triangleBytes.data(),
        indices.data(), indices.size(), &vertices[0].position[0],
        vertices.size(), sizeof(Cooked::Vertex), maxVertices, maxTriangles,
        0.25f);
    meshlets.resize(count);
    if (count) {
        const meshopt_Meshlet& last = meshlets.back();
        vertexIndices.resize(last.vertex_offset + last.vertex_count);
        triangleBytes.resize(last.triangle_offset + last.triangle_count * 3);
    } else {
        vertexIndices.clear();
        triangleBytes.clear();
    }
    result.vertices = std::move(vertexIndices);
    result.descriptors.reserve(count);
    result.bounds.reserve(count);
    for (const meshopt_Meshlet& source : meshlets) {
        Cooked::MeshletDesc descriptor = {
            source.vertex_offset, source.vertex_count,
            static_cast<uint32_t>(result.triangles.size()),
            source.triangle_count
        };
        result.descriptors.push_back(descriptor);
        for (uint32_t triangle = 0; triangle < source.triangle_count; ++triangle) {
            const uint8_t* local =
                &triangleBytes[source.triangle_offset + triangle * 3];
            result.triangles.push_back(uint32_t(local[0]) |
                uint32_t(local[1]) << 8 | uint32_t(local[2]) << 16);
        }
        float minimum[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float maximum[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (uint32_t i = 0; i < source.vertex_count; ++i) {
            const auto& position =
                vertices[result.vertices[source.vertex_offset + i]].position;
            for (uint32_t c = 0; c < 3; ++c) {
                minimum[c] = std::min(minimum[c], position[c]);
                maximum[c] = std::max(maximum[c], position[c]);
            }
        }
        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            result.vertices.data() + source.vertex_offset,
            triangleBytes.data() + source.triangle_offset,
            source.triangle_count, &vertices[0].position[0],
            vertices.size(), sizeof(Cooked::Vertex));
        Cooked::MeshletBounds cooked = {};
        std::copy(minimum, minimum + 3, cooked.boundsMin);
        std::copy(maximum, maximum + 3, cooked.boundsMax);
        std::copy(bounds.center, bounds.center + 3, cooked.sphereCenter);
        cooked.sphereRadius = bounds.radius;
        std::copy(bounds.cone_axis, bounds.cone_axis + 3, cooked.coneAxis);
        cooked.coneCutoff = doubleSided ? -1.0f : bounds.cone_cutoff;
        result.bounds.push_back(cooked);
    }
    return result;
}

struct PrimitiveData {
    Cooked::Primitive record;
    std::vector<Cooked::Vertex> vertices;
    std::vector<uint32_t> indices;
    MeshletPayload meshlets;
};

struct AnimationTrackData {
    Cooked::Track record;
    std::vector<Cooked::QuantizedVecKey> positions;
    std::vector<Cooked::QuantizedQuatKey> rotations;
    std::vector<Cooked::QuantizedVecKey> scales;
};

struct AnimationClipData {
    Cooked::Clip record;
    std::vector<AnimationTrackData> tracks;
};

uint16_t QuantizeTime(double time, double duration) {
    if (duration <= 0.0) return 0;
    return static_cast<uint16_t>(std::lround(
        std::clamp(time / duration, 0.0, 1.0) * 65535.0));
}

uint16_t QuantizeFloat(float value, float minimum, float maximum) {
    if (maximum - minimum < 1e-12f) return 0;
    const float normalized = std::clamp(
        (value - minimum) / (maximum - minimum), 0.0f, 1.0f);
    return static_cast<uint16_t>(std::lround(normalized * 65535.0f));
}

int16_t QuantizeSnorm(float value) {
    return static_cast<int16_t>(std::lround(
        std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

std::string LowerExtension(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

bool IsModel(const fs::path& path) {
    const std::string extension = LowerExtension(path);
    return extension == ".fbx" || extension == ".glb" ||
           extension == ".gltf";
}

struct CookContext {
    fs::path sourcePath;
    const aiScene* scene = nullptr;
    Strings strings;
    std::vector<Cooked::Material> materials;
    std::vector<EncodedTexture> textures;
    std::unordered_map<std::string, uint32_t> textureByKey;
    std::vector<PrimitiveData> primitives;
    std::vector<AnimationClipData> clips;

    uint32_t AddTexture(const aiString& reference,
                        Cooked::TextureFormat format) {
        std::string raw = reference.C_Str();
        std::replace(raw.begin(), raw.end(), '\\', '/');
        const std::string key = raw + "#" +
            std::to_string(static_cast<uint32_t>(format));
        auto found = textureByKey.find(key);
        if (found != textureByKey.end()) return found->second;

        Image image;
        std::string sourceName = raw;
        if (!raw.empty() && raw[0] == '*') {
            if (!LoadEmbeddedImage(scene->GetEmbeddedTexture(raw.c_str()), image))
                return Cooked::kInvalidIndex;
        } else {
            fs::path path = fs::path(raw);
            if (!path.is_absolute()) path = sourcePath.parent_path() / path;
            path = path.lexically_normal();
            if (!LoadExternalImage(path, image)) return Cooked::kInvalidIndex;
        }
        EncodedTexture texture = EncodeTexture(image, format);
        texture.record.name = strings.Add(fs::path(raw).filename().string());
        texture.record.source = strings.Add(sourceName);
        const uint32_t index = static_cast<uint32_t>(textures.size());
        textures.push_back(std::move(texture));
        textureByKey.emplace(key, index);
        return index;
    }

    uint32_t MaterialTexture(aiMaterial* material,
                             std::initializer_list<aiTextureType> types,
                             Cooked::TextureFormat format) {
        aiString path;
        for (aiTextureType type : types)
            if (material->GetTextureCount(type) &&
                material->GetTexture(type, 0, &path) == AI_SUCCESS)
                return AddTexture(path, format);
        return Cooked::kInvalidIndex;
    }
};

void ExtractMaterials(CookContext& context) {
    context.materials.reserve(context.scene->mNumMaterials);
    for (uint32_t i = 0; i < context.scene->mNumMaterials; ++i) {
        aiMaterial* source = context.scene->mMaterials[i];
        Cooked::Material material;
        aiString name;
        source->Get(AI_MATKEY_NAME, name);
        material.name = context.strings.Add(name.C_Str());
        aiColor4D color(1, 1, 1, 1);
        if (source->Get(AI_MATKEY_BASE_COLOR, color) != AI_SUCCESS)
            source->Get(AI_MATKEY_COLOR_DIFFUSE, color);
        material.baseColor[0] = color.r;
        material.baseColor[1] = color.g;
        material.baseColor[2] = color.b;
        material.baseColor[3] = color.a;
        source->Get(AI_MATKEY_METALLIC_FACTOR, material.metallic);
        source->Get(AI_MATKEY_ROUGHNESS_FACTOR, material.roughness);
        int twoSided = 0;
        source->Get(AI_MATKEY_TWOSIDED, twoSided);
        if (twoSided) material.flags |= Cooked::DoubleSided;
        aiString alphaMode;
        if (source->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS &&
            std::string(alphaMode.C_Str()) != "OPAQUE")
            material.flags |= Cooked::AlphaCutout;
        material.baseColorTexture = context.MaterialTexture(source,
            { aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE },
            Cooked::TextureFormat::BC3);
        material.normalTexture = context.MaterialTexture(source,
            { aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA,
              aiTextureType_HEIGHT }, Cooked::TextureFormat::BC5);
        material.metallicRoughnessTexture = context.MaterialTexture(source,
            { aiTextureType_GLTF_METALLIC_ROUGHNESS,
              aiTextureType_DIFFUSE_ROUGHNESS, aiTextureType_METALNESS,
              aiTextureType_UNKNOWN }, Cooked::TextureFormat::BC3);
        context.materials.push_back(material);
    }
}

PrimitiveData ExtractPrimitive(const aiMesh& mesh, Strings& strings,
                               const std::vector<Cooked::Material>& materials) {
    PrimitiveData result;
    result.record.name = strings.Add(mesh.mName.C_Str());
    result.record.material = mesh.mMaterialIndex;
    result.vertices.resize(mesh.mNumVertices);
    for (uint32_t i = 0; i < mesh.mNumVertices; ++i) {
        Cooked::Vertex& vertex = result.vertices[i];
        vertex.position[0] = mesh.mVertices[i].x;
        vertex.position[1] = mesh.mVertices[i].y;
        vertex.position[2] = mesh.mVertices[i].z;
        const aiVector3D normal = mesh.HasNormals()
            ? mesh.mNormals[i] : aiVector3D(0, 1, 0);
        vertex.normal[0] = normal.x;
        vertex.normal[1] = normal.y;
        vertex.normal[2] = normal.z;
        const aiVector3D uv = mesh.HasTextureCoords(0)
            ? mesh.mTextureCoords[0][i] : aiVector3D();
        vertex.uv[0] = uv.x;
        vertex.uv[1] = uv.y;
        const aiVector3D tangent = mesh.HasTangentsAndBitangents()
            ? mesh.mTangents[i] : aiVector3D(1, 0, 0);
        vertex.tangent[0] = tangent.x;
        vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z;
        vertex.tangent[3] = 1.0f;
        if (mesh.HasTangentsAndBitangents()) {
            const aiVector3D cross = normal ^ tangent;
            vertex.tangent[3] = (cross * mesh.mBitangents[i]) < 0 ? -1.0f : 1.0f;
        }
    }
    result.indices.reserve(static_cast<size_t>(mesh.mNumFaces) * 3);
    for (uint32_t face = 0; face < mesh.mNumFaces; ++face) {
        const aiFace& source = mesh.mFaces[face];
        if (source.mNumIndices != 3) continue;
        result.indices.insert(result.indices.end(),
            source.mIndices, source.mIndices + 3);
    }
    if (!result.indices.empty() && !result.vertices.empty()) {
        std::vector<uint32_t> cacheOptimized(result.indices.size());
        meshopt_optimizeVertexCache(cacheOptimized.data(), result.indices.data(),
                                    result.indices.size(), result.vertices.size());
        std::vector<uint32_t> overdrawOptimized(result.indices.size());
        meshopt_optimizeOverdraw(overdrawOptimized.data(), cacheOptimized.data(),
            cacheOptimized.size(), result.vertices[0].position,
            result.vertices.size(), sizeof(Cooked::Vertex), 1.05f);
        std::vector<Cooked::Vertex> fetchOptimized(result.vertices.size());
        const size_t vertexCount = meshopt_optimizeVertexFetch(
            fetchOptimized.data(), overdrawOptimized.data(),
            overdrawOptimized.size(), result.vertices.data(),
            result.vertices.size(), sizeof(Cooked::Vertex));
        fetchOptimized.resize(vertexCount);
        result.vertices = std::move(fetchOptimized);
        result.indices = std::move(overdrawOptimized);
    }
    float minimum[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float maximum[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const auto& vertex : result.vertices) {
        for (uint32_t c = 0; c < 3; ++c) {
            minimum[c] = std::min(minimum[c], vertex.position[c]);
            maximum[c] = std::max(maximum[c], vertex.position[c]);
        }
    }
    std::copy(minimum, minimum + 3, result.record.boundsMin);
    std::copy(maximum, maximum + 3, result.record.boundsMax);
    const bool doubleSided = result.record.material < materials.size() &&
        (materials[result.record.material].flags & Cooked::DoubleSided);
    result.meshlets = BuildMeshlets(
        result.vertices, result.indices, doubleSided);
    result.record.vertexCount = static_cast<uint32_t>(result.vertices.size());
    result.record.indexCount = static_cast<uint32_t>(result.indices.size());
    result.record.indexElementSize =
        result.vertices.size() <= 65535 ? 2u : 4u;
    result.record.meshletCount =
        static_cast<uint32_t>(result.meshlets.descriptors.size());
    result.record.meshletVertexCount =
        static_cast<uint32_t>(result.meshlets.vertices.size());
    result.record.meshletTriangleCount =
        static_cast<uint32_t>(result.meshlets.triangles.size());
    return result;
}

template<class Key>
void VecBounds(const Key* keys, uint32_t count, float minimum[3],
               float maximum[3]) {
    std::fill(minimum, minimum + 3, FLT_MAX);
    std::fill(maximum, maximum + 3, -FLT_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        const aiVector3D& value = keys[i].mValue;
        minimum[0] = std::min(minimum[0], value.x);
        minimum[1] = std::min(minimum[1], value.y);
        minimum[2] = std::min(minimum[2], value.z);
        maximum[0] = std::max(maximum[0], value.x);
        maximum[1] = std::max(maximum[1], value.y);
        maximum[2] = std::max(maximum[2], value.z);
    }
    if (!count) {
        std::fill(minimum, minimum + 3, 0.0f);
        std::fill(maximum, maximum + 3, 0.0f);
    }
}

void ExtractAnimations(CookContext& context) {
    context.clips.reserve(context.scene->mNumAnimations);
    for (uint32_t clipIndex = 0; clipIndex < context.scene->mNumAnimations;
         ++clipIndex) {
        const aiAnimation& source = *context.scene->mAnimations[clipIndex];
        AnimationClipData clip;
        clip.record.name = context.strings.Add(source.mName.C_Str());
        const double ticksPerSecond =
            source.mTicksPerSecond > 0.0 ? source.mTicksPerSecond : 25.0;
        clip.record.durationSeconds =
            static_cast<float>(source.mDuration / ticksPerSecond);
        clip.tracks.reserve(source.mNumChannels);
        for (uint32_t channelIndex = 0; channelIndex < source.mNumChannels;
             ++channelIndex) {
            const aiNodeAnim& channel = *source.mChannels[channelIndex];
            AnimationTrackData track;
            track.record.boneName =
                context.strings.Add(channel.mNodeName.C_Str());
            VecBounds(channel.mPositionKeys, channel.mNumPositionKeys,
                      track.record.positionMin, track.record.positionMax);
            VecBounds(channel.mScalingKeys, channel.mNumScalingKeys,
                      track.record.scaleMin, track.record.scaleMax);
            track.positions.reserve(channel.mNumPositionKeys);
            for (uint32_t i = 0; i < channel.mNumPositionKeys; ++i) {
                const auto& key = channel.mPositionKeys[i];
                Cooked::QuantizedVecKey value = {};
                value.time = QuantizeTime(key.mTime, source.mDuration);
                value.value[0] = QuantizeFloat(key.mValue.x,
                    track.record.positionMin[0], track.record.positionMax[0]);
                value.value[1] = QuantizeFloat(key.mValue.y,
                    track.record.positionMin[1], track.record.positionMax[1]);
                value.value[2] = QuantizeFloat(key.mValue.z,
                    track.record.positionMin[2], track.record.positionMax[2]);
                track.positions.push_back(value);
            }
            track.rotations.reserve(channel.mNumRotationKeys);
            for (uint32_t i = 0; i < channel.mNumRotationKeys; ++i) {
                const auto& key = channel.mRotationKeys[i];
                Cooked::QuantizedQuatKey value = {};
                value.time = QuantizeTime(key.mTime, source.mDuration);
                value.value[0] = QuantizeSnorm(key.mValue.x);
                value.value[1] = QuantizeSnorm(key.mValue.y);
                value.value[2] = QuantizeSnorm(key.mValue.z);
                value.value[3] = QuantizeSnorm(key.mValue.w);
                track.rotations.push_back(value);
            }
            track.scales.reserve(channel.mNumScalingKeys);
            for (uint32_t i = 0; i < channel.mNumScalingKeys; ++i) {
                const auto& key = channel.mScalingKeys[i];
                Cooked::QuantizedVecKey value = {};
                value.time = QuantizeTime(key.mTime, source.mDuration);
                value.value[0] = QuantizeFloat(key.mValue.x,
                    track.record.scaleMin[0], track.record.scaleMax[0]);
                value.value[1] = QuantizeFloat(key.mValue.y,
                    track.record.scaleMin[1], track.record.scaleMax[1]);
                value.value[2] = QuantizeFloat(key.mValue.z,
                    track.record.scaleMin[2], track.record.scaleMax[2]);
                track.scales.push_back(value);
            }
            track.record.positionCount =
                static_cast<uint32_t>(track.positions.size());
            track.record.rotationCount =
                static_cast<uint32_t>(track.rotations.size());
            track.record.scaleCount =
                static_cast<uint32_t>(track.scales.size());
            clip.tracks.push_back(std::move(track));
        }
        clip.record.trackCount = static_cast<uint32_t>(clip.tracks.size());
        context.clips.push_back(std::move(clip));
    }
}

bool WriteAsset(CookContext& context, const fs::path& destination) {
    Cooked::Header header;
    header.headerSize = sizeof(Cooked::Header);
    header.sourceHash = HashFile(context.sourcePath);
    header.sourceSize = fs::file_size(context.sourcePath);
    header.sourceWriteTime = fs::last_write_time(context.sourcePath)
        .time_since_epoch().count();
    header.primitiveCount = static_cast<uint32_t>(context.primitives.size());
    header.materialCount = static_cast<uint32_t>(context.materials.size());
    header.textureCount = static_cast<uint32_t>(context.textures.size());
    header.clipCount = static_cast<uint32_t>(context.clips.size());
    header.flags = 0;
    if (!context.primitives.empty())
        header.flags |= Cooked::HasGeometry | Cooked::GPUReadyVertices |
            Cooked::OptimizedIndices | Cooked::PrebuiltMeshlets;
    if (!context.textures.empty()) header.flags |= Cooked::HasTextures;
    if (!context.clips.empty()) header.flags |= Cooked::HasAnimations;

    BlobBuilder blob(sizeof(Cooked::Header));
    header.primitiveOffset = blob.Append(nullptr,
        context.primitives.size() * sizeof(Cooked::Primitive));
    header.materialOffset = blob.Append(nullptr,
        context.materials.size() * sizeof(Cooked::Material));
    header.textureOffset = blob.Append(nullptr,
        context.textures.size() * sizeof(Cooked::Texture));
    header.clipOffset = blob.Append(nullptr,
        context.clips.size() * sizeof(Cooked::Clip));
    header.stringOffset = blob.Append(context.strings.data.data(),
        context.strings.data.size(), 1);
    header.stringSize = context.strings.data.size();
    header.payloadOffset = AlignUp(blob.bytes.size(), 16);
    blob.bytes.resize(static_cast<size_t>(header.payloadOffset), 0);

    std::vector<Cooked::Texture> textureRecords;
    textureRecords.reserve(context.textures.size());
    for (EncodedTexture& texture : context.textures) {
        texture.record.data = blob.Append(texture.data);
        textureRecords.push_back(texture.record);
    }

    std::vector<Cooked::Primitive> primitiveRecords;
    primitiveRecords.reserve(context.primitives.size());
    for (PrimitiveData& primitive : context.primitives) {
        primitive.record.vertices = blob.Append(primitive.vertices);
        if (primitive.record.indexElementSize == 2) {
            std::vector<uint16_t> indices(primitive.indices.size());
            std::transform(primitive.indices.begin(), primitive.indices.end(),
                indices.begin(), [](uint32_t value) {
                    return static_cast<uint16_t>(value);
                });
            primitive.record.indices = blob.Append(indices);
        } else {
            primitive.record.indices = blob.Append(primitive.indices);
        }
        primitive.record.meshlets =
            blob.Append(primitive.meshlets.descriptors);
        primitive.record.meshletBounds =
            blob.Append(primitive.meshlets.bounds);
        primitive.record.meshletVertices =
            blob.Append(primitive.meshlets.vertices);
        primitive.record.meshletTriangles =
            blob.Append(primitive.meshlets.triangles);
        primitiveRecords.push_back(primitive.record);
    }

    std::vector<Cooked::Clip> clipRecords;
    clipRecords.reserve(context.clips.size());
    for (AnimationClipData& clip : context.clips) {
        std::vector<Cooked::Track> trackRecords;
        trackRecords.reserve(clip.tracks.size());
        for (AnimationTrackData& track : clip.tracks) {
            track.record.positions = blob.Append(track.positions);
            track.record.rotations = blob.Append(track.rotations);
            track.record.scales = blob.Append(track.scales);
            trackRecords.push_back(track.record);
        }
        clip.record.tracks = blob.Append(trackRecords);
        clipRecords.push_back(clip.record);
    }

    blob.Patch(header.primitiveOffset, primitiveRecords);
    blob.Patch(header.materialOffset, context.materials);
    blob.Patch(header.textureOffset, textureRecords);
    blob.Patch(header.clipOffset, clipRecords);
    header.fileSize = blob.bytes.size();
    header.payloadSize = header.fileSize - header.payloadOffset;
    header.contentHash = HashBytes(
        blob.bytes.data() + header.payloadOffset,
        static_cast<size_t>(header.payloadSize));
    std::memcpy(blob.bytes.data(), &header, sizeof(header));

    fs::create_directories(destination.parent_path());
    const fs::path temporary = destination.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream.write(reinterpret_cast<const char*>(blob.bytes.data()),
                     static_cast<std::streamsize>(blob.bytes.size()));
        if (!stream) return false;
    }
    std::error_code error;
    fs::remove(destination, error);
    error.clear();
    fs::rename(temporary, destination, error);
    if (error) {
        fs::remove(temporary);
        return false;
    }
    std::cout << destination.generic_string() << ": "
              << context.primitives.size() << " primitives, "
              << context.textures.size() << " BC textures, "
              << context.clips.size() << " compressed clips, "
              << blob.bytes.size() << " bytes\n";
    return true;
}

bool Cook(const fs::path& source, const fs::path& destination) {
    unsigned baseFlags = aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices |
        aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
        aiProcess_ImproveCacheLocality | aiProcess_OptimizeMeshes |
        aiProcess_OptimizeGraph | aiProcess_SortByPType |
        aiProcess_RemoveRedundantMaterials | aiProcess_LimitBoneWeights;
    if (LowerExtension(source) == ".fbx")
        baseFlags |= aiProcess_FlipWindingOrder;

    // Read animation channels before PreTransformVertices removes hierarchy
    // animation. Geometry then gets a second, static-only flattened import.
    Assimp::Importer animationImporter;
    const aiScene* animationScene =
        animationImporter.ReadFile(source.string(), baseFlags);
    if (!animationScene ||
        (!animationScene->HasMeshes() && animationScene->mNumAnimations == 0)) {
        std::cerr << source.generic_string() << ": "
                  << animationImporter.GetErrorString() << "\n";
        return false;
    }
    CookContext context;
    context.sourcePath = source;
    context.scene = animationScene;
    ExtractAnimations(context);

    if (animationScene->HasMeshes()) {
        Assimp::Importer geometryImporter;
        const aiScene* scene = geometryImporter.ReadFile(
            source.string(), baseFlags | aiProcess_PreTransformVertices);
        if (!scene || !scene->HasMeshes()) {
            std::cerr << source.generic_string() << ": "
                      << geometryImporter.GetErrorString() << "\n";
            return false;
        }
        context.scene = scene;
        ExtractMaterials(context);
        context.primitives.reserve(scene->mNumMeshes);
        for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
            context.primitives.push_back(ExtractPrimitive(
                *scene->mMeshes[i], context.strings, context.materials));
    }
    return WriteAsset(context, destination);
}

void Usage() {
    std::cerr << "AssetCooker <input.fbx|glb|gltf> <output.sgeasset>\n"
                 "AssetCooker --all <content-root> --out <cooked-root>\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) != "--all")
            return Cook(fs::path(argv[1]), fs::path(argv[2])) ? 0 : 1;
        if (argc == 5 && std::string(argv[1]) == "--all" &&
            std::string(argv[3]) == "--out") {
            const fs::path root = fs::absolute(argv[2]).lexically_normal();
            const fs::path output = fs::absolute(argv[4]).lexically_normal();
            uint32_t cooked = 0;
            uint32_t failed = 0;
            for (const fs::directory_entry& entry :
                 fs::recursive_directory_iterator(root)) {
                if (!entry.is_regular_file() || !IsModel(entry.path())) continue;
                fs::path relative = fs::relative(entry.path(), root);
                relative.replace_extension(".sgeasset");
                if (Cook(entry.path(), output / relative)) ++cooked;
                else ++failed;
            }
            std::cout << "Cook complete: " << cooked << " succeeded, "
                      << failed << " failed\n";
            return failed ? 1 : 0;
        }
        Usage();
        return 2;
    } catch (const std::exception& exception) {
        std::cerr << "AssetCooker: " << exception.what() << "\n";
        return 1;
    }
}
