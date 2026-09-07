#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace SGE {

struct DestructionRenderPose {
    std::array<double, 3> position = {};
    std::array<float, 4> rotation = { 0, 0, 0, 1 };

    bool operator==(const DestructionRenderPose& other) const {
        return std::memcmp(position.data(), other.position.data(), sizeof(position)) == 0 &&
            std::memcmp(rotation.data(), other.rotation.data(), sizeof(rotation)) == 0;
    }
};

enum class DestructionRenderChange { Unchanged, Transform, Rebuild };

// Ranges refer to the published CPU render lists, never to GPU descriptors.
// A full rebuild gives them a new epoch; newly split actors start invalid.
struct DestructionRenderSpan {
    bool valid = false;
    bool spatial = false;
    uint64_t epoch = 0;
    std::size_t chunkCount = 0;
    std::size_t itemBegin = 0, itemCount = 0;
    std::size_t batchBegin = 0, batchCount = 0;
    DestructionRenderPose pose;

    DestructionRenderChange Change(uint64_t currentEpoch, std::size_t currentChunks,
                                   bool currentlySpatial,
                                   const DestructionRenderPose& currentPose) const {
        if (!valid || epoch != currentEpoch || chunkCount != currentChunks ||
            spatial != currentlySpatial) return DestructionRenderChange::Rebuild;
        if (pose == currentPose) return DestructionRenderChange::Unchanged;
        // Spatial meshes bake world transforms. Even a sleeping body's explicit
        // teleport must invalidate the cell instead of moving its neighbours.
        return spatial ? DestructionRenderChange::Rebuild : DestructionRenderChange::Transform;
    }
};

} // namespace SGE
