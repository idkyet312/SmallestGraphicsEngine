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
    return 0;
}
