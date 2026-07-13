#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
// Define these to avoid tinygltf including them if they are not found, 
// but we hope vcpkg provided them or we need to provide them. 
// Actually tinygltf header usually includes them.
#include <tiny_gltf.h>

#include "GLBImporter.h"
#include "MipGenerator.h"
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
    const tinygltf::Image& image, std::vector<ComPtr<ID3D12Resource>>& uploadHeaps) {
    if (image.width == 0 || image.height == 0) return nullptr;

    UINT baseW = (UINT)image.width;
    UINT baseH = (UINT)image.height;
    UINT16 mipLevels = 1;
    while ((baseW >> mipLevels) > 0 || (baseH >> mipLevels) > 0) mipLevels++;

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.MipLevels = mipLevels;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.Width = baseW;
    textureDesc.Height = baseH;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; // compute mip pass writes via UAV
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

    // Prepare base-level data (force RGBA)
    std::vector<unsigned char> rgba;
    const unsigned char* pSource = image.image.data();
    if (image.component == 3) {
        rgba.resize((size_t)baseW * baseH * 4);
        for (size_t i = 0; i < (size_t)baseW * baseH; i++) {
            rgba[i * 4 + 0] = pSource[i * 3 + 0];
            rgba[i * 4 + 1] = pSource[i * 3 + 1];
            rgba[i * 4 + 2] = pSource[i * 3 + 2];
            rgba[i * 4 + 3] = 255;
        }
        pSource = rgba.data();
    }

    UINT64 uploadBufferSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadBufferSize);

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

    ComPtr<ID3D12Resource> uploadHeap;
    if (FAILED(device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadHeap)))) {
        return nullptr;
    }

    uploadHeaps.push_back(uploadHeap);

    BYTE* pData;
    uploadHeap->Map(0, nullptr, (void**)&pData);
    BYTE* pDest = pData + footprint.Offset;
    for (UINT h = 0; h < baseH; ++h) {
        memcpy(pDest + h * footprint.Footprint.RowPitch,
            pSource + (size_t)h * baseW * 4,
            (size_t)baseW * 4);
    }
    uploadHeap->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = uploadHeap.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Mip 0 -> PIXEL_SHADER_RESOURCE; remaining mips start there too so the
    // compute pass's per-level transitions (which assume that starting state)
    // are uniform across the whole chain even before they've been written.
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
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

