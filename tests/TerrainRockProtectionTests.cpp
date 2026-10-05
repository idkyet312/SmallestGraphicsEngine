#include "TerrainRockProtection.h"

#include <iostream>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __LINE__ << " CHECK failed: " #value "\n"; ++failures; \
} } while (false)

int main() {
    const uint8_t rock[] = {0, 0, 0, 255};
    auto mask = BuildTerrainRockMask(rock, 1);
    const float fullRock = SampleTerrainRockMask(mask.data(),
        kTerrainRockMaskResolution, 0.5f, 0.5f);
    CHECK(fullRock == 1.0f);
    CHECK(BlendTerrainDestruction(2.5f, -3.0f,
        TerrainDestructionWeight(fullRock)) == 2.5f);
    // Neither successive cuts nor a raised crater lip can move solid bedrock.
    CHECK(BlendTerrainDestruction(2.5f, 2.9f,
        TerrainDestructionWeight(fullRock)) == 2.5f);
    CHECK(BlendTerrainDestruction(2.5f, -9.0f,
        TerrainDestructionWeight(fullRock)) == 2.5f);

    const uint8_t dirt[] = {0, 255, 0, 0};
    mask = BuildTerrainRockMask(dirt, 1);
    const float fullDirt = SampleTerrainRockMask(mask.data(),
        kTerrainRockMaskResolution, 0.5f, 0.5f);
    CHECK(fullDirt == 1.0f);
    CHECK(BlendTerrainDestruction(2.5f, -3.0f,
        TerrainDestructionWeight(fullDirt)) == 2.5f);
    CHECK(BlendTerrainDestruction(2.5f, 2.9f,
        TerrainDestructionWeight(fullDirt)) == 2.5f);
    CHECK(BlendTerrainDestruction(2.5f, -9.0f,
        TerrainDestructionWeight(fullDirt)) == 2.5f);

    // Mixing the two protected layers must not open a destructible seam.
    const uint8_t dirtRock[] = {0, 255, 0, 255};
    mask = BuildTerrainRockMask(dirtRock, 1);
    CHECK(std::all_of(mask.begin(), mask.end(), [](uint16_t v) { return v == 65535; }));

    const uint8_t unprotected[] = {255, 0, 0, 0, 0, 0, 255, 0,
                                   0, 0, 0, 0, 255, 0, 255, 0};
    mask = BuildTerrainRockMask(unprotected, 2);
    CHECK(std::all_of(mask.begin(), mask.end(), [](uint16_t v) { return v == 0; }));
    CHECK(BlendTerrainDestruction(2.5f, -3.0f,
        TerrainDestructionWeight(0)) == -3.0f);

    const uint8_t faint[] = {0, 0, 0, 128};
    mask = BuildTerrainRockMask(faint, 1);
    const float faintWeight = TerrainDestructionWeight(
        SampleTerrainRockMask(mask.data(), kTerrainRockMaskResolution, 0.5f, 0.5f));
    CHECK(faintWeight > 0.49f && faintWeight < 0.51f);
    const float partialCut = BlendTerrainDestruction(2.5f, -3.0f, faintWeight);
    CHECK(partialCut > -3.0f && partialCut < 2.5f);

    const uint8_t faintDirt[] = {0, 128, 0, 0};
    mask = BuildTerrainRockMask(faintDirt, 1);
    CHECK(std::abs(TerrainDestructionWeight(mask[0] / 65535.0f) - faintWeight) < 1e-6f);

    const uint8_t mixed[] = {255, 0, 0, 255};
    mask = BuildTerrainRockMask(mixed, 1);
    CHECK(std::abs(TerrainDestructionWeight(mask[0] / 65535.0f) - 0.5f) < 0.001f);

    const uint8_t mixedDirt[] = {255, 255, 0, 0};
    mask = BuildTerrainRockMask(mixedDirt, 1);
    CHECK(std::abs(TerrainDestructionWeight(mask[0] / 65535.0f) - 0.5f) < 0.001f);

    // A hard paint boundary still interpolates continuously, and sampling
    // respects texel centres rather than shifting the protection half a cell.
    const uint16_t boundary[] = {0, 65535, 0, 65535};
    CHECK(SampleTerrainRockMask(boundary, 2, 0.25f, 0.5f) == 0);
    CHECK(SampleTerrainRockMask(boundary, 2, 0.75f, 0.5f) == 1);
    CHECK(std::abs(SampleTerrainRockMask(boundary, 2, 0.5f, 0.5f) - 0.5f) < 1e-6f);
    CHECK(SampleTerrainRockMask(boundary, 2, -0.1f, 0.5f) == 0);
    CHECK(SampleTerrainRockMask(boundary, 2, 1.1f, 0.5f) == 0);
    float previous = 1;
    for (int i = 0; i <= 100; ++i) {
        const float weight = TerrainDestructionWeight(i / 100.0f);
        CHECK(weight <= previous && weight >= 0 && weight <= 1);
        previous = weight;
    }
    mask = BuildTerrainRockMask(nullptr, 0);
    CHECK(std::all_of(mask.begin(), mask.end(), [](uint16_t v) { return v == 0; }));
    return failures ? 1 : 0;
}
