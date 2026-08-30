#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
// Define these to avoid tinygltf including them if they are not found, 
// but we hope vcpkg provided them or we need to provide them. 
// Actually tinygltf header usually includes them.
#include <tiny_gltf.h>

#include "GLBImporter.h"
#include "CookedAssetLoader.h"
#include "MipGenerator.h"
#include "StaticBufferDX12.h"
#include "TextureUploadArenaDX12.h"
#include <iostream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <unordered_map>
#include <array>
#include <meshoptimizer.h>

#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>

using namespace DirectX;

MipGenerator g_mipGen;

// Helper to copy data from GLTF buffer to vector
template<typename T>
void CopyBufferData(const tinygltf::Model& model, int accessorIndex, std::vector<T>& outData) {
    if (accessorIndex < 0) return;
    const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
    const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
    const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
    
    const unsigned char* dataStart = &buffer.data[bufferView.byteOffset + accessor.byteOffset];
    size_t dataStep = accessor.ByteStride(bufferView);
    
    outData.reserve(accessor.count);
    for (size_t i = 0; i < accessor.count; i++) {
        const T* val = reinterpret_cast<const T*>(dataStart + i * dataStep);
        outData.push_back(*val);
    }
}

// Reads a JOINTS_n accessor. glTF allows unsigned byte or short here, and the
// two show up interchangeably depending on the exporter, so both are widened to
// the uint32 the SkinVertex palette index expects.
static void CopyJointIndices(const tinygltf::Model& model, int accessorIndex,
                             std::vector<std::array<uint32_t, 4>>& outData) {
    if (accessorIndex < 0) return;
    const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
    if (accessor.bufferView < 0) return;
    const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
    const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

    const unsigned char* dataStart =
        &buffer.data[bufferView.byteOffset + accessor.byteOffset];
    const size_t dataStep = accessor.ByteStride(bufferView);

    outData.reserve(accessor.count);
    for (size_t i = 0; i < accessor.count; i++) {
        const unsigned char* element = dataStart + i * dataStep;
        std::array<uint32_t, 4> joints{ 0, 0, 0, 0 };
        for (int c = 0; c < 4; c++) {
            if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                joints[c] = element[c];
            } else if (accessor.componentType ==
                       TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                unsigned short value = 0;
                memcpy(&value, element + c * sizeof(unsigned short),
                       sizeof(value));
                joints[c] = value;
            }
        }
        outData.push_back(joints);
    }
}

// Reads a WEIGHTS_n accessor. Floats are the common case; the normalized
// integer forms are also legal and are scaled back into 0..1 here.
static void CopyJointWeights(const tinygltf::Model& model, int accessorIndex,
                             std::vector<DirectX::XMFLOAT4>& outData) {
    if (accessorIndex < 0) return;
    const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
    if (accessor.bufferView < 0) return;
    const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
    const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

    const unsigned char* dataStart =
        &buffer.data[bufferView.byteOffset + accessor.byteOffset];
    const size_t dataStep = accessor.ByteStride(bufferView);

    outData.reserve(accessor.count);
    for (size_t i = 0; i < accessor.count; i++) {
        const unsigned char* element = dataStart + i * dataStep;
        float weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (int c = 0; c < 4; c++) {
            if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                memcpy(&weights[c], element + c * sizeof(float), sizeof(float));
            } else if (accessor.componentType ==
                       TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                weights[c] = element[c] / 255.0f;
            } else if (accessor.componentType ==
                       TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                unsigned short value = 0;
                memcpy(&value, element + c * sizeof(unsigned short),
                       sizeof(value));
                weights[c] = value / 65535.0f;
            }
        }
        outData.push_back(
            DirectX::XMFLOAT4(weights[0], weights[1], weights[2], weights[3]));
    }
}

static DirectX::XMFLOAT3 Add3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
    return DirectX::XMFLOAT3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static DirectX::XMFLOAT3 Sub3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
    return DirectX::XMFLOAT3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static DirectX::XMFLOAT3 Mul3(const DirectX::XMFLOAT3& a, float s) {
    return DirectX::XMFLOAT3(a.x * s, a.y * s, a.z * s);
}

static float Dot3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static DirectX::XMFLOAT3 Cross3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
    return DirectX::XMFLOAT3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x);
}

static DirectX::XMFLOAT3 Normalize3(const DirectX::XMFLOAT3& v) {
    float lenSq = Dot3(v, v);
    if (lenSq <= 1e-12f) return DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f);
    return Mul3(v, 1.0f / std::sqrt(lenSq));
}

static void GenerateTangents(const std::vector<DirectX::XMFLOAT3>& positions,
    const std::vector<DirectX::XMFLOAT3>& normals,
    const std::vector<DirectX::XMFLOAT2>& texCoords,
    const std::vector<unsigned int>& indices,
    std::vector<DirectX::XMFLOAT4>& tangents) {
    tangents.assign(positions.size(), DirectX::XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f));
    if (positions.size() < 3 || texCoords.size() != positions.size()) return;

    std::vector<DirectX::XMFLOAT3> tan1(positions.size(), DirectX::XMFLOAT3(0, 0, 0));
    std::vector<DirectX::XMFLOAT3> tan2(positions.size(), DirectX::XMFLOAT3(0, 0, 0));

    auto accumulateTri = [&](unsigned int i0, unsigned int i1, unsigned int i2) {
        if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size()) return;

        const DirectX::XMFLOAT3& p0 = positions[i0];
        const DirectX::XMFLOAT3& p1 = positions[i1];
        const DirectX::XMFLOAT3& p2 = positions[i2];
        const DirectX::XMFLOAT2& w0 = texCoords[i0];
        const DirectX::XMFLOAT2& w1 = texCoords[i1];
        const DirectX::XMFLOAT2& w2 = texCoords[i2];

        float x1 = p1.x - p0.x, x2 = p2.x - p0.x;
        float y1 = p1.y - p0.y, y2 = p2.y - p0.y;
        float z1 = p1.z - p0.z, z2 = p2.z - p0.z;
        float s1 = w1.x - w0.x, s2 = w2.x - w0.x;
        float t1 = w1.y - w0.y, t2 = w2.y - w0.y;

        float det = s1 * t2 - s2 * t1;
        if (std::abs(det) < 1e-8f) return;
        float r = 1.0f / det;

        DirectX::XMFLOAT3 sdir((t2 * x1 - t1 * x2) * r, (t2 * y1 - t1 * y2) * r, (t2 * z1 - t1 * z2) * r);
        DirectX::XMFLOAT3 tdir((s1 * x2 - s2 * x1) * r, (s1 * y2 - s2 * y1) * r, (s1 * z2 - s2 * z1) * r);

        tan1[i0] = Add3(tan1[i0], sdir); tan1[i1] = Add3(tan1[i1], sdir); tan1[i2] = Add3(tan1[i2], sdir);
        tan2[i0] = Add3(tan2[i0], tdir); tan2[i1] = Add3(tan2[i1], tdir); tan2[i2] = Add3(tan2[i2], tdir);
    };

    if (!indices.empty()) {
        for (size_t i = 0; i + 2 < indices.size(); i += 3) accumulateTri(indices[i], indices[i + 1], indices[i + 2]);
    } else {
        for (unsigned int i = 0; i + 2 < positions.size(); i += 3) accumulateTri(i, i + 1, i + 2);
    }

    for (size_t i = 0; i < positions.size(); i++) {
        DirectX::XMFLOAT3 n = i < normals.size() ? Normalize3(normals[i]) : DirectX::XMFLOAT3(0, 1, 0);
        DirectX::XMFLOAT3 t = Sub3(tan1[i], Mul3(n, Dot3(n, tan1[i])));
        t = Normalize3(t);
        float sign = Dot3(Cross3(n, t), tan2[i]) < 0.0f ? -1.0f : 1.0f;
        tangents[i] = DirectX::XMFLOAT4(t.x, t.y, t.z, sign);
    }
}

