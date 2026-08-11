#include "AssetWatcher.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("sge-watcher-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "models");
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(root);
    AssetWatcher watcher;
    watcher.Start({"models"});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    { std::ofstream stream("models/new.glb"); stream << "changed"; }
    bool changed = false;
    for (int attempt = 0; attempt < 40 && !changed; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        changed = watcher.ConsumeChange();
    }
    watcher.Stop();
    std::filesystem::current_path(previous);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    if (!changed) {
        std::cerr << "filesystem change was not reported\n";
        return 1;
    }

    // Content/Levels must stay out of the default roots. Watching it made the
    // editor's own Save trip an asset-change refresh -- a full prefab registry
    // rescan, which Assimp-imports models on the main thread and froze the
    // editor for over a second right after the file had already been written.
    // A level file is not an asset input, so nothing needs reloading when one
    // changes. Guarded by a test because the cost is invisible at the call site.
    {
        const auto& watchedRoots = AssetWatcherDefaultRoots();
        bool watchesLevels = false;
        for (const std::filesystem::path& r : watchedRoots)
            if (r.generic_string().find("Levels") != std::string::npos)
                watchesLevels = true;
        if (watchesLevels) {
            std::cerr << "AssetWatcher must not watch Content/Levels: saving a "
                         "level would trigger a full asset refresh\n";
            return 1;
        }
        if (watchedRoots.empty()) {
            std::cerr << "AssetWatcher default roots are empty\n";
            return 1;
        }
    }
    return 0;
}
