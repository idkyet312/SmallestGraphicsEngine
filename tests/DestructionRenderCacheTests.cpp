#include "DestructionRenderCache.h"

#include <iostream>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

int main() {
    using Change = SGE::DestructionRenderChange;
    SGE::DestructionRenderSpan span;
    SGE::DestructionRenderPose pose;
    CHECK(span.Change(1, 4, false, pose) == Change::Rebuild);
    span.valid = true;
    span.epoch = 1;
    span.chunkCount = 4;
    CHECK(span.Change(1, 4, false, pose) == Change::Unchanged);
    CHECK(span.Change(2, 4, false, pose) == Change::Rebuild);
    CHECK(span.Change(1, 3, false, pose) == Change::Rebuild);

    pose.position[0] = 12.0;
    CHECK(span.Change(1, 4, false, pose) == Change::Transform);
    span.pose = pose;
    pose.rotation = { 0, 0.70710678f, 0, 0.70710678f };
    CHECK(span.Change(1, 4, false, pose) == Change::Transform);

    // Settling requires a rebuild even if the final step had no displacement.
    // Waking at the same pose must likewise remove geometry from its cell.
    span.pose = pose;
    CHECK(span.Change(1, 4, true, pose) == Change::Rebuild);
    span.spatial = true;
    CHECK(span.Change(1, 4, true, pose) == Change::Unchanged);
    CHECK(span.Change(1, 4, false, pose) == Change::Rebuild);
    pose.position[1] = 0.25;
    CHECK(span.Change(1, 4, true, pose) == Change::Rebuild);

    // A retained individual/actor batch still captures the final sleep pose.
    span.spatial = false;
    CHECK(span.Change(1, 4, false, pose) == Change::Transform);
    span.pose = pose;
    CHECK(span.Change(1, 4, false, pose) == Change::Unchanged);
    pose.position[2] = -0.0;
    CHECK(span.Change(1, 4, false, pose) == Change::Transform);
    span.valid = false; // failed upload or in-place chunk retirement
    CHECK(span.Change(1, 4, false, span.pose) == Change::Rebuild);
    if (failures) return 1;
    std::cout << "Render cache invalidation and final-pose checks passed\n";
    return 0;
}
