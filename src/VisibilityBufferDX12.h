#ifndef VISIBILITY_BUFFER_DX12_H
#define VISIBILITY_BUFFER_DX12_H

#include "DX12Core.h"
#include "ShaderDX12.h"
#include "SceneGraph.h"
#include <DirectXPackedVector.h>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

// Instance IDs occupy a full uint in the visibility target. Keep this aligned
// with ShaderDX12's per-frame matrix capacity.
static const UINT VB_MAX_DRAW_CALLS = MAX_DRAW_CALLS_PER_FRAME;
// Maximum total vertices across all draw calls
static const UINT VB_MAX_VERTICES = 1024 * 1024;
// Maximum total indices across all draw calls
static const UINT VB_MAX_INDICES = 1024 * 1024 * 3;
static const UINT VB_CLUSTER_X = 16;
static const UINT VB_CLUSTER_Y = 9;
static const UINT VB_CLUSTER_Z = 10;
static const UINT VB_CLUSTER_COUNT = VB_CLUSTER_X * VB_CLUSTER_Y * VB_CLUSTER_Z;
static const UINT VB_MAX_LIGHTS_PER_CLUSTER = 32;
static const UINT VB_MAX_MATERIALS = 256;
static const UINT VB_MAX_MATERIAL_TEXTURES = 64;

// Must match the compute shader's DrawCallData
struct VBDrawCallData {
    XMFLOAT4X4 modelMatrix;
    XMFLOAT4X4 previousModelMatrix;
    XMFLOAT3   objectColor;
    float      useTexture;
    float      metalness;
    float      roughness;
    float      useNormalMap;
    UINT       materialID;
    UINT       vertexOffset;
    UINT       indexOffset;
    UINT       indexCount;
    UINT       hasIndices;
    UINT       flags;
    XMFLOAT4   palmWindRoot;
};

struct VBMeshData {
    UINT vertexOffset = 0;
    UINT vertexCount = 0;
    UINT indexOffset = 0;
    UINT indexCount = 0;
    UINT hasIndices = 0;
};

struct VBClusterData {
    UINT lightCount = 0;
    UINT lightIndices[VB_MAX_LIGHTS_PER_CLUSTER] = {};
    UINT padding[3] = {};
};

struct VBMaterialData {
    XMFLOAT4 baseColorFactor = { 1, 1, 1, 1 };
    XMFLOAT4 emissiveOcclusion = { 0, 0, 0, 0 };
    XMFLOAT4 pbrParams = { 0, 0.5f, 1, 0 };
    XMFLOAT4 shadingParams = { 1, 0, 0.7f, 0 };
    UINT textureIndices[4] = { UINT_MAX, UINT_MAX, UINT_MAX, 0 };
};

static const UINT VB_INVALID_MESH = UINT_MAX;

// Must match the compute shader's PackedVertex (two float4s).
struct VBPackedVertex {
    XMFLOAT4 d0; // pos.xyz, normal.x
    XMFLOAT4 d1; // normal.yz, uv.xy
};

// Must match FrameConstants in compute shader (256-byte aligned)
struct alignas(256) VBFrameConstants {
    XMMATRIX viewMatrix;
    XMMATRIX projMatrix;
    XMMATRIX invViewProj;
    XMMATRIX shadowCascadeMatrices[SHADOW_CASCADE_COUNT];
    XMMATRIX previousViewProj;
    XMFLOAT4 shadowCascadeSplits;
    XMFLOAT3 cameraPos;
    float    screenWidth;
    float    screenHeight;
    float    nearPlane;
    float    farPlane;
    UINT     debugViewMode;
    UINT     enableMotionVectors;
    UINT     padding[3];
    XMFLOAT4 palmWind;
    XMFLOAT4 palmPrimary;
    XMFLOAT4 palmSecondary;
    XMFLOAT4 palmPreviousPrimary;
    XMFLOAT4 palmPreviousSecondary;
    XMFLOAT4 palmParams;
};

struct alignas(256) VBPostConstants {
    UINT outputWidth;
    UINT outputHeight;
    float exposure;
    float bloomStrength;
    float vignetteStrength;
    float grainStrength;
    UINT frameIndex;
    UINT historyValid;
    float taaFeedback;
    float motionBlurStrength;
    float focusDistance;
    float aperture;
    float nearPlane;
    float farPlane;
    UINT debugViewMode;
    UINT validationMode;
};

struct alignas(256) VBExposureConstants {
    UINT inputWidth;
    UINT inputHeight;
    float adaptationRate;
    float middleGray;
};

class VisibilityBufferDX12 {
public:
    // Full-width instance and primitive IDs (R32G32_UINT).
    ComPtr<ID3D12Resource> visBufferRT;
    ComPtr<ID3D12DescriptorHeap> visRtvHeap;    // RTV for visibility pass
    ComPtr<ID3D12DescriptorHeap> visSrvUavHeap; // SRV/UAV for compute resolve

    // Depth buffer SRV for the compute pass (reads main depth)
    // We'll create a SRV for the engine's existing depth buffer

    // Lighting stays HDR until the dedicated cinematic post pass.
    ComPtr<ID3D12Resource> outputTexture;
    ComPtr<ID3D12DescriptorHeap> outputRtvHeap;
    ComPtr<ID3D12Resource> presentTexture;
    ComPtr<ID3D12Resource> motionTexture;
    // Depth immediately after visibility resolve. Forward extensions can then
    // be detected and rejected from history when they lack motion vectors.
    ComPtr<ID3D12Resource> visibilityDepthTexture;
    ComPtr<ID3D12Resource> historyTextures[2];
    ComPtr<ID3D12Resource> exposureState;
    ComPtr<ID3D12Resource> colorLUT;
    ComPtr<ID3D12Resource> colorLUTUpload;

    // Visibility pass PSO + root signature
    ComPtr<ID3D12RootSignature> visPassRootSig;
    ComPtr<ID3D12PipelineState> visPassPSO;
    ComPtr<ID3D12PipelineState> visPassDoubleSidedPSO;
    ComPtr<ID3D12PipelineState> visPassAlphaPSO;
    ComPtr<ID3D12PipelineState> visPassAlphaDoubleSidedPSO;

    // Compute resolve PSO + root signature
    ComPtr<ID3D12RootSignature> resolveRootSig;
    ComPtr<ID3D12PipelineState> resolvePSO;
    ComPtr<ID3D12RootSignature> postRootSig;
    ComPtr<ID3D12PipelineState> postPSO;
    ComPtr<ID3D12DescriptorHeap> postDescHeap;
    ComPtr<ID3D12RootSignature> exposureRootSig;
    ComPtr<ID3D12PipelineState> exposureResetPSO;
    ComPtr<ID3D12PipelineState> exposureAccumulatePSO;
    ComPtr<ID3D12PipelineState> exposureFinalizePSO;
    ComPtr<ID3D12DescriptorHeap> exposureDescHeap;

    // GPU-visible structured buffers
    ComPtr<ID3D12Resource> drawCallBuffer;       // StructuredBuffer<DrawCallData>
    ComPtr<ID3D12Resource> drawCallUpload;
    ComPtr<ID3D12Resource> vertexDataBuffer;     // StructuredBuffer<PackedVertex>
    ComPtr<ID3D12Resource> vertexDataUpload;
    ComPtr<ID3D12Resource> indexDataBuffer;       // StructuredBuffer<uint>
    ComPtr<ID3D12Resource> indexDataUpload;
    ComPtr<ID3D12Resource> clusterDataBuffer;     // StructuredBuffer<ClusterData>
    ComPtr<ID3D12Resource> clusterDataUpload;
    ComPtr<ID3D12Resource> materialDataBuffer;
    VBMaterialData* mappedMaterials = nullptr;

    // Upload buffer for frame constants
    UploadBuffer<VBFrameConstants> frameConstantBuffer;
    UploadBuffer<VBPostConstants> postConstantBuffer;
    UploadBuffer<VBExposureConstants> exposureConstantBuffer;

    // Descriptor heap for compute pass SRVs/UAVs
    ComPtr<ID3D12DescriptorHeap> computeDescHeap;

    // CPU-side staging data
    std::vector<VBDrawCallData> cpuDrawCalls;
    std::vector<VBPackedVertex> cpuVertices;
    std::vector<UINT>           cpuIndices;
    std::vector<VBClusterData>  cpuClusters;
    std::vector<XMFLOAT4X4>     previousModels;
    std::unordered_map<uint64_t, XMFLOAT4X4> previousModelByInstance;
    std::vector<VBMeshData>     meshes;
    std::unordered_map<const MeshPrimitive*, UINT> primitiveMeshLookup;
    std::unordered_map<const SceneMaterial*, UINT> materialLookup;
    std::unordered_map<ID3D12Resource*, UINT> materialTextureLookup;
    UINT materialCount = 1;
    UINT materialTextureCount = 0;
    UINT currentDrawCall = 0;
    UINT previousDrawCount = 0;
    UINT drawCallDirtyMin = UINT_MAX;
    UINT drawCallDirtyMax = 0;
    UINT persistentVertexCount = 0;
    UINT persistentIndexCount = 0;
    bool geometryUploaded = false;
    bool geometryDirty = false;
    UINT postFrameIndex = 0;
    float exposure = 1.15f;
    float bloomStrength = 0.12f;
    float vignetteStrength = 0.18f;
    float grainStrength = 0.012f;
    float taaFeedback = 0.86f;
    bool temporalEffectsEnabled = false;
    bool temporalHistoryValid = false;
    bool exposureReadable = false;
    float exposureAdaptation = 0.05f;
    float motionBlurStrength = 0.0f;
    PalmWindFrameDX12 palmWindFrame{};
    float focusDistance = 8.0f;
    float aperture = 0.0f;
    float currentNearPlane = 0.1f;
    float currentFarPlane = 1000.0f;
    int debugViewMode = 0; // 0=lit, 1=instance/primitive IDs, 2=depth
    bool validationMode = false;

