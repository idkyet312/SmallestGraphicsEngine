#ifndef FORWARD_RENDERER_H
#define FORWARD_RENDERER_H

// Forward clustered renderer � the original rendering path.
// Binds the clustered forward shader and draws each object with per-draw CBVs.

#include "DX12Core.h"
#include "ShaderDX12.h"
#include "DDGI_DX12.h"
#include "Scene.h"
#include "SceneGraph.h"
#include "MeshShaderDX12.h"
#include "TerrainRendererDX12.h"
#include "DestructionDX12.h"
#include "RoofModel.h"
#include "WaterVolume.h"
#include "PrefabRuntime.h"
#include "PalmTrees.h"
#include "PalmModel.h"
#include "GunModel.h"
#include "ArmsModel.h"
#include "GrassField.h"
#include <unordered_map>

extern MeshShaderDX12 g_meshShader;
extern bool g_useMeshShader;
extern TerrainRendererDX12 g_terrain;
// Shared island-builder terrain params (island size, extent, origin offset,
// GPU-safe clamps). Defined in main.cpp; declared here so the terrain draw
// uses the same params as foliage/collision/GI instead of a stale default.
TerrainRendererDX12::Params CurrentTerrainParams();
extern bool g_showH2Model;
extern WaterVolume g_water;
extern WaterVolume g_ocean;   // sea ringing the island
extern ComPtr<ID3D12Resource> g_smokeTexture;   // soft smoke sprite for billboards
extern ComPtr<ID3D12Resource> g_bloodTexture;
extern ComPtr<ID3D12Resource> g_muzzleFlashTexture;
extern ComPtr<ID3D12Resource> g_fireTexture;
extern ComPtr<ID3D12Resource> g_explosionTexture;   // 4x4 flipbook explosion sheet
extern std::shared_ptr<SceneNode> g_explosiveBarrelModel;
extern std::shared_ptr<SceneNode> g_explosiveBarrelShadowModel;
extern std::shared_ptr<SceneNode> g_humveeModel;
extern std::shared_ptr<SceneNode> g_humveeShadowModel;
extern std::shared_ptr<SceneNode> g_helicopterModel;
struct DandelionInstance {
    DirectX::XMFLOAT4X4 transform;
    DirectX::XMFLOAT3 center;
    float radius = 1.0f;
};
extern std::shared_ptr<SceneNode> g_dandelionModel;
extern std::vector<DandelionInstance> g_dandelionInstances;
extern ID3D12Resource* g_skyEnvironmentResource;
extern ID3D12Resource* g_specularEnvironmentResource;
extern ID3D12Resource* g_brdfIntegrationResource;
extern ID3D12Resource* g_ddgiIrradianceResource;
extern ID3D12Resource* g_ddgiVisibilityResource;
extern ID3D12Resource* g_dxrDDGIProbeResource;
extern ID3D12Resource* g_dxrDDGICellResource;
extern ID3D12Resource* g_dxrDDGIIndexResource;
extern UINT g_dxrDDGIProbeCount;
extern UINT g_dxrDDGICellCount;
extern UINT g_dxrDDGIIndexCount;
extern float g_dxrDDGICellSize;
extern DDGIRendererDX12 g_ddgiRenderer;
extern DirectX::XMFLOAT3 g_helicopterPosition;
extern bool g_stressTestMode;
extern bool g_emptyLevelMode;
DirectX::XMMATRIX HumveeWorldMatrix();
DirectX::XMMATRIX SecondaryHumveeWorldMatrix();
DirectX::XMMATRIX HelicopterWorldMatrix();
DirectX::XMMATRIX SecondaryHelicopterWorldMatrix();

struct GeometryBuffers {
    ComPtr<ID3D12Resource>   cubeVertexBuffer;
    ComPtr<ID3D12Resource>   planeVertexBuffer;
    ComPtr<ID3D12Resource>   sphereVertexBuffer;
    ComPtr<ID3D12Resource>   capsuleVertexBuffer;
    ComPtr<ID3D12Resource>   quadVertexBuffer;      // unit XY billboard quad
    ComPtr<ID3D12Resource>   flashVertexBuffer;     // first cell of 4-frame VFX sheet
    ComPtr<ID3D12Resource>   fireVertexBuffer;      // 60 cells from 10x6 fire sheet
    ComPtr<ID3D12Resource>   explosionVertexBuffer; // 16 cells from 4x4 explosion sheet
    D3D12_VERTEX_BUFFER_VIEW cubeVBV  = {};
    D3D12_VERTEX_BUFFER_VIEW planeVBV = {};
    D3D12_VERTEX_BUFFER_VIEW sphereVBV = {};
    D3D12_VERTEX_BUFFER_VIEW capsuleVBV = {};
    D3D12_VERTEX_BUFFER_VIEW quadVBV = {};
    D3D12_VERTEX_BUFFER_VIEW flashVBV = {};
    D3D12_VERTEX_BUFFER_VIEW fireVBV = {};
    D3D12_VERTEX_BUFFER_VIEW explosionVBV = {};
    UINT                     sphereVertexCount = 0;
    UINT                     capsuleVertexCount = 0;
};

// Vertex layout shared across renderers
struct VertexPosNormUV {
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT2 texCoord;
    XMFLOAT4 tangent = XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f);
};

inline bool CreateVertexBuffer(const std::vector<VertexPosNormUV>& verts,
                               ComPtr<ID3D12Resource>& buffer,
                               D3D12_VERTEX_BUFFER_VIEW& vbv) {
    UINT size = (UINT)(verts.size() * sizeof(VertexPosNormUV));
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = g_dx12.device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&buffer));
    if (FAILED(hr)) return false;

    void* mapped;
    D3D12_RANGE r = { 0, 0 };
    if (FAILED(buffer->Map(0, &r, &mapped))) return false;
    memcpy(mapped, verts.data(), size);
    buffer->Unmap(0, nullptr);

    vbv.BufferLocation = buffer->GetGPUVirtualAddress();
    vbv.SizeInBytes    = size;
    vbv.StrideInBytes  = sizeof(VertexPosNormUV);
    return true;
}

