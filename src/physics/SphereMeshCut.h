// Runtime sphere boolean: subtracts a sphere from a triangle mesh, leaving a
// bowl-shaped hole with a capped rim so the result is not see-through.
//
// Split out of the game code so the geometry can be tested without a D3D
// device or any game content. The properties that matter are pure arithmetic:
// no surviving triangle intersects the sphere's interior, geometry outside the
// sphere is preserved exactly, and the crater rim is closed.
//
// The cut runs in the mesh's own local space, so a result can be cut again.

#pragma once

#include <array>
#include <cmath>
#include <vector>

namespace SGE {

// Engine packed vertex: Pos(3), Normal(3), Tex(2), Tangent(4).
inline constexpr int kSphereCutVertexStride = 12;
using SphereCutVertex = std::array<float, kSphereCutVertexStride>;

struct SphereCut {
    float centreX = 0.0f;
    float centreY = 0.0f;
    float centreZ = 0.0f;
    float radius = 1.0f;

    float DistanceSq(const SphereCutVertex& vertex) const {
        const float dx = vertex[0] - centreX;
        const float dy = vertex[1] - centreY;
        const float dz = vertex[2] - centreZ;
        return dx * dx + dy * dy + dz * dz;
    }

    bool Contains(const SphereCutVertex& vertex) const {
        return DistanceSq(vertex) < radius * radius;
    }

    // Signed distance to the surface: negative inside, positive outside. Used
    // to find where an edge crosses the sphere so the cut lands on the surface
    // rather than on the nearest original vertex.
    float SignedDistance(const SphereCutVertex& vertex) const {
        return std::sqrt(DistanceSq(vertex)) - radius;
    }
};

// Interpolates every channel, then pushes the position exactly onto the
// sphere. Interpolation alone lands slightly inside on a curved surface, which
// would leave a visible crack between the wall and its cap.
SphereCutVertex CrossingVertex(const SphereCutVertex& outside,
                               const SphereCutVertex& inside,
                               const SphereCut& sphere);

// Removes the part of `vertices`/`indices` inside `sphere`, appending the
// result to the out-parameters. Triangles straddling the surface are clipped;
// the opening is capped with a rim fan so the hole reads as a crater rather
// than a window into an empty shell.
//
// Returns false when the sphere misses the mesh entirely, in which case the
// outputs are left untouched and the caller should keep the original geometry.
bool CutSphereFromMesh(const std::vector<float>& vertices,
                       const std::vector<unsigned int>& indices,
                       const SphereCut& sphere,
                       std::vector<float>& outVertices,
                       std::vector<unsigned int>& outIndices);

}  // namespace SGE
