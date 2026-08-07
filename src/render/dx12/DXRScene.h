#ifndef DXR_SCENE_H
#define DXR_SCENE_H

#include "DX12Core.h"
#include <DirectXMath.h>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

// Shared static-geometry acceleration structure used by probe GI and
// screen-space raytracing. Dynamic gameplay objects deliberately never enter it.
class DXRScene {
public:
    struct Geometry {
        D3D12_GPU_VIRTUAL_ADDRESS vertexAddress = 0;
        uint32_t vertexCount = 0;
        uint32_t vertexStride = 0;
        DXGI_FORMAT vertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        D3D12_GPU_VIRTUAL_ADDRESS indexAddress = 0;
        uint32_t indexCount = 0;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT;
        bool opaque = true;
    };
    struct Instance {
        uint64_t meshId = 0;
        uint64_t entityId = 0;
        DirectX::XMFLOAT4X4 transform{};
        uint8_t mask = 0xff;
    };
    struct BLAS {
        ComPtr<ID3D12Resource> result;
        ComPtr<ID3D12Resource> scratch;
        uint64_t sourceHash = 0;
    };

    bool Initialize(ID3D12Device* device) {
        supported_ = false;
        device_.Reset();
        if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&device_))))
            return false;
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                &options, sizeof(options))) ||
            options.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
            return false;
        supported_ = true;
        // Tier 1.1 is what inline raytracing (RayQuery in a compute shader)
        // requires. Tier 1.0 hardware can still run the DispatchRays probe path,
        // so this is tracked separately rather than raising the bar for
        // everyone -- the enhanced-visuals tier checks this, the probe GI does
        // not.
        inlineSupported_ = options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
        return true;
    }

    bool Supported() const { return supported_; }
    // True when the device can run RayQuery from compute. Gates enhanced
    // visuals; everything else works without it.
    bool InlineSupported() const { return inlineSupported_; }
    ID3D12Resource* TLAS() const { return tlas_.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS TLASAddress() const {
        return tlas_ ? tlas_->GetGPUVirtualAddress() : 0;
    }
    size_t BLASCount() const { return meshes_.size(); }
    static constexpr uint64_t TerrainMeshId() { return ~uint64_t(0); }

    bool BuildMeshBLAS(ID3D12GraphicsCommandList4* commandList,
                       uint64_t meshId, uint64_t sourceHash,
                       const std::vector<Geometry>& geometries) {
        if (!supported_ || !commandList || geometries.empty()) return false;
        auto current = meshes_.find(meshId);
        if (current != meshes_.end() && current->second.sourceHash == sourceHash)
            return true;

        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descriptions;
        descriptions.reserve(geometries.size());
        for (const Geometry& geometry : geometries) {
            if (!geometry.vertexAddress || geometry.vertexCount < 3) continue;
            D3D12_RAYTRACING_GEOMETRY_DESC description{};
            description.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            description.Flags = geometry.opaque
                ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            description.Triangles.VertexBuffer.StartAddress =
                geometry.vertexAddress;
            description.Triangles.VertexBuffer.StrideInBytes =
                geometry.vertexStride;
            description.Triangles.VertexCount = geometry.vertexCount;
            description.Triangles.VertexFormat = geometry.vertexFormat;
            description.Triangles.IndexBuffer = geometry.indexAddress;
            description.Triangles.IndexCount = geometry.indexCount;
            description.Triangles.IndexFormat = geometry.indexCount
                ? geometry.indexFormat : DXGI_FORMAT_UNKNOWN;
            descriptions.push_back(description);
        }
        if (descriptions.empty()) return false;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = static_cast<uint32_t>(descriptions.size());
        inputs.pGeometryDescs = descriptions.data();
        inputs.Flags =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        if (!info.ResultDataMaxSizeInBytes || !info.ScratchDataSizeInBytes)
            return false;

        BLAS next;
        if (!CreateBuffer(info.ResultDataMaxSizeInBytes,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                D3D12_HEAP_TYPE_DEFAULT, next.result) ||
            !CreateBuffer(info.ScratchDataSizeInBytes,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_HEAP_TYPE_DEFAULT, next.scratch))
            return false;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs = inputs;
        build.DestAccelerationStructureData =
            next.result->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData =
            next.scratch->GetGPUVirtualAddress();
        commandList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = next.result.Get();
        commandList->ResourceBarrier(1, &barrier);
        next.sourceHash = sourceHash;
        meshes_[meshId] = std::move(next);
        topologyDirty_ = true;
        return true;
    }

    bool BuildTerrainBLAS(ID3D12GraphicsCommandList4* commandList,
                          uint64_t sourceHash, const Geometry& geometry) {
        return BuildMeshBLAS(commandList, kTerrainMeshId, sourceHash,
                             std::vector<Geometry>{ geometry });
    }

    bool BuildTerrainBLAS(
        ID3D12GraphicsCommandList4* commandList, uint64_t sourceHash,
        const std::vector<DirectX::XMFLOAT3>& vertices,
        const std::vector<uint32_t>& indices) {
        if (vertices.empty() || indices.empty()) return false;
        const uint64_t vertexBytes = vertices.size() * sizeof(vertices[0]);
        const uint64_t indexBytes = indices.size() * sizeof(indices[0]);
        if (!CreateBuffer(vertexBytes, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD,
                terrainVertices_) ||
            !CreateBuffer(indexBytes, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD,
                terrainIndices_))
            return false;
        void* mapped = nullptr;
        if (FAILED(terrainVertices_->Map(0, nullptr, &mapped))) return false;
        memcpy(mapped, vertices.data(), static_cast<size_t>(vertexBytes));
        terrainVertices_->Unmap(0, nullptr);
        if (FAILED(terrainIndices_->Map(0, nullptr, &mapped))) return false;
        memcpy(mapped, indices.data(), static_cast<size_t>(indexBytes));
        terrainIndices_->Unmap(0, nullptr);
        Geometry geometry;
        geometry.vertexAddress = terrainVertices_->GetGPUVirtualAddress();
        geometry.vertexCount = static_cast<uint32_t>(vertices.size());
        geometry.vertexStride = sizeof(DirectX::XMFLOAT3);
        geometry.indexAddress = terrainIndices_->GetGPUVirtualAddress();
        geometry.indexCount = static_cast<uint32_t>(indices.size());
        return BuildTerrainBLAS(commandList, sourceHash, geometry);
    }

    void RemoveMesh(uint64_t meshId) {
        topologyDirty_ |= meshes_.erase(meshId) != 0;
    }

    bool UpdateTLAS(ID3D12GraphicsCommandList4* commandList,
                    const std::vector<Instance>& instances) {
        if (!supported_ || !commandList) return false;
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descriptions;
        descriptions.reserve(instances.size());
        for (const Instance& source : instances) {
            const auto found = meshes_.find(source.meshId);
            if (found == meshes_.end() || !found->second.result) continue;
            D3D12_RAYTRACING_INSTANCE_DESC instance{};
            StoreDXRTransform(source.transform, instance.Transform);
            instance.InstanceID = static_cast<uint32_t>(source.entityId);
            instance.InstanceMask = source.mask;
            instance.InstanceContributionToHitGroupIndex = 0;
            instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            instance.AccelerationStructure =
                found->second.result->GetGPUVirtualAddress();
            descriptions.push_back(instance);
        }
        if (descriptions.empty()) {
            tlas_.Reset();
            return false;
        }
        const uint64_t instanceBytes =
            descriptions.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        if (!instanceUpload_ ||
            instanceUpload_->GetDesc().Width < instanceBytes) {
            if (!CreateBuffer(instanceBytes, D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    D3D12_HEAP_TYPE_UPLOAD, instanceUpload_))
                return false;
        }
        void* mapped = nullptr;
        if (FAILED(instanceUpload_->Map(0, nullptr, &mapped))) return false;
        memcpy(mapped, descriptions.data(), static_cast<size_t>(instanceBytes));
        instanceUpload_->Unmap(0, nullptr);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = static_cast<uint32_t>(descriptions.size());
        inputs.InstanceDescs = instanceUpload_->GetGPUVirtualAddress();
        inputs.Flags =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        if (!tlas_ || tlas_->GetDesc().Width < info.ResultDataMaxSizeInBytes ||
            topologyDirty_) {
            if (!CreateBuffer(info.ResultDataMaxSizeInBytes,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                    D3D12_HEAP_TYPE_DEFAULT, tlas_) ||
                !CreateBuffer(info.ScratchDataSizeInBytes,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_HEAP_TYPE_DEFAULT, tlasScratch_))
                return false;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs = inputs;
        build.DestAccelerationStructureData = tlas_->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData =
            tlasScratch_->GetGPUVirtualAddress();
        commandList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = tlas_.Get();
        commandList->ResourceBarrier(1, &barrier);
        topologyDirty_ = false;
        return true;
    }

private:
    static constexpr uint64_t kTerrainMeshId = ~uint64_t(0);
    ComPtr<ID3D12Device5> device_;
    std::unordered_map<uint64_t, BLAS> meshes_;
    ComPtr<ID3D12Resource> tlas_;
    ComPtr<ID3D12Resource> tlasScratch_;
    ComPtr<ID3D12Resource> instanceUpload_;
    ComPtr<ID3D12Resource> terrainVertices_;
    ComPtr<ID3D12Resource> terrainIndices_;
    bool supported_ = false;
    bool inlineSupported_ = false;
    bool topologyDirty_ = true;

    bool CreateBuffer(uint64_t bytes, D3D12_RESOURCE_FLAGS flags,
                      D3D12_RESOURCE_STATES state, D3D12_HEAP_TYPE heapType,
                      ComPtr<ID3D12Resource>& output) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = heapType;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = (bytes + 255u) & ~255ull;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = flags;
        return SUCCEEDED(device_->CreateCommittedResource(&heap,
            D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
            IID_PPV_ARGS(&output)));
    }

    static void StoreDXRTransform(const DirectX::XMFLOAT4X4& matrix,
                                  float output[3][4]) {
        output[0][0] = matrix._11; output[0][1] = matrix._21;
        output[0][2] = matrix._31; output[0][3] = matrix._41;
        output[1][0] = matrix._12; output[1][1] = matrix._22;
        output[1][2] = matrix._32; output[1][3] = matrix._42;
        output[2][0] = matrix._13; output[2][1] = matrix._23;
        output[2][2] = matrix._33; output[2][3] = matrix._43;
    }
};

#endif
