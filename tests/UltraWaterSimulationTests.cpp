#include "UltraWaterSimulation.h"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << \
        " CHECK failed: " #condition << '\n'; ++failures; } } while (false)
}

int main() {
    WaterBathymetryDesc invalid;
    CHECK(!invalid.Valid());
    WaterBathymetryDesc valid;
    valid.minimumXZ = {-128.0f, -96.0f};
    valid.maximumXZ = {128.0f, 96.0f};
    valid.resolution = 1024;
    valid.terrainRevision = 7;
    valid.heightAt = [](float x, float z) { return x + z; };
    CHECK(valid.Valid());
    CHECK(SelectBathymetryResolution(256.0f, 192.0f) == 1024);
    CHECK(SelectBathymetryResolution(257.0f, 192.0f) == 2048);
    CHECK(SelectCoastalResolution(256.0f, 192.0f) == 512);
    CHECK(SelectCoastalResolution(512.0f, 192.0f) == 1024);

    const std::vector<float> straightShore = {
        1.0f, 1.0f, 1.0f, -1.0f, -1.0f};
    const std::vector<float> signedDistance =
        UltraWaterDetail::BuildSignedShoreDistance(
            straightShore, 5, 1, 1.0f, 1.0f);
    CHECK(signedDistance.size() == straightShore.size());
    CHECK(std::abs(signedDistance[0] - 2.5f) < 1e-5f);
    CHECK(std::abs(signedDistance[2] - 0.5f) < 1e-5f);
    CHECK(std::abs(signedDistance[3] + 0.5f) < 1e-5f);
    CHECK(std::abs(signedDistance[4] + 1.5f) < 1e-5f);

    const std::vector<float> cornerShore = {
        -1.0f, -1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f};
    const std::vector<float> cornerDistance =
        UltraWaterDetail::BuildSignedShoreDistance(
            cornerShore, 3, 3, 2.0f, 1.0f);
    CHECK(std::abs(cornerDistance[4] - 0.5f) < 1e-5f);
    CHECK(std::abs(cornerDistance[0] +
                   (std::sqrt(5.0f) - 0.5f)) < 1e-5f);
    // The solver bed stands 30 m off the real shoreline, so its own waterline
    // sits at -30 m of signed distance and every contour is that much further
    // out. The 0.35 offshore slope is unchanged, so -70 m still reaches the
    // -14 m deep hand-off depth that -40 m used to.
    CHECK(std::abs(UltraWaterDetail::ShoreDistanceToBed(
                       -UltraWaterDetail::kShoreOffsetMetres)) < 1e-5f);
    CHECK(std::abs(UltraWaterDetail::ShoreDistanceToBed(-70.0f) +
                   14.0f) < 1e-5f);
    // Well inland still clamps to the same 8 m land cap.
    CHECK(std::abs(UltraWaterDetail::ShoreDistanceToBed(200.0f) -
                   8.0f) < 1e-5f);

    WaterInteractionRing<3> ring;
    for (int i = 0; i < 4; ++i) {
        WaterInteraction event;
        event.worldXZ.x = static_cast<float>(i);
        ring.Push(event);
    }
    CHECK(ring.Size() == 3);
    CHECK(ring.OverflowCount() == 1);
    WaterInteraction drained[3] = {};
    CHECK(ring.Drain(drained, 3) == 3);
    CHECK(drained[0].worldXZ.x == 1.0f);
    CHECK(drained[2].worldXZ.x == 3.0f);
    CHECK(ring.Size() == 0);

    const auto a = BuildUltraSpectralWaves();
    const auto b = BuildUltraSpectralWaves();
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].direction.x == b[i].direction.x);
        CHECK(a[i].direction.y == b[i].direction.y);
        CHECK(a[i].amplitude == b[i].amplitude);
        CHECK(a[i].phase == b[i].phase);
        CHECK(a[i].wavelength >= 2.0f && a[i].wavelength <= 36.01f);
    }
    CHECK(std::isfinite(EvaluateUltraSpectralHeight(a, 12.0f, -4.0f, 3.0f)));

    WaterHeightQueryFrameSlots<4, 2> querySlots;
    WaterHeightQuery submitted[2] = {
        {41, {1.0f, 2.0f}}, {87, {3.0f, 4.0f}}};
    querySlots.Submit(0, submitted, 2);
    DirectX::XMFLOAT4 samples[2] = {
        {5.0f, 0.1f, 0.2f, 1.0f}, {6.0f, 0.3f, 0.4f, 1.0f}};
    WaterHeightResult resolved[2] = {};
    CHECK(querySlots.Resolve(1, samples, resolved, 2) == 0);
    CHECK(querySlots.Resolve(0, samples, resolved, 2) == 2);
    CHECK(resolved[0].objectId == 41 && resolved[0].height == 5.0f);
    CHECK(resolved[1].objectId == 87 && resolved[1].slope.y == 0.4f);
    CHECK(querySlots.Resolve(0, samples, resolved, 2) == 0);

    constexpr uint32_t gridCells = 64;
    for (uint32_t level = 1; level < 10; ++level) {
        const uint32_t inner =
            UltraClipmapInnerSegments(gridCells, level);
        const uint32_t outer =
            UltraClipmapOuterSegments(gridCells, level);
        CHECK(inner == outer || inner == outer * 2);
        for (uint32_t side = 0; side < 4; ++side) {
            for (uint32_t segment = 0; segment <= outer; ++segment) {
                const DirectX::XMFLOAT2 outerPoint = UltraClipmapEdgePoint(
                    16.0f, side, segment, outer);
                const DirectX::XMFLOAT2 nextInnerPoint =
                    UltraClipmapEdgePoint(
                        16.0f, side, segment,
                        UltraClipmapInnerSegments(gridCells, level + 1));
                CHECK(outerPoint.x == nextInnerPoint.x);
                CHECK(outerPoint.y == nextInnerPoint.y);
            }
        }
    }

    if (failures == 0) std::cout << "Ultra water simulation tests passed\n";
    return failures == 0 ? 0 : 1;
}
