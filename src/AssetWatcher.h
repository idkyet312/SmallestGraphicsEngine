#ifndef ASSET_WATCHER_H
#define ASSET_WATCHER_H

#include <atomic>
#include <filesystem>
#include <memory>
#include <vector>

class AssetWatcher {
public:
    AssetWatcher();
    ~AssetWatcher();
    AssetWatcher(const AssetWatcher&) = delete;
    AssetWatcher& operator=(const AssetWatcher&) = delete;

    void Start(const std::vector<std::filesystem::path>& roots = {
        "models", "textures", "audio", "prefabs", "levels" });
    void Stop();
    bool ConsumeChange() { return changed_.exchange(false); }

private:
    struct Watch;
    std::vector<std::unique_ptr<Watch>> watches_;
    std::atomic<bool> changed_{ false };
    std::atomic<bool> stopping_{ false };
};

#endif
