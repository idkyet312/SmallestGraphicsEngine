#ifndef ANIMATION_CLIP_UTILS_H
#define ANIMATION_CLIP_UTILS_H

#include "SkinnedTypes.h"

#include <cstddef>

namespace AnimationClipUtils {

// Animation-only FBXs from different Mixamo exports can use different character
// origins. Shift each matching translation track so target frame zero begins at
// the reference clip's frame-zero position. Per-frame motion remains unchanged.
inline std::size_t RebaseTranslationOrigin(
        const AnimationClip& reference, AnimationClip& target) {
    std::size_t rebasedTracks = 0;
    for (BoneTrack& targetTrack : target.tracks) {
        if (targetTrack.positions.empty()) continue;

        const BoneTrack* referenceTrack = nullptr;
        for (const BoneTrack& candidate : reference.tracks) {
            if (candidate.bone == targetTrack.bone &&
                !candidate.positions.empty()) {
                referenceTrack = &candidate;
                break;
            }
        }
        if (!referenceTrack) continue;

        const DirectX::XMFLOAT3& referenceOrigin =
            referenceTrack->positions.front().value;
        const DirectX::XMFLOAT3& targetOrigin =
            targetTrack.positions.front().value;
        const DirectX::XMFLOAT3 delta{
            referenceOrigin.x - targetOrigin.x,
            referenceOrigin.y - targetOrigin.y,
            referenceOrigin.z - targetOrigin.z
        };
        for (BoneTrack::VecKey& key : targetTrack.positions) {
            key.value.x += delta.x;
            key.value.y += delta.y;
            key.value.z += delta.z;
        }
        ++rebasedTracks;
    }
    return rebasedTracks;
}

} // namespace AnimationClipUtils

#endif
