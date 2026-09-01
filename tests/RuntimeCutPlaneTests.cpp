// Geometry tests for the runtime fracture planes used to break intact authored
// chunks (fence panels, the watchtower) apart when their health is spent.
//
// The property under test is that the 2^N fragments partition the source
// volume: every point belongs to exactly one fragment, so the break neither
// loses geometry nor renders it twice. That is pure arithmetic, so no D3D
// device, Blast framework or game content is involved.

#include "RuntimeCutPlane.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

using SGE::RuntimeCutPlane;

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

namespace {

struct Bounds {
    float minimum[3];
    float maximum[3];
};

std::array<float, 12> Vertex(float x, float y, float z) {
    std::array<float, 12> vertex = {};
    vertex[0] = x;
    vertex[1] = y;
    vertex[2] = z;
    return vertex;
}

// Mirrors the plane setup in DestructionDX12's CreateFenceFragmentAsset: a
// horizontal cut, plus a crossing vertical cut along each horizontal axis when
// the chunk breaks into eight.
std::vector<RuntimeCutPlane> BuildPlanes(const Bounds& bounds, bool crossCut,
                                         unsigned seed) {
    std::srand(seed);
    const auto randomUnit = []() {
        return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
    };
    const float axisLength = bounds.maximum[1] - bounds.minimum[1];
    const bool longAxisX = bounds.maximum[0] - bounds.minimum[0] >=
                           bounds.maximum[2] - bounds.minimum[2];
    const float pi = 3.14159265358979f;
    const float maximumTiltRadians = 30.0f * pi / 180.0f;

    std::vector<RuntimeCutPlane> planes;
    {
        RuntimeCutPlane plane;
        plane.horizontal = true;
        plane.alongAxisX = longAxisX;
        plane.offset = bounds.minimum[1] + axisLength *
            (0.35f + randomUnit() * 0.30f);
        const float longMinimum = longAxisX ? bounds.minimum[0] : bounds.minimum[2];
        const float longMaximum = longAxisX ? bounds.maximum[0] : bounds.maximum[2];
        plane.centre = (longMinimum + longMaximum) * 0.5f;
        const float longHalfExtent = (longMaximum - longMinimum) * 0.5f;
        const float clearance = (std::min)(plane.offset - bounds.minimum[1],
                                           bounds.maximum[1] - plane.offset);
        const float geometryLimit = longHalfExtent > 0.001f
            ? std::atan(clearance / longHalfExtent) : pi * 0.5f;
        const float tilt = (randomUnit() * 2.0f - 1.0f) *
            (std::min)(maximumTiltRadians, geometryLimit);
        plane.tiltCosine = std::cos(tilt);
        plane.tiltSine = std::sin(tilt);
        planes.push_back(plane);
    }
    if (crossCut) {
        for (const bool alongAxisX : { true, false }) {
            RuntimeCutPlane plane;
            plane.horizontal = false;
            plane.alongAxisX = alongAxisX;
            const float cutMinimum = alongAxisX ? bounds.minimum[0] : bounds.minimum[2];
            const float cutMaximum = alongAxisX ? bounds.maximum[0] : bounds.maximum[2];
            const float cutLength = cutMaximum - cutMinimum;
            // A panel with no depth gets only the cut it can support.
            if (cutLength <= 0.001f) continue;
            plane.offset = cutMinimum + cutLength *
                (0.35f + randomUnit() * 0.30f);
            plane.centre = (bounds.minimum[1] + bounds.maximum[1]) * 0.5f;
            const float halfHeight = axisLength * 0.5f;
            const float clearance = (std::min)(plane.offset - cutMinimum,
                                               cutMaximum - plane.offset);
            const float geometryLimit = halfHeight > 0.001f
                ? std::atan(clearance / halfHeight) : pi * 0.5f;
            const float tilt = (randomUnit() * 2.0f - 1.0f) *
                (std::min)(maximumTiltRadians, geometryLimit);
            plane.tiltCosine = std::cos(tilt);
            plane.tiltSine = std::sin(tilt);
            planes.push_back(plane);
        }
    }
    return planes;
}

// How many cut planes a chunk of these bounds supports: the horizontal one,
// plus a vertical cut for each horizontal axis that has real extent.
size_t ExpectedPlaneCount(const Bounds& bounds, bool crossCut) {
    size_t count = 1;
    if (crossCut) {
        if (bounds.maximum[0] - bounds.minimum[0] > 0.001f) ++count;
        if (bounds.maximum[2] - bounds.minimum[2] > 0.001f) ++count;
    }
    return count;
}

// Samples the source volume and counts, for every point, how many fragments
// claim it. Exactly one is correct.
void CheckFragmentsTile(const Bounds& bounds, bool crossCut, unsigned seed) {
    const std::vector<RuntimeCutPlane> planes =
        BuildPlanes(bounds, crossCut, seed);
    CHECK(planes.size() == ExpectedPlaneCount(bounds, crossCut));

    const unsigned pieceCount = 1u << planes.size();
    std::vector<int> hits(pieceCount, 0);
    int unassigned = 0;
    int doubleAssigned = 0;
    constexpr int kSteps = 24;
    for (int ix = 0; ix < kSteps; ++ix)
    for (int iy = 0; iy < kSteps; ++iy)
    for (int iz = 0; iz < kSteps; ++iz) {
        const auto axis = [&](int index, int step) {
            return bounds.minimum[index] +
                (bounds.maximum[index] - bounds.minimum[index]) *
                (step + 0.5f) / kSteps;
        };
        const std::array<float, 12> point =
            Vertex(axis(0, ix), axis(1, iy), axis(2, iz));

        int owners = 0;
        for (unsigned piece = 0; piece < pieceCount; ++piece) {
            bool inside = true;
            for (size_t plane = 0; plane < planes.size(); ++plane) {
                const bool keepBelow = ((piece >> plane) & 1u) == 0u;
                if (!planes[plane].Keeps(point, keepBelow)) {
                    inside = false;
                    break;
                }
            }
            if (inside) {
                ++hits[piece];
                ++owners;
            }
        }
        if (owners == 0) ++unassigned;
        if (owners > 1) ++doubleAssigned;
    }

    // No geometry may vanish, and none may be claimed by two fragments -- that
    // would render and collide twice.
    CHECK(unassigned == 0);
    CHECK(doubleAssigned == 0);
    // Every fragment must carry geometry, or the structure would break into
    // fewer pieces than intended.
    for (unsigned piece = 0; piece < pieceCount; ++piece)
        CHECK(hits[piece] > 0);
}

}  // namespace

