#pragma once
#include <vector>
#include <memory>
#include <string>
#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

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

struct SceneMaterial {
    std::string name;
    
    DirectX::XMFLOAT4 baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    bool doubleSided = false;
    
    ComPtr<ID3D12Resource> baseColorTexture;
    ComPtr<ID3D12Resource> metallicRoughnessTexture;
    ComPtr<ID3D12Resource> normalTexture;

    // Keep upload heaps alive until GPU finishes
    std::vector<ComPtr<ID3D12Resource>> uploadHeaps;
};

// Represents a mesh part (subset) with its own material/texture
struct MeshPrimitive {
    std::vector<float> vertices; // Interleaved: Pos(3), Normal(3), Tex(2), Tangent(4) -> 12 floats per vertex
    std::vector<unsigned int> indices;
    int materialIndex = -1;
    std::shared_ptr<SceneMaterial> material;
    
    // DX12 Resources (to be filled by the renderer/loader)
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    ComPtr<ID3D12Resource> meshletBoundsBuffer;
    ComPtr<ID3D12Resource> meshletDescBuffer;
    ComPtr<ID3D12Resource> meshletVertexIndexBuffer;
    ComPtr<ID3D12Resource> meshletTriangleBuffer;
    D3D12_VERTEX_BUFFER_VIEW vbv;
    D3D12_INDEX_BUFFER_VIEW ibv;
    UINT indexCount = 0;
    UINT meshletCount = 0;
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
    
    void UpdateGlobalTransform(const DirectX::XMFLOAT4X4& parentGlobal) {
        UpdateLocalTransform(); // Ensure local is fresh
        
        DirectX::XMMATRIX local = DirectX::XMLoadFloat4x4(&localTransform);
        DirectX::XMMATRIX parent = DirectX::XMLoadFloat4x4(&parentGlobal);
        DirectX::XMMATRIX global = DirectX::XMMatrixMultiply(local, parent);
        
        DirectX::XMStoreFloat4x4(&globalTransform, global);
        
        for (auto& child : children) {
            child->UpdateGlobalTransform(globalTransform);
        }
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
