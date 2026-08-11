#ifndef ASSET_WATCHER_H
#define ASSET_WATCHER_H

#include <atomic>
#include <filesystem>
#include <memory>
#include <vector>

// Directories watched for asset changes when none are named explicitly.
//
// Content/Levels is deliberately absent. The only thing that writes a level file
// is the editor's own Save, and the change it raised here made saving trigger a
// full asset+prefab registry refresh -- which Assimp-imports models on the main
// thread, freezing the editor for over a second immediately after the file had
// already been written. A level is not an asset input, so nothing needs
// reloading when one changes; the editor refreshes its own file list in
// SaveTo/RefreshLevelFiles. Pinned by AssetWatcherTests.
const std::vector<std::filesystem::path>& AssetWatcherDefaultRoots();

class AssetWatcher {
public:
    AssetWatcher();
    ~AssetWatcher();
    AssetWatcher(const AssetWatcher&) = delete;
    AssetWatcher& operator=(const AssetWatcher&) = delete;

    void Start();
    void Start(const std::vector<std::filesystem::path>& roots);
    void Stop();
    bool ConsumeChange() { return changed_.exchange(false); }

private:
    struct Watch;
    std::vector<std::unique_ptr<Watch>> watches_;
    std::atomic<bool> changed_{ false };
    std::atomic<bool> stopping_{ false };
};

#endif