int main() {
    // A deep, tall box: the shape the crossing vertical cuts exist for, so it
    // must come apart in eight. Bounds taken from a real structural asset.
    const Bounds deepBox{ { -0.006f, 0.0f, -12.722f },
                          { 7.673f, 15.731f, 0.006f } };
    // Measured bounds of the shipped fence panel.
    const Bounds fencePanel{ { -0.081f, 0.0f, -0.086f },
                             { 3.419f, 2.857f, 0.086f } };
    // A panel whose long axis runs along Z rather than X, so the alongAxisX
    // branch is covered in both directions.
    const Bounds rotatedPanel{ { -0.086f, 0.0f, -0.081f },
                               { 0.086f, 2.857f, 3.419f } };

    for (unsigned seed = 1; seed <= 25; ++seed) {
        CheckFragmentsTile(deepBox, /*crossCut=*/true, seed);
        CheckFragmentsTile(fencePanel, /*crossCut=*/false, seed);
        CheckFragmentsTile(rotatedPanel, /*crossCut=*/false, seed);
        CheckFragmentsTile(rotatedPanel, /*crossCut=*/true, seed);

        // A deep box supports all three planes and so comes apart in eight.
        CHECK(BuildPlanes(deepBox, /*crossCut=*/true, seed).size() == 3);
        // A fence panel is nearly flat, so its single horizontal cut yields two.
        CHECK(BuildPlanes(fencePanel, /*crossCut=*/false, seed).size() == 1);
    }

    // A degenerate, perfectly flat panel must still partition cleanly rather
    // than dividing by a zero extent.
    const Bounds flat{ { 0.0f, 0.0f, 0.0f }, { 4.0f, 3.0f, 0.0f } };
    CheckFragmentsTile(flat, /*crossCut=*/true, 1);

    if (failures == 0) std::cout << "RuntimeCutPlaneTests passed\n";
    return failures == 0 ? 0 : 1;
}
