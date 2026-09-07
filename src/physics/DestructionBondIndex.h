#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace SGE {

struct DestructionBondPair { uint32_t a = 0, b = 0; };

// The level's bonds never change endpoints. Splits only change which chunks an
// actor owns and which bonds are alive, so select its bond indices once and
// continue reading live health from Blast on every structural pass.
class DestructionBondIndex {
public:
    void Build(std::size_t chunkCount, const std::vector<DestructionBondPair>& bonds) {
        outgoing_.assign(chunkCount, {});
        owned_.assign(chunkCount, 0);
        epoch_ = 0;
        for (uint32_t i = 0; i < bonds.size(); ++i) {
            const auto& bond = bonds[i];
            if (bond.a < chunkCount && bond.b < chunkCount)
                outgoing_[bond.a].push_back({ bond.b, i });
        }
    }

    void SelectActorBonds(const std::vector<uint32_t>& chunks,
                          std::vector<uint32_t>& result) {
        if (++epoch_ == 0) {
            std::fill(owned_.begin(), owned_.end(), 0);
            ++epoch_;
        }
        for (uint32_t chunk : chunks)
            if (chunk < owned_.size()) owned_[chunk] = epoch_;
        result.clear();
        for (uint32_t chunk : chunks) {
            if (chunk >= outgoing_.size()) continue;
            for (const Edge& edge : outgoing_[chunk])
                if (owned_[edge.other] == epoch_) result.push_back(edge.bond);
        }
        // Preserve the full scan's order, including parallel bonds. Runtime
        // fence chunks lie outside this index and belong to separate assets.
        std::sort(result.begin(), result.end());
    }

private:
    struct Edge { uint32_t other, bond; };
    std::vector<std::vector<Edge>> outgoing_;
    std::vector<uint32_t> owned_;
    uint32_t epoch_ = 0;
};

// Scratch is shared by successive actors/passes. Only counters used by the
// previous actor are cleared; the level-sized vectors and group buckets stay.
class DestructionBondCounts {
public:
    template<class GroupForChunk>
    void Count(std::size_t chunkCount, const std::vector<uint32_t>& actorChunks,
               const std::vector<uint32_t>& actorBonds,
               const std::vector<DestructionBondPair>& bonds,
               const float* health, uint32_t healthCount, GroupForChunk groupFor) {
        live_.resize(chunkCount);
        for (uint32_t chunk : actorChunks) live_[chunk] = 0;
        for (int group : touchedGroups_) external_[group] = 0;
        touchedGroups_.clear();
        for (uint32_t index : actorBonds) {
            if (index >= healthCount || health[index] <= 0.0f) continue;
            const auto& bond = bonds[index];
            ++live_[bond.a];
            ++live_[bond.b];
            const int a = groupFor(bond.a), b = groupFor(bond.b);
            if (a >= 0 && a != b) IncrementGroup(a);
            if (b >= 0 && b != a) IncrementGroup(b);
        }
    }

    uint32_t Live(uint32_t chunk) const { return live_[chunk]; }
    uint32_t External(int group) const {
        const auto it = external_.find(group);
        return it == external_.end() ? 0 : it->second;
    }

private:
    void IncrementGroup(int group) {
        uint32_t& count = external_[group];
        if (count == 0) touchedGroups_.push_back(group);
        ++count;
    }
    std::vector<uint32_t> live_;
    std::unordered_map<int, uint32_t> external_;
    std::vector<int> touchedGroups_;
};

} // namespace SGE
