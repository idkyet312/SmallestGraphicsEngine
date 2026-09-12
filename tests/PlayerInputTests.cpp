#include "PlayerInput.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

// PlayerInput goes on the wire, so the properties that matter are: the button
// bitfield packs and unpacks without bleeding between flags, and a byte-for-byte
// copy (what a transport memcpy does) reproduces the original exactly. Both are
// pure, so this needs no socket and no device.
int main() {
    // Buttons are independent: setting one must not disturb the others, and
    // clearing one must not clear its neighbours.
    const PlayerInput::Button all[] = {
        PlayerInput::Jump, PlayerInput::Crouch, PlayerInput::Sprint,
        PlayerInput::Swim, PlayerInput::SwimDown,
    };
    for (PlayerInput::Button set : all) {
        PlayerInput input;
        input.Set(set, true);
        for (PlayerInput::Button probe : all)
            Check(input.Held(probe) == (probe == set),
                  "Setting one button disturbed another");
        input.Set(set, false);
        Check(input.buttons == 0, "Clearing a button left residue");
    }

    // Every button held at once, then cleared one at a time.
    PlayerInput every;
    for (PlayerInput::Button button : all) every.Set(button, true);
    for (PlayerInput::Button button : all)
        Check(every.Held(button), "Lost a button while setting the rest");
    for (PlayerInput::Button button : all) {
        every.Set(button, false);
        Check(!every.Held(button), "Button stayed held after clear");
    }
    Check(every.buttons == 0, "Buttons left set after clearing all");

    // Round-trip through raw bytes, standing in for the transport's memcpy.
    PlayerInput sent;
    sent.forward = -0.75f;
    sent.strafe = 0.5f;
    sent.yaw = 123.5f;
    sent.pitch = -42.25f;
    sent.movementMultiplier = 1.5f;
    sent.sequence = 4242u;
    sent.deltaTime = 1.0f / 60.0f;
    sent.Set(PlayerInput::Sprint, true);
    sent.Set(PlayerInput::Jump, true);

    unsigned char buffer[sizeof(PlayerInput)];
    std::memcpy(buffer, &sent, sizeof(buffer));
    PlayerInput received;
    std::memcpy(&received, buffer, sizeof(buffer));

    Check(received.forward == sent.forward, "forward did not round-trip");
    Check(received.strafe == sent.strafe, "strafe did not round-trip");
    Check(received.yaw == sent.yaw, "yaw did not round-trip");
    Check(received.pitch == sent.pitch, "pitch did not round-trip");
    Check(received.movementMultiplier == sent.movementMultiplier,
          "movementMultiplier did not round-trip");
    Check(received.sequence == sent.sequence, "sequence did not round-trip");
    Check(received.deltaTime == sent.deltaTime, "deltaTime did not round-trip");
    Check(received.buttons == sent.buttons, "buttons did not round-trip");
    Check(received.Held(PlayerInput::Sprint) && received.Held(PlayerInput::Jump),
          "Held flags did not survive the round trip");
    Check(!received.Held(PlayerInput::Crouch),
          "An unset flag came back set");

    // Moving() is what the animation and snapshot code will branch on, so pin
    // its edges: neither axis moving is idle, either axis alone is moving.
    PlayerInput idle;
    Check(!idle.Moving(), "Default input reported as moving");
    idle.strafe = -0.01f;
    Check(idle.Moving(), "Strafe-only input reported as idle");
    idle.strafe = 0.0f;
    idle.forward = 0.01f;
    Check(idle.Moving(), "Forward-only input reported as idle");

    std::cout << "PlayerInput tests passed\n";
    return 0;
}
