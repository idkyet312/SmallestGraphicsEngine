#ifndef IDTECH_RENDERER_H
#define IDTECH_RENDERER_H

// id Tech 8–style Visibility Buffer + Deferred hybrid.
//
// Pass 1  – Visibility rasterisation (ultra-thin VS/PS, writes drawID|triID to R32_UINT)
// Pass 2  – G-Buffer fill            (compute, reads VB + structured geometry ? writes compact G-Buffer)
// Pass 3  – Deferred lighting         (compute, reads G-Buffer + clustered lights ? writes final colour)
//
// The existing VisibilityBufferDX12 class handles Pass 1 + a single combined resolve.
// Here we orchestrate the full frame using that class and populate the light upload
// buffers correctly so the compute pass reads them via descriptor table.

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include "ProfilerDX12.h"
#include "ShaderDX12.h"
#include "VisibilityBufferDX12.h"
#include "OcclusionDepthDX12.h"
#include "Scene.h"
#include "ForwardRenderer.h"
#include <array>
#include <algorithm>
#include <cmath>

// Defined in main.cpp, which includes this header before that definition.
extern ProfilerDX12 g_profiler;

// CPU-side flat vertex float arrays (pos3 norm3 uv2 = 8 floats per vert)
struct PackedGeometry {
    std::vector<float> cubeFloats;
    std::vector<float> planeFloats;
};

inline void BuildPackedGeometry(const std::vector<VertexPosNormUV>& cubeVerts,
                                const std::vector<VertexPosNormUV>& planeVerts,
                                PackedGeometry& out) {
    auto pack = [](const std::vector<VertexPosNormUV>& src, std::vector<float>& dst) {
        dst.resize(src.size() * 8);
        for (size_t i = 0; i < src.size(); i++) {
            dst[i * 8 + 0] = src[i].position.x; dst[i * 8 + 1] = src[i].position.y; dst[i * 8 + 2] = src[i].position.z;
            dst[i * 8 + 3] = src[i].normal.x;   dst[i * 8 + 4] = src[i].normal.y;   dst[i * 8 + 5] = src[i].normal.z;
            dst[i * 8 + 6] = src[i].texCoord.x; dst[i * 8 + 7] = src[i].texCoord.y;
        }
    };
    pack(cubeVerts, out.cubeFloats);
    pack(planeVerts, out.planeFloats);
}

enum IdTechMaterialId : UINT {
    MAT_FLOOR = 0,
    MAT_CUBE = 1,
    MAT_PROJECTILE = 2,
    MAT_GUN = 3,
    MAT_LIGHT_SPHERE = 4
};

struct IdTechDrawItem {
    XMMATRIX model;
    XMFLOAT3 color;
    bool isCube;
    UINT materialId;
    bool backfaceCullable;
    std::shared_ptr<SceneMaterial> material;
    MeshPrimitive* primitive = nullptr;
    UINT visibilityMeshID = VB_INVALID_MESH;
    uint64_t instanceKey = 0;
    bool doubleSided = false;
    bool alphaCutout = false;
    bool alphaFromLuminance = false;
    XMFLOAT4 palmWindRoot{};
    // Destruction chunk geometry. Tracked so the pass can tell whether those
    // chunks actually registered visibility IDs this frame, which is what lets
    // the forward pass skip redrawing them.
    bool destructionChunk = false;
};

struct FrustumPlanes {
    std::array<XMFLOAT4, 6> p;
};

inline XMFLOAT4 NormalizePlane(const XMFLOAT4& in) {
    XMVECTOR v = XMLoadFloat4(&in);
    XMVECTOR n = XMVector3Normalize(v);
    XMFLOAT4 out;
    XMStoreFloat4(&out, n);
    float len = sqrtf(in.x * in.x + in.y * in.y + in.z * in.z);
    if (len > 1e-6f) out.w = in.w / len;
    return out;
}

inline FrustumPlanes ExtractFrustumPlanes(const XMMATRIX& vp) {
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, vp);
    FrustumPlanes f;
    f.p[0] = NormalizePlane(XMFLOAT4(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41)); // left
    f.p[1] = NormalizePlane(XMFLOAT4(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41)); // right
    f.p[2] = NormalizePlane(XMFLOAT4(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42)); // top
    f.p[3] = NormalizePlane(XMFLOAT4(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42)); // bottom
    f.p[4] = NormalizePlane(XMFLOAT4(m._13, m._23, m._33, m._43));                                   // near
    f.p[5] = NormalizePlane(XMFLOAT4(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43)); // far
    return f;
}

inline bool SphereInsideFrustum(const FrustumPlanes& f, const XMFLOAT3& c, float r) {
    for (const auto& p : f.p) {
        float d = p.x * c.x + p.y * c.y + p.z * c.z + p.w;
        if (d < -r) return false;
    }
    return true;
}

inline float ApproximateProjectedPixelArea(float radius, const XMFLOAT3& center,
                                           const XMFLOAT3& camPos, float fovDeg, float screenH) {
    XMFLOAT3 d = XMFLOAT3(center.x - camPos.x, center.y - camPos.y, center.z - camPos.z);
    float dist = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    dist = (dist < 0.001f) ? 0.001f : dist;
    float tanHalf = tanf(XMConvertToRadians(fovDeg) * 0.5f);
    float pxRadius = (radius / (dist * tanHalf)) * (screenH * 0.5f);
    return 3.14159265f * pxRadius * pxRadius;
}

inline XMFLOAT3 TransformPoint(const XMMATRIX& m, const XMFLOAT3& p) {
    XMVECTOR v = XMVectorSet(p.x, p.y, p.z, 1.0f);
    XMVECTOR t = XMVector4Transform(v, m);
    XMFLOAT3 out;
    XMStoreFloat3(&out, t);
    return out;
}

inline XMFLOAT3 TransformDir(const XMMATRIX& m, const XMFLOAT3& d) {
    XMVECTOR v = XMVectorSet(d.x, d.y, d.z, 0.0f);
    XMVECTOR t = XMVector3Normalize(XMVector3TransformNormal(v, m));
    XMFLOAT3 out;
    XMStoreFloat3(&out, t);
    return out;
}

