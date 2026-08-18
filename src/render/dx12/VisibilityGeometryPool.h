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
            out = free_[best];
            out.retiredFrame = 0;
            // The mesh keeps the whole range. Splitting off the remainder would
            // shed slivers too small for any later mesh to use.
            free_.erase(free_.begin() + static_cast<ptrdiff_t>(best));
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
        auto stillQuarantined = std::remove_if(retired_.begin(), retired_.end(),
            [&](const VBGeometryRange& range) {
                if (frame_ - range.retiredFrame <=
                    static_cast<uint64_t>(frameCount_))
                    return false;
                free_.push_back(range);
                return true;
            });
        retired_.erase(stillQuarantined, retired_.end());
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
