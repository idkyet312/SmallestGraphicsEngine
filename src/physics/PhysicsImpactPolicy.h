#pragma once

#include <cstdint>

namespace PhysicsImpactPolicy {
constexpr uint64_t World = 1ull << 0;
constexpr uint64_t Debris = 1ull << 1;
constexpr uint64_t LodDebris = 1ull << 2;
constexpr uint64_t Barrel = 1ull << 3;
constexpr uint64_t Ragdoll = 1ull << 4;
constexpr uint64_t Vehicle = 1ull << 5;

constexpr uint64_t FractureDealerMask =
    Debris | LodDebris | Barrel | Vehicle;

// Box3D negative collision groups suppress contacts only between shapes sharing
// that group. Each corpse gets its own group: no self-collision explosions, but
// separate corpses still collide with each other and the world.
constexpr int RagdollCollisionGroup(uint32_t ragdollId) {
    constexpr uint32_t MaxGroup = 0x7FFFFFFEu;
    return -static_cast<int>((ragdollId % MaxGroup) + 1u);
}

constexpr bool CanFracture(uint64_t categoryA, uint64_t categoryB) {
    const uint64_t categories = categoryA | categoryB;
    return (categories & Ragdoll) == 0 &&
           (categories & FractureDealerMask) != 0;
}
}
