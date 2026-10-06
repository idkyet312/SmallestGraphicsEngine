#include "BoatPhysics.h"
#include "VehicleSystem.h"
#include <iostream>

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    if (ok) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
b3WorldId World(float ground) {
    b3WorldDef worldDef = b3DefaultWorldDef();
    worldDef.gravity = {0,-9.81f,0};
    const b3WorldId world = b3CreateWorld(&worldDef);
    b3BodyDef floorDef = b3DefaultBodyDef();
    floorDef.position = {0,ground - 0.5f,0};
    const b3BodyId floor = b3CreateBody(world, &floorDef);
    b3ShapeDef shape = b3DefaultShapeDef();
    shape.baseMaterial.friction = 0.7f;
    b3BoxHull box = b3MakeBoxHull(200,0.5f,200);
    b3CreateHullShape(floor, &shape, &box.base);
    return world;
}
void Step(b3WorldId world, b3BodyId boat, float seconds,
           float ground, float throttle = 0, float steering = 0) {
    for (int i = 0; i < int(seconds * 60); ++i) {
        SGE::BoatPhysics::ApplyForces(boat, throttle, steering, throttle == 0,
            [ground](float, float, float& water) {
                water = 0;
                return ground < water;
            });
        b3World_Step(world, 1.0f / 60.0f, 4);
    }
}
}

int main() {
    using SGE::BoatPhysics;
    Check(BoatPhysics::Draft == VehicleSystem::BoatFloatDepth,
          "Rendered hull depth and physical hull draft must agree");
    for (const float ground : {-5.0f, -0.4f}) {
        const b3WorldId world = World(ground);
        const b3BodyId boat = BoatPhysics::Create(world, {0,0,0},
            b3Quat_identity, {}, B3_DEFAULT_CATEGORY_BITS);
        Check(!B3_IS_NULL(boat), "Boat hull must create a dynamic body");
        Step(world, boat, 5, ground);
        const auto p = b3Body_GetPosition(boat);
        const auto up = b3RotateVector(b3Body_GetRotation(boat), {0,1,0});
        Check(std::abs((float)p.y) < 0.08f && up.y > 0.98f,
              "Buoyancy must settle upright at the waterline in deep and 40 cm water");
        Step(world, boat, 2, ground, 1);
        Check(b3Body_GetPosition(boat).z > 4 &&
              b3Body_GetLinearVelocity(boat).z > 3,
              "A boat must propel through shallow water without a shore-position clamp");
        Step(world, boat, 1, ground, 1, 1);
        Check(std::abs(b3Body_GetRotation(boat).v.y) > 0.03f,
              "The rudder must turn a moving boat in water");
        b3DestroyWorld(world);
    }
    const b3WorldId land = World(0.8f);
    const b3Quat tilt = {{0,0,std::sin(0.18f)},std::cos(0.18f)};
    const b3BodyId stranded = BoatPhysics::Create(land, {0,3,0}, tilt,
        {0,0,2}, B3_DEFAULT_CATEGORY_BITS);
    Step(land, stranded, 4, 0.8f, 1, 1);
    const auto settled = b3Body_GetPosition(stranded);
    const auto up = b3RotateVector(b3Body_GetRotation(stranded), {0,1,0});
    Check(std::isfinite((float)settled.y) && settled.y > 0.4f && settled.y < 1.6f,
          "A dry boat must fall and settle against the real ground rather than sink through it");
    Check(up.y < 0.95f,
          "An off-balance narrow hull must flop on land with free roll and pitch");
    const float oldZ = (float)settled.z;
    Step(land, stranded, 2, 0.8f, 1, 1);
    Check(std::abs((float)b3Body_GetPosition(stranded).z - oldZ) < 0.4f,
          "Holding throttle and steering must not motor a beached boat across dry land");
    b3DestroyWorld(land);
    if (!failures) std::cout << "BoatPhysicsTests passed\n";
    return failures ? 1 : 0;
}
