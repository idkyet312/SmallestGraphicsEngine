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

    // FNV-1a over a buffer, and over a file's contents. Exposed because the
    // collision-mesh cache keys its trees on exactly the same source hash this
    // loader uses, and two implementations that must agree byte-for-byte should
    // not be written twice.
    //
    // Both pump the window's message queue as they go: hashing a 233 MB GLB
    // synchronously is long enough for Windows to classify the process as hung
    // and terminate it before the first level-load task finishes.
    static uint64_t HashBytes(const void* data, size_t size,
                              uint64_t hash = 1469598103934665603ull);
    static uint64_t HashFile(const std::filesystem::path& path);
};
