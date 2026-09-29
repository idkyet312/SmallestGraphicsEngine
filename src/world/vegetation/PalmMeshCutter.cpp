#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "PalmMeshCutter.h"

#include "DX12Core.h"
#include "GLBImporter.h"
#include "PalmModel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <vector>

using namespace DirectX;

namespace {

constexpr size_t kVertexStride = 12;
constexpr float kPlaneEpsilon = 1e-5f;
constexpr float kCutTilt = 0.04f;
// Concentric rings subdividing the jagged cap fan, plus the number of splinter
// lobes carved into the fracture height field. More rings give the spikes
// actual slope (a single hull fan can only tilt at its rim); the lobe count is
// low enough to read as a handful of distinct splinters, not sandpaper.
constexpr int kCapRings = 4;
constexpr int kSplinterLobes = 7;

using Vertex = std::array<float, kVertexStride>;

struct CapPoint {
    Vertex vertex{};
    float u = 0.0f;
    float v = 0.0f;
};

// Deterministic hash -> [0,1). Used instead of <random> so the same cutY,
// trunk radius and impact direction reproduce byte-identical geometry on
// host and client (multiplayer sync requires this, not just "looks similar").
float Hash01(float x) {
    float s = std::sin(x * 127.1f + 43.758f) * 43758.5453f;
    return s - std::floor(s);
}

// Fracture height field over the cut cross-section, expressed in trunk-radius
// units. `angle` is the position around the ring (radians), `radiusFrac` is
// distance from the trunk axis over trunk radius (0 at centre, ~1 at bark),
// `tearSide` is +1 on the side facing away from the impact (where real wood
// fibres tear longest) and -1 facing the impact (the side that snaps first,
// shorter splinters). `seed` comes from the cut so repeated cuts on the same
// tree at different heights do not all look identical.
float FractureHeight(float angle, float radiusFrac, float tearSide,
                     float seed) {
    float height = 0.0f;
    // A handful of tall, sharp splinter spikes at pseudo-random angles. Each
    // lobe is a raised-cosine bump so it has a real peak rather than a smooth
    // sinusoid, which reads as "splintered" instead of "wavy".
    for (int lobe = 0; lobe < kSplinterLobes; ++lobe) {
        const float lobeSeed = seed + static_cast<float>(lobe) * 7.31f;
        const float lobeAngle = Hash01(lobeSeed) * XM_2PI;
        const float lobeWidth = 0.35f + 0.5f * Hash01(lobeSeed + 1.7f);
        float delta = std::fmod(angle - lobeAngle, XM_2PI);
        if (delta > XM_PI) delta -= XM_2PI;
        if (delta < -XM_PI) delta += XM_2PI;
        const float falloff = std::max(0.0f, 1.0f - std::abs(delta) / lobeWidth);
        const float bump = falloff * falloff * (3.0f - 2.0f * falloff);
        const float lobeAmplitude = 0.25f + 0.35f * Hash01(lobeSeed + 3.1f);
        height += bump * lobeAmplitude;
    }
    // Low-frequency undulation so the surface between spikes is not perfectly
    // flat, plus a finer ripple for fibrous texture.
    height += 0.10f * std::sin(angle * 3.0f + seed * 2.0f);
    height += 0.04f * std::sin(angle * 11.0f - seed * 5.3f);
    // Splinters taper toward the pith and are tallest near the bark, matching
    // how a snapped trunk shatters at the outside first.
    height *= 0.25f + 0.75f * std::clamp(radiusFrac, 0.0f, 1.0f);
    // The far side (away from the impact) tears into long spikes; the impact
    // side shears cleaner and shorter.
    height *= 0.55f + 0.45f * std::clamp(tearSide, -1.0f, 1.0f);
    return height;
}

// Fresh-cut wood colour, procedural. Root cause of the old "flat tan disc"
// look, measured before changing anything:
//   - MeshPrimitive's vertex format is Pos(3)/Normal(3)/UV(2)/Tangent(4) --
//     see SceneGraph.h -- there is no per-vertex COLOR channel to carry ring/
//     streak variation.
//   - WoodCapMaterial() set baseColorFactor to one constant and left
//     baseColorTexture null, so every cap texel sampled the same RGB; the cap
//     fan had nothing but a flat factor to sample.
//   - the cap normal was the single flat cut-plane normal on every vertex, so
//     even directional light couldn't reveal shape.
// Fix: bake a small procedural RGBA8 texture (growth rings by radius, radial
// crack streaks, darker bark-side rim, lighter fresh core) once, upload it via
// the same runtime path other vegetation materials already use at gameplay
// time (GLBImporter::CreateTextureFromRGBA + g_dx12.commandList, as in
// Vegetation.h's LoadDandelionModel), and assign it as WoodCapMaterial's
// baseColorTexture. Each cap's UVs are centered on its own cut before being
// scaled into 0..1 (see appendCapVertex), so offset trunks sample the same
// cross-section rather than a shifted part of the texture.
constexpr int kWoodTextureSize = 128;

std::vector<unsigned char> BuildWoodCapPixels() {
    std::vector<unsigned char> pixels(
        static_cast<size_t>(kWoodTextureSize) * kWoodTextureSize * 4);
    const float center = (kWoodTextureSize - 1) * 0.5f;
    for (int y = 0; y < kWoodTextureSize; ++y) {
        for (int x = 0; x < kWoodTextureSize; ++x) {
            const float dx = (x - center) / center;
            const float dy = (y - center) / center;
            const float radiusFrac = std::clamp(
                std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            const float angle = std::atan2(dy, dx);

            // Concentric growth rings: a sawtooth-like ring pattern that gets
            // finer toward the bark, like real annual rings.
            const float ringPhase = radiusFrac * 26.0f + std::sin(angle * 3.0f) * 0.6f;
            const float ring = 0.5f + 0.5f * std::sin(ringPhase * XM_PI);
            const float ringSharp = std::pow(ring, 3.0f);

            // Radial crack streaks: thin dark lines running outward from the
            // pith at a handful of angles, plus fine fibrous streaking.
            float crack = 0.0f;
            for (int lobe = 0; lobe < kSplinterLobes; ++lobe) {
                const float lobeAngle = Hash01(static_cast<float>(lobe) * 7.31f) * XM_2PI;
                float delta = std::fmod(angle - lobeAngle, XM_2PI);
                if (delta > XM_PI) delta -= XM_2PI;
                if (delta < -XM_PI) delta += XM_2PI;
                const float width = 0.05f + 0.03f * Hash01(lobeAngle + 1.0f);
                crack = std::max(crack,
                    std::max(0.0f, 1.0f - std::abs(delta) / width) * radiusFrac);
            }
            const float fiber = 0.5f + 0.5f * std::sin(angle * 37.0f + radiusFrac * 9.0f);

            // Pale fresh core darkening toward a bark-adjacent rim.
            const float core = 1.0f - radiusFrac;
            XMFLOAT3 innerColor(0.80f, 0.64f, 0.40f);
            XMFLOAT3 rimColor(0.40f, 0.26f, 0.13f);
            XMFLOAT3 color(
                innerColor.x + (rimColor.x - innerColor.x) * (1.0f - core),
                innerColor.y + (rimColor.y - innerColor.y) * (1.0f - core),
                innerColor.z + (rimColor.z - innerColor.z) * (1.0f - core));

            const float ringShade = 0.82f + 0.18f * ringSharp;
            const float fiberShade = 0.92f + 0.08f * fiber;
            const float crackShade = 1.0f - crack * 0.55f;
            color.x *= ringShade * fiberShade * crackShade;
            color.y *= ringShade * fiberShade * crackShade;
            color.z *= ringShade * fiberShade * crackShade;

            const size_t i = (static_cast<size_t>(y) * kWoodTextureSize +
                              static_cast<size_t>(x)) * 4;
            pixels[i + 0] = static_cast<unsigned char>(
                std::clamp(color.x * 255.0f, 0.0f, 255.0f));
            pixels[i + 1] = static_cast<unsigned char>(
                std::clamp(color.y * 255.0f, 0.0f, 255.0f));
            pixels[i + 2] = static_cast<unsigned char>(
                std::clamp(color.z * 255.0f, 0.0f, 255.0f));
            pixels[i + 3] = 255;
        }
    }
    return pixels;
}

Vertex ReadVertex(const MeshPrimitive& primitive, unsigned index) {
    Vertex result{};
    const size_t offset = static_cast<size_t>(index) * kVertexStride;
    if (offset + kVertexStride > primitive.vertices.size()) return result;
    std::copy_n(primitive.vertices.begin() + offset, kVertexStride,
                result.begin());
    return result;
}

Vertex Interpolate(const Vertex& a, const Vertex& b, float t) {
    Vertex result{};
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = a[i] + (b[i] - a[i]) * t;

    XMVECTOR normal = XMVector3Normalize(
        XMVectorSet(result[3], result[4], result[5], 0.0f));
    XMVECTOR tangent = XMVector3Normalize(
        XMVectorSet(result[8], result[9], result[10], 0.0f));
    XMFLOAT3 n{}, tan{};
    XMStoreFloat3(&n, normal);
    XMStoreFloat3(&tan, tangent);
    result[3] = n.x; result[4] = n.y; result[5] = n.z;
    result[8] = tan.x; result[9] = tan.y; result[10] = tan.z;
    return result;
}

float PlaneDistance(const Vertex& vertex, const XMFLOAT3& normal,
                    float planeDistance) {
    return vertex[0] * normal.x + vertex[1] * normal.y +
           vertex[2] * normal.z - planeDistance;
}

void AppendPolygon(MeshPrimitive& output, const std::vector<Vertex>& polygon) {
    if (polygon.size() < 3) return;
    const unsigned base =
        static_cast<unsigned>(output.vertices.size() / kVertexStride);
    for (const Vertex& vertex : polygon)
        output.vertices.insert(output.vertices.end(),
                               vertex.begin(), vertex.end());
    for (unsigned i = 1; i + 1 < polygon.size(); ++i) {
        output.indices.push_back(base);
        output.indices.push_back(base + i);
        output.indices.push_back(base + i + 1);
    }
}

std::vector<Vertex> ClipTriangle(const std::array<Vertex, 3>& triangle,
                                 const XMFLOAT3& normal, float planeDistance,
                                 bool keepLower) {
    std::vector<Vertex> input(triangle.begin(), triangle.end());
    std::vector<Vertex> output;
    output.reserve(4);
    for (size_t i = 0; i < input.size(); ++i) {
        const Vertex& current = input[i];
        const Vertex& next = input[(i + 1) % input.size()];
        const float currentDistance =
            PlaneDistance(current, normal, planeDistance);
        const float nextDistance = PlaneDistance(next, normal, planeDistance);
        const bool currentInside = keepLower
            ? currentDistance <= kPlaneEpsilon
            : currentDistance >= -kPlaneEpsilon;
        const bool nextInside = keepLower
            ? nextDistance <= kPlaneEpsilon
            : nextDistance >= -kPlaneEpsilon;

        if (currentInside) output.push_back(current);
        if (currentInside != nextInside) {
            const float denominator = currentDistance - nextDistance;
            const float t = std::abs(denominator) > 1e-8f
                ? currentDistance / denominator : 0.5f;
            output.push_back(Interpolate(current, next,
                                         std::clamp(t, 0.0f, 1.0f)));
        }
    }
    return output;
}

void CollectTriangleIntersections(const std::array<Vertex, 3>& triangle,
                                  const XMFLOAT3& normal, float planeDistance,
                                  std::vector<Vertex>& intersections) {
    for (size_t edge = 0; edge < triangle.size(); ++edge) {
        const Vertex& a = triangle[edge];
        const Vertex& b = triangle[(edge + 1) % triangle.size()];
        const float da = PlaneDistance(a, normal, planeDistance);
        const float db = PlaneDistance(b, normal, planeDistance);
        if ((da < -kPlaneEpsilon && db > kPlaneEpsilon) ||
            (da > kPlaneEpsilon && db < -kPlaneEpsilon)) {
            const float t = da / (da - db);
            intersections.push_back(Interpolate(a, b, t));
        } else if (std::abs(da) <= kPlaneEpsilon) {
            intersections.push_back(a);
        }
    }
}

bool IsTrunkSurface(const MeshPrimitive& primitive) {
    // PalmModel assigns shared, named materials before slicing. Only bark forms
    // the closed trunk contour. Leaf cards can cross the cut plane far from the
    // trunk; including those points in one convex cap creates the long stretched
    // triangles seen after a split.
    return primitive.material && primitive.material->name == "palm_bark";
}

std::shared_ptr<SceneMaterial> WoodCapMaterial() {
    static std::shared_ptr<SceneMaterial> material = [] {
        auto result = std::make_shared<SceneMaterial>();
        result->name = "palm_fresh_cut";
        // A visible wood fallback if the texture could not be prepared.
        result->baseColorFactor = XMFLOAT4(0.72f, 0.52f, 0.31f, 1.0f);
        result->metallicFactor = 0.0f;
        result->roughnessFactor = 0.94f;
        result->doubleSided = true;
        result->disableOcclusionCulling = true;
        return result;
    }();
    return material;
}

float Cross2D(const CapPoint& origin, const CapPoint& a, const CapPoint& b) {
    return (a.u - origin.u) * (b.v - origin.v) -
           (a.v - origin.v) * (b.u - origin.u);
}

std::vector<CapPoint> BuildHull(const std::vector<Vertex>& intersections,
                                const XMFLOAT3& normal,
                                XMFLOAT3& tangent,
                                XMFLOAT3& bitangent) {
    const XMVECTOR n = XMLoadFloat3(&normal);
    const XMVECTOR reference = std::abs(normal.y) > 0.8f
        ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
        : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR t = XMVector3Normalize(XMVector3Cross(reference, n));
    const XMVECTOR b = XMVector3Normalize(XMVector3Cross(n, t));
    XMStoreFloat3(&tangent, t);
    XMStoreFloat3(&bitangent, b);

    std::vector<CapPoint> points;
    points.reserve(intersections.size());
    for (const Vertex& vertex : intersections) {
        const XMVECTOR p =
            XMVectorSet(vertex[0], vertex[1], vertex[2], 0.0f);
        CapPoint point;
        point.vertex = vertex;
        point.u = XMVectorGetX(XMVector3Dot(p, t));
        point.v = XMVectorGetX(XMVector3Dot(p, b));
        const bool duplicate = std::any_of(
            points.begin(), points.end(), [&](const CapPoint& existing) {
                const float du = existing.u - point.u;
                const float dv = existing.v - point.v;
                return du * du + dv * dv < 1e-8f;
            });
        if (!duplicate) points.push_back(point);
    }
    if (points.size() < 3) return {};

    std::sort(points.begin(), points.end(),
        [](const CapPoint& a, const CapPoint& b) {
            return a.u < b.u || (a.u == b.u && a.v < b.v);
        });
    std::vector<CapPoint> hull;
    hull.reserve(points.size() * 2);
    for (const CapPoint& point : points) {
        while (hull.size() >= 2 &&
               Cross2D(hull[hull.size() - 2], hull.back(), point) <= 0.0f)
            hull.pop_back();
        hull.push_back(point);
    }
    const size_t lowerSize = hull.size();
    for (auto it = points.rbegin() + 1; it != points.rend(); ++it) {
        while (hull.size() > lowerSize &&
               Cross2D(hull[hull.size() - 2], hull.back(), *it) <= 0.0f)
            hull.pop_back();
        hull.push_back(*it);
    }
    if (!hull.empty()) hull.pop_back();
    return hull;
}

// Shared parameters for the fracture height field, computed once per Cut()
// call from the (pre-clip) plane-intersection points. Both the trunk's
// cut-edge vertices and the cap's hull vertices displace through the same
// frame and the same FractureHeight(), so a point that starts on the cut
// plane in both the trunk polygon and the cap ends up at the identical
// displaced world position -- the jagged seam mates exactly by construction,
// not by tolerance.
struct CutFrame {
    XMFLOAT3 planeNormal{};
    XMFLOAT3 tangent{};
    XMFLOAT3 bitangent{};
    XMFLOAT3 center{};
    float centerU = 0.0f;
    float centerV = 0.0f;
    float trunkRadius = 0.0f;
    XMFLOAT2 impactUV{};
    float seed = 0.0f;
};

// Displaces a world-space point that lies on the cut plane by the fracture
// height field, along `planeNormal` (the same physical plane normal for both
// lower and upper pieces -- never the outward-facing normal, which flips
// sign between the two and would break mating).
XMFLOAT3 DisplaceOnPlane(const XMFLOAT3& position, const CutFrame& frame) {
    const XMVECTOR p = XMVectorSet(position.x, position.y, position.z, 0.0f);
    const float u = XMVectorGetX(
        XMVector3Dot(p, XMLoadFloat3(&frame.tangent)));
    const float v = XMVectorGetX(
        XMVector3Dot(p, XMLoadFloat3(&frame.bitangent)));
    const float du = u - frame.centerU;
    const float dv = v - frame.centerV;
    const float radius = std::sqrt(du * du + dv * dv);
    const float radiusFrac =
        frame.trunkRadius > 1e-5f ? radius / frame.trunkRadius : 0.0f;
    const float angle = std::atan2(dv, du);
    const float impactLen = std::sqrt(
        frame.impactUV.x * frame.impactUV.x + frame.impactUV.y * frame.impactUV.y);
    float tearSide = 0.0f;
    if (impactLen > 1e-5f && radius > 1e-5f) {
        // Dot of the point's outward direction with the impact direction:
        // positive on the impact side (shears clean/short), negative on the
        // far side (tears long). FractureHeight wants "away from impact" as
        // the tall side, so negate.
        tearSide = -(du * frame.impactUV.x + dv * frame.impactUV.y) /
                   (radius * impactLen);
    }
    const float amplitude = frame.trunkRadius * 0.45f; // ~0.3-0.6x trunk radius
    const float height =
        FractureHeight(angle, radiusFrac, tearSide, frame.seed) * amplitude;

    return XMFLOAT3(
        position.x + frame.planeNormal.x * height,
        position.y + frame.planeNormal.y * height,
        position.z + frame.planeNormal.z * height);
}

CapPoint DisplaceHullPoint(const CapPoint& point, const CutFrame& frame) {
    CapPoint result = point;
    const XMFLOAT3 displaced = DisplaceOnPlane(
        XMFLOAT3(point.vertex[0], point.vertex[1], point.vertex[2]), frame);
    result.vertex[0] = displaced.x;
    result.vertex[1] = displaced.y;
    result.vertex[2] = displaced.z;
    return result;
}

void AddCap(const std::shared_ptr<SceneMesh>& mesh,
            const std::vector<Vertex>& intersections,
            const CutFrame& frame, const XMFLOAT3& outwardNormal) {
    if (!mesh || intersections.size() < 3) return;
    XMFLOAT3 unusedTangent{}, unusedBitangent{};
    std::vector<CapPoint> hull =
        BuildHull(intersections, frame.planeNormal, unusedTangent, unusedBitangent);
    if (hull.size() < 3) return;
    // BuildHull derives its own tangent/bitangent from planeNormal with the
    // same fixed reference-axis rule CutFrame used, so they are identical to
    // frame.tangent/frame.bitangent; asserted implicitly by both callers
    // (trunk-skirt displacement and this cap) sharing one CutFrame instance.

    MeshPrimitive cap;
    cap.material = WoodCapMaterial();
    cap.materialIndex = 0;

    const float centerU = frame.centerU;
    const float centerV = frame.centerV;
    const XMFLOAT3& center = frame.center;
    const float trunkRadius = frame.trunkRadius;
    const float inverseCount = 1.0f / static_cast<float>(hull.size());

    // Displace the boundary ring, then build interior concentric rings by
    // lerping each boundary point toward the (undisplaced) centroid and
    // displacing those too -- the height field naturally tapers via
    // radiusFrac, so inner rings sit lower and the spikes get real slope
    // instead of a single flat-fan tilt.
    std::vector<std::vector<CapPoint>> rings(kCapRings + 1);
    for (int ring = 0; ring <= kCapRings; ++ring) {
        const float t = static_cast<float>(ring) / static_cast<float>(kCapRings);
        rings[ring].reserve(hull.size());
        for (const CapPoint& boundaryPoint : hull) {
            CapPoint lerped = boundaryPoint;
            lerped.u = centerU + (boundaryPoint.u - centerU) * t;
            lerped.v = centerV + (boundaryPoint.v - centerV) * t;
            lerped.vertex[0] = center.x + (boundaryPoint.vertex[0] - center.x) * t;
            lerped.vertex[1] = center.y + (boundaryPoint.vertex[1] - center.y) * t;
            lerped.vertex[2] = center.z + (boundaryPoint.vertex[2] - center.z) * t;
            rings[ring].push_back(DisplaceHullPoint(lerped, frame));
        }
    }

    auto appendCapVertex = [&](const CapPoint& point) {
        // Each cut gets its own wood cross-section, including trunks offset
        // from the model origin and pieces cut again after falling.
        const float u = (point.u - centerU) / trunkRadius;
        const float v = (point.v - centerV) / trunkRadius;
        const float vertex[kVertexStride] = {
            point.vertex[0], point.vertex[1], point.vertex[2],
            outwardNormal.x, outwardNormal.y, outwardNormal.z,
            u * 0.5f + 0.5f, v * 0.5f + 0.5f,
            frame.tangent.x, frame.tangent.y, frame.tangent.z, 1.0f
        };
        cap.vertices.insert(cap.vertices.end(), std::begin(vertex),
                            std::end(vertex));
    };

    const unsigned ringSize = static_cast<unsigned>(hull.size());
    std::vector<unsigned> ringBase(kCapRings + 1);
    for (int ring = 0; ring <= kCapRings; ++ring) {
        ringBase[ring] = static_cast<unsigned>(cap.vertices.size() / kVertexStride);
        for (const CapPoint& point : rings[ring]) appendCapVertex(point);
    }

    // Innermost ring collapses to a fan around its own centroid (the "hub"
    // below) rather than a single flat point, so the very centre keeps some
    // of the fracture noise instead of a perfectly flat tip.
    XMFLOAT3 hub{};
    float hubU = 0.0f, hubV = 0.0f;
    for (const CapPoint& point : rings[0]) {
        hub.x += point.vertex[0];
        hub.y += point.vertex[1];
        hub.z += point.vertex[2];
        hubU += point.u;
        hubV += point.v;
    }
    hub.x *= inverseCount; hub.y *= inverseCount; hub.z *= inverseCount;
    hubU *= inverseCount; hubV *= inverseCount;
    CapPoint hubPoint;
    hubPoint.vertex = {hub.x, hub.y, hub.z};
    hubPoint.u = hubU;
    hubPoint.v = hubV;
    const unsigned hubIndex = static_cast<unsigned>(cap.vertices.size() / kVertexStride);
    appendCapVertex(hubPoint);

    const bool flip = (outwardNormal.x * frame.planeNormal.x +
                       outwardNormal.y * frame.planeNormal.y +
                       outwardNormal.z * frame.planeNormal.z) < 0.0f;
    auto pushTriangle = [&](unsigned a, unsigned b, unsigned c) {
        if (flip) {
            cap.indices.push_back(a);
            cap.indices.push_back(c);
            cap.indices.push_back(b);
        } else {
            cap.indices.push_back(a);
            cap.indices.push_back(b);
            cap.indices.push_back(c);
        }
    };

    // Hub fan into the innermost ring.
    for (unsigned i = 0; i < ringSize; ++i) {
        const unsigned a = ringBase[0] + i;
        const unsigned b = ringBase[0] + (i + 1) % ringSize;
        pushTriangle(hubIndex, a, b);
    }
    // Quad strips between consecutive rings.
    for (int ring = 0; ring < kCapRings; ++ring) {
        for (unsigned i = 0; i < ringSize; ++i) {
            const unsigned a0 = ringBase[ring] + i;
            const unsigned a1 = ringBase[ring] + (i + 1) % ringSize;
            const unsigned b0 = ringBase[ring + 1] + i;
            const unsigned b1 = ringBase[ring + 1] + (i + 1) % ringSize;
            pushTriangle(a0, b0, b1);
            pushTriangle(a0, b1, a1);
        }
    }

    // The jagged surface has real slope now (unlike the old single flat
    // disc), so a constant per-vertex normal would leave spikes shaded like a
    // flat plane despite the geometry. Recompute smooth per-vertex normals
    // from the actual triangle faces so lighting reveals the ring/spike
    // shape, keeping the outward hemisphere so backface culling still agrees
    // with `outwardNormal`.
    std::vector<XMFLOAT3> accumulatedNormals(
        cap.vertices.size() / kVertexStride, XMFLOAT3(0.0f, 0.0f, 0.0f));
    for (size_t i = 0; i + 2 < cap.indices.size(); i += 3) {
        const unsigned ia = cap.indices[i];
        const unsigned ib = cap.indices[i + 1];
        const unsigned ic = cap.indices[i + 2];
        auto readPos = [&](unsigned index) {
            const size_t base = static_cast<size_t>(index) * kVertexStride;
            return XMVectorSet(cap.vertices[base], cap.vertices[base + 1],
                               cap.vertices[base + 2], 0.0f);
        };
        const XMVECTOR pa = readPos(ia), pb = readPos(ib), pc = readPos(ic);
        XMVECTOR faceNormal = XMVector3Cross(
            XMVectorSubtract(pb, pa), XMVectorSubtract(pc, pa));
        if (XMVectorGetX(XMVector3LengthSq(faceNormal)) > 1e-12f)
            faceNormal = XMVector3Normalize(faceNormal);
        XMFLOAT3 fn{};
        XMStoreFloat3(&fn, faceNormal);
        for (unsigned index : {ia, ib, ic}) {
            accumulatedNormals[index].x += fn.x;
            accumulatedNormals[index].y += fn.y;
            accumulatedNormals[index].z += fn.z;
        }
    }
    for (size_t v = 0; v < accumulatedNormals.size(); ++v) {
        XMVECTOR n = XMLoadFloat3(&accumulatedNormals[v]);
        if (XMVectorGetX(XMVector3LengthSq(n)) > 1e-12f) {
            n = XMVector3Normalize(n);
            // Keep the normal in the outward hemisphere: a spike's local face
            // can lean past 90 degrees from the flat cut normal, but it must
            // never flip to face into the wood.
            if (XMVectorGetX(XMVector3Dot(
                    n, XMLoadFloat3(&outwardNormal))) < 0.0f) {
                n = XMVectorNegate(n);
            }
        } else {
            n = XMLoadFloat3(&outwardNormal);
        }
        XMFLOAT3 result{};
        XMStoreFloat3(&result, n);
        const size_t base = v * kVertexStride;
        cap.vertices[base + 3] = result.x;
        cap.vertices[base + 4] = result.y;
        cap.vertices[base + 5] = result.z;
    }

    mesh->primitives.push_back(std::move(cap));
}

void AppendPrimitive(MeshPrimitive& destination,
                     const MeshPrimitive& source) {
    const unsigned base =
        static_cast<unsigned>(destination.vertices.size() / kVertexStride);
    destination.vertices.insert(destination.vertices.end(),
                                source.vertices.begin(), source.vertices.end());
    destination.indices.reserve(destination.indices.size() +
                                source.indices.size());
    for (unsigned index : source.indices)
        destination.indices.push_back(base + index);
}

} // namespace

bool PalmMeshCutter::PrepareCapMaterial() {
    const std::shared_ptr<SceneMaterial> material = WoodCapMaterial();
    if (material->baseColorTexture) return true;
    if (!g_dx12.device || !g_dx12.commandList) return false;
    const std::vector<unsigned char> pixels = BuildWoodCapPixels();
    material->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
        g_dx12.device.Get(), g_dx12.commandList.Get(), pixels,
        kWoodTextureSize, kWoodTextureSize, material->uploadHeaps);
    if (!material->baseColorTexture) return false;
    material->baseColorFactor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    return true;
}

