#include "CollisionMeshCache.h"

#include "CookedAssetLoader.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace fs = std::filesystem;

namespace CollisionCache {
namespace {

constexpr uint64_t kMagic = 0x4c4c4f43454753ull; // "SGECOLL"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kEndianTag = 0x01020304u;
constexpr uint64_t kHashSeed = 1469598103934665603ull;

// Mirrors SGE::Cooked::Header's shape: identity and version first, then the
// staleness fields, then section offsets, then an integrity hash over the
// payload. The two additions beyond the cooked pattern are transformHash and
// buildParamsHash -- see the header for why neither has a cooked equivalent.
struct Header {
    uint64_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t endianTag = kEndianTag;
    uint32_t headerSize = 0;
    uint32_t reservedPadding = 0;
    uint64_t fileSize = 0;

    uint64_t sourceHash = 0;
    uint64_t sourceSize = 0;
    int64_t sourceWriteTime = 0;
    uint64_t transformHash = 0;
    uint64_t buildParamsHash = 0;

    uint64_t triangleCount = 0;
    uint64_t nodeCount = 0;
    uint64_t triangleOffset = 0;
    uint64_t nodeOffset = 0;
    uint64_t sourceTriangleOffset = 0;

    uint64_t contentHash = 0;
    uint64_t reserved[4] = {};
};

uint64_t HashScalar(uint64_t hash, const void* data, size_t size) {
    return CookedAssetLoader::HashBytes(data, size, hash);
}

bool RangeValid(uint64_t offset, uint64_t size, uint64_t fileSize) {
    return offset <= fileSize && size <= fileSize - offset;
}

bool ArrayValid(uint64_t offset, uint64_t count, uint64_t elementSize,
                uint64_t fileSize) {
    return elementSize == 0 ||
        (count <= (std::numeric_limits<uint64_t>::max)() / elementSize &&
         RangeValid(offset, count * elementSize, fileSize));
}

// Filesystem-safe form of a prefab id: "props/airport" -> "props_airport".
std::string SanitizeId(const std::string& id) {
    std::string result;
    result.reserve(id.size());
    for (char value : id) {
        const bool safe = (value >= 'a' && value <= 'z') ||
                          (value >= 'A' && value <= 'Z') ||
                          (value >= '0' && value <= '9') ||
                          value == '-' || value == '.';
        result += safe ? value : '_';
    }
    return result;
}

} // namespace

bool Enabled() {
    // Same spelling as SGE_NO_COOKED: the value must be "1", so leaving the
    // variable set to 0 in a shell does not silently disable the cache.
    const char* disable = std::getenv("SGE_NO_COLLISION_CACHE");
    return !(disable && disable[0] == '1');
}

uint64_t HashBuildParams(const CollisionMeshBuildParams& params) {
    uint64_t hash = kHashSeed;
    hash = HashScalar(hash, &params.binCount, sizeof(params.binCount));
    hash = HashScalar(hash, &params.maxTrianglesPerLeaf,
                      sizeof(params.maxTrianglesPerLeaf));
    hash = HashScalar(hash, &params.maxDepth, sizeof(params.maxDepth));
    return hash;
}

uint64_t HashTransform(float targetSize, const DirectX::XMFLOAT3& scale,
                       const DirectX::XMFLOAT3& translation) {
    uint64_t hash = kHashSeed;
    hash = HashScalar(hash, &targetSize, sizeof(targetSize));
    hash = HashScalar(hash, &scale, sizeof(scale));
    hash = HashScalar(hash, &translation, sizeof(translation));
    return hash;
}

fs::path PathFor(const Key& key) {
    return fs::path("Content") / "Cooked" / "Collision" /
           (SanitizeId(key.prefabId) + ".sgecoll");
}

bool Load(const Key& key, CollisionMesh& out, std::string* error) {
    const auto fail = [&](const char* reason) {
        if (error) *error = reason;
        return false;
    };
    if (!Enabled()) return fail("collision cache disabled");

    const fs::path path = PathFor(key);
    std::error_code ec;
    if (!fs::exists(path, ec)) return fail("no cached tree");

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return fail("cached tree could not be opened");

    Header header;
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (stream.gcount() != static_cast<std::streamsize>(sizeof(header)))
        return fail("cached tree is truncated");

    if (header.magic != kMagic || header.endianTag != kEndianTag ||
        header.headerSize != sizeof(Header))
        return fail("cached tree is not a collision tree");
    if (header.version != kVersion) return fail("cached tree version mismatch");
    if (header.buildParamsHash != key.buildParamsHash)
        return fail("build parameters changed");
    if (header.transformHash != key.transformHash)
        return fail("prefab transform changed");

    const uint64_t fileSize = fs::file_size(path, ec);
    if (ec || header.fileSize != fileSize)
        return fail("cached tree size mismatch");

    // Source staleness, in the cooked loader's order: the cheap size and
    // write-time comparisons gate the full content hash, which for the airport
    // means reading 233 MB.
    if (fs::exists(key.sourcePath, ec)) {
        const uint64_t sourceSize = fs::file_size(key.sourcePath, ec);
        if (ec || header.sourceSize != sourceSize)
            return fail("source model changed size");
        const auto writeTime = fs::last_write_time(key.sourcePath, ec);
        const int64_t stamp = ec ? 0
            : static_cast<int64_t>(writeTime.time_since_epoch().count());
        if (header.sourceWriteTime != stamp) {
            // A touched-but-identical file is common (a re-export that changed
            // nothing), so pay for the hash before declaring the tree stale.
            if (header.sourceHash != CookedAssetLoader::HashFile(key.sourcePath))
                return fail("source model changed");
        }
    }

    const uint64_t triangleFloats = header.triangleCount * 9;
    if (!ArrayValid(header.triangleOffset, triangleFloats, sizeof(float), fileSize) ||
        !ArrayValid(header.nodeOffset, header.nodeCount,
                    sizeof(CollisionMesh::Node), fileSize) ||
        !ArrayValid(header.sourceTriangleOffset, header.triangleCount,
                    sizeof(uint32_t), fileSize))
        return fail("cached tree offsets are out of range");
    if (header.nodeCount == 0 || header.triangleCount == 0)
        return fail("cached tree is empty");

    CollisionMesh loaded;
    loaded.triangles.resize(static_cast<size_t>(triangleFloats));
    loaded.nodes.resize(static_cast<size_t>(header.nodeCount));
    loaded.sourceTriangle.resize(static_cast<size_t>(header.triangleCount));

    // Read into owned vectors rather than memory-mapping. Unlike cooked meshes,
    // which upload to the GPU and drop the mapping, this is CPU-resident data
    // queried every frame.
    const auto readSection = [&](uint64_t offset, void* destination, uint64_t bytes) {
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        stream.read(static_cast<char*>(destination),
                    static_cast<std::streamsize>(bytes));
        return stream.gcount() == static_cast<std::streamsize>(bytes);
    };
    const uint64_t triangleBytes = triangleFloats * sizeof(float);
    const uint64_t nodeBytes = header.nodeCount * sizeof(CollisionMesh::Node);
    const uint64_t sourceBytes = header.triangleCount * sizeof(uint32_t);
    if (!readSection(header.triangleOffset, loaded.triangles.data(), triangleBytes) ||
        !readSection(header.nodeOffset, loaded.nodes.data(), nodeBytes) ||
        !readSection(header.sourceTriangleOffset, loaded.sourceTriangle.data(),
                     sourceBytes))
        return fail("cached tree is truncated");

    // Integrity, as the cooked loader does: a tree that survived a bad disk or
    // a partial write would otherwise be traversed as if it were valid, and a
    // corrupt node index reads out of bounds.
    uint64_t content = kHashSeed;
    content = HashScalar(content, loaded.triangles.data(),
                         static_cast<size_t>(triangleBytes));
    content = HashScalar(content, loaded.nodes.data(),
                         static_cast<size_t>(nodeBytes));
    content = HashScalar(content, loaded.sourceTriangle.data(),
                         static_cast<size_t>(sourceBytes));
    if (content != header.contentHash) return fail("cached tree failed its hash");

    // Structural check before anything traverses this: a leaf pointing past the
    // triangle array, or a child index past the node array, is an out-of-bounds
    // read at query time rather than a visible glitch.
    const uint32_t triangleCount = static_cast<uint32_t>(header.triangleCount);
    const uint32_t nodeCount = static_cast<uint32_t>(header.nodeCount);
    for (const CollisionMesh::Node& node : loaded.nodes) {
        if (node.count > 0) {
            if (node.leftFirst > triangleCount ||
                node.count > triangleCount - node.leftFirst)
                return fail("cached tree has an out-of-range leaf");
        } else if (node.leftFirst + 1 >= nodeCount) {
            return fail("cached tree has an out-of-range child");
        }
    }

    out = std::move(loaded);
    return true;
}

bool Save(const Key& key, const CollisionMesh& mesh, std::string* error) {
    const auto fail = [&](const char* reason) {
        if (error) *error = reason;
        return false;
    };
    if (!Enabled()) return fail("collision cache disabled");
    if (mesh.Empty() || mesh.triangles.empty())
        return fail("refusing to cache an empty tree");

    const fs::path path = PathFor(key);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    Header header;
    header.headerSize = sizeof(Header);
    header.transformHash = key.transformHash;
    header.buildParamsHash = key.buildParamsHash;
    if (fs::exists(key.sourcePath, ec)) {
        header.sourceSize = fs::file_size(key.sourcePath, ec);
        if (ec) header.sourceSize = 0;
        const auto writeTime = fs::last_write_time(key.sourcePath, ec);
        header.sourceWriteTime = ec ? 0
            : static_cast<int64_t>(writeTime.time_since_epoch().count());
        header.sourceHash = CookedAssetLoader::HashFile(key.sourcePath);
    }

    header.triangleCount = mesh.TriangleCount();
    header.nodeCount = mesh.nodes.size();
    const uint64_t triangleBytes = mesh.triangles.size() * sizeof(float);
    const uint64_t nodeBytes = mesh.nodes.size() * sizeof(CollisionMesh::Node);
    const uint64_t sourceBytes = mesh.sourceTriangle.size() * sizeof(uint32_t);
    header.triangleOffset = sizeof(Header);
    header.nodeOffset = header.triangleOffset + triangleBytes;
    header.sourceTriangleOffset = header.nodeOffset + nodeBytes;
    header.fileSize = header.sourceTriangleOffset + sourceBytes;

    uint64_t content = kHashSeed;
    content = HashScalar(content, mesh.triangles.data(),
                         static_cast<size_t>(triangleBytes));
    content = HashScalar(content, mesh.nodes.data(),
                         static_cast<size_t>(nodeBytes));
    content = HashScalar(content, mesh.sourceTriangle.data(),
                         static_cast<size_t>(sourceBytes));
    header.contentHash = content;

    // Write to a sibling temporary and rename. An interrupted write would
    // otherwise leave a file whose header parses but whose payload is short,
    // and the next run would have to detect that via the content hash alone.
    fs::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return fail("cached tree could not be created");
        stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
        stream.write(reinterpret_cast<const char*>(mesh.triangles.data()),
                     static_cast<std::streamsize>(triangleBytes));
        stream.write(reinterpret_cast<const char*>(mesh.nodes.data()),
                     static_cast<std::streamsize>(nodeBytes));
        stream.write(reinterpret_cast<const char*>(mesh.sourceTriangle.data()),
                     static_cast<std::streamsize>(sourceBytes));
        if (!stream) {
            stream.close();
            fs::remove(temporary, ec);
            return fail("cached tree could not be written");
        }
    }
    fs::remove(path, ec);
    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(temporary, ec);
        return fail("cached tree could not be renamed into place");
    }
    return true;
}

} // namespace CollisionCache
