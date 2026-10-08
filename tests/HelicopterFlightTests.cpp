#include "HelicopterFlight.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

static void Fly(HelicopterFlight& flight, const HelicopterFlightInput& input,
                const HelicopterFlightTuning& tuning, float seconds,
                float groundY = 0.0f, bool powered = true) {
    constexpr float dt = 1.0f / 60.0f;
    for (float t = 0.0f; t < seconds; t += dt)
        flight.Step(input, tuning, dt, groundY, powered);
}

int main() {
    HelicopterFlightTuning tuning;
    tuning.restHeight = 2.0f;

    {
        HelicopterFlight flight;
        flight.Park({ 10.0f, 50.0f, -4.0f }, 0.0f, 5.0f, tuning);
        Check(flight.grounded && flight.position.y == 7.0f,
              "parking must rest the origin restHeight above the ground");
        Fly(flight, {}, tuning, 2.0f, 5.0f);
        Check(flight.grounded && std::abs(flight.position.y - 7.0f) < 1e-4f,
              "a parked airframe with no input must stay on the ground");
        HelicopterFlightInput turn;
        turn.yaw = 1.0f;
        turn.forward = 1.0f;
        Fly(flight, turn, tuning, 2.0f, 5.0f);
        Check(flight.yaw == 0.0f && flight.pitch == 0.0f &&
              std::abs(flight.position.x - 10.0f) < 1e-4f &&
              std::abs(flight.position.z + 4.0f) < 1e-4f,
              "on the skids the stick and pedals must do nothing until it lifts");
    }

    {
        HelicopterFlight flight;
        flight.Park({ 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, tuning);
        HelicopterFlightInput climb;
        climb.lift = 1.0f;
        Fly(flight, climb, tuning, 5.0f);
        Check(!flight.grounded && flight.position.y > 25.0f,
              "full collective must climb clear of the ground");
        Check(std::abs(flight.velocity.y - tuning.maxClimbRate) < 0.5f,
              "climb rate must settle at maxClimbRate");

        HelicopterFlightInput forward;
        forward.forward = 1.0f;
        Fly(flight, forward, tuning, 20.0f);
        Check(flight.pitch < -0.9f * tuning.maxPitch,
              "forward flight must hold the nose down (negative pitch)");
        Check(flight.velocity.z > 0.9f * tuning.maxSpeed &&
              flight.velocity.z <= tuning.maxSpeed * 1.001f,
              "full forward at heading 0 must approach maxSpeed along +Z");
        Check(std::abs(flight.velocity.x) < 0.01f,
              "heading 0 must not drift sideways");

        Fly(flight, {}, tuning, 12.0f);
        Check(std::abs(flight.pitch) < 0.01f && flight.ForwardSpeed() < 2.0f,
              "releasing the stick must level the nose and bleed speed off");
    }

    {
        HelicopterFlight flight;
        flight.Adopt({ 0.0f, 40.0f, 0.0f }, {}, 0.0f, 0.0f, 0.0f, false);
        HelicopterFlightInput right;
        right.yaw = 1.0f;
        Fly(flight, right, tuning, 0.5f);
        Check(flight.yaw > 0.0f, "D must turn toward +X (positive yaw)");
        HelicopterFlightInput slide;
        slide.strafe = 1.0f;
        flight.Adopt({ 0.0f, 40.0f, 0.0f }, {}, 0.0f, 0.0f, 0.0f, false);
        Fly(flight, slide, tuning, 3.0f);
        Check(flight.roll > 0.0f && flight.velocity.x > 1.0f,
              "strafing right must bank right side down and slide toward +X");
    }

    {
        HelicopterFlight flight;
        flight.Adopt({ 0.0f, 60.0f, 0.0f }, { 0.0f, 0.0f, 20.0f }, 0.0f,
                     -0.2f, 0.0f, false);
        HelicopterFlightInput ignored;
        ignored.lift = 1.0f;
        ignored.forward = 1.0f;
        Fly(flight, ignored, tuning, 30.0f, 0.0f, /*powered=*/false);
        Check(flight.grounded && std::abs(flight.position.y - 2.0f) < 1e-3f,
              "an abandoned airframe must settle onto the ground, not hang");
        Check(std::abs(flight.velocity.x) + std::abs(flight.velocity.z) < 0.1f,
              "a settled airframe must not keep sliding");
    }

    {
        HelicopterFlight flight;
        flight.Adopt({ 0.0f, 200.0f, 0.0f }, {}, 0.0f, 0.0f, 0.0f, false);
        HelicopterFlightInput climb;
        climb.lift = 1.0f;
        Fly(flight, climb, tuning, 30.0f, 0.0f);
        Check(flight.position.y <= tuning.ceiling + 1e-3f,
              "climb must stop at the ceiling above the ground");
    }

    {
        HelicopterFlight flight;
        flight.Adopt({ 0.0f, 40.0f, 0.0f }, {}, 0.0f, 0.0f, 0.0f, false);
        const HelicopterFlight before = flight;
        flight.Step({ 1.0f, 1.0f, 1.0f, 1.0f }, tuning, 0.0f, 0.0f);
        Check(flight.position.y == before.position.y && flight.yaw == before.yaw,
              "a zero-length step must not move the airframe");
    }

    std::cout << "HelicopterFlightTests passed\n";
    return 0;
}
