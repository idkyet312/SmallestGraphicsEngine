#include "CookedAssetLoader.h"

#include "CookedAssetFormat.h"
#include "StaticBufferDX12.h"
#include "TextureUploadArenaDX12.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

namespace fs = std::filesystem;
namespace Cooked = SGE::Cooked;
using Microsoft::WRL::ComPtr;

namespace {

static_assert(sizeof(Cooked::MeshletDesc) == sizeof(MeshletDescDX12));
static_assert(sizeof(Cooked::MeshletBounds) == sizeof(MeshletBoundsDX12));

struct MappedFile {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const uint8_t* data = nullptr;
    uint64_t size = 0;

    ~MappedFile() {
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    bool Open(const fs::path& path, std::string& error) {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                           nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = "cannot open cooked asset";
            return false;
        }
        LARGE_INTEGER length = {};
        if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0) {
            error = "invalid cooked asset size";
            return false;
        }
        size = static_cast<uint64_t>(length.QuadPart);
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) {
            error = "cannot create cooked asset mapping";
            return false;
        }
        data = static_cast<const uint8_t*>(
            MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (!data) {
            error = "cannot map cooked asset";
            return false;
        }
        return true;
    }
};

// Large cooked assets are loaded synchronously because their texture copies
// record onto the render thread's active command list. Keep the window's
// message queue moving while validation and CPU-to-upload copies run; otherwise
// Windows classifies the process as hung and can terminate it before the first
// level-load task completes.
void PumpPendingWindowMessages() {
    MSG message = {};
    const auto pumpRange = [&message](UINT first, UINT last) {
        while (PeekMessageW(&message, nullptr, first, last, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    };
    // WM_SIZE rebuilds swap-chain and renderer resources. Leave it queued for
    // the main loop, which will handle it after the loading frame's command
    // list has been submitted rather than resetting that list mid-recording.
    pumpRange(0, WM_SIZE - 1);
    pumpRange(WM_SIZE + 1, (std::numeric_limits<UINT>::max)());
}

// Local spellings kept so the many call sites below read unchanged; the bodies
// now live on CookedAssetLoader so the collision cache can key its trees on the
// identical hash.
uint64_t HashBytes(const void* data, size_t size,
                   uint64_t hash = 1469598103934665603ull) {
    return CookedAssetLoader::HashBytes(data, size, hash);
}

uint64_t HashFile(const fs::path& path) {
    return CookedAssetLoader::HashFile(path);
}

const char* GetString(const MappedFile& map, const Cooked::Header& header,
                      uint32_t offset) {
    if (offset >= header.stringSize) return "";
    const char* begin = reinterpret_cast<const char*>(
        map.data + header.stringOffset + offset);
    const size_t remaining =
        static_cast<size_t>(header.stringSize - offset);
    return std::memchr(begin, '\0', remaining) ? begin : "";
}

DXGI_FORMAT TextureFormat(Cooked::TextureFormat format) {
    switch (format) {
    case Cooked::TextureFormat::BC3: return DXGI_FORMAT_BC3_UNORM;
    case Cooked::TextureFormat::BC5: return DXGI_FORMAT_BC5_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

ComPtr<ID3D12Resource> CreateTexture(
    const MappedFile& map, const Cooked::Texture& source,
    ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
    std::vector<ComPtr<ID3D12Resource>>& uploads) {
    if (!device || !commandList || !source.width || !source.height ||
        !source.mipCount || source.mipCount > Cooked::kMaxTextureMips ||
        !Cooked::RangeValid(source.data, source.dataSize, map.size))
        return {};
    const DXGI_FORMAT format = TextureFormat(source.format);
    if (format == DXGI_FORMAT_UNKNOWN) return {};

    D3D12_RESOURCE_DESC description = {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = source.width;
    description.Height = source.height;
    description.DepthOrArraySize = 1;
    description.MipLevels = static_cast<UINT16>(source.mipCount);
    description.Format = format;
    description.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&texture))))
        return {};

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(
        source.mipCount);
    std::vector<UINT> rowCounts(source.mipCount);
    std::vector<UINT64> rowSizes(source.mipCount);
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&description, 0, source.mipCount, 0,
        footprints.data(), rowCounts.data(), rowSizes.data(), &uploadSize);

    ComPtr<ID3D12Resource> upload;
    ID3D12Resource* uploadResource = nullptr;
    uint8_t* mapped = nullptr;
    uint64_t pooledOffset = 0;
    const bool pooled = IsTextureUploadArenaActiveDX12();
    if (pooled) {
        const TextureUploadAllocationDX12 allocation =
            AllocateTextureUploadDX12(device, uploadSize);
        if (!allocation) return {};
        uploadResource = allocation.resource;
        mapped = allocation.cpuAddress;
        pooledOffset = allocation.offset;
    } else {
        D3D12_RESOURCE_DESC uploadDescription = {};
        uploadDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDescription.Width = uploadSize;
        uploadDescription.Height = 1;
        uploadDescription.DepthOrArraySize = 1;
        uploadDescription.MipLevels = 1;
        uploadDescription.SampleDesc.Count = 1;
        uploadDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDescription,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&upload)))) return {};
        const D3D12_RANGE noRead = { 0, 0 };
        if (FAILED(upload->Map(0, &noRead,
                reinterpret_cast<void**>(&mapped)))) return {};
        uploadResource = upload.Get();
    }
    bool valid = true;
    for (uint32_t mip = 0; mip < source.mipCount; ++mip) {
        if (source.mipOffsets[mip] > source.dataSize ||
            source.mipSizes[mip] >
                source.dataSize - source.mipOffsets[mip]) {
            valid = false;
            break;
        }
        const uint32_t width = (std::max)(1u, source.width >> mip);
        const uint32_t height = (std::max)(1u, source.height >> mip);
        const uint32_t blockRows = (std::max)(1u, (height + 3) / 4);
        const uint32_t sourceRowBytes =
            (std::max)(1u, (width + 3) / 4) * 16;
        if (uint64_t(sourceRowBytes) * blockRows >
            source.mipSizes[mip]) {
            valid = false;
            break;
        }
        const uint8_t* sourceData =
            map.data + source.data + source.mipOffsets[mip];
        uint8_t* destination =
            mapped + footprints[mip].Offset;
        for (uint32_t row = 0; row < blockRows; ++row)
            std::memcpy(destination +
                    static_cast<size_t>(row) *
                        footprints[mip].Footprint.RowPitch,
                sourceData + static_cast<size_t>(row) * sourceRowBytes,
                sourceRowBytes);
    }
    if (pooled) {
        for (auto& footprint : footprints) footprint.Offset += pooledOffset;
    } else {
        upload->Unmap(0, nullptr);
    }
    // Outside a pooled level load, park the heap before the validity bailout.
    // The arena already owns pooled ranges through the final queue drain.
    if (!pooled) uploads.push_back(upload);
    if (!valid) return {};

    for (uint32_t mip = 0; mip < source.mipCount; ++mip) {
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = mip;
        D3D12_TEXTURE_COPY_LOCATION origin = {};
        origin.pResource = uploadResource;
        origin.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        origin.PlacedFootprint = footprints[mip];
        commandList->CopyTextureRegion(
            &destination, 0, 0, 0, &origin, nullptr);
    }
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
    // `upload` was already added to `uploads` above, before the validity check.
    return texture;
}

