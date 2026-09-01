// One randomized cut plane used to break an intact authored chunk apart at
// runtime, expressed in that chunk's model space.
//
// Split out of DestructionDX12.cpp so the geometry can be tested without a
// D3D device, a Blast framework or any game content: the property that matters
// -- that the 2^N fragments tile the source volume exactly, with no sample left
// unassigned and none claimed twice -- is pure arithmetic.

#pragma once

#include <array>

namespace SGE {

// A fragment keeps the side of plane N selected by bit N of its index, so the
// set of fragments partitions the source exactly.
struct RuntimeCutPlane {
    bool horizontal = true;   // false = a vertical plane splitting left/right
    bool alongAxisX = true;   // which horizontal axis carries the tilt
    float offset = 0.0f;      // position on the plane's primary axis
    float centre = 0.0f;      // origin of the tilt on the secondary axis
    float tiltCosine = 1.0f;
    float tiltSine = 0.0f;

    // Vertex layout is the engine's packed 12-float format; only position is
    // read here, but the full stride is kept so callers can pass their vertices
    // through unchanged.
    float Distance(const std::array<float, 12>& vertex) const {
        // A horizontal plane is positioned by height and leans along a
        // horizontal axis; a vertical plane is the transpose of that.
        const float horizontalAxis = alongAxisX ? vertex[0] : vertex[2];
        const float primary = horizontal ? vertex[1] : horizontalAxis;
        const float secondary = horizontal ? horizontalAxis : vertex[1];
        return (primary - offset) * tiltCosine +
               (secondary - centre) * tiltSine;
    }

    // True when `vertex` is on the side this fragment keeps for this plane.
    bool Keeps(const std::array<float, 12>& vertex, bool keepBelow) const {
        const float distance = Distance(vertex);
        return keepBelow ? distance <= 0.0f : distance >= 0.0f;
    }
};

}  // namespace SGE
