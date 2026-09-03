#ifndef BULLET_PENETRATION_H
#define BULLET_PENETRATION_H

// Bullet penetration budget.
//
// A round carries a budget seeded from its weapon's penetrationPower. Each
// surface it punches through spends some of that budget and cuts the damage it
// carries onward; when the budget runs out the next impact stops it. Weapons
// left at 0 never enter this path, so the default behaviour is unchanged.
//
// Split out of main.cpp so the rule can be tested without a device or a scene:
// the arithmetic decides how far a round reaches and how hard it still hits,
// which is worth pinning down independently of the collision code that calls it.

#include <cstdint>

namespace SGE {

// Flesh is cheap to cross and costs little damage. Sheet metal costs more of
// both -- a round that has already gone through a wall should not still be
// killing at full strength on the far side.
inline constexpr float kPenetrationCostFlesh = 1.0f;
inline constexpr float kPenetrationFalloffFlesh = 0.65f;
inline constexpr float kPenetrationCostSheet = 1.6f;
inline constexpr float kPenetrationFalloffSheet = 0.50f;
// A round can cross at most this many surfaces. Without a ceiling a high-power
// weapon fired down a crowded line would keep re-entering the collision work
// for one bullet; the harpoon uses the same guard for the same reason.
inline constexpr uint8_t kPenetrationMaxSurfaces = 4;

// The mutable per-round state this rule touches. Projectile carries these
// fields directly; taking them as a small struct keeps the rule free of the
// scene's much larger projectile type.
struct PenetrationState {
    float power = 0.0f;      // budget remaining
    uint8_t crossed = 0;     // surfaces already punched through
    float damage = 1.0f;     // damage multiplier carried onward
};

// Spends `cost` from the round's budget and applies `falloff` to the damage it
// carries. Returns true when the round survives and should keep flying, false
// when it is spent and the caller should stop it as usual.
//
// A round with exactly enough budget for a surface does cross it, and lands on
// zero: the budget is what it can pay for, not what it must have left over.
inline bool TryPenetrateState(PenetrationState& state, float cost,
                              float falloff) {
    if (state.power < cost) return false;
    if (state.crossed >= kPenetrationMaxSurfaces) return false;
    state.power -= cost;
    ++state.crossed;
    state.damage *= falloff;
    return true;
}

}  // namespace SGE

#endif  // BULLET_PENETRATION_H
