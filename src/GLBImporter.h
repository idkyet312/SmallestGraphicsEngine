#pragma once
#include <string>
#include <memory>
#include <vector>
#include "SceneGraph.h"
#include <d3d12.h>
#include <wrl/client.h>

class GLBImporter {
public:
    static std::shared_ptr<SceneNode> LoadGLB(const std::string& filepath, Microsoft::WRL::ComPtr<ID3D12Device> device, Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList);
    static Microsoft::WRL::ComPtr<ID3D12Resource> LoadTextureFromFile(const std::string& filepath, Microsoft::WRL::ComPtr<ID3D12Device> device, Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadHeaps);

    // Collapses an entire (static) node hierarchy into one SceneNode per unique
    // material, each holding a single merged draw call. Each primitive's
    // vertices are baked from model-root-relative space (the node's transform
    // is folded into the vertex positions/normals/tangents), so the returned
    // root can still be moved/animated as a rigid whole exactly like the
    // original multi-node tree. Intended for static imports where per-node
    // animation isn't needed, to cut draw-call count for the Colour Pass.
    static std::shared_ptr<SceneNode> MergeSceneByMaterial(const std::shared_ptr<SceneNode>& modelRoot, Microsoft::WRL::ComPtr<ID3D12Device> device);
};

