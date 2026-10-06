#include "GrassDamageRanges.h"
#include <iostream>

namespace {
struct Root { float x, z; };
struct DrawRange { uint32_t firstInstance, instanceCount; };
int failures = 0;
void Check(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
std::vector<uint32_t> Visible(const GrassDamageRanges& damage,
                              uint32_t first, uint32_t count) {
    std::vector<DrawRange> ranges;
    damage.AppendVisible(first, count, ranges);
    std::vector<uint32_t> instances;
    for (const auto& range : ranges)
        for (uint32_t i = 0; i < range.instanceCount; ++i)
            instances.push_back(range.firstInstance + i);
    return instances;
}
}

int main() {
    // All these roots fit in one 8 m cell. A small impact must leave the
    // corners and unrelated blades in that same cell alive.
    const std::vector<Root> roots = {
        {99,99}, {99,99}, // unrelated instances before this cell
        {1.5f,1.5f}, {0,0}, {2,0}, {2.01f,0},
        {-1.5f,-1.5f}, {0,1}, {3,3}, {3,-3}
    };
    const auto rootAt = [&](uint32_t i) -> const Root& { return roots[i]; };
    GrassDamageRanges damage;
    damage.ExcludeCircle(2, 8, 0, 0, 2, rootAt);
    Check(Visible(damage, 2, 8) == std::vector<uint32_t>({2,5,6,8,9}),
          "Only roots inside the circular crater disappear, including its boundary");
    Check(Visible(damage, 2, 4) == std::vector<uint32_t>({2,5}),
          "Lower density preserves the original prefix without filling crater holes");
    damage.ExcludeCircle(2, 8, 0, 0, 2, rootAt);
    Check(Visible(damage, 2, 8) == std::vector<uint32_t>({2,5,6,8,9}),
          "Replaying an impact leaves its surviving blades unchanged");
    damage.ExcludeCircle(2, 8, 3, 3, 1, rootAt);
    Check(Visible(damage, 2, 8) == std::vector<uint32_t>({2,5,6,9}),
          "Later impacts accumulate without resurrecting previous damage");
    damage.ExcludeCircle(2, 8, 0, 0, 20, rootAt);
    damage.ExcludeCircle(2, 8, 3, 3, 1, rootAt);
    Check(Visible(damage, 2, 8).empty(), "A fully destroyed cell stays empty");
    damage.Clear();
    Check(Visible(damage, 2, 8) == std::vector<uint32_t>({2,3,4,5,6,7,8,9}),
          "Clearing runtime damage restores the original field");
    damage.ExcludeCircle(2, 8, 90, 90, 1, rootAt);
    damage.ExcludeCircle(2, 8, 0, 0, 0, rootAt);
    std::vector<DrawRange> intact;
    damage.AppendVisible(2, 8, intact);
    Check(intact.size() == 1 && intact[0].firstInstance == 2 &&
          intact[0].instanceCount == 8,
          "Misses and zero-radius impacts keep the original single draw");

    // Shoot just across the junction of four cells, then overlap with a second
    // crater. The grid broad phase must never become the clearing shape.
    std::vector<Root> grid;
    std::vector<GrassDamageRanges> cells(4);
    for (int cz = 0; cz < 2; ++cz)
        for (int cx = 0; cx < 2; ++cx)
            for (int z = 0; z < 8; ++z)
                for (int x = 0; x < 8; ++x)
                    grid.push_back({cx * 8 + x + 0.5f, cz * 8 + z + 0.5f});
    struct Impact { float x, z, radius; };
    const Impact impacts[] = {{7.7f,8.2f,2.36f}, {9.2f,7.8f,1.8f}};
    for (const auto& impact : impacts)
        for (uint32_t c = 0; c < 4; ++c)
            if (GrassDamageRanges::CircleIntersectsCell(
                    (c % 2) * 8 + 4.0f, (c / 2) * 8 + 4.0f, 4,
                    impact.x, impact.z, impact.radius))
                cells[c].ExcludeCircle(c * 64, 64,
                    impact.x, impact.z, impact.radius,
                    [&](uint32_t i) -> const Root& { return grid[i]; });
    std::vector<uint32_t> actual, expected;
    for (uint32_t c = 0; c < 4; ++c) {
        const auto visible = Visible(cells[c], c * 64, 64);
        actual.insert(actual.end(), visible.begin(), visible.end());
    }
    for (uint32_t i = 0; i < grid.size(); ++i) {
        bool hit = false;
        for (const auto& impact : impacts)
            hit |= std::hypot(grid[i].x - impact.x, grid[i].z - impact.z)
                <= impact.radius;
        if (!hit) expected.push_back(i);
    }
    Check(actual == expected,
          "Crater edges stay circular across cell boundaries and overlapping impacts");
    Check(!GrassDamageRanges::CircleIntersectsCell(4,4,4,10,10,2),
          "A circle outside a cell corner does not trigger its broad phase");
    if (!failures) std::cout << "GrassDamageTests passed\n";
    return failures ? 1 : 0;
}
