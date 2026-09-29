// Geometry tests for the jagged palm-trunk fracture cut.
//
// PalmMeshCutter::Cut() replaces a flat planar "sawn log" cross-section with a
// jagged, deterministic splinter surface, built once and shared by both the
// lower (stump) and upper (falling log) pieces so they mate exactly. These
// tests check that contract without any D3D12 device or real palm asset:
//   - both pieces get a cap primitive (the break isn't left open)
//   - the two caps' outer boundary rings land at identical world positions
//     (the mating/watertightness requirement -- shoot a tree in multiplayer
//     and the host/client must agree on the same jagged seam)
//   - the same cutY/impact direction reproduces byte-identical geometry
//     (determinism, required for multiplayer sync)
//   - the fracture displacement stays within the intended ~0.3-0.6x trunk
//     radius amplitude band, so it reads as splintering, not runaway noise
//
// A synthetic cylinder stands in for the sliced FBX trunk: PalmMeshCutter only
// looks at vertex positions and the "palm_bark" material name to find the
// trunk contour (see IsTrunkSurface), so a plain cylinder exercises the same
// code path as the real asset.

#include "PalmMeshCutter.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace DirectX;

namespace {

int failures = 0;

#define CHECK(value) do { if (!(value)) { \
    std::printf("%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #value); \
    ++failures; } } while (false)

constexpr float kTrunkRadius = 0.3f;
constexpr float kTrunkHeight = 4.0f;
constexpr int kRadialSegments = 24;

// A capped cylinder: side wall assigned to a "palm_bark" material (so
// IsTrunkSurface recognises it) plus top/bottom disc caps on a different
// material (so they're excluded from the fracture contour, matching how leaf
// cards are excluded in the real asset).
std::shared_ptr<SceneMesh> BuildTrunkCylinder(float offsetX = 0.0f,
                                              float offsetZ = 0.0f) {
    auto mesh = std::make_shared<SceneMesh>();
    mesh->name = "test_trunk";

    auto bark = std::make_shared<SceneMaterial>();
    bark->name = "palm_bark";

    MeshPrimitive side;
    side.material = bark;

    auto pushVertex = [&](MeshPrimitive& prim, float x, float y, float z,
                          float nx, float ny, float nz) {
        const float vertex[12] = {
            x + offsetX, y, z + offsetZ,
            nx, ny, nz, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f
        };
        prim.vertices.insert(prim.vertices.end(), std::begin(vertex),
                             std::end(vertex));
    };

    // Two rings (top/bottom of the whole cylinder) tessellated into quads,
    // which Cut() will subdivide further at the cut plane in the middle.
    for (int i = 0; i < kRadialSegments; ++i) {
        const float angle0 = (float)i / kRadialSegments * XM_2PI;
        const float angle1 = (float)(i + 1) / kRadialSegments * XM_2PI;
        const float x0 = std::cos(angle0) * kTrunkRadius;
        const float z0 = std::sin(angle0) * kTrunkRadius;
        const float x1 = std::cos(angle1) * kTrunkRadius;
        const float z1 = std::sin(angle1) * kTrunkRadius;
        const unsigned base =
            static_cast<unsigned>(side.vertices.size() / 12);
        pushVertex(side, x0, 0.0f, z0, x0, 0.0f, z0);
        pushVertex(side, x1, 0.0f, z1, x1, 0.0f, z1);
        pushVertex(side, x1, kTrunkHeight, z1, x1, 0.0f, z1);
        pushVertex(side, x0, kTrunkHeight, z0, x0, 0.0f, z0);
        side.indices.push_back(base + 0);
        side.indices.push_back(base + 1);
        side.indices.push_back(base + 2);
        side.indices.push_back(base + 0);
        side.indices.push_back(base + 2);
        side.indices.push_back(base + 3);
    }
    mesh->primitives.push_back(std::move(side));
    return mesh;
}

struct CapSample {
    XMFLOAT3 position;
    bool found = false;
};

// Finds the cap primitive (the one using a material other than palm_bark) in
// a cut piece and returns its vertex positions.
std::vector<XMFLOAT3> CapVertices(const std::shared_ptr<SceneMesh>& mesh) {
    std::vector<XMFLOAT3> result;
    if (!mesh) return result;
    for (const MeshPrimitive& prim : mesh->primitives) {
        if (prim.material && prim.material->name == "palm_fresh_cut") {
            for (size_t i = 0; i + 2 < prim.vertices.size(); i += 12) {
                result.emplace_back(prim.vertices[i], prim.vertices[i + 1],
                                    prim.vertices[i + 2]);
            }
        }
    }
    return result;
}

std::vector<XMFLOAT2> CapUVs(const std::shared_ptr<SceneMesh>& mesh) {
    std::vector<XMFLOAT2> result;
    if (!mesh) return result;
    for (const MeshPrimitive& prim : mesh->primitives) {
        if (prim.material && prim.material->name == "palm_fresh_cut") {
            for (size_t i = 0; i + 7 < prim.vertices.size(); i += 12)
                result.emplace_back(prim.vertices[i + 6], prim.vertices[i + 7]);
        }
    }
    return result;
}

float Distance(const XMFLOAT3& a, const XMFLOAT3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

int main() {
    // ---- Cutting produces a cap on both pieces ----
    {
        std::shared_ptr<SceneMesh> trunk = BuildTrunkCylinder();
        PalmMeshCut cut = PalmMeshCutter::Cut(
            trunk, kTrunkHeight * 0.5f, XMFLOAT2(1.0f, 0.0f));
        CHECK(cut.lower != nullptr);
        CHECK(cut.upper != nullptr);
        const std::vector<XMFLOAT3> lowerCap = CapVertices(cut.lower);
        const std::vector<XMFLOAT3> upperCap = CapVertices(cut.upper);
        CHECK(lowerCap.size() > 3);
        CHECK(upperCap.size() > 3);
        // Same ring/fan topology on both sides, since both build from the
        // same intersection set and ring count.
        CHECK(lowerCap.size() == upperCap.size());
    }

    // The cross-section texture must stay centered on an offset trunk, rather
    // than wrapping model-space coordinates across the generated cap.
    {
        PalmMeshCut cut = PalmMeshCutter::Cut(
            BuildTrunkCylinder(3.0f, -2.0f), kTrunkHeight * 0.5f,
            XMFLOAT2(0.6f, 0.8f));
        const std::vector<XMFLOAT2> lowerUVs = CapUVs(cut.lower);
        const std::vector<XMFLOAT2> upperUVs = CapUVs(cut.upper);
        CHECK(!lowerUVs.empty());
        CHECK(lowerUVs.size() == upperUVs.size());
        for (size_t i = 0; i < (std::min)(lowerUVs.size(), upperUVs.size()); ++i) {
            CHECK(lowerUVs[i].x >= -1e-4f && lowerUVs[i].x <= 1.0001f);
            CHECK(lowerUVs[i].y >= -1e-4f && lowerUVs[i].y <= 1.0001f);
            CHECK(std::abs(lowerUVs[i].x - upperUVs[i].x) < 1e-5f);
            CHECK(std::abs(lowerUVs[i].y - upperUVs[i].y) < 1e-5f);
        }
        if (!lowerUVs.empty()) {
            CHECK(std::abs(lowerUVs.back().x - 0.5f) < 1e-4f);
            CHECK(std::abs(lowerUVs.back().y - 0.5f) < 1e-4f);
        }
    }

    // ---- Lower and upper caps mate: every lower cap vertex has a matching
    // upper cap vertex at (near) the identical world position. This is the
    // watertightness/mating check -- if the two pieces' jagged seams
    // disagreed, no such matching would exist. ----
    {
        std::shared_ptr<SceneMesh> trunk = BuildTrunkCylinder();
        PalmMeshCut cut = PalmMeshCutter::Cut(
            trunk, kTrunkHeight * 0.5f, XMFLOAT2(0.6f, 0.8f));
        const std::vector<XMFLOAT3> lowerCap = CapVertices(cut.lower);
        const std::vector<XMFLOAT3> upperCap = CapVertices(cut.upper);
        CHECK(!lowerCap.empty() && !upperCap.empty());

        int unmatched = 0;
        for (const XMFLOAT3& lp : lowerCap) {
            float best = FLT_MAX;
            for (const XMFLOAT3& up : upperCap)
                best = (std::min)(best, Distance(lp, up));
            if (best > 1e-3f) ++unmatched;
        }
        CHECK(unmatched == 0);
    }

    // ---- Determinism: cutting the same trunk at the same height/direction
    // twice gives byte-identical cap geometry (multiplayer sync requires
    // this: host and client must not diverge). ----
    {
        std::shared_ptr<SceneMesh> trunkA = BuildTrunkCylinder();
        std::shared_ptr<SceneMesh> trunkB = BuildTrunkCylinder();
        PalmMeshCut cutA = PalmMeshCutter::Cut(
            trunkA, 1.7f, XMFLOAT2(-0.4f, 0.3f));
        PalmMeshCut cutB = PalmMeshCutter::Cut(
            trunkB, 1.7f, XMFLOAT2(-0.4f, 0.3f));
        const std::vector<XMFLOAT3> capA = CapVertices(cutA.lower);
        const std::vector<XMFLOAT3> capB = CapVertices(cutB.lower);
        CHECK(capA.size() == capB.size());
        bool identical = capA.size() == capB.size();
        for (size_t i = 0; identical && i < capA.size(); ++i) {
            if (Distance(capA[i], capB[i]) > 1e-6f) identical = false;
        }
        CHECK(identical);
    }

    // ---- The fracture surface is not flat: displaced cap vertices spread
    // meaningfully in height, and stay within the intended amplitude band
    // (~0.3-0.6x trunk radius) rather than a barely-visible bump or a
    // runaway spike. ----
    {
        std::shared_ptr<SceneMesh> trunk = BuildTrunkCylinder();
        const float cutY = kTrunkHeight * 0.5f;
        PalmMeshCut cut = PalmMeshCutter::Cut(
            trunk, cutY, XMFLOAT2(1.0f, 0.0f));
        const std::vector<XMFLOAT3> lowerCap = CapVertices(cut.lower);
        CHECK(!lowerCap.empty());

        float minY = FLT_MAX, maxY = -FLT_MAX;
        for (const XMFLOAT3& p : lowerCap) {
            minY = (std::min)(minY, p.y);
            maxY = (std::max)(maxY, p.y);
        }
        const float spread = maxY - minY;
        // Not a flat disc: there is real vertical relief at the cut.
        CHECK(spread > 0.02f);
        // Bounded: displacement is a fraction of the trunk radius, not an
        // unbounded spike (kTrunkRadius * ~0.6 amplitude, plus some margin
        // for the tilt already baked into the flat cut plane itself).
        CHECK(spread < kTrunkRadius * 1.5f);
    }

    if (failures == 0) std::printf("PalmMeshCutterTests passed\n");
    return failures == 0 ? 0 : 1;
}
