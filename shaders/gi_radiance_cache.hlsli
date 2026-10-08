// World-space radiance cache for the Lumen GI bounce (spatial hash).
//
// Lumen traces one ray per pixel and used to shade every hit live: a material
// and texture fetch plus a second, sun shadow ray. The cache stores the
// radiance leaving a hit surface in a hashed world cell, so a ray that lands in
// a cell another ray already shaded reads one entry instead. Same structure as
// idTech 8's world radiance cache [Sousa, SIGGRAPH 2025] and NVIDIA's SHaRC:
// cells are 25 cm near the camera and double with distance, keyed by position,
// LOD and the axis the ray arrived along.
//
// Insertion happens in the resolve: the lane that claims an empty cell shades
// its hit as before and stores the result, along with the surface (position,
// normal, albedo, emissive). GICacheRefreshMain then relights stored surfaces
// round-robin -- sun, local lights, and one bounce ray that reads the cache
// itself -- so the cache carries multiple bounces and local-light bounce on
// every level, not only on levels with a DDGI grid.
//
// Entries are three uint4:
//   [0] checksum (0 = empty), f16 r|g, f16 b | sampleCount << 16, lastUsedFrame
//   [1] position xyz (float bits), octahedral normal (snorm16 x2)
//   [2] albedo rgb8, f16 emissive r|g, f16 emissive b, unused
// [0] is written in one 16-byte store, so a reader sees the checksum and the
// radiance it belongs to together or sees the cell still pending.

#ifndef GI_RADIANCE_CACHE_HLSLI
#define GI_RADIANCE_CACHE_HLSLI

#define GI_CACHE_CAPACITY_LOG2 19u
#define GI_CACHE_CAPACITY (1u << GI_CACHE_CAPACITY_LOG2)
#define GI_CACHE_STRIDE 3u
#define GI_CACHE_PROBES 8u
#define GI_CACHE_CELL 0.25
// Cells double each time the camera distance passes 8 m * (2^k - 1):
// 0.25 m to 8 m, 0.5 m to 24 m, 1 m to 56 m, 2 m to 120 m.
#define GI_CACHE_LOD_DISTANCE 8.0
// Entries visited per frame by the refresh; the capacity divides into 16
// slices, so every entry is relit every 16 frames.
#define GI_CACHE_REFRESH_PER_FRAME (GI_CACHE_CAPACITY / 16u)
// EMA floor: a refreshed entry keeps 1/8 of each new sample.
#define GI_CACHE_MAX_SAMPLES 8u
// Frames without a lookup before a cell is released.
#define GI_CACHE_EVICT_FRAMES 600u

#define GI_CACHE_HIT 0u
#define GI_CACHE_PENDING 1u
#define GI_CACHE_CLAIMED 2u
#define GI_CACHE_FULL 3u

RWStructuredBuffer<uint4> giCacheEntries : register(u16);

uint GICachePcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

struct GICacheKey {
    uint index;
    uint checksum;
};

// LOD from the camera distance, so a cell stays a roughly constant fraction of
// a pixel footprint. cameraPos is the render eye (FrameConstants).
uint GICacheLod(float3 position) {
    float distance = length(position - cameraPos);
    return min((uint)floor(log2(1.0 + distance / GI_CACHE_LOD_DISTANCE)), 15u);
}

float GICacheCellSize(uint lod) {
    return GI_CACHE_CELL * exp2((float)lod);
}

// `facing` buckets the cell by its dominant axis and sign. The resolve passes
// the reversed ray direction, which it has for free: the true surface normal
// would cost the vertex fetch the cache exists to skip. Diffuse exitance does
// not depend on the arrival direction, so a surface split across buckets only
// costs duplicate entries, while the two faces of a thin wall in one cell are
// reached from opposite sides and stay apart.
GICacheKey GICacheMakeKey(float3 position, float3 facing, uint lod) {
    int3 cell = (int3)floor(position / GICacheCellSize(lod));
    float3 a = abs(facing);
    uint axis = a.x > a.y ? (a.x > a.z ? 0u : 2u) : (a.y > a.z ? 1u : 2u);
    uint face = axis * 2u + (facing[axis] < 0.0 ? 1u : 0u);
    uint meta = lod | (face << 4u);
    GICacheKey key;
    key.index = GICachePcg(meta + GICachePcg(asuint(cell.z) +
        GICachePcg(asuint(cell.y) + GICachePcg(asuint(cell.x))))) &
        (GI_CACHE_CAPACITY - 1u);
    // A second, independent hash identifies the cell inside its probe chain.
    uint checksum = GICachePcg((asuint(cell.x) ^ 0x9e3779b9u) +
        GICachePcg(asuint(cell.y) + GICachePcg((asuint(cell.z) ^ 0x85ebca6bu) +
        GICachePcg(meta ^ 0xc2b2ae35u))));
    key.checksum = max(checksum, 1u);
    return key;
}

// Jitters the lookup position by up to half a cell (Binder 2021, SHaRC) so the
// cell grid does not print onto surfaces; the temporal filter absorbs it.
float3 GICacheJitter(float3 position, uint lod, uint seed) {
    float3 xi = float3(GICachePcg(seed), GICachePcg(seed ^ 0x68bc21ebu),
                       GICachePcg(seed ^ 0x02e5be93u)) * (1.0 / 4294967296.0);
    return position + (xi - 0.5) * GICacheCellSize(lod);
}

