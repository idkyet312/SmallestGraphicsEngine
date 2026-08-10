#include "DeferredReleaseQueue.h"
#include "AnimationRuntime.h"
#include "AnimationClipUtils.h"
#include "FixedStepClock.h"
#include "GameCommandQueue.h"
#include "GameRuntime.h"
#include "GameSession.h"
#include "CombatSystem.h"
#include "VehicleSystem.h"
#include "DeploymentPlanner.h"
#include "LevelLoadingController.h"
#include "LevelRuntimeBuilder.h"
#include "PlayerState.h"
#include "PlayerMovementTracker.h"
#include "ProceduralRunAnimation.h"
#include "RenderCoordinator.h"
#include "RuntimeWorld.h"

#include <iostream>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

int main() {
    Skeleton additiveSkeleton;
    additiveSkeleton.names.push_back("root");
    additiveSkeleton.parent.push_back(-1);
    DirectX::XMFLOAT4X4 identity;
    DirectX::XMStoreFloat4x4(
        &identity, DirectX::XMMatrixIdentity());
    additiveSkeleton.offset.push_back(identity);
    additiveSkeleton.localBind.push_back(identity);
    additiveSkeleton.globalInverse = identity;

    AnimationClip baseClip;
    baseClip.duration = 1.0f;
    BoneTrack baseTrack;
    baseTrack.bone = 0;
    baseTrack.positions.push_back({ 0.0f, { 0.0f, 10.0f, 0.0f } });
    baseClip.tracks.push_back(baseTrack);
    AnimationClip additiveClip;
    additiveClip.duration = 1.0f;
    BoneTrack additiveTrack;
    additiveTrack.bone = 0;
    additiveTrack.positions.push_back({ 0.0f, { 0.0f, 2.0f, 0.0f } });
    additiveTrack.positions.push_back({ 1.0f, { 0.0f, 6.0f, 0.0f } });
    additiveClip.tracks.push_back(additiveTrack);

    AnimationInstance baseAnimation;
    AnimationInstance additiveAnimation;
    baseAnimation.Play(&baseClip);
    additiveAnimation.Play(&additiveClip);
    additiveAnimation.time = 1.0f;
    std::vector<DirectX::XMFLOAT4X4> additivePalette;
    std::vector<DirectX::XMFLOAT4X4> additiveGlobals;
    baseAnimation.ComputeAdditivePalette(
        additiveSkeleton, additiveAnimation, 0.0f, 0.5f,
        additivePalette, &additiveGlobals);
    CHECK(std::abs(additiveGlobals[0]._42 - 12.0f) < 0.0001f);

    AnimationClip rangedIdleClip;
    rangedIdleClip.duration = 1.0f;
    BoneTrack rangedIdleTrack;
    rangedIdleTrack.bone = 0;
    rangedIdleTrack.positions.push_back({ 0.0f, { 0.0f, 0.0f, 0.0f } });
    rangedIdleTrack.positions.push_back({ 1.0f, { 0.0f, 10.0f, 0.0f } });
    rangedIdleClip.tracks.push_back(rangedIdleTrack);
    AnimationInstance rangedIdleAnimation;
    AnimationInstance noAdditiveAnimation;
    rangedIdleAnimation.Play(&rangedIdleClip);
    rangedIdleAnimation.time = 1.0f;
    rangedIdleAnimation.ComputeAdditivePalette(
        additiveSkeleton, noAdditiveAnimation, 0.0f, 0.0f,
        additivePalette, &additiveGlobals, 0.2f, 0.0f);
    CHECK(std::abs(additiveGlobals[0]._42 - 2.0f) < 0.0001f);

    AnimationClip discontinuousLoop;
    discontinuousLoop.duration = 1.0f;
    BoneTrack loopTrack;
    loopTrack.bone = 0;
    loopTrack.positions.push_back({ 0.0f, { 0.0f, 0.0f, 0.0f } });
    loopTrack.positions.push_back({ 1.0f, { 0.0f, 10.0f, 0.0f } });
    discontinuousLoop.tracks.push_back(loopTrack);
    AnimationInstance loopAnimation;
    loopAnimation.Play(&discontinuousLoop);
    loopAnimation.loopBlendDuration = 0.1f;
    std::vector<DirectX::XMFLOAT4X4> beforeLoop;
    std::vector<DirectX::XMFLOAT4X4> afterLoop;
    loopAnimation.time = 0.999f;
    loopAnimation.ComputeGlobalMatrices(additiveSkeleton, beforeLoop);
    loopAnimation.time = 0.001f;
    loopAnimation.ComputeGlobalMatrices(additiveSkeleton, afterLoop);
    CHECK(std::abs(beforeLoop[0]._42 - afterLoop[0]._42) < 0.2f);

    Skeleton proceduralSkeleton;
    proceduralSkeleton.names = {
        "mixamorig:Hips", "mixamorig:Spine2",
        "mixamorig:LeftArm", "mixamorig:RightArm"
    };
    proceduralSkeleton.parent = { -1, 0, 1, 1 };
    for (size_t i = 0; i < proceduralSkeleton.names.size(); ++i) {
        proceduralSkeleton.localBind.push_back(identity);
        proceduralSkeleton.offset.push_back(identity);
    }
    proceduralSkeleton.globalInverse = identity;
    const AnimationClip proceduralRun =
        ProceduralRunAnimation::Build(proceduralSkeleton);
    CHECK(proceduralRun.name == "Procedural Run");
    CHECK(proceduralRun.duration > 0.0f);
    CHECK(proceduralRun.tracks.size() == 4);
    for (const BoneTrack& track : proceduralRun.tracks) {
        CHECK(!track.positions.empty());
        CHECK(!track.rotations.empty());
        CHECK(std::abs(track.positions.front().value.x -
                       track.positions.back().value.x) < 0.0001f);
        CHECK(std::abs(track.positions.front().value.y -
                       track.positions.back().value.y) < 0.0001f);
        CHECK(std::abs(track.rotations.front().value.x -
                       track.rotations.back().value.x) < 0.0001f);
    }

    AnimationClip idleClip;
    BoneTrack idleHips;
    idleHips.bone = 3;
    idleHips.positions.push_back({ 0.0f, { -0.276f, 94.204f, 0.059f } });
    idleClip.tracks.push_back(idleHips);
    AnimationClip runClip;
    BoneTrack runHips;
    runHips.bone = 3;
    runHips.positions.push_back({ 0.0f, { -0.237f, 81.832f, 1.404f } });
    runHips.positions.push_back({ 1.0f, { -0.175f, 82.092f, 1.403f } });
    runClip.tracks.push_back(runHips);
    CHECK(AnimationClipUtils::RebaseTranslationOrigin(
        idleClip, runClip) == 1);
    CHECK(std::abs(runClip.tracks[0].positions[0].value.y - 94.204f) <
        0.0001f);
    CHECK(std::abs(runClip.tracks[0].positions[0].value.z - 0.059f) <
        0.0001f);
    CHECK(std::abs(
        (runClip.tracks[0].positions[1].value.y -
         runClip.tracks[0].positions[0].value.y) - 0.260f) < 0.0001f);

    GameCommandQueue commands;
    commands.Request(GameCommand::RebuildDDGI);
    CHECK(commands.Pending(GameCommand::RebuildDDGI));
    CHECK(commands.Consume(GameCommand::RebuildDDGI));
    CHECK(!commands.Consume(GameCommand::RebuildDDGI));
    commands.Request(GameCommand::EditorBeginPlay);
    commands.Clear();
    CHECK(!commands.Pending(GameCommand::EditorBeginPlay));

    using LoadClock = LevelLoadingController::Clock;
    const auto loadStart = LoadClock::time_point{};
    LevelLoadingController loading;
    loading.Begin({ 3, "First", "asset-a" }, loadStart);
    CHECK(loading.Active());
    CHECK(loading.Stage() == LevelLoadStage::WorldAssets);
    CHECK(loading.TaskIndex() == 1);
    loading.RecordSubmittedUploads(2);
    loading.Advance(LevelLoadStage::Environment, "Second", "asset-b", true,
        loadStart + std::chrono::milliseconds(5));
    CHECK(loading.TaskIndex() == 2);
    CHECK(loading.Records().size() == 1);
    CHECK(loading.Records().front().milliseconds == 5.0);
    CHECK(loading.SubmittedUploads() == 2);
    loading.Complete(true, loadStart + std::chrono::milliseconds(9));
    CHECK(!loading.Active());
    CHECK(loading.Progress() == 1.0f);
    CHECK(loading.Records().size() == 2);

    GameRuntime runtime;
    runtime.combat.heldBarrelIndex = 9;
    runtime.vehicles.drivingHumvee = true;
    runtime.commands.Request(GameCommand::ResetDDGIHistory);
    runtime.ResetLevelState();
    CHECK(runtime.combat.heldBarrelIndex == SIZE_MAX);
    CHECK(!runtime.vehicles.drivingHumvee);
    CHECK(!runtime.commands.Pending(GameCommand::ResetDDGIHistory));

    PlayerMovementTracker movement;
    CHECK(movement.Update({ 0.0f, 0.0f, 0.0f }, 0.1f) == 0.0f);
    CHECK(movement.Update({ 0.5f, 5.0f, 0.0f }, 0.1f) == 5.0f);
    CHECK(movement.Update({ 100.0f, 5.0f, 0.0f }, 0.1f) == 0.0f);
    CHECK(movement.Update({ 100.5f, 5.0f, 0.0f }, 0.1f, false) == 0.0f);

    PlayerMovementTracker platformMovement;
    CHECK(platformMovement.Update({ 0.0f, 0.0f, 0.0f }, 0.1f) == 0.0f);
    CHECK(std::abs(platformMovement.Update(
        { 0.2f, 0.0f, 0.0f }, 0.1f) - 2.0f) < 0.001f);
    platformMovement.ApplyPlatformDisplacement({ 1.0f, 0.0f, 0.0f });
    CHECK(std::abs(platformMovement.Update(
        { 1.4f, 0.0f, 0.0f }, 0.1f) - 2.0f) < 0.001f);

    GameSession session;
    CHECK(session.Screen() == GameScreen::MainMenu);
    session.SetScreen(GameScreen::Level1);
    session.ResetTimer(true);
    session.Tick(1.0f);
    CHECK(session.ElapsedSeconds() == 0.25f);
    session.StopTimer();
    session.Tick(0.1f);
    CHECK(session.ElapsedSeconds() == 0.25f);

    FixedStepClock clock(0.1f, 3);
    clock.Accumulate(1.0f);
    float step = 0.0f;
    int steps = 0;
    while (clock.Consume(step)) ++steps;
    CHECK(steps == 3);
    CHECK(step == 0.1f);

    CHECK(RenderCoordinator::Choose(
        { true, true, true, true }) == RenderPath::Raytracing);
    CHECK(RenderCoordinator::Choose(
        { true, true, false, true }) == RenderPath::VisibilityBuffer);
    CHECK(RenderCoordinator::Choose(
        { false, true, false, false }) == RenderPath::Forward);

    LevelDefinition level = MakeLevelOneTemplate();
    RuntimeLevelPlan plan = LevelRuntimeBuilder::Build(level);
    CHECK(plan.playerSpawn.has_value());
    CHECK(plan.humveeSpawn.has_value());
    CHECK(plan.helicopterSpawn.has_value());
    CHECK(!plan.explosiveBarrels.empty());

    level.entities.front().enabled = false;
    plan = LevelRuntimeBuilder::Build(level);
    CHECK(!plan.playerSpawn.has_value());

    VehicleSystem vehicles;
    float rotorSpeed = 1.0f;
    for (int frame = 0; frame < 150; ++frame)
        rotorSpeed = VehicleSystem::StepHelicopterRotorSpeed(
            rotorSpeed, false, 1.0f / 60.0f);
    CHECK(std::abs(rotorSpeed - 0.5f) < 0.001f);
    for (int frame = 0; frame < 150; ++frame)
        rotorSpeed = VehicleSystem::StepHelicopterRotorSpeed(
            rotorSpeed, false, 1.0f / 60.0f);
    CHECK(rotorSpeed < 0.001f);
    vehicles.humveeModelScale = 3.0f;
    vehicles.helicopterDead = true;
    vehicles.drivingHumvee = true;
    vehicles.ResetLevel();
    CHECK(vehicles.humveeModelScale == 3.0f);
    CHECK(!vehicles.helicopterDead);
    CHECK(vehicles.helicopterRotorSpeedScale == 1.0f);
    CHECK(!vehicles.drivingHumvee);
    auto vehicleDamage = vehicles.DamagePrimaryHelicopter(500.0f);
    CHECK(vehicleDamage.applied);
    CHECK(!vehicleDamage.destroyed);
    vehicleDamage = vehicles.DamagePrimaryHelicopter(1500.0f);
    CHECK(vehicleDamage.destroyed);
    CHECK(vehicles.helicopterDead);

    vehicles.insertionBoatPhase = VehicleSystem::InsertionBoatPhase::Inbound;
    vehicles.insertionBoatHealth = VehicleSystem::InsertionBoatMaxHealth;
    vehicles.UpdateInsertionBoat(10.0f);
    CHECK(vehicles.insertionBoatHealth == VehicleSystem::InsertionBoatMaxHealth);
    vehicleDamage = vehicles.DamageInsertionBoatFromEnemyFire(
        VehicleSystem::InsertionBoatMaxHealth * 0.35f);
    CHECK(vehicleDamage.applied);
    CHECK(!vehicleDamage.destroyed);
    CHECK(std::abs(vehicles.insertionBoatHealth -
        VehicleSystem::InsertionBoatMaxHealth * 0.65f) < 0.001f);
    vehicleDamage = vehicles.DamageInsertionBoatFromEnemyFire(
        VehicleSystem::InsertionBoatMaxHealth * 0.65f);
    CHECK(vehicleDamage.destroyed);
    CHECK(vehicles.InsertionBoatIsFoundering());
    CHECK(!vehicles.DamageInsertionBoatFromEnemyFire(1.0f).applied);

    vehicles.blackHawkPhase = VehicleSystem::BlackHawkPhase::Inbound;
    vehicles.blackHawkHealth = VehicleSystem::BlackHawkMaxHealth;
    vehicles.UpdateBlackHawk(10.0f);
    CHECK(vehicles.blackHawkHealth == VehicleSystem::BlackHawkMaxHealth);

    VehicleSystem normalInsertion;
    VehicleSystem fastInsertion;
    normalInsertion.BeginBlackHawkInsertion({ 0.0f, 0.0f, 0.0f },
                                             0.0f, 0.0f, false);
    fastInsertion.BeginBlackHawkInsertion({ 0.0f, 0.0f, 0.0f },
                                           0.0f, 0.0f, true);
    normalInsertion.UpdateBlackHawk(1.0f);
    fastInsertion.UpdateBlackHawk(1.0f);
    const float normalTravel = normalInsertion.blackHawkPosition.z +
        VehicleSystem::BlackHawkApproachDistance;
    const float fastTravel = fastInsertion.blackHawkPosition.z +
        VehicleSystem::BlackHawkApproachDistance;
    CHECK(std::abs(fastTravel - normalTravel *
        VehicleSystem::BlackHawkFastSpeedMultiplier) < 0.001f);
    for (int stepIndex = 0;
         stepIndex < 1000 &&
             fastInsertion.blackHawkPhase ==
                 VehicleSystem::BlackHawkPhase::Inbound;
         ++stepIndex)
        fastInsertion.UpdateBlackHawk(0.05f);
    CHECK(fastInsertion.BlackHawkIsRappelling());
    CHECK(std::abs(fastInsertion.blackHawkPosition.y -
        VehicleSystem::BlackHawkRappelHoverHeight) < 0.001f);
    fastInsertion.UpdateBlackHawk(
        VehicleSystem::BlackHawkRappelTime * 0.5f);
    CHECK(std::abs(fastInsertion.blackHawkRappelProgress - 0.5f) < 0.001f);
    CHECK(fastInsertion.blackHawkCarryingPlayer);
    fastInsertion.UpdateBlackHawk(
        VehicleSystem::BlackHawkRappelTime * 0.5f);
    CHECK(fastInsertion.blackHawkDroppedPlayer);
    CHECK(!fastInsertion.blackHawkCarryingPlayer);
    CHECK(fastInsertion.blackHawkPhase ==
        VehicleSystem::BlackHawkPhase::Departing);

    vehicleDamage = vehicles.DamageInsertionBlackHawkFromEnemyFire(
        VehicleSystem::BlackHawkMaxHealth * 0.40f);
    CHECK(vehicleDamage.applied);
    CHECK(!vehicleDamage.destroyed);
    CHECK(std::abs(vehicles.blackHawkHealth -
        VehicleSystem::BlackHawkMaxHealth * 0.60f) < 0.001f);
    vehicleDamage = vehicles.DamageInsertionBlackHawkFromEnemyFire(
        VehicleSystem::BlackHawkMaxHealth * 0.60f);
    CHECK(vehicleDamage.destroyed);
    CHECK(vehicles.BlackHawkIsCrashing());
    CHECK(!vehicles.DamageInsertionBlackHawkFromEnemyFire(1.0f).applied);

    CombatSystem combat;
    combat.heldBarrelIndex = 4;
    combat.ResetLevel();
    CHECK(combat.heldBarrelIndex == SIZE_MAX);
    CHECK(combat.suppressFireUntilMouseRelease);

    PlayerState player;
    CHECK(std::abs(player.HealthRegenPerSecond() - 50.0f) < 0.0001f);
    CHECK(std::abs(player.HealthRegenPerSecond() * player.regenDuration -
                   player.maxHealth) < 0.0001f);
    player.magazine[0] = 1;
    CHECK(player.ConsumeAmmo(0));
    CHECK(player.magazine[0] == 0);
    CHECK(!player.ConsumeAmmo(0));
    player.reserve[0] = 5;
    CHECK(player.BeginReload(0));
    player.UpdateReload(10.0f);
    CHECK(player.magazine[0] == 5);
    CHECK(player.reserve[0] == 0);
    CHECK(PlayerState::kWeaponSlots == 8);
    player.magazine[4] = 1;
    CHECK(player.ConsumeAmmo(4));
    CHECK(player.magazine[4] == 0);
    player.reserve[4] = 7;
    CHECK(player.BeginReload(4));
    player.UpdateReload(10.0f);
    CHECK(player.magazine[4] == 7);
    CHECK(player.reserve[4] == 0);
    CHECK(player.magazineSize[7] == 1);

    PlayerState fastRappelLoadout;
    fastRappelLoadout.magazine[0] = 5;
    fastRappelLoadout.reserve[0] = 9;
    fastRappelLoadout.magazine[2] = 1;
    fastRappelLoadout.reserve[2] = 5;
    fastRappelLoadout.reloadTimer = 1.0f;
    fastRappelLoadout.reloadingSlot = 0;
    fastRappelLoadout.HalveAmmo();
    CHECK(fastRappelLoadout.magazine[0] == 2);
    CHECK(fastRappelLoadout.reserve[0] == 4);
    CHECK(fastRappelLoadout.magazine[2] == 0);
    CHECK(fastRappelLoadout.reserve[2] == 2);
    CHECK(!fastRappelLoadout.Reloading());
    CHECK(fastRappelLoadout.reloadingSlot == -1);
    CHECK(player.maxReserve[7] == 24);

    const auto deploymentZones = DeploymentPlanner::BuildPerimeterZones(
        34.0f, 68.0f, 8,
        [](float x, float z) { return x * 0.25f + z * 0.5f; });
    CHECK(deploymentZones.size() == 8);
    CHECK(std::abs(deploymentZones[0].x) < 0.001f);
    CHECK(std::abs(deploymentZones[0].z - 68.0f) < 0.001f);
    CHECK(std::abs(deploymentZones[0].y - 34.0f) < 0.001f);
    CHECK(std::abs(deploymentZones[2].x - 34.0f) < 0.001f);
    CHECK(std::abs(deploymentZones[2].z) < 0.001f);
    const float westwardHeading =
        DeploymentPlanner::HeadingTowardIslandCenter({ 10.0f, 0.0f, 0.0f });
    CHECK(std::abs(westwardHeading + DirectX::XM_PIDIV2) < 0.001f);
    CHECK(std::abs(std::sin(westwardHeading) + 1.0f) < 0.001f);
    CHECK(std::abs(std::cos(westwardHeading)) < 0.001f);

    DeferredReleaseQueue<int> releases;
    releases.Retire(4, 10);
    releases.Retire(8, 20);
    releases.Collect(3);
    CHECK(releases.PendingCount() == 2);
    releases.Collect(4);
    CHECK(releases.PendingCount() == 1);
    releases.Collect(8);
    CHECK(releases.PendingCount() == 0);

    RuntimeWorld combatWorld;
    LevelEntity destructible;
    destructible.id = 500;
    destructible.type = LevelEntityType::Prefab;
    destructible.prefabId = "test/destructible";
    combatWorld.Level().entities.push_back(destructible);
    combatWorld.Prefabs().destructibles.push_back(
        { 500, { 1.0f, 0.0f, 0.0f }, 100.0f });
    auto damageResult = combat.DamagePrefab(
        combatWorld, 500, 40.0f, { 1.0f, 0.0f, 0.0f });
    CHECK(damageResult.applied);
    CHECK(!damageResult.destroyed);
    damageResult = combat.DamagePrefab(
        combatWorld, 500, 60.0f, { 1.0f, 0.0f, 0.0f });
    CHECK(damageResult.destroyed);
    CHECK(!combatWorld.Level().entities.back().enabled);

    return failures ? 1 : 0;
}