    UINT width = 0;
    UINT height = 0;
    bool initialized = false;
    std::string initError;

    XMFLOAT2 GetTemporalJitterPixels() const {
        if (!temporalEffectsEnabled || validationMode) return { 0.0f, 0.0f };
        // Eight-sample Halton(2,3), centered on pixel. Sequence repeats only
        // after covering complementary sub-pixel locations.
        static constexpr XMFLOAT2 sequence[8] = {
            { 0.0f,       -0.1666667f },
            { -0.25f,      0.1666667f },
            { 0.25f,      -0.3888889f },
            { -0.375f,    -0.0555556f },
            { 0.125f,      0.2777778f },
            { -0.125f,    -0.2777778f },
            { 0.375f,      0.0555556f },
            { -0.4375f,    0.3888889f }
        };
        return sequence[postFrameIndex & 7u];
    }

    void InvalidateTemporalHistory() { temporalHistoryValid = false; }

    bool Init(UINT screenWidth, UINT screenHeight) {
        width = screenWidth;
        height = screenHeight;

        initError.clear();
        auto require = [&](bool success, const char* stage) {
            if (success) return true;
            initError = stage;
            std::ofstream log("visibility_buffer_error.log", std::ios::trunc);
            log << "Visibility buffer initialization failed: " << stage << '\n';
            return false;
        };
        if (!require(CreateVisBufferRT(), "visibility target")) return false;
        if (!require(CreateOutputTexture(), "output textures")) return false;
        if (!require(CreateColorLUT(), "colour LUT")) return false;
        if (!require(CreateStructuredBuffers(), "structured buffers")) return false;
        if (!require(CreateComputeDescriptorHeap(), "compute descriptors")) return false;
        if (!require(CreateVisPassPipeline(), "visibility shaders")) return false;
        if (!require(CreateResolvePipeline(), "resolve shader")) return false;
        if (!require(CreatePostPipeline(), "post-process shader")) return false;
        if (!require(CreateExposurePipeline(), "exposure shaders")) return false;

        if (!require(frameConstantBuffer.Create(FRAME_COUNT), "frame constants")) return false;
        if (!require(postConstantBuffer.Create(FRAME_COUNT), "post constants")) return false;
        if (!require(exposureConstantBuffer.Create(FRAME_COUNT), "exposure constants")) return false;

        cpuDrawCalls.resize(VB_MAX_DRAW_CALLS);
        cpuVertices.resize(VB_MAX_VERTICES);
        cpuIndices.resize(VB_MAX_INDICES);
        cpuClusters.resize(VB_CLUSTER_COUNT);
        previousModels.resize(VB_MAX_DRAW_CALLS);
        if (mappedMaterials) mappedMaterials[0] = VBMaterialData{};

        initialized = true;
        std::cout << "Visibility Buffer initialized (" << width << "x" << height << ")" << std::endl;
        return true;
    }

    void BeginFrame() {
        if (previousModelByInstance.size() >
            static_cast<size_t>(VB_MAX_DRAW_CALLS) * 4u)
            previousModelByInstance.clear();
        previousDrawCount = currentDrawCall;
        currentDrawCall = 0;
        drawCallDirtyMin = UINT_MAX;
        drawCallDirtyMax = 0;
    }

    void SetCluster(UINT clusterIndex, UINT lightCount, const int* lightIndices) {
        if (clusterIndex >= cpuClusters.size()) return;
        VBClusterData& cluster = cpuClusters[clusterIndex];
        cluster.lightCount = (std::min)(lightCount, VB_MAX_LIGHTS_PER_CLUSTER);
        for (UINT i = 0; i < cluster.lightCount; ++i)
            cluster.lightIndices[i] = static_cast<UINT>(lightIndices[i]);
    }

    UINT RegisterMaterial(const SceneMaterial* material) {
        if (!material) return 0;
        auto found = materialLookup.find(material);
        if (found != materialLookup.end()) return found->second;
        if (materialCount >= VB_MAX_MATERIALS) return 0;

        VBMaterialData data;
        data.baseColorFactor = material->baseColorFactor;
        data.emissiveOcclusion.w = material->occlusionStrength;
        data.pbrParams = XMFLOAT4(material->metallicFactor,
            material->roughnessFactor, material->normalYSign, 1.0f);
        data.shadingParams = XMFLOAT4(material->ambientScale,
            material->viewFillStrength, 0.7f, 0.0f);
        auto addTexture = [&](ID3D12Resource* texture) -> UINT {
            if (!texture) return UINT_MAX;
            auto foundTexture = materialTextureLookup.find(texture);
            if (foundTexture != materialTextureLookup.end())
                return foundTexture->second;
            if (materialTextureCount >= VB_MAX_MATERIAL_TEXTURES)
                return UINT_MAX;
            const UINT textureIndex = materialTextureCount++;
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = texture;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_dx12.commandList->ResourceBarrier(1, &barrier);
            UINT descriptorSize = g_dx12.cbvSrvUavDescriptorSize;
            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                computeDescHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += (SIZE_T)descriptorSize * (8 + textureIndex);
            D3D12_RESOURCE_DESC resource = texture->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = resource.Format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = resource.MipLevels;
            g_dx12.device->CreateShaderResourceView(texture, &srv, handle);
            materialTextureLookup.emplace(texture, textureIndex);
            return textureIndex;
        };
        data.textureIndices[0] = addTexture(material->baseColorTexture.Get());
        data.textureIndices[1] = addTexture(material->normalTexture.Get());
        data.textureIndices[2] = addTexture(material->metallicRoughnessTexture.Get());
        data.textureIndices[3] = material->roughnessOnlyTexture ? 1u : 0u;

        const UINT id = materialCount++;
        mappedMaterials[id] = data;
        materialLookup.emplace(material, id);
        return id;
    }

    // Upload-once mesh registration. Instances reference this immutable geometry
    // every frame instead of duplicating vertices per draw.
    UINT RegisterMesh(const float* vertexData, UINT vertexCount,
                      const UINT* indexData, UINT indexCount,
                      UINT vertexStrideFloats = 8) {
        if (!vertexData || vertexCount == 0 ||
            persistentVertexCount + vertexCount > VB_MAX_VERTICES ||
            persistentIndexCount + indexCount > VB_MAX_INDICES) {
            return VB_INVALID_MESH;
        }

        VBMeshData mesh;
        mesh.vertexOffset = persistentVertexCount;
        mesh.vertexCount = vertexCount;
        mesh.indexOffset = persistentIndexCount;
        mesh.indexCount = indexCount;
        mesh.hasIndices = (indexData && indexCount > 0) ? 1u : 0u;

        for (UINT i = 0; i < vertexCount; ++i) {
            const float* v = vertexData + i * vertexStrideFloats;
            VBPackedVertex& pv = cpuVertices[persistentVertexCount + i];
            pv.d0 = XMFLOAT4(v[0], v[1], v[2], v[3]);
            pv.d1 = XMFLOAT4(v[4], v[5], v[6], v[7]);
        }
        if (mesh.hasIndices) {
            memcpy(cpuIndices.data() + persistentIndexCount, indexData,
                   indexCount * sizeof(UINT));
        }

        persistentVertexCount += vertexCount;
        persistentIndexCount += indexCount;
        meshes.push_back(mesh);
        geometryDirty = true;
        return static_cast<UINT>(meshes.size() - 1);
    }

    UINT RegisterPrimitive(MeshPrimitive* primitive) {
        if (!primitive || primitive->vertices.empty()) return VB_INVALID_MESH;
        auto found = primitiveMeshLookup.find(primitive);
        if (found != primitiveMeshLookup.end()) {
            // Spatial batches replace their merged SceneNode after a cell
            // changes. The allocator can reuse the old MeshPrimitive address,
            // but a freshly constructed primitive has not been registered yet.
            // Do not bind the recycled address to the old material bucket's
            // geometry.
            if (primitive->visibilityMeshID == found->second)
                return found->second;
            primitiveMeshLookup.erase(found);
        }
        const UINT mesh = RegisterMesh(primitive->vertices.data(),
            static_cast<UINT>(primitive->vertices.size() / 12),
            primitive->indices.empty() ? nullptr : primitive->indices.data(),
            static_cast<UINT>(primitive->indices.size()), 12);
        if (mesh != VB_INVALID_MESH) {
            primitiveMeshLookup.emplace(primitive, mesh);
            primitive->visibilityMeshID = mesh;
        }
        return mesh;
    }