// Helper to load texture. Uploads mip 0 only; the remaining mip levels of the
// resource are filled in by a GPU compute pass (see MipGenerator) since DX12
// has no built-in equivalent of DX11's GenerateMips.
ComPtr<ID3D12Resource> CreateTexture(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    const tinygltf::Image& image, std::vector<ComPtr<ID3D12Resource>>& uploadHeaps,
    bool generateMips = true, bool immediateMipUpload = false) {
    if (image.width == 0 || image.height == 0) return nullptr;

    UINT baseW = (UINT)image.width;
    UINT baseH = (UINT)image.height;
    UINT16 mipLevels = 1;
    if (generateMips)
        while ((baseW >> mipLevels) > 0 || (baseH >> mipLevels) > 0) mipLevels++;

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.MipLevels = mipLevels;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.Width = baseW;
    textureDesc.Height = baseH;
    textureDesc.Flags = generateMips && !immediateMipUpload
        ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
        : D3D12_RESOURCE_FLAG_NONE;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&texture)))) {
        return nullptr;
    }

    // Prepare base-level data (force 8-bit RGBA).
    //
    // Bit depth first: a 16-bit PNG decodes to two bytes per channel, and the
    // texture below is R8G8B8A8. Copying that through unconverted reads the low
    // half of one channel as the high half of the next, which scrambles the
    // colours -- a tan albedo comes out saturated blue. Take the high byte of
    // each sample, which is the 8-bit value.
    std::vector<unsigned char> rgba;
    const unsigned char* pSource = image.image.data();
    std::vector<unsigned char> narrowed;
    const int channels = image.component > 0 ? image.component : 4;
    if (image.bits == 16) {
        const size_t samples = (size_t)baseW * baseH * channels;
        narrowed.resize(samples);
        for (size_t i = 0; i < samples; ++i) {
            // Little-endian in tinygltf's decoded buffer: high byte is second.
            narrowed[i] = pSource[i * 2 + 1];
        }
        pSource = narrowed.data();
    }
    if (channels == 3) {
        rgba.resize((size_t)baseW * baseH * 4);
        for (size_t i = 0; i < (size_t)baseW * baseH; i++) {
            rgba[i * 4 + 0] = pSource[i * 3 + 0];
            rgba[i * 4 + 1] = pSource[i * 3 + 1];
            rgba[i * 4 + 2] = pSource[i * 3 + 2];
            rgba[i * 4 + 3] = 255;
        }
        pSource = rgba.data();
    }

    // Editor asset refreshes happen after the level-load GPU mip flush. Build
    // those rare late-load chains on the CPU and copy every level on the direct
    // list that already owns mip 0; otherwise distant sampling reaches queued,
    // uninitialized levels while close-up sampling still looks correct.
    if (generateMips && immediateMipUpload) {
        std::vector<std::vector<unsigned char>> mips(mipLevels);
        std::vector<UINT> mipWidths(mipLevels), mipHeights(mipLevels);
        mipWidths[0] = baseW;
        mipHeights[0] = baseH;
        mips[0].assign(pSource, pSource + (size_t)baseW * baseH * 4);
        for (UINT16 level = 1; level < mipLevels; ++level) {
            const UINT sourceWidth = mipWidths[level - 1];
            const UINT sourceHeight = mipHeights[level - 1];
            const UINT destinationWidth = (std::max)(1u, sourceWidth / 2);
            const UINT destinationHeight = (std::max)(1u, sourceHeight / 2);
            mipWidths[level] = destinationWidth;
            mipHeights[level] = destinationHeight;
            mips[level].resize((size_t)destinationWidth * destinationHeight * 4);
            for (UINT y = 0; y < destinationHeight; ++y) {
                const UINT y0 = (std::min)(y * 2, sourceHeight - 1);
                const UINT y1 = (std::min)(y * 2 + 1, sourceHeight - 1);
                for (UINT x = 0; x < destinationWidth; ++x) {
                    const UINT x0 = (std::min)(x * 2, sourceWidth - 1);
                    const UINT x1 = (std::min)(x * 2 + 1, sourceWidth - 1);
                    for (UINT channel = 0; channel < 4; ++channel) {
                        const auto sample = [&](UINT sx, UINT sy) {
                            return mips[level - 1][
                                ((size_t)sy * sourceWidth + sx) * 4 + channel];
                        };
                        const UINT sum = sample(x0, y0) + sample(x1, y0) +
                            sample(x0, y1) + sample(x1, y1);
                        mips[level][((size_t)y * destinationWidth + x) * 4 +
                            channel] = static_cast<unsigned char>((sum + 2u) / 4u);
                    }
                }
            }
        }

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipLevels);
        std::vector<UINT> rowCounts(mipLevels);
        UINT64 uploadBufferSize = 0;
        device->GetCopyableFootprints(&textureDesc, 0, mipLevels, 0,
            footprints.data(), rowCounts.data(), nullptr,
            &uploadBufferSize);

        ComPtr<ID3D12Resource> uploadHeap;
        ID3D12Resource* uploadResource = nullptr;
        BYTE* mapped = nullptr;
        uint64_t pooledOffset = 0;
        const bool pooled = IsTextureUploadArenaActiveDX12();
        if (pooled) {
            const TextureUploadAllocationDX12 allocation =
                AllocateTextureUploadDX12(device, uploadBufferSize);
            if (!allocation) return nullptr;
            uploadResource = allocation.resource;
            mapped = allocation.cpuAddress;
            pooledOffset = allocation.offset;
        } else {
            D3D12_HEAP_PROPERTIES uploadHeapProperties = {};
            uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC uploadDescription = {};
            uploadDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            uploadDescription.Width = uploadBufferSize;
            uploadDescription.Height = 1;
            uploadDescription.DepthOrArraySize = 1;
            uploadDescription.MipLevels = 1;
            uploadDescription.Format = DXGI_FORMAT_UNKNOWN;
            uploadDescription.SampleDesc.Count = 1;
            uploadDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device->CreateCommittedResource(
                    &uploadHeapProperties, D3D12_HEAP_FLAG_NONE,
                    &uploadDescription, D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr, IID_PPV_ARGS(&uploadHeap)))) return nullptr;
            if (FAILED(uploadHeap->Map(0, nullptr,
                    reinterpret_cast<void**>(&mapped)))) return nullptr;
            uploadResource = uploadHeap.Get();
        }

        for (UINT16 level = 0; level < mipLevels; ++level) {
            const size_t sourceRowBytes = (size_t)mipWidths[level] * 4;
            BYTE* destination = mapped + footprints[level].Offset;
            for (UINT row = 0; row < rowCounts[level]; ++row) {
                memcpy(destination + (size_t)row *
                        footprints[level].Footprint.RowPitch,
                    mips[level].data() + (size_t)row * sourceRowBytes,
                    sourceRowBytes);
            }
        }
        if (pooled) {
            for (auto& footprint : footprints) footprint.Offset += pooledOffset;
        } else {
            uploadHeap->Unmap(0, nullptr);
            uploadHeaps.push_back(uploadHeap);
        }

        for (UINT16 level = 0; level < mipLevels; ++level) {
            D3D12_TEXTURE_COPY_LOCATION destination = {};
            destination.pResource = texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = level;
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = uploadResource;
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[level];
            cmdList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }

        D3D12_RESOURCE_BARRIER ready = {};
        ready.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        ready.Transition.pResource = texture.Get();
        ready.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        ready.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ready.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &ready);
        return texture;
    }

    UINT64 uploadBufferSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadBufferSize);

    ComPtr<ID3D12Resource> uploadHeap;
    ID3D12Resource* uploadResource = nullptr;
    BYTE* pData = nullptr;
    const bool pooled = IsTextureUploadArenaActiveDX12();
    if (pooled) {
        const TextureUploadAllocationDX12 allocation =
            AllocateTextureUploadDX12(device, uploadBufferSize);
        if (!allocation) return nullptr;
        uploadResource = allocation.resource;
        pData = allocation.cpuAddress;
        footprint.Offset += allocation.offset;
    } else {
        D3D12_HEAP_PROPERTIES uploadHeapProps = {};
        uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadBufferSize;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        uploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(device->CreateCommittedResource(
                &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&uploadHeap)))) return nullptr;
        if (FAILED(uploadHeap->Map(0, nullptr,
                reinterpret_cast<void**>(&pData)))) return nullptr;
        uploadResource = uploadHeap.Get();
    }

    BYTE* pDest = pData;
    for (UINT h = 0; h < baseH; ++h) {
        memcpy(pDest + h * footprint.Footprint.RowPitch,
            pSource + (size_t)h * baseW * 4,
            (size_t)baseW * 4);
    }
    if (!pooled) {
        uploadHeap->Unmap(0, nullptr);
        uploadHeaps.push_back(uploadHeap);
    }

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = uploadResource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Dedicated compute lists cannot use PIXEL_SHADER_RESOURCE. Mipmapped
    // textures stay compute-readable until MipGenerator's direct-queue handoff.
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = mipLevels > 1
        ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
        : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    g_mipGen.GenerateMips(cmdList, texture.Get(), baseW, baseH, mipLevels);

    return texture;
}

ComPtr<ID3D12Resource> GLBImporter::LoadTextureFromFile(const std::string& filepath, ComPtr<ID3D12Device> device,
    ComPtr<ID3D12GraphicsCommandList> commandList, std::vector<ComPtr<ID3D12Resource>>& uploadHeaps) {
    int width = 0;
    int height = 0;
    int components = 0;
    unsigned char* pixels = stbi_load(filepath.c_str(), &width, &height, &components, 4);
    if (!pixels) {
        std::cerr << "Failed to load texture: " << filepath << std::endl;
        return nullptr;
    }

    tinygltf::Image image;
    image.width = width;
    image.height = height;
    image.component = 4;
    image.bits = 8;
    image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.image.assign(pixels, pixels + (size_t)width * (size_t)height * 4);
    stbi_image_free(pixels);

    return CreateTexture(device.Get(), commandList.Get(), image, uploadHeaps);
}

ComPtr<ID3D12Resource> GLBImporter::LoadTextureSingleMip(
    const std::string& filepath, ComPtr<ID3D12Device> device,
    ComPtr<ID3D12GraphicsCommandList> commandList,
    std::vector<ComPtr<ID3D12Resource>>& uploadHeaps) {
    int width = 0, height = 0, components = 0;
    unsigned char* pixels = stbi_load(filepath.c_str(), &width, &height, &components, 4);
    if (!pixels) return nullptr;
    tinygltf::Image image;
    image.width = width; image.height = height; image.component = 4;
    image.bits = 8; image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.image.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return CreateTexture(device.Get(), commandList.Get(), image, uploadHeaps, false);
}