inline XMFLOAT4 ComputeDrawItemBounds(const IdTechDrawItem& item) {
    if (item.primitive && item.primitive->boundsValid) {
        const XMFLOAT3 localCenter(
            (item.primitive->boundsMin.x + item.primitive->boundsMax.x) * 0.5f,
            (item.primitive->boundsMin.y + item.primitive->boundsMax.y) * 0.5f,
            (item.primitive->boundsMin.z + item.primitive->boundsMax.z) * 0.5f);
        const XMFLOAT3 worldCenter = TransformPoint(item.model, localCenter);
        const XMFLOAT3 extent(
            (item.primitive->boundsMax.x - item.primitive->boundsMin.x) * 0.5f,
            (item.primitive->boundsMax.y - item.primitive->boundsMin.y) * 0.5f,
            (item.primitive->boundsMax.z - item.primitive->boundsMin.z) * 0.5f);
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, item.model);
        const float sx = XMVectorGetX(XMVector3Length(XMVectorSet(m._11, m._12, m._13, 0)));
        const float sy = XMVectorGetX(XMVector3Length(XMVectorSet(m._21, m._22, m._23, 0)));
        const float sz = XMVectorGetX(XMVector3Length(XMVectorSet(m._31, m._32, m._33, 0)));
        float radius = sqrtf(extent.x * extent.x + extent.y * extent.y +
            extent.z * extent.z) * (std::max)(sx, (std::max)(sy, sz));
        if (item.palmWindRoot.z > 0.5f) radius += 3.0f *
            (std::max)(sx, (std::max)(sy, sz));
        return XMFLOAT4(worldCenter.x, worldCenter.y, worldCenter.z, radius);
    }
    XMFLOAT3 center = TransformPoint(item.model, XMFLOAT3(0, 0, 0));
    float radius = 28.5f;
    if (item.isCube) {
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, item.model);
        float sx = sqrtf(m._11 * m._11 + m._12 * m._12 + m._13 * m._13);
        float sy = sqrtf(m._21 * m._21 + m._22 * m._22 + m._23 * m._23);
        float sz = sqrtf(m._31 * m._31 + m._32 * m._32 + m._33 * m._33);
        radius = 0.8660254f * (std::max)(sx, (std::max)(sy, sz));
    }
    return XMFLOAT4(center.x, center.y, center.z, radius);
}

inline void AppendOpaquePrimitiveDrawItem(MeshPrimitive& primitive,
    const XMMATRIX& model, std::vector<IdTechDrawItem>& items,
    XMFLOAT4 palmWindRoot = {}) {
    const std::shared_ptr<SceneMaterial>& material = primitive.material;
    const bool transparent = material && material->baseColorFactor.w < 0.999f;
    // Alpha-tested cards need their material texture bound per draw. Keep them
    // in forward extensions with transparent/skinned geometry instead of
    // claiming them for the opaque visibility batch.
    const bool alphaCutout = material && material->alphaCutout;
    if (transparent || alphaCutout || primitive.skinBuffer || primitive.vertices.empty() ||
        primitive.vbv.BufferLocation == 0) return;
    // Material baseColorFactor is already uploaded in VBMaterialData.
    // Keep instance tint white or imported materials get multiplied twice.
    IdTechDrawItem item = {};
    item.model = model;
    item.color = XMFLOAT3(1, 1, 1);
    item.materialId = material
        ? static_cast<UINT>(primitive.materialIndex + 2) : 0u;
    item.material = material;
    item.primitive = &primitive;
    item.instanceKey = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(&primitive));
    // Forward imported-mesh PSOs intentionally disable culling because
    // FBX/glTF assets in this project mix winding conventions. Match
    // that ownership contract in VB; otherwise Humvee/bin shells show
    // their opposite faces and look spatially flipped.
    item.doubleSided = true;
    item.alphaCutout = material && material->alphaCutout &&
        material->baseColorTexture != nullptr;
    item.alphaFromLuminance = material && material->alphaFromLuminance;
    item.palmWindRoot = palmWindRoot;
    if (palmWindRoot.z > 0.5f) {
        const int rootX = static_cast<int>(palmWindRoot.x * 100.0f);
        const int rootZ = static_cast<int>(palmWindRoot.y * 100.0f);
        item.instanceKey ^= static_cast<uint64_t>(static_cast<uint32_t>(rootX));
        item.instanceKey ^= static_cast<uint64_t>(static_cast<uint32_t>(rootZ)) << 32;
    }
    items.push_back(item);
}

inline void AppendOpaqueMeshDrawItems(const std::shared_ptr<SceneMesh>& mesh,
    const XMMATRIX& model, std::vector<IdTechDrawItem>& items,
    XMFLOAT4 palmWindRoot = {}) {
    if (!mesh) return;
    for (MeshPrimitive& primitive : mesh->primitives)
        AppendOpaquePrimitiveDrawItem(primitive, model, items, palmWindRoot);
}

struct FlattenedOpaquePacket {
    MeshPrimitive* primitive = nullptr;
    SceneNode* owner = nullptr;
};

struct FlattenedOpaquePacketCacheEntry {
    std::weak_ptr<SceneNode> root;
    std::vector<FlattenedOpaquePacket> packets;
};

inline std::unordered_map<const SceneNode*, FlattenedOpaquePacketCacheEntry>
    g_flattenedOpaquePacketCache;

inline void CollectFlattenedOpaquePackets(const std::shared_ptr<SceneNode>& node,
    std::vector<FlattenedOpaquePacket>& packets) {
    if (!node) return;
    if (node->mesh) for (MeshPrimitive& primitive : node->mesh->primitives)
        packets.push_back({ &primitive, node.get() });
    for (const std::shared_ptr<SceneNode>& child : node->children)
        CollectFlattenedOpaquePackets(child, packets);
}

inline const std::vector<FlattenedOpaquePacket>& GetFlattenedOpaquePackets(
    const std::shared_ptr<SceneNode>& node) {
    static const std::vector<FlattenedOpaquePacket> empty;
    if (!node) return empty;
    auto found = g_flattenedOpaquePacketCache.find(node.get());
    if (found != g_flattenedOpaquePacketCache.end()) {
        std::shared_ptr<SceneNode> cached = found->second.root.lock();
        if (cached.get() == node.get()) return found->second.packets;
        g_flattenedOpaquePacketCache.erase(found);
    }
    FlattenedOpaquePacketCacheEntry entry;
    entry.root = node;
    CollectFlattenedOpaquePackets(node, entry.packets);
    auto inserted = g_flattenedOpaquePacketCache.emplace(node.get(),
        std::move(entry));
    return inserted.first->second.packets;
}

inline void AppendOpaqueSceneNodeDrawItems(
    const std::shared_ptr<SceneNode>& node, const XMMATRIX& worldTransform,
    std::vector<IdTechDrawItem>& items) {
    if (!node) return;
    const std::vector<FlattenedOpaquePacket>& packets =
        GetFlattenedOpaquePackets(node);
    for (const FlattenedOpaquePacket& packet : packets) {
        if (!packet.primitive || !packet.owner) continue;
        // Node hierarchy is cached; animated transforms are not. Rotor nodes
        // update globalTransform every frame.
        const XMMATRIX model = XMLoadFloat4x4(&packet.owner->globalTransform) *
            worldTransform;
        AppendOpaquePrimitiveDrawItem(*packet.primitive, model, items);
    }
}