inline void DrawCube(const GeometryBuffers& geo) {
    g_dx12.commandList->IASetVertexBuffers(0, 1, &geo.cubeVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(36, 1, 0, 0);
}

inline void DrawPlane(const GeometryBuffers& geo) {
    g_dx12.commandList->IASetVertexBuffers(0, 1, &geo.planeVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(6, 1, 0, 0);
}

// A unit quad in the XY plane (-0.5..0.5, UV 0..1), for camera-facing billboards.
inline void DrawQuad(const GeometryBuffers& geo) {
    g_dx12.commandList->IASetVertexBuffers(0, 1, &geo.quadVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(6, 1, 0, 0);
}

inline void DrawFlashQuad(const GeometryBuffers& geo) {
    g_dx12.commandList->IASetVertexBuffers(0, 1, &geo.flashVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(6, 1, 0, 0);
}

inline void DrawFireFrame(const GeometryBuffers& geo, UINT frame) {
    g_dx12.commandList->IASetVertexBuffers(0, 1, &geo.fireVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(6, 1, (frame % 60) * 6, 0);
}

// One cell of the 4x4 explosion flipbook (frame 0 top-left, row-major).
inline void DrawExplosionFrame(const GeometryBuffers& geo, UINT frame) {
    g_dx12.commandList->IASetVertexBuffers(0, 1, &geo.explosionVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(6, 1, (frame % 16) * 6, 0);
}

inline void DrawSphere(const GeometryBuffers& geo) {
    if (!geo.sphereVertexCount) { DrawCube(geo); return; }
    g_dx12.commandList->IASetVertexBuffers(0, 1, &geo.sphereVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(geo.sphereVertexCount, 1, 0, 0);
}

inline void DrawCapsule(const GeometryBuffers& geo) {
    if (!geo.capsuleVertexCount) { DrawSphere(geo); return; }
    g_dx12.commandList->IASetVertexBuffers(0, 1, &geo.capsuleVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(geo.capsuleVertexCount, 1, 0, 0);
}

// Transparent world particles must run after every opaque pass, including GPU
// skinned enemies. They still depth-test, so smoke behind a character stays
// hidden while smoke in front blends over it.
inline void RenderImpactBillboards(Scene& scene, ShaderDX12& shader,
                                   const GeometryBuffers& geo,
                                   const XMMATRIX& lightSpace) {
    const XMMATRIX view = scene.GetViewMatrix();
    const XMMATRIX proj = scene.GetProjectionMatrix();
    const XMMATRIX invRot = XMMatrixTranspose(view);
    const XMVECTOR camRight = XMVectorSetW(invRot.r[0], 0.0f);
    const XMVECTOR camUp = XMVectorSetW(invRot.r[1], 0.0f);
    const XMVECTOR camFwd = XMVectorSetW(invRot.r[2], 0.0f);

    shader.UseTransparent();
    for (auto& sp : scene.impactParticles) {
        if (sp.spark) continue;
        const float fade = sp.life / sp.maxLife;
        const float age = 1.0f - fade;
        const float fadeIn = age < 0.15f ? age / 0.15f : 1.0f;
        const float fadeOut = fade < 0.4f ? fade / 0.4f : 1.0f;
        const float opacity = sp.blood
            ? (std::min)(0.36f, fadeIn * fadeOut * 0.36f)
            : (std::min)(0.85f, fadeIn * fadeOut * 0.85f);
        if (opacity <= 0.01f) { shader.NextDrawCall(); continue; }

        const XMVECTOR pos = XMVectorSet(
            sp.position.x, sp.position.y, sp.position.z, 1.0f);
        const XMMATRIX model(camRight * sp.size, camUp * sp.size,
                            camFwd * sp.size, XMVectorSetW(pos, 1.0f));
        shader.SetMatrices(model, view, proj, lightSpace);
        XMFLOAT3 tint = sp.color;
        ID3D12Resource* texture = g_bloodTexture.Get();
        if (!sp.blood) {
            const float g = 1.0f + 2.0f * age;
            tint = XMFLOAT3((std::min)(1.0f, sp.color.x * g),
                            (std::min)(1.0f, sp.color.y * g),
                            (std::min)(1.0f, sp.color.z * g));
            texture = g_smokeTexture.Get();
        }
        shader.SetSmokeMaterial(tint, opacity, texture);
        DrawQuad(geo);
        shader.NextDrawCall();
    }

    // Animated explosion flipbooks: alpha-blended so the smoky tail frames of
    // the sheet still read as smoke instead of washing out additively.
    if (g_explosionTexture && geo.explosionVBV.BufferLocation) {
        for (const ExplosionFX& fx : scene.explosionFX) {
            const float t = (std::min)(1.0f, fx.age / fx.duration);
            const UINT frame = (std::min)(15u, (UINT)(t * 16.0f));
            // Fast pressure bloom, slight overshoot, then smoke collapse.
            const float bloom = 0.18f + 0.92f * (std::min)(1.0f, t * 5.0f);
            const float opacity = t > 0.72f ? (1.0f - t) / 0.28f : 1.0f;
            const float size = fx.size * bloom;
            const XMVECTOR pos = XMVectorSet(
                fx.position.x, fx.position.y, fx.position.z, 1.0f);
            const XMMATRIX model(camRight * size, camUp * size,
                                 camFwd * size, XMVectorSetW(pos, 1.0f));
            shader.SetMatrices(model, view, proj, lightSpace);
            shader.SetSmokeMaterial(XMFLOAT3(1.0f, 1.0f, 1.0f), opacity,
                                    g_explosionTexture.Get());
            DrawExplosionFrame(geo, frame);
            shader.NextDrawCall();
        }
    }

    if (g_fireTexture && geo.fireVBV.BufferLocation) {
        shader.UseAdditive();
        for (const ExplosiveBarrel& barrel : scene.explosiveBarrels) {
            if (!barrel.active || !barrel.burning) continue;
            const float age = 3.0f - barrel.fuse;
            const UINT frame = static_cast<UINT>((std::max)(0.0f, age) * 24.0f) % 60;
            const XMVECTOR pos = XMVectorSet(
                barrel.position.x, barrel.position.y + 0.82f,
                barrel.position.z, 1.0f);
            const XMMATRIX model(camRight * 0.48f, camUp * 0.82f,
                                 camFwd * 0.48f, XMVectorSetW(pos, 1.0f));
            shader.SetMatrices(model, view, proj, lightSpace);
            shader.SetSmokeMaterial(XMFLOAT3(1.0f, 0.72f, 0.38f), 0.92f,
                                    g_fireTexture.Get());
            DrawFireFrame(geo, frame);
            shader.NextDrawCall();
        }
    }
    shader.Use(scene.wireframeMode);
}

// UV sphere as a non-indexed triangle list of VertexPosNormUV.
inline std::vector<VertexPosNormUV> BuildSphereVertices(int stacks = 12, int slices = 16) {
    std::vector<VertexPosNormUV> verts;
    auto at = [&](int st, int sl) {
        const float v = (float)st / stacks, u = (float)sl / slices;
        const float phi = v * 3.14159265f, theta = u * 2.0f * 3.14159265f;
        const float sp = sinf(phi), cp = cosf(phi), stc = sinf(theta), ctc = cosf(theta);
        XMFLOAT3 n(sp * ctc, cp, sp * stc);
        return VertexPosNormUV{ { n.x * 0.5f, n.y * 0.5f, n.z * 0.5f }, n, { u, v } };
    };
    for (int st = 0; st < stacks; ++st) for (int sl = 0; sl < slices; ++sl) {
        VertexPosNormUV a = at(st, sl), b = at(st + 1, sl), c = at(st + 1, sl + 1), d = at(st, sl + 1);
        verts.push_back(a); verts.push_back(b); verts.push_back(c);
        verts.push_back(a); verts.push_back(c); verts.push_back(d);
    }
    return verts;
}

// Y-axis capsule contained in x/z +/-0.25 and y +/-0.5. Ragdoll transforms
// scale it to each physics part. Cylinder middle keeps limbs connected and
// readable; stretched spheres tapered to points and looked like floating blobs.
inline std::vector<VertexPosNormUV> BuildCapsuleVertices(int hemiRings = 6, int slices = 16) {
    struct Ring { float y, radius, normalY, normalRadius; };
    std::vector<Ring> rings;
    constexpr float pi = 3.14159265f;
    constexpr float radius = 0.25f;
    constexpr float cylinderHalf = 0.25f;

    for (int i = 0; i <= hemiRings; ++i) {
        const float angle = -pi * 0.5f + pi * 0.5f * (float)i / hemiRings;
        rings.push_back({ -cylinderHalf + radius * sinf(angle), radius * cosf(angle),
                          sinf(angle), cosf(angle) });
    }
    rings.push_back({ cylinderHalf, radius, 0.0f, 1.0f });
    for (int i = 1; i <= hemiRings; ++i) {
        const float angle = pi * 0.5f * (float)i / hemiRings;
        rings.push_back({ cylinderHalf + radius * sinf(angle), radius * cosf(angle),
                          sinf(angle), cosf(angle) });
    }

    auto at = [&](size_t ring, int slice) {
        const float u = (float)slice / slices;
        const float theta = u * 2.0f * pi;
        const Ring& r = rings[ring];
        const float c = cosf(theta), s = sinf(theta);
        return VertexPosNormUV{
            { r.radius * c, r.y, r.radius * s },
            { r.normalRadius * c, r.normalY, r.normalRadius * s },
            { u, (float)ring / (rings.size() - 1) }
        };
    };

    std::vector<VertexPosNormUV> verts;
    for (size_t ring = 0; ring + 1 < rings.size(); ++ring) {
        for (int slice = 0; slice < slices; ++slice) {
            const VertexPosNormUV a = at(ring, slice);
            const VertexPosNormUV b = at(ring + 1, slice);
            const VertexPosNormUV c = at(ring + 1, slice + 1);
            const VertexPosNormUV d = at(ring, slice + 1);
            verts.push_back(a); verts.push_back(b); verts.push_back(c);
            verts.push_back(a); verts.push_back(c); verts.push_back(d);
        }
    }
    return verts;
}

// Draw a standalone mesh's primitives at `model`, honouring their materials. This
// is the per-primitive body of DrawSceneNode, pulled out so meshes that are not
// part of a scene graph (e.g. the sliced palm) can be drawn the same way.
inline void DrawMeshAt(const std::shared_ptr<SceneMesh>& mesh, ShaderDX12& shader,
                       const XMMATRIX& model,
                       const XMMATRIX& view, const XMMATRIX& proj,
                       const XMMATRIX& lightSpace, bool colorNormalOnly = false,
                       bool visibilityExtensionsOnly = false,
                       ID3D12PipelineState* pipelineOverride = nullptr,
                       XMFLOAT4 palmWindRoot = {}) {
    if (!mesh) return;
    shader.SetMatrices(model, view, proj, lightSpace, palmWindRoot);

    for (const auto& prim : mesh->primitives) {
        if (prim.vbv.BufferLocation == 0) continue;

        const bool transparent = prim.material && prim.material->baseColorFactor.w < 0.999f;
        const bool alphaCutout = prim.material && prim.material->alphaCutout;
        const bool visibilityOwned = prim.visibilityMeshID != UINT_MAX &&
            !transparent && !alphaCutout && !prim.skinBuffer &&
            !prim.vertices.empty();
        if (visibilityExtensionsOnly && visibilityOwned) continue;
        if (prim.material) {
            if (transparent) shader.UseTransparent(); else shader.Use(false);
            XMFLOAT3 color(prim.material->baseColorFactor.x,
                           prim.material->baseColorFactor.y,
                           prim.material->baseColorFactor.z);
            shader.SetObjectMaterial(
                color,
                prim.material->baseColorTexture != nullptr,
                prim.material->normalTexture != nullptr,
                colorNormalOnly ? 0.0f : prim.material->metallicFactor,
                colorNormalOnly ? 0.58f : prim.material->roughnessFactor,
                prim.material->baseColorTexture.Get(),
                prim.material->normalTexture.Get(),
                colorNormalOnly ? nullptr : prim.material->metallicRoughnessTexture.Get(),
                colorNormalOnly ? false : prim.material->roughnessOnlyTexture,
                prim.material->baseColorFactor.w,
                prim.material->alphaCutout,
                prim.material.get());   // cache its descriptors: they never change
        } else {
            shader.SetObjectMaterial(XMFLOAT3(1, 1, 1), false, false, 0.0f, 0.5f, nullptr, nullptr, nullptr);
        }

        // Palm slices are IA meshes (no meshlet data), so always take the VS/PS path.
        g_dx12.commandList->SetPipelineState(
            pipelineOverride && !transparent ? pipelineOverride :
            (transparent ? shader.GetTransparentPipelineState()
                         : shader.GetPipelineState(false)));
        g_dx12.commandList->IASetVertexBuffers(0, 1, &prim.vbv);
        g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (prim.ibv.BufferLocation != 0) {
            g_dx12.commandList->IASetIndexBuffer(&prim.ibv);
            g_dx12.commandList->DrawIndexedInstanced(prim.indexCount, 1, 0, 0, 0);
        } else {
            g_dx12.commandList->DrawInstanced((UINT)(prim.vertices.size() / 12), 1, 0, 0);
        }
        shader.NextDrawCall();
    }
}

// Draw an imported GLB scene graph node (and its children) with an extra
// world transform applied on top of each node's own local/global transform.
inline bool RasterPrimitiveIntersectsFrustum(const MeshPrimitive& primitive,
                                             const XMMATRIX& modelViewProjection) {
    if (!primitive.boundsValid) return true;

    bool outsideLeft = true, outsideRight = true;
    bool outsideBottom = true, outsideTop = true;
    bool outsideNear = true, outsideFar = true;
    for (UINT corner = 0; corner < 8; ++corner) {
        const XMVECTOR local = XMVectorSet(
            (corner & 1) ? primitive.boundsMax.x : primitive.boundsMin.x,
            (corner & 2) ? primitive.boundsMax.y : primitive.boundsMin.y,
            (corner & 4) ? primitive.boundsMax.z : primitive.boundsMin.z,
            1.0f);
        XMFLOAT4 clip;
        XMStoreFloat4(&clip, XMVector4Transform(local, modelViewProjection));
        // Bounds crossing the camera plane are ambiguous in homogeneous clip
        // space. Keep them to avoid near-camera popping.
        if (clip.w <= 0.0f) return true;
        outsideLeft &= clip.x < -clip.w;
        outsideRight &= clip.x > clip.w;
        outsideBottom &= clip.y < -clip.w;
        outsideTop &= clip.y > clip.w;
        outsideNear &= clip.z < 0.0f;
        outsideFar &= clip.z > clip.w;
    }
    return !(outsideLeft || outsideRight || outsideBottom || outsideTop ||
             outsideNear || outsideFar);
}

struct ForwardExtensionListCacheEntry {
    std::weak_ptr<SceneNode> root;
    std::vector<SceneNode*> nodes;
};

inline std::unordered_map<const SceneNode*, ForwardExtensionListCacheEntry>
    g_forwardExtensionListCache;

inline void CollectForwardExtensionNodes(const std::shared_ptr<SceneNode>& node,
    std::vector<SceneNode*>& nodes) {
    if (!node) return;
    bool nodeHasExtensions = false;
    if (node->mesh) for (const MeshPrimitive& prim : node->mesh->primitives) {
        if (prim.vbv.BufferLocation == 0) continue;
        const bool transparent = prim.material &&
            prim.material->baseColorFactor.w < 0.999f;
        const bool alphaCutout = prim.material && prim.material->alphaCutout;
        const bool visibilityOwned = prim.visibilityMeshID != UINT_MAX &&
            !transparent && !alphaCutout && !prim.skinBuffer && !prim.vertices.empty();
        if (!visibilityOwned) {
            nodeHasExtensions = true;
            break;
        }
    }
    if (nodeHasExtensions) nodes.push_back(node.get());
    for (const std::shared_ptr<SceneNode>& child : node->children)
        CollectForwardExtensionNodes(child, nodes);
}

inline const std::vector<SceneNode*>& GetForwardExtensionNodes(
    const std::shared_ptr<SceneNode>& node) {
    static const std::vector<SceneNode*> empty;
    if (!node) return empty;
    auto found = g_forwardExtensionListCache.find(node.get());
    if (found != g_forwardExtensionListCache.end()) {
        std::shared_ptr<SceneNode> cached = found->second.root.lock();
        if (cached.get() == node.get()) return found->second.nodes;
        g_forwardExtensionListCache.erase(found);
    }
    ForwardExtensionListCacheEntry entry;
    entry.root = node;
    CollectForwardExtensionNodes(node, entry.nodes);
    auto inserted = g_forwardExtensionListCache.emplace(node.get(),
        std::move(entry));
    if ((g_forwardExtensionListCache.size() & 511u) == 0u) {
        for (auto it = g_forwardExtensionListCache.begin();
             it != g_forwardExtensionListCache.end();) {
            if (it->second.root.expired())
                it = g_forwardExtensionListCache.erase(it);
            else
                ++it;
        }
    }
    return inserted.first->second.nodes;
}

inline void DrawSceneNodeMesh(SceneNode* node, ShaderDX12& shader,
                              const XMMATRIX& worldTransform,
                              const XMMATRIX& view, const XMMATRIX& proj,
                              const XMMATRIX& lightSpace,
                              bool visibilityExtensionsOnly) {
    if (!node) return;

    if (node->mesh) {
        XMMATRIX model = XMLoadFloat4x4(&node->globalTransform) * worldTransform;
        shader.SetMatrices(model, view, proj, lightSpace);

        for (const auto& prim : node->mesh->primitives) {
            if (prim.vbv.BufferLocation == 0) continue;

            const bool transparent = prim.material && prim.material->baseColorFactor.w < 0.999f;
            const bool alphaCutout = prim.material && prim.material->alphaCutout;
            const bool visibilityOwned = prim.visibilityMeshID != UINT_MAX &&
                !transparent && !alphaCutout && !prim.skinBuffer &&
                !prim.vertices.empty();
            if (visibilityExtensionsOnly && visibilityOwned) continue;
            const D3D12_GPU_VIRTUAL_ADDRESS meshletDescAddress = prim.meshletDescBuffer
                ? prim.meshletDescBuffer->GetGPUVirtualAddress() : 0;
            const D3D12_GPU_VIRTUAL_ADDRESS boundsAddress = prim.meshletBoundsBuffer
                ? prim.meshletBoundsBuffer->GetGPUVirtualAddress() : 0;
            const D3D12_GPU_VIRTUAL_ADDRESS vertexIndexAddress = prim.meshletVertexIndexBuffer
                ? prim.meshletVertexIndexBuffer->GetGPUVirtualAddress() : 0;
            const D3D12_GPU_VIRTUAL_ADDRESS triangleAddress = prim.meshletTriangleBuffer
                ? prim.meshletTriangleBuffer->GetGPUVirtualAddress() : 0;
            const bool meshShaderDraw = !transparent && g_useMeshShader &&
                g_meshShader.CanDraw(prim.meshletCount, meshletDescAddress,
                    boundsAddress, vertexIndexAddress, triangleAddress);
            if (!meshShaderDraw && !transparent && !prim.skinBuffer &&
                !RasterPrimitiveIntersectsFrustum(prim, model * view * proj))
                continue;

            if (prim.material) {
                if (transparent) shader.UseTransparent(); else shader.Use(false);
                XMFLOAT3 color(prim.material->baseColorFactor.x,
                                prim.material->baseColorFactor.y,
                                prim.material->baseColorFactor.z);
                shader.SetObjectMaterial(
                    color,
                    prim.material->baseColorTexture != nullptr,
                    prim.material->normalTexture != nullptr,
                    prim.material->metallicFactor,
                    prim.material->roughnessFactor,
                    prim.material->baseColorTexture.Get(),
                    prim.material->normalTexture.Get(),
                    prim.material->metallicRoughnessTexture.Get(),
                    prim.material->roughnessOnlyTexture,
                    prim.material->baseColorFactor.w,
                    prim.material->alphaCutout,
                    prim.material.get());   // cache its descriptors: they never change
            } else {
                shader.SetObjectMaterial(XMFLOAT3(1, 1, 1), false, false, 0.0f, 0.5f, nullptr, nullptr, nullptr);
            }

            if (meshShaderDraw) {
                g_meshShader.Draw(prim.vbv,
                    (UINT)(prim.vertices.size() / 12), prim.indexCount,
                    prim.meshletCount, meshletDescAddress, boundsAddress,
                    vertexIndexAddress, triangleAddress,
                    0, 0, prim.material && prim.material->doubleSided,
                    !prim.material || !prim.material->disableOcclusionCulling);
            } else {
                // A previous mesh draw leaves the mesh PSO bound. Restore the
                // conventional VS/PS pipeline before issuing IA draw calls.
                g_dx12.commandList->SetPipelineState(transparent
                    ? shader.GetTransparentPipelineState()
                    : shader.GetPipelineState(false));
                g_dx12.commandList->IASetVertexBuffers(0, 1, &prim.vbv);
                g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                if (prim.ibv.BufferLocation != 0) {
                g_dx12.commandList->IASetIndexBuffer(&prim.ibv);
                g_dx12.commandList->DrawIndexedInstanced(prim.indexCount, 1, 0, 0, 0);
                } else {
                g_dx12.commandList->DrawInstanced((UINT)(prim.vertices.size() / 12), 1, 0, 0);
                }
            }

            shader.NextDrawCall();
        }
    }

}

inline void DrawSceneNode(const std::shared_ptr<SceneNode>& node,
                          ShaderDX12& shader,
                          const XMMATRIX& worldTransform,
                          const XMMATRIX& view, const XMMATRIX& proj,
                          const XMMATRIX& lightSpace,
                          bool visibilityExtensionsOnly = false) {
    if (!node) return;
    if (visibilityExtensionsOnly) {
        const std::vector<SceneNode*>& extensionNodes =
            GetForwardExtensionNodes(node);
        for (SceneNode* extensionNode : extensionNodes)
            DrawSceneNodeMesh(extensionNode, shader, worldTransform, view,
                proj, lightSpace, true);
        return;
    }
    DrawSceneNodeMesh(node.get(), shader, worldTransform, view, proj,
        lightSpace, false);
    for (const std::shared_ptr<SceneNode>& child : node->children)
        DrawSceneNode(child, shader, worldTransform, view, proj, lightSpace,
            false);
}

inline bool SceneNodeSupportsMeshInstancing(const std::shared_ptr<SceneNode>& node) {
    if (!node) return false;
    if (node->mesh) for (const auto& prim : node->mesh->primitives) {
        const bool transparent = prim.material && prim.material->baseColorFactor.w < 0.999f;
        const D3D12_GPU_VIRTUAL_ADDRESS desc = prim.meshletDescBuffer
            ? prim.meshletDescBuffer->GetGPUVirtualAddress() : 0;
        const D3D12_GPU_VIRTUAL_ADDRESS bounds = prim.meshletBoundsBuffer
            ? prim.meshletBoundsBuffer->GetGPUVirtualAddress() : 0;
        const D3D12_GPU_VIRTUAL_ADDRESS vertices = prim.meshletVertexIndexBuffer
            ? prim.meshletVertexIndexBuffer->GetGPUVirtualAddress() : 0;
        const D3D12_GPU_VIRTUAL_ADDRESS triangles = prim.meshletTriangleBuffer
            ? prim.meshletTriangleBuffer->GetGPUVirtualAddress() : 0;
        if (transparent || prim.skinBuffer || !g_meshShader.CanDraw(
                prim.meshletCount, desc, bounds, vertices, triangles)) return false;
    }
    for (const auto& child : node->children)
        if (!SceneNodeSupportsMeshInstancing(child)) return false;
    return true;
}

inline size_t SceneNodeMeshPrimitiveCount(const std::shared_ptr<SceneNode>& node) {
    if (!node) return 0;
    size_t count = node->mesh ? node->mesh->primitives.size() : 0;
    for (const auto& child : node->children)
        count += SceneNodeMeshPrimitiveCount(child);
    return count;
}

// Geometry/material-keyed static batch. Every world transform shares the same
// scene graph and material set, allowing each primitive to become one dispatch.
// Incompatible, transparent, and skinned models use the unchanged draw path.
inline void DrawSceneNodeInstances(const std::shared_ptr<SceneNode>& node,
                                   ShaderDX12& shader,
                                   const std::vector<XMMATRIX>& worldTransforms,
                                   const XMMATRIX& view, const XMMATRIX& proj,
                                   const XMMATRIX& lightSpace) {
    if (!node || worldTransforms.empty()) return;
    const size_t requiredInstances = worldTransforms.size() *
        SceneNodeMeshPrimitiveCount(node);
    if (worldTransforms.size() < 2 || !g_useMeshShader ||
        !SceneNodeSupportsMeshInstancing(node) ||
        requiredInstances > MeshShaderDX12::MaxInstancesPerFrame -
            g_meshShader.currentInstance) {
        for (const XMMATRIX& world : worldTransforms)
            DrawSceneNode(node, shader, world, view, proj, lightSpace);
        return;
    }

    if (node->mesh) {
        std::vector<XMMATRIX> models;
        models.reserve(worldTransforms.size());
        const XMMATRIX local = XMLoadFloat4x4(&node->globalTransform);
        for (const XMMATRIX& world : worldTransforms) models.push_back(local * world);

        shader.SetMatrices(XMMatrixIdentity(), view, proj, lightSpace);
        for (const auto& prim : node->mesh->primitives) {
            if (!prim.vbv.BufferLocation) continue;
            shader.Use(false);
            if (prim.material) {
                const XMFLOAT3 color(prim.material->baseColorFactor.x,
                    prim.material->baseColorFactor.y,
                    prim.material->baseColorFactor.z);
                shader.SetObjectMaterial(color,
                    prim.material->baseColorTexture != nullptr,
                    prim.material->normalTexture != nullptr,
                    prim.material->metallicFactor, prim.material->roughnessFactor,
                    prim.material->baseColorTexture.Get(),
                    prim.material->normalTexture.Get(),
                    prim.material->metallicRoughnessTexture.Get(),
                    prim.material->roughnessOnlyTexture,
                    prim.material->baseColorFactor.w,
                    prim.material->alphaCutout, prim.material.get(),
                    prim.material->alphaFromLuminance,
                    prim.material->ambientScale,
                    prim.material->occlusionStrength,
                    prim.material->normalYSign,
                    prim.material->viewFillStrength);
            } else {
                shader.SetObjectMaterial(XMFLOAT3(1, 1, 1), false, false,
                    0.0f, 0.5f, nullptr, nullptr, nullptr);
            }

            g_meshShader.DrawInstanced(prim.vbv,
                static_cast<UINT>(prim.vertices.size() / 12), prim.indexCount,
                prim.meshletCount,
                prim.meshletDescBuffer->GetGPUVirtualAddress(),
                prim.meshletBoundsBuffer->GetGPUVirtualAddress(),
                prim.meshletVertexIndexBuffer->GetGPUVirtualAddress(),
                prim.meshletTriangleBuffer->GetGPUVirtualAddress(), models,
                prim.material && prim.material->doubleSided,
                !prim.material || !prim.material->disableOcclusionCulling);
            shader.NextDrawCall();
        }
    }

    for (const auto& child : node->children)
        DrawSceneNodeInstances(child, shader, worldTransforms,
            view, proj, lightSpace);
}

class ForwardStaticBatchQueueDX12 {
    struct Entry {
        std::shared_ptr<SceneNode> node;
        std::vector<XMMATRIX> transforms;
    };
    std::vector<Entry> entries;
    std::unordered_map<const SceneNode*, size_t> byGeometry;

public:
    bool Submit(const std::shared_ptr<SceneNode>& node,
                const XMMATRIX& transform) {
        if (!node || !g_useMeshShader) return false;
        auto it = byGeometry.find(node.get());
        if (it != byGeometry.end()) {
            entries[it->second].transforms.push_back(transform);
            return true;
        }
        if (!SceneNodeSupportsMeshInstancing(node)) return false;
        auto [insertedIt, inserted] = byGeometry.emplace(
            node.get(), entries.size());
        (void)inserted;
        entries.push_back({ node, {} });
        it = insertedIt;
        entries[it->second].transforms.push_back(transform);
        return true;
    }

    void Flush(ShaderDX12& shader, const XMMATRIX& view,
               const XMMATRIX& proj, const XMMATRIX& lightSpace) {
        for (Entry& entry : entries)
            DrawSceneNodeInstances(entry.node, shader, entry.transforms,
                view, proj, lightSpace);
        entries.clear();
        byGeometry.clear();
    }
};

// Gribb-Hartmann plane extraction from the CPU-side (row-vector) view*proj.
// Planes come out unnormalised with inward-facing normals; SphereVisible scales
// the radius test by the plane length instead of normalising here.
inline void BuildFrustumPlanes(const XMMATRIX& viewProj, XMFLOAT4 planes[6]) {
    XMFLOAT4X4 m; XMStoreFloat4x4(&m, viewProj);
    const auto set = [&](int i, float a, float b, float c, float d) {
        planes[i] = XMFLOAT4(a, b, c, d);
    };
    set(0, m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41);  // left
    set(1, m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41);  // right
    set(2, m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42);  // bottom
    set(3, m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42);  // top
    set(4, m._13, m._23, m._33, m._43);                                  // near (D3D z>=0)
    set(5, m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43);  // far
}

// Diagnostic: chunks skipped by the frustum cull in the last recorded frame.
inline UINT g_destructionCulledThisFrame = 0;
inline UINT g_destructionBatchesThisFrame = 0;
inline UINT g_destructionChunksSubmittedThisFrame = 0;

inline bool SphereVisible(const XMFLOAT4 planes[6], const XMFLOAT3& c, float r) {
    for (int i = 0; i < 6; ++i) {
        const XMFLOAT4& p = planes[i];
        const float length = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r * length) return false;
    }
    return true;
}

// Draw only the wind-driven grass. Used by the visibility-buffer path to put
// foliage in an independent multisampled layer without multisampling the full
// visibility buffer.
inline void RenderGrassForward(Scene& scene, ShaderDX12& shader,
                               const XMMATRIX& view, const XMMATRIX& proj,
                               const XMMATRIX& lightSpace,
                               ID3D12Resource* shadowMap,
                               ID3D12PipelineState* pipelineOverride = nullptr) {
    ID3D12PipelineState* grassPipeline = pipelineOverride
        ? pipelineOverride : shader.GetGrassPipelineState();
    if (g_emptyLevelMode || !grassPipeline) return;

    // Compute post passes replace descriptor heaps and root signatures. Restore
    // every binding the grass shader consumes before this late raster pass.
    shader.InvalidateGraphicsRootBinding();
    shader.Use(false);
    shader.BindGlobalResources(shadowMap, g_ddgiIrradianceResource,
        g_ddgiVisibilityResource,
        g_specularEnvironmentResource, g_brdfIntegrationResource,
        g_dxrDDGIProbeResource, g_dxrDDGICellResource,
        g_dxrDDGIIndexResource, g_dxrDDGIProbeCount,
        g_dxrDDGICellCount, g_dxrDDGIIndexCount);

    if (g_grass.IsInitialized()) {
        g_grass.SetViewer(scene.camera.Position);
        static std::vector<GrassField::DrawRange> grassRanges;
        g_grass.GetVisible(grassRanges);

        const D3D12_VERTEX_BUFFER_VIEW& gvbv = g_grass.GetVBV();
        const D3D12_GPU_VIRTUAL_ADDRESS ginst =
            g_grass.GetInstanceBufferAddress();
        if (!grassRanges.empty() && gvbv.BufferLocation && ginst) {
            const D3D12_INDEX_BUFFER_VIEW& gibv = g_grass.GetIBV();
            shader.SetMatrices(XMMatrixIdentity(), view, proj, lightSpace);
            shader.SetObjectMaterial(XMFLOAT3(0.117f, 0.152f, 0.025f), false, false,
                                     0.0f, 0.85f, nullptr, nullptr, nullptr);

            GrassField::Params gp = g_grass.GetParams(
                scene.EffectiveCameraFOV(), static_cast<float>(g_dx12.screenHeight));
            static_assert(sizeof(GrassField::Params) == 13 * sizeof(UINT),
                          "GrassParams must match the 13 root constants at b6");
            g_dx12.commandList->SetGraphicsRoot32BitConstants(8, 13, &gp, 0);
            g_dx12.commandList->SetGraphicsRootShaderResourceView(9, ginst);
            g_dx12.commandList->SetPipelineState(grassPipeline);
            g_dx12.commandList->IASetVertexBuffers(0, 1, &gvbv);
            g_dx12.commandList->IASetIndexBuffer(&gibv);
            g_dx12.commandList->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (const GrassField::DrawRange& r : grassRanges) {
                g_dx12.commandList->SetGraphicsRoot32BitConstants(
                    8, 1, &r.firstInstance, 7);
                g_dx12.commandList->DrawIndexedInstanced(
                    GrassField::IndexCount(), r.instanceCount, 0, 0, 0);
            }
            shader.NextDrawCall();
        }
    }

    // Palm crowns are alpha-cut foliage too. Draw them into this same 4x HDR
    // layer with the regular material shader so frond texture edges get true
    // per-sample coverage while trunks stay in the single-sample scene pass.
    ID3D12PipelineState* crownPipeline = shader.GetHDRMSAAPipelineState();
    if (crownPipeline && g_trees.IsInitialized()) {
        const std::shared_ptr<SceneMesh> crown = PalmModel::Crown();
        if (crown) for (const TreeItem& item : g_trees.GetItems()) {
            if (!item.crown) continue;
            DrawMeshAt(crown, shader, XMLoadFloat4x4(&item.transform),
                view, proj, lightSpace, false, false, crownPipeline,
                item.palmWindRoot);
        }
    }
}

// Render the whole scene using the forward clustered path
inline void RenderForward(Scene& scene, ShaderDX12& shader, const GeometryBuffers& geo,
                           const std::shared_ptr<SceneNode>& crateModel = nullptr,
                           const std::shared_ptr<SceneMaterial>& floorMaterial = nullptr,
                           XMMATRIX lightSpace = XMMatrixIdentity(),
                           ID3D12Resource* shadowMap = nullptr,
                           bool visibilityExtensionsOnly = false,
                           bool includeGrass = true) {
    XMMATRIX view = scene.GetViewMatrix();
    XMMATRIX proj = scene.GetProjectionMatrix();

    shader.Use(scene.wireframeMode);
    shader.BindGlobalResources(shadowMap, g_ddgiIrradianceResource,
        g_ddgiVisibilityResource,
        g_specularEnvironmentResource, g_brdfIntegrationResource,
        g_dxrDDGIProbeResource, g_dxrDDGICellResource,
        g_dxrDDGIIndexResource, g_dxrDDGIProbeCount,
        g_dxrDDGICellCount, g_dxrDDGIIndexCount);

    shader.SetLight(scene.lightPos, scene.lightType,
                    scene.EffectiveLightColor(),
                    scene.lightConstant, scene.lightLinear, scene.lightQuadratic,
                    scene.ambientStrength, scene.ambientLightingIntensity,
                    scene.specularStrength, scene.specularShininess,
                    scene.shadowBias, scene.enableShadows && shadowMap != nullptr,
                    shadowMap ? 1.0f / (float)shadowMap->GetDesc().Width
                              : 1.0f / 2048.0f);
    shader.SetCamera(scene.camera.Position);

    // Clustered light cull
    scene.clusteredRenderer.setScreenSize((float)g_dx12.screenWidth, (float)g_dx12.screenHeight);
    scene.clusteredRenderer.setCamera(scene.EffectiveCameraFOV(), scene.cameraNear,
                                      scene.cameraFar, view, proj);
    scene.clusteredRenderer.cullLights();

    auto lightData = scene.clusteredRenderer.getPointLightData();
    if (scene.muzzleFlashTime > 0.0f && lightData.size() < 64) {
        PointLightDataDX12 flashLight = {};
        flashLight.position = scene.GetMuzzleWorldPosition();
        flashLight.radius = 4.5f;
        flashLight.color = XMFLOAT3(1.0f, 0.42f, 0.08f);
        flashLight.intensity = 13.0f * (scene.muzzleFlashTime / scene.muzzleFlashDuration);
        lightData.push_back(flashLight);
    }
    for (const ExplosionFX& fx : scene.explosionFX) {
        if (lightData.size() >= 64) break;
        const float t = (std::min)(1.0f, fx.age / fx.duration);
        const float energy = (1.0f - t) * (1.0f - t);
        if (energy < 0.01f) continue;
        PointLightDataDX12 blastLight = {};
        blastLight.position = fx.position;
        blastLight.radius = fx.size * (1.1f + t * 0.7f);
        blastLight.color = XMFLOAT3(1.0f, 0.24f + t * 0.18f, 0.025f);
        blastLight.intensity = 24.0f * energy;
        lightData.push_back(blastLight);
    }
    shader.SetPointLights((int)lightData.size(), lightData);
    shader.SetDDGI(scene.useDDGI, scene.giIntensity, scene.normalBias,
        scene.probeSpacing, g_dxrDDGIProbeCount, g_dxrDDGICellCount,
        g_dxrDDGICellSize);
    shader.SetSH();

    if (!visibilityExtensionsOnly && scene.useDDGI &&
        g_dxrDDGIProbeCount == 0 &&
        g_ddgiRenderer.computeInitialized) {
        DDGIMainLightData mainLight = {};
        mainLight.lightPos = scene.lightPos;
        mainLight.lightType = scene.lightType;
        mainLight.lightColor = scene.lightColor;
        mainLight.intensity = scene.directionalLightIntensity;
        mainLight.lightSpaceMatrix = XMMatrixTranspose(lightSpace);
        mainLight.shadowBias = scene.shadowBias;
        mainLight.enableShadows = scene.enableShadows && shadowMap ? 1 : 0;
        g_ddgiRenderer.UpdateProbes(g_dx12.commandList.Get(),
            shader.pointLightsBuffer.GetGPUAddress(g_dx12.frameIndex),
            (int)lightData.size(),
            shader.ddgiBuffer.GetGPUAddress(g_dx12.frameIndex), mainLight);
        // Probe update binds its private heap. Restore forward global table.
        shader.BindGlobalResources(shadowMap, g_ddgiIrradianceResource,
            g_ddgiVisibilityResource,
            g_specularEnvironmentResource, g_brdfIntegrationResource,
            g_dxrDDGIProbeResource, g_dxrDDGICellResource,
            g_dxrDDGIIndexResource, g_dxrDDGIProbeCount,
            g_dxrDDGICellCount, g_dxrDDGIIndexCount);
    }

    g_meshShader.wireframe = scene.meshletWireframe;
    g_terrain.wireframe = scene.meshletWireframe;
    ForwardStaticBatchQueueDX12 staticBatches;

    // Floor: visibility owns the flat floor in hybrid mode. Mesh terrain remains
    // on its amplification/mesh path until it emits visibility IDs directly.
    XMMATRIX model = XMMatrixIdentity();
    if (!visibilityExtensionsOnly || (scene.useMeshTerrain && g_terrain.supported)) {
    model = (!scene.useMeshTerrain || !g_terrain.supported) &&
        g_stressTestMode
        ? XMMatrixScaling(6.4f, 1.0f, 6.4f)
        : XMMatrixIdentity();
    shader.SetMatrices(model, view, proj, lightSpace);
    if (scene.useMeshTerrain && g_terrain.supported) {
        shader.SetTerrainMaterial();
    } else if (floorMaterial && floorMaterial->baseColorTexture) {
        shader.SetObjectMaterial(scene.floor.color,
                                 true,
                                 floorMaterial->normalTexture != nullptr,
                                 floorMaterial->metallicFactor,
                                 floorMaterial->roughnessFactor,
                                 floorMaterial->baseColorTexture.Get(),
                                 floorMaterial->normalTexture.Get(),
                                 floorMaterial->metallicRoughnessTexture.Get(),
                                 floorMaterial->roughnessOnlyTexture,
                                 1.0f, false,
                                 floorMaterial.get());   // cached: never changes
    } else {
        shader.SetObjectColor(scene.floor.color);
    }
    if (scene.useMeshTerrain && g_terrain.supported) {
        // Use the shared island-builder params so the drawn mesh matches the
        // island size/extent that foliage, collision and GI already read from
        // CurrentTerrainParams(). Building a fresh Params{} here ignored the
        // level's islandScale/extent, so the mesh never grew with the slider.
        TerrainRendererDX12::Params terrainParams = CurrentTerrainParams();
        terrainParams.heightScale = scene.terrainHeightScale;
        g_terrain.Draw(shader, terrainParams);
        // Terrain used the mesh pipeline; restore the IA pipeline for the
        // raster draws that follow (same pattern as imported-model draws).
        g_dx12.commandList->SetPipelineState(
            shader.GetPipelineState(scene.wireframeMode));
    } else {
        DrawPlane(geo);
    }
    shader.NextDrawCall();
    }

    // Cube 1 - draw the imported model if loaded, else fall back to the procedural cube.
    // The model is its own multi-meter scene (not a unit cube), so place it directly
    // on the floor at the origin rather than reusing cube1's small transform.
    if (!g_emptyLevelMode && crateModel && g_showH2Model) {
        DrawSceneNode(crateModel, shader, XMMatrixIdentity(), view, proj,
            lightSpace, visibilityExtensionsOnly);
        // Imported model used the mesh pipeline. Restore IA pipeline for
        // procedural objects that follow it.
        shader.Use(scene.wireframeMode);
    }

    // Separate destructible brick wall beside the house.
    g_destructionCulledThisFrame = 0;
    g_destructionBatchesThisFrame = 0;
    g_destructionChunksSubmittedThisFrame = 0;
    if (scene.useDestruction && g_destruction.IsInitialized()) {
        // ~588 chunk draws when both houses stand; the frustum test drops the
        // ones behind the camera before any CBV write or draw is recorded.
        XMFLOAT4 frustum[6];
        BuildFrustumPlanes(view * proj, frustum);
        const auto& batches = g_destruction.GetRenderBatches();
        if (!batches.empty()) {
            for (const DestructionRenderBatch& batch : batches) {
                if (batch.sphereRadius > 0.0f &&
                    !SphereVisible(frustum, batch.sphereCenter, batch.sphereRadius)) {
                    g_destructionCulledThisFrame += batch.chunkCount;
                    continue;
                }
                DrawSceneNode(batch.colourNode, shader,
                    XMLoadFloat4x4(&batch.transform), view, proj, lightSpace,
                    visibilityExtensionsOnly);
                ++g_destructionBatchesThisFrame;
                g_destructionChunksSubmittedThisFrame += batch.chunkCount;
            }
        }
        for (const DestructionRenderItem& item : g_destruction.GetRenderItems()) {
                if (item.sphereRadius > 0.0f &&
                    !SphereVisible(frustum, item.sphereCenter, item.sphereRadius)) {
                    ++g_destructionCulledThisFrame;
                    continue;
                }
                DrawSceneNode(item.node, shader, XMLoadFloat4x4(&item.transform),
                    view, proj, lightSpace, visibilityExtensionsOnly);
                ++g_destructionBatchesThisFrame;
                ++g_destructionChunksSubmittedThisFrame;
        }
        shader.Use(scene.wireframeMode);
        for (const RagdollRenderItem& item : g_destruction.GetRagdollRenderItems()) {
            if (item.sphereRadius > 0.0f &&
                !SphereVisible(frustum, item.sphereCenter, item.sphereRadius)) continue;
            shader.SetMatrices(XMLoadFloat4x4(&item.transform), view, proj, lightSpace);
            shader.SetObjectColor(item.color);
            // Physics remains box-based. Capsules provide continuous limbs and
            // torso; only the head uses a sphere.
            if (item.shape == 2) DrawSphere(geo);
            else if (item.shape == 1) DrawCapsule(geo);
            else DrawCube(geo);
            shader.NextDrawCall();
        }
        // Hover enemies carry the same imported AK as the player. Gun transforms
        // are aimed independently while ragdoll limbs trail under physics.
        for (const EnemyGunRenderItem& gun : g_destruction.GetEnemyGunRenderItems()) {
            const XMMATRIX xf = XMLoadFloat4x4(&gun.transform);
            const XMFLOAT3 gunCenter(gun.transform._41, gun.transform._42, gun.transform._43);
            if (!SphereVisible(frustum, gunCenter, 1.0f)) continue;
            if (GunModel::Loaded()) {
                DrawMeshAt(GunModel::Mesh(), shader, xf, view, proj, lightSpace, true);
                shader.Use(scene.wireframeMode);
            } else {
                shader.SetMatrices(XMMatrixScaling(0.22f, 0.18f, 1.35f) * xf,
                                   view, proj, lightSpace);
                shader.SetObjectColor(XMFLOAT3(0.06f, 0.055f, 0.05f));
                DrawCube(geo);
                shader.NextDrawCall();
            }
        }
    }

    // Palm trees: trunk sections and fronds are boxes driven by physics bodies,
    // standing or toppling, so they all draw through the ordinary cube path.
    if (!g_emptyLevelMode && g_trees.IsInitialized()) {
        shader.Use(scene.wireframeMode);
        for (const TreeItem& item : g_trees.GetItems()) {
            if (item.crown && !includeGrass) continue;
            const XMMATRIX xf = XMLoadFloat4x4(&item.transform);
            // If the real palm model loaded, each physics box carries the identity
            // of a model slice (a trunk segment or the crown); draw that slice's
            // geometry at the box's transform. Otherwise the item is a plain box.
            std::shared_ptr<SceneMesh> slice;
            if (item.crown) slice = PalmModel::Crown();
            else if (item.segment >= 0 && item.segment < (int)PalmModel::TrunkSlices().size())
                slice = PalmModel::TrunkSlices()[item.segment].mesh;

            if (slice) {
                DrawMeshAt(slice, shader, xf, view, proj, lightSpace, false,
                    visibilityExtensionsOnly, nullptr, item.palmWindRoot);
            } else {
                if (visibilityExtensionsOnly) continue;
                shader.Use(scene.wireframeMode);
                shader.SetMatrices(xf, view, proj, lightSpace);
                shader.SetObjectColor(item.color);
                DrawCube(geo);
                shader.NextDrawCall();
            }
        }
        // A palm slice draw may have left a non-cube pipeline bound.
        shader.Use(scene.wireframeMode);
    }

    // Dandelion cards use the regular lit foliage material, but share one model and
    // one mesh-shader dispatch. CPU work is limited to cheap distance/frustum
    // tests; no draw call is emitted per plant when mesh shaders are enabled.
    if (!g_emptyLevelMode && g_dandelionModel && !g_dandelionInstances.empty()) {
        XMFLOAT4 dandelionFrustum[6];
        BuildFrustumPlanes(view * proj, dandelionFrustum);
        static std::vector<XMMATRIX> visibleDandelions;
        visibleDandelions.clear();
        visibleDandelions.reserve(g_dandelionInstances.size());
        const float maxDandelionDistance = g_grass.IsInitialized()
            ? g_grass.DrawDistance() : 28.0f;
        for (const DandelionInstance& dandelion : g_dandelionInstances) {
            const float dx = dandelion.center.x - scene.camera.Position.x;
            const float dz = dandelion.center.z - scene.camera.Position.z;
            const float range = maxDandelionDistance + dandelion.radius;
            if (dx * dx + dz * dz > range * range ||
                !SphereVisible(dandelionFrustum, dandelion.center, dandelion.radius) ||
                g_grass.RuntimeExcluded(
                    dandelion.center.x, dandelion.center.z,
                    dandelion.radius)) continue;
            visibleDandelions.push_back(XMLoadFloat4x4(&dandelion.transform));
        }
        DrawSceneNodeInstances(g_dandelionModel, shader, visibleDandelions,
                               view, proj, lightSpace);
        shader.Use(scene.wireframeMode);
    }

    // Grass: an INSTANCED draw of one 8-vertex blade, bent in the wind by its own
    // vertex shader. This is how a real engine draws foliage, and it is the third
    // version -- the first re-simulated every blade on the CPU each frame (~11 ms,
    // more than the whole rest of the scene), and the second made the geometry
    // static but still pushed 1.4M unique vertices through the vertex fetch.
    //
    // Two things keep it cheap. The mesh is a single blade template drawn once per
    // blade, so the per-blade data is read once from a structured buffer instead of
    // being duplicated across eight vertices. And the field is submitted a patch at
    // a time, so distant grass is not drawn at all -- shrinking blades away in the
    // shader is not enough, because a zero-height blade still costs vertex shading
    // and triangle setup.
    //
    // Blades are built in world space, hence the identity model matrix. Opaque, so
    // this lands before the transparent water below or it would sort wrong.
    if (includeGrass && !g_emptyLevelMode && g_grass.IsInitialized() && shader.GetGrassPipelineState()) {
        // The shader fades blades with distance, and the cull below needs it too.
        g_grass.SetViewer(scene.camera.Position);

        static std::vector<GrassField::DrawRange> grassRanges;
        g_grass.GetVisible(grassRanges);

        const D3D12_VERTEX_BUFFER_VIEW& gvbv = g_grass.GetVBV();
        const D3D12_GPU_VIRTUAL_ADDRESS ginst = g_grass.GetInstanceBufferAddress();
        if (!grassRanges.empty() && gvbv.BufferLocation && ginst) {
            const D3D12_INDEX_BUFFER_VIEW& gibv = g_grass.GetIBV();

            shader.SetMatrices(XMMatrixIdentity(), view, proj, lightSpace);
            // The blades are untextured, so this colour goes into the lighting
            // as-is -- i.e. it is already LINEAR. The ground beneath them samples
            // Grass004's albedo and decodes it with pow(x, 2.2) first, so matching
            // the two by eye in sRGB is what left the blades looking pale and
            // yellow against the turf. This is that texture's mean colour taken
            // through the same decode: sRGB (96, 108, 48) -> linear.
            shader.SetObjectMaterial(XMFLOAT3(0.117f, 0.152f, 0.025f), false, false,
                                     0.0f, 0.85f, nullptr, nullptr, nullptr);

            // Wind parameters as root constants (b6), and the per-blade data as a
            // root SRV (t6). Both slots exist for the terrain mesh-shader path and
            // are unused by the raster path, so the grass borrows them and needs no
            // root-signature change of its own. (Terrain and grass never draw in the
            // same call, so sharing the slots is safe.)
            GrassField::Params gp = g_grass.GetParams(
                scene.EffectiveCameraFOV(), static_cast<float>(g_dx12.screenHeight));
            static_assert(sizeof(GrassField::Params) == 13 * sizeof(UINT),
                          "GrassParams must match the 13 root constants at b6");
            g_dx12.commandList->SetGraphicsRoot32BitConstants(8, 13, &gp, 0);
            g_dx12.commandList->SetGraphicsRootShaderResourceView(9, ginst);

            g_dx12.commandList->SetPipelineState(
                shader.GetGrassPipelineState());
            g_dx12.commandList->IASetVertexBuffers(0, 1, &gvbv);
            g_dx12.commandList->IASetIndexBuffer(&gibv);
            g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            // Every patch draws the same template with the same material. The only
            // thing that changes is which run of blades it reads -- and that has to
            // go in as a root constant, because SV_InstanceID ignores
            // StartInstanceLocation for a structured buffer (see grass_vs.hlsl).
            for (const GrassField::DrawRange& r : grassRanges) {
                g_dx12.commandList->SetGraphicsRoot32BitConstants(8, 1, &r.firstInstance, 7);
                g_dx12.commandList->DrawIndexedInstanced(
                    GrassField::IndexCount(), r.instanceCount, 0, 0, 0);
            }
            shader.NextDrawCall();

            // Hand the pipeline back to the ordinary object shader for what follows.
            shader.Use(scene.wireframeMode);
        }
    }

    // The sea around the island. Drawn before the pool because it is the farther
    // transparent surface, and blended surfaces have to go back-to-front.
    if (!g_emptyLevelMode && g_ocean.IsInitialized()) {
        const D3D12_VERTEX_BUFFER_VIEW& ovbv = g_ocean.UpdateAndGetVBV(g_dx12.frameIndex);
        const D3D12_INDEX_BUFFER_VIEW& oibv = g_ocean.GetIBV();
        const UINT oceanIndices = g_ocean.GetIndexCount();
        if (oceanIndices && ovbv.BufferLocation) {
            shader.UseTransparent();
            shader.SetMatrices(XMMatrixIdentity(), view, proj, lightSpace);
            // Deeper and less transparent than the pool: open water reads darker.
            shader.SetObjectMaterial(XMFLOAT3(0.03f, 0.18f, 0.38f), false, false,
                                     0.04f, 0.05f, nullptr, nullptr, nullptr, false, 0.80f);
            g_dx12.commandList->IASetVertexBuffers(0, 1, &ovbv);
            g_dx12.commandList->IASetIndexBuffer(&oibv);
            g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_dx12.commandList->DrawIndexedInstanced(oceanIndices, 1, 0, 0, 0);
            shader.NextDrawCall();
        }
        shader.Use(scene.wireframeMode);
    }

    // Water pool: floating crates + any debris (opaque) drawn first, then the
    // animated, undulating water surface on top so the floaters show through it.
    if (!g_emptyLevelMode && g_water.IsInitialized()) {
        shader.Use(scene.wireframeMode);
        if (!visibilityExtensionsOnly) for (const WaterFloaterItem& item : g_water.GetFloaterItems()) {
            shader.SetMatrices(XMLoadFloat4x4(&item.transform), view, proj, lightSpace);
            shader.SetObjectColor(item.color);
            DrawCube(geo);
            shader.NextDrawCall();
        }
        // Wave surface: positions/normals recomputed on the CPU each frame into a
        // per-frame upload buffer, drawn as a translucent glossy sheet so the
        // moving swell catches the light.
        const D3D12_VERTEX_BUFFER_VIEW& wvbv = g_water.UpdateAndGetVBV(g_dx12.frameIndex);
        const D3D12_INDEX_BUFFER_VIEW& wibv = g_water.GetIBV();
        const UINT waterIndices = g_water.GetIndexCount();
        if (waterIndices && wvbv.BufferLocation) {
            shader.UseTransparent();
            shader.SetMatrices(XMMatrixIdentity(), view, proj, lightSpace);  // verts are world-space
            shader.SetObjectMaterial(XMFLOAT3(0.06f, 0.30f, 0.55f), false, false,
                                     0.05f, 0.06f, nullptr, nullptr, nullptr, false, 0.62f);
            g_dx12.commandList->IASetVertexBuffers(0, 1, &wvbv);
            g_dx12.commandList->IASetIndexBuffer(&wibv);
            g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_dx12.commandList->DrawIndexedInstanced(waterIndices, 1, 0, 0, 0);
            shader.NextDrawCall();
        }
        shader.Use(scene.wireframeMode);
    }

    // Cube 2
    if (!visibilityExtensionsOnly && !g_emptyLevelMode && scene.cube2.visible) {
        model = scene.cube2.GetModelMatrix();
        shader.SetMatrices(model, view, proj, lightSpace);
        shader.SetObjectColor(scene.cube2.color);
        DrawCube(geo);
        shader.NextDrawCall();
    }

    if (!g_emptyLevelMode && g_humveeModel) {
        if (visibilityExtensionsOnly) {
            DrawSceneNode(g_humveeModel, shader, HumveeWorldMatrix(),
                view, proj, lightSpace, true);
        } else if (!staticBatches.Submit(g_humveeModel, HumveeWorldMatrix()))
            DrawSceneNode(g_humveeModel, shader, HumveeWorldMatrix(),
                view, proj, lightSpace);
        if (g_stressTestMode) {
            if (visibilityExtensionsOnly)
                DrawSceneNode(g_humveeModel, shader, SecondaryHumveeWorldMatrix(),
                    view, proj, lightSpace, true);
            else if (!staticBatches.Submit(g_humveeModel, SecondaryHumveeWorldMatrix()))
                DrawSceneNode(g_humveeModel, shader, SecondaryHumveeWorldMatrix(),
                    view, proj, lightSpace);
        }
    }

    for (const PrefabRenderBatch& batch : g_prefabRenderBatches) {
        if (!batch.model) continue;
        for (const XMMATRIX& transform : batch.transforms)
            DrawSceneNode(batch.model, shader, transform,
                          view, proj, lightSpace, visibilityExtensionsOnly);
        shader.Use(scene.wireframeMode);
    }

    if (!g_emptyLevelMode && g_helicopterModel && scene.showHelicopter) {
        // Rotor child transforms and aggressively simplified cockpit/fuselage
        // meshlets make the generic static-mesh bounds unsuitable here. A bad
        // amplification-shader reject made the loaded aircraft disappear. The
        // aircraft already has an import-time LOD, so keep this one model on the
        // conventional raster path.
        const bool meshShadersWereEnabled = g_useMeshShader;
        g_useMeshShader = false;
        DrawSceneNode(g_helicopterModel, shader, HelicopterWorldMatrix(),
                      view, proj, lightSpace, visibilityExtensionsOnly);
        if (g_stressTestMode)
            DrawSceneNode(g_helicopterModel, shader, SecondaryHelicopterWorldMatrix(),
                          view, proj, lightSpace, visibilityExtensionsOnly);
        g_useMeshShader = meshShadersWereEnabled;
    }

    // Authored shootable barrel FBX. Its pivot is at the base, while gameplay
    // stores barrel.position at collision center 0.75 m above ground.
    if (!g_emptyLevelMode && g_explosiveBarrelModel) {
        for (const ExplosiveBarrel& barrel : scene.explosiveBarrels) {
            if (!barrel.active) continue;
            const XMMATRIX barrelTransform = XMMatrixTranslation(
                barrel.position.x, barrel.position.y - 0.75f, barrel.position.z);
            if (visibilityExtensionsOnly)
                DrawSceneNode(g_explosiveBarrelModel, shader, barrelTransform,
                    view, proj, lightSpace, true);
            else if (!staticBatches.Submit(g_explosiveBarrelModel, barrelTransform))
                DrawSceneNode(g_explosiveBarrelModel, shader, barrelTransform,
                    view, proj, lightSpace);
        }
    } else if (!g_emptyLevelMode) for (const ExplosiveBarrel& barrel : scene.explosiveBarrels) {
        if (barrel.active) {
            model = XMMatrixScaling(1.6f, 1.5f, 1.6f) *
                    XMMatrixTranslation(barrel.position.x, barrel.position.y,
                                        barrel.position.z);
            shader.SetMatrices(model, view, proj, lightSpace);
            shader.SetObjectMaterial(XMFLOAT3(0.68f, 0.035f, 0.025f), false, false,
                                     0.68f, 0.28f, nullptr, nullptr, nullptr);
            DrawCapsule(geo);
            shader.NextDrawCall();
        }
    }

    // Projectiles. Grenades: dark spheres. Bullets: bright tracer rounds -- a
    // thin streak stretched along the flight direction, glowing hot so it reads
    // like a real tracer whipping downrange.
    // Imported helicopter/barrel draws may leave a mesh-shader PSO and its
    // meshlet SRVs bound. Procedural projectiles use the input assembler; without
    // this restore a grenade can execute the last rotor mesh state instead.
    shader.Use(scene.wireframeMode);
    for (auto& p : scene.projectiles) {
        if (!p.active) continue;
        if (p.rocket) {
            XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&p.direction));
            XMVECTOR up0 = fabsf(XMVectorGetY(fwd)) > 0.95f ? XMVectorSet(1, 0, 0, 0)
                                                            : XMVectorSet(0, 1, 0, 0);
            XMVECTOR right = XMVector3Normalize(XMVector3Cross(up0, fwd));
            XMVECTOR up = XMVector3Cross(fwd, right);
            XMMATRIX basis = XMMatrixIdentity();
            basis.r[0] = XMVectorSetW(right, 0.0f);
            basis.r[1] = XMVectorSetW(up, 0.0f);
            basis.r[2] = XMVectorSetW(fwd, 0.0f);
            basis.r[3] = XMVectorSet(p.position.x, p.position.y, p.position.z, 1.0f);
            if (GunModel::RPGRocketMesh()) {
                DrawMeshAt(GunModel::RPGRocketMesh(), shader, basis,
                           view, proj, lightSpace);
            } else {
                model = XMMatrixScaling(0.09f, 0.09f, 0.52f) * basis;
                shader.SetMatrices(model, view, proj, lightSpace);
                shader.SetObjectMaterial(XMFLOAT3(0.18f, 0.20f, 0.12f), false, false,
                                         0.72f, 0.25f, nullptr, nullptr, nullptr);
                DrawCube(geo);
                shader.NextDrawCall();
            }
            continue;
        }
        if (p.grenade) {
            model = XMMatrixScaling(scene.projectileScale * 1.6f, scene.projectileScale * 1.6f,
                                    scene.projectileScale * 1.6f) *
                    XMMatrixTranslation(p.position.x, p.position.y, p.position.z);
            shader.SetMatrices(model, view, proj, lightSpace);
            shader.SetObjectMaterial(XMFLOAT3(0.10f, 0.12f, 0.08f), false, false,
                                     0.6f, 0.5f, nullptr, nullptr, nullptr);
            DrawSphere(geo);
            shader.NextDrawCall();
            continue;
        }

        // Orthonormal basis with local +Z along the bullet's travel direction.
        XMVECTOR fwd = XMLoadFloat3(&p.direction);
        if (XMVectorGetX(XMVector3LengthSq(fwd)) < 1e-6f) fwd = XMVectorSet(0, 0, 1, 0);
        fwd = XMVector3Normalize(fwd);
        XMVECTOR up0 = fabsf(XMVectorGetY(fwd)) > 0.95f ? XMVectorSet(1, 0, 0, 0)
                                                        : XMVectorSet(0, 1, 0, 0);
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(up0, fwd));
        XMVECTOR up    = XMVector3Cross(fwd, right);
        // Streak covers distance travelled during this rendered frame. Clamp its
        // exposure length so high refresh rates still show it and frame hitches do
        // not produce a giant laser beam.
        const XMVECTOR current = XMLoadFloat3(&p.position);
        const XMVECTOR previous = XMLoadFloat3(&p.previousPosition);
        const float moved = XMVectorGetX(XMVector3Length(current - previous));
        const float len = (std::min)(5.0f, (std::max)(1.2f, moved));
        const XMVECTOR center = current - fwd * (len * 0.5f);

        XMMATRIX basis = XMMatrixIdentity();
        basis.r[0] = XMVectorSetW(right, 0.0f);
        basis.r[1] = XMVectorSetW(up, 0.0f);
        basis.r[2] = XMVectorSetW(fwd, 0.0f);
        basis.r[3] = XMVectorSetW(center, 1.0f);

        // Warm translucent envelope first, then a needle-thin white-hot core.
        // Additive unlit passes stay bright in shadow and stop looking like an
        // orange physical box tumbling through the scene.
        const float haloR = (std::max)(0.012f, scene.projectileScale * 0.16f);
        shader.UseAdditive();
        model = XMMatrixScaling(haloR * 2.0f, haloR * 2.0f, len) * basis;
        shader.SetMatrices(model, view, proj, lightSpace);
        const XMFLOAT3 halo = p.hostile ? XMFLOAT3(5.0f, 0.08f, 0.015f)
                                        : XMFLOAT3(4.0f, 0.55f, 0.025f);
        shader.SetEmissiveMaterial(halo, 0.24f);
        DrawCube(geo);
        shader.NextDrawCall();

        const float coreR = haloR * 0.38f;
        model = XMMatrixScaling(coreR * 2.0f, coreR * 2.0f, len * 0.92f) * basis;
        shader.SetMatrices(model, view, proj, lightSpace);
        const XMFLOAT3 core = p.hostile ? XMFLOAT3(9.0f, 0.8f, 0.18f)
                                        : XMFLOAT3(9.0f, 3.2f, 0.45f);
        shader.SetEmissiveMaterial(core, 0.92f);
        DrawCube(geo);
        shader.NextDrawCall();
        shader.Use(scene.wireframeMode);
    }

    // Opaque sparks stay in Forward. Transparent smoke/blood runs after the
    // separate skinned-enemy pass via RenderImpactBillboards().
    shader.Use(scene.wireframeMode);
    for (auto& sp : scene.impactParticles) {
        if (!sp.spark) continue;
        const float fade = sp.life / sp.maxLife;
        model = XMMatrixScaling(sp.size, sp.size, sp.size) *
                XMMatrixTranslation(sp.position.x, sp.position.y, sp.position.z);
        shader.SetMatrices(model, view, proj, lightSpace);
        const float b = 0.6f + 0.4f * fade;
        shader.SetObjectColor(XMFLOAT3(sp.color.x * b, sp.color.y * b, sp.color.z * b));
        DrawCube(geo);
        shader.NextDrawCall();
    }

    // Gun: the AK47 model, drawn in the gun's local space (+Z down the barrel)
    // and placed in front of the camera. If the FBX did not load, fall back to
    // the boxed carbine below so the player still sees a weapon.
    if (scene.gun.visible && !scene.sniperScopeActive) {
        const XMMATRIX gunBase = scene.GetGunBaseMatrix();
        const float S = scene.GunModelScale();

        if (GunModel::PlayerLoaded()) {
            // The weapon sits at a fixed spot in front of the camera: the model
            // is normalised with its origin at the rear of the weapon, so shift
            // it back and down into the same pocket of screen space the boxed M4
            // occupied, then scale to the gun's on-screen size.
            //
            // The weapon deliberately does NOT ride the character's hand bone.
            // Hanging it off the hand meant any error in the body's placement
            // moved the gun too, so the one thing that was framed correctly
            // stopped being so. Keeping the gun fixed makes it the reference the
            // arms are aligned against, rather than the other way round.
            // Base placement: the pocket of screen space the weapon was tuned
            // in, before any hand motion is added.
            XMMATRIX xf =
                XMMatrixScaling(S, S, S) *
                XMMatrixTranslation(0.0f, -0.10f * S, -0.44f * S) *
                gunBase;
            // With the idle playing, ride the trigger hand so the rifle stays in
            // the grip instead of hanging still while the arms breathe past it.
            // The follow transform is a delta from the reference pose, so it
            // composes on top of the tuned placement rather than replacing it.
            XMMATRIX handFollow;
            if (ArmsModel::WeaponFollowTransform(handFollow, S))
                xf = XMMatrixScaling(S, S, S) *
                     XMMatrixTranslation(0.0f, -0.10f * S, -0.44f * S) *
                     handFollow * gunBase;
            shader.Use(scene.wireframeMode);
            DrawMeshAt(GunModel::PlayerMesh(), shader, xf, view, proj, lightSpace);
            shader.Use(scene.wireframeMode);

            // The body hangs off the same base transform as the weapon, so it
            // inherits recoil, the ADS slide and the hip offset for free. This
            // one is GPU-skinned and animating, so it owns its own draw.
            ArmsModel::Draw(shader, gunBase, view, proj, lightSpace, S);
            shader.Use(scene.wireframeMode);
        } else {
            // Fallback: the old M4-style carbine, built from boxed parts.
            // Gunmetal / polymer palette.
            const XMFLOAT3 metal(0.14f, 0.14f, 0.16f);   // receiver, barrel
            const XMFLOAT3 poly (0.09f, 0.10f, 0.11f);   // handguard, stock, grip
            const XMFLOAT3 mag  (0.11f, 0.12f, 0.13f);   // magazine
            const XMFLOAT3 iron (0.05f, 0.05f, 0.06f);   // sights, muzzle

            struct Part { XMFLOAT3 c, h, col; };
            // center (x,y,z), half-extents (x,y,z), colour -- local units.
            const Part parts[] = {
                // Upper + lower receiver (main body).
                {{0.00f,  0.00f,  0.05f}, {0.055f, 0.075f, 0.26f}, metal},
                // Handguard around the barrel (forward, slightly fatter).
                {{0.00f, -0.01f,  0.42f}, {0.05f,  0.055f, 0.20f}, poly},
                // Barrel poking out of the handguard.
                {{0.00f,  0.01f,  0.66f}, {0.018f, 0.018f, 0.10f}, metal},
                // Flash hider / muzzle tip.
                {{0.00f,  0.01f,  0.78f}, {0.026f, 0.026f, 0.03f}, iron},
                // Magazine, canted slightly forward under the receiver.
                {{0.00f, -0.20f,  0.02f}, {0.04f,  0.13f,  0.055f}, mag},
                // Pistol grip, behind the mag.
                {{0.00f, -0.15f, -0.16f}, {0.035f, 0.10f,  0.04f}, poly},
                // Buffer tube + stock, to the rear.
                {{0.00f,  0.00f, -0.24f}, {0.03f,  0.05f,  0.10f}, metal},
                {{0.00f, -0.02f, -0.38f}, {0.05f,  0.085f, 0.06f}, poly},
                // Optic/carry-handle rail on top.
                {{0.00f,  0.10f,  0.02f}, {0.03f,  0.03f,  0.20f}, iron},
                // Front sight post.
                {{0.00f,  0.11f,  0.52f}, {0.014f, 0.05f,  0.02f}, iron},
                // Charging-handle bump at the back top.
                {{0.00f,  0.09f, -0.14f}, {0.028f, 0.024f, 0.05f}, iron},
            };

            shader.Use(scene.wireframeMode);
            for (const Part& p : parts) {
                model = XMMatrixScaling(p.h.x * 2.0f * S, p.h.y * 2.0f * S, p.h.z * 2.0f * S) *
                        XMMatrixTranslation(p.c.x * S, p.c.y * S, p.c.z * S) *
                        gunBase;
                shader.SetMatrices(model, view, proj, lightSpace);
                shader.SetObjectMaterial(p.col, false, false, 0.85f, 0.35f,
                                         nullptr, nullptr, nullptr);
            DrawCube(geo);
            shader.NextDrawCall();
        }
    }
    staticBatches.Flush(shader, view, proj, lightSpace);


        // Downloaded CC0 muzzle-flash sheet. One random-looking star frame is
        // enough because real rifle flashes last only a few milliseconds.
        if (scene.muzzleFlashTime > 0.0f && g_muzzleFlashTexture) {
            const XMFLOAT3 muzzle = scene.GetMuzzleWorldPosition();
            const float fade = scene.muzzleFlashTime / scene.muzzleFlashDuration;
            const float size = scene.GunModelScale() * (0.32f + 0.10f * fade);
            const XMVECTOR pos = XMVectorSet(muzzle.x, muzzle.y, muzzle.z, 1.0f);
            const XMMATRIX invRot = XMMatrixTranspose(view);
            const XMVECTOR camRight = XMVectorSetW(invRot.r[0], 0.0f);
            const XMVECTOR camUp = XMVectorSetW(invRot.r[1], 0.0f);
            const XMVECTOR camFwd = XMVectorSetW(invRot.r[2], 0.0f);
            model = XMMATRIX(camRight * size, camUp * size, camFwd * size,
                             XMVectorSetW(pos, 1.0f));
            shader.UseAdditive();
            shader.SetMatrices(model, view, proj, lightSpace);
            shader.SetSmokeMaterial(XMFLOAT3(1.0f, 0.34f, 0.035f), fade,
                                    g_muzzleFlashTexture.Get());
            DrawFlashQuad(geo);
            shader.NextDrawCall();
            shader.Use(scene.wireframeMode);
        }
    }

    // Diagnostic: prove the material-descriptor cache and the destruction
    // early-out are doing what they claim. Written once, a couple of seconds in,
    // so the numbers reflect a settled steady-state frame rather than startup.
    {
        static int frames = 0;
        if (++frames % 300 == 0) {
            if (FILE* f = std::fopen("perf_counters.log", "w")) {
                std::fprintf(f, "srvCreates=%u srvCacheHits=%u destructionRenderItems=%zu destructionRenderBatches=%zu culled=%u batchWorker=%u\n",
                             shader.srvCreatesThisFrame, shader.srvCacheHitsThisFrame,
                             g_destruction.GetRenderItems().size(),
                             g_destruction.GetRenderBatches().size(),
                             g_destructionCulledThisFrame,
                             g_destruction.IsBatchBuildPending() ? 1u : 0u);
                std::fclose(f);
            }
        }
    }

    // Light spheres
    for (int i = 0; i < scene.clusteredRenderer.getTotalLightCount(); i++) {
        PointLightDX12* light = scene.clusteredRenderer.getLight(i);
        if (!light || !light->active) continue;
        model = XMMatrixScaling(0.2f, 0.2f, 0.2f);
        model = model * XMMatrixTranslation(light->position.x, light->position.y, light->position.z);
        shader.SetMatrices(model, view, proj, lightSpace);
        XMFLOAT3 lc(light->color.x * light->intensity,
                     light->color.y * light->intensity,
                     light->color.z * light->intensity);
        shader.SetObjectColor(lc);
        DrawCube(geo);
        shader.NextDrawCall();
    }
}

#endif // FORWARD_RENDERER_H