ComPtr<ID3D12Resource> GLBImporter::LoadTextureFromMemory(
    const unsigned char* data, size_t size, ComPtr<ID3D12Device> device,
    ComPtr<ID3D12GraphicsCommandList> commandList,
    std::vector<ComPtr<ID3D12Resource>>& uploadHeaps) {
    if (!data || size == 0 || size > static_cast<size_t>(INT_MAX)) return nullptr;
    int width = 0, height = 0, components = 0;
    unsigned char* pixels = stbi_load_from_memory(data, static_cast<int>(size),
        &width, &height, &components, 4);
    if (!pixels) return nullptr;
    ComPtr<ID3D12Resource> texture = LoadEmbeddedTextureRGBA256(pixels, width,
        height, device, commandList, uploadHeaps);
    stbi_image_free(pixels);
    return texture;
}

ComPtr<ID3D12Resource> GLBImporter::LoadEmbeddedTextureRGBA256(
    const unsigned char* rgba, int width, int height, ComPtr<ID3D12Device> device,
    ComPtr<ID3D12GraphicsCommandList> commandList,
    std::vector<ComPtr<ID3D12Resource>>& uploadHeaps) {
    if (!rgba || width <= 0 || height <= 0) return nullptr;
    constexpr int target = 256;
    std::vector<unsigned char> resized(static_cast<size_t>(target) * target * 4);
    for (int y = 0; y < target; ++y) {
        const float sourceY = ((y + 0.5f) * height / target) - 0.5f;
        const int y0 = (std::max)(0, (std::min)(height - 1,
            static_cast<int>(std::floor(sourceY))));
        const int y1 = (std::min)(height - 1, y0 + 1);
        const float fy = (std::max)(0.0f, sourceY - std::floor(sourceY));
        for (int x = 0; x < target; ++x) {
            const float sourceX = ((x + 0.5f) * width / target) - 0.5f;
            const int x0 = (std::max)(0, (std::min)(width - 1,
                static_cast<int>(std::floor(sourceX))));
            const int x1 = (std::min)(width - 1, x0 + 1);
            const float fx = (std::max)(0.0f, sourceX - std::floor(sourceX));
            for (int channel = 0; channel < 4; ++channel) {
                const float a = rgba[(static_cast<size_t>(y0) * width + x0) * 4 + channel];
                const float b = rgba[(static_cast<size_t>(y0) * width + x1) * 4 + channel];
                const float c = rgba[(static_cast<size_t>(y1) * width + x0) * 4 + channel];
                const float d = rgba[(static_cast<size_t>(y1) * width + x1) * 4 + channel];
                resized[(static_cast<size_t>(y) * target + x) * 4 + channel] =
                    static_cast<unsigned char>((a + (b - a) * fx) * (1.0f - fy) +
                                               (c + (d - c) * fx) * fy + 0.5f);
            }
        }
    }
    tinygltf::Image image;
    image.width = target;
    image.height = target;
    image.component = 4;
    image.bits = 8;
    image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.image = std::move(resized);
    return CreateTexture(device.Get(), commandList.Get(), image, uploadHeaps);
}

bool GLBImporter::LoadPixelsRGBAFromMemory(const unsigned char* data, size_t size,
    std::vector<unsigned char>& outRGBA, int& outWidth, int& outHeight) {
    if (!data || size == 0 || size > static_cast<size_t>(INT_MAX)) return false;
    int comps = 0;
    unsigned char* pixels = stbi_load_from_memory(data, static_cast<int>(size),
        &outWidth, &outHeight, &comps, 4);
    if (!pixels) {
        std::cerr << "Failed to decode embedded image" << std::endl;
        return false;
    }
    outRGBA.assign(pixels, pixels + (size_t)outWidth * (size_t)outHeight * 4);
    stbi_image_free(pixels);
    return true;
}

bool GLBImporter::LoadPixelsRGBA(const std::string& filepath,
    std::vector<unsigned char>& outRGBA, int& outWidth, int& outHeight) {
    int comps = 0;
    unsigned char* pixels = stbi_load(filepath.c_str(), &outWidth, &outHeight, &comps, 4);
    if (!pixels) {
        std::cerr << "Failed to load image: " << filepath << std::endl;
        return false;
    }
    outRGBA.assign(pixels, pixels + (size_t)outWidth * (size_t)outHeight * 4);
    stbi_image_free(pixels);
    return true;
}

bool GLBImporter::LoadPixelsGray16(const std::string& filepath,
    std::vector<uint16_t>& outGray, int& outWidth, int& outHeight) {
    if (!stbi_is_16_bit(filepath.c_str())) {
        std::cerr << "Heightmap is not 16-bit: " << filepath << std::endl;
        return false;
    }
    int components = 0;
    stbi_us* pixels = stbi_load_16(filepath.c_str(), &outWidth, &outHeight,
                                   &components, 1);
    if (!pixels) {
        std::cerr << "Failed to load 16-bit heightmap: " << filepath << std::endl;
        return false;
    }
    outGray.assign(pixels,
        pixels + static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight));
    stbi_image_free(pixels);
    return true;
}

ComPtr<ID3D12Resource> GLBImporter::CreateTextureFromRGBA(ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList, const std::vector<unsigned char>& rgba,
    int width, int height, std::vector<ComPtr<ID3D12Resource>>& uploadHeaps) {
    if (width <= 0 || height <= 0 || rgba.size() < (size_t)width * height * 4) return nullptr;
    tinygltf::Image image;
    image.width = width;
    image.height = height;
    image.component = 4;
    image.bits = 8;
    image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.image = rgba;
    return CreateTexture(device, commandList, image, uploadHeaps);
}

namespace {
// Decodes an HDR environment map to RGBA32F. Two container formats are in use:
// OpenEXR (.exr) via tinyexr and Radiance (.hdr) via stb_image. Both produce the
// same 4-float-per-pixel layout, so every consumer below shares this and only
// the decode differs.
//
// Selected by extension because that is what the asset actually is -- feeding a
// Radiance file to LoadEXR fails with a confusing "read version info" error,
// which is exactly how a night sky silently lost its irradiance.
//
// `outIsStbAllocation` tells the caller which free to use: stb_image has its own
// allocator, tinyexr hands back a malloc'd block.
float* LoadHDRPixels(const std::string& filepath, const char* usage,
                     int& width, int& height, bool& outIsStbAllocation) {
    outIsStbAllocation = filepath.size() >= 4 &&
        _stricmp(filepath.c_str() + filepath.size() - 4, ".hdr") == 0;

    if (outIsStbAllocation) {
        int channels = 0;
        // Force 4 channels: every consumer assumes a 4-float stride, and
        // Radiance maps are usually 3-channel RGBE.
        float* pixels =
            stbi_loadf(filepath.c_str(), &width, &height, &channels, 4);
        if (!pixels) {
            std::cerr << "Failed to load HDR for " << usage << ": " << filepath
                      << " (" << (stbi_failure_reason()
                                      ? stbi_failure_reason() : "unknown")
                      << ")" << std::endl;
        }
        return pixels;
    }

    float* pixels = nullptr;
    const char* err = nullptr;
    if (LoadEXR(&pixels, &width, &height, filepath.c_str(), &err) !=
            TINYEXR_SUCCESS) {
        std::cerr << "Failed to load EXR for " << usage << ": " << filepath
                  << (err ? (std::string(" (") + err + ")") : "") << std::endl;
        if (err) FreeEXRErrorMessage(err);
        return nullptr;
    }
    return pixels;
}

void FreeHDRPixels(float* pixels, bool isStbAllocation) {
    if (!pixels) return;
    if (isStbAllocation) stbi_image_free(pixels);
    else free(pixels);
}
}  // namespace

