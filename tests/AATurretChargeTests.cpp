#include "AATurretCharge.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static bool Near(float a, float b) { return std::abs(a - b) < 0.001f; }

int main() {
    using namespace SGE;
    using DirectX::XMFLOAT3;
    VehicleSystem::AATurret turret;
    turret.position = { 10.0f, 2.0f, 20.0f };
    turret.yaw = 0.0f;
    turret.pitch = 0.0f;
    XMFLOAT3 position, normal;

    ResolveAATurretCharge(turret, ChargeAnchorPart::AATurretBase,
        { 1.0f, 0.2f, 0.0f }, { 1.0f, 0.0f, 0.0f }, position, normal);
    Check(Near(position.x, 11.0f) && Near(position.y, 2.2f),
          "base attachment should use the fixed base pose");
    turret.yaw = DirectX::XM_PIDIV2;
    turret.pitch = 0.4f;
    ResolveAATurretCharge(turret, ChargeAnchorPart::AATurretBase,
        { 1.0f, 0.2f, 0.0f }, { 1.0f, 0.0f, 0.0f }, position, normal);
    Check(Near(position.x, 11.0f) && Near(position.y, 2.2f),
          "base charge moved as the gun aimed");

    ResolveAATurretCharge(turret, ChargeAnchorPart::AATurretTraverse,
        { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f }, position, normal);
    Check(Near(position.x, 11.0f) &&
          Near(position.y, 2.0f + VehicleSystem::AATurretMountHeight) &&
          Near(position.z, 20.0f) && Near(normal.x, 1.0f),
          "traverse charge failed to follow yaw");

    ResolveAATurretCharge(turret, ChargeAnchorPart::AATurretElevation,
        { 0.0f, 0.0f, 2.0f }, { 0.0f, 0.0f, 1.0f }, position, normal);
    Check(Near(position.x, 10.0f + 2.0f * std::cos(0.4f)) &&
          Near(position.y, 2.0f + VehicleSystem::AATurretMountHeight +
                           2.0f * std::sin(0.4f)) &&
          Near(normal.y, std::sin(0.4f)),
          "barrel charge failed to follow elevation");

    turret.position = { 0.0f, 0.0f, 0.0f };
    turret.yaw = 0.0f;
    turret.pitch = 0.0f;
    AATurretChargeHit hit;
    Check(HitAATurretChargeSegment(turret, { 3.0f, 0.15f, 1.0f },
                                  { 0.0f, 0.15f, 1.0f }, 0.16f, hit) &&
          hit.part == ChargeAnchorPart::AATurretBase,
          "base collision missed");
    hit = {};
    Check(HitAATurretChargeSegment(turret, { 3.0f, 1.9f, 0.0f },
                                  { 0.0f, 1.9f, 0.0f }, 0.16f, hit) &&
          hit.part == ChargeAnchorPart::AATurretTraverse,
          "mount collision missed");
    hit = {};
    Check(HitAATurretChargeSegment(turret, { 0.24f, 3.0f, 2.5f },
                                  { 0.24f, 1.0f, 2.5f }, 0.16f, hit) &&
          hit.part == ChargeAnchorPart::AATurretElevation,
          "barrel collision missed");
    Check(!AATurretChargeIsNearer({ 0, 0, 0 }, { 2, 0, 0 }, { 1, 0, 0 }),
          "a wall in front of the turret must catch the charge");
    Check(AATurretChargeIsNearer({ 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 }),
          "a turret in front of the wall must catch the charge");
    turret.dead = true;
    hit = {};
    Check(!HitAATurretChargeSegment(turret, { 3, 0.15f, 1 },
                                   { 0, 0.15f, 1 }, 0.16f, hit),
          "destroyed turrets must not accept new charges");
    VehicleSystem vehicles;
    const size_t target = vehicles.PlaceAATurret({ 0.0f, 0.0f, 0.0f });
    const auto blast = vehicles.DamageAATurret(
        target, VehicleSystem::AATurretMaxHealth);
    Check(blast.destroyed && !vehicles.aaTurrets[target].Active(),
          "a direct C4 blast must destroy the AA turret");
    return 0;
}
