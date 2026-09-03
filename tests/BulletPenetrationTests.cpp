#include "BulletPenetration.h"

#include <cmath>
#include <iostream>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

using SGE::PenetrationState;
using SGE::TryPenetrateState;
using SGE::kPenetrationCostFlesh;
using SGE::kPenetrationFalloffFlesh;
using SGE::kPenetrationCostSheet;
using SGE::kPenetrationFalloffSheet;
using SGE::kPenetrationMaxSurfaces;

// The authored values from WeaponCustomization.h, so these cases assert against
// what the weapons actually carry rather than a convenient stand-in.
static constexpr float kAkPower = 2.0f;
static constexpr float kSvdPower = 4.0f;
static constexpr float kM4Power = 1.8f;
static constexpr float kShotgunPower = 0.0f;

int main() {
    // A weapon with no penetration stops at the first thing it touches. This is
    // every weapon's default, so it is the case that must never regress.
    {
        PenetrationState state{ kShotgunPower, 0, 1.0f };
        CHECK(!TryPenetrateState(state, kPenetrationCostFlesh,
                                 kPenetrationFalloffFlesh));
        CHECK(state.crossed == 0);
        CHECK(state.damage == 1.0f);   // damage untouched on a refusal
    }

    // The AK carries enough for two bodies and no more.
    {
        PenetrationState state{ kAkPower, 0, 1.0f };
        CHECK(TryPenetrateState(state, kPenetrationCostFlesh,
                                kPenetrationFalloffFlesh));
        CHECK(TryPenetrateState(state, kPenetrationCostFlesh,
                                kPenetrationFalloffFlesh));
        CHECK(!TryPenetrateState(state, kPenetrationCostFlesh,
                                 kPenetrationFalloffFlesh));
        CHECK(state.crossed == 2);
        // Damage compounds per surface rather than being subtracted once.
        CHECK(std::abs(state.damage - 0.65f * 0.65f) < 1e-5f);
    }

    // The M4A1 is the weaker carbine: one body, not two.
    {
        PenetrationState state{ kM4Power, 0, 1.0f };
        CHECK(TryPenetrateState(state, kPenetrationCostFlesh,
                                kPenetrationFalloffFlesh));
        CHECK(!TryPenetrateState(state, kPenetrationCostFlesh,
                                 kPenetrationFalloffFlesh));
        CHECK(state.crossed == 1);
    }

    // Sheet metal costs more than flesh, so the same budget reaches less far.
    {
        PenetrationState sheet{ kSvdPower, 0, 1.0f };
        int sheetCrossings = 0;
        while (TryPenetrateState(sheet, kPenetrationCostSheet,
                                 kPenetrationFalloffSheet))
            ++sheetCrossings;

        PenetrationState flesh{ kSvdPower, 0, 1.0f };
        int fleshCrossings = 0;
        while (TryPenetrateState(flesh, kPenetrationCostFlesh,
                                 kPenetrationFalloffFlesh))
            ++fleshCrossings;

        CHECK(sheetCrossings == 2);
        CHECK(sheetCrossings < fleshCrossings);
    }

    // Exactly enough budget crosses the surface and lands on zero: the budget
    // is what a round can pay for, not what it must have left over.
    {
        PenetrationState state{ kPenetrationCostFlesh, 0, 1.0f };
        CHECK(TryPenetrateState(state, kPenetrationCostFlesh,
                                kPenetrationFalloffFlesh));
        CHECK(state.power == 0.0f);
        CHECK(!TryPenetrateState(state, kPenetrationCostFlesh,
                                 kPenetrationFalloffFlesh));
    }

    // The surface cap holds even when the budget would allow more. Without it
    // one round could keep re-entering the collision work indefinitely.
    {
        PenetrationState state{ 1000.0f, 0, 1.0f };
        int crossings = 0;
        while (TryPenetrateState(state, kPenetrationCostFlesh,
                                 kPenetrationFalloffFlesh))
            ++crossings;
        CHECK(crossings == kPenetrationMaxSurfaces);
        CHECK(state.power > 0.0f);   // stopped by the cap, not the budget
    }

    // Damage only ever falls, and never goes negative or grows.
    {
        PenetrationState state{ 1000.0f, 0, 1.0f };
        float previous = state.damage;
        while (TryPenetrateState(state, kPenetrationCostFlesh,
                                 kPenetrationFalloffFlesh)) {
            CHECK(state.damage < previous);
            CHECK(state.damage > 0.0f);
            previous = state.damage;
        }
    }

    if (failures == 0) std::cout << "BulletPenetrationTests passed\n";
    return failures == 0 ? 0 : 1;
}
