#ifndef PLAYER_INPUT_H
#define PLAYER_INPUT_H

#include <cstdint>

// One frame of player intent, separated from how it was produced.
//
// Movement used to be read straight off GetAsyncKeyState at the point of use,
// which meant the only description of "what the player is doing" was the key
// state itself -- untransmittable, and unavailable for anything but the local
// player. This struct is that description: ProcessInput fills it from the
// keyboard (or the on-screen pad), and Camera::ApplyInput consumes it. The
// same struct can therefore drive a remote player from the network without the
// movement code knowing the difference.
//
// Deliberately a POD of fixed-width types so it can be memcpy'd onto the wire.
// Analog axes stay floats rather than quantised ints until a real bandwidth
// measurement says otherwise -- guessing at a packing format before measuring
// is how wire protocols end up wrong and hard to change.
struct PlayerInput {
    // Analog movement, -1..1, matching VirtualInput's convention:
    // +forward is W, +strafe is A (left), mirroring the existing
    // forwardInput/strafeInput locals this replaced.
    float forward = 0.0f;
    float strafe  = 0.0f;

    // Absolute view angles in degrees rather than deltas. A dropped input
    // packet then costs one frame of staleness instead of permanently
    // desyncing the remote player's aim, which accumulated deltas would.
    float yaw   = 0.0f;
    float pitch = 0.0f;

    // Speed scale already resolved from crouch/sprint/stamina, so the movement
    // code keeps one authority for "how fast is this" (see the
    // movementMultiplier comment in WindowInput.h).
    float movementMultiplier = 1.0f;

    // Held/one-shot buttons. Packed into a bitfield for the wire; named
    // accessors keep the call sites readable.
    enum Button : uint16_t {
        Jump     = 1u << 0,
        Crouch   = 1u << 1,
        Sprint   = 1u << 2,
        Swim     = 1u << 3, // ascend while swimming
        SwimDown = 1u << 4,
    };
    uint16_t buttons = 0;

    // Monotonic per-client counter. Milestone 1 only logs gaps; client-side
    // reconciliation (replaying unacknowledged inputs) needs it later.
    uint32_t sequence = 0;

    // The frame this input was sampled over. Sent so the host integrates a
    // remote player with the client's own timestep rather than its own.
    float deltaTime = 0.0f;

    bool Held(Button button) const { return (buttons & button) != 0; }
    void Set(Button button, bool held) {
        buttons = static_cast<uint16_t>(
            held ? (buttons | button) : (buttons & ~button));
    }
    bool Moving() const {
        return forward != 0.0f || strafe != 0.0f;
    }
};

#endif // PLAYER_INPUT_H
