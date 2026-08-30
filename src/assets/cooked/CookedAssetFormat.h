#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace SGE::Cooked {

constexpr uint64_t kMagic = 0x5445535341454753ull; // "SGEASSET"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kEndianTag = 0x01020304u;
constexpr uint32_t kInvalidIndex =
    (std::numeric_limits<uint32_t>::max)();
constexpr uint32_t kMaxTextureMips = 16;

enum class TextureFormat : uint32_t {
    None = 0,
    BC3 = 1,
    BC5 = 2,
};

enum AssetFlags : uint32_t {
    HasGeometry = 1u << 0,
    HasTextures = 1u << 1,
    HasAnimations = 1u << 2,
    GPUReadyVertices = 1u << 3,
    OptimizedIndices = 1u << 4,
    PrebuiltMeshlets = 1u << 5,
};

struct Header {
    uint64_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t endianTag = kEndianTag;
    uint32_t headerSize = 0;
    uint32_t flags = 0;
    uint64_t fileSize = 0;
    uint64_t sourceHash = 0;
    uint64_t sourceSize = 0;
    int64_t sourceWriteTime = 0;
    uint32_t primitiveCount = 0;
    uint32_t materialCount = 0;
    uint32_t textureCount = 0;
    uint32_t clipCount = 0;
    uint64_t primitiveOffset = 0;
    uint64_t materialOffset = 0;
    uint64_t textureOffset = 0;
    uint64_t clipOffset = 0;
    uint64_t stringOffset = 0;
    uint64_t stringSize = 0;
    uint64_t payloadOffset = 0;
    uint64_t payloadSize = 0;
    uint64_t contentHash = 0;
    uint64_t reserved[4] = {};
};

// Exact engine GPU layout: position, normal, UV, tangent. Keeping this section
// GPU-ready avoids runtime vertex parsing or repacking.
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    float tangent[4];
};
static_assert(sizeof(Vertex) == 48);

struct MeshletDesc {
    uint32_t vertexOffset;
    uint32_t vertexCount;
    uint32_t triangleOffset;
    uint32_t triangleCount;
};

struct MeshletBounds {
    float boundsMin[3];
    float padding0;
    float boundsMax[3];
    float padding1;
    float sphereCenter[3];
    float sphereRadius;
    float coneAxis[3];
    float coneCutoff;
};

struct Primitive {
    uint32_t name = 0;
    uint32_t material = kInvalidIndex;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t indexElementSize = 4;
    uint32_t meshletCount = 0;
    uint32_t meshletVertexCount = 0;
    uint32_t meshletTriangleCount = 0;
    float boundsMin[3] = {};
    float boundsMax[3] = {};
    uint64_t vertices = 0;
    uint64_t indices = 0;
    uint64_t meshlets = 0;
    uint64_t meshletBounds = 0;
    uint64_t meshletVertices = 0;
    uint64_t meshletTriangles = 0;
};

enum MaterialFlags : uint32_t {
    DoubleSided = 1u << 0,
    RoughnessOnly = 1u << 1,
    AlphaCutout = 1u << 2,
    AlphaBlend = 1u << 3,
};

struct Material {
    uint32_t name = 0;
    uint32_t flags = 0;
    uint32_t baseColorTexture = kInvalidIndex;
    uint32_t normalTexture = kInvalidIndex;
    uint32_t metallicRoughnessTexture = kInvalidIndex;
    uint32_t reserved0 = 0;
    float baseColor[4] = { 1, 1, 1, 1 };
    float metallic = 1.0f;
    float roughness = 1.0f;
    float reserved1[2] = {};
};

struct Texture {
    uint32_t name = 0;
    uint32_t source = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 0;
    TextureFormat format = TextureFormat::None;
    uint32_t dataSize = 0;
    uint32_t reserved = 0;
    uint64_t data = 0;
    uint32_t mipOffsets[kMaxTextureMips] = {};
    uint32_t mipSizes[kMaxTextureMips] = {};
};

// Animation key values are normalized 16-bit. Vec tracks carry decode bounds.
// Quaternion components are signed normalized int16.
struct QuantizedVecKey {
    uint16_t time;
    uint16_t value[3];
};

struct QuantizedQuatKey {
    uint16_t time;
    int16_t value[4];
};

struct Track {
    uint32_t boneName = 0;
    uint32_t positionCount = 0;
    uint32_t rotationCount = 0;
    uint32_t scaleCount = 0;
    float positionMin[3] = {};
    float positionMax[3] = {};
    float scaleMin[3] = {};
    float scaleMax[3] = {};
    uint64_t positions = 0;
    uint64_t rotations = 0;
    uint64_t scales = 0;
};

struct Clip {
    uint32_t name = 0;
    uint32_t trackCount = 0;
    float durationSeconds = 0.0f;
    float reserved = 0.0f;
    uint64_t tracks = 0;
};

inline bool RangeValid(uint64_t offset, uint64_t size, uint64_t fileSize) {
    return offset <= fileSize && size <= fileSize - offset;
}

inline bool ArrayValid(uint64_t offset, uint64_t count, uint64_t elementSize,
                       uint64_t fileSize) {
    return elementSize == 0 ||
        (count <= (std::numeric_limits<uint64_t>::max)() / elementSize &&
         RangeValid(offset, count * elementSize, fileSize));
}

inline bool HeaderValid(const Header& header, uint64_t mappedSize) {
    if (header.magic != kMagic || header.version != kVersion ||
        header.endianTag != kEndianTag || header.headerSize != sizeof(Header) ||
        header.fileSize != mappedSize)
        return false;
    return ArrayValid(header.primitiveOffset, header.primitiveCount,
                      sizeof(Primitive), mappedSize) &&
           ArrayValid(header.materialOffset, header.materialCount,
                      sizeof(Material), mappedSize) &&
           ArrayValid(header.textureOffset, header.textureCount,
                      sizeof(Texture), mappedSize) &&
           ArrayValid(header.clipOffset, header.clipCount,
                      sizeof(Clip), mappedSize) &&
           RangeValid(header.stringOffset, header.stringSize, mappedSize) &&
           RangeValid(header.payloadOffset, header.payloadSize, mappedSize);
}

} // namespace SGE::Cooked
