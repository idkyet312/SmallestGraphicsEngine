#pragma once

// Suballocator for the visibility buffer's persistent vertex/index/triangle
// arrays.
//
// RegisterMesh is upload-once: a mesh keeps its range for as long as it is
// registered. That is right for level geometry, but destruction rebuilds its
// merged chunk batches every time a fracture changes an actor's chunk set, and
// each rebuild produces brand-new MeshPrimitives. Without reclamation those
// rebuilds consumed fresh storage until the pool ran dry, after which
// registration failed and the chunks silently stopped rasterising.
//
// Split out of VisibilityBufferDX12 so it can be tested without a D3D device --
// it is deliberately pure integer arithmetic, holding no GPU resources.

#include <algorithm>
#include <cstdint>
#include <vector>

struct VBGeometryRange {
    unsigned int vertexOffset = 0;
    unsigned int vertexCapacity = 0;
    unsigned int indexOffset = 0;
    unsigned int indexCapacity = 0;
    unsigned int triangleOffset = 0;
    unsigned int triangleCapacity = 0;
    // Frame on which this range stopped being referenced. It stays quarantined
    // until enough further frames have retired that no in-flight frame can
    // still be reading it.
    uint64_t retiredFrame = 0;
};

class VisibilityGeometryPool {
public:
    // Smallest remainder worth carving off a recycled range. Below this a
    // split would only produce an entry no mesh can use, so the allocating
    // mesh keeps the slack instead. Tunable: watch FreeRangeCount() for
    // fragmentation and the high-water marks for stranding.
    static const unsigned int kMinSplitVertices = 64;
    static const unsigned int kMinSplitIndices = kMinSplitVertices * 3;
    static const unsigned int kMinSplitTriangles = kMinSplitVertices;

    VisibilityGeometryPool(unsigned int maxVertices, unsigned int maxIndices,
                           unsigned int maxTriangles, unsigned int frameCount)
        : maxVertices_(maxVertices), maxIndices_(maxIndices),
          maxTriangles_(maxTriangles), frameCount_(frameCount) {}

    // Reserve storage for one mesh, reusing a recycled range when one fits.
    // Returns false when the pool is exhausted; the caller must then treat the
    // registration as failed rather than drawing with a bogus offset.
    bool Allocate(unsigned int vertexCount, unsigned int indexCount,
                  unsigned int triangleCount, VBGeometryRange& out) {
        // Best fit, not first fit: destruction cycles through a wide spread of
        // merged-batch sizes, and first fit fragmented the pool by carving big
        // ranges up for small meshes.
        size_t best = free_.size();
        for (size_t i = 0; i < free_.size(); ++i) {
            const VBGeometryRange& range = free_[i];
            if (range.vertexCapacity < vertexCount ||
                range.indexCapacity < indexCount ||
                range.triangleCapacity < triangleCount)
                continue;
            if (best == free_.size() ||
                range.vertexCapacity < free_[best].vertexCapacity)
                best = i;
        }
        if (best != free_.size()) {
            const VBGeometryRange range = free_[best];
            free_.erase(free_.begin() + static_cast<ptrdiff_t>(best));

            out = range;
            out.retiredFrame = 0;

            // Carve the remainder off when it is big enough to serve a later
            // mesh, and keep it otherwise. Splitting unconditionally sheds
            // slivers too small for anything to use; never splitting strands
            // the tail permanently, which is what bled the pool dry: a small
            // destruction chunk claiming a large retired house range kept the
            // whole thing, and the high-water mark ratcheted until Allocate
            // failed and the chunks stopped rasterising.
            //
            // All three dimensions must clear the threshold. A tail that is
            // useless in any one of them is useless outright, since Allocate
            // requires vertices, indices and triangles to fit together.
            const unsigned int vertexRemainder = range.vertexCapacity - vertexCount;
            const unsigned int indexRemainder = range.indexCapacity - indexCount;
            const unsigned int triangleRemainder =
                range.triangleCapacity - triangleCount;
            if (vertexRemainder >= kMinSplitVertices &&
                indexRemainder >= kMinSplitIndices &&
                triangleRemainder >= kMinSplitTriangles) {
                // The head keeps the original offsets. An allocated range's
                // offsets must never move while a mesh holds them -- the DXR
                // hit path and in-flight draws snapshot them.
                out.vertexCapacity = vertexCount;
                out.indexCapacity = indexCount;
                out.triangleCapacity = triangleCount;

                VBGeometryRange tail;
                tail.vertexOffset = range.vertexOffset + vertexCount;
                tail.vertexCapacity = vertexRemainder;
                tail.indexOffset = range.indexOffset + indexCount;
                tail.indexCapacity = indexRemainder;
                tail.triangleOffset = range.triangleOffset + triangleCount;
                tail.triangleCapacity = triangleRemainder;
                // Straight to free_, never retired_: this storage was never
                // handed to the GPU, so it needs no quarantine.
                tail.retiredFrame = 0;
                free_.push_back(tail);
            }
            return true;
        }

        if (vertexHighWater_ + vertexCount > maxVertices_ ||
            indexHighWater_ + indexCount > maxIndices_ ||
            triangleHighWater_ + triangleCount > maxTriangles_)
            return false;

        out = VBGeometryRange{};
        out.vertexOffset = vertexHighWater_;
        out.vertexCapacity = vertexCount;
        out.indexOffset = indexHighWater_;
        out.indexCapacity = indexCount;
        out.triangleOffset = triangleHighWater_;
        out.triangleCapacity = triangleCount;
        vertexHighWater_ += vertexCount;
        indexHighWater_ += indexCount;
        triangleHighWater_ += triangleCount;
        return true;
    }