inline void BuildSceneDrawItems(Scene& scene, std::vector<IdTechDrawItem>& items,
                                const std::shared_ptr<SceneMaterial>& floorMaterial,
                                const std::shared_ptr<SceneNode>& importedScene) {
    items.clear();

    if (!scene.useMeshTerrain) {
        items.push_back({ XMMatrixIdentity(), scene.floor.color, false,
            MAT_FLOOR, true, floorMaterial });
    }

    if (scene.cube2.visible) {
        items.push_back({ scene.cube2.GetModelMatrix(), scene.cube2.color, true, MAT_CUBE, false });
    }

    if (!g_emptyLevelMode && importedScene && g_showH2Model)
        AppendOpaqueSceneNodeDrawItems(importedScene, XMMatrixIdentity(), items);

    if (!g_emptyLevelMode && scene.useDestruction && g_destruction.IsInitialized()) {
        const size_t destructionBegin = items.size();
        for (const DestructionRenderBatch& batch : g_destruction.GetRenderBatches())
            AppendOpaqueSceneNodeDrawItems(batch.colourNode,
                XMLoadFloat4x4(&batch.transform), items);
        for (const DestructionRenderItem& item : g_destruction.GetRenderItems())
            AppendOpaqueSceneNodeDrawItems(item.node,
                XMLoadFloat4x4(&item.transform), items);
        // Tagged in bulk rather than threaded through the append helpers, which
        // are shared with every other geometry source in this function.
        for (size_t i = destructionBegin; i < items.size(); ++i)
            items[i].destructionChunk = true;
    }

    if (!g_emptyLevelMode && g_trees.IsInitialized()) {
        for (const TreeItem& tree : g_trees.GetItems()) {
            std::shared_ptr<SceneMesh> slice = tree.meshOverride;
            if (!slice) {
                if (tree.crown) slice = PalmModel::Crown();
                else if (tree.segment >= 0 &&
                         tree.segment < static_cast<int>(
                             PalmModel::TrunkSlices().size()))
                    slice = PalmModel::TrunkSlices()[tree.segment].mesh;
            }
            if (slice) AppendOpaqueMeshDrawItems(slice,
                XMLoadFloat4x4(&tree.transform), items, tree.palmWindRoot);
            else items.push_back({ XMLoadFloat4x4(&tree.transform), tree.color,
                true, MAT_CUBE, false });
        }
    }

    if (!g_emptyLevelMode && g_water.IsInitialized()) {
        for (const WaterFloaterItem& floater : g_water.GetFloaterItems())
            items.push_back({ XMLoadFloat4x4(&floater.transform), floater.color,
                true, MAT_CUBE, false });
    }

    // Rappel rope links. This path carries no shape enum, only isCube, so the
    // links draw as boxes here rather than capsules -- they are thin enough that
    // the difference does not read at rappel distance.
    for (const RopeItem& link : BlackHawkRopeItems())
        items.push_back({ XMLoadFloat4x4(&link.transform), link.color,
            true, MAT_CUBE, false });
    // Enemy fast-ropes under the reinforcement dropship, same encoding.
    for (const RopeItem& link : DropshipRopeItems())
        items.push_back({ XMLoadFloat4x4(&link.transform), link.color,
            true, MAT_CUBE, false });

    if (!g_emptyLevelMode && g_humveeModel) {
        AppendOpaqueSceneNodeDrawItems(g_humveeModel, HumveeWorldMatrix(), items);
        if (g_stressTestMode)
            AppendOpaqueSceneNodeDrawItems(g_humveeModel,
                SecondaryHumveeWorldMatrix(), items);
    }

    if (!g_emptyLevelMode && g_helicopterModel && scene.showHelicopter) {
        AppendOpaqueSceneNodeDrawItems(g_helicopterModel, HelicopterWorldMatrix(), items);
        if (SecondaryHelicopterVisible())
            AppendOpaqueSceneNodeDrawItems(
                g_secondaryHelicopterModel ? g_secondaryHelicopterModel
                                           : g_helicopterModel,
                SecondaryHelicopterWorldMatrix(), items);
    }

    if (!g_emptyLevelMode && g_explosiveBarrelModel) {
        for (const ExplosiveBarrel& barrel : scene.explosiveBarrels) {
            if (!barrel.active) continue;
            AppendOpaqueSceneNodeDrawItems(g_explosiveBarrelModel,
                XMMatrixTranslation(barrel.position.x,
                    barrel.position.y - 0.75f, barrel.position.z), items);
        }
    }

}

inline void CullAndBatchDrawItems(Scene& scene, const XMMATRIX& view, const XMMATRIX& proj,
                                  std::vector<IdTechDrawItem>& items) {
    items.erase(std::remove_if(items.begin(), items.end(),
        [&](const IdTechDrawItem& item) {
        XMFLOAT4 bounds = ComputeDrawItemBounds(item);
        XMFLOAT3 center(bounds.x, bounds.y, bounds.z);

        // Backface culling (object-level) where safe
        if (item.backfaceCullable) {
            XMFLOAT3 worldNormal = TransformDir(item.model, XMFLOAT3(0, 1, 0));
            XMFLOAT3 toCamera(scene.camera.Position.x - center.x,
                              scene.camera.Position.y - center.y,
                              scene.camera.Position.z - center.z);
            float facing = worldNormal.x * toCamera.x + worldNormal.y * toCamera.y + worldNormal.z * toCamera.z;
            if (facing <= 0.0f) return true;
        }
        return false;
    }), items.end());
    // Preserve cached packet order. Opaque indirect streams are PSO-batched
    // later; sorting thousands of records by material added CPU work without
    // reducing state changes.
}

#pragma pack(push, 1)
struct VisIndexedIndirectCommand {
    D3D12_VERTEX_BUFFER_VIEW vbv;
    D3D12_INDEX_BUFFER_VIEW ibv;
    D3D12_GPU_VIRTUAL_ADDRESS matrixCBV;
    UINT drawCallID;
    D3D12_DRAW_INDEXED_ARGUMENTS drawArgs;
};

struct GPUVisibilityCullInput {
    VisIndexedIndirectCommand command;
    XMFLOAT4 worldBounds;
};
#pragma pack(pop)

static_assert(sizeof(VisIndexedIndirectCommand) == sizeof(D3D12_VERTEX_BUFFER_VIEW) +
    sizeof(D3D12_INDEX_BUFFER_VIEW) + sizeof(D3D12_GPU_VIRTUAL_ADDRESS) +
    sizeof(UINT) + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
    "VisIndexedIndirectCommand must be tightly packed for ExecuteIndirect.");
