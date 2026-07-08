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
};