    // Quarantine a range. It is stamped with the current frame and only becomes
    // reusable once BeginFrame has advanced far enough past it.
    void Release(const VBGeometryRange& range) {
        VBGeometryRange retired = range;
        retired.retiredFrame = frame_;
        retired_.push_back(retired);
    }

    // Advance one frame and return any quarantined ranges that every in-flight
    // frame has now finished reading.
    void BeginFrame() {
        ++frame_;
        if (retired_.empty()) return;
        bool released = false;
        auto stillQuarantined = std::remove_if(retired_.begin(), retired_.end(),
            [&](const VBGeometryRange& range) {
                if (frame_ - range.retiredFrame <=
                    static_cast<uint64_t>(frameCount_))
                    return false;
                free_.push_back(range);
                released = true;
                return true;
            });
        retired_.erase(stillQuarantined, retired_.end());
        if (released) CoalesceFree();
    }

    unsigned int VertexHighWater() const { return vertexHighWater_; }
    unsigned int IndexHighWater() const { return indexHighWater_; }
    unsigned int TriangleHighWater() const { return triangleHighWater_; }
    size_t FreeRangeCount() const { return free_.size(); }
    size_t QuarantinedRangeCount() const { return retired_.size(); }

    void Reset() {
        free_.clear();
        retired_.clear();
        vertexHighWater_ = 0;
        indexHighWater_ = 0;
        triangleHighWater_ = 0;
        frame_ = 0;
    }

private:
    // Merge free ranges that are contiguous in all three arrays back into one
    // entry. Splitting makes the free list fragment over a long session, and a
    // pool holding the right total capacity in pieces too small to use fails
    // allocations just as surely as an empty one.
    //
    // A merge is only valid when the ranges abut in vertices, indices AND
    // triangles. Ranges are carved from the high-water marks in lockstep, so
    // neighbours normally do abut in all three, but a pair that abuts in only
    // one array describes disjoint storage and must be left alone.
    void CoalesceFree() {
        if (free_.size() < 2) return;
        std::sort(free_.begin(), free_.end(),
            [](const VBGeometryRange& a, const VBGeometryRange& b) {
                return a.vertexOffset < b.vertexOffset;
            });
        std::vector<VBGeometryRange> merged;
        merged.reserve(free_.size());
        merged.push_back(free_.front());
        for (size_t i = 1; i < free_.size(); ++i) {
            VBGeometryRange& back = merged.back();
            const VBGeometryRange& next = free_[i];
            if (back.vertexOffset + back.vertexCapacity == next.vertexOffset &&
                back.indexOffset + back.indexCapacity == next.indexOffset &&
                back.triangleOffset + back.triangleCapacity ==
                    next.triangleOffset) {
                back.vertexCapacity += next.vertexCapacity;
                back.indexCapacity += next.indexCapacity;
                back.triangleCapacity += next.triangleCapacity;
                continue;
            }
            merged.push_back(next);
        }
        free_.swap(merged);
    }

    std::vector<VBGeometryRange> free_;
    std::vector<VBGeometryRange> retired_;
    unsigned int maxVertices_ = 0;
    unsigned int maxIndices_ = 0;
    unsigned int maxTriangles_ = 0;
    unsigned int frameCount_ = 2;
    // High-water marks for storage never yet handed out. Freed ranges come back
    // through free_ rather than by rewinding these -- rewinding would move
    // offsets for meshes the GPU is still reading.
    unsigned int vertexHighWater_ = 0;
    unsigned int indexHighWater_ = 0;
    unsigned int triangleHighWater_ = 0;
    uint64_t frame_ = 0;
};
