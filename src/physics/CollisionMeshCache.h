#ifndef COLLISION_MESH_CACHE_H
#define COLLISION_MESH_CACHE_H

#include "CollisionMesh.h"

#include <cstdint>
#include <filesystem>
#include <string>

// On-disk cache for built collision trees.
//
// Building the airport's BVH costs ~430 ms and the prefab model cache is retired
// wholesale whenever the asset watcher ticks, so an in-memory-only tree would
// re-pay that on every editor reload. Persisting it is not an optimization here;
// it is what makes mesh collision usable in the editor at all.
//
// The file lives under Content/Cooked/Collision/ rather than assetcache/: the
// latter is the editor's per-workstation asset database and is gitignored, while
// a 40 MB derived binary keyed to a source hash belongs beside the .sgeasset
// files that ship.
namespace CollisionCache {

// Everything that, if changed, must invalidate a cached tree.
struct Key {
    // The source model, used for the size / write-time / content-hash checks.
    std::filesystem::path sourcePath;
    // Prefab id, which selects the cache file. Two prefabs may share one GLB.
    std::string prefabId;
    // Hashes the prefab's targetSize and the post-normalization root transform.
    // Required because the cooked format has no equivalent: two prefabs can
    // reference the same GLB at different targetSize and would otherwise share
    // one tree built at the wrong scale.
    uint64_t transformHash = 0;
    // Bin count, leaf size and depth bound folded together, so retuning the
    // build invalidates every tree already on disk.
    uint64_t buildParamsHash = 0;
};

// Folds the build parameters into the hash the Key carries.
uint64_t HashBuildParams(const CollisionMeshBuildParams& params);

// Folds targetSize and a root transform into the Key's transformHash.
uint64_t HashTransform(float targetSize, const DirectX::XMFLOAT3& scale,
                       const DirectX::XMFLOAT3& translation);

// Where the tree for this key is stored.
std::filesystem::path PathFor(const Key& key);

// Loads a cached tree, returning false on a miss, a stale entry, a version
// mismatch or a failed integrity check. `error` receives the reason, which is
// worth logging: "stale" and "corrupt" mean very different things.
bool Load(const Key& key, CollisionMesh& out, std::string* error = nullptr);

// Writes the tree, creating parent directories. Writes to a temporary file and
// renames, so an interrupted save cannot leave a half-written tree that would
// later pass the header check.
bool Save(const Key& key, const CollisionMesh& mesh, std::string* error = nullptr);

// SGE_NO_COLLISION_CACHE=1 disables both halves, mirroring SGE_NO_COOKED.
bool Enabled();

} // namespace CollisionCache

#endif