ComPtr<ID3D12Resource> GLBImporter::LoadEXRTextureFromFile(const std::string& filepath, ComPtr<ID3D12Device> device,
    ComPtr<ID3D12GraphicsCommandList> commandList, std::vector<ComPtr<ID3D12Resource>>& uploadHeaps) {
    int width = 0, height = 0;
    bool isRadianceHDR = false;
    float* pixels =
        LoadHDRPixels(filepath, "texture", width, height, isRadianceHDR);
    if (!pixels) return nullptr;
    // Build a full CPU mip chain (RGBA float). Box filter: wrap in U (longitude)
    // and clamp in V (latitude), matching how an equirect map tiles. Doing this on
    // the CPU (one-time load cost) avoids needing typed-UAV float support the GPU
    // MipGenerator assumes for R8G8B8A8. Trilinear sampling of these mips is what
    // tames the pole singularity's radial smearing.
    const UINT baseW = (UINT)width, baseH = (UINT)height;
    UINT16 mipLevels = 1;
    while ((baseW >> mipLevels) > 0 || (baseH >> mipLevels) > 0) mipLevels++;

    std::vector<std::vector<float>> mips(mipLevels);
    std::vector<UINT> mipW(mipLevels), mipH(mipLevels);
    mipW[0] = baseW; mipH[0] = baseH;
    mips[0].assign(pixels, pixels + (size_t)baseW * baseH * 4);
    FreeHDRPixels(pixels, isRadianceHDR);

    for (UINT16 level = 1; level < mipLevels; ++level) {
        UINT sw = mipW[level - 1], sh = mipH[level - 1];
        UINT dw = std::max(1u, sw / 2), dh = std::max(1u, sh / 2);
        mipW[level] = dw; mipH[level] = dh;
        mips[level].resize((size_t)dw * dh * 4);
        const std::vector<float>& s = mips[level - 1];
        std::vector<float>& d = mips[level];
        for (UINT y = 0; y < dh; ++y) {
            UINT y0 = std::min(y * 2, sh - 1);
            UINT y1 = std::min(y * 2 + 1, sh - 1);
            for (UINT x = 0; x < dw; ++x) {
                UINT x0 = (x * 2) % sw;
                UINT x1 = (x * 2 + 1) % sw;
                for (int c = 0; c < 4; ++c) {
                    d[((size_t)y * dw + x) * 4 + c] = 0.25f * (
                        s[((size_t)y0 * sw + x0) * 4 + c] +
                        s[((size_t)y0 * sw + x1) * 4 + c] +
                        s[((size_t)y1 * sw + x0) * 4 + c] +
                        s[((size_t)y1 * sw + x1) * 4 + c]);
                }
            }
        }
    }

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.MipLevels = mipLevels;
    textureDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    textureDesc.Width = baseW;
    textureDesc.Height = baseH;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&texture)))) {
        return nullptr;
    }

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipLevels);
    std::vector<UINT> numRows(mipLevels);
    std::vector<UINT64> rowSizes(mipLevels);
    UINT64 uploadBufferSize = 0;
    device->GetCopyableFootprints(&textureDesc, 0, mipLevels, 0,
        footprints.data(), numRows.data(), rowSizes.data(), &uploadBufferSize);

    ComPtr<ID3D12Resource> uploadHeap;
    ID3D12Resource* uploadResource = nullptr;
    BYTE* pData = nullptr;
    uint64_t pooledOffset = 0;
    const bool pooled = IsTextureUploadArenaActiveDX12();
    if (pooled) {
        const TextureUploadAllocationDX12 allocation =
            AllocateTextureUploadDX12(device.Get(), uploadBufferSize);
        if (!allocation) return nullptr;
        uploadResource = allocation.resource;
        pData = allocation.cpuAddress;
        pooledOffset = allocation.offset;
    } else {
        D3D12_HEAP_PROPERTIES uploadHeapProps = {};
        uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadBufferSize;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&uploadHeap)))) return nullptr;
        if (FAILED(uploadHeap->Map(0, nullptr,
                reinterpret_cast<void**>(&pData)))) return nullptr;
        uploadResource = uploadHeap.Get();
    }
    for (UINT16 level = 0; level < mipLevels; ++level) {
        const size_t srcRowBytes = (size_t)mipW[level] * 4 * sizeof(float);
        BYTE* pDest = pData + footprints[level].Offset;
        for (UINT row = 0; row < numRows[level]; ++row) {
            memcpy(pDest + (size_t)row * footprints[level].Footprint.RowPitch,
                (const BYTE*)mips[level].data() + (size_t)row * srcRowBytes,
                srcRowBytes);
        }
    }
    if (pooled) {
        for (auto& footprint : footprints) footprint.Offset += pooledOffset;
    } else {
        uploadHeap->Unmap(0, nullptr);
        uploadHeaps.push_back(uploadHeap);
    }

    for (UINT16 level = 0; level < mipLevels; ++level) {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = level;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = uploadResource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[level];

        commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);

    return texture;
}

std::array<XMFLOAT3, 9> GLBImporter::ComputeSkyIrradianceSH(
    const std::string& filepath, float environmentRotationRadians) {
    std::array<XMFLOAT3, 9> coeffs{};
    for (auto& c : coeffs) c = XMFLOAT3(0, 0, 0);

    int width = 0, height = 0;
    bool isStbAllocation = false;
    float* pixels =
        LoadHDRPixels(filepath, "sky SH", width, height, isStbAllocation);
    if (!pixels) return coeffs;

    // Real SH basis function values (unnormalized-direction form), L0-L2.
    double sh[9];
    double weightSum = 0.0;
    double raw[9][3] = {};
    const double rotationCos = cos(environmentRotationRadians);
    const double rotationSin = sin(environmentRotationRadians);

    for (int y = 0; y < height; ++y) {
        // Equirectangular row -> polar angle theta in [0, pi]; sin(theta) is
        // both the Jacobian for solid angle and the per-row sample weight.
        double v = (y + 0.5) / height;
        double theta = v * XM_PI;
        double sinTheta = sin(theta);
        double cosTheta = cos(theta);
        if (sinTheta <= 0.0) continue;

        for (int x = 0; x < width; ++x) {
            double u = (x + 0.5) / width;
            double phi = (u - 0.5) * 2.0 * XM_PI;

            const double sourceX = sinTheta * cos(phi);
            const double sourceZ = sinTheta * sin(phi);
            // Matches XMMatrixRotationY under the engine's row-vector
            // convention: world = source * rotationY.
            double dx = sourceX * rotationCos + sourceZ * rotationSin;
            double dz = -sourceX * rotationSin + sourceZ * rotationCos;
            double dy = cosTheta;

            const float* p = &pixels[((size_t)y * width + x) * 4];
            double r = p[0], g = p[1], b = p[2];
            if (!std::isfinite(r)) r = 0.0;
            if (!std::isfinite(g)) g = 0.0;
            if (!std::isfinite(b)) b = 0.0;

            // L0
            sh[0] = 0.282095;
            // L1
            sh[1] = 0.488603 * dy;
            sh[2] = 0.488603 * dz;
            sh[3] = 0.488603 * dx;
            // L2
            sh[4] = 1.092548 * dx * dy;
            sh[5] = 1.092548 * dy * dz;
            sh[6] = 0.315392 * (3.0 * dz * dz - 1.0);
            sh[7] = 1.092548 * dx * dz;
            sh[8] = 0.546274 * (dx * dx - dy * dy);

            double weight = sinTheta;
            for (int i = 0; i < 9; ++i) {
                raw[i][0] += r * sh[i] * weight;
                raw[i][1] += g * sh[i] * weight;
                raw[i][2] += b * sh[i] * weight;
            }
            weightSum += weight;
        }
    }
    FreeHDRPixels(pixels, isStbAllocation);

    if (weightSum <= 0.0) return coeffs;

    // Normalize so integrating sh[0] over the sphere reproduces the mean
    // radiance, then fold in the cosine-lobe (Lambertian) convolution
    // factors (A_l) so the shader only needs a flat dot product per band.
    double normalization = (4.0 * XM_PI) / weightSum;
    const double A[3] = { 1.0, 2.0 / 3.0, 1.0 / 4.0 };
    const int bandOfCoeff[9] = { 0, 1, 1, 1, 2, 2, 2, 2, 2 };

    for (int i = 0; i < 9; ++i) {
        double scale = normalization * A[bandOfCoeff[i]];
        coeffs[i] = XMFLOAT3(
            (float)(raw[i][0] * scale),
            (float)(raw[i][1] * scale),
            (float)(raw[i][2] * scale));
    }
    return coeffs;
}

