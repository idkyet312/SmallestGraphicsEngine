#pragma once

#include <cstdint>

namespace PhysicsImpactPolicy {
constexpr uint64_t World = 1ull << 0;
constexpr uint64_t Debris = 1ull << 1;
constexpr uint64_t LodDebris = 1ull << 2;
constexpr uint64_t Barrel = 1ull << 3;
constexpr uint64_t Ragdoll = 1ull << 4;
constexpr uint64_t Vehicle = 1ull << 5;
constexpr uint64_t Grenade = 1ull << 6;
// A prefab prop that was authored static but is simulated as a rigid body -- a
// shipping container the player shoves or an explosion throws. It fractures
// what it lands on like any other heavy mover, so it joins the dealer mask.
constexpr uint64_t Prop = 1ull << 7;

constexpr uint64_t FractureDealerMask =
    Debris | LodDebris | Barrel | Vehicle | Prop;

constexpr bool CanFracture(uint64_t categoryA, uint64_t categoryB) {
    const uint64_t categories = categoryA | categoryB;
    return (categories & Ragdoll) == 0 &&
           (categories & Grenade) == 0 &&
           (categories & FractureDealerMask) != 0;
}
}
