#ifndef HELICOPTER_FLIGHT_H
#define HELICOPTER_FLIGHT_H

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

// Arcade flight model for a helicopter the player is piloting. Shared by every
// airframe the player can take the stick of (the parked gunship, the insertion
// BlackHawk), so the two fly alike and the model can be tested without the
// renderer.
//
// The aircraft translates the way a helicopter does: input sets a target
// attitude, the airframe tilts toward it, and the tilt -- not the key -- is
// what accelerates it. Letting go of W therefore levels the nose first and
// bleeds speed through drag, rather than stopping dead.
//
// Attitude follows the convention both airframes already use: yaw is the
// heading with the nose along world (sin yaw, 0, cos yaw), negative pitch is
// nose down (forward flight), positive roll drops the right side. Right of the
// nose is world (cos yaw, 0, -sin yaw).

struct HelicopterFlightInput {
    float forward = 0.0f;   // W/S: + flies forward
    float strafe = 0.0f;    // + slides right
    float lift = 0.0f;      // Space/Ctrl: + climbs
    float yaw = 0.0f;       // A/D: + turns right
};

struct HelicopterFlightTuning {
    float maxSpeed = 42.0f;        // m/s level flight at full tilt
    float maxStrafeSpeed = 16.0f;  // m/s sideways
    float maxClimbRate = 9.0f;     // m/s
    float maxDescentRate = 11.0f;  // m/s
    float yawRate = 1.35f;         // rad/s at full pedal
    float maxPitch = 0.32f;        // rad of nose-down at full forward
    float maxRoll = 0.30f;         // rad of bank at full strafe
    float attitudeResponse = 3.2f; // 1/s, how fast the airframe tilts
    float drag = 0.55f;            // 1/s horizontal velocity decay
    float verticalResponse = 2.4f; // 1/s, how fast climb rate follows input
    float unpoweredSinkRate = 7.0f;// m/s an abandoned airframe settles at
    float restHeight = 0.0f;       // origin height above ground when parked
    float ceiling = 260.0f;        // m above the ground under it
};

struct HelicopterFlight {
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 velocity{};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    bool grounded = true;

    // Places the airframe at rest on the ground at `groundY`.
    void Park(const DirectX::XMFLOAT3& at, float heading, float groundY,
              const HelicopterFlightTuning& tuning) {
        position = { at.x, groundY + tuning.restHeight, at.z };
        velocity = {};
        yaw = heading;
        pitch = roll = 0.0f;
        grounded = true;
    }

    // Takes over an airframe that some other code was flying, keeping its
    // momentum so the handover does not jolt.
    void Adopt(const DirectX::XMFLOAT3& at, const DirectX::XMFLOAT3& atVelocity,
               float heading, float atPitch, float atRoll, bool onGround) {
        position = at;
        velocity = atVelocity;
        yaw = heading;
        pitch = atPitch;
        roll = atRoll;
        grounded = onGround;
    }

    float ForwardSpeed() const {
        return velocity.x * std::sin(yaw) + velocity.z * std::cos(yaw);
    }

    float HeightAboveGround(float groundY,
                            const HelicopterFlightTuning& tuning) const {
        return position.y - (groundY + tuning.restHeight);
    }

    // `powered` false is an empty cockpit: no attitude input and the airframe
    // sinks until it settles on the ground, so leaving it in the air does not
    // leave it hanging there forever.
    void Step(HelicopterFlightInput input, const HelicopterFlightTuning& tuning,
              float deltaTime, float groundY, bool powered = true) {
        const float dt = (std::max)(0.0f, (std::min)(deltaTime, 0.1f));
        if (dt <= 0.0f) return;
        const auto clampUnit = [](float v) {
            return (std::max)(-1.0f, (std::min)(1.0f, v));
        };
        if (!powered) input = {};
        input.forward = clampUnit(input.forward);
        input.strafe = clampUnit(input.strafe);
        input.lift = clampUnit(input.lift);
        input.yaw = clampUnit(input.yaw);

        // On the skids nothing tilts or turns until the collective lifts it.
        const bool lifting = input.lift > 0.0f;
        const bool airborne = !grounded || lifting;

        // Attitude chases the stick; the achieved tilt drives the motion.
        const float targetPitch = airborne ? -input.forward * tuning.maxPitch : 0.0f;
        const float targetRoll = airborne ? input.strafe * tuning.maxRoll : 0.0f;
        const float attitudeLerp =
            1.0f - std::exp(-tuning.attitudeResponse * dt);
        pitch += (targetPitch - pitch) * attitudeLerp;
        roll += (targetRoll - roll) * attitudeLerp;
        if (airborne) yaw += input.yaw * tuning.yawRate * dt;
        yaw = std::remainder(yaw, DirectX::XM_2PI);

        const float noseX = std::sin(yaw), noseZ = std::cos(yaw);
        const float rightX = noseZ, rightZ = -noseX;
        // Terminal speed is accel / drag, so scaling by drag pins full tilt to
        // maxSpeed whatever the drag is tuned to.
        const float forwardTilt = tuning.maxPitch > 0.0f
            ? -pitch / tuning.maxPitch : 0.0f;
        const float strafeTilt = tuning.maxRoll > 0.0f
            ? roll / tuning.maxRoll : 0.0f;
        const float forwardAccel = forwardTilt * tuning.maxSpeed * tuning.drag;
        const float strafeAccel = strafeTilt * tuning.maxStrafeSpeed * tuning.drag;
        if (airborne) {
            velocity.x += (noseX * forwardAccel + rightX * strafeAccel) * dt;
            velocity.z += (noseZ * forwardAccel + rightZ * strafeAccel) * dt;
        }
        const float dragFactor = std::exp(-tuning.drag * dt);
        velocity.x *= dragFactor;
        velocity.z *= dragFactor;

        const float targetVertical = !powered ? -tuning.unpoweredSinkRate
            : input.lift >= 0.0f ? input.lift * tuning.maxClimbRate
                                 : input.lift * tuning.maxDescentRate;
        const float verticalLerp =
            1.0f - std::exp(-tuning.verticalResponse * dt);
        velocity.y += (targetVertical - velocity.y) * verticalLerp;

        position.x += velocity.x * dt;
        position.y += velocity.y * dt;
        position.z += velocity.z * dt;

        const float floorY = groundY + tuning.restHeight;
        const float ceilingY = groundY + tuning.ceiling;
        if (position.y > ceilingY) {
            position.y = ceilingY;
            velocity.y = (std::min)(velocity.y, 0.0f);
        }
        grounded = false;
        if (position.y <= floorY) {
            position.y = floorY;
            velocity.y = (std::max)(velocity.y, 0.0f);
            // Skid friction: a settled airframe does not slide across a field.
            if (!lifting) {
                const float friction = std::exp(-6.0f * dt);
                velocity.x *= friction;
                velocity.z *= friction;
                grounded = true;
            }
        }
    }
};

#endif