HDRISunLight GLBImporter::ExtractHDRISunLight(const std::string& filepath,
                                              float targetLuminance,
                                              float environmentRotationRadians) {
    HDRISunLight result;
    int width = 0, height = 0;
    bool isStbAllocation = false;
    float* pixels =
        LoadHDRPixels(filepath, "HDRI sun analysis", width, height,
                      isStbAllocation);
    if (!pixels) return result;

    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<float> luminance(pixelCount, 0.0f);
    double luminanceSum = 0.0;
    for (size_t i = 0; i < pixelCount; ++i) {
        const float* p = pixels + i * 4;
        const float r = std::isfinite(p[0]) ? (std::max)(p[0], 0.0f) : 0.0f;
        const float g = std::isfinite(p[1]) ? (std::max)(p[1], 0.0f) : 0.0f;
        const float b = std::isfinite(p[2]) ? (std::max)(p[2], 0.0f) : 0.0f;
        luminance[i] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        luminanceSum += luminance[i];
    }

    // Top 0.02% captures sun disk plus immediate glow while rejecting bright
    // cloud banks. Slightly lower threshold makes centroid stable across EXRs.
    std::vector<float> ranked = luminance;
    const size_t percentileIndex = static_cast<size_t>(pixelCount * 0.9998);
    std::nth_element(ranked.begin(), ranked.begin() + percentileIndex,
                     ranked.end());
    const float meanLuminance = static_cast<float>(
        luminanceSum / (std::max)(pixelCount, size_t{1}));
    const float threshold = (std::max)(ranked[percentileIndex] * 0.85f,
                                       meanLuminance * 4.0f);

    XMVECTOR directionSum = XMVectorZero();
    double colorR = 0.0, colorG = 0.0, colorB = 0.0, weightSum = 0.0;
    for (int y = 0; y < height; ++y) {
        const double theta = (static_cast<double>(y) + 0.5) / height * XM_PI;
        const double sinTheta = sin(theta);
        const double cosTheta = cos(theta);
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const float value = luminance[index];
            if (value < threshold) continue;
            const double weight = sqrt((std::max)(
                static_cast<double>(value - threshold),
                static_cast<double>(threshold) * 0.002));
            const double phi = ((static_cast<double>(x) + 0.5) / width - 0.5) *
                               2.0 * XM_PI;
            const XMVECTOR direction = XMVectorSet(
                static_cast<float>(sinTheta * cos(phi)),
                static_cast<float>(cosTheta),
                static_cast<float>(sinTheta * sin(phi)), 0.0f);
            directionSum = XMVectorMultiplyAdd(
                direction, XMVectorReplicate(static_cast<float>(weight)),
                directionSum);
            const float* p = pixels + index * 4;
            colorR += (std::max)(p[0], 0.0f) * weight;
            colorG += (std::max)(p[1], 0.0f) * weight;
            colorB += (std::max)(p[2], 0.0f) * weight;
            weightSum += weight;
        }
    }
    FreeHDRPixels(pixels, isStbAllocation);

    if (weightSum <= 0.0 || XMVectorGetX(XMVector3LengthSq(directionSum)) < 1e-8f)
        return result;
    const XMVECTOR rotatedDirection = XMVector3TransformNormal(
        XMVector3Normalize(directionSum),
        XMMatrixRotationY(environmentRotationRadians));
    XMStoreFloat3(&result.direction, rotatedDirection);
    const float averageR = static_cast<float>(colorR / weightSum);
    const float averageG = static_cast<float>(colorG / weightSum);
    const float averageB = static_cast<float>(colorB / weightSum);
    result.sourceLuminance = 0.2126f * averageR + 0.7152f * averageG +
                             0.0722f * averageB;
    if (result.sourceLuminance <= 1e-5f) return result;

    const float scale = targetLuminance / result.sourceLuminance;
    result.color = {
        (std::max)(0.45f, (std::min)(1.55f, averageR * scale)),
        (std::max)(0.45f, (std::min)(1.55f, averageG * scale)),
        (std::max)(0.45f, (std::min)(1.55f, averageB * scale))
    };
    // Preserve target luminance after chromaticity safety clamps.
    const float clampedLuminance = 0.2126f * result.color.x +
        0.7152f * result.color.y + 0.0722f * result.color.z;
    if (clampedLuminance > 1e-5f) {
        const float renormalize = targetLuminance / clampedLuminance;
        result.color.x *= renormalize;
        result.color.y *= renormalize;
        result.color.z *= renormalize;
    }
    result.valid = true;
    return result;
}

// Convert GLTF mesh to SceneMesh
std::shared_ptr<SceneMesh> ProcessMesh(const tinygltf::Model& model, const tinygltf::Mesh& gltfMesh, ID3D12Device* device, const std::vector<std::shared_ptr<SceneMaterial>>& materials) {
    auto sceneMesh = std::make_shared<SceneMesh>();
    sceneMesh->name = gltfMesh.name;
    
    for (const auto& primitive : gltfMesh.primitives) {
        MeshPrimitive meshPrim;
        meshPrim.materialIndex = primitive.material;
        if (meshPrim.materialIndex >= 0 && meshPrim.materialIndex < materials.size()) {
            meshPrim.material = materials[meshPrim.materialIndex];
        }

        // Get accessors
        int posIdx = -1;
        int normIdx = -1;
        int texIdx = -1;
        int tangentIdx = -1;
        
        if (primitive.attributes.find("POSITION") != primitive.attributes.end())
            posIdx = primitive.attributes.at("POSITION");
        if (primitive.attributes.find("NORMAL") != primitive.attributes.end())
            normIdx = primitive.attributes.at("NORMAL");
        if (primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end())
            texIdx = primitive.attributes.at("TEXCOORD_0");
        if (primitive.attributes.find("TANGENT") != primitive.attributes.end())
            tangentIdx = primitive.attributes.at("TANGENT");
        int jointsIdx = -1;
        int weightsIdx = -1;
        if (primitive.attributes.find("JOINTS_0") != primitive.attributes.end())
            jointsIdx = primitive.attributes.at("JOINTS_0");
        if (primitive.attributes.find("WEIGHTS_0") != primitive.attributes.end())
            weightsIdx = primitive.attributes.at("WEIGHTS_0");

        // Extract data
        std::vector<XMFLOAT3> positions;
        std::vector<XMFLOAT3> normals;
        std::vector<XMFLOAT2> texCoords;
        std::vector<XMFLOAT4> tangents;
        std::vector<std::array<uint32_t, 4>> jointIndices;
        std::vector<XMFLOAT4> jointWeights;

        if (posIdx >= 0) CopyBufferData(model, posIdx, positions);
        if (normIdx >= 0) CopyBufferData(model, normIdx, normals);
        if (texIdx >= 0) CopyBufferData(model, texIdx, texCoords);
        if (tangentIdx >= 0) CopyBufferData(model, tangentIdx, tangents);
        CopyJointIndices(model, jointsIdx, jointIndices);
        CopyJointWeights(model, weightsIdx, jointWeights);
        
        // Resize to match positions
        if (normals.empty() && !positions.empty()) normals.resize(positions.size(), XMFLOAT3(0, 1, 0));
        if (texCoords.empty() && !positions.empty()) texCoords.resize(positions.size(), XMFLOAT2(0, 0));
        
        // Indices
        if (primitive.indices >= 0) {
            const tinygltf::Accessor& accessor = model.accessors[primitive.indices];
            const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
            const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
            
            const unsigned char* dataStart = &buffer.data[bufferView.byteOffset + accessor.byteOffset];
            size_t dataStep = accessor.ByteStride(bufferView);
            
            for (size_t i = 0; i < accessor.count; i++) {
                if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                    const unsigned short* val = reinterpret_cast<const unsigned short*>(dataStart + i * dataStep);
                    meshPrim.indices.push_back(*val);
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                    const unsigned int* val = reinterpret_cast<const unsigned int*>(dataStart + i * dataStep);
                    meshPrim.indices.push_back(*val);
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                    const unsigned char* val = reinterpret_cast<const unsigned char*>(dataStart + i * dataStep);
                    meshPrim.indices.push_back(*val);
                }
            }
        }

        if (tangents.size() != positions.size()) {
            GenerateTangents(positions, normals, texCoords, meshPrim.indices, tangents);
        }
        
        // Interleave data
        for (size_t i = 0; i < positions.size(); i++) {
            meshPrim.vertices.push_back(positions[i].x);
            meshPrim.vertices.push_back(positions[i].y);
            meshPrim.vertices.push_back(positions[i].z);
            
            meshPrim.vertices.push_back(normals[i].x);
            meshPrim.vertices.push_back(normals[i].y);
            meshPrim.vertices.push_back(normals[i].z);
            
            meshPrim.vertices.push_back(texCoords[i].x);
            meshPrim.vertices.push_back(texCoords[i].y);

            meshPrim.vertices.push_back(tangents[i].x);
            meshPrim.vertices.push_back(tangents[i].y);
            meshPrim.vertices.push_back(tangents[i].z);
            meshPrim.vertices.push_back(tangents[i].w);
        }

        // Per-vertex skin attributes, parallel to the interleaved stream above.
        // Weights are renormalized: exporters often leave them summing slightly
        // off 1, which would shrink or inflate the skinned mesh.
        if (!jointIndices.empty() && jointIndices.size() == positions.size() &&
            jointWeights.size() == positions.size()) {
            meshPrim.skin.resize(positions.size());
            for (size_t i = 0; i < positions.size(); i++) {
                SkinVertex& vertex = meshPrim.skin[i];
                const float weights[4] = {
                    jointWeights[i].x, jointWeights[i].y,
                    jointWeights[i].z, jointWeights[i].w };
                float total = 0.0f;
                for (int c = 0; c < 4; c++) total += weights[c];
                for (int c = 0; c < 4; c++) {
                    vertex.boneIndex[c] = jointIndices[i][c];
                    vertex.boneWeight[c] =
                        total > 1e-6f ? weights[c] / total : 0.0f;
                }
                // A vertex with no influences would collapse to the origin, so
                // pin it to its first joint at full weight instead.
                if (total <= 1e-6f) vertex.boneWeight[0] = 1.0f;
            }
        }

        meshPrim.indexCount = (UINT)meshPrim.indices.size();
        
        // Create Buffers (Upload Heap for simplicity)
        if (device) {
            // Vertex Buffer
            UINT vbSize = (UINT)(meshPrim.vertices.size() * sizeof(float));
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            
            D3D12_RESOURCE_DESC bufferDesc = {};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = vbSize;
            bufferDesc.Height = 1;
            bufferDesc.DepthOrArraySize = 1;
            bufferDesc.MipLevels = 1;
            bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufferDesc.SampleDesc.Count = 1;
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

            HRESULT hr = device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&meshPrim.vertexBuffer));
                
            if (SUCCEEDED(hr)) {
                void* mappedData;
                meshPrim.vertexBuffer->Map(0, nullptr, &mappedData);
                memcpy(mappedData, meshPrim.vertices.data(), vbSize);
                meshPrim.vertexBuffer->Unmap(0, nullptr);
                
                meshPrim.vbv.BufferLocation = meshPrim.vertexBuffer->GetGPUVirtualAddress();
                meshPrim.vbv.SizeInBytes = vbSize;
                meshPrim.vbv.StrideInBytes = 12 * sizeof(float); // 3 pos + 3 norm + 2 uv + 4 tangent
            }
            
            // Index Buffer
            if (!meshPrim.indices.empty()) {
                UINT ibSize = (UINT)(meshPrim.indices.size() * sizeof(unsigned int));
                bufferDesc.Width = ibSize;
                
                hr = device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&meshPrim.indexBuffer));
                    
                if (SUCCEEDED(hr)) {
                    void* mappedData;
                    meshPrim.indexBuffer->Map(0, nullptr, &mappedData);
                    memcpy(mappedData, meshPrim.indices.data(), ibSize);
                    meshPrim.indexBuffer->Unmap(0, nullptr);
                    
                    meshPrim.ibv.BufferLocation = meshPrim.indexBuffer->GetGPUVirtualAddress();
                    meshPrim.ibv.SizeInBytes = ibSize;
                    meshPrim.ibv.Format = DXGI_FORMAT_R32_UINT;
                }
            }

            // Skin Buffer: the StructuredBuffer<SkinVertex> the vertex and mesh
            // shaders read at t13. skinVertexCount is what marks the primitive
            // as skinned, so it is only set once the upload succeeds.
            if (!meshPrim.skin.empty()) {
                UINT skinSize = (UINT)(meshPrim.skin.size() * sizeof(SkinVertex));
                bufferDesc.Width = skinSize;

                hr = device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&meshPrim.skinBuffer));

                if (SUCCEEDED(hr)) {
                    void* mappedData;
                    meshPrim.skinBuffer->Map(0, nullptr, &mappedData);
                    memcpy(mappedData, meshPrim.skin.data(), skinSize);
                    meshPrim.skinBuffer->Unmap(0, nullptr);
                    meshPrim.skinVertexCount = (UINT)meshPrim.skin.size();
                } else {
                    meshPrim.skinBuffer.Reset();
                }
            }
        }
        
        sceneMesh->primitives.push_back(meshPrim);
    }
    
    return sceneMesh;
}

