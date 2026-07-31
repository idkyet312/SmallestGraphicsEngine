#pragma once

#include "SceneGraph.h"

#include <d3d12.h>
#include <filesystem>
#include <memory>
#include <string>
#include <wrl/client.h>

class CookedAssetLoader {
public:
    static std::filesystem::path FindForSource(
        const std::filesystem::path& source);

    static std::shared_ptr<SceneNode> LoadForSource(
        const std::filesystem::path& source,
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
        std::string* error = nullptr);

    static std::shared_ptr<SceneNode> Load(
        const std::filesystem::path& cookedPath,
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
        std::string* error = nullptr);

    static bool LoadAnimationsForSource(
        const std::filesystem::path& source,
        const Skeleton& skeleton,
        std::vector<AnimationClip>& clips,
        std::string* error = nullptr);
};
