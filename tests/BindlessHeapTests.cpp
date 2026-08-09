// CPU-only tests for the bindless descriptor index allocator. No D3D device is
// created, so this runs anywhere -- the allocator is deliberately pure index
// arithmetic for exactly this reason.

#include "BindlessHeapDX12.h"
#include <iostream>
#include <vector>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

// Distinct fake resource pointers. Only their identity matters.
static const void* FakeResource(size_t i) {
    // Allocator tests compare identity only and never dereference resources.
    // Spacing the synthetic values avoids null while allowing the overflow
    // test to cover all 57,280 persistent entries.
    return reinterpret_cast<const void*>((i + 1u) * 16u);
}

int main() {
    // --- Range boundaries -------------------------------------------------
    {
        CHECK(BINDLESS_FALLBACK_COUNT == 4);
        CHECK(BINDLESS_SYSTEM_BEGIN == BINDLESS_FALLBACK_COUNT);
        CHECK(BINDLESS_PERSISTENT_BEGIN == BINDLESS_SYSTEM_END);
        CHECK(BINDLESS_PERSISTENT_CAPACITY == 57344 - 64);
        // The two transient slices must exactly fill the heap tail, with no gap
        // and no overrun past the end of the heap.
        CHECK(BINDLESS_TRANSIENT_BEGIN == BINDLESS_PERSISTENT_END);
        CHECK(BINDLESS_TRANSIENT_BEGIN +
              BINDLESS_TRANSIENT_SLICES * BINDLESS_TRANSIENT_SLICE_SIZE ==
              BINDLESS_HEAP_SIZE);
        CHECK(BindlessDescriptorAllocator::TransientSliceBegin(0) == 57344);
        CHECK(BindlessDescriptorAllocator::TransientSliceBegin(1) == 61440);

        BindlessDescriptorAllocator allocator;
        bool created = false;
        // First persistent registration lands exactly at the range start, not
        // in the reserved system region.
        const uint32_t first =
            allocator.RegisterPersistent(FakeResource(0), BINDLESS_FALLBACK_WHITE, &created);
        CHECK(first == BINDLESS_PERSISTENT_BEGIN);
        CHECK(created);
        const uint32_t second =
            allocator.RegisterPersistent(FakeResource(1), BINDLESS_FALLBACK_WHITE, &created);
        CHECK(second == BINDLESS_PERSISTENT_BEGIN + 1);
        CHECK(created);
    }

    // --- Resource deduplication -------------------------------------------
    {
        BindlessDescriptorAllocator allocator;
        bool created = false;
        const uint32_t a =
            allocator.RegisterPersistent(FakeResource(7), BINDLESS_FALLBACK_WHITE, &created);
        CHECK(created);
        // Re-registering the same resource must return the same descriptor and
        // must NOT report a creation -- this is what stops thousands of
        // identical chunk albedos from each burning a descriptor.
        const uint32_t b =
            allocator.RegisterPersistent(FakeResource(7), BINDLESS_FALLBACK_WHITE, &created);
        CHECK(a == b);
        CHECK(!created);
        CHECK(allocator.CacheHits() == 1);
        CHECK(allocator.DescriptorCreations() == 1);
        CHECK(allocator.PersistentCount() == 1);

        CHECK(allocator.FindPersistent(FakeResource(7)) == a);
        CHECK(allocator.FindPersistent(FakeResource(8)) == BINDLESS_INVALID_INDEX);

        // A null resource is not an error; it resolves to the fallback and
        // allocates nothing.
        CHECK(allocator.RegisterPersistent(nullptr, BINDLESS_FALLBACK_NORMAL, &created) ==
              BINDLESS_FALLBACK_NORMAL);
        CHECK(!created);
        CHECK(allocator.PersistentCount() == 1);
    }

    // --- Frame-slice isolation --------------------------------------------
    {
        BindlessDescriptorAllocator allocator;
        allocator.BeginTransientFrame(0);
        const uint32_t slot0 = allocator.AllocateTransient(16);
        CHECK(slot0 == BindlessDescriptorAllocator::TransientSliceBegin(0));

        allocator.BeginTransientFrame(1);
        const uint32_t slot1 = allocator.AllocateTransient(16);
        CHECK(slot1 == BindlessDescriptorAllocator::TransientSliceBegin(1));
        // The two frames' allocations must not overlap, or writing this frame's
        // descriptors would stomp descriptors the previous frame still reads.
        CHECK(slot1 >= slot0 + 16);

        // Returning to slot 0 restarts that slice from its base.
        allocator.BeginTransientFrame(2);   // parity 0
        CHECK(allocator.CurrentSlot() == 0);
        CHECK(allocator.AllocateTransient(16) ==
              BindlessDescriptorAllocator::TransientSliceBegin(0));

        // Contiguity: a table of N descriptors occupies N consecutive slots.
        const uint32_t base = allocator.AllocateTransient(8);
        CHECK(base == BindlessDescriptorAllocator::TransientSliceBegin(0) + 16);
        CHECK(allocator.TransientCount() == 24);
    }

    // --- Overflow falls back, never corrupts ------------------------------
    {
        BindlessDescriptorAllocator allocator;
        allocator.BeginTransientFrame(0);
        // Exactly filling the slice is legal.
        CHECK(allocator.AllocateTransient(BINDLESS_TRANSIENT_SLICE_SIZE) !=
              BINDLESS_INVALID_INDEX);
        // One more descriptor overflows and reports it.
        CHECK(allocator.AllocateTransient(1) == BINDLESS_INVALID_INDEX);
        CHECK(allocator.OverflowCount() == 1);
        // A fresh frame recovers the full slice rather than staying broken.
        allocator.BeginTransientFrame(1);
        CHECK(allocator.AllocateTransient(1) != BINDLESS_INVALID_INDEX);

        CHECK(allocator.AllocateTransient(0) == BINDLESS_INVALID_INDEX);
    }

    {
        BindlessDescriptorAllocator allocator;
        bool created = false;
        for (uint32_t i = 0; i < BINDLESS_PERSISTENT_CAPACITY; ++i) {
            const uint32_t index = allocator.RegisterPersistent(
                FakeResource(i), BINDLESS_FALLBACK_WHITE, &created);
            CHECK(index == BINDLESS_PERSISTENT_BEGIN + i);
            CHECK(created);
        }
        const uint32_t overflow = allocator.RegisterPersistent(
            FakeResource(BINDLESS_PERSISTENT_CAPACITY),
            BINDLESS_FALLBACK_NORMAL, &created);
        CHECK(overflow == BINDLESS_FALLBACK_NORMAL);
        CHECK(!created);
        CHECK(allocator.PersistentCount() == BINDLESS_PERSISTENT_CAPACITY);
        CHECK(allocator.OverflowCount() == 1);
    }

    // --- Generation invalidation ------------------------------------------
    {
        BindlessDescriptorAllocator allocator;
        const uint32_t generationBefore = allocator.Generation();
        // Generation starts non-zero so a default-initialised material cache
        // (generation 0) never looks valid.
        CHECK(generationBefore != 0);

        bool created = false;
        const uint32_t index =
            allocator.RegisterPersistent(FakeResource(3), BINDLESS_FALLBACK_WHITE, &created);
        CHECK(created);
        CHECK(allocator.IsLiveIndex(index));

        allocator.ResetPersistent();
        CHECK(allocator.Generation() != generationBefore);
        CHECK(allocator.PersistentCount() == 0);
        // The old index is no longer live, and the old resource must
        // re-register (returning a freshly created descriptor) rather than
        // silently reusing a slot that no longer holds its view.
        CHECK(allocator.FindPersistent(FakeResource(3)) == BINDLESS_INVALID_INDEX);
        const uint32_t reindexed =
            allocator.RegisterPersistent(FakeResource(3), BINDLESS_FALLBACK_WHITE, &created);
        CHECK(created);
        CHECK(reindexed == BINDLESS_PERSISTENT_BEGIN);
    }

    // --- Live-index validation --------------------------------------------
    {
        BindlessDescriptorAllocator allocator;
        // Fallbacks are always live, including before anything is registered.
        CHECK(allocator.IsLiveIndex(BINDLESS_FALLBACK_WHITE));
        CHECK(allocator.IsLiveIndex(BINDLESS_FALLBACK_BLACK));
        // Reserved-system and unallocated persistent slots are not live.
        CHECK(!allocator.IsLiveIndex(BINDLESS_SYSTEM_BEGIN));
        CHECK(!allocator.IsLiveIndex(BINDLESS_PERSISTENT_BEGIN));
        CHECK(!allocator.IsLiveIndex(BINDLESS_INVALID_INDEX));

        allocator.BeginTransientFrame(0);
        const uint32_t transient = allocator.AllocateTransient(4);
        CHECK(allocator.IsLiveIndex(transient));
        CHECK(allocator.IsLiveIndex(transient + 3));
        CHECK(!allocator.IsLiveIndex(transient + 4));
        // Descriptors allocated in the other slice are not live for this frame.
        CHECK(!allocator.IsLiveIndex(
            BindlessDescriptorAllocator::TransientSliceBegin(1)));
    }

    if (failures == 0) {
        std::cout << "BindlessHeapTests passed\n";
        return 0;
    }
    std::cerr << "BindlessHeapTests: " << failures << " failure(s)\n";
    return 1;
}