    // Register only mutable instance/material data for this frame.
    UINT RegisterInstance(UINT meshID, const XMMATRIX& modelMatrix,
                          const XMFLOAT3& color, float metalness, float roughness,
                          UINT materialID = 0, UINT flags = 0,
                          uint64_t instanceKey = 0,
                          XMFLOAT4 palmWindRoot = {}) {
        if (currentDrawCall >= VB_MAX_DRAW_CALLS || meshID >= meshes.size())
            return UINT_MAX;

        UINT dcID = currentDrawCall;
        VBDrawCallData& dc = cpuDrawCalls[dcID];
        VBDrawCallData next = {};
        const VBMeshData& mesh = meshes[meshID];

        XMMATRIX transposed = XMMatrixTranspose(modelMatrix);
        XMStoreFloat4x4(&next.modelMatrix, transposed);
        auto previous = temporalEffectsEnabled && instanceKey
            ? previousModelByInstance.find(instanceKey)
            : previousModelByInstance.end();
        if (!temporalEffectsEnabled) {
            next.previousModelMatrix = next.modelMatrix;
        } else if (previous != previousModelByInstance.end()) {
            next.previousModelMatrix = previous->second;
        } else if (instanceKey == 0 && dcID < previousDrawCount) {
            next.previousModelMatrix = previousModels[dcID];
        } else {
            next.previousModelMatrix = next.modelMatrix;
        }
        previousModels[dcID] = next.modelMatrix;
        if (temporalEffectsEnabled && instanceKey)
            previousModelByInstance[instanceKey] = next.modelMatrix;

        next.objectColor = color;
        next.useTexture = 0.0f;
        next.metalness = metalness;
        next.roughness = roughness;
        next.useNormalMap = 0.0f;
        next.materialID = materialID;
        next.vertexOffset = mesh.vertexOffset;
        next.indexOffset = mesh.indexOffset;
        next.indexCount = mesh.indexCount;
        next.hasIndices = mesh.hasIndices;
        next.flags = flags;
        next.palmWindRoot = palmWindRoot;
        if (dcID >= previousDrawCount ||
            memcmp(&dc, &next, sizeof(VBDrawCallData)) != 0) {
            dc = next;
            drawCallDirtyMin = (std::min)(drawCallDirtyMin, dcID);
            drawCallDirtyMax = (std::max)(drawCallDirtyMax, dcID);
        }

        currentDrawCall++;
        return dcID;
    }

