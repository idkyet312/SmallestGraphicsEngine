#include "DXRProbeLayout.h"
#include <filesystem>
#include <iostream>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

int main() {
    LevelDXRDDGISettings settings;
    settings.enabled = true;
    settings.surfaceSpacing = 1.0f;
    settings.surfaceOffset = 0.25f;
    settings.maxProbes = 32;
    std::vector<DXRProbeTriangle> triangles = {
        {{0, 0, 0}, {4, 0, 0}, {0, 0, 4}, 10},
        {{4, 0, 0}, {4, 0, 4}, {0, 0, 4}, 11}
    };
    DXRProbeLayout first;
    CHECK(first.Build(triangles, settings, 1234));
    CHECK(!first.probes.empty());
    CHECK(first.probes.size() <= settings.maxProbes);
    for (const DXRProbeRecord& probe : first.probes) {
        CHECK(probe.position.y < 0.0f || probe.position.y > 0.0f);
        CHECK(probe.state == DXRProbeState::Pending);
    }
    DXRProbeLayout second;
    CHECK(second.Build(triangles, settings, 1234));
    CHECK(second.probes.size() == first.probes.size());
    for (size_t i = 0; i < first.probes.size(); ++i)
        CHECK(first.probes[i].stableId == second.probes[i].stableId);
    CHECK(!first.cells.empty());
    CHECK(first.cellProbeIndices.size() == first.probes.size());

    const auto cache = std::filesystem::temp_directory_path() /
        "smallest-graphics-engine-probe-layout.ddgi";
    const uint64_t settingsHash = DXRProbeLayout::SettingsHash(settings);
    CHECK(first.SaveCache(cache, settingsHash));
    DXRProbeLayout cached;
    CHECK(cached.LoadCache(cache, 1234, settingsHash, settings.maxProbes));
    CHECK(cached.probes.size() == first.probes.size());
    CHECK(!cached.LoadCache(cache, 9999, settingsHash, settings.maxProbes));
    std::error_code ignored;
    std::filesystem::remove(cache, ignored);
    return failures ? 1 : 0;
}