bool ValidatePrimitive(const Cooked::Primitive& primitive,
                       uint64_t fileSize) {
    return primitive.vertexCount > 0 && primitive.indexCount > 0 &&
           Cooked::ArrayValid(primitive.vertices, primitive.vertexCount,
                              sizeof(Cooked::Vertex), fileSize) &&
           Cooked::ArrayValid(primitive.indices, primitive.indexCount,
                              primitive.indexElementSize, fileSize) &&
           Cooked::ArrayValid(primitive.meshlets, primitive.meshletCount,
                              sizeof(Cooked::MeshletDesc), fileSize) &&
           Cooked::ArrayValid(primitive.meshletBounds, primitive.meshletCount,
                              sizeof(Cooked::MeshletBounds), fileSize) &&
           Cooked::ArrayValid(primitive.meshletVertices,
                              primitive.meshletVertexCount, sizeof(uint32_t),
                              fileSize) &&
           Cooked::ArrayValid(primitive.meshletTriangles,
                              primitive.meshletTriangleCount, sizeof(uint32_t),
                              fileSize) &&
           (primitive.indexElementSize == 2 ||
            primitive.indexElementSize == 4);
}

template<class T>
ComPtr<ID3D12Resource> StaticBuffer(
    ID3D12Device* device, const T* data, uint32_t count,
    D3D12_RESOURCE_STATES state, const char* label) {
    ComPtr<ID3D12Resource> result;
    if (!count || !CreateStaticBufferDX12(
            device, data, uint64_t(count) * sizeof(T), state, result, label))
        return {};
    return result;
}

} // namespace