static_assert(sizeof(GPUVisibilityCullInput) == 80,
    "GPU cull input must match visibility_cull_cs.hlsl.");

struct alignas(256) GPUVisibilityCullConstants {
    XMFLOAT4 frustumPlanes[6];
    XMMATRIX previousViewProjection;
    XMFLOAT3 cameraPosition;
    float projectionScaleY;
    XMUINT2 screenSize;
    UINT commandCount;
    UINT hzbMipCount;
    UINT useOcclusion;
    float lodPixelThreshold;
    XMFLOAT2 padding;
};

struct GPUDrivenVisibilityContext {
    bool initialized = false;
    UINT maxCommands = MAX_DRAW_CALLS_PER_FRAME;
    ComPtr<ID3D12CommandSignature> commandSignature;
    ComPtr<ID3D12Resource> inputBuffer;
    ComPtr<ID3D12Resource> visibleCommandBuffer;
    ComPtr<ID3D12Resource> visibleCountBuffer;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    ComPtr<ID3D12RootSignature> cullRootSignature;
    ComPtr<ID3D12PipelineState> cullPipeline;
    GPUVisibilityCullInput* mappedInputs = nullptr;
    std::vector<uint64_t> inputKeys;
    UploadBuffer<GPUVisibilityCullConstants> constants;

    bool Init(ID3D12RootSignature* visRootSig) {
        if (initialized) return true;

        D3D12_INDIRECT_ARGUMENT_DESC args[5] = {};
        args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
        args[0].VertexBuffer.Slot = 0;
        args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
        args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
        args[2].ConstantBufferView.RootParameterIndex = 0; // matrix CBV
        args[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        args[3].Constant.RootParameterIndex = 1; // drawCallID root constants
        args[3].Constant.DestOffsetIn32BitValues = 0;
        args[3].Constant.Num32BitValuesToSet = 1;
        args[4].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

        D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
        sigDesc.ByteStride = sizeof(VisIndexedIndirectCommand);
        sigDesc.NumArgumentDescs = _countof(args);
        sigDesc.pArgumentDescs = args;

        HRESULT hr = g_dx12.device->CreateCommandSignature(&sigDesc, visRootSig, IID_PPV_ARGS(&commandSignature));
        if (FAILED(hr)) return false;

        if (!CreateBuffers()) return false;
        inputKeys.assign(maxCommands, 0);
        if (!CreateCullPipeline()) return false;
        if (!constants.Create(FRAME_COUNT)) return false;

        initialized = true;
        return true;
    }

    bool CreateBuffers() {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        desc.Width = sizeof(GPUVisibilityCullInput) * maxCommands * FRAME_COUNT;
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&inputBuffer));
        if (FAILED(hr)) return false;
        D3D12_RANGE noRead = { 0, 0 };
        hr = inputBuffer->Map(0, &noRead,
            reinterpret_cast<void**>(&mappedInputs));
        if (FAILED(hr)) return false;