void ProcessNode(const tinygltf::Model& model, int nodeIndex, SceneNode* parentNode, std::vector<std::shared_ptr<SceneMesh>>& meshes) {
    const tinygltf::Node& gltfNode = model.nodes[nodeIndex];
    auto newNode = std::make_shared<SceneNode>(gltfNode.name);
    
    // Transform
    if (!gltfNode.matrix.empty()) {
        // Decompose or use as is. For simplicity, we assume translation/rotation/scale or matrix.
        // If matrix is provided, we might need to decompose if we want TRS.
        // For this simple implementation, if matrix exists, we just set the local transform directly.
        // But SceneNode splits TRS. 
        // Let's check for T, R, S.
    }
    
    if (!gltfNode.translation.empty()) {
        newNode->translation = XMFLOAT3((float)gltfNode.translation[0], (float)gltfNode.translation[1], (float)gltfNode.translation[2]);
    }
    if (!gltfNode.rotation.empty()) {
        newNode->rotation = XMFLOAT4((float)gltfNode.rotation[0], (float)gltfNode.rotation[1], (float)gltfNode.rotation[2], (float)gltfNode.rotation[3]);
    }
    if (!gltfNode.scale.empty()) {
        newNode->scale = XMFLOAT3((float)gltfNode.scale[0], (float)gltfNode.scale[1], (float)gltfNode.scale[2]);
    }
    
    // Mesh
    if (gltfNode.mesh >= 0) {
        newNode->mesh = meshes[gltfNode.mesh];
    }
    
    parentNode->AddChild(newNode);
    
    for (int childIndex : gltfNode.children) {
        ProcessNode(model, childIndex, newNode.get(), meshes);
    }
}

// Builds a Skeleton from a glTF skin. Bone ids follow the skin's joint order,
// which is also the order JOINTS_0 indexes into, so the palette the shader
// reads lines up with the per-vertex indices without any remapping.
static void BuildSkeletonFromSkin(const tinygltf::Model& model,
                                  const tinygltf::Skin& skin,
                                  Skeleton& outSkeleton) {
    const size_t boneCount = skin.joints.size();
    if (boneCount == 0) return;

    outSkeleton.names.resize(boneCount);
    outSkeleton.parent.assign(boneCount, -1);
    outSkeleton.offset.resize(boneCount);
    outSkeleton.localBind.resize(boneCount);
    XMStoreFloat4x4(&outSkeleton.globalInverse, XMMatrixIdentity());

    // glTF node index -> bone id, so parent links can be resolved below.
    std::unordered_map<int, int> nodeToBone;
    for (size_t bone = 0; bone < boneCount; bone++)
        nodeToBone[skin.joints[bone]] = (int)bone;

    // Inverse-bind matrices are optional; identity is the spec's default.
    std::vector<XMFLOAT4X4> inverseBind(boneCount);
    for (auto& matrix : inverseBind) XMStoreFloat4x4(&matrix, XMMatrixIdentity());
    if (skin.inverseBindMatrices >= 0) {
        const tinygltf::Accessor& accessor =
            model.accessors[skin.inverseBindMatrices];
        if (accessor.bufferView >= 0) {
            const tinygltf::BufferView& bufferView =
                model.bufferViews[accessor.bufferView];
            const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
            const unsigned char* dataStart =
                &buffer.data[bufferView.byteOffset + accessor.byteOffset];
            const size_t dataStep = accessor.ByteStride(bufferView);
            const size_t available = (std::min)(boneCount, accessor.count);
            for (size_t bone = 0; bone < available; bone++)
                memcpy(&inverseBind[bone], dataStart + bone * dataStep,
                       sizeof(XMFLOAT4X4));
        }
    }

    for (size_t bone = 0; bone < boneCount; bone++) {
        const int nodeIndex = skin.joints[bone];
        const tinygltf::Node& node = model.nodes[nodeIndex];
        outSkeleton.names[bone] = node.name;
        outSkeleton.index[node.name] = (int)bone;
        outSkeleton.offset[bone] = inverseBind[bone];

        // Bind-pose local transform, from TRS or an explicit matrix.
        XMMATRIX local = XMMatrixIdentity();
        if (node.matrix.size() == 16) {
            XMFLOAT4X4 matrix{};
            for (int e = 0; e < 16; e++)
                (&matrix._11)[e] = (float)node.matrix[e];
            local = XMLoadFloat4x4(&matrix);
        } else {
            const XMVECTOR translation = node.translation.size() == 3
                ? XMVectorSet((float)node.translation[0],
                              (float)node.translation[1],
                              (float)node.translation[2], 0.0f)
                : XMVectorZero();
            const XMVECTOR rotation = node.rotation.size() == 4
                ? XMVectorSet((float)node.rotation[0], (float)node.rotation[1],
                              (float)node.rotation[2], (float)node.rotation[3])
                : XMQuaternionIdentity();
            const XMVECTOR scale = node.scale.size() == 3
                ? XMVectorSet((float)node.scale[0], (float)node.scale[1],
                              (float)node.scale[2], 0.0f)
                : XMVectorSplatOne();
            local = XMMatrixAffineTransformation(scale, XMVectorZero(), rotation,
                                                 translation);
        }
        XMStoreFloat4x4(&outSkeleton.localBind[bone], local);
    }

    // Parent links: a joint's parent is the nearest ancestor that is also a
    // joint. Joints whose parent sits outside the skin stay roots (-1).
    for (size_t node = 0; node < model.nodes.size(); node++) {
        auto parentBone = nodeToBone.find((int)node);
        for (int childNode : model.nodes[node].children) {
            auto childBone = nodeToBone.find(childNode);
            if (childBone == nodeToBone.end()) continue;
            if (parentBone != nodeToBone.end())
                outSkeleton.parent[childBone->second] = parentBone->second;
        }
    }
}

std::shared_ptr<SceneNode> GLBImporter::LoadGLBSkinned(
    const std::string& filepath, ComPtr<ID3D12Device> device,
    ComPtr<ID3D12GraphicsCommandList> commandList, Skeleton& outSkeleton) {
    outSkeleton = Skeleton{};
    return LoadGLBInternal(filepath, device, commandList, &outSkeleton, false);
}

std::shared_ptr<SceneNode> GLBImporter::LoadGLB(
    const std::string& filepath, ComPtr<ID3D12Device> device,
    ComPtr<ID3D12GraphicsCommandList> commandList, bool immediateMipUpload) {
    return LoadGLBInternal(
        filepath, device, commandList, nullptr, immediateMipUpload);
}

