#pragma once
#include <vector>
#include <memory>
#include <string>
#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>
#include "SkinnedTypes.h"

using Microsoft::WRL::ComPtr;

struct MeshletBoundsDX12 {
    DirectX::XMFLOAT3 boundsMin;
    float padding0;
    DirectX::XMFLOAT3 boundsMax;
    float padding1;
    DirectX::XMFLOAT3 sphereCenter;
    float sphereRadius;
    DirectX::XMFLOAT3 coneAxis;
    float coneCutoff;
};

struct MeshletDescDX12 {
    UINT vertexOffset;
    UINT vertexCount;
    UINT triangleOffset;
    UINT triangleCount;
};

enum class WaterTransparencyMode {
    Automatic,
    BeforeWater,
    AfterWater
};

struct SceneMaterial {
    std::string name;
    
    DirectX::XMFLOAT4 baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
    // glTF emissiveFactor premultiplied by KHR_materials_emissive_strength.
    // Added to the lit result, so it stays visible in shadow (beacons, panel
    // lights). Black by default, which is a no-op in the resolve.
    DirectX::XMFLOAT3 emissiveFactor = { 0.0f, 0.0f, 0.0f };
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    bool doubleSided = false;
    // Automatic compares the primitive and camera against the local water
    // surface. The explicit modes are escape hatches for large meshes that
    // straddle a shoreline and cannot be classified from one bounds centre.
    WaterTransparencyMode waterTransparency =
        WaterTransparencyMode::Automatic;
    // Closed fracture prisms and layered shells can self-occlude against the
    // previous-frame HZB. Keep frustum culling, but skip HZB for those materials.
    bool disableOcclusionCulling = false;
    // Alpha-tested cutout (foliage cards): the pixel shader clips texels whose
    // texture alpha is below threshold. Opt-in because clip() costs early-Z.
    bool alphaCutout = false;
    // glTF alphaMode=BLEND. Kept separate from alphaCutout so texture-driven
    // glass can use blending even when baseColorFactor alpha itself is 1.
    bool alphaBlend = false;
    // Hair/eyelash atlases ship as opaque RGB with white strands on black.
    // Clip from luminance instead of alpha for those cards.
    bool alphaFromLuminance = false;

    bool IsTransparent() const {
        return alphaBlend || baseColorFactor.w < 0.999f;
    }
    float ambientScale = 1.0f;
    float occlusionStrength = 0.0f;
    float normalYSign = 1.0f;
    float viewFillStrength = 0.0f;
    
    ComPtr<ID3D12Resource> baseColorTexture;
    ComPtr<ID3D12Resource> metallicRoughnessTexture;
    bool roughnessOnlyTexture = false;
    ComPtr<ID3D12Resource> normalTexture;

    // Where this material's three texture descriptors (albedo/normal/metal-rough)
    // live in the persistent region of the shared CBV/SRV/UAV heap. They are
    // created the first time the material is drawn and then reused forever -- a
    // material's textures never change after load, so recreating the descriptors
    // every draw was pure waste (~1,764 CreateShaderResourceView calls a frame
    // just for the destructible house's chunks). ~0u means "not cached yet".
    // See ShaderDX12::SetObjectMaterial.
    UINT srvHeapSlot = ~0u;

    // Bindless tier: absolute indices into the separate bindless descriptor
    // heap, valid only while bindlessGeneration matches the allocator's current
    // generation. Unlike srvHeapSlot these are three independent indices, not a
    // 3-descriptor table base, because the bindless heap deduplicates on the
    // texture resource -- two materials sharing an albedo share its descriptor.
    //
    // Generation-stamped rather than cleared on scene teardown: a material can
    // outlive the scene that registered it, and walking every material to clear
    // stale indices is both slower and easy to miss a path in.
    UINT bindlessAlbedoIndex = ~0u;
    UINT bindlessNormalIndex = ~0u;
    UINT bindlessMetalRoughIndex = ~0u;
    UINT bindlessGeneration = 0;

    // Drop every cached texture binding, legacy and bindless alike. Texture
    // overrides must call this rather than clearing srvHeapSlot by hand --
    // missing the bindless fields there leaves the material sampling its old
    // texture through a stale descriptor index.
    void InvalidateTextureBindings() {
        srvHeapSlot = ~0u;
        bindlessAlbedoIndex = ~0u;
        bindlessNormalIndex = ~0u;
        bindlessMetalRoughIndex = ~0u;
        bindlessGeneration = 0;
    }

    // Keep upload heaps alive until GPU finishes
    std::vector<ComPtr<ID3D12Resource>> uploadHeaps;
};

// Represents a mesh part (subset) with its own material/texture
struct MeshPrimitive {
    std::vector<float> vertices; // Interleaved: Pos(3), Normal(3), Tex(2), Tangent(4) -> 12 floats per vertex
    std::vector<unsigned int> indices;
    // Optional persistent identity for each indexed triangle. Destruction
    // geometry carries its original chunk/local-triangle key through later
    // material merges, while ordinary meshes leave this empty and use their
    // current local triangle index. This is temporal metadata only; raster and
    // shading still use the current index buffer and SV_PrimitiveID.
    std::vector<unsigned int> stableTriangleIDs;
    unsigned int stableTriangleNamespace = 0;
    int materialIndex = -1;
    std::shared_ptr<SceneMaterial> material;

