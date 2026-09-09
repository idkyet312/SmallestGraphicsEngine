#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Virtual shadow maps as a world-anchored clipmap.
//
// Independent of the cascade shadow maps by design: this owns its own light
// basis, its own projections and its own atlas, and while it is active the
// cascade pass does not render at all. Nothing here falls back to a cascade
// sample -- a position with no resident page reads as lit. Coverage is
// therefore a correctness property, not a quality knob.
//
// Pages live on an ABSOLUTE WORLD LATTICE. A page's identity is
// floor(lightXY / PageExtent(level)) and nothing else: the lattice origin is
// the world origin and never moves. The viewer only chooses *which* pages to
// request -- it never affects what a page *is*. That is the whole point. An
// earlier design snapped a viewer-centred window to page granularity, which
// looks equivalent but is not: crossing one page boundary slid the window and
// renumbered every cell, invalidating a whole level even though nearly all of
// those world squares were still covered. With absolute keys, walking one page
// over costs exactly one new page.
namespace VirtualShadows {
// Physical atlas: a 4x4 grid of 1024px pages in one 4096px texture.
//
// Sized against measured VRAM, not ambition: the target card has 4 GB and the
// main level already reports ~5.2 GB in use, i.e. it is spilling to shared
// system memory before shadows are considered. At 4096px an atlas costs 64 MB
// per resource (192 MB across the two frame outputs plus the static cache),
// matching what this system already allocated. An 8192px/64-page atlas would
// have been 768 MB, which on this budget is the silent allocation-failure
// profile rather than a quality upgrade.
constexpr uint32_t PageSize = 1024;
constexpr uint32_t AtlasPages = 4;
constexpr uint32_t AtlasSize = PageSize * AtlasPages;   // 4096
constexpr uint32_t Capacity = AtlasPages * AtlasPages;  // 16

constexpr uint32_t Levels = 3;
constexpr uint32_t Invalid = ~0u;

// World size of one page at each level, quadrupling outward. With
// shadowFarPlane at 90 m these are chosen so the middle level already covers
// past the range shadows are expected at, and the outer level reaches distant
// terrain:
//
//   level 0 -- 12 m page,  ~12 mm texels, 3x3 pages = 36 m around the viewer
//   level 1 -- 48 m page,  ~47 mm texels, 3x3 pages = 144 m
//   level 2 -- 192 m page, ~188 mm texels, 3x3 pages = 576 m
//
// There is no per-level window or grid bound: the lattice is unbounded and
// coverage is limited only by how many pages the atlas holds.
constexpr float BasePageExtent = 12.0f;
inline float PageExtent(uint32_t level) {
    return BasePageExtent * static_cast<float>(1u << (level * 2));
}

// Page key packing. ix/iy are signed lattice coordinates, biased into unsigned
// fields so a key is a plain integer the shader can compare and hash.
//
// 14 bits each gives +/-8192 pages: +/-98 km at level 0, far beyond any level.
// Levels take the bits above. Coordinates outside the range cannot be
// represented, so callers must check CoordInRange rather than let a key
// silently alias onto another page -- an aliased key resolves to the wrong
// world square, which reads as a shadow bug rather than a range bug.
constexpr int32_t CoordBits = 14;
constexpr int32_t CoordBias = 1 << (CoordBits - 1);          // 8192
constexpr uint32_t CoordMask = (1u << CoordBits) - 1u;

constexpr bool CoordInRange(int32_t v) {
    return v >= -CoordBias && v < CoordBias;
}

constexpr uint32_t Key(uint32_t level, int32_t ix, int32_t iy) {
    return (level << (CoordBits * 2)) |
           ((static_cast<uint32_t>(ix + CoordBias) & CoordMask) << CoordBits) |
           (static_cast<uint32_t>(iy + CoordBias) & CoordMask);
}
constexpr uint32_t KeyLevel(uint32_t key) { return key >> (CoordBits * 2); }
constexpr int32_t KeyX(uint32_t key) {
    return static_cast<int32_t>((key >> CoordBits) & CoordMask) - CoordBias;
}
constexpr int32_t KeyY(uint32_t key) {
    return static_cast<int32_t>(key & CoordMask) - CoordBias;
}

inline uint32_t BuildRequests(float viewerX, float viewerY, uint32_t budget,
                             std::array<uint32_t, Capacity>& requests) {
    requests.fill(Invalid);
    budget = (std::min)(budget, Capacity);
    uint32_t count = 0;
    auto add = [&](uint32_t level, int x, int y) {
        if (count == budget || !CoordInRange(x) || !CoordInRange(y)) return;
        const auto key = Key(level, x, y);
        if (std::find(requests.begin(), requests.end(), key) == requests.end())
            requests[count++] = key;
    };
    // Reserve coverage at every scale before spending spare slots on detail.
    // The nearest 2x2 block contains the viewer even across negative coordinates
    // and gives the outer level at least half a page of coverage in every direction.
    for (uint32_t corner = 0; corner < 4; ++corner) {
        for (uint32_t level = 0; level < Levels; ++level) {
            const float x = viewerX / PageExtent(level);
            const float y = viewerY / PageExtent(level);
            const int cx = static_cast<int>(std::floor(x));
            const int cy = static_cast<int>(std::floor(y));
            const int dx = x - cx < 0.5f ? -1 : 1;
            const int dy = y - cy < 0.5f ? -1 : 1;
            add(level, cx + ((corner & 1) ? dx : 0),
                       cy + ((corner & 2) ? dy : 0));
        }
    }
    const int cx = static_cast<int>(std::floor(viewerX / PageExtent(0)));
    const int cy = static_cast<int>(std::floor(viewerY / PageExtent(0)));
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) add(0, cx + x, cy + y);
    return count;
}