ComPtr<ID3D12Resource> GLBImporter::LoadEXRTextureFromFile(const std::string& filepath, ComPtr<ID3D12Device> device,
    ComPtr<ID3D12GraphicsCommandList> commandList, std::vector<ComPtr<ID3D12Resource>>& uploadHeaps) {
    float* pixels = nullptr;
    int width = 0, height = 0;
    const char* err = nullptr;
    if (LoadEXR(&pixels, &width, &height, filepath.c_str(), &err) != TINYEXR_SUCCESS) {
        std::cerr << "Failed to load EXR texture: " << filepath
                  << (err ? (std::string(" (") + err + ")") : "") << std::endl;
        if (err) FreeEXRErrorMessage(err);
        return nullptr;
    }
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
    free(pixels);

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

    ComPtr<ID3D12Resource> uploadHeap;
    if (FAILED(device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadHeap)))) {
        return nullptr;
    }
    uploadHeaps.push_back(uploadHeap);

    BYTE* pData;
    uploadHeap->Map(0, nullptr, (void**)&pData);
    for (UINT16 level = 0; level < mipLevels; ++level) {
        const size_t srcRowBytes = (size_t)mipW[level] * 4 * sizeof(float);
        BYTE* pDest = pData + footprints[level].Offset;
        for (UINT row = 0; row < numRows[level]; ++row) {
            memcpy(pDest + (size_t)row * footprints[level].Footprint.RowPitch,
                (const BYTE*)mips[level].data() + (size_t)row * srcRowBytes,
                srcRowBytes);
        }
    }
    uploadHeap->Unmap(0, nullptr);

    for (UINT16 level = 0; level < mipLevels; ++level) {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = level;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = uploadHeap.Get();
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

std::array<XMFLOAT3, 9> GLBImporter::ComputeSkyIrradianceSH(const std::string& filepath) {
    std::array<XMFLOAT3, 9> coeffs{};
    for (auto& c : coeffs) c = XMFLOAT3(0, 0, 0);

    float* pixels = nullptr;
    int width = 0, height = 0;
    const char* err = nullptr;
    int ret = LoadEXR(&pixels, &width, &height, filepath.c_str(), &err);
    if (ret != TINYEXR_SUCCESS) {
        std::cerr << "Failed to load EXR for sky SH: " << filepath
                   << (err ? (std::string(" (") + err + ")") : "") << std::endl;
        if (err) FreeEXRErrorMessage(err);
        return coeffs;
    }

    // Real SH basis function values (unnormalized-direction form), L0-L2.
    double sh[9];
    double weightSum = 0.0;
    double raw[9][3] = {};

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

            double dx = sinTheta * cos(phi);
            double dz = sinTheta * sin(phi);
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
    free(pixels);

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
            
        // Extract data
        std::vector<XMFLOAT3> positions;
        std::vector<XMFLOAT3> normals;
        std::vector<XMFLOAT2> texCoords;
        std::vector<XMFLOAT4> tangents;
        
        if (posIdx >= 0) CopyBufferData(model, posIdx, positions);
        if (normIdx >= 0) CopyBufferData(model, normIdx, normals);
        if (texIdx >= 0) CopyBufferData(model, texIdx, texCoords);
        if (tangentIdx >= 0) CopyBufferData(model, tangentIdx, tangents);
        
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

std::shared_ptr<SceneNode> GLBImporter::LoadGLB(const std::string& filepath, ComPtr<ID3D12Device> device, ComPtr<ID3D12GraphicsCommandList> commandList) {
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

        // Textures
        // Base Color
        int baseColorIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
        if (baseColorIdx >= 0) {
            int imgIdx = model.textures[baseColorIdx].source;
            if (imgIdx >= 0 && imgIdx < model.images.size())
                sceneMat->baseColorTexture = CreateTexture(device.Get(), commandList.Get(), model.images[imgIdx], sceneMat->uploadHeaps);
        }

        // Metallic Roughness
        int mrIdx = mat.pbrMetallicRoughness.metallicRoughnessTexture.index;
        if (mrIdx >= 0) {
            int imgIdx = model.textures[mrIdx].source;
            if (imgIdx >= 0 && imgIdx < model.images.size())
                sceneMat->metallicRoughnessTexture = CreateTexture(device.Get(), commandList.Get(), model.images[imgIdx], sceneMat->uploadHeaps);
        }

        // Normal
        int nIdx = mat.normalTexture.index;
        if (nIdx >= 0) {
            int imgIdx = model.textures[nIdx].source;
            if (imgIdx >= 0 && imgIdx < model.images.size())
                sceneMat->normalTexture = CreateTexture(device.Get(), commandList.Get(), model.images[imgIdx], sceneMat->uploadHeaps);
        }

        materials.push_back(sceneMat);
    }

    // Convert Meshes
    std::vector<std::shared_ptr<SceneMesh>> convertedMeshes;
    for (const auto& mesh : model.meshes) {
        convertedMeshes.push_back(ProcessMesh(model, mesh, device.Get(), materials));
    }

    // Process Scene
    auto rootNode = std::make_shared<SceneNode>("ModelRoot");
    const tinygltf::Scene& scene = model.scenes[model.defaultScene > -1 ? model.defaultScene : 0];

    for (int nodeIndex : scene.nodes) {
        ProcessNode(model, nodeIndex, rootNode.get(), convertedMeshes);
    }

    // Initial update
    rootNode->UpdateGlobalTransform(rootNode->localTransform);

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

static Microsoft::WRL::ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, const void* data, UINT sizeBytes) {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (sizeBytes == 0) return resource;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeBytes;
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
        IID_PPV_ARGS(&resource));
    if (FAILED(hr)) return nullptr;

    void* mapped;
    resource->Map(0, nullptr, &mapped);
    memcpy(mapped, data, sizeBytes);
    resource->Unmap(0, nullptr);
    return resource;
}