uint64_t CookedAssetLoader::HashBytes(const void* data, size_t size,
                                      uint64_t hash) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
        if ((i & ((4u * 1024u * 1024u) - 1u)) == 0u && i != 0u)
            PumpPendingWindowMessages();
    }
    return hash;
}

uint64_t CookedAssetLoader::HashFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return 0;
    std::array<char, 64 * 1024> block{};
    uint64_t hash = 1469598103934665603ull;
    while (stream) {
        stream.read(block.data(), block.size());
        hash = CookedAssetLoader::HashBytes(block.data(),
            static_cast<size_t>(stream.gcount()), hash);
        PumpPendingWindowMessages();
    }
    return hash;
}

fs::path CookedAssetLoader::FindForSource(const fs::path& source) {
    if (source.extension() == ".sgeasset" && fs::exists(source))
        return source;
    fs::path sibling = source;
    sibling.replace_extension(".sgeasset");
    if (fs::exists(sibling)) return sibling;

    const std::string generic = source.lexically_normal().generic_string();
    constexpr const char* contentPrefix = "Content/";
    const size_t content = generic.find(contentPrefix);
    if (content != std::string::npos) {
        fs::path relative = generic.substr(content + std::strlen(contentPrefix));
        relative.replace_extension(".sgeasset");
        fs::path candidate = fs::path("Content/Cooked") / relative;
        if (fs::exists(candidate)) return candidate;
    }
    return {};
}

