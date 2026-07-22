#ifndef ASSET_REGISTRY_H
#define ASSET_REGISTRY_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class AssetKind { Model, Texture, Audio, Prefab, Level };

struct AssetRecord {
    std::string guid;
    std::filesystem::path path;
    AssetKind kind = AssetKind::Model;
    uintmax_t size = 0;
    int64_t modified = 0;
    int width = 0;
    int height = 0;
    std::vector<std::string> dependencies;
};

class AssetRegistry {
public:
    bool Refresh(bool force = false);
    const std::vector<AssetRecord>& Assets() const { return assets_; }
    std::vector<const AssetRecord*> Assets(AssetKind kind) const;
    const AssetRecord* FindGuid(const std::string& guid) const;
    const AssetRecord* FindPath(const std::filesystem::path& path) const;
    uint64_t Revision() const { return revision_; }
    const std::string& LastError() const { return lastError_; }
    static const char* KindName(AssetKind kind);

private:
    std::vector<AssetRecord> assets_;
    uint64_t fingerprint_ = 0;
    uint64_t revision_ = 0;
    std::string lastError_;
};

#endif