        D3D12_HEAP_PROPERTIES defaultHeap = {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        desc.Width = sizeof(VisIndexedIndirectCommand) * maxCommands;
        hr = g_dx12.device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, nullptr,
            IID_PPV_ARGS(&visibleCommandBuffer));
        if (FAILED(hr)) return false;
        desc.Width = sizeof(UINT);
        hr = g_dx12.device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, nullptr,
            IID_PPV_ARGS(&visibleCountBuffer));
        return SUCCEEDED(hr);
    }

    bool CreateCullPipeline() {
        std::ifstream file("shaders/visibility_cull_cs.hlsl");
        if (!file.is_open()) return false;
        std::stringstream stream;
        stream << file.rdbuf();
        std::string source = stream.str();
        ComPtr<ID3DBlob> shader, errors;
        HRESULT hr = ShaderCacheDX12::CompileCached(source.data(), source.size(),
            "visibility_cull_cs.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS |
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
        if (FAILED(hr)) {
            if (errors) std::cerr << "Visibility cull CS error: "
                << (char*)errors->GetBufferPointer() << std::endl;
            return false;
        }

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 2;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 2;
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
        ComPtr<ID3DBlob> rootBlob;
        hr = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
            &rootBlob, &errors);
        if (FAILED(hr)) return false;
        hr = g_dx12.device->CreateRootSignature(0, rootBlob->GetBufferPointer(),
            rootBlob->GetBufferSize(), IID_PPV_ARGS(&cullRootSignature));
        if (FAILED(hr)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = cullRootSignature.Get();
        pso.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        hr = g_dx12.device->CreateComputePipelineState(&pso,
            IID_PPV_ARGS(&cullPipeline));
        if (FAILED(hr)) return false;

        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.NumDescriptors = 4 * FRAME_COUNT;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = g_dx12.device->CreateDescriptorHeap(&heap,
            IID_PPV_ARGS(&descriptorHeap));
        if (FAILED(hr)) return false;
        for (UINT frame = 0; frame < FRAME_COUNT; ++frame)
            CreateBufferDescriptors(frame);
        return true;
    }

    GPUVisibilityCullInput& Input(UINT index) {
        return mappedInputs[g_dx12.frameIndex * maxCommands + index];
    }

    bool NeedsInput(UINT index, uint64_t key) const {
        if (index >= maxCommands) return false;
        if (key == 0) key = 1;
        return inputKeys[index] != key;
    }

    GPUVisibilityCullInput& PrepareInput(UINT index, uint64_t key,
        const GPUVisibilityCullInput& staticInput) {
        if (index >= maxCommands) return Input(0);
        if (key == 0) key = 1;
        if (inputKeys[index] != key) {
            for (UINT frame = 0; frame < FRAME_COUNT; ++frame)
                mappedInputs[frame * maxCommands + index] = staticInput;
            inputKeys[index] = key;
        }
        return Input(index);
    }

    void CreateBufferDescriptors(UINT frame) {
        UINT size = g_dx12.cbvSrvUavDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (SIZE_T)size * frame * 4;
        D3D12_SHADER_RESOURCE_VIEW_DESC inputSrv = {};
        inputSrv.Format = DXGI_FORMAT_UNKNOWN;
        inputSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        inputSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        inputSrv.Buffer.FirstElement = frame * maxCommands;
        inputSrv.Buffer.NumElements = maxCommands;
        inputSrv.Buffer.StructureByteStride = sizeof(GPUVisibilityCullInput);
        g_dx12.device->CreateShaderResourceView(inputBuffer.Get(), &inputSrv, handle);
        handle.ptr += size * 2;

        D3D12_UNORDERED_ACCESS_VIEW_DESC commandsUav = {};
        commandsUav.Format = DXGI_FORMAT_R32_TYPELESS;
        commandsUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        commandsUav.Buffer.NumElements =
            (sizeof(VisIndexedIndirectCommand) * maxCommands) / sizeof(UINT);
        commandsUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        g_dx12.device->CreateUnorderedAccessView(visibleCommandBuffer.Get(),
            nullptr, &commandsUav, handle);
        handle.ptr += size;
        D3D12_UNORDERED_ACCESS_VIEW_DESC countUav = commandsUav;
        countUav.Buffer.NumElements = 1;
        g_dx12.device->CreateUnorderedAccessView(visibleCountBuffer.Get(),
            nullptr, &countUav, handle);
    }

    void UpdateHZBDescriptor(OcclusionDepthDX12* hzb) {
        UINT size = g_dx12.cbvSrvUavDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (SIZE_T)size * (g_dx12.frameIndex * 4 + 1);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = hzb ? hzb->GetMipCount() : 1;
        g_dx12.device->CreateShaderResourceView(
            hzb ? hzb->previousDepth.Get() : nullptr, &srv, handle);
    }

    void Cull(ID3D12GraphicsCommandList* cmd,
              const GPUVisibilityCullConstants& data,
              OcclusionDepthDX12* hzb) {
        if (!initialized || data.commandCount == 0) return;
        UpdateHZBDescriptor(hzb);
        constants.CopyData(g_dx12.frameIndex, data);

        D3D12_RESOURCE_BARRIER barriers[2] = {};
        for (UINT i = 0; i < 2; ++i) {
            barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        barriers[0].Transition.pResource = visibleCommandBuffer.Get();
        barriers[1].Transition.pResource = visibleCountBuffer.Get();
        cmd->ResourceBarrier(2, barriers);

        UINT descriptorSize = g_dx12.cbvSrvUavDescriptorSize;
        const UINT frameBase = g_dx12.frameIndex * 4;
        D3D12_CPU_DESCRIPTOR_HANDLE countCpu =
            descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        countCpu.ptr += (SIZE_T)descriptorSize * (frameBase + 3);
        D3D12_GPU_DESCRIPTOR_HANDLE countGpu =
            descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        countGpu.ptr += (UINT64)descriptorSize * (frameBase + 3);
        D3D12_GPU_DESCRIPTOR_HANDLE tableGpu =
            descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        tableGpu.ptr += (UINT64)descriptorSize * frameBase;
        ID3D12DescriptorHeap* heaps[] = { descriptorHeap.Get() };
        cmd->SetDescriptorHeaps(1, heaps);
        const UINT zeros[4] = {};
        cmd->ClearUnorderedAccessViewUint(countGpu, countCpu,
            visibleCountBuffer.Get(), zeros, 0, nullptr);

        cmd->SetComputeRootSignature(cullRootSignature.Get());
        cmd->SetPipelineState(cullPipeline.Get());
        cmd->SetComputeRootConstantBufferView(0,
            constants.GetGPUAddress(g_dx12.frameIndex));
        cmd->SetComputeRootDescriptorTable(1, tableGpu);
        cmd->Dispatch((data.commandCount + 63) / 64, 1, 1);

        for (UINT i = 0; i < 2; ++i) {
            barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[i].UAV.pResource = i == 0
                ? visibleCommandBuffer.Get() : visibleCountBuffer.Get();
        }
        cmd->ResourceBarrier(2, barriers);
        for (UINT i = 0; i < 2; ++i) {
            barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i].Transition.pResource = i == 0
                ? visibleCommandBuffer.Get() : visibleCountBuffer.Get();
            barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        cmd->ResourceBarrier(2, barriers);
    }

    void Execute(ID3D12GraphicsCommandList* cmd, UINT submittedCount) {
        if (!initialized || submittedCount == 0) return;
        cmd->ExecuteIndirect(commandSignature.Get(), submittedCount,
            visibleCommandBuffer.Get(), 0, visibleCountBuffer.Get(), 0);
    }
};

inline void FillFrameMatrixBufferForIndirect(ShaderDX12& matrixShader,
                                        const XMMATRIX& view,
                                        const XMMATRIX& proj, const XMMATRIX& lightSpace,
                                        D3D12_GPU_VIRTUAL_ADDRESS& outCBV) {
    UINT bufferIndex = g_dx12.frameIndex * MAX_DRAW_CALLS_PER_FRAME;

    // Zero-initialised: the visibility VS only reads model/view/projection, but
    // terrain's amplification shader shares this CBV and culls against
    // modelViewProjection. Leaving the struct uninitialised fed it garbage
    // frustum planes, and the resulting DispatchMesh count hung the GPU.
    MatrixBufferDX12 data = {};
    data.model = XMMatrixIdentity();
    data.view = XMMatrixTranspose(view);
    data.projection = XMMatrixTranspose(proj);
    data.lightSpaceMatrix = XMMatrixTranspose(lightSpace);
    // model is identity here, so modelView/MVP are just view and view*proj.
    data.modelView = XMMatrixTranspose(view);
    data.modelViewProjection = XMMatrixTranspose(view * proj);
    data.previousViewProjection =
        XMMatrixTranspose(matrixShader.previousViewProjection);
    data.previousModel = XMMatrixIdentity();
    data.palmWind = matrixShader.palmWindFrame.wind;
    data.palmPrimary = matrixShader.palmWindFrame.primary;
    data.palmSecondary = matrixShader.palmWindFrame.secondary;
    data.palmPreviousPrimary = matrixShader.palmWindFrame.previousPrimary;
    data.palmPreviousSecondary = matrixShader.palmWindFrame.previousSecondary;
    data.palmParams = matrixShader.palmWindFrame.params;
    data.palmRoot = {};
    matrixShader.matrixBuffer.CopyData(bufferIndex, data);

    outCBV = matrixShader.matrixBuffer.GetGPUAddress(bufferIndex);
}

// Full id Tech–style frame.
// Pass 1: Visibility rasterisation
// Pass 2+3 (combined in visbuf_resolve_cs): G-Buffer fill + deferred lighting
inline void RenderIdTech(Scene& scene, ShaderDX12& shader,
                         VisibilityBufferDX12& vb,
                         const GeometryBuffers& geo,
                         const PackedGeometry& packed,
                         const XMMATRIX& lightSpace,
                         ID3D12Resource* shadowResource,
                         OcclusionDepthDX12* hzb,
                         bool useHZBOcclusion,
                         const XMMATRIX& previousViewProjection,
                         const std::shared_ptr<SceneMaterial>& floorMaterial,
                         const std::shared_ptr<SceneNode>& importedScene = nullptr) {
    XMMATRIX view = scene.GetViewMatrix();
    XMMATRIX proj = scene.GetProjectionMatrix();

    // Build draw item list
    static std::vector<IdTechDrawItem> drawItems;
    BuildSceneDrawItems(scene, drawItems, floorMaterial, importedScene);

    // CPU retains only cheap material ordering/backface rejection. GPU decides
    // frustum, projected-size LOD, HZB visibility, and final indirect draw count.
    CullAndBatchDrawItems(scene, view, proj, drawItems);

    scene.clusteredRenderer.setScreenSize((float)g_dx12.screenWidth, (float)g_dx12.screenHeight);
    scene.clusteredRenderer.setCamera(scene.EffectiveCameraFOV(), scene.cameraNear,
        scene.cameraFar, view, proj);
    scene.clusteredRenderer.cullLights();

    // Register immutable meshes once. Frames upload instance/material records only.
    static UINT cubeMesh = VB_INVALID_MESH;
    static UINT planeMesh = VB_INVALID_MESH;
    if (cubeMesh == VB_INVALID_MESH)
        cubeMesh = vb.RegisterMesh(packed.cubeFloats.data(),
            (UINT)(packed.cubeFloats.size() / 8), nullptr, 0);
    if (planeMesh == VB_INVALID_MESH)
        planeMesh = vb.RegisterMesh(packed.planeFloats.data(),
            (UINT)(packed.planeFloats.size() / 8), nullptr, 0);

    vb.SetPalmWindFrame(g_trees.GetWindFrame());
    vb.BeginFrame();
    for (UINT clusterIndex = 0;
         clusterIndex < (UINT)scene.clusteredRenderer.clusters.size();
         ++clusterIndex) {
        const auto& cluster = scene.clusteredRenderer.clusters[clusterIndex];
        vb.SetCluster(clusterIndex, (UINT)cluster.lightCount, cluster.lightIndices);
    }
    static std::vector<UINT> dcIDs;
    dcIDs.clear();
    dcIDs.reserve(drawItems.size());
    static std::vector<IdTechDrawItem> registeredItems;
    registeredItems.clear();
    registeredItems.reserve(drawItems.size());

    // Destruction chunks may only be skipped by the forward pass if every one
    // of them registered. A partial registration -- the pool filling up, say --
    // must leave the forward redraw in place, or the unregistered chunks would
    // be drawn by neither pass and simply vanish.
    UINT destructionChunksSeen = 0;
    UINT destructionChunksRegistered = 0;
    for (IdTechDrawItem item : drawItems) {
        if (item.destructionChunk) ++destructionChunksSeen;
        item.visibilityMeshID = item.primitive
            ? vb.RegisterPrimitive(item.primitive)
            : (item.isCube ? cubeMesh : planeMesh);
        if (item.visibilityMeshID == VB_INVALID_MESH) continue;
        if (item.destructionChunk) ++destructionChunksRegistered;
        if (item.instanceKey)
            item.instanceKey ^= static_cast<uint64_t>(item.visibilityMeshID + 1u) *
                0x9e3779b97f4a7c15ull;
        UINT materialID =
            vb.RegisterMaterialForCurrentPath(item.material.get());
        const UINT flags =
            (item.doubleSided ? 1u : 0u) |
            (item.alphaCutout ? 2u : 0u) |
            (item.alphaFromLuminance ? 4u : 0u);
        UINT dc = vb.RegisterInstance(item.visibilityMeshID,
            item.model, item.color, 0.0f, 0.5f, materialID, flags,
            item.instanceKey, item.palmWindRoot);
        if (dc == UINT_MAX) continue;
        item.materialId = materialID;
        registeredItems.push_back(item);
        dcIDs.push_back(dc);
    }
    drawItems.swap(registeredItems);

    vb.UploadBuffers(g_dx12.commandList.Get());

    // Pass 1: GPU cull and compact the actual ExecuteIndirect stream.
    // PSO state cannot change inside ExecuteIndirect. Keep separate compacted
    // streams for culled and double-sided opaque indexed geometry. Alpha-cutout
    // draws retain direct submission because each batch needs its own texture SRV.
    static GPUDrivenVisibilityContext gpuCulled;
    static GPUDrivenVisibilityContext gpuDoubleSided;
    if (!gpuCulled.initialized) gpuCulled.Init(vb.visPassRootSig.Get());
    if (!gpuDoubleSided.initialized) gpuDoubleSided.Init(vb.visPassRootSig.Get());
    const bool useGPUDriven = gpuCulled.initialized && gpuDoubleSided.initialized;

    UINT drawCount = (UINT)drawItems.size();
    if (drawCount > gpuCulled.maxCommands) drawCount = gpuCulled.maxCommands;
    if (drawCount > MAX_DRAW_CALLS_PER_FRAME) drawCount = MAX_DRAW_CALLS_PER_FRAME;

    UINT culledCount = 0;
    UINT doubleSidedCount = 0;
    static std::vector<UINT> directDraws;
    directDraws.clear();
    directDraws.reserve(drawCount);

    D3D12_GPU_VIRTUAL_ADDRESS frameMatrixCBV = 0;
    FillFrameMatrixBufferForIndirect(shader, view, proj, lightSpace,
        frameMatrixCBV);

    for (UINT i = 0; i < drawCount; i++) {
        const bool indexedOpaque = useGPUDriven && !drawItems[i].alphaCutout &&
            drawItems[i].primitive &&
            drawItems[i].primitive->ibv.BufferLocation != 0;
        if (indexedOpaque) {
            UINT& batchCount = drawItems[i].doubleSided
                ? doubleSidedCount : culledCount;
            GPUDrivenVisibilityContext& context = drawItems[i].doubleSided
                ? gpuDoubleSided : gpuCulled;
            GPUVisibilityCullInput staticInput = {};
            staticInput.command.vbv = drawItems[i].primitive->vbv;
            staticInput.command.ibv = drawItems[i].primitive->ibv;
            staticInput.command.drawCallID = dcIDs[i];
            staticInput.command.drawArgs.IndexCountPerInstance =
                drawItems[i].primitive->indexCount;
            staticInput.command.drawArgs.InstanceCount = 1;
            staticInput.command.drawArgs.StartIndexLocation = 0;
            staticInput.command.drawArgs.BaseVertexLocation = 0;
            staticInput.command.drawArgs.StartInstanceLocation = 0;
            uint64_t inputKey = static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(drawItems[i].primitive));
            inputKey ^= static_cast<uint64_t>(dcIDs[i]) *
                0x9e3779b97f4a7c15ull;
            inputKey ^= static_cast<uint64_t>(
                drawItems[i].primitive->indexCount) << 32;
            XMFLOAT4X4 modelForHash;
            XMStoreFloat4x4(&modelForHash, drawItems[i].model);
            const uint32_t* modelWords =
                reinterpret_cast<const uint32_t*>(&modelForHash);
            for (UINT word = 0; word < 16; ++word) {
                inputKey ^= modelWords[word];
                inputKey *= 1099511628211ull;
            }
            const UINT inputIndex = batchCount++;
            if (context.NeedsInput(inputIndex, inputKey))
                staticInput.worldBounds = ComputeDrawItemBounds(drawItems[i]);
            GPUVisibilityCullInput& input = context.PrepareInput(
                inputIndex, inputKey, staticInput);
            input.command.matrixCBV = frameMatrixCBV;
        } else {
            directDraws.push_back(i);
        }
    }

    // These CBVs live in the same persistently mapped upload buffer used by
    // forward extensions. Reserve the written range so later weapon,
    // transparent, and particle draws cannot overwrite visibility matrices
    // before the GPU consumes them.
    shader.currentDrawCall = (std::max)(shader.currentDrawCall,
        (std::max)(drawCount, 1u));

    GPUVisibilityCullConstants cull = {};
    if ((culledCount > 0 || doubleSidedCount > 0) && useGPUDriven) {
        FrustumPlanes frustum = ExtractFrustumPlanes(view * proj);
        for (UINT i = 0; i < 6; ++i) cull.frustumPlanes[i] = frustum.p[i];
        cull.previousViewProjection = XMMatrixTranspose(previousViewProjection);
        cull.cameraPosition = scene.camera.Position;
        XMFLOAT4X4 projection;
        XMStoreFloat4x4(&projection, proj);
        cull.projectionScaleY = fabsf(projection._22);
        cull.screenSize = XMUINT2(g_dx12.screenWidth, g_dx12.screenHeight);
        cull.hzbMipCount = hzb ? hzb->GetMipCount() : 0;
        cull.useOcclusion = useHZBOcclusion ? 1u : 0u;
        cull.lodPixelThreshold = 2.0f;
        if (culledCount > 0) {
            cull.commandCount = culledCount;
            gpuCulled.Cull(g_dx12.commandList.Get(), cull, hzb);
        }
        if (doubleSidedCount > 0) {
            cull.commandCount = doubleSidedCount;
            gpuDoubleSided.Cull(g_dx12.commandList.Get(), cull, hzb);
        }
    }

    {
    // Raster and resolve are separately timed: the resolve is a single
    // full-screen compute dispatch whose cost scales with pixels and lighting
    // complexity, while the raster scales with draw count. One combined number
    // cannot tell you which to attack.
    ProfilerDX12::Scope rasterScope(g_profiler, "VB Raster", g_dx12.commandList.Get());
    vb.FlushBindlessTextureTransitions(g_dx12.commandList.Get());
    vb.BeginVisibilityPass(g_dx12.commandList.Get());
    g_dx12.commandList->SetGraphicsRootConstantBufferView(0, frameMatrixCBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (culledCount > 0) {
        vb.SetVisPassDraw(g_dx12.commandList.Get(), 0, 0, false, false, false);
        gpuCulled.Execute(g_dx12.commandList.Get(), culledCount);
    }
    if (doubleSidedCount > 0) {
        vb.SetVisPassDraw(g_dx12.commandList.Get(), 0, 0, true, false, false);
        gpuDoubleSided.Execute(g_dx12.commandList.Get(), doubleSidedCount);
    }
    for (UINT i : directDraws) {
            D3D12_VERTEX_BUFFER_VIEW vbv = drawItems[i].primitive
                ? drawItems[i].primitive->vbv
                : (drawItems[i].isCube ? geo.cubeVBV : geo.planeVBV);
            g_dx12.commandList->IASetVertexBuffers(0, 1, &vbv);
            vb.SetVisPassDraw(g_dx12.commandList.Get(), dcIDs[i],
                drawItems[i].materialId, drawItems[i].doubleSided,
                drawItems[i].alphaCutout, drawItems[i].alphaFromLuminance);
            if (drawItems[i].primitive &&
                drawItems[i].primitive->ibv.BufferLocation != 0) {
                g_dx12.commandList->IASetIndexBuffer(&drawItems[i].primitive->ibv);
                g_dx12.commandList->DrawIndexedInstanced(
                    drawItems[i].primitive->indexCount, 1, 0, 0, 0);
            } else {
                const UINT vertexCount = drawItems[i].primitive
                    ? static_cast<UINT>(drawItems[i].primitive->vertices.size() / 12)
                    : (drawItems[i].isCube ? 36u : 6u);
                g_dx12.commandList->DrawInstanced(vertexCount, 1, 0, 0);
            }
    }

    // Terrain rasterizes into the visibility buffer before the pass ends, using
    // the same amplification/mesh shaders the forward path uses -- so the
    // clipmap, culling and LOD are unchanged, and only the pixel shader
    // differs. It goes last because it switches to the mesh pipeline and binds
    // its own root tables, which would disturb the IA draws above.
    //
    // terrainVisibilityActiveThisFrame is what the resolve keys off, and what
    // ForwardRenderer reads to decide whether to skip the forward terrain draw.
    // It is set only after the draw is actually recorded, so a failure anywhere
    // here leaves terrain on the forward path rather than dropping it.
    vb.terrainVisibilityActiveThisFrame = false;
    // Publish terrain's textures and material parameters BEFORE the readiness
    // test, not inside it. TerrainVisibilityReady() requires the layer arrays
    // to be non-null, so setting them inside the gate they gate is a deadlock:
    // the arrays never arrive, the toggle reports unavailable forever.
    if (scene.useMeshTerrain && g_terrain.supported) {
        // Mirrors SetTerrainMaterial: terrain scans are OpenGL normal maps, and
        // the authored footpaths are built-in levels only.
        vb.SetTerrainMaterialParams(g_customLevelMode ? 0.0f : 3.0f, -1.0f);
        vb.SetTerrainTextures(g_terrain.terrainAlbedoArray.Get(),
                              g_terrain.terrainNormalArray.Get(),
                              g_terrain.terrainRoughnessArray.Get());
    }
    if (scene.useMeshTerrain && g_terrain.VisibilitySupported() &&
        vb.TerrainVisibilityReady()) {
        ProfilerDX12::Scope terrainScope(
            g_profiler, "VB Terrain", g_dx12.commandList.Get());
        TerrainRendererDX12::Params terrainParams = CurrentTerrainParams();
        terrainParams.heightScale = scene.terrainHeightScale;
        // DrawVisibility switches to the main graphics root signature, so
        // matrices and camera are (re)bound here against that signature rather
        // than inherited from the visibility pass. Both matter to the AS:
        // modelViewProjection drives its frustum cull, and viewPos is what
        // every clipmap ring origin snaps to.
        if (g_terrain.PrepareVisibilityRootSignature(shader)) {
            // Terrain rasterizes with the SAME projection as every other
            // visibility-pass draw. It shares one depth buffer with them, and
            // the resolve rebuilds world position from that depth through a
            // single invViewProj -- so a projection of its own would make the
            // reconstructed surface swing against the camera. The forward
            // extensions pass drawing unjittered over jittered depth is a
            // pre-existing, accepted sub-pixel tradeoff; it is not terrain's to
            // correct here.
            shader.SetMatrices(XMMatrixIdentity(), view, proj, lightSpace);
            shader.SetCamera(scene.camera.Position);
            vb.SetTerrainProjection(proj);
            if (g_terrain.DrawVisibility(shader, terrainParams))
                vb.terrainVisibilityActiveThisFrame = true;
            // SetMatrices writes the shader's current per-draw upload slot.
            // Keep later forward-extension draws from overwriting that memory
            // before this command list executes; a destruction chunk transform
            // here rotates/translates the entire procedural terrain.
            shader.NextDrawCall();
        }
        // No pipeline restore here. Terrain is the last draw in the pass, and
        // binding an IA pipeline built for the main root signature while the
        // visibility render target is still set would leave the command list
        // in a mismatched state for no benefit. The next pass sets its own
        // root signature and PSO before it draws anything.
    }
    g_terrainInVisibilityBuffer = vb.terrainVisibilityActiveThisFrame;
    // Only claim the chunks for the resolve when the toggle is on AND no chunk
    // this frame failed to register. A registration failure leaves the forward
    // pass responsible, which is the safe direction: chunks drawn twice cost
    // performance, chunks drawn by nobody are invisible.
    //
    // Deliberately NOT conditioned on "some chunks were seen". A frame where
    // every chunk is culled or the batch list is momentarily empty says nothing
    // about ownership, and flipping the flag on those frames hands the chunks
    // back and forth between the two passes. Both write the shared depth
    // buffer, and the toggle invalidates temporal history, so the oscillation
    // destabilises everything the resolve shades from that depth -- terrain
    // most visibly, since it covers the screen.
    g_destructionInVisibilityBuffer = vb.DestructionVisibilityRequested() &&
        destructionChunksRegistered == destructionChunksSeen;

    vb.EndVisibilityPass(g_dx12.commandList.Get());
    }

    // Lighting setup
    {
        LightBufferDX12 lb = {};
        lb.lightPos = scene.lightPos;
        lb.lightType = scene.lightType;
        lb.lightColor = scene.EffectiveLightColor();
        lb.constant = scene.lightConstant;
        lb.linear = scene.lightLinear;
        lb.quadratic = scene.lightQuadratic;
        lb.ambientStrength = scene.ambientStrength;
        lb.ambientLightingIntensity = scene.ambientLightingIntensity;
        lb.specularStrength = scene.specularStrength;
        lb.shininess = scene.specularShininess;
        lb.shadowBias = scene.shadowBias;
        lb.enableShadows = scene.enableShadows ? 1 : 0;
        shader.lightBuffer.CopyData(g_dx12.frameIndex, lb);

        PointLightsBufferDX12 plb = {};
        const auto& sourceLights = scene.clusteredRenderer.lights;
        int cnt = ((int)sourceLights.size() < 64) ? (int)sourceLights.size() : 64;
        plb.numPointLights = cnt;
        for (int i = 0; i < cnt; i++) {
            plb.lights[i].position = sourceLights[i].position;
            plb.lights[i].radius = sourceLights[i].radius;
            plb.lights[i].color = sourceLights[i].color;
            plb.lights[i].intensity = sourceLights[i].active
                ? sourceLights[i].intensity : 0.0f;
        }
        shader.pointLightsBuffer.CopyData(g_dx12.frameIndex, plb);
    }

    shader.SetSH();
    shader.SetDDGI(scene.useDDGI &&
        (g_ddgiRenderer.computeInitialized || g_dxrDDGIProbeCount > 0),
        scene.giIntensity, scene.normalBias, scene.probeSpacing,
        g_dxrDDGIProbeCount, g_dxrDDGICellCount, g_dxrDDGICellSize);
    if (scene.useDDGI && g_dxrDDGIProbeCount == 0 &&
        g_ddgiRenderer.computeInitialized) {
        DDGIMainLightData mainLight = {};
        mainLight.lightPos = scene.lightPos;
        mainLight.lightType = scene.lightType;
        mainLight.lightColor = scene.lightColor;
        mainLight.intensity = scene.directionalLightIntensity;
        mainLight.lightSpaceMatrix = XMMatrixTranspose(lightSpace);
        mainLight.shadowBias = scene.shadowBias;
        mainLight.enableShadows = scene.enableShadows && shadowResource ? 1 : 0;
        const int pointLightCount = (std::min)(
            (int)scene.clusteredRenderer.lights.size(), 64);
        g_ddgiRenderer.UpdateProbes(g_dx12.commandList.Get(),
            shader.pointLightsBuffer.GetGPUAddress(g_dx12.frameIndex),
            pointLightCount,
            shader.ddgiBuffer.GetGPUAddress(g_dx12.frameIndex), mainLight);
    }
    vb.UpdateLightDescriptors(
        shader.lightBuffer.GetGPUAddress(g_dx12.frameIndex),
        shader.pointLightsBuffer.GetGPUAddress(g_dx12.frameIndex),
        shader.shBuffer.GetGPUAddress(g_dx12.frameIndex),
        shader.ddgiBuffer.GetGPUAddress(g_dx12.frameIndex));
    vb.UpdateShadowMapDescriptor(shadowResource);

    LightBufferDX12 dummyLB = {};
    PointLightsBufferDX12 dummyPL = {};
    {
        ProfilerDX12::Scope resolveScope(
            g_profiler, "VB Resolve", g_dx12.commandList.Get());
        vb.Resolve(g_dx12.commandList.Get(), view, proj, lightSpace,
            previousViewProjection,
            scene.camera.Position, scene.cameraNear, scene.cameraFar,
            scene.contactShadowStrength, scene.ambientOcclusionRadius,
            scene.contactShadowLinearDepth,
            dummyLB, dummyPL);
    }

    vb.BeginForwardExtensions(g_dx12.commandList.Get());
}

#endif // IDTECH_RENDERER_H
