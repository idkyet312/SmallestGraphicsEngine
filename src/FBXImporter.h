#pragma once
#include "SceneGraph.h"
#include <string>

class FBXImporter {
public:
    static std::shared_ptr<SceneNode> Load(
        const std::string& filepath,
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
        float uniformScale = 0.1f,
        bool splitIntoDestructibleBoards = false,
        bool loadMaterials = true,
        bool diffuseAndNormalOnly = false);
};