std::shared_ptr<SceneNode> GLBImporter::LoadGLBInternal(
    const std::string& filepath, ComPtr<ID3D12Device> device,
    ComPtr<ID3D12GraphicsCommandList> commandList, Skeleton* outSkeleton,
    bool immediateMipUpload) {
    // The cooked cache stores baked static geometry only, so a caller asking
    // for a skeleton has to go through the source glTF.
    if (device && !outSkeleton) {
        if (auto cooked = CookedAssetLoader::LoadForSource(
                filepath, device, commandList)) {
            std::cout << "Loaded cooked model: "
                      << CookedAssetLoader::FindForSource(filepath).string()
                      << "\n";
            return cooked;
        }
    }
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;

    bool ret = loader.LoadBinaryFromFile(&model, &err, &warn, filepath);
    if (!warn.empty()) std::cout << "GLB Warning: " << warn << std::endl;
    if (!err.empty()) std::cerr << "GLB Error: " << err << std::endl;
    if (!ret) return nullptr;

    // Load Materials
    std::vector<std::shared_ptr<SceneMaterial>> materials;
    for (const auto& mat : model.materials) {
        auto sceneMat = std::make_shared<SceneMaterial>();
        sceneMat->name = mat.name;

        // Factors
        if (mat.pbrMetallicRoughness.baseColorFactor.size() == 4) {
            sceneMat->baseColorFactor = XMFLOAT4(
                (float)mat.pbrMetallicRoughness.baseColorFactor[0],
                (float)mat.pbrMetallicRoughness.baseColorFactor[1],
                (float)mat.pbrMetallicRoughness.baseColorFactor[2],
                (float)mat.pbrMetallicRoughness.baseColorFactor[3]);
        }
        sceneMat->roughnessFactor = (float)mat.pbrMetallicRoughness.roughnessFactor;
        sceneMat->metallicFactor = (float)mat.pbrMetallicRoughness.metallicFactor;
        sceneMat->doubleSided = mat.doubleSided;

        // Emissive, premultiplied by KHR_materials_emissive_strength. Authoring
        // tools export beacons and panel lights as a low emissiveFactor with a
        // large strength multiplier, so dropping the extension renders them
        // nearly black.
        if (mat.emissiveFactor.size() == 3) {
            float strength = 1.0f;
            const auto emissiveExt =
                mat.extensions.find("KHR_materials_emissive_strength");
            if (emissiveExt != mat.extensions.end() &&
                emissiveExt->second.Has("emissiveStrength")) {
                const auto& value = emissiveExt->second.Get("emissiveStrength");
                if (value.IsNumber())
                    strength = (float)value.GetNumberAsDouble();
            }
            sceneMat->emissiveFactor = XMFLOAT3(
                (float)mat.emissiveFactor[0] * strength,
                (float)mat.emissiveFactor[1] * strength,
                (float)mat.emissiveFactor[2] * strength);
        }

        // Textures
        // Base Color
        int baseColorIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
        if (baseColorIdx >= 0) {
            int imgIdx = model.textures[baseColorIdx].source;
            if (imgIdx >= 0 && imgIdx < model.images.size())
                sceneMat->baseColorTexture = CreateTexture(device.Get(),
                    commandList.Get(), model.images[imgIdx],
                    sceneMat->uploadHeaps, true, immediateMipUpload);
        }

        // Metallic Roughness
        int mrIdx = mat.pbrMetallicRoughness.metallicRoughnessTexture.index;
        if (mrIdx >= 0) {
            int imgIdx = model.textures[mrIdx].source;
            if (imgIdx >= 0 && imgIdx < model.images.size())
                sceneMat->metallicRoughnessTexture = CreateTexture(device.Get(),
                    commandList.Get(), model.images[imgIdx],
                    sceneMat->uploadHeaps, true, immediateMipUpload);
        }

        // Normal
        int nIdx = mat.normalTexture.index;
        if (nIdx >= 0) {
            int imgIdx = model.textures[nIdx].source;
            if (imgIdx >= 0 && imgIdx < model.images.size())
                sceneMat->normalTexture = CreateTexture(device.Get(),
                    commandList.Get(), model.images[imgIdx],
                    sceneMat->uploadHeaps, true, immediateMipUpload);
        }

        materials.push_back(sceneMat);
    }

    // Convert Meshes
    std::vector<std::shared_ptr<SceneMesh>> convertedMeshes;
    for (const auto& mesh : model.meshes) {
        auto converted = ProcessMesh(model, mesh, device.Get(), materials);
        if (converted) {
            for (MeshPrimitive& primitive : converted->primitives)
                BuildMeshletData(primitive, device.Get());
        }
        convertedMeshes.push_back(std::move(converted));
    }

    // Process Scene
    auto rootNode = std::make_shared<SceneNode>("ModelRoot");
    const tinygltf::Scene& scene = model.scenes[model.defaultScene > -1 ? model.defaultScene : 0];

    for (int nodeIndex : scene.nodes) {
        ProcessNode(model, nodeIndex, rootNode.get(), convertedMeshes);
    }

    // Initial update
    rootNode->UpdateGlobalTransform(rootNode->localTransform);

    if (outSkeleton && !model.skins.empty())
        BuildSkeletonFromSkin(model, model.skins[0], *outSkeleton);

    return rootNode;
}

// Recursively collects (primitive, nodeLocalToRoot) pairs, folding each
// node's local transform into its parent's so the accumulated matrix maps
// straight from the primitive's original vertex space into modelRoot space.
static void CollectPrimitivesRelativeToRoot(
    SceneNode* node, const XMMATRIX& parentToRoot,
    std::vector<std::pair<const MeshPrimitive*, XMMATRIX>>& out) {
    if (!node) return;

    node->UpdateLocalTransform();
    XMMATRIX localToRoot = XMLoadFloat4x4(&node->localTransform) * parentToRoot;

    if (node->mesh) {
        for (const auto& prim : node->mesh->primitives) {
            if (prim.vbv.BufferLocation != 0) {
                out.push_back({ &prim, localToRoot });
            }
        }
    }

    for (auto& child : node->children) {
        CollectPrimitivesRelativeToRoot(child.get(), localToRoot, out);
    }
}

static Microsoft::WRL::ComPtr<ID3D12Resource> CreateStaticGeometryBuffer(
    ID3D12Device* device, const void* data, UINT sizeBytes,
    D3D12_RESOURCE_STATES finalState, const char* debugLabel) {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (sizeBytes == 0) return resource;
    if (!CreateStaticBufferDX12(device, data, sizeBytes, finalState, resource,
            debugLabel))
        return {};
    return resource;
}

