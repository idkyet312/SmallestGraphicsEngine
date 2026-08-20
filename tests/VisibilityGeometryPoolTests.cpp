// CPU-only tests for the visibility buffer's geometry suballocator. No D3D
// device is created -- the pool is deliberately pure integer arithmetic so the
// recycling behaviour destruction depends on can be tested anywhere.

#include "VisibilityGeometryPool.h"
#include <iostream>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

namespace {

// Push a pool past the quarantine window so released ranges become reusable.
void RetireQuarantine(VisibilityGeometryPool& pool, unsigned int frameCount) {
    for (unsigned int i = 0; i <= frameCount + 1u; ++i) pool.BeginFrame();
}

} // namespace

int main() {
    constexpr unsigned int kFrameCount = 2;

    // --- Fresh allocations come off the high-water mark, packed ------------
    {
        VisibilityGeometryPool pool(1000, 3000, 1000, kFrameCount);
        VBGeometryRange a, b;
        CHECK(pool.Allocate(100, 300, 100, a));
        CHECK(pool.Allocate(50, 150, 50, b));
        CHECK(a.vertexOffset == 0);
        CHECK(b.vertexOffset == 100);
        CHECK(a.indexOffset == 0);
        CHECK(b.indexOffset == 300);
        CHECK(pool.VertexHighWater() == 150);
        CHECK(pool.FreeRangeCount() == 0);
    }

    // --- Exhaustion is reported, not silently mis-allocated ---------------
    {
        VisibilityGeometryPool pool(100, 300, 100, kFrameCount);
        VBGeometryRange a, b;
        CHECK(pool.Allocate(100, 300, 100, a));
        // No room left: the caller must see this and drop the registration.
        CHECK(!pool.Allocate(1, 3, 1, b));
        // A failed allocation must not consume storage.
        CHECK(pool.VertexHighWater() == 100);
    }

    // --- A released range is NOT reusable until quarantine expires ---------
    // This is the correctness constraint: handing storage back while a frame in
    // flight still reads it would corrupt geometry rather than merely leak.
    {
        VisibilityGeometryPool pool(100, 300, 100, kFrameCount);
        VBGeometryRange a, b;
        CHECK(pool.Allocate(100, 300, 100, a));
        pool.Release(a);
        CHECK(pool.QuarantinedRangeCount() == 1);
        CHECK(pool.FreeRangeCount() == 0);
        // Immediately after release the pool is still full.
        CHECK(!pool.Allocate(100, 300, 100, b));
        pool.BeginFrame();
        CHECK(!pool.Allocate(100, 300, 100, b));
        RetireQuarantine(pool, kFrameCount);
        CHECK(pool.FreeRangeCount() == 1);
        CHECK(pool.Allocate(100, 300, 100, b));
        // Reuses the same storage rather than growing the pool.
        CHECK(b.vertexOffset == a.vertexOffset);
        CHECK(pool.VertexHighWater() == 100);
    }

    // --- Repeated rebuild cycles must not grow the pool -------------------
    // This is the destruction case: every fracture rebuilds a merged batch,
    // registering new geometry and retiring the old. Before recycling existed
    // this walked the high-water mark to the cap and chunks stopped drawing.
    {
        VisibilityGeometryPool pool(1000, 3000, 1000, kFrameCount);
        VBGeometryRange live;
        CHECK(pool.Allocate(400, 1200, 400, live));
        const unsigned int afterFirst = pool.VertexHighWater();
        for (int cycle = 0; cycle < 200; ++cycle) {
            pool.Release(live);
            RetireQuarantine(pool, kFrameCount);
            VBGeometryRange next;
            CHECK(pool.Allocate(400, 1200, 400, next));
            live = next;
        }
        // 200 rebuilds of a mesh that fits once: storage must be flat.
        CHECK(pool.VertexHighWater() == afterFirst);
    }

    // --- Best fit picks the tightest range that fits ----------------------
    {
        VisibilityGeometryPool pool(1000, 3000, 1000, kFrameCount);
        VBGeometryRange big, spacer, small;
        CHECK(pool.Allocate(400, 1200, 400, big));
        // Spacer sits BETWEEN the two and stays live, so the freed ranges are
        // genuinely non-adjacent and coalescing cannot merge them.
        CHECK(pool.Allocate(10, 30, 10, spacer));
        CHECK(pool.Allocate(100, 300, 100, small));
        pool.Release(big);
        pool.Release(small);
        RetireQuarantine(pool, kFrameCount);
        CHECK(pool.FreeRangeCount() == 2);

        // Takes the 100-vertex range, not the 400 one. The remainder is only
        // 10 vertices, below the split threshold, so no tail is shed.
        VBGeometryRange fit;
        CHECK(pool.Allocate(90, 270, 90, fit));
        CHECK(fit.vertexOffset == small.vertexOffset);
        CHECK(pool.FreeRangeCount() == 1);

        VBGeometryRange large;
        CHECK(pool.Allocate(400, 1200, 400, large));
        CHECK(large.vertexOffset == big.vertexOffset);
        CHECK(pool.FreeRangeCount() == 0);
    }

    // --- A reused range keeps capacity when the remainder is a sliver -------
    // Releasing must return the capacity, not the smaller live count, or
    // repeated reuse would ratchet a range down toward zero. A remainder below
    // the split threshold stays with the mesh rather than becoming an
    // unusable free entry.
    {
        VisibilityGeometryPool pool(1000, 3000, 1000, kFrameCount);
        VBGeometryRange original, reused, again;
        CHECK(pool.Allocate(400, 1200, 400, original));
        pool.Release(original);
        RetireQuarantine(pool, kFrameCount);

        // Remainder here is 400-380=20 vertices, under kMinSplitVertices, so
        // the mesh keeps the whole range and no sliver is shed.
        CHECK(pool.Allocate(380, 1140, 380, reused));
        CHECK(reused.vertexCapacity == 400);
        CHECK(pool.FreeRangeCount() == 0);
        pool.Release(reused);
        RetireQuarantine(pool, kFrameCount);

        // The full 400 must still be available afterwards.
        CHECK(pool.Allocate(400, 1200, 400, again));
        CHECK(again.vertexOffset == original.vertexOffset);
        CHECK(pool.VertexHighWater() == 400);
    }

    // --- A large remainder is split off and stays usable -------------------
    // The destruction leak: a small chunk claiming a big retired house range
    // used to strand the remainder permanently. The tail must return to the
    // free list so later meshes can still use it.
    {
        VisibilityGeometryPool pool(1000, 3000, 1000, kFrameCount);
        VBGeometryRange big, small, tail;
        CHECK(pool.Allocate(800, 2400, 800, big));
        pool.Release(big);
        RetireQuarantine(pool, kFrameCount);
        CHECK(pool.FreeRangeCount() == 1);

        // Small mesh takes only what it needs; the rest comes back as one
        // free range rather than being swallowed.
        CHECK(pool.Allocate(100, 300, 100, small));
        CHECK(small.vertexCapacity == 100);
        CHECK(pool.FreeRangeCount() == 1);
        // The head keeps the original offsets -- an allocated range's offsets
        // must never move while a mesh holds them.
        CHECK(small.vertexOffset == big.vertexOffset);
        CHECK(small.indexOffset == big.indexOffset);
        CHECK(small.triangleOffset == big.triangleOffset);

        // The tail is immediately usable and sits after the head, with no
        // growth in the high-water mark.
        CHECK(pool.Allocate(700, 2100, 700, tail));
        CHECK(tail.vertexOffset == big.vertexOffset + 100);
        CHECK(pool.VertexHighWater() == 800);
    }

    // --- Batch <-> per-chunk churn must not exhaust the pool ---------------
    // The actual destruction flicker. Each spatial cell oscillates between one
    // merged batch primitive and N individual chunk primitives. Without
    // splitting, every small chunk strands a whole merged-batch range and the
    // pool bleeds capacity until Allocate fails -- which flips the house
    // between the visibility and forward passes and reads as a flicker.
    //
    // Sizes must VARY. Uniform sizes hide the bug: the freed chunk ranges then
    // exactly satisfy the next cycle's requests and even the unfixed allocator
    // reaches a steady state. Real fractures produce a spread, and measuring
    // the unfixed pool under a spread showed the high-water mark climbing
    // 3400 -> 10516 over 400 cycles and still rising.
    {
        VisibilityGeometryPool pool(1024u * 1024u, 3u * 1024u * 1024u,
                                    1024u * 1024u, kFrameCount);
        // Deterministic LCG: a fixed spread of sizes, reproducible across runs.
        unsigned int seed = 12345u;
        auto rnd = [&](unsigned int lo, unsigned int hi) {
            seed = seed * 1103515245u + 12345u;
            return lo + (seed >> 16) % (hi - lo + 1u);
        };
        unsigned int highWaterAfterWarmup = 0;
        for (int cycle = 0; cycle < 400; ++cycle) {
            // Merged representation: one big primitive for the whole cell.
            VBGeometryRange merged;
            const unsigned int mergedVerts = rnd(800, 1600);
            CHECK(pool.Allocate(mergedVerts, mergedVerts * 3, mergedVerts,
                                merged));
            pool.Release(merged);
            RetireQuarantine(pool, kFrameCount);

            // Fallback representation: one primitive per chunk.
            const unsigned int chunkCount = rnd(6, 16);
            VBGeometryRange chunks[16];
            for (unsigned int i = 0; i < chunkCount; ++i) {
                const unsigned int verts = rnd(40, 220);
                CHECK(pool.Allocate(verts, verts * 3, verts, chunks[i]));
            }
            for (unsigned int i = 0; i < chunkCount; ++i)
                pool.Release(chunks[i]);
            RetireQuarantine(pool, kFrameCount);

            // Let the first cycles establish a working set large enough to
            // absorb the size spread, then require the mark to hold flat for
            // the remaining ~75% of the run. A leak shows as continued growth,
            // not as a single early plateau. Measured: the fixed pool settles
            // at 2684 vertices by cycle ~75 and never moves again; the unfixed
            // one passed 10516 by cycle 400 and was still climbing.
            if (cycle == 100) highWaterAfterWarmup = pool.VertexHighWater();
        }
        CHECK(highWaterAfterWarmup > 0);
        CHECK(pool.VertexHighWater() == highWaterAfterWarmup);
        // Coalescing must keep the free list from fragmenting into unusable
        // pieces over a long session.
        CHECK(pool.FreeRangeCount() <= 4);
    }

    // --- Index and triangle capacity are honoured independently -----------
    // A range can be wide enough in vertices but too small in indices.
    {
        VisibilityGeometryPool pool(1000, 3000, 1000, kFrameCount);
        VBGeometryRange thin, live;
        // Few indices per vertex.
        CHECK(pool.Allocate(200, 30, 10, thin));
        CHECK(pool.Allocate(10, 30, 10, live));
        pool.Release(thin);
        RetireQuarantine(pool, kFrameCount);
        CHECK(pool.FreeRangeCount() == 1);

        // Fits on vertices but needs far more indices: must not reuse.
        VBGeometryRange dense;
        CHECK(pool.Allocate(150, 900, 300, dense));
        CHECK(dense.vertexOffset != thin.vertexOffset);
        CHECK(pool.FreeRangeCount() == 1);
    }

    // --- Reset returns the pool to empty ----------------------------------
    {
        VisibilityGeometryPool pool(1000, 3000, 1000, kFrameCount);
        VBGeometryRange a;
        CHECK(pool.Allocate(400, 1200, 400, a));
        pool.Release(a);
        pool.Reset();
        CHECK(pool.VertexHighWater() == 0);
        CHECK(pool.FreeRangeCount() == 0);
        CHECK(pool.QuarantinedRangeCount() == 0);
        VBGeometryRange b;
        CHECK(pool.Allocate(1000, 3000, 1000, b));
        CHECK(b.vertexOffset == 0);
    }

    if (failures == 0) std::cout << "VisibilityGeometryPoolTests passed\n";
    return failures == 0 ? 0 : 1;
}
