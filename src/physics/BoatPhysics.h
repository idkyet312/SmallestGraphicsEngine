#pragma once

#include <box3d/box3d.h>
#include <algorithm>
#include <cmath>

namespace SGE {

// The origin stays at the model's waterline, so rendering and deck offsets do
// not need to change when the hull becomes a freely rotating rigid body.
struct BoatPhysics {
    static constexpr float Draft = 0.30f;
    static constexpr float HalfBeam = 1.5f;
    static constexpr float HalfLength = 4.5f;

    static b3BodyId Create(b3WorldId world, b3Pos position, b3Quat rotation,
                           b3Vec3 velocity, uint64_t category) {
        b3BodyDef definition = b3DefaultBodyDef();
        definition.type = b3_dynamicBody;
        definition.position = position;
        definition.rotation = rotation;
        definition.linearVelocity = velocity;
        definition.linearDamping = 0.04f;
        definition.angularDamping = 0.15f;
        definition.allowFastRotation = true;
        const b3BodyId body = b3CreateBody(world, &definition);
        // A narrow keel and wide gunwales let beach contacts rock the hull;
        // a flat full-width box would park upright like a shipping container.
        const b3Vec3 points[] = {
            {-0.12f,-Draft,-3.8f}, {0.12f,-Draft,-3.8f},
            {-0.12f,-Draft, 3.8f}, {0.12f,-Draft, 3.8f},
            {-HalfBeam,0.45f,-HalfLength}, {HalfBeam,0.45f,-HalfLength},
            {-HalfBeam,0.45f, HalfLength}, {HalfBeam,0.45f, HalfLength}
        };
        b3HullData* hull = b3CreateHull(points, 8, 8);
        if (!hull) { b3DestroyBody(body); return b3_nullBodyId; }
        b3ShapeDef shape = b3DefaultShapeDef();
        shape.density = 140.0f;
        shape.baseMaterial.friction = 0.65f;
        shape.baseMaterial.restitution = 0.08f;
        shape.filter.categoryBits = category;
        b3CreateHullShape(body, &shape, hull);
        b3DestroyHull(hull);
        return body;
    }

    template<class WaterAt>
    static void ApplyForces(b3BodyId body, float throttle, float steering,
                             bool brake, WaterAt waterAt) {
        const float mass = b3Body_GetMass(body);
        float wet = 0.0f;
        // Separate buoyancy points supply restoring torque in water. They exert
        // no force over dry ground, leaving gravity and terrain contacts to tip
        // and settle the hull on land instead of pinning it to a water plane.
        for (float x : {-HalfBeam * 0.75f, HalfBeam * 0.75f})
        for (float z : {-HalfLength * 0.75f, HalfLength * 0.75f}) {
            const b3Pos point = b3Body_GetWorldPoint(body, {x, -Draft, z});
            float surface = 0.0f;
            if (!waterAt((float)point.x, (float)point.z, surface)) continue;
            const float depth = surface - (float)point.y;
            if (depth <= 0.0f) continue;
            const b3Vec3 velocity = b3Body_GetWorldPointVelocity(body, point);
            const float lift = mass * 0.25f * (std::clamp)(
                9.81f * depth / Draft - velocity.y * 9.0f, 0.0f, 29.43f);
            b3Body_ApplyForce(body, {0.0f, lift, 0.0f}, point, true);
            wet += 0.25f;
        }
        if (wet <= 0.0f) return;
        const b3Quat rotation = b3Body_GetRotation(body);
        const b3Vec3 forward = b3RotateVector(rotation, {0,0,1});
        const b3Vec3 right = b3RotateVector(rotation, {1,0,0});
        const b3Vec3 velocity = b3Body_GetLinearVelocity(body);
        const float speed = b3Dot(velocity, forward);
        const float sideways = b3Dot(velocity, right);
        const float target = brake ? 0.0f : throttle * (throttle >= 0 ? 18.0f : 6.0f);
        const float acceleration = (std::clamp)((target - speed) * 1.5f,
            brake ? -16.0f : -5.0f, brake ? 16.0f : 5.0f);
        b3Vec3 force = b3MulSV(mass * wet * acceleration, forward);
        force = b3Sub(force, b3MulSV(mass * wet * sideways * 3.0f, right));
        b3Body_ApplyForceToCenter(body, force, true);
        const b3Vec3 angular = b3Body_GetAngularVelocity(body);
        const float turn = steering * (std::clamp)(speed / 5.0f, -1.0f, 1.0f) * 0.8f;
        // Rudder torque is water-dependent; there is no motor spinning a dry hull.
        const b3MassData inertia = b3Body_GetMassData(body);
        b3Body_ApplyTorque(body,
            {0, inertia.inertia.cy.y * wet * (turn - angular.y) * 2.0f, 0}, true);
    }
};

} // namespace SGE
