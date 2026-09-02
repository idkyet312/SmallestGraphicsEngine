#include "SphereMeshCut.h"

#include <algorithm>
#include <cstddef>

namespace SGE {
namespace {

void Append(std::vector<float>& vertices, std::vector<unsigned int>& indices,
            const SphereCutVertex& a, const SphereCutVertex& b,
            const SphereCutVertex& c) {
    const unsigned int base =
        static_cast<unsigned int>(vertices.size() / kSphereCutVertexStride);
    for (const SphereCutVertex* vertex : { &a, &b, &c })
        vertices.insert(vertices.end(), vertex->begin(), vertex->end());
    indices.push_back(base);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
}

SphereCutVertex Read(const std::vector<float>& vertices, unsigned int index) {
    SphereCutVertex vertex{};
    const size_t offset = static_cast<size_t>(index) * kSphereCutVertexStride;
    for (int i = 0; i < kSphereCutVertexStride; ++i)
        vertex[i] = vertices[offset + i];
    return vertex;
}

// A rim point plus the angle it sits at around the crater, so the cap fan can
// be wound in order rather than in whatever order triangles were clipped.
struct RimPoint {
    SphereCutVertex vertex;
    float angle = 0.0f;
};

}  // namespace

SphereCutVertex CrossingVertex(const SphereCutVertex& outside,
                               const SphereCutVertex& inside,
                               const SphereCut& sphere) {
    const float outsideDistance = sphere.SignedDistance(outside);
    const float insideDistance = sphere.SignedDistance(inside);
    const float span = outsideDistance - insideDistance;
    // Both ends on the surface: nothing to interpolate along.
    const float t = std::abs(span) < 1e-8f
        ? 0.0f
        : std::clamp(outsideDistance / span, 0.0f, 1.0f);

    SphereCutVertex crossing{};
    for (int i = 0; i < kSphereCutVertexStride; ++i)
        crossing[i] = outside[i] + (inside[i] - outside[i]) * t;

    // Snap onto the sphere so the wall meets its cap with no crack.
    const float dx = crossing[0] - sphere.centreX;
    const float dy = crossing[1] - sphere.centreY;
    const float dz = crossing[2] - sphere.centreZ;
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length > 1e-8f) {
        const float scale = sphere.radius / length;
        crossing[0] = sphere.centreX + dx * scale;
        crossing[1] = sphere.centreY + dy * scale;
        crossing[2] = sphere.centreZ + dz * scale;
    }
    return crossing;
}