    // Upload all CPU-side data to GPU before the resolve pass
    void UploadBuffers(ID3D12GraphicsCommandList* cmdList) {
        // Upload draw calls
        if (drawCallDirtyMin != UINT_MAX) {
            const UINT64 offset = static_cast<UINT64>(drawCallDirtyMin) *
                sizeof(VBDrawCallData);
            const UINT64 size = static_cast<UINT64>(
                drawCallDirtyMax - drawCallDirtyMin + 1) *
                sizeof(VBDrawCallData);
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            drawCallUpload->Map(0, &readRange, &mapped);
            memcpy(static_cast<uint8_t*>(mapped) + offset,
                cpuDrawCalls.data() + drawCallDirtyMin, size);
            drawCallUpload->Unmap(0, nullptr);

            cmdList->CopyBufferRegion(drawCallBuffer.Get(), offset,
                drawCallUpload.Get(), offset, size);
        }

        // Geometry changes only when a mesh is added. DEFAULT buffers stay SRVs
        // between frames, eliminating per-instance vertex/index uploads.
        if (geometryDirty && geometryUploaded) {
            D3D12_RESOURCE_BARRIER geometryToCopy[2] = {};
            for (UINT i = 0; i < 2; ++i) {
                geometryToCopy[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                geometryToCopy[i].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                geometryToCopy[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                geometryToCopy[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            geometryToCopy[0].Transition.pResource = vertexDataBuffer.Get();
            geometryToCopy[1].Transition.pResource = indexDataBuffer.Get();
            cmdList->ResourceBarrier(2, geometryToCopy);
        }


        // CPU cluster construction already exists in ClusteredRendererDX12.
        // Upload its compact light lists instead of scanning every light per pixel.
        {
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            clusterDataUpload->Map(0, &readRange, &mapped);
            memcpy(mapped, cpuClusters.data(),
                   cpuClusters.size() * sizeof(VBClusterData));
            clusterDataUpload->Unmap(0, nullptr);
            cmdList->CopyBufferRegion(clusterDataBuffer.Get(), 0,
                clusterDataUpload.Get(), 0,
                cpuClusters.size() * sizeof(VBClusterData));
        }

        if (geometryDirty && persistentVertexCount > 0) {
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            vertexDataUpload->Map(0, &readRange, &mapped);
            memcpy(mapped, cpuVertices.data(), persistentVertexCount * sizeof(VBPackedVertex));
            vertexDataUpload->Unmap(0, nullptr);

            cmdList->CopyBufferRegion(vertexDataBuffer.Get(), 0,
                vertexDataUpload.Get(), 0, persistentVertexCount * sizeof(VBPackedVertex));
        }

        if (geometryDirty && persistentIndexCount > 0) {
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            indexDataUpload->Map(0, &readRange, &mapped);
            memcpy(mapped, cpuIndices.data(), persistentIndexCount * sizeof(UINT));
            indexDataUpload->Unmap(0, nullptr);

            cmdList->CopyBufferRegion(indexDataBuffer.Get(), 0,
                indexDataUpload.Get(), 0, persistentIndexCount * sizeof(UINT));
        }

        // Barriers: transition structured buffers from copy dest to SRV
        D3D12_RESOURCE_BARRIER barriers[4] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = drawCallBuffer.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = clusterDataBuffer.Get();

        UINT barrierCount = 2;
        if (geometryDirty) {
            barriers[2] = barriers[0];
            barriers[2].Transition.pResource = vertexDataBuffer.Get();
            barriers[3] = barriers[0];
            barriers[3].Transition.pResource = indexDataBuffer.Get();
            barrierCount = 4;
            geometryUploaded = true;
            geometryDirty = false;
        }
        cmdList->ResourceBarrier(barrierCount, barriers);
    }

    // Transition structured buffers back to copy dest for next frame
    void TransitionBuffersForUpload(ID3D12GraphicsCommandList* cmdList) {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = drawCallBuffer.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = clusterDataBuffer.Get();
        cmdList->ResourceBarrier(2, barriers);
    }

    // Begin the visibility pass: clear VB RT, set render targets
    void BeginVisibilityPass(ID3D12GraphicsCommandList* cmdList) {
        // Transition vis buffer to render target
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = visBufferRT.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);

        // Zero means background. Stored instance IDs are biased by one.
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = visRtvHeap->GetCPUDescriptorHandleForHeapStart();
        const float clearValue[4] = {};
        cmdList->ClearRenderTargetView(rtvHandle, clearValue, 0, nullptr);

        // Also clear main depth
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        // Set render targets: vis buffer + main depth buffer
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

        // Set viewport / scissor
        cmdList->RSSetViewports(1, &g_dx12.viewport);
        cmdList->RSSetScissorRects(1, &g_dx12.scissorRect);

        // Set pipeline
        ID3D12DescriptorHeap* heaps[] = { computeDescHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootSignature(visPassRootSig.Get());
        cmdList->SetGraphicsRootShaderResourceView(3,
            drawCallBuffer->GetGPUVirtualAddress());
        D3D12_GPU_DESCRIPTOR_HANDLE defaultTexture =
            computeDescHeap->GetGPUDescriptorHandleForHeapStart();
        defaultTexture.ptr += static_cast<UINT64>(
            g_dx12.cbvSrvUavDescriptorSize) * 8u;
        cmdList->SetGraphicsRootDescriptorTable(2, defaultTexture);
        cmdList->SetPipelineState(visPassPSO.Get());
    }

    void SetVisPassDraw(ID3D12GraphicsCommandList* cmdList, UINT drawCallID,
                        UINT materialID, bool doubleSided, bool alphaCutout,
                        bool alphaFromLuminance) {
        const UINT constants[4] = { drawCallID, alphaCutout ? 1u : 0u,
            alphaFromLuminance ? 1u : 0u, 0u };
        cmdList->SetGraphicsRoot32BitConstants(1, 4, constants, 0);

        UINT textureIndex = 0;
        if (materialID < materialCount &&
            mappedMaterials[materialID].textureIndices[0] < VB_MAX_MATERIAL_TEXTURES)
            textureIndex = mappedMaterials[materialID].textureIndices[0];
        D3D12_GPU_DESCRIPTOR_HANDLE texture =
            computeDescHeap->GetGPUDescriptorHandleForHeapStart();
        texture.ptr += static_cast<UINT64>(g_dx12.cbvSrvUavDescriptorSize) *
            (8u + textureIndex);
        cmdList->SetGraphicsRootDescriptorTable(2, texture);
        cmdList->SetPipelineState(alphaCutout
            ? (doubleSided ? visPassAlphaDoubleSidedPSO.Get()
                           : visPassAlphaPSO.Get())
            : (doubleSided ? visPassDoubleSidedPSO.Get()
                           : visPassPSO.Get()));
    }

    void SetPalmWindFrame(const PalmWindFrameDX12& frame) {
        palmWindFrame = frame;
    }

    // Set matrices for the current draw (reuses the matrix CBV at slot 0)
    void SetVisPassMatrices(ID3D12GraphicsCommandList* cmdList,
                            const XMMATRIX& model, const XMMATRIX& view,
                            const XMMATRIX& proj, const XMMATRIX& lightSpace,
                            ShaderDX12& matrixSource, UINT drawIndex) {
        // We reuse the existing matrix buffer from ShaderDX12
        UINT bufferIndex = g_dx12.frameIndex * MAX_DRAW_CALLS_PER_FRAME + drawIndex;

        MatrixBufferDX12 data = {};
        data.model = XMMatrixTranspose(model);
        data.view = XMMatrixTranspose(view);
        data.projection = XMMatrixTranspose(proj);
        data.lightSpaceMatrix = XMMatrixTranspose(lightSpace);
        data.palmWind = matrixSource.palmWindFrame.wind;
        data.palmPrimary = matrixSource.palmWindFrame.primary;
        data.palmSecondary = matrixSource.palmWindFrame.secondary;
        data.palmPreviousPrimary = matrixSource.palmWindFrame.previousPrimary;
        data.palmPreviousSecondary = matrixSource.palmWindFrame.previousSecondary;
        data.palmParams = matrixSource.palmWindFrame.params;
        matrixSource.matrixBuffer.CopyData(bufferIndex, data);

        cmdList->SetGraphicsRootConstantBufferView(0,
            matrixSource.matrixBuffer.GetGPUAddress(bufferIndex));
    }

    void EndVisibilityPass(ID3D12GraphicsCommandList* cmdList) {
        // Transition vis buffer to SRV for compute
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = visBufferRT.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    // Indirect dispatch for compute resolve
    ComPtr<ID3D12CommandSignature> resolveDispatchSignature;
    ComPtr<ID3D12Resource> resolveDispatchArgsBuffer;
    D3D12_DISPATCH_ARGUMENTS* mappedResolveDispatchArgs = nullptr;

    // Run the compute resolve pass
    void Resolve(ID3D12GraphicsCommandList* cmdList,
                 const XMMATRIX& view, const XMMATRIX& proj,
                 const XMMATRIX& lightViewProj,
                 const XMMATRIX& previousViewProj,
                 const XMFLOAT3& cameraPos,
                 float nearPlane, float farPlane,
                 const LightBufferDX12& lightData,
                 const PointLightsBufferDX12& pointLightData) {
        currentNearPlane = nearPlane;
        currentFarPlane = farPlane;
        // Transition HDR and motion-vector outputs to UAV.
        {
            D3D12_RESOURCE_BARRIER barriers[2] = {};
            for (UINT i = 0; i < 2; ++i) {
                barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            barriers[0].Transition.pResource = outputTexture.Get();
            barriers[1].Transition.pResource = motionTexture.Get();
            cmdList->ResourceBarrier(2, barriers);
        }

        // Also transition depth buffer to SRV for reading
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = g_dx12.depthStencilBuffer.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }

        // Upload frame constants
        VBFrameConstants fc;
        fc.viewMatrix = XMMatrixTranspose(view);
        fc.projMatrix = XMMatrixTranspose(proj);
        XMMATRIX invVP = XMMatrixInverse(nullptr, view * proj);
        fc.invViewProj = XMMatrixTranspose(invVP);
        for (UINT i = 0; i < SHADOW_CASCADE_COUNT; ++i)
            fc.shadowCascadeMatrices[i] = XMMatrixTranspose(g_shadowCascadeMatrices[i]);
        fc.previousViewProj = XMMatrixTranspose(previousViewProj);
        fc.shadowCascadeSplits = g_shadowCascadeSplits;
        fc.cameraPos = cameraPos;
        fc.screenWidth = (float)width;
        fc.screenHeight = (float)height;
        fc.nearPlane = nearPlane;
        fc.farPlane = farPlane;
        fc.debugViewMode = static_cast<UINT>(debugViewMode);
        fc.enableMotionVectors = temporalEffectsEnabled ? 1u : 0u;
        fc.palmWind = palmWindFrame.wind;
        fc.palmPrimary = palmWindFrame.primary;
        fc.palmSecondary = palmWindFrame.secondary;
        fc.palmPreviousPrimary = palmWindFrame.previousPrimary;
        fc.palmPreviousSecondary = palmWindFrame.previousSecondary;
        fc.palmParams = palmWindFrame.params;
        frameConstantBuffer.CopyData(g_dx12.frameIndex, fc);

        // Set compute pipeline
        cmdList->SetComputeRootSignature(resolveRootSig.Get());
        cmdList->SetPipelineState(resolvePSO.Get());

        // Set descriptor heap
        ID3D12DescriptorHeap* heaps[] = { computeDescHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        // Bind root parameters
        // b0 - frame constants
        cmdList->SetComputeRootConstantBufferView(0,
            frameConstantBuffer.GetGPUAddress(g_dx12.frameIndex));

        // b1 - light buffer (we'll upload via a temporary inline approach)
        // Actually, we reuse the mainShader's lightBuffer
        // For simplicity, create inline CBVs using the mainShader's addresses
        // We'll pass these from outside. For now, bind the descriptor table.

        // Descriptor table at root param 1 (SRVs + UAV)
        cmdList->SetComputeRootDescriptorTable(1,
            computeDescHeap->GetGPUDescriptorHandleForHeapStart());

        // Dispatch (GPU-driven via ExecuteIndirect)
        UINT groupsX = (width + 7) / 8;
        UINT groupsY = (height + 7) / 8;
        if (mappedResolveDispatchArgs) {
            mappedResolveDispatchArgs->ThreadGroupCountX = groupsX;
            mappedResolveDispatchArgs->ThreadGroupCountY = groupsY;
            mappedResolveDispatchArgs->ThreadGroupCountZ = 1;
        }
        if (resolveDispatchSignature && resolveDispatchArgsBuffer) {
            cmdList->ExecuteIndirect(resolveDispatchSignature.Get(), 1, resolveDispatchArgsBuffer.Get(), 0, nullptr, 0);
        } else {
            cmdList->Dispatch(groupsX, groupsY, 1);
        }

        // Keep linear HDR and motion vectors as SRVs for post processing.
        {
            D3D12_RESOURCE_BARRIER barriers[2] = {};
            for (UINT i = 0; i < 2; ++i) {
                barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            barriers[0].Transition.pResource = outputTexture.Get();
            barriers[1].Transition.pResource = motionTexture.Get();
            cmdList->ResourceBarrier(2, barriers);
        }

        // Preserve visibility depth before forward-only animated/alpha-tested
        // geometry modifies it. Post uses the difference as a reactive mask.
        {
            D3D12_RESOURCE_BARRIER barriers[3] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Transition.pResource = g_dx12.depthStencilBuffer.Get();
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[1] = barriers[0];
            barriers[1].Transition.pResource = visibilityDepthTexture.Get();
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            cmdList->ResourceBarrier(2, barriers);
            cmdList->CopyResource(visibilityDepthTexture.Get(),
                                  g_dx12.depthStencilBuffer.Get());
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            cmdList->ResourceBarrier(2, barriers);
        }

    }

    void UpdateExposure(ID3D12GraphicsCommandList* cmdList) {
        if (exposureReadable) {
            D3D12_RESOURCE_BARRIER transition = {};
            transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transition.Transition.pResource = exposureState.Get();
            transition.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            transition.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &transition);
        }

        VBExposureConstants constants = {};
        constants.inputWidth = width;
        constants.inputHeight = height;
        constants.adaptationRate = exposureAdaptation;
        constants.middleGray = 0.18f;
        exposureConstantBuffer.CopyData(g_dx12.frameIndex, constants);

        cmdList->SetComputeRootSignature(exposureRootSig.Get());
        ID3D12DescriptorHeap* heaps[] = { exposureDescHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootConstantBufferView(0,
            exposureConstantBuffer.GetGPUAddress(g_dx12.frameIndex));
        cmdList->SetComputeRootDescriptorTable(1,
            exposureDescHeap->GetGPUDescriptorHandleForHeapStart());

        D3D12_RESOURCE_BARRIER uav = {};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = exposureState.Get();
        cmdList->SetPipelineState(exposureResetPSO.Get());
        cmdList->Dispatch(1, 1, 1);
        cmdList->ResourceBarrier(1, &uav);
        cmdList->SetPipelineState(exposureAccumulatePSO.Get());
        cmdList->Dispatch((width + 127) / 128, (height + 127) / 128, 1);
        cmdList->ResourceBarrier(1, &uav);
        cmdList->SetPipelineState(exposureFinalizePSO.Get());
        cmdList->Dispatch(1, 1, 1);
        cmdList->ResourceBarrier(1, &uav);

        D3D12_RESOURCE_BARRIER transition = {};
        transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Transition.pResource = exposureState.Get();
        transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        transition.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &transition);
        exposureReadable = true;
    }

    // Composite forward-only materials into the same linear HDR image produced
    // by the visibility resolve. Post-processing must run after this range.
    void BeginHDRBackground(ID3D12GraphicsCommandList* cmdList) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = outputTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetOutputRTV();
        // Never preserve previous-frame HDR contents. If sky initialization
        // fails or its draw is skipped, background resolve intentionally leaves
        // untouched pixels alone, so an uncleared target becomes feedback.
        const float clearColor[4] = {};
        cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmdList->RSSetViewports(1, &g_dx12.viewport);
        cmdList->RSSetScissorRects(1, &g_dx12.scissorRect);
    }

    void EndHDRBackground(ID3D12GraphicsCommandList* cmdList) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = outputTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    void BeginForwardExtensions(ID3D12GraphicsCommandList* cmdList) {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = outputTexture.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = g_dx12.depthStencilBuffer.Get();
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        cmdList->ResourceBarrier(2, barriers);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetOutputRTV();
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        cmdList->RSSetViewports(1, &g_dx12.viewport);
        cmdList->RSSetScissorRects(1, &g_dx12.scissorRect);
    }

    void EndForwardExtensions(ID3D12GraphicsCommandList* cmdList) {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = outputTexture.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = g_dx12.depthStencilBuffer.Get();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        cmdList->ResourceBarrier(2, barriers);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputRTV() const {
        return outputRtvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    ID3D12Resource* GetOutputResource() const { return outputTexture.Get(); }
    ID3D12Resource* GetMotionResource() const { return motionTexture.Get(); }
    ID3D12Resource* GetVisibilityDepthResource() const {
        return visibilityDepthTexture.Get();
    }

    void PostProcess(ID3D12GraphicsCommandList* cmdList, bool allowHistory) {
        const UINT historyIndex = postFrameIndex & 1u;
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = presentTexture.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = historyTextures[historyIndex].Get();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cmdList->ResourceBarrier(2, barriers);

        VBPostConstants constants = {};
        constants.outputWidth = width;
        constants.outputHeight = height;
        constants.exposure = validationMode ? 1.0f : exposure;
        constants.bloomStrength = validationMode ? 0.0f : bloomStrength;
        constants.vignetteStrength = validationMode ? 0.0f : vignetteStrength;
        constants.grainStrength = validationMode ? 0.0f : grainStrength;
        constants.frameIndex = postFrameIndex++;
        constants.historyValid = (temporalEffectsEnabled && !validationMode &&
            allowHistory && temporalHistoryValid) ? 1u : 0u;
        constants.taaFeedback = (temporalEffectsEnabled && !validationMode)
            ? taaFeedback : 0.0f;
        constants.motionBlurStrength = (temporalEffectsEnabled &&
            !validationMode) ? motionBlurStrength : 0.0f;
        constants.focusDistance = focusDistance;
        // Depth of field disabled. It blurred the entire game view whenever
        // parity validation was off.
        constants.aperture = 0.0f;
        constants.nearPlane = currentNearPlane;
        constants.farPlane = currentFarPlane;
        constants.debugViewMode = static_cast<UINT>(debugViewMode);
        constants.validationMode = validationMode ? 1u : 0u;
        postConstantBuffer.CopyData(g_dx12.frameIndex, constants);

        cmdList->SetComputeRootSignature(postRootSig.Get());
        cmdList->SetPipelineState(postPSO.Get());
        ID3D12DescriptorHeap* heaps[] = { postDescHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootConstantBufferView(0,
            postConstantBuffer.GetGPUAddress(g_dx12.frameIndex));
        D3D12_GPU_DESCRIPTOR_HANDLE table =
            postDescHeap->GetGPUDescriptorHandleForHeapStart();
        table.ptr += (UINT64)g_dx12.cbvSrvUavDescriptorSize * historyIndex * 9;
        cmdList->SetComputeRootDescriptorTable(1, table);
        cmdList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cmdList->ResourceBarrier(2, barriers);
        D3D12_RESOURCE_BARRIER depth = {};
        depth.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        depth.Transition.pResource = g_dx12.depthStencilBuffer.Get();
        depth.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        depth.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        depth.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &depth);
        temporalHistoryValid = temporalEffectsEnabled;
    }

    // Copy the resolved output to the back buffer
    void CopyToBackBuffer(ID3D12GraphicsCommandList* cmdList) {
        ID3D12Resource* backBuffer = g_dx12.renderTargets[g_dx12.frameIndex].Get();

        // Back buffer is already in RENDER_TARGET state from BeginFrame
        // Transition it to COPY_DEST
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = backBuffer;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }

        cmdList->CopyResource(backBuffer, presentTexture.Get());

        // Transition back to RENDER_TARGET for ImGui
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = backBuffer;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }
    }

    void Resize(UINT newWidth, UINT newHeight) {
        if (newWidth == width && newHeight == height) return;
        width = newWidth;
        height = newHeight;

        visBufferRT.Reset();
        outputTexture.Reset();
        presentTexture.Reset();
        motionTexture.Reset();
        visibilityDepthTexture.Reset();
        historyTextures[0].Reset();
        historyTextures[1].Reset();
        exposureState.Reset();
        temporalHistoryValid = false;
        exposureReadable = false;
        visRtvHeap.Reset();
        outputRtvHeap.Reset();

        CreateVisBufferRT();
        CreateOutputTexture();
        UpdateComputeDescriptors();
        UpdatePostDescriptors();
        UpdateExposureDescriptors();
    }

private:
    bool CreateVisBufferRT() {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32G32_UINT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_R32G32_UINT;

        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &clearValue,
            IID_PPV_ARGS(&visBufferRT));
        if (FAILED(hr)) {
            std::cerr << "Failed to create visibility buffer RT" << std::endl;
            return false;
        }

        // Create RTV heap
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = 1;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hr = g_dx12.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&visRtvHeap));
        if (FAILED(hr)) return false;

        // Create RTV
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_R32G32_UINT;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_dx12.device->CreateRenderTargetView(visBufferRT.Get(), &rtvDesc,
            visRtvHeap->GetCPUDescriptorHandleForHeapStart());

        return true;
    }

    bool CreateOutputTexture() {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&outputTexture));
        if (FAILED(hr)) {
            std::cerr << "Failed to create VB output texture" << std::endl;
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC outputHeap = {};
        outputHeap.NumDescriptors = 1;
        outputHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hr = g_dx12.device->CreateDescriptorHeap(
            &outputHeap, IID_PPV_ARGS(&outputRtvHeap));
        if (FAILED(hr)) return false;
        D3D12_RENDER_TARGET_VIEW_DESC outputRtv = {};
        outputRtv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        outputRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_dx12.device->CreateRenderTargetView(outputTexture.Get(),
            &outputRtv, outputRtvHeap->GetCPUDescriptorHandleForHeapStart());

        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
            IID_PPV_ARGS(&presentTexture));
        if (FAILED(hr)) {
            std::cerr << "Failed to create VB present texture" << std::endl;
            return false;
        }

        desc.Format = DXGI_FORMAT_R16G16_FLOAT;
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&motionTexture));
        if (FAILED(hr)) return false;

        D3D12_RESOURCE_DESC depthSnapshotDesc =
            g_dx12.depthStencilBuffer->GetDesc();
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &depthSnapshotDesc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&visibilityDepthTexture));
        if (FAILED(hr)) return false;

        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        for (UINT i = 0; i < 2; ++i) {
            hr = g_dx12.device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&historyTextures[i]));
            if (FAILED(hr)) return false;
        }

        D3D12_RESOURCE_DESC exposureDesc = {};
        exposureDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        exposureDesc.Width = 3 * sizeof(UINT);
        exposureDesc.Height = 1;
        exposureDesc.DepthOrArraySize = 1;
        exposureDesc.MipLevels = 1;
        exposureDesc.SampleDesc.Count = 1;
        exposureDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        exposureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &exposureDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&exposureState));
        if (FAILED(hr)) return false;
        return true;
    }

    bool CreateStructuredBuffers() {
        auto CreateDefaultAndUpload = [](UINT64 size,
                                          ComPtr<ID3D12Resource>& defaultBuf,
                                          ComPtr<ID3D12Resource>& uploadBuf) -> bool {
            D3D12_HEAP_PROPERTIES defaultHeap = {};
            defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_HEAP_PROPERTIES uploadHeap = {};
            uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC bufDesc = {};
            bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufDesc.Width = size;
            bufDesc.Height = 1;
            bufDesc.DepthOrArraySize = 1;
            bufDesc.MipLevels = 1;
            bufDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufDesc.SampleDesc.Count = 1;
            bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            HRESULT hr = g_dx12.device->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&defaultBuf));
            if (FAILED(hr)) return false;

            hr = g_dx12.device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&uploadBuf));
            if (FAILED(hr)) return false;

            return true;
        };

        if (!CreateDefaultAndUpload(VB_MAX_DRAW_CALLS * sizeof(VBDrawCallData),
                                     drawCallBuffer, drawCallUpload))
            return false;

        if (!CreateDefaultAndUpload(VB_MAX_VERTICES * sizeof(VBPackedVertex),
                                     vertexDataBuffer, vertexDataUpload))
            return false;

        if (!CreateDefaultAndUpload(VB_MAX_INDICES * sizeof(UINT),
                                     indexDataBuffer, indexDataUpload))
            return false;

        if (!CreateDefaultAndUpload(VB_CLUSTER_COUNT * sizeof(VBClusterData),
                                     clusterDataBuffer, clusterDataUpload))
            return false;

        D3D12_HEAP_PROPERTIES materialHeap = {};
        materialHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC materialDesc = {};
        materialDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        materialDesc.Width = VB_MAX_MATERIALS * sizeof(VBMaterialData);
        materialDesc.Height = 1;
        materialDesc.DepthOrArraySize = 1;
        materialDesc.MipLevels = 1;
        materialDesc.SampleDesc.Count = 1;
        materialDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &materialHeap, D3D12_HEAP_FLAG_NONE, &materialDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&materialDataBuffer));
        if (FAILED(hr)) return false;
        D3D12_RANGE noRead = { 0, 0 };
        hr = materialDataBuffer->Map(0, &noRead,
            reinterpret_cast<void**>(&mappedMaterials));
        if (FAILED(hr)) return false;

        return true;
    }

    bool CreateColorLUT() {
        constexpr UINT LUTSize = 16;
        D3D12_RESOURCE_DESC texture = {};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        texture.Width = LUTSize;
        texture.Height = LUTSize;
        texture.DepthOrArraySize = LUTSize;
        texture.MipLevels = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES defaultHeap = {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&colorLUT));
        if (FAILED(hr)) return false;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 rowSize = 0;
        UINT64 uploadSize = 0;
        g_dx12.device->GetCopyableFootprints(
            &texture, 0, 1, 0, &footprint, &rows, &rowSize, &uploadSize);
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = uploadSize;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        hr = g_dx12.device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&colorLUTUpload));
        if (FAILED(hr)) return false;

        BYTE* mapped = nullptr;
        D3D12_RANGE noRead = { 0, 0 };
        hr = colorLUTUpload->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
        if (FAILED(hr)) return false;
        mapped += footprint.Offset;
        for (UINT z = 0; z < LUTSize; ++z) {
            for (UINT y = 0; y < LUTSize; ++y) {
                BYTE* row = mapped + (SIZE_T)z * footprint.Footprint.RowPitch * LUTSize
                    + (SIZE_T)y * footprint.Footprint.RowPitch;
                for (UINT x = 0; x < LUTSize; ++x) {
                    float r = x / float(LUTSize - 1);
                    float g = y / float(LUTSize - 1);
                    float b = z / float(LUTSize - 1);
                    row[x * 4 + 0] = (BYTE)roundf(r * 255.0f);
                    row[x * 4 + 1] = (BYTE)roundf(g * 255.0f);
                    row[x * 4 + 2] = (BYTE)roundf(b * 255.0f);
                    row[x * 4 + 3] = 255;
                }
            }
        }
        colorLUTUpload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = colorLUTUpload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = colorLUT.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        g_dx12.commandList->CopyTextureRegion(
            &destination, 0, 0, 0, &source, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = colorLUT.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_dx12.commandList->ResourceBarrier(1, &barrier);
        return true;
    }

    bool CreateComputeDescriptorHeap() {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 85;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hr = g_dx12.device->CreateDescriptorHeap(
            &heapDesc, IID_PPV_ARGS(&computeDescHeap));
        if (FAILED(hr)) {
            std::cerr << "Failed to create visibility compute descriptor heap\n";
            return false;
        }
        UpdateComputeDescriptors();
        return true;
    }

    bool CreateVisPassPipeline() {
        // Read and compile shaders
        std::ifstream vsFile("shaders/visbuf_vs.hlsl");
        std::ifstream psFile("shaders/visbuf_ps.hlsl");
        if (!vsFile.is_open() || !psFile.is_open()) {
            std::cerr << "Failed to open visibility buffer shader files" << std::endl;
            return false;
        }

        std::stringstream vsSS, psSS;
        vsSS << vsFile.rdbuf();
        psSS << psFile.rdbuf();
        std::string vsCode = vsSS.str();
        std::string psCode = psSS.str();

        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> vsBlob, psBlob, alphaPsBlob, errorBlob;

        HRESULT hr = D3DCompile(vsCode.c_str(), vsCode.length(),
            "shaders/visbuf_vs.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_0",
            compileFlags, 0, &vsBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB VS error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        errorBlob.Reset();
        hr = D3DCompile(psCode.c_str(), psCode.length(),
            "shaders/visbuf_ps.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_0",
            compileFlags, 0, &psBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB PS error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        errorBlob.Reset();
        hr = D3DCompile(psCode.c_str(), psCode.length(),
            "shaders/visbuf_ps.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "mainAlpha", "ps_5_0",
            compileFlags, 0, &alphaPsBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB alpha PS error: "
                << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        // Root signature for vis pass:
        // 0: CBV - MatrixBuffer (b0)
        // 1: Root constants - draw/material flags (b1), 4 UINT values
        // 2: Alpha-test base colour (t0)
        // 3: Persistent per-instance data (t1); VS reads model by drawCallID
        D3D12_ROOT_PARAMETER visParams[4] = {};

        visParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        visParams[0].Descriptor.ShaderRegister = 0;
        visParams[0].Descriptor.RegisterSpace = 0;
        visParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        visParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        visParams[1].Constants.ShaderRegister = 1;
        visParams[1].Constants.RegisterSpace = 0;
        visParams[1].Constants.Num32BitValues = 4;
        visParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_DESCRIPTOR_RANGE alphaRange = {};
        alphaRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        alphaRange.NumDescriptors = 1;
        alphaRange.BaseShaderRegister = 0;
        alphaRange.OffsetInDescriptorsFromTableStart = 0;
        visParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        visParams[2].DescriptorTable.NumDescriptorRanges = 1;
        visParams[2].DescriptorTable.pDescriptorRanges = &alphaRange;
        visParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        visParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        visParams[3].Descriptor.ShaderRegister = 1;
        visParams[3].Descriptor.RegisterSpace = 0;
        visParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_STATIC_SAMPLER_DESC alphaSampler = {};
        alphaSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        alphaSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        alphaSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        alphaSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        alphaSampler.MaxLOD = D3D12_FLOAT32_MAX;
        alphaSampler.ShaderRegister = 0;
        alphaSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC visRootSigDesc = {};
        visRootSigDesc.NumParameters = 4;
        visRootSigDesc.pParameters = visParams;
        visRootSigDesc.NumStaticSamplers = 1;
        visRootSigDesc.pStaticSamplers = &alphaSampler;
        visRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sigBlob;
        errorBlob.Reset();
        hr = D3D12SerializeRootSignature(&visRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB root sig error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        hr = g_dx12.device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
            sigBlob->GetBufferSize(), IID_PPV_ARGS(&visPassRootSig));
        if (FAILED(hr)) return false;

        // Input layout - same as main shader
        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.pRootSignature = visPassRootSig.Get();
        psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
        psoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        psoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        psoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;

        psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
        psoDesc.BlendState.IndependentBlendEnable = FALSE;
        psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psoDesc.DepthStencilState.StencilEnable = FALSE;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R32G32_UINT;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count = 1;

        hr = g_dx12.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&visPassPSO));
        if (FAILED(hr)) {
            std::cerr << "Failed to create vis pass PSO, HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        hr = g_dx12.device->CreateGraphicsPipelineState(
            &psoDesc, IID_PPV_ARGS(&visPassDoubleSidedPSO));
        if (FAILED(hr)) return false;

        psoDesc.PS = { alphaPsBlob->GetBufferPointer(), alphaPsBlob->GetBufferSize() };
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        hr = g_dx12.device->CreateGraphicsPipelineState(
            &psoDesc, IID_PPV_ARGS(&visPassAlphaPSO));
        if (FAILED(hr)) return false;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        hr = g_dx12.device->CreateGraphicsPipelineState(
            &psoDesc, IID_PPV_ARGS(&visPassAlphaDoubleSidedPSO));
        if (FAILED(hr)) return false;

        std::cout << "Visibility pass pipeline created" << std::endl;
        return true;
    }

    bool CreateResolvePipeline() {
        // Read and compile compute shader
        std::ifstream csFile("shaders/visbuf_resolve_cs.hlsl");
        if (!csFile.is_open()) {
            std::cerr << "Failed to open visbuf_resolve_cs.hlsl" << std::endl;
            return false;
        }

        std::stringstream csSS;
        csSS << csFile.rdbuf();
        std::string csCode = csSS.str();

        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> csBlob, errorBlob;
        HRESULT hr = D3DCompile(csCode.c_str(), csCode.length(),
            "shaders/visbuf_resolve_cs.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_1",
            compileFlags, 0, &csBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) {
                const char* message = static_cast<const char*>(errorBlob->GetBufferPointer());
                std::cerr << "VB CS error: " << message << std::endl;
                std::ofstream log("visibility_buffer_shader_error.log", std::ios::trunc);
                log.write(message, static_cast<std::streamsize>(errorBlob->GetBufferSize()));
            }
            return false;
        }

        // Root signature for compute resolve:
        // 0: CBV (b0) - FrameConstants
        // 1: Descriptor table - SRVs (t0..t5) + UAV (u0) + CBVs (b1, b2)
        //
        // We'll put everything in a single descriptor table for simplicity.
        // Layout in the heap:
        //   [0] t0 - visBuffer SRV
        //   [1] t1 - depthBuffer SRV
        //   [2] t2 - (unused/shadow placeholder)
        //   [3] t3 - drawCalls SRV
        //   [4] t4 - vertices SRV
        //   [5] t5 - indices SRV
        //   [6] t6 - clustered light lists
        //   [7] u0 - output UAV
        //   [8] b1 - light buffer CBV
        //   [9] b2 - point lights CBV

        D3D12_DESCRIPTOR_RANGE ranges[3] = {};
        // SRVs t0..t78: frame/geometry, materials, IBL, DDGI, sparse lookup.
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 79;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;

        // UAVs u0..u1 (HDR + motion vectors)
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 79;

        // CBVs b1..b4 (lights, point lights, sky SH, DDGI)
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        ranges[2].NumDescriptors = 4;
        ranges[2].BaseShaderRegister = 1;
        ranges[2].RegisterSpace = 0;
        ranges[2].OffsetInDescriptorsFromTableStart = 81;

        D3D12_ROOT_PARAMETER resolveParams[2] = {};

        // b0 - frame constants (root CBV)
        resolveParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        resolveParams[0].Descriptor.ShaderRegister = 0;
        resolveParams[0].Descriptor.RegisterSpace = 0;
        resolveParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Descriptor table
        resolveParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        resolveParams[1].DescriptorTable.NumDescriptorRanges = 3;
        resolveParams[1].DescriptorTable.pDescriptorRanges = ranges;
        resolveParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Static samplers
        D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};

        // Regular sampler s0
        staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSamplers[0].MipLODBias = 0.0f;
        staticSamplers[0].MinLOD = 0.0f;
        staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[0].ShaderRegister = 0;
        staticSamplers[0].RegisterSpace = 0;
        staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Shadow comparison sampler s1
        staticSamplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        staticSamplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        staticSamplers[1].ShaderRegister = 1;
        staticSamplers[1].RegisterSpace = 0;
        staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC resolveRootSigDesc = {};
        resolveRootSigDesc.NumParameters = 2;
        resolveRootSigDesc.pParameters = resolveParams;
        resolveRootSigDesc.NumStaticSamplers = 2;
        resolveRootSigDesc.pStaticSamplers = staticSamplers;

        ComPtr<ID3DBlob> sigBlob;
        errorBlob.Reset();
        hr = D3D12SerializeRootSignature(&resolveRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB resolve root sig error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        hr = g_dx12.device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
            sigBlob->GetBufferSize(), IID_PPV_ARGS(&resolveRootSig));
        if (FAILED(hr)) return false;

        // Compute PSO
        D3D12_COMPUTE_PIPELINE_STATE_DESC cpsoDesc = {};
        cpsoDesc.pRootSignature = resolveRootSig.Get();
        cpsoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };

        hr = g_dx12.device->CreateComputePipelineState(&cpsoDesc, IID_PPV_ARGS(&resolvePSO));
        if (FAILED(hr)) {
            std::cerr << "Failed to create VB resolve compute PSO" << std::endl;
            return false;
        }

        // Create indirect dispatch command signature + args buffer
        {
            D3D12_INDIRECT_ARGUMENT_DESC arg = {};
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

            D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
            sigDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
            sigDesc.NumArgumentDescs = 1;
            sigDesc.pArgumentDescs = &arg;

            hr = g_dx12.device->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&resolveDispatchSignature));
            if (FAILED(hr)) return false;

            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC bufDesc = {};
            bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufDesc.Width = sizeof(D3D12_DISPATCH_ARGUMENTS);
            bufDesc.Height = 1;
            bufDesc.DepthOrArraySize = 1;
            bufDesc.MipLevels = 1;
            bufDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufDesc.SampleDesc.Count = 1;
            bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            hr = g_dx12.device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&resolveDispatchArgsBuffer));
            if (FAILED(hr)) return false;

            D3D12_RANGE rr = { 0, 0 };
            hr = resolveDispatchArgsBuffer->Map(0, &rr, reinterpret_cast<void**>(&mappedResolveDispatchArgs));
            if (FAILED(hr)) return false;
        }

        std::cout << "Visibility buffer resolve pipeline created" << std::endl;
        return true;
    }

    void UpdateComputeDescriptors() {
        UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = computeDescHeap->GetCPUDescriptorHandleForHeapStart();

        // [0] t0 - visBuffer SRV (R32G32_UINT)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R32G32_UINT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(visBufferRT.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [1] t1 - depthBuffer SRV (R32_FLOAT from D32 typeless)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(g_dx12.depthStencilBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [2] t2 - shadow map placeholder (null SRV)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2DArray.MipLevels = 1;
            srvDesc.Texture2DArray.ArraySize = SHADOW_CASCADE_COUNT;
            g_dx12.device->CreateShaderResourceView(nullptr, &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [3] t3 - drawCalls SRV (structured buffer)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = VB_MAX_DRAW_CALLS;
            srvDesc.Buffer.StructureByteStride = sizeof(VBDrawCallData);
            g_dx12.device->CreateShaderResourceView(drawCallBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [4] t4 - vertices SRV (structured buffer)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = VB_MAX_VERTICES;
            srvDesc.Buffer.StructureByteStride = sizeof(VBPackedVertex);
            g_dx12.device->CreateShaderResourceView(vertexDataBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [5] t5 - indices SRV (structured buffer)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = VB_MAX_INDICES;
            srvDesc.Buffer.StructureByteStride = sizeof(UINT);
            g_dx12.device->CreateShaderResourceView(indexDataBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [6] t6 - clustered light lists
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = VB_CLUSTER_COUNT;
            srvDesc.Buffer.StructureByteStride = sizeof(VBClusterData);
            g_dx12.device->CreateShaderResourceView(clusterDataBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [7] t7 - persistent material table
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.NumElements = VB_MAX_MATERIALS;
            srvDesc.Buffer.StructureByteStride = sizeof(VBMaterialData);
            g_dx12.device->CreateShaderResourceView(
                materialDataBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [8..71] t8..t71 - material texture array, initialized to null.
        for (UINT i = 0; i < VB_MAX_MATERIAL_TEXTURES; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
            nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(nullptr, &nullSrv, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [72] t72 - HDR environment map for specular IBL.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC environment = {};
            environment.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            environment.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            environment.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            environment.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(nullptr, &environment, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [73] t73 - split-sum GGX BRDF integration LUT.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC brdf = {};
            brdf.Format = DXGI_FORMAT_R32G32_FLOAT;
            brdf.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            brdf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            brdf.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(nullptr, &brdf, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [74] t74 - DDGI irradiance atlas.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(nullptr, &srv, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [75] t75 - DDGI visibility atlas.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R16G16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(nullptr, &srv, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [76..78] t76..t78 - sparse probes, hash cells, flattened indices.
        {
            const UINT strides[3] = {
                sizeof(DXRProbeRecord), sizeof(DXRProbeGridCell), sizeof(UINT)
            };
            for (UINT i = 0; i < 3; ++i) {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                srv.Format = DXGI_FORMAT_UNKNOWN;
                srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srv.Shader4ComponentMapping =
                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Buffer.NumElements = 1;
                srv.Buffer.StructureByteStride = strides[i];
                g_dx12.device->CreateShaderResourceView(nullptr, &srv, cpuHandle);
                cpuHandle.ptr += descSize;
            }
        }

        // [79] u0 - linear HDR output UAV
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_dx12.device->CreateUnorderedAccessView(outputTexture.Get(), nullptr, &uavDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [80] u1 - screen-space motion vectors
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_dx12.device->CreateUnorderedAccessView(
                motionTexture.Get(), nullptr, &uavDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [81] b1 - light buffer CBV
        // [82] b2 - point lights CBV
        // [83] b3 - sky SH CBV
        // [84] b4 - DDGI CBV
        // These will be created in UpdateLightDescriptors
    }

    bool CreateExposurePipeline() {
        std::ifstream file("shaders/visbuf_exposure_cs.hlsl");
        if (!file.is_open()) return false;
        std::stringstream stream;
        stream << file.rdbuf();
        const std::string source = stream.str();

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root = {};
        root.NumParameters = 2;
        root.pParameters = params;
        ComPtr<ID3DBlob> rootBlob, errors;
        HRESULT hr = D3D12SerializeRootSignature(&root,
            D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errors);
        if (FAILED(hr)) return false;
        hr = g_dx12.device->CreateRootSignature(0, rootBlob->GetBufferPointer(),
            rootBlob->GetBufferSize(), IID_PPV_ARGS(&exposureRootSig));
        if (FAILED(hr)) return false;

        auto createPSO = [&](const char* entry,
                             ComPtr<ID3D12PipelineState>& result) -> bool {
            ComPtr<ID3DBlob> shader;
            errors.Reset();
            HRESULT compile = D3DCompile(source.data(), source.size(),
                "visbuf_exposure_cs.hlsl", nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, "cs_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &shader, &errors);
            if (FAILED(compile)) {
                if (errors) std::cerr << "VB exposure CS error: "
                    << (char*)errors->GetBufferPointer() << std::endl;
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
            pso.pRootSignature = exposureRootSig.Get();
            pso.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
            return SUCCEEDED(g_dx12.device->CreateComputePipelineState(
                &pso, IID_PPV_ARGS(&result)));
        };
        if (!createPSO("Reset", exposureResetPSO) ||
            !createPSO("Accumulate", exposureAccumulatePSO) ||
            !createPSO("Finalize", exposureFinalizePSO)) return false;

        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.NumDescriptors = 2;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = g_dx12.device->CreateDescriptorHeap(&heap,
            IID_PPV_ARGS(&exposureDescHeap));
        if (FAILED(hr)) return false;
        UpdateExposureDescriptors();
        return true;
    }

    void UpdateExposureDescriptors() {
        if (!exposureDescHeap) return;
        UINT size = g_dx12.cbvSrvUavDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            exposureDescHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC hdr = {};
        hdr.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hdr.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        hdr.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        hdr.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(outputTexture.Get(), &hdr, handle);
        handle.ptr += size;
        D3D12_UNORDERED_ACCESS_VIEW_DESC state = {};
        state.Format = DXGI_FORMAT_R32_TYPELESS;
        state.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        state.Buffer.NumElements = 3;
        state.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        g_dx12.device->CreateUnorderedAccessView(
            exposureState.Get(), nullptr, &state, handle);
    }

    bool CreatePostPipeline() {
        std::ifstream csFile("shaders/visbuf_post_cs.hlsl");
        if (!csFile.is_open()) return false;
        std::stringstream stream;
        stream << csFile.rdbuf();
        const std::string source = stream.str();

        ComPtr<ID3DBlob> shaderBlob, errorBlob;
        HRESULT hr = D3DCompile(source.data(), source.size(),
            "shaders/visbuf_post_cs.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &shaderBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB post CS error: "
                << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 7;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 7;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;
        D3D12_STATIC_SAMPLER_DESC lutSampler = {};
        lutSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        lutSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lutSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lutSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lutSampler.ShaderRegister = 0;
        lutSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        lutSampler.MinLOD = 0.0f;
        lutSampler.MaxLOD = D3D12_FLOAT32_MAX;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers = &lutSampler;
        ComPtr<ID3DBlob> rootBlob;
        hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            &rootBlob, &errorBlob);
        if (FAILED(hr)) return false;
        hr = g_dx12.device->CreateRootSignature(0, rootBlob->GetBufferPointer(),
            rootBlob->GetBufferSize(), IID_PPV_ARGS(&postRootSig));
        if (FAILED(hr)) return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = postRootSig.Get();
        pso.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };
        hr = g_dx12.device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&postPSO));
        if (FAILED(hr)) return false;

        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.NumDescriptors = 18;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = g_dx12.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&postDescHeap));
        if (FAILED(hr)) return false;
        UpdatePostDescriptors();
        return true;
    }

    void UpdatePostDescriptors() {
        if (!postDescHeap) return;
        UINT descriptorSize = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (UINT parity = 0; parity < 2; ++parity) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                postDescHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += (SIZE_T)descriptorSize * parity * 9;

            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(outputTexture.Get(), &srv, handle);
            handle.ptr += descriptorSize;
            srv.Format = DXGI_FORMAT_R16G16_FLOAT;
            g_dx12.device->CreateShaderResourceView(motionTexture.Get(), &srv, handle);
            handle.ptr += descriptorSize;
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            g_dx12.device->CreateShaderResourceView(
                historyTextures[parity ^ 1u].Get(), &srv, handle);
            handle.ptr += descriptorSize;
            D3D12_SHADER_RESOURCE_VIEW_DESC exposureSrv = {};
            exposureSrv.Format = DXGI_FORMAT_R32_TYPELESS;
            exposureSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            exposureSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            exposureSrv.Buffer.NumElements = 3;
            exposureSrv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            g_dx12.device->CreateShaderResourceView(
                exposureState.Get(), &exposureSrv, handle);
            handle.ptr += descriptorSize;
            D3D12_SHADER_RESOURCE_VIEW_DESC lutSrv = {};
            lutSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            lutSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            lutSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            lutSrv.Texture3D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(colorLUT.Get(), &lutSrv, handle);
            handle.ptr += descriptorSize;
            D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = {};
            depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
            depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            depthSrv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(
                g_dx12.depthStencilBuffer.Get(), &depthSrv, handle);
            handle.ptr += descriptorSize;
            g_dx12.device->CreateShaderResourceView(
                visibilityDepthTexture.Get(), &depthSrv, handle);
            handle.ptr += descriptorSize;

            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_dx12.device->CreateUnorderedAccessView(
                presentTexture.Get(), nullptr, &uav, handle);
            handle.ptr += descriptorSize;
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            g_dx12.device->CreateUnorderedAccessView(
                historyTextures[parity].Get(), nullptr, &uav, handle);
        }
    }

public:
    // Call this each frame before resolve to update the light CBV descriptors
    void UpdateLightDescriptors(D3D12_GPU_VIRTUAL_ADDRESS lightBufferAddr,
                                D3D12_GPU_VIRTUAL_ADDRESS pointLightsAddr,
                                D3D12_GPU_VIRTUAL_ADDRESS shBufferAddr,
                                D3D12_GPU_VIRTUAL_ADDRESS ddgiBufferAddr) {
        UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += 81 * descSize;

        // [7] b1 - light buffer CBV
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = lightBufferAddr;
            cbvDesc.SizeInBytes = sizeof(LightBufferDX12);
            g_dx12.device->CreateConstantBufferView(&cbvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [8] b2 - point lights CBV
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = pointLightsAddr;
            cbvDesc.SizeInBytes = sizeof(PointLightsBufferDX12);
            g_dx12.device->CreateConstantBufferView(&cbvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [11] b3 - preconvolved HDRI spherical harmonics
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = shBufferAddr;
            cbvDesc.SizeInBytes = sizeof(SHBufferDX12);
            g_dx12.device->CreateConstantBufferView(&cbvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [81] b4 - DDGI grid parameters
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = ddgiBufferAddr;
            cbvDesc.SizeInBytes = sizeof(DDGIBufferDX12);
            g_dx12.device->CreateConstantBufferView(&cbvDesc, cpuHandle);
        }
    }

    void UpdateDDGIResources(ID3D12Resource* irradianceResource,
                             ID3D12Resource* visibilityResource) {
        if (!computeDescHeap) return;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(g_dx12.cbvSrvUavDescriptorSize) * 74u;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Format = irradianceResource
            ? irradianceResource->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
        g_dx12.device->CreateShaderResourceView(irradianceResource, &srv, handle);
        handle.ptr += g_dx12.cbvSrvUavDescriptorSize;
        srv.Format = visibilityResource
            ? visibilityResource->GetDesc().Format : DXGI_FORMAT_R16G16_FLOAT;
        g_dx12.device->CreateShaderResourceView(visibilityResource, &srv, handle);
    }

    void UpdateSparseDDGIResources(ID3D12Resource* probes, UINT probeCount,
                                   ID3D12Resource* cells, UINT cellCount,
                                   ID3D12Resource* indices, UINT indexCount) {
        if (!computeDescHeap) return;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(
            g_dx12.cbvSrvUavDescriptorSize) * 76u;
        ID3D12Resource* resources[3] = { probes, cells, indices };
        const UINT counts[3] = {
            (std::max)(probeCount, 1u), (std::max)(cellCount, 1u),
            (std::max)(indexCount, 1u)
        };
        const UINT strides[3] = {
            sizeof(DXRProbeRecord), sizeof(DXRProbeGridCell), sizeof(UINT)
        };
        for (UINT i = 0; i < 3; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.NumElements = counts[i];
            srv.Buffer.StructureByteStride = strides[i];
            g_dx12.device->CreateShaderResourceView(resources[i], &srv, handle);
            handle.ptr += g_dx12.cbvSrvUavDescriptorSize;
        }
    }

    void UpdateEnvironmentMap(ID3D12Resource* environmentResource,
                              ID3D12Resource* brdfResource) {
        if (!computeDescHeap) return;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(g_dx12.cbvSrvUavDescriptorSize) * 72u;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = environmentResource
            ? environmentResource->GetDesc().Format : DXGI_FORMAT_R32G32B32A32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = environmentResource
            ? environmentResource->GetDesc().MipLevels : 1;
        g_dx12.device->CreateShaderResourceView(environmentResource, &srv, handle);
        handle.ptr += g_dx12.cbvSrvUavDescriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC brdf = {};
        brdf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        brdf.Format = DXGI_FORMAT_R32G32_FLOAT;
        brdf.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        brdf.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(brdfResource, &brdf, handle);
    }

    // Update the shadow map SRV in the compute descriptor heap
    void UpdateShadowMapDescriptor(ID3D12Resource* shadowMapResource) {
        if (!computeDescHeap) return;
        
        UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += 2 * descSize; // slot [2] = t2

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.ArraySize = SHADOW_CASCADE_COUNT;

        if (shadowMapResource) {
            g_dx12.device->CreateShaderResourceView(shadowMapResource, &srvDesc, cpuHandle);
        } else {
            g_dx12.device->CreateShaderResourceView(nullptr, &srvDesc, cpuHandle);
        }
    }
};

#endif // VISIBILITY_BUFFER_DX12_H
