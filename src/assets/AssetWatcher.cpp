#include "AssetWatcher.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <array>
#include <thread>

struct AssetWatcher::Watch {
    HANDLE directory = INVALID_HANDLE_VALUE;
    std::thread thread;
};

AssetWatcher::AssetWatcher() = default;
AssetWatcher::~AssetWatcher() { Stop(); }

const std::vector<std::filesystem::path>& AssetWatcherDefaultRoots() {
    // See the note in AssetWatcher.h: Content/Levels must not be listed here.
    static const std::vector<std::filesystem::path> roots = {
        "Content/Models", "Content/Textures", "Content/Audio",
        "Content/Prefabs" };
    return roots;
}

void AssetWatcher::Start() { Start(AssetWatcherDefaultRoots()); }

void AssetWatcher::Start(const std::vector<std::filesystem::path>& roots) {
    Stop();
    stopping_ = false;
    changed_ = false;
    for (const std::filesystem::path& root : roots) {
        std::error_code error;
        std::filesystem::create_directories(root, error);
        auto watch = std::make_unique<Watch>();
        watch->directory = CreateFileW(std::filesystem::absolute(root).c_str(),
            FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (watch->directory == INVALID_HANDLE_VALUE) continue;
        Watch* raw = watch.get();
        raw->thread = std::thread([this, raw]() {
            std::array<unsigned char, 16 * 1024> buffer{};
            while (!stopping_) {
                DWORD bytes = 0;
                const BOOL ok = ReadDirectoryChangesW(raw->directory, buffer.data(),
                    static_cast<DWORD>(buffer.size()), TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                    &bytes, nullptr, nullptr);
                if (!ok || stopping_) break;
                if (bytes) changed_ = true;
            }
        });
        watches_.push_back(std::move(watch));
    }
}

void AssetWatcher::Stop() {
    stopping_ = true;
    for (auto& watch : watches_)
        if (watch->directory != INVALID_HANDLE_VALUE)
            CancelIoEx(watch->directory, nullptr);
    for (auto& watch : watches_) {
        if (watch->thread.joinable()) watch->thread.join();
        if (watch->directory != INVALID_HANDLE_VALUE) CloseHandle(watch->directory);
    }
    watches_.clear();
}