std::shared_ptr<SceneMesh> PalmMeshCutter::BuildWholeTrunk() {
    auto result = std::make_shared<SceneMesh>();
    result->name = "palm_runtime_trunk";
    std::unordered_map<const SceneMaterial*, size_t> buckets;
    for (const PalmSlice& slice : PalmModel::TrunkSlices()) {
        if (!slice.mesh) continue;
        for (const MeshPrimitive& source : slice.mesh->primitives) {
            const SceneMaterial* key = source.material.get();
            auto found = buckets.find(key);
            if (found == buckets.end()) {
                MeshPrimitive primitive;
                primitive.material = source.material;
                primitive.materialIndex = source.materialIndex;
                result->primitives.push_back(std::move(primitive));
                found = buckets.emplace(key, result->primitives.size() - 1).first;
            }
            AppendPrimitive(result->primitives[found->second], source);
        }
    }
    return result;
}

PalmMeshCut PalmMeshCutter::Cut(
    const std::shared_ptr<SceneMesh>& source, float cutY,
    const XMFLOAT2& impactDirectionXZ) {
    PalmMeshCut result;
    if (!source) return result;
    result.lower = std::make_shared<SceneMesh>();
    result.upper = std::make_shared<SceneMesh>();
    result.lower->name = source->name + "_lower";
    result.upper->name = source->name + "_upper";

    XMVECTOR planeNormalVec = XMVector3Normalize(XMVectorSet(
        -impactDirectionXZ.x * kCutTilt, 1.0f,
        -impactDirectionXZ.y * kCutTilt, 0.0f));
    XMFLOAT3 normal{};
    XMStoreFloat3(&normal, planeNormalVec);
    const float planeDistance = cutY * normal.y;

    // Pass 1: collect the trunk's plane intersections only, so the fracture
    // frame (centre, trunk radius, tangent basis) is known before any output
    // vertex is displaced. Splitting this out is what lets the cut-edge
    // trunk vertices in pass 2 and the cap's hull vertices share one frame
    // and therefore land at the exact same displaced positions.
    std::vector<Vertex> intersections;
    for (const MeshPrimitive& sourcePrimitive : source->primitives) {
        if (!IsTrunkSurface(sourcePrimitive)) continue;
        const size_t triangleCount = sourcePrimitive.indices.empty()
            ? sourcePrimitive.vertices.size() / (kVertexStride * 3)
            : sourcePrimitive.indices.size() / 3;
        for (size_t triangleIndex = 0; triangleIndex < triangleCount;
             ++triangleIndex) {
            std::array<Vertex, 3> triangle{};
            for (size_t corner = 0; corner < 3; ++corner) {
                const unsigned index = sourcePrimitive.indices.empty()
                    ? static_cast<unsigned>(triangleIndex * 3 + corner)
                    : sourcePrimitive.indices[triangleIndex * 3 + corner];
                triangle[corner] = ReadVertex(sourcePrimitive, index);
            }
            CollectTriangleIntersections(
                triangle, normal, planeDistance, intersections);
        }
    }

    CutFrame frame;
    frame.planeNormal = normal;
    frame.seed = cutY * 17.0f + impactDirectionXZ.x * 3.1f +
                impactDirectionXZ.y * 5.7f;
    frame.impactUV = XMFLOAT2(0.0f, 0.0f);
    if (intersections.size() >= 3) {
        XMFLOAT3 tangent{}, bitangent{};
        std::vector<CapPoint> hull =
            BuildHull(intersections, normal, tangent, bitangent);
        frame.tangent = tangent;
        frame.bitangent = bitangent;
        if (hull.size() >= 3) {
            XMFLOAT3 center{};
            float centerU = 0.0f, centerV = 0.0f;
            for (const CapPoint& point : hull) {
                center.x += point.vertex[0];
                center.y += point.vertex[1];
                center.z += point.vertex[2];
                centerU += point.u;
                centerV += point.v;
            }
            const float inverseCount = 1.0f / static_cast<float>(hull.size());
            center.x *= inverseCount; center.y *= inverseCount;
            center.z *= inverseCount;
            centerU *= inverseCount; centerV *= inverseCount;
            frame.center = center;
            frame.centerU = centerU;
            frame.centerV = centerV;
            float trunkRadius = 0.0f;
            for (const CapPoint& point : hull) {
                const float du = point.u - centerU;
                const float dv = point.v - centerV;
                trunkRadius = std::max(trunkRadius, std::sqrt(du * du + dv * dv));
            }
            frame.trunkRadius =
                trunkRadius > 1e-5f ? trunkRadius : PalmModel::ModelRadius();
        } else {
            frame.trunkRadius = PalmModel::ModelRadius();
        }
        const XMVECTOR impact3 = XMVectorSet(
            impactDirectionXZ.x, 0.0f, impactDirectionXZ.y, 0.0f);
        frame.impactUV = XMFLOAT2(
            XMVectorGetX(XMVector3Dot(impact3, XMLoadFloat3(&tangent))),
            XMVectorGetX(XMVector3Dot(impact3, XMLoadFloat3(&bitangent))));
    } else {
        frame.trunkRadius = PalmModel::ModelRadius();
    }

    // Pass 2: clip for real, displacing every output vertex that lies on the
    // cut plane (the freshly created edge, whether from the trunk or any
    // other trunk-adjacent primitive) by the shared fracture frame. Leaf
    // cards etc. are not trunk surfaces and are excluded from `intersections`
    // above, but if a non-trunk primitive still happens to straddle the
    // plane its cut edge is left flat -- consistent with IsTrunkSurface's
    // existing rationale that only bark forms the closed contour worth
    // shaping.
    for (const MeshPrimitive& sourcePrimitive : source->primitives) {
        MeshPrimitive lower;
        MeshPrimitive upper;
        lower.material = upper.material = sourcePrimitive.material;
        lower.materialIndex = upper.materialIndex =
            sourcePrimitive.materialIndex;
        const bool trunkSurface = IsTrunkSurface(sourcePrimitive);
        const size_t triangleCount = sourcePrimitive.indices.empty()
            ? sourcePrimitive.vertices.size() / (kVertexStride * 3)
            : sourcePrimitive.indices.size() / 3;
        for (size_t triangleIndex = 0; triangleIndex < triangleCount;
             ++triangleIndex) {
            std::array<Vertex, 3> triangle{};
            for (size_t corner = 0; corner < 3; ++corner) {
                const unsigned index = sourcePrimitive.indices.empty()
                    ? static_cast<unsigned>(triangleIndex * 3 + corner)
                    : sourcePrimitive.indices[triangleIndex * 3 + corner];
                triangle[corner] = ReadVertex(sourcePrimitive, index);
            }
            std::vector<Vertex> lowerPoly = ClipTriangle(
                triangle, normal, planeDistance, true);
            std::vector<Vertex> upperPoly = ClipTriangle(
                triangle, normal, planeDistance, false);
            if (trunkSurface) {
                for (Vertex& vertex : lowerPoly) {
                    if (std::abs(PlaneDistance(vertex, normal, planeDistance)) >
                        kPlaneEpsilon) continue;
                    const XMFLOAT3 displaced = DisplaceOnPlane(
                        XMFLOAT3(vertex[0], vertex[1], vertex[2]), frame);
                    vertex[0] = displaced.x;
                    vertex[1] = displaced.y;
                    vertex[2] = displaced.z;
                }
                for (Vertex& vertex : upperPoly) {
                    if (std::abs(PlaneDistance(vertex, normal, planeDistance)) >
                        kPlaneEpsilon) continue;
                    const XMFLOAT3 displaced = DisplaceOnPlane(
                        XMFLOAT3(vertex[0], vertex[1], vertex[2]), frame);
                    vertex[0] = displaced.x;
                    vertex[1] = displaced.y;
                    vertex[2] = displaced.z;
                }
            }
            AppendPolygon(lower, lowerPoly);
            AppendPolygon(upper, upperPoly);
        }
        if (!lower.indices.empty())
            result.lower->primitives.push_back(std::move(lower));
        if (!upper.indices.empty())
            result.upper->primitives.push_back(std::move(upper));
    }

    AddCap(result.lower, intersections, frame, normal);
    AddCap(result.upper, intersections, frame,
           XMFLOAT3(-normal.x, -normal.y, -normal.z));
    if (result.lower->primitives.empty()) result.lower.reset();
    if (result.upper->primitives.empty()) result.upper.reset();
    return result;
}

bool PalmMeshCutter::Upload(const std::shared_ptr<SceneMesh>& mesh) {
    if (!mesh || !g_dx12.device) return false;
    bool uploaded = false;
    for (MeshPrimitive& primitive : mesh->primitives) {
        primitive.visibilityMeshID = UINT_MAX;
        uploaded |= GLBImporter::BuildMeshletData(
            primitive, g_dx12.device.Get());
    }
    return uploaded;
}
