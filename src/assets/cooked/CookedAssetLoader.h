#pragma once

#include "SceneGraph.h"

#include <d3d12.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>

class CookedAssetLoader {
public:
    static std::filesystem::path FindForSource(
        const std::filesystem::path& source);

    // loadTextures=false skips the baked textures entirely -- no resources, no
    // copies recorded -- for callers that replace every material themselves.
    // Creating them and dropping them is what made such callers avoid the
    // cache: the open command list still referenced the released textures.
    static std::shared_ptr<SceneNode> LoadForSource(
        const std::filesystem::path& source,
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
        std::string* error = nullptr, bool loadTextures = true);

    static std::shared_ptr<SceneNode> Load(
        const std::filesystem::path& cookedPath,
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
        std::string* error = nullptr, bool loadTextures = true);

    // A loose image's texture-only cook (AssetCooker <image>): BC with a full
    // mip chain. Null when there is no fresh cook; the caller then decodes the
    // image itself. Upload heaps go to `uploads` like the importer's own.
    static Microsoft::WRL::ComPtr<ID3D12Resource> LoadTextureForSource(
        const std::filesystem::path& source, ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploads);

    // glTF metallic-roughness packed at cook time from separate roughness and
    // metallic images (AssetCooker --pack-mr). Stale if either image changed.
    static Microsoft::WRL::ComPtr<ID3D12Resource>
    LoadPackedMetalRoughnessForSources(
        const std::filesystem::path& roughness,
        const std::filesystem::path& metallic, ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploads);

    // Background warm-up for assets that will be loaded soon. A worker thread
    // reads each cooked asset sequentially -- pulling it into the OS file
    // cache -- and verifies its payload hash, so a later LoadForSource skips
    // the re-hash and maps memory that is already resident. Sources are only
    // read when there is no cooked asset to load instead. Returns at once;
    // safe to call from the main thread.
    //
    // Measured 2026-10-02 (game on a USB SSD): the armory's weapon set took
    // 21.3 s through the memory-mapped, random-access loader on a cold cache
    // and 5.8 s on a warm one.
    static void PrefetchSources(std::vector<std::filesystem::path> sources);
    // Blocks until every prefetch started so far has finished.
    static void WaitForPrefetch();

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