// Sparse page table published to shaders: open addressing with linear probing.
// A dense array is not an option now that the lattice is unbounded.
//
// TableSize must be a power of two and comfortably larger than Capacity so
// probe chains stay short; 64 slots for 16 pages is a load factor of 0.25.
constexpr uint32_t TableSize = 64;
constexpr uint32_t TableMask = TableSize - 1;
static_assert((TableSize & TableMask) == 0, "TableSize must be a power of two");
static_assert(TableSize >= Capacity * 2, "keep the page table sparse");

// Key 0 is a valid lattice position (level 0, ix = iy = -8192), so an empty
// slot cannot be spelled with a zero key. Entries store slot + 1 and treat 0
// in that field as empty instead.
constexpr uint32_t Hash(uint32_t key) {
    uint32_t h = key * 2654435761u;     // Knuth multiplicative
    h ^= h >> 15;
    return h & TableMask;
}

// Packed uint4s match HLSL constant-buffer array strides.
struct alignas(16) Constants {
    // x = active, y = resident page count, z = TableSize, w = PageSize
    std::array<uint32_t, 4> config{};
    // 1 / PageExtent(level) per level, so the shader turns a light-space
    // position into lattice coordinates with a multiply.
    std::array<float, 4> levelScale{};
    // Light rotation, shared with every consuming shader so the basis has one
    // definition and cannot drift between C++ and HLSL.
    std::array<std::array<float, 4>, 4> lightRotation{};
    // entry.x = key, entry.y = slot + 1 (0 = empty). zw unused, kept for the
    // uint4 stride HLSL constant buffers impose on arrays.
    std::array<std::array<uint32_t, 4>, TableSize> entries{};

    // Insert one resident page. Linear probing; the table can never fill
    // because TableSize > Capacity, so this always terminates.
    void Map(uint32_t key, uint32_t slot) {
        const uint32_t index = Hash(key);
        for (uint32_t probe = 0; probe < TableSize; ++probe) {
            auto& entry = entries[(index + probe) & TableMask];
            if (entry[1] == 0 || entry[0] == key) {
                entry[0] = key;
                entry[1] = slot + 1;
                return;
            }
        }
    }
};
// Pin the layout: HLSL re-pads to 16-byte boundaries and C++ does not, so a
// silent divergence here would corrupt the page table and read as a shadow bug.
static_assert(sizeof(Constants) == 16 + 16 + 64 + TableSize * 16,
              "VSM shader layout");
static_assert(offsetof(Constants, levelScale) == 16, "VSM levelScale offset");
static_assert(offsetof(Constants, lightRotation) == 32, "VSM rotation offset");
static_assert(offsetof(Constants, entries) == 96, "VSM entries offset");

struct PageCache {
    std::array<uint32_t, Capacity> keys;
    std::array<bool, Capacity> valid{};
    std::array<bool, Capacity> requested{};
    PageCache() { keys.fill(Invalid); }

    // The only wholesale invalidation in the system. Absolute keys mean nothing
    // else can stale a page: not camera motion, not rotation, not the viewer
    // crossing a boundary. Only a change to the light basis does, because that
    // re-rasterizes every page through a different projection.
    void Invalidate() { valid.fill(false); }

    // Protect all hits before assigning misses: request order must not evict a
    // resident page that is requested later in the same frame.
    void Request(const std::array<uint32_t, Capacity>& wanted, uint32_t count) {
        requested.fill(false);
        count = (std::min)(count, Capacity);
        for (uint32_t n = 0; n < count; ++n)
            for (uint32_t i = 0; i < Capacity; ++i)
                if (wanted[n] != Invalid && keys[i] == wanted[n]) requested[i] = true;
        for (uint32_t n = 0; n < count; ++n) {
            if (wanted[n] == Invalid ||
                std::find(keys.begin(), keys.end(), wanted[n]) != keys.end()) continue;
            for (uint32_t i = 0; i < Capacity; ++i) {
                if (requested[i]) continue;
                keys[i] = wanted[n];
                requested[i] = true;
                valid[i] = false;
                break;
            }
        }
    }
};
}