float3 GICacheRadiance(uint4 entry) {
    return float3(f16tof32(entry.y), f16tof32(entry.y >> 16u),
                  f16tof32(entry.z));
}

uint GICacheSamples(uint4 entry) { return entry.z >> 16u; }

uint4 GICachePackHead(uint checksum, float3 radiance, uint samples,
                      uint lastUsed) {
    radiance = min(max(radiance, 0.0), 60000.0);
    return uint4(checksum,
                 f32tof16(radiance.r) | (f32tof16(radiance.g) << 16u),
                 f32tof16(radiance.b) | (min(samples, 0xffffu) << 16u),
                 lastUsed);
}

float2 GICacheOctEncode(float3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.xy;
    if (n.z < 0.0)
        e = (1.0 - abs(e.yx)) * float2(e.x >= 0.0 ? 1.0 : -1.0,
                                       e.y >= 0.0 ? 1.0 : -1.0);
    return e;
}

float3 GICacheOctDecode(float2 e) {
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0,
                                          n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

uint GICachePackSnorm2(float2 v) {
    int2 q = (int2)round(clamp(v, -1.0, 1.0) * 32767.0);
    return (asuint(q.x) & 0xffffu) | (asuint(q.y) << 16u);
}

float2 GICacheUnpackSnorm2(uint v) {
    int2 q = int2((int)(v << 16u) >> 16, (int)v >> 16);
    return max((float2)q / 32767.0, -1.0);
}

uint GICachePackUnorm3(float3 v) {
    uint3 q = (uint3)round(saturate(v) * 255.0);
    return q.r | (q.g << 8u) | (q.b << 16u);
}

float3 GICacheUnpackUnorm3(uint v) {
    return float3(v & 0xffu, (v >> 8u) & 0xffu, (v >> 16u) & 0xffu) / 255.0;
}

// Finds the cell for `key`. HIT fills `radiance`. CLAIMED means this lane took
// an empty slot and must shade the hit and call GICacheStore. PENDING means a
// lane claimed it this frame and has not stored yet; FULL means the probe chain
// had no room. Both of those shade live without storing.
uint GICacheFind(GICacheKey key, out uint slot, out float3 radiance) {
    radiance = 0.0;
    slot = 0xffffffffu;
    for (uint i = 0u; i < GI_CACHE_PROBES; ++i) {
        uint index = (key.index + i) & (GI_CACHE_CAPACITY - 1u);
        uint stored = giCacheEntries[index * GI_CACHE_STRIDE].x;
        if (stored == 0u) {
            uint original;
            InterlockedCompareExchange(
                giCacheEntries[index * GI_CACHE_STRIDE].x, 0u, key.checksum,
                original);
            if (original == 0u) {
                slot = index;
                return GI_CACHE_CLAIMED;
            }
            stored = original;
        }
        if (stored != key.checksum) continue;
        slot = index;
        uint4 head = giCacheEntries[index * GI_CACHE_STRIDE];
        if (head.x != key.checksum || GICacheSamples(head) == 0u)
            return GI_CACHE_PENDING;
        radiance = GICacheRadiance(head);
        // Recency only drives eviction, so it is refreshed coarsely: one
        // write per cell every few frames instead of one per ray.
        if (enhancedFrameIndex - head.w > 8u)
            giCacheEntries[index * GI_CACHE_STRIDE].w = enhancedFrameIndex;
        return GI_CACHE_HIT;
    }
    return GI_CACHE_FULL;
}

void GICacheStore(uint slot, uint checksum, float3 radiance, float3 position,
                  float3 normal, float3 albedo, float3 emissive) {
    uint base = slot * GI_CACHE_STRIDE;
    emissive = min(max(emissive, 0.0), 60000.0);
    giCacheEntries[base + 1u] = uint4(asuint(position),
        GICachePackSnorm2(GICacheOctEncode(normal)));
    giCacheEntries[base + 2u] = uint4(GICachePackUnorm3(albedo),
        f32tof16(emissive.r) | (f32tof16(emissive.g) << 16u),
        f32tof16(emissive.b), 0u);
    // The head last: a reader that sees the sample count sees the surface.
    giCacheEntries[base] = GICachePackHead(checksum, radiance, 1u,
                                           enhancedFrameIndex);
}

// Lookup without claiming, for the refresh's bounce rays: a bounce must not
// create cells nobody on screen asked for.
bool GICacheLookup(GICacheKey key, out float3 radiance) {
    radiance = 0.0;
    for (uint i = 0u; i < GI_CACHE_PROBES; ++i) {
        uint index = (key.index + i) & (GI_CACHE_CAPACITY - 1u);
        uint4 head = giCacheEntries[index * GI_CACHE_STRIDE];
        if (head.x == 0u) return false;
        if (head.x != key.checksum) continue;
        if (GICacheSamples(head) == 0u) return false;
        radiance = GICacheRadiance(head);
        return true;
    }
    return false;
}

#endif // GI_RADIANCE_CACHE_HLSLI
