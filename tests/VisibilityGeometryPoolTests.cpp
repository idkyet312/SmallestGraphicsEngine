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
        VBGeometryRange big, small, spacer;
        CHECK(pool.Allocate(400, 1200, 400, big));
        CHECK(pool.Allocate(100, 300, 100, small));
        // Spacer stays live so the two freed ranges cannot be coalesced by
        // adjacency assumptions -- the pool does not coalesce at all.
        CHECK(pool.Allocate(10, 30, 10, spacer));
        pool.Release(big);
        pool.Release(small);
        RetireQuarantine(pool, kFrameCount);
        CHECK(pool.FreeRangeCount() == 2);

        VBGeometryRange fit;
        CHECK(pool.Allocate(90, 270, 90, fit));
        // Must take the 100-vertex range, leaving the 400 one for a big mesh.
        CHECK(fit.vertexOffset == small.vertexOffset);

        VBGeometryRange large;
        CHECK(pool.Allocate(400, 1200, 400, large));
        CHECK(large.vertexOffset == big.vertexOffset);
        CHECK(pool.FreeRangeCount() == 0);
    }

    // --- A reused range keeps its original capacity ------------------------
    // Releasing must return the capacity, not the smaller live count, or
    // repeated reuse would ratchet a range down toward zero.
    {
        VisibilityGeometryPool pool(1000, 3000, 1000, kFrameCount);
        VBGeometryRange original, reused, again;
        CHECK(pool.Allocate(400, 1200, 400, original));
        pool.Release(original);
        RetireQuarantine(pool, kFrameCount);

        // A much smaller mesh takes the big range and inherits its capacity.
        CHECK(pool.Allocate(10, 30, 10, reused));
        CHECK(reused.vertexCapacity == 400);
        pool.Release(reused);
        RetireQuarantine(pool, kFrameCount);

        // The full 400 must still be available afterwards.
        CHECK(pool.Allocate(400, 1200, 400, again));
        CHECK(again.vertexOffset == original.vertexOffset);
        CHECK(pool.VertexHighWater() == 400);
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
