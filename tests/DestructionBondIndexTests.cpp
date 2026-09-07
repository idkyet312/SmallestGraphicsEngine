#include "DestructionBondIndex.h"

#include <iostream>
#include <numeric>
#include <random>
#include <unordered_map>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

int main() {
    SGE::DestructionBondIndex index;
    SGE::DestructionBondCounts counts;
    std::mt19937 random(48129);
    for (uint32_t trial = 0; trial < 250; ++trial) {
        const uint32_t chunkCount = 1 + random() % 128;
        std::vector<SGE::DestructionBondPair> bonds;
        for (uint32_t i = 0; i < chunkCount * 4; ++i)
            bonds.push_back({ random() % chunkCount, random() % chunkCount });
        index.Build(chunkCount, bonds);
        std::vector<int> groups(chunkCount + 3);
        for (int& group : groups) group = int(random() % 9) - 2;
        // Include sparse authored IDs, ungrouped chunks, self/parallel bonds,
        // and chunks appended for an independent runtime fence asset.
        groups[0] = 1000001;
        for (uint32_t split = 0; split < 16; ++split) {
            std::vector<uint32_t> actor;
            for (uint32_t c = 0; c < groups.size(); ++c)
                if (random() % 3 == 0) actor.push_back(c);
            std::shuffle(actor.begin(), actor.end(), random);
            std::vector<uint32_t> selected;
            index.SelectActorBonds(actor, selected);
            std::vector<bool> owned(groups.size());
            for (uint32_t c : actor) owned[c] = true;
            std::vector<uint32_t> expected;
            for (uint32_t b = 0; b < bonds.size(); ++b)
                if (owned[bonds[b].a] && owned[bonds[b].b]) expected.push_back(b);
            CHECK(selected == expected);

            // Health changes without a topology change must immediately affect
            // counts. Compare the production counter to the original full scan.
            for (int damage = 0; damage < 3; ++damage) {
                std::vector<float> health(bonds.size());
                for (float& h : health) h = float(int(random() % 4) - 1);
                const uint32_t healthCount = random() % (health.size() + 1);
                counts.Count(groups.size(), actor, selected, bonds,
                    health.data(), healthCount, [&](uint32_t c) { return groups[c]; });
                std::vector<uint32_t> live(groups.size());
                std::unordered_map<int, uint32_t> external;
                for (uint32_t b = 0; b < healthCount; ++b) {
                    if (health[b] <= 0) continue;
                    const auto& pair = bonds[b];
                    if (!owned[pair.a] || !owned[pair.b]) continue;
                    ++live[pair.a]; ++live[pair.b];
                    const int a = groups[pair.a], other = groups[pair.b];
                    if (a >= 0 && a != other) ++external[a];
                    if (other >= 0 && a != other) ++external[other];
                }
                for (uint32_t c : actor) CHECK(counts.Live(c) == live[c]);
                for (int group : groups) CHECK(counts.External(group) == external[group]);
            }
        }
    }
    index.Build(0, {});
    std::vector<uint32_t> empty{ 123 };
    index.SelectActorBonds({}, empty);
    CHECK(empty.empty());
    if (failures) return 1;
    std::cout << "12,000 indexed/full-scan bond-count comparisons passed\n";
    return 0;
}