bool GLBImporter::BuildMeshletData(MeshPrimitive& primitive, ID3D12Device* device) {
    primitive.indexCount = (UINT)primitive.indices.size();
    if (!device || primitive.vertices.empty()) return false;

    const UINT vbSize = (UINT)(primitive.vertices.size() * sizeof(float));
    primitive.vertexBuffer = CreateUploadBuffer(device, primitive.vertices.data(), vbSize);
    if (!primitive.vertexBuffer) return false;
    primitive.vbv.BufferLocation = primitive.vertexBuffer->GetGPUVirtualAddress();
    primitive.vbv.SizeInBytes = vbSize;
    primitive.vbv.StrideInBytes = 12 * sizeof(float);

    if (!primitive.indices.empty()) {
        const UINT ibSize = (UINT)(primitive.indices.size() * sizeof(unsigned int));
        primitive.indexBuffer = CreateUploadBuffer(device, primitive.indices.data(), ibSize);
        if (!primitive.indexBuffer) return false;
        primitive.ibv.BufferLocation = primitive.indexBuffer->GetGPUVirtualAddress();
        primitive.ibv.SizeInBytes = ibSize;
        primitive.ibv.Format = DXGI_FORMAT_R32_UINT;
    }

    constexpr UINT MaxMeshletVertices = 64;
    constexpr UINT MaxMeshletTriangles = 124;
    std::vector<UINT> sourceIndices = primitive.indices;
    if (sourceIndices.empty()) {
        sourceIndices.resize(primitive.vertices.size() / 12);
        for (UINT i = 0; i < sourceIndices.size(); ++i) sourceIndices[i] = i;
    }
    if (sourceIndices.size() < 3) return true;

    const size_t vertexCount = primitive.vertices.size() / 12;
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
        primitive.meshletDescBuffer = CreateUploadBuffer(device, meshlets.data(),
            (UINT)(meshlets.size() * sizeof(MeshletDescDX12)));
        primitive.meshletVertexIndexBuffer = CreateUploadBuffer(device, meshletVertexIndices.data(),
            (UINT)(meshletVertexIndices.size() * sizeof(UINT)));
        primitive.meshletTriangleBuffer = CreateUploadBuffer(device, meshletTriangles.data(),
            (UINT)(meshletTriangles.size() * sizeof(UINT)));
        primitive.meshletBoundsBuffer = CreateUploadBuffer(device, bounds.data(),
            (UINT)(bounds.size() * sizeof(MeshletBoundsDX12)));
    }
    return true;
}

std::shared_ptr<SceneNode> GLBImporter::MergeSceneByMaterial(const std::shared_ptr<SceneNode>& modelRoot, ComPtr<ID3D12Device> device) {
    if (!modelRoot) return nullptr;

    std::vector<std::pair<const MeshPrimitive*, XMMATRIX>> collected;
    CollectPrimitivesRelativeToRoot(modelRoot.get(), XMMatrixIdentity(), collected);

    // Group by material identity (nullptr = default material bucket)
    std::vector<std::shared_ptr<SceneMaterial>> materialOrder;
    std::vector<std::vector<std::pair<const MeshPrimitive*, XMMATRIX>>> buckets;

    for (auto& entry : collected) {
        std::shared_ptr<SceneMaterial> mat = entry.first->material;
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
        }

        BuildMeshletData(merged, device.Get());

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
