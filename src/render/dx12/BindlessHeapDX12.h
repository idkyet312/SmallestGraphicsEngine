#pragma once

// Bindless descriptor index allocation for SceneMaterial geometry.
//
// The legacy path binds a 3-descriptor table per material draw. That costs a
// root-descriptor-table set per draw and, worse, caps the visibility resolve at
// VB_MAX_MATERIAL_TEXTURES (64) distinct textures because the resolve compute
// shader can only see a fixed-size table. Bindless removes both limits: every
// material texture gets an absolute index into one big shader-visible heap, and
// shaders read it with ResourceDescriptorHeap[NonUniformResourceIndex(i)].
//
// This header holds the *index arithmetic only* -- no D3D calls -- so the range
// layout, deduplication, frame-slice isolation and overflow behaviour can be
// unit-tested on a machine with no GPU. BindlessHeapDX12 (below the allocator)
// wraps it with the actual descriptor heap.
//
// Heap layout (65,536 entries, deliberately separate from the 32,768-entry
// legacy heap so enabling bindless cannot perturb the legacy path):
//
//   0..3         fallback textures: white, flat-normal, neutral metal/rough, black
//   4..63        reserved for system descriptors
//   64..57343    persistent, append-only, resource-deduplicated scene textures
//   57344..61439 transient descriptors for frame slot 0
//   61440..65535 transient descriptors for frame slot 1
//
// Persistent descriptors are never overwritten or recycled during a scene. That
// is the property that makes a cached index safe to hold in SceneMaterial: the
// GPU may still be reading a descriptor from two frames ago, and reusing that
// slot would corrupt an in-flight frame. Reclaim happens only at scene
// teardown, after a full GPU sync, via ResetPersistent().

// This header is pure index arithmetic and deliberately includes no D3D
// headers, so BindlessHeapTests builds without a GPU. The device-side wrapper
// that owns the actual descriptor heap lives in BindlessHeapDeviceDX12.h.
#include <cstdint>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Range layout
// ---------------------------------------------------------------------------

static const uint32_t BINDLESS_HEAP_SIZE = 65536;

// Fallback descriptors, bound whenever a material has no texture of that kind
// or when allocation overflows. Index 0 is white so an untextured albedo
// multiplies to baseColorFactor unchanged.
static const uint32_t BINDLESS_FALLBACK_WHITE = 0;
static const uint32_t BINDLESS_FALLBACK_NORMAL = 1;   // flat tangent-space normal
static const uint32_t BINDLESS_FALLBACK_METALROUGH = 2;
static const uint32_t BINDLESS_FALLBACK_BLACK = 3;
static const uint32_t BINDLESS_FALLBACK_COUNT = 4;

static const uint32_t BINDLESS_SYSTEM_BEGIN = 4;
static const uint32_t BINDLESS_SYSTEM_END = 64;       // exclusive

static const uint32_t BINDLESS_PERSISTENT_BEGIN = 64;
static const uint32_t BINDLESS_PERSISTENT_END = 57344; // exclusive
static const uint32_t BINDLESS_PERSISTENT_CAPACITY =
    BINDLESS_PERSISTENT_END - BINDLESS_PERSISTENT_BEGIN;

// Two transient slices, one per frame-in-flight parity. The renderer writes
// only the idle slice, so updating this frame's descriptors can never modify
// descriptors the previous frame is still reading.
static const uint32_t BINDLESS_TRANSIENT_BEGIN = 57344;
static const uint32_t BINDLESS_TRANSIENT_SLICE_SIZE = 4096;
static const uint32_t BINDLESS_TRANSIENT_SLICES = 2;

static const uint32_t BINDLESS_INVALID_INDEX = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// BindlessDescriptorAllocator -- pure index bookkeeping, no D3D dependency.
// ---------------------------------------------------------------------------

class BindlessDescriptorAllocator {
public:
    // Registers `resource` in the persistent range and returns its absolute
    // heap index. Deduplicates on the pointer: the same ID3D12Resource* always
    // maps to one descriptor, which is what keeps a 4,000-chunk destructible
    // house from consuming 4,000 identical albedo descriptors.
    //
    // `outCreated` reports whether the caller must actually create a
    // ShaderResourceView at the returned index (true) or whether an existing
    // descriptor was reused (false).
    //
    // On overflow returns `fallback` and increments the overflow counter --
    // rendering continues with a fallback texture rather than dropping the
    // draw or disabling bindless mid-frame.
    uint32_t RegisterPersistent(const void* resource, uint32_t fallback,
                                bool* outCreated) {
        if (outCreated) *outCreated = false;
        if (!resource) return fallback;

        auto found = persistentLookup.find(resource);
        if (found != persistentLookup.end()) {
            ++cacheHits;
            return found->second;
        }

        if (persistentCount >= BINDLESS_PERSISTENT_CAPACITY) {
            ++overflowCount;
            return fallback;
        }

        const uint32_t index = BINDLESS_PERSISTENT_BEGIN + persistentCount;
        ++persistentCount;
        persistentLookup.emplace(resource, index);
        ++descriptorCreations;
        if (outCreated) *outCreated = true;
        return index;
    }