bool GLBImporter::BuildMeshletData(MeshPrimitive& primitive, ID3D12Device* device,
                                   bool buildMeshlets) {
    primitive.indexCount = (UINT)primitive.indices.size();
    if (!device || primitive.vertices.empty()) return false;

    const size_t primitiveVertexCount = primitive.vertices.size() / 12;
    if (primitiveVertexCount > 0) {
        XMFLOAT3 minimum(FLT_MAX, FLT_MAX, FLT_MAX);
        XMFLOAT3 maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (size_t vertex = 0; vertex < primitiveVertexCount; ++vertex) {
            const float* position = &primitive.vertices[vertex * 12];
            minimum.x = (std::min)(minimum.x, position[0]);
            minimum.y = (std::min)(minimum.y, position[1]);
            minimum.z = (std::min)(minimum.z, position[2]);
            maximum.x = (std::max)(maximum.x, position[0]);
            maximum.y = (std::max)(maximum.y, position[1]);
            maximum.z = (std::max)(maximum.z, position[2]);
        }
        primitive.boundsMin = minimum;
        primitive.boundsMax = maximum;
        primitive.boundsValid = true;
    }

    const UINT vbSize = (UINT)(primitive.vertices.size() * sizeof(float));
    primitive.vertexBuffer = CreateStaticGeometryBuffer(device, primitive.vertices.data(), vbSize,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "VertexBuffer");
    if (!primitive.vertexBuffer) return false;
    primitive.vbv.BufferLocation = primitive.vertexBuffer->GetGPUVirtualAddress();
    primitive.vbv.SizeInBytes = vbSize;
    primitive.vbv.StrideInBytes = 12 * sizeof(float);

    if (!primitive.indices.empty()) {
        const UINT ibSize = (UINT)(primitive.indices.size() * sizeof(unsigned int));
        primitive.indexBuffer = CreateStaticGeometryBuffer(device, primitive.indices.data(), ibSize,
            D3D12_RESOURCE_STATE_INDEX_BUFFER |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            "IndexBuffer");
        if (!primitive.indexBuffer) return false;
        primitive.ibv.BufferLocation = primitive.indexBuffer->GetGPUVirtualAddress();
        primitive.ibv.SizeInBytes = ibSize;
        primitive.ibv.Format = DXGI_FORMAT_R32_UINT;
    }

    if (!buildMeshlets) return true;

    constexpr UINT MaxMeshletVertices = 64;
    constexpr UINT MaxMeshletTriangles = 124;
    std::vector<UINT> sourceIndices = primitive.indices;
    if (sourceIndices.empty()) {
        sourceIndices.resize(primitive.vertices.size() / 12);
        for (UINT i = 0; i < sourceIndices.size(); ++i) sourceIndices[i] = i;
    }
    if (sourceIndices.size() < 3) return true;

    const size_t vertexCount = primitiveVertexCount;
    const size_t maxMeshlets = meshopt_buildMeshletsBound(
        sourceIndices.size(), MaxMeshletVertices, MaxMeshletTriangles);
    std::vector<meshopt_Meshlet> optimizedMeshlets(maxMeshlets);
    std::vector<UINT> meshletVertexIndices(maxMeshlets * MaxMeshletVertices);
    std::vector<unsigned char> meshletTriangleBytes(maxMeshlets * MaxMeshletTriangles * 3);
    const size_t meshletCount = meshopt_buildMeshlets(
        optimizedMeshlets.data(), meshletVertexIndices.data(), meshletTriangleBytes.data(),
        sourceIndices.data(), sourceIndices.size(), primitive.vertices.data(), vertexCount,
        12 * sizeof(float), MaxMeshletVertices, MaxMeshletTriangles, 0.25f);
    optimizedMeshlets.resize(meshletCount);
    if (meshletCount > 0) {
        const meshopt_Meshlet& last = optimizedMeshlets.back();
        meshletVertexIndices.resize(last.vertex_offset + last.vertex_count);
        meshletTriangleBytes.resize(last.triangle_offset + last.triangle_count * 3);
    }

    std::vector<MeshletDescDX12> meshlets;
    std::vector<UINT> meshletTriangles;
    std::vector<MeshletBoundsDX12> bounds;
    meshlets.reserve(meshletCount);
    bounds.reserve(meshletCount);
    for (const meshopt_Meshlet& source : optimizedMeshlets) {
        meshlets.push_back({ source.vertex_offset, source.vertex_count,
            (UINT)meshletTriangles.size(), source.triangle_count });
        for (UINT triangle = 0; triangle < source.triangle_count; ++triangle) {
            const unsigned char* local = &meshletTriangleBytes[source.triangle_offset + triangle * 3];
            meshletTriangles.push_back(UINT(local[0]) | (UINT(local[1]) << 8) | (UINT(local[2]) << 16));
        }

        XMFLOAT3 boundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
        XMFLOAT3 boundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (UINT i = 0; i < source.vertex_count; ++i) {
            const UINT vertex = meshletVertexIndices[source.vertex_offset + i];
            const float* p = &primitive.vertices[(size_t)vertex * 12];
            boundsMin.x = std::min(boundsMin.x, p[0]);
            boundsMin.y = std::min(boundsMin.y, p[1]);
            boundsMin.z = std::min(boundsMin.z, p[2]);
            boundsMax.x = std::max(boundsMax.x, p[0]);
            boundsMax.y = std::max(boundsMax.y, p[1]);
            boundsMax.z = std::max(boundsMax.z, p[2]);
        }
        const meshopt_Bounds optimizedBounds = meshopt_computeMeshletBounds(
            &meshletVertexIndices[source.vertex_offset],
            &meshletTriangleBytes[source.triangle_offset], source.triangle_count,
            primitive.vertices.data(), vertexCount, 12 * sizeof(float));
        const XMFLOAT3 center(optimizedBounds.center[0], optimizedBounds.center[1], optimizedBounds.center[2]);
        const XMFLOAT3 coneAxis(optimizedBounds.cone_axis[0], optimizedBounds.cone_axis[1], optimizedBounds.cone_axis[2]);
        const float coneCutoff = (primitive.material && primitive.material->doubleSided)
            ? -1.0f : optimizedBounds.cone_cutoff;
        bounds.push_back({ boundsMin, 0.0f, boundsMax, 0.0f,
            center, optimizedBounds.radius, coneAxis, coneCutoff });
    }

    primitive.meshletCount = (UINT)meshlets.size();
    if (!meshlets.empty()) {
        primitive.meshletDescBuffer = CreateStaticGeometryBuffer(device, meshlets.data(),
            (UINT)(meshlets.size() * sizeof(MeshletDescDX12)),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "MeshletDescBuffer");
        primitive.meshletVertexIndexBuffer = CreateStaticGeometryBuffer(device, meshletVertexIndices.data(),
            (UINT)(meshletVertexIndices.size() * sizeof(UINT)),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "MeshletVertexIndices");
        primitive.meshletTriangleBuffer = CreateStaticGeometryBuffer(device, meshletTriangles.data(),
            (UINT)(meshletTriangles.size() * sizeof(UINT)),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "MeshletTriangles");
        primitive.meshletBoundsBuffer = CreateStaticGeometryBuffer(device, bounds.data(),
            (UINT)(bounds.size() * sizeof(MeshletBoundsDX12)),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "MeshletBoundsBuffer");
    }
    return true;
}

static std::shared_ptr<SceneNode> MergeSceneGeometry(
    const std::shared_ptr<SceneNode>& modelRoot, ComPtr<ID3D12Device> device,
    bool preserveMaterials) {
    if (!modelRoot) return nullptr;

    std::vector<std::pair<const MeshPrimitive*, XMMATRIX>> collected;
    CollectPrimitivesRelativeToRoot(modelRoot.get(), XMMatrixIdentity(), collected);

    // Group by material identity (nullptr = default material bucket)
    std::vector<std::shared_ptr<SceneMaterial>> materialOrder;
    std::vector<std::vector<std::pair<const MeshPrimitive*, XMMATRIX>>> buckets;

    for (auto& entry : collected) {
        std::shared_ptr<SceneMaterial> mat = preserveMaterials
            ? entry.first->material : nullptr;
        int bucketIdx = -1;
        for (size_t i = 0; i < materialOrder.size(); i++) {
            if (materialOrder[i] == mat) { bucketIdx = (int)i; break; }
        }
        if (bucketIdx < 0) {
            bucketIdx = (int)materialOrder.size();
            materialOrder.push_back(mat);
            buckets.push_back({});
        }
        buckets[bucketIdx].push_back(entry);
    }

    auto mergedMesh = std::make_shared<SceneMesh>();
    mergedMesh->name = modelRoot->name + "_Merged";

    for (size_t b = 0; b < buckets.size(); b++) {
        MeshPrimitive merged;
        merged.materialIndex = -1;
        merged.material = materialOrder[b];

        for (auto& entry : buckets[b]) {
            const MeshPrimitive* src = entry.first;
            const XMMATRIX& localToRoot = entry.second;
            XMMATRIX normalMat = XMMatrixTranspose(XMMatrixInverse(nullptr, localToRoot));

            UINT baseVertex = (UINT)(merged.vertices.size() / 12);
            size_t vertCount = src->vertices.size() / 12;

            for (size_t v = 0; v < vertCount; v++) {
                const float* sv = &src->vertices[v * 12];

                XMVECTOR pos = XMVectorSet(sv[0], sv[1], sv[2], 1.0f);
                pos = XMVector3Transform(pos, localToRoot);

                XMVECTOR norm = XMVectorSet(sv[3], sv[4], sv[5], 0.0f);
                norm = XMVector3Normalize(XMVector3TransformNormal(norm, normalMat));

                XMVECTOR tangentVec = XMVectorSet(sv[8], sv[9], sv[10], 0.0f);
                tangentVec = XMVector3Normalize(XMVector3TransformNormal(tangentVec, localToRoot));

                float px, py, pz, nx, ny, nz, tx, ty, tz;
                XMFLOAT3 tmp;
                XMStoreFloat3(&tmp, pos); px = tmp.x; py = tmp.y; pz = tmp.z;
                XMStoreFloat3(&tmp, norm); nx = tmp.x; ny = tmp.y; nz = tmp.z;
                XMStoreFloat3(&tmp, tangentVec); tx = tmp.x; ty = tmp.y; tz = tmp.z;

                merged.vertices.push_back(px);
                merged.vertices.push_back(py);
                merged.vertices.push_back(pz);
                merged.vertices.push_back(nx);
                merged.vertices.push_back(ny);
                merged.vertices.push_back(nz);
                merged.vertices.push_back(sv[6]);
                merged.vertices.push_back(sv[7]);
                merged.vertices.push_back(tx);
                merged.vertices.push_back(ty);
                merged.vertices.push_back(tz);
                merged.vertices.push_back(sv[11]);
            }

            for (unsigned int idx : src->indices) {
                merged.indices.push_back(baseVertex + idx);
            }
            const size_t triangleCount = src->indices.size() / 3u;
            const size_t stableTriangleBase =
                merged.stableTriangleIDs.size();
            if (merged.stableTriangleNamespace == 0u)
                merged.stableTriangleNamespace =
                    src->stableTriangleNamespace;
            for (size_t triangle = 0; triangle < triangleCount; ++triangle) {
                merged.stableTriangleIDs.push_back(
                    triangle < src->stableTriangleIDs.size()
                        ? src->stableTriangleIDs[triangle]
                        : static_cast<unsigned int>(
                              stableTriangleBase + triangle));
            }
        }

        GLBImporter::BuildMeshletData(merged, device.Get(), preserveMaterials);

        mergedMesh->primitives.push_back(std::move(merged));
    }

    auto mergedRoot = std::make_shared<SceneNode>(modelRoot->name);
    mergedRoot->translation = modelRoot->translation;
    mergedRoot->rotation = modelRoot->rotation;
    mergedRoot->scale = modelRoot->scale;
    mergedRoot->mesh = mergedMesh;
    mergedRoot->UpdateLocalTransform();
    mergedRoot->UpdateGlobalTransform(mergedRoot->localTransform);

    return mergedRoot;
}

std::shared_ptr<SceneNode> GLBImporter::MergeSceneByMaterial(
    const std::shared_ptr<SceneNode>& modelRoot, ComPtr<ID3D12Device> device) {
    return MergeSceneGeometry(modelRoot, std::move(device), true);
}

std::shared_ptr<SceneNode> GLBImporter::MergeSceneForDepth(
    const std::shared_ptr<SceneNode>& modelRoot, ComPtr<ID3D12Device> device) {
    return MergeSceneGeometry(modelRoot, std::move(device), false);
}