bool CutSphereFromMesh(const std::vector<float>& vertices,
                       const std::vector<unsigned int>& indices,
                       const SphereCut& sphere,
                       std::vector<float>& outVertices,
                       std::vector<unsigned int>& outIndices) {
    if (vertices.empty() || indices.size() < 3 || !(sphere.radius > 0.0f))
        return false;

    std::vector<float> keptVertices;
    std::vector<unsigned int> keptIndices;
    std::vector<RimPoint> rim;
    bool cutAnything = false;

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const SphereCutVertex triangle[3] = {
            Read(vertices, indices[i]),
            Read(vertices, indices[i + 1]),
            Read(vertices, indices[i + 2])
        };
        const bool inside[3] = {
            sphere.Contains(triangle[0]),
            sphere.Contains(triangle[1]),
            sphere.Contains(triangle[2])
        };
        const int insideCount = (inside[0] ? 1 : 0) + (inside[1] ? 1 : 0) +
                                (inside[2] ? 1 : 0);

        if (insideCount == 0) {
            Append(keptVertices, keptIndices, triangle[0], triangle[1],
                   triangle[2]);
            continue;
        }
        cutAnything = true;
        // Wholly swallowed: contributes nothing but the hole it leaves.
        if (insideCount == 3) continue;

        // Rotate so the odd vertex out is first, keeping the original winding
        // so the clipped pieces face the same way as the triangle they came
        // from. With one vertex inside the survivor is a quad (two triangles);
        // with two inside it is a single corner triangle.
        const bool oneInside = insideCount == 1;
        int pivot = 0;
        for (int corner = 0; corner < 3; ++corner)
            if (inside[corner] == oneInside) { pivot = corner; break; }

        const SphereCutVertex& a = triangle[pivot];
        const SphereCutVertex& b = triangle[(pivot + 1) % 3];
        const SphereCutVertex& c = triangle[(pivot + 2) % 3];

        if (oneInside) {
            // a is inside; b and c survive along with the two crossings.
            const SphereCutVertex ab = CrossingVertex(b, a, sphere);
            const SphereCutVertex ac = CrossingVertex(c, a, sphere);
            Append(keptVertices, keptIndices, ab, b, c);
            Append(keptVertices, keptIndices, ab, c, ac);
            rim.push_back({ ab, 0.0f });
            rim.push_back({ ac, 0.0f });
        } else {
            // a is the only survivor; b and c are inside.
            const SphereCutVertex ab = CrossingVertex(a, b, sphere);
            const SphereCutVertex ac = CrossingVertex(a, c, sphere);
            Append(keptVertices, keptIndices, a, ab, ac);
            rim.push_back({ ab, 0.0f });
            rim.push_back({ ac, 0.0f });
        }
    }

    if (!cutAnything) return false;

    // Cap the opening. The rim points all lie on the sphere, so a fan closes
    // the hole as a bowl: cheap, watertight for a convex cut, and it reads as
    // blasted-out material rather than a window into an empty shell.
    if (rim.size() >= 3) {
        SphereCutVertex hub{};
        for (const RimPoint& point : rim)
            for (int i = 0; i < kSphereCutVertexStride; ++i)
                hub[i] += point.vertex[i];
        const float inverse = 1.0f / static_cast<float>(rim.size());
        for (int i = 0; i < kSphereCutVertexStride; ++i) hub[i] *= inverse;

        // The bowl axis is the rim plane normal, not the offset of the rim
        // centroid from the sphere centre. For a flat wall cut through the
        // middle those two coincide, so the centroid offset is ~zero and gives
        // no usable direction; the rim plane still has a well-defined normal.
        // Fitted by summing Newell area terms around the ordered-enough rim.
        float axisX = 0.0f, axisY = 0.0f, axisZ = 0.0f;
        for (size_t i = 0; i < rim.size(); ++i) {
            const SphereCutVertex& current = rim[i].vertex;
            const SphereCutVertex& next = rim[(i + 1) % rim.size()].vertex;
            axisX += (current[1] - next[1]) * (current[2] + next[2]);
            axisY += (current[2] - next[2]) * (current[0] + next[0]);
            axisZ += (current[0] - next[0]) * (current[1] + next[1]);
        }
        float axisLengthFit =
            std::sqrt(axisX * axisX + axisY * axisY + axisZ * axisZ);
        if (axisLengthFit <= 1e-6f) {
            // Degenerate Newell fit (rim points effectively collinear): fall
            // back to the centroid offset, which is valid for a curved surface.
            axisX = hub[0] - sphere.centreX;
            axisY = hub[1] - sphere.centreY;
            axisZ = hub[2] - sphere.centreZ;
            axisLengthFit =
                std::sqrt(axisX * axisX + axisY * axisY + axisZ * axisZ);
        }

        // Sink the hub along that axis so the cap is a bowl rather than a flat
        // lid sitting in the plane of the rim.
        if (axisLengthFit > 1e-6f) {
            const float depth = sphere.radius * 0.55f / axisLengthFit;
            hub[0] -= axisX * depth;
            hub[1] -= axisY * depth;
            hub[2] -= axisZ * depth;
        }

        // Order the rim around the hole so the fan does not self-intersect.
        // The cut is convex, so an angle about the bowl axis suffices.
        if (axisLengthFit > 1e-6f) {
            axisX /= axisLengthFit;
            axisY /= axisLengthFit;
            axisZ /= axisLengthFit;
            float tangentX = 0.0f, tangentY = 0.0f, tangentZ = 0.0f;
            if (std::abs(axisY) < 0.9f) { tangentX = -axisZ; tangentZ = axisX; }
            else { tangentY = -axisZ; tangentZ = axisY; }
            const float tangentLength = std::sqrt(
                tangentX * tangentX + tangentY * tangentY + tangentZ * tangentZ);
            if (tangentLength > 1e-6f) {
                tangentX /= tangentLength;
                tangentY /= tangentLength;
                tangentZ /= tangentLength;
                const float bitangentX = axisY * tangentZ - axisZ * tangentY;
                const float bitangentY = axisZ * tangentX - axisX * tangentZ;
                const float bitangentZ = axisX * tangentY - axisY * tangentX;
                for (RimPoint& point : rim) {
                    const float px = point.vertex[0] - sphere.centreX;
                    const float py = point.vertex[1] - sphere.centreY;
                    const float pz = point.vertex[2] - sphere.centreZ;
                    point.angle = std::atan2(
                        px * bitangentX + py * bitangentY + pz * bitangentZ,
                        px * tangentX + py * tangentY + pz * tangentZ);
                }
                std::sort(rim.begin(), rim.end(),
                    [](const RimPoint& lhs, const RimPoint& rhs) {
                        return lhs.angle < rhs.angle;
                    });
                // Cap normals face back out of the bowl, toward the centre of
                // the sphere that carved it.
                for (RimPoint& point : rim) {
                    const float nx = sphere.centreX - point.vertex[0];
                    const float ny = sphere.centreY - point.vertex[1];
                    const float nz = sphere.centreZ - point.vertex[2];
                    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (length > 1e-6f) {
                        point.vertex[3] = nx / length;
                        point.vertex[4] = ny / length;
                        point.vertex[5] = nz / length;
                    }
                }
                for (size_t i = 0; i < rim.size(); ++i) {
                    const RimPoint& current = rim[i];
                    const RimPoint& next = rim[(i + 1) % rim.size()];
                    Append(keptVertices, keptIndices, hub, current.vertex,
                           next.vertex);
                }
            }
        }
    }

    outVertices = std::move(keptVertices);
    outIndices = std::move(keptIndices);
    return true;
}

}  // namespace SGE
