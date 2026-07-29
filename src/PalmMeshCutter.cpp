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

using Vertex = std::array<float, kVertexStride>;

struct CapPoint {
    Vertex vertex{};
    float u = 0.0f;
    float v = 0.0f;
};

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

std::shared_ptr<SceneMaterial> WoodCapMaterial() {
    static std::shared_ptr<SceneMaterial> material = [] {
        auto result = std::make_shared<SceneMaterial>();
        result->name = "palm_fresh_cut";
        result->baseColorFactor = XMFLOAT4(0.56f, 0.33f, 0.14f, 1.0f);
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

void AddCap(const std::shared_ptr<SceneMesh>& mesh,
            const std::vector<Vertex>& intersections,
            const XMFLOAT3& outwardNormal) {
    if (!mesh || intersections.size() < 3) return;
    XMFLOAT3 tangent{}, bitangent{};
    std::vector<CapPoint> hull =
        BuildHull(intersections, outwardNormal, tangent, bitangent);
    if (hull.size() < 3) return;

    MeshPrimitive cap;
    cap.material = WoodCapMaterial();
    cap.materialIndex = 0;

    XMFLOAT3 center{};
    for (const CapPoint& point : hull) {
        center.x += point.vertex[0];
        center.y += point.vertex[1];
        center.z += point.vertex[2];
    }
    const float inverseCount = 1.0f / static_cast<float>(hull.size());
    center.x *= inverseCount;
    center.y *= inverseCount;
    center.z *= inverseCount;

    auto appendCapVertex = [&](const XMFLOAT3& position, float u, float v) {
        const float vertex[kVertexStride] = {
            position.x, position.y, position.z,
            outwardNormal.x, outwardNormal.y, outwardNormal.z,
            u * 1.8f + 0.5f, v * 1.8f + 0.5f,
            tangent.x, tangent.y, tangent.z, 1.0f
        };
        cap.vertices.insert(cap.vertices.end(), std::begin(vertex),
                            std::end(vertex));
    };
    appendCapVertex(center, 0.0f, 0.0f);
    for (const CapPoint& point : hull) {
        appendCapVertex(
            XMFLOAT3(point.vertex[0], point.vertex[1], point.vertex[2]),
            point.u, point.v);
    }
    for (unsigned i = 0; i < hull.size(); ++i) {
        cap.indices.push_back(0);
        cap.indices.push_back(i + 1);
        cap.indices.push_back((i + 1) % static_cast<unsigned>(hull.size()) + 1);
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

    XMVECTOR planeNormal = XMVector3Normalize(XMVectorSet(
        -impactDirectionXZ.x * 0.13f, 1.0f,
        -impactDirectionXZ.y * 0.13f, 0.0f));
    XMFLOAT3 normal{};
    XMStoreFloat3(&normal, planeNormal);
    const float planeDistance = cutY * normal.y;
    std::vector<Vertex> intersections;

    for (const MeshPrimitive& sourcePrimitive : source->primitives) {
        MeshPrimitive lower;
        MeshPrimitive upper;
        lower.material = upper.material = sourcePrimitive.material;
        lower.materialIndex = upper.materialIndex =
            sourcePrimitive.materialIndex;
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
            AppendPolygon(lower, ClipTriangle(
                triangle, normal, planeDistance, true));
            AppendPolygon(upper, ClipTriangle(
                triangle, normal, planeDistance, false));
        }
        if (!lower.indices.empty())
            result.lower->primitives.push_back(std::move(lower));
        if (!upper.indices.empty())
            result.upper->primitives.push_back(std::move(upper));
    }

    AddCap(result.lower, intersections, normal);
    AddCap(result.upper, intersections,
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