    // Optional per-vertex skin attributes (one SkinVertex per interleaved
    // vertex, same order). Empty for static meshes; populated by the skinned
    // importer. Uploaded to skinBuffer and bound at t13 when skinning is on.
    std::vector<SkinVertex> skin;

    // DX12 Resources (to be filled by the renderer/loader)
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    ComPtr<ID3D12Resource> meshletBoundsBuffer;
    ComPtr<ID3D12Resource> meshletDescBuffer;
    ComPtr<ID3D12Resource> meshletVertexIndexBuffer;
    ComPtr<ID3D12Resource> meshletTriangleBuffer;
    ComPtr<ID3D12Resource> skinBuffer; // StructuredBuffer<SkinVertex>, bound at t13
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    UINT indexCount = 0;
    UINT meshletCount = 0;
    UINT skinVertexCount = 0; // >0 when this primitive carries skin data
    // Set only after visibility geometry registration succeeds. Forward hybrid
    // filtering checks this so capacity failures never make geometry disappear.
    UINT visibilityMeshID = UINT_MAX;
    // Geometry that is rebuilt during play rather than living for the whole
    // level -- destruction re-merges its chunk batches on every fracture. The
    // visibility buffer recycles these mesh slots when the primitive is
    // retired; permanent geometry keeps its slot, since a stale draw call
    // pointing at a reused permanent slot would sample the wrong vertices.
    bool transientGeometry = false;
    // CPU-side local bounds retained for conservative culling when this
    // primitive must use the conventional IA raster fallback.
    DirectX::XMFLOAT3 boundsMin = {};
    DirectX::XMFLOAT3 boundsMax = {};
    bool boundsValid = false;
};

struct SceneMesh {
    std::string name;
    std::vector<MeshPrimitive> primitives;
};

class SceneNode {
public:
    std::string name;
    
    // Transforms
    DirectX::XMFLOAT3 translation = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f }; // Quaternion
    DirectX::XMFLOAT3 scale = { 1.0f, 1.0f, 1.0f };
    
    DirectX::XMFLOAT4X4 localTransform;
    DirectX::XMFLOAT4X4 globalTransform;
    
    std::shared_ptr<SceneMesh> mesh;
    
    SceneNode* parent = nullptr;
    std::vector<std::shared_ptr<SceneNode>> children;
    
    SceneNode(const std::string& n = "Node") : name(n) {
        DirectX::XMStoreFloat4x4(&localTransform, DirectX::XMMatrixIdentity());
        DirectX::XMStoreFloat4x4(&globalTransform, DirectX::XMMatrixIdentity());
    }
    
    void AddChild(std::shared_ptr<SceneNode> child) {
        child->parent = this;
        children.push_back(child);
    }
    
    void UpdateLocalTransform() {
        DirectX::XMVECTOR t = DirectX::XMLoadFloat3(&translation);
        DirectX::XMVECTOR r = DirectX::XMLoadFloat4(&rotation);
        DirectX::XMVECTOR s = DirectX::XMLoadFloat3(&scale);
        
        DirectX::XMMATRIX m = DirectX::XMMatrixAffineTransformation(s, DirectX::XMVectorZero(), r, t);
        DirectX::XMStoreFloat4x4(&localTransform, m);
    }
    
    void UpdateGlobalTransform(const DirectX::XMFLOAT4X4& parentGlobal,
                               int depth = 0) {
        // Depth guard: a malformed/cyclic imported model (a child that also
        // appears up its own ancestor chain) would recurse until the thread
        // stack overflows and crashes in a deep memcpy. Real hierarchies are
        // shallow; 256 is far beyond any legitimate model.
        if (depth > 256) return;
        UpdateLocalTransform(); // Ensure local is fresh

        DirectX::XMMATRIX local = DirectX::XMLoadFloat4x4(&localTransform);
        DirectX::XMMATRIX parent = DirectX::XMLoadFloat4x4(&parentGlobal);
        DirectX::XMMATRIX global = DirectX::XMMatrixMultiply(local, parent);

        DirectX::XMStoreFloat4x4(&globalTransform, global);

        for (auto& child : children) {
            child->UpdateGlobalTransform(globalTransform, depth + 1);
        }
    }

    // Recomputes this subtree treating the node as its own root. Prefer this to
    // UpdateGlobalTransform(self->localTransform): that idiom passes the node's
    // own local as if it were the parent's global, so the local transform gets
    // applied twice and every child inherits the doubled basis. It only looks
    // harmless because most importer roots happen to be identity.
    void RefreshHierarchy() {
        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        UpdateGlobalTransform(identity);
    }
};

class SceneGraph {
    std::shared_ptr<SceneNode> root;
    
public:
    SceneGraph() {
        root = std::make_shared<SceneNode>("Root");
    }
    
    std::shared_ptr<SceneNode> GetRoot() { return root; }
    
    void Update() {
        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        root->UpdateGlobalTransform(identity);
    }
};