// Bisect helper for the cooked-asset GPU hang.
//
//   SGE_NO_COOKED=1           -> no cooked assets at all
//   SGE_COOKED_ONLY=ak47,SVD  -> cooked ONLY for sources matching a substring,
//                                everything else falls back to the raw import
//
// Matching is case-insensitive on the full source path, so "ak47", "Models/RPG7"
// and "shotgun" all work. Unset means normal behaviour (cooked for everything).
static bool CookedEnabledFor(const fs::path& source) {
    if (const char* disable = std::getenv("SGE_NO_COOKED");
        disable && disable[0] == '1')
        return false;

    const char* only = std::getenv("SGE_COOKED_ONLY");
    if (!only || !only[0]) return true;

    std::string haystack = source.generic_string();
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    std::string list(only);
    std::transform(list.begin(), list.end(), list.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    for (size_t start = 0; start <= list.size();) {
        const size_t comma = list.find(',', start);
        const size_t end = comma == std::string::npos ? list.size() : comma;
        const std::string token = list.substr(start, end - start);
        if (!token.empty() && haystack.find(token) != std::string::npos)
            return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

std::shared_ptr<SceneNode> CookedAssetLoader::LoadForSource(
    const fs::path& source, ComPtr<ID3D12Device> device,
    ComPtr<ID3D12GraphicsCommandList> commandList, std::string* error) {
    // Escape hatch: SGE_NO_COOKED=1 makes every caller fall back to importing
    // the original FBX/GLB, which is how assets loaded before the cooked
    // pipeline landed. Returning empty here is exactly the "no cooked asset"
    // path each caller already handles, so no call site needs to change.
    if (!CookedEnabledFor(source)) {
        if (error) *error = "cooked assets disabled for this source";
        return {};
    }

    const fs::path cooked = FindForSource(source);
    if (cooked.empty()) return {};
    if (fs::exists(source)) {
        std::string headerError;
        MappedFile headerMap;
        if (!headerMap.Open(cooked, headerError) ||
            headerMap.size < sizeof(Cooked::Header)) {
            if (error) *error = headerError.empty()
                ? "truncated cooked asset" : headerError;
            return {};
        }
        const Cooked::Header& header =
            *reinterpret_cast<const Cooked::Header*>(headerMap.data);
        std::error_code ec;
        const uint64_t sourceSize = fs::file_size(source, ec);
        if (ec || header.sourceSize != sourceSize ||
            header.sourceHash != HashFile(source)) {
            if (error) *error = "cooked asset is stale";
            return {};
        }
    }
    return Load(cooked, std::move(device), std::move(commandList), error);
}

std::shared_ptr<SceneNode> CookedAssetLoader::Load(
    const fs::path& cookedPath, ComPtr<ID3D12Device> device,
    ComPtr<ID3D12GraphicsCommandList> commandList, std::string* error) {
    std::string localError;
    MappedFile map;
    if (!map.Open(cookedPath, localError)) {
        if (error) *error = localError;
        return {};
    }
    if (map.size < sizeof(Cooked::Header)) {
        if (error) *error = "truncated cooked asset";
        return {};
    }
    const Cooked::Header& header =
        *reinterpret_cast<const Cooked::Header*>(map.data);
    if (!Cooked::HeaderValid(header, map.size)) {
        if (error) *error = "invalid cooked asset header";
        return {};
    }
    if (HashBytes(map.data + header.payloadOffset,
                  static_cast<size_t>(header.payloadSize)) !=
        header.contentHash) {
        if (error) *error = "cooked asset payload hash mismatch";
        return {};
    }

    const auto* materialRecords =
        reinterpret_cast<const Cooked::Material*>(
            map.data + header.materialOffset);
    const auto* textureRecords =
        reinterpret_cast<const Cooked::Texture*>(
            map.data + header.textureOffset);
    const auto* primitiveRecords =
        reinterpret_cast<const Cooked::Primitive*>(
            map.data + header.primitiveOffset);

    std::vector<ComPtr<ID3D12Resource>> textureUploads;
    std::vector<ComPtr<ID3D12Resource>> textures(header.textureCount);
    for (uint32_t i = 0; i < header.textureCount; ++i) {
        textures[i] = CreateTexture(map, textureRecords[i], device.Get(),
                                    commandList.Get(), textureUploads);
        PumpPendingWindowMessages();
    }

    std::vector<std::shared_ptr<SceneMaterial>> materials;
    materials.reserve((std::max)(1u, header.materialCount));
    for (uint32_t i = 0; i < header.materialCount; ++i) {
        const Cooked::Material& source = materialRecords[i];
        auto material = std::make_shared<SceneMaterial>();
        material->name = GetString(map, header, source.name);
        std::memcpy(&material->baseColorFactor, source.baseColor,
                    sizeof(source.baseColor));
        material->metallicFactor = source.metallic;
        material->roughnessFactor = source.roughness;
        material->doubleSided =
            (source.flags & Cooked::DoubleSided) != 0;
        material->roughnessOnlyTexture =
            (source.flags & Cooked::RoughnessOnly) != 0;
        material->alphaCutout =
            (source.flags & Cooked::AlphaCutout) != 0;
        material->alphaBlend =
            (source.flags & Cooked::AlphaBlend) != 0;
        // Caches cooked before alphaCutoff existed store zero; fall back to the
        // glTF default rather than clipping everything away.
        material->alphaCutoff =
            source.alphaCutoff > 0.0f ? source.alphaCutoff : 0.5f;
        // Cooked cutouts are foliage in every shipped asset (palm, fern,
        // dandelion), and the 0.20 clip plus the edge-bleed and dark-texel lift
        // were tuned against them. Keep that path until a cooked hard-surface
        // cutout needs otherwise.
        material->foliageShading = material->alphaCutout;
        if (material->foliageShading) material->alphaCutoff = 0.20f;
        if (source.baseColorTexture < textures.size())
            material->baseColorTexture = textures[source.baseColorTexture];
        if (source.normalTexture < textures.size())
            material->normalTexture = textures[source.normalTexture];
        if (source.metallicRoughnessTexture < textures.size())
            material->metallicRoughnessTexture =
                textures[source.metallicRoughnessTexture];
        material->uploadHeaps.insert(material->uploadHeaps.end(),
            textureUploads.begin(), textureUploads.end());
        materials.push_back(std::move(material));
    }
    if (materials.empty()) {
        auto material = std::make_shared<SceneMaterial>();
        material->uploadHeaps = textureUploads;
        materials.push_back(std::move(material));
    }

    auto root = std::make_shared<SceneNode>(
        cookedPath.stem().string() + "_Cooked");
    root->mesh = std::make_shared<SceneMesh>();
    root->mesh->name = root->name;
    root->mesh->primitives.reserve(header.primitiveCount);
    for (uint32_t i = 0; i < header.primitiveCount; ++i) {
        const Cooked::Primitive& source = primitiveRecords[i];
        if (!ValidatePrimitive(source, map.size)) {
            if (error) *error = "invalid cooked primitive range";
            return {};
        }
        MeshPrimitive primitive;
        primitive.materialIndex = source.material < materials.size()
            ? static_cast<int>(source.material) : -1;
        primitive.material = source.material < materials.size()
            ? materials[source.material] : materials.front();
        primitive.boundsMin = { source.boundsMin[0], source.boundsMin[1],
                                source.boundsMin[2] };
        primitive.boundsMax = { source.boundsMax[0], source.boundsMax[1],
                                source.boundsMax[2] };
        primitive.boundsValid = true;
        primitive.indexCount = source.indexCount;
        primitive.meshletCount = source.meshletCount;

        const auto* vertices = reinterpret_cast<const Cooked::Vertex*>(
            map.data + source.vertices);
        primitive.vertices.resize(static_cast<size_t>(source.vertexCount) * 12);
        std::memcpy(primitive.vertices.data(), vertices,
            static_cast<size_t>(source.vertexCount) * sizeof(Cooked::Vertex));
        if (source.indexElementSize == 2) {
            const auto* indices = reinterpret_cast<const uint16_t*>(
                map.data + source.indices);
            primitive.indices.assign(indices, indices + source.indexCount);
        } else {
            const auto* indices = reinterpret_cast<const uint32_t*>(
                map.data + source.indices);
            primitive.indices.assign(indices, indices + source.indexCount);
        }

        primitive.vertexBuffer = StaticBuffer(device.Get(), vertices,
            source.vertexCount,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            "CookedVertexBuffer");
        if (!primitive.vertexBuffer) {
            if (error) *error = "failed to create cooked vertex buffer";
            return {};
        }
        primitive.vbv.BufferLocation =
            primitive.vertexBuffer->GetGPUVirtualAddress();
        primitive.vbv.SizeInBytes =
            source.vertexCount * sizeof(Cooked::Vertex);
        primitive.vbv.StrideInBytes = sizeof(Cooked::Vertex);

        const void* indexData = map.data + source.indices;
        const uint64_t indexBytes =
            uint64_t(source.indexCount) * source.indexElementSize;
        if (!CreateStaticBufferDX12(device.Get(), indexData, indexBytes,
                D3D12_RESOURCE_STATE_INDEX_BUFFER |
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                primitive.indexBuffer,
                "CookedIndexBuffer")) {
            if (error) *error = "failed to create cooked index buffer";
            return {};
        }
        primitive.ibv.BufferLocation =
            primitive.indexBuffer->GetGPUVirtualAddress();
        primitive.ibv.SizeInBytes = static_cast<UINT>(indexBytes);
        primitive.ibv.Format = source.indexElementSize == 2
            ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

        primitive.meshletDescBuffer = StaticBuffer(device.Get(),
            reinterpret_cast<const MeshletDescDX12*>(
                map.data + source.meshlets), source.meshletCount,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            "CookedMeshletDesc");
        primitive.meshletBoundsBuffer = StaticBuffer(device.Get(),
            reinterpret_cast<const MeshletBoundsDX12*>(
                map.data + source.meshletBounds), source.meshletCount,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            "CookedMeshletBounds");
        primitive.meshletVertexIndexBuffer = StaticBuffer(device.Get(),
            reinterpret_cast<const uint32_t*>(
                map.data + source.meshletVertices),
            source.meshletVertexCount,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            "CookedMeshletVertices");
        primitive.meshletTriangleBuffer = StaticBuffer(device.Get(),
            reinterpret_cast<const uint32_t*>(
                map.data + source.meshletTriangles),
            source.meshletTriangleCount,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            "CookedMeshletTriangles");
        root->mesh->primitives.push_back(std::move(primitive));
        PumpPendingWindowMessages();
    }
    root->UpdateGlobalTransform(root->localTransform);
    return root;
}

bool CookedAssetLoader::LoadAnimationsForSource(
    const fs::path& sourcePath, const Skeleton& skeleton,
    std::vector<AnimationClip>& clips, std::string* error) {
    // Same kill switch as LoadForSource, so SGE_NO_COOKED=1 gives a fully
    // uncooked run rather than cooked animations on uncooked meshes.
    if (!CookedEnabledFor(sourcePath)) {
        if (error) *error = "cooked assets disabled for this source";
        return false;
    }

    const fs::path cookedPath = FindForSource(sourcePath);
    if (cookedPath.empty()) return false;
    if (fs::exists(sourcePath)) {
        std::string headerError;
        MappedFile headerMap;
        if (!headerMap.Open(cookedPath, headerError) ||
            headerMap.size < sizeof(Cooked::Header)) {
            if (error) *error = headerError.empty()
                ? "truncated cooked animation" : headerError;
            return false;
        }
        const Cooked::Header& sourceHeader =
            *reinterpret_cast<const Cooked::Header*>(headerMap.data);
        std::error_code ec;
        const uint64_t sourceSize = fs::file_size(sourcePath, ec);
        if (ec || sourceHeader.sourceSize != sourceSize ||
            sourceHeader.sourceHash != HashFile(sourcePath)) {
            if (error) *error = "cooked animation is stale";
            return false;
        }
    }

    std::string localError;
    MappedFile map;
    if (!map.Open(cookedPath, localError)) {
        if (error) *error = localError;
        return false;
    }
    if (map.size < sizeof(Cooked::Header)) {
        if (error) *error = "truncated cooked animation";
        return false;
    }
    const Cooked::Header& header =
        *reinterpret_cast<const Cooked::Header*>(map.data);
    if (!Cooked::HeaderValid(header, map.size) ||
        HashBytes(map.data + header.payloadOffset,
                  static_cast<size_t>(header.payloadSize)) !=
            header.contentHash) {
        if (error) *error = "invalid cooked animation";
        return false;
    }
    if (!header.clipCount) return false;
    const auto* sourceClips = reinterpret_cast<const Cooked::Clip*>(
        map.data + header.clipOffset);
    const size_t firstClip = clips.size();
    for (uint32_t clipIndex = 0; clipIndex < header.clipCount; ++clipIndex) {
        const Cooked::Clip& sourceClip = sourceClips[clipIndex];
        if (!Cooked::ArrayValid(sourceClip.tracks, sourceClip.trackCount,
                                sizeof(Cooked::Track), map.size)) {
            clips.resize(firstClip);
            if (error) *error = "invalid cooked animation tracks";
            return false;
        }
        AnimationClip clip;
        clip.name = GetString(map, header, sourceClip.name);
        clip.duration = sourceClip.durationSeconds;
        const auto* sourceTracks = reinterpret_cast<const Cooked::Track*>(
            map.data + sourceClip.tracks);
        clip.tracks.reserve(sourceClip.trackCount);
        for (uint32_t trackIndex = 0; trackIndex < sourceClip.trackCount;
             ++trackIndex) {
            const Cooked::Track& sourceTrack = sourceTracks[trackIndex];
            if (!Cooked::ArrayValid(sourceTrack.positions,
                    sourceTrack.positionCount,
                    sizeof(Cooked::QuantizedVecKey), map.size) ||
                !Cooked::ArrayValid(sourceTrack.rotations,
                    sourceTrack.rotationCount,
                    sizeof(Cooked::QuantizedQuatKey), map.size) ||
                !Cooked::ArrayValid(sourceTrack.scales,
                    sourceTrack.scaleCount,
                    sizeof(Cooked::QuantizedVecKey), map.size)) {
                clips.resize(firstClip);
                if (error) *error = "invalid cooked animation keys";
                return false;
            }
            const int bone =
                skeleton.Find(GetString(map, header, sourceTrack.boneName));
            if (bone < 0) continue;
            BoneTrack track;
            track.bone = bone;
            const auto decodeTime = [&](uint16_t time) {
                return float(time) * (clip.duration / 65535.0f);
            };
            const auto decodeVec = [](const uint16_t value[3],
                                      const float minimum[3],
                                      const float maximum[3]) {
                DirectX::XMFLOAT3 result;
                float* output = &result.x;
                for (uint32_t c = 0; c < 3; ++c)
                    output[c] = minimum[c] +
                        (maximum[c] - minimum[c]) *
                        (float(value[c]) / 65535.0f);
                return result;
            };
            const auto* positions =
                reinterpret_cast<const Cooked::QuantizedVecKey*>(
                    map.data + sourceTrack.positions);
            track.positions.reserve(sourceTrack.positionCount);
            for (uint32_t i = 0; i < sourceTrack.positionCount; ++i)
                track.positions.push_back({
                    decodeTime(positions[i].time),
                    decodeVec(positions[i].value, sourceTrack.positionMin,
                              sourceTrack.positionMax) });
            const auto* rotations =
                reinterpret_cast<const Cooked::QuantizedQuatKey*>(
                    map.data + sourceTrack.rotations);
            track.rotations.reserve(sourceTrack.rotationCount);
            for (uint32_t i = 0; i < sourceTrack.rotationCount; ++i) {
                DirectX::XMFLOAT4 rotation(
                    float(rotations[i].value[0]) / 32767.0f,
                    float(rotations[i].value[1]) / 32767.0f,
                    float(rotations[i].value[2]) / 32767.0f,
                    float(rotations[i].value[3]) / 32767.0f);
                DirectX::XMStoreFloat4(&rotation,
                    DirectX::XMQuaternionNormalize(
                        DirectX::XMLoadFloat4(&rotation)));
                track.rotations.push_back(
                    { decodeTime(rotations[i].time), rotation });
            }
            const auto* scales =
                reinterpret_cast<const Cooked::QuantizedVecKey*>(
                    map.data + sourceTrack.scales);
            track.scales.reserve(sourceTrack.scaleCount);
            for (uint32_t i = 0; i < sourceTrack.scaleCount; ++i)
                track.scales.push_back({
                    decodeTime(scales[i].time),
                    decodeVec(scales[i].value, sourceTrack.scaleMin,
                              sourceTrack.scaleMax) });
            clip.tracks.push_back(std::move(track));
        }
        clips.push_back(std::move(clip));
    }
    return clips.size() > firstClip;
}