    // Returns the already-registered index for `resource`, or
    // BINDLESS_INVALID_INDEX. Does not allocate and does not count as a hit.
    uint32_t FindPersistent(const void* resource) const {
        auto found = persistentLookup.find(resource);
        return found == persistentLookup.end() ? BINDLESS_INVALID_INDEX
                                               : found->second;
    }

    // Drops every persistent registration and bumps the generation. Only legal
    // after the GPU has finished all frames referencing these descriptors --
    // i.e. at scene teardown behind a full fence wait. The generation bump is
    // what makes stale SceneMaterial caches self-invalidating: a material
    // holding indices from generation N ignores them once the allocator moves
    // to N+1 and re-registers on next use.
    void ResetPersistent() {
        persistentLookup.clear();
        persistentCount = 0;
        ++generation;
    }

    // Begins a transient frame slice, discarding whatever the slice held. The
    // caller passes the frame parity; only that slice is touched.
    void BeginTransientFrame(uint32_t frameSlot) {
        currentSlot = frameSlot % BINDLESS_TRANSIENT_SLICES;
        transientCount = 0;
        transientPeak = 0;
    }

    // Allocates `count` contiguous transient descriptors in the current slice
    // and returns the absolute index of the first. Contiguity matters because
    // callers place descriptor *tables* here. On overflow returns
    // BINDLESS_INVALID_INDEX so the caller can fall back to a static table.
    uint32_t AllocateTransient(uint32_t count) {
        if (count == 0) return BINDLESS_INVALID_INDEX;
        if (transientCount + count > BINDLESS_TRANSIENT_SLICE_SIZE) {
            ++overflowCount;
            return BINDLESS_INVALID_INDEX;
        }
        const uint32_t index = TransientSliceBegin(currentSlot) + transientCount;
        transientCount += count;
        if (transientCount > transientPeak) transientPeak = transientCount;
        return index;
    }

    static uint32_t TransientSliceBegin(uint32_t frameSlot) {
        return BINDLESS_TRANSIENT_BEGIN +
               (frameSlot % BINDLESS_TRANSIENT_SLICES) * BINDLESS_TRANSIENT_SLICE_SIZE;
    }

    // True when `index` addresses a descriptor this allocator owns and that has
    // actually been handed out. Used by validation and tests; a bindless shader
    // index that fails this is a bug, not a fallback condition.
    bool IsLiveIndex(uint32_t index) const {
        if (index < BINDLESS_FALLBACK_COUNT) return true;
        if (index >= BINDLESS_PERSISTENT_BEGIN &&
            index < BINDLESS_PERSISTENT_BEGIN + persistentCount)
            return true;
        const uint32_t sliceBegin = TransientSliceBegin(currentSlot);
        return index >= sliceBegin && index < sliceBegin + transientCount;
    }

    uint32_t Generation() const { return generation; }
    uint32_t PersistentCount() const { return persistentCount; }
    uint32_t PersistentCapacity() const { return BINDLESS_PERSISTENT_CAPACITY; }
    uint32_t TransientCount() const { return transientCount; }
    uint32_t TransientPeak() const { return transientPeak; }
    uint32_t TransientCapacity() const { return BINDLESS_TRANSIENT_SLICE_SIZE; }
    uint32_t CurrentSlot() const { return currentSlot; }
    uint64_t CacheHits() const { return cacheHits; }
    uint64_t DescriptorCreations() const { return descriptorCreations; }
    uint64_t OverflowCount() const { return overflowCount; }

    void ResetStatistics() {
        cacheHits = 0;
        descriptorCreations = 0;
        overflowCount = 0;
        transientPeak = 0;
    }

private:
    std::unordered_map<const void*, uint32_t> persistentLookup;
    uint32_t persistentCount = 0;
    uint32_t transientCount = 0;
    uint32_t transientPeak = 0;
    uint32_t currentSlot = 0;
    // Starts at 1 so a default-constructed SceneMaterial cache (generation 0)
    // is always treated as stale and re-registers on first use.
    uint32_t generation = 1;
    uint64_t cacheHits = 0;
    uint64_t descriptorCreations = 0;
    uint64_t overflowCount = 0;
};
