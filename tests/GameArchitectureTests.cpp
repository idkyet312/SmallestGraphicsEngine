#include "DeferredReleaseQueue.h"
#include "AnimationRuntime.h"
#include "AnimationClipUtils.h"
#include "FixedStepClock.h"
#include "GameCommandQueue.h"
#include "GameRuntime.h"
#include "GameSession.h"
#include "MissionSystem.h"
#include "RankSystem.h"
#include "CombatSystem.h"
#include "VehicleSystem.h"
#include "HumveeCrew.h"
#include "BoatWakeEmitter.h"
#include "DeploymentPlanner.h"
#include "LevelLoadingController.h"
#include "LevelRuntimeBuilder.h"
#include "PlayerState.h"
#include "WeaponCustomization.h"
#include "TimeOfDay.h"
#include "Weather.h"
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

    // Loading deferred stock must enter at Weapons and finish through the
    // existing upload stages, without replaying the level/world setup.
    loading.Begin({ 4, "Armory stock", "firearms", LevelLoadStage::Weapons }, loadStart);
    CHECK(loading.Active());
    CHECK(loading.Stage() == LevelLoadStage::Weapons);
    CHECK(loading.TaskCount() == 4);
    CHECK(loading.SubmittedUploads() == 0);
    loading.Advance(LevelLoadStage::GPUFinalize, "Finalize", "textures", true,
        loadStart + std::chrono::milliseconds(4));
    loading.Advance(LevelLoadStage::SubmitUploads, "Submit", "copies", true,
        loadStart + std::chrono::milliseconds(5));
    loading.Advance(LevelLoadStage::ReleaseUploads, "Release", "staging", true,
        loadStart + std::chrono::milliseconds(6));
    CHECK(loading.TaskIndex() == loading.TaskCount());
    loading.Complete(true, loadStart + std::chrono::milliseconds(7));
    CHECK(!loading.Active());
    CHECK(loading.Stage() == LevelLoadStage::Complete);
    CHECK(loading.Records().size() == 4);

    GameRuntime runtime;
    runtime.combat.heldBarrelIndex = 9;
    runtime.vehicles.drivingHumvee = true;
    runtime.commands.Request(GameCommand::ResetDDGIHistory);
    runtime.ResetLevelState();
    CHECK(runtime.combat.heldBarrelIndex == SIZE_MAX);
    CHECK(!runtime.vehicles.drivingHumvee);
    CHECK(!runtime.commands.Pending(GameCommand::ResetDDGIHistory));
    CHECK(runtime.mission.Loadout().Valid());
    runtime.mission.Loadout().SelectWeapon(0, 4);
    runtime.mission.Loadout().SelectWeapon(1, 7);
    runtime.mission.RecordWeaponFired(4, 3);
    runtime.ResetLevelState();
    CHECK(runtime.mission.Loadout().weapons[0] == 4);
    CHECK(runtime.mission.Loadout().weapons[1] == 7);
    CHECK(runtime.mission.Stats().shotsFired == 0);

    // Career state survives a level reset. Money and rank are the two things a
    // replayed level must not cost the player -- only their per-run counters
    // are cleared, which is what keeps the extraction screen honest without
    // rolling the career back with it.
    runtime.money.Award(MoneyEvent::EnemyKilled);
    runtime.rank.Award(XpEvent::EnemyKilled);
    const int64_t bankedMoney = runtime.money.Balance();
    const int64_t bankedXp = runtime.rank.TotalXp();
    runtime.ResetLevelState();
    CHECK(runtime.money.Balance() == bankedMoney);
    CHECK(runtime.money.SessionEarned() == 0);
    CHECK(runtime.rank.TotalXp() == bankedXp);
    CHECK(runtime.rank.LifetimeKills() == 1);
    CHECK(runtime.rank.SessionXp() == 0);

    // A fresh loadout carries only the issued charge in both slots, so picking
    // the same weapon for the other slot swaps the charge back into slot 0.
    MissionLoadout loadout;
    CHECK(loadout.Empty());
    CHECK(loadout.Valid());
    loadout.SelectWeapon(0, 2);
    CHECK(loadout.weapons[0] == 2);
    CHECK(loadout.weapons[1] == MissionLoadout::kIssuedChargeWeapon);
    loadout.SelectWeapon(1, 2);
    CHECK(loadout.weapons[0] == MissionLoadout::kIssuedChargeWeapon);
    CHECK(loadout.weapons[1] == 2);
    // The grading below expects a shotgun + RPG kit.
    loadout.SelectWeapon(0, 1);
    CHECK(loadout.weapons[0] == 1);
    CHECK(loadout.weapons[1] == 2);
    CHECK(loadout.Valid());
    loadout.grenade = GrenadeType::Vortex;
    loadout.insertion = LevelInsertionMode::Boat;

    // The suppressed SVD is a real loadout pick, not a hidden variant: it has
    // to be selectable and gradeable like any other weapon. Slot 8 sits at the
    // top of the range, so this also pins kWeaponCount against a table that was
    // extended in one place but not another.
    constexpr int kSuppressedSVD = 8;
    CHECK(kSuppressedSVD < MissionLoadout::kWeaponCount);
    MissionLoadout stealth;
    stealth.SelectWeapon(0, kSuppressedSVD);
    CHECK(stealth.weapons[0] == kSuppressedSVD);
    CHECK(stealth.Valid());
    CHECK(stealth.ContainsWeapon(kSuppressedSVD));
    // Ammo tables must cover it, or firing it reads uninitialised memory.
    CHECK(PlayerState::kWeaponSlots > kSuppressedSVD);
    PlayerState stealthAmmo;
    CHECK(stealthAmmo.MagazineSize(kSuppressedSVD) > 0);
    CHECK(stealthAmmo.MaxReserve(kSuppressedSVD) > 0);
    CHECK(stealthAmmo.ReloadTime(kSuppressedSVD) > 0.0f);
    // Quiet is the advantage, so it must not also carry more than the loud
    // rifle it shares a round with.
    CHECK(stealthAmmo.MagazineSize(kSuppressedSVD) <=
          stealthAmmo.MagazineSize(3));
    CHECK(stealthAmmo.MaxReserve(kSuppressedSVD) <
          stealthAmmo.MaxReserve(3));

    // ---- Weapon customisation ----------------------------------------------
    SGE::WeaponCustomizationSystem customisation;
    CHECK(customisation.FindWeapon("ak47") != nullptr);
    CHECK(customisation.FindWeapon("ak47")->legacyWeaponId == 0);
    CHECK(customisation.FindAttachment("silencer") != nullptr);
    CHECK(customisation.FindAttachment("red_dot") != nullptr);
    CHECK(customisation.FindAttachment("laser") != nullptr);
    CHECK(customisation.AttachmentInstalled(1, "red_dot"));
    CHECK(!customisation.AttachmentInstalled(1, "laser"));
    const SGE::WeaponInstance defaultShotgun = customisation.CreateInstance(1);
    CHECK(customisation.Resolve(defaultShotgun).redDotSight);
    CHECK(!customisation.Resolve(defaultShotgun).laserSight);
    // The RPG has no attachment mounts; compatibility is enforced in the data
    // layer rather than relying on the deployment UI to hide an invalid choice.
    CHECK(!customisation.EquipAttachment(2, "silencer"));
    CHECK(customisation.EquipAttachment(0, "laser"));
    CHECK(customisation.EquipAttachment(0, "red_dot"));
    CHECK(customisation.EquipAttachment(0, "silencer"));
    const SGE::ResolvedWeaponStats customisedAK = customisation.Resolve(0);
    CHECK(customisedAK.suppressed);
    CHECK(customisedAK.redDotSight);
    CHECK(customisedAK.laserSight);
    CHECK(customisedAK.noiseRadiusMultiplier < 1.0f);
    // The suppressor cuts recoil below the AK's own unmodified baseline.
    // Compared against that baseline rather than a literal, so retuning the
    // weapon or kGlobalRecoilScale cannot silently invalidate this check.
    CHECK(customisedAK.recoilPitchDegrees <
          customisation.Resolve(customisation.CreateInstance(0))
              .recoilPitchDegrees);
    CHECK(customisedAK.adsSpreadMultiplier < 1.0f);
    CHECK(customisedAK.hipSpreadMultiplier < 1.0f);
    CHECK(customisedAK.adsFovDegrees < 42.0f);

    // Equipping in another order resolves identically because modifiers are
    // sorted by their authored order (and stable ID as the tie-breaker).
    SGE::WeaponCustomizationSystem reversedCustomisation;
    CHECK(reversedCustomisation.EquipAttachment(0, "silencer"));
    CHECK(reversedCustomisation.EquipAttachment(0, "red_dot"));
    CHECK(reversedCustomisation.EquipAttachment(0, "laser"));
    const SGE::ResolvedWeaponStats reversedAK =
        reversedCustomisation.Resolve(0);
    CHECK(std::abs(customisedAK.recoilPitchDegrees -
                   reversedAK.recoilPitchDegrees) < 0.0001f);
    CHECK(std::abs(customisedAK.hipSpreadMultiplier -
                   reversedAK.hipSpreadMultiplier) < 0.0001f);

    SGE::WeaponInstance worldRifle =
        customisation.CreateInstance(0, 11, 37);
    CHECK(worldRifle.weaponId == "ak47");
    CHECK(worldRifle.magazine == 11);
    CHECK(worldRifle.reserve == 37);
    // Weapon-used tracking is a bitmask; slot 8 has to fit in it.
    CHECK((1u << kSuppressedSVD) != 0u);

    MissionRunStats missionStats;
    missionStats.weaponsUsedMask = (1u << 1) | (1u << 2);
    missionStats.shotsFired = 10;
    missionStats.shotsHit = 5;
    missionStats.friendliesDeployed = 4;
    missionStats.grenadesThrown = 1;
    missionStats.destructionEvents = MissionSystem::kDestructionScoreTarget;
    const MissionReport missionReport = MissionSystem::Grade(
        loadout, missionStats, 200.0f, 3);
    CHECK(std::abs(missionReport.accuracyPercent - 50.0f) < 0.001f);
    CHECK(missionReport.casualties == 1);
    CHECK(missionReport.optionalObjectivesCompleted == 3);
    CHECK(missionReport.timeScore == 15);
    CHECK(missionReport.accuracyScore == 10);
    CHECK(missionReport.casualtyScore == 11);
    CHECK(missionReport.optionalScore == 15);
    CHECK(missionReport.destructionScore == 10);
    // No tower authored on this run, so the primary objective pays in full --
    // otherwise a map without one could never grade above a C.
    CHECK(!missionReport.primaryObjectivePresent);
    CHECK(missionReport.primaryObjectiveComplete);
    CHECK(missionReport.primaryScore == MissionSystem::kPrimaryObjectiveScore);
    CHECK(missionReport.totalScore == 86);
    CHECK(missionReport.rank == MissionRank::A);

    // Comm-tower objective: the mission is the tower. A run that leaves it
    // standing forfeits the primary score and is not a complete mission, and a
    // partial clear on a two-tower map takes half credit.
    MissionRunStats towerStats = missionStats;
    towerStats.commTowersTotal = 1;
    const MissionReport towerMissed = MissionSystem::Grade(
        loadout, towerStats, 200.0f, 3);
    CHECK(towerMissed.primaryObjectivePresent);
    CHECK(!towerMissed.primaryObjectiveComplete);
    CHECK(towerMissed.primaryScore == 0);
    CHECK(towerMissed.totalScore == 61);

    towerStats.commTowersDestroyed = 1;
    const MissionReport towerFelled = MissionSystem::Grade(
        loadout, towerStats, 200.0f, 3);
    CHECK(towerFelled.primaryObjectiveComplete);
    CHECK(towerFelled.primaryScore == MissionSystem::kPrimaryObjectiveScore);
    CHECK(towerFelled.totalScore == 86);

    MissionRunStats twoTowers = missionStats;
    twoTowers.commTowersTotal = 2;
    twoTowers.commTowersDestroyed = 1;
    const MissionReport halfCleared = MissionSystem::Grade(
        loadout, twoTowers, 200.0f, 3);
    CHECK(!halfCleared.primaryObjectiveComplete);
    CHECK(halfCleared.primaryScore == 13);   // lround(25 * 1/2)

    // A stray extra report cannot credit a tower the level never authored.
    MissionSystem overCounted;
    overCounted.SetCommTowerCount(1);
    overCounted.RecordCommTowerDestroyed();
    overCounted.RecordCommTowerDestroyed();
    CHECK(overCounted.Stats().commTowersDestroyed == 1);
    CHECK(overCounted.CommTowerObjectiveComplete());

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
    CHECK(plan.humveeSpawns.size() == 1);
    CHECK(plan.helicopterSpawn.has_value());
    CHECK(!plan.explosiveBarrels.empty());
    CHECK(plan.patrolBoatEnabled);

    level.patrolBoatEnabled = false;
    plan = LevelRuntimeBuilder::Build(level);
    CHECK(!plan.patrolBoatEnabled);

    LevelEntity parkedHumvee = level.entities.back();
    parkedHumvee.id = 1000;
    parkedHumvee.type = LevelEntityType::Humvee;
    parkedHumvee.transform.position[0] = 24.0f;
    level.entities.push_back(parkedHumvee);
    plan = LevelRuntimeBuilder::Build(level);
    CHECK(plan.humveeSpawns.size() == 2);
    CHECK(plan.humveeSpawns[1].position[0] == 24.0f);

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
    {
        const DirectX::XMFLOAT3 minimum{-1.51464f, -0.83008f, -3.39820f};
        const DirectX::XMFLOAT3 maximum{1.51458f, 1.57925f, 3.37742f};
        const float scale = 9.0f / (maximum.z - minimum.z);
        const auto bounds = VehicleSystem::BoatCameraBounds::FromModel(
            minimum, maximum, scale, VehicleSystem::BoatFloatDepth);
        CHECK(bounds.FollowDistance() > bounds.radius + 0.75f);
        const float halfX = (maximum.x - minimum.x) * scale * 0.5f;
        const float halfY = (maximum.y - minimum.y) * scale * 0.5f;
        const float halfZ = (maximum.z - minimum.z) * scale * 0.5f;
        // The old deck camera sat inside this cabin. Every allowed orbit must
        // remain outside it, including broadside and steep downward views.
        for (float pitch : {-65.0f, -15.0f, -12.0f}) {
            for (int heading = 0; heading < 360; heading += 15) {
                const float yaw = DirectX::XMConvertToRadians(static_cast<float>(heading));
                const float p = DirectX::XMConvertToRadians(pitch);
                const float x = -std::cos(yaw) * std::cos(p) * bounds.FollowDistance();
                const float y = -std::sin(p) * bounds.FollowDistance();
                const float z = -std::sin(yaw) * std::cos(p) * bounds.FollowDistance();
                CHECK(std::abs(x) > halfX || std::abs(y) > halfY || std::abs(z) > halfZ);
                CHECK(bounds.centerHeight + y > 0.75f);
            }
        }
    }
    {
        VehicleSystem boat;
        boat.boatPosition = {10.0f, 0.0f, 20.0f};
        CHECK(boat.PlayerCanDriveBoat({10.0f, 1.8f, 20.0f}));
        CHECK(boat.PlayerCanDriveBoat({13.5f, 1.8f, 20.0f}));
        CHECK(!boat.PlayerCanDriveBoat({15.0f, 1.8f, 20.0f}));
        CHECK(!boat.PlayerCanDriveBoat({10.0f, 20.0f, 20.0f}));
        boat.boatYaw = DirectX::XM_PIDIV2;
        CHECK(boat.PlayerCanDriveBoat({16.0f, 1.8f, 20.0f}));
        CHECK(!boat.PlayerCanDriveBoat({10.0f, 1.8f, 25.0f}));
        boat.boatYaw = 0.0f;
        boat.SetBoatInput(1.0f, 1.0f, false);
        boat.StepDrivenBoat(0.25f);
        CHECK(boat.boatPosition.x == 10.0f && boat.boatPosition.z == 20.0f);
        boat.drivingBoat = boat.boatCaptured = true;
        boat.SetBoatInput(1.0f, 0.0f, false);
        for (int i = 0; i < 600; ++i) boat.StepDrivenBoat(1.0f / 60.0f);
        CHECK(boat.boatPosition.z > 100.0f);
        CHECK(std::abs(boat.boatPosition.x - 10.0f) < 0.001f);
        CHECK(std::abs(boat.boatSpeed - VehicleSystem::BoatForwardSpeed) < 0.001f);
        const float yaw = boat.boatYaw;
        boat.SetBoatInput(1.0f, -1.0f, false);
        boat.StepDrivenBoat(0.25f);
        CHECK(boat.boatYaw < yaw); // A steers left when moving forward.
        boat.SetBoatInput(0.0f, 0.0f, true);
        for (int i = 0; i < 120; ++i) boat.StepDrivenBoat(1.0f / 60.0f);
        CHECK(std::abs(boat.boatSpeed) < 0.001f);
        boat.boatYaw = 0.0f;
        const float reverseStart = boat.boatPosition.z;
        boat.SetBoatInput(-1.0f, 0.0f, false);
        for (int i = 0; i < 120; ++i) boat.StepDrivenBoat(1.0f / 60.0f);
        CHECK(boat.boatPosition.z < reverseStart);
        CHECK(std::abs(boat.boatSpeed + VehicleSystem::BoatReverseSpeed) < 0.001f);
        boat.SetBoatInput(-1.0f, -1.0f, false);
        boat.StepDrivenBoat(0.25f);
        CHECK(boat.boatYaw > 0.0f); // Rudder reverses when backing up.
        boat.StopDrivingBoat();
        const auto parked = boat.boatPosition;
        boat.StepDrivenBoat(0.25f);
        CHECK(boat.boatCaptured && !boat.drivingBoat);
        CHECK(boat.boatPosition.x == parked.x && boat.boatPosition.z == parked.z);
        CHECK(boat.boatSpeed == 0.0f);
        boat.boatDead = true;
        CHECK(!boat.PlayerCanDriveBoat(boat.boatPosition));
        boat.ResetLevel();
        CHECK(!boat.boatCaptured && !boat.drivingBoat && !boat.boatDead);
        CHECK(boat.boatThrottle == 0.0f && boat.boatSteering == 0.0f && boat.boatBrake);

        VehicleSystem slowFrame, fastFrame;
        slowFrame.drivingBoat = fastFrame.drivingBoat = true;
        slowFrame.SetBoatInput(1.0f, 0.5f, false);
        fastFrame.SetBoatInput(1.0f, 0.5f, false);
        for (int i = 0; i < 30; ++i) slowFrame.StepDrivenBoat(1.0f / 30.0f);
        for (int i = 0; i < 120; ++i) fastFrame.StepDrivenBoat(1.0f / 120.0f);
        CHECK(std::abs(slowFrame.boatPosition.x - fastFrame.boatPosition.x) < 0.03f);
        CHECK(std::abs(slowFrame.boatPosition.z - fastFrame.boatPosition.z) < 0.03f);
        CHECK(std::abs(slowFrame.boatYaw - fastFrame.boatYaw) < 0.01f);
        const auto beforePause = fastFrame.boatPosition;
        fastFrame.StepDrivenBoat(0.0f);
        CHECK(fastFrame.boatPosition.x == beforePause.x &&
              fastFrame.boatPosition.z == beforePause.z);
    }
    {
        VehicleSystem insertion;
        insertion.DisableInsertionBoat();
        CHECK(!insertion.TakeInsertionBoatHelm({}));
        insertion.BeginInsertionBoatRun({20.0f, 0.0f, 30.0f}, 0.0f, 0.0f);
        insertion.UpdateInsertionBoat(0.1f);
        const auto start = insertion.insertionBoatPosition;
        const auto patrol = insertion.boatPosition;
        CHECK(insertion.TakeInsertionBoatHelm(insertion.InsertionBoatRidePosition()));
        CHECK(insertion.drivingInsertionBoat && !insertion.drivingBoat);
        CHECK(insertion.insertionBoatCaptured && insertion.insertionBoatCarryingPlayer);
        CHECK(!insertion.BailOutOfInsertionBoat());
        insertion.SetBoatInput(1.0f, 0.0f, false);
        for (int i = 0; i < 120; ++i) {
            insertion.StepDrivenBoat(1.0f / 60.0f);
            insertion.UpdateInsertionBoat(1.0f / 60.0f);
        }
        CHECK(insertion.insertionBoatPosition.z > start.z + 8.0f);
        CHECK(insertion.boatPosition.x == patrol.x && insertion.boatPosition.z == patrol.z);
        CHECK(insertion.boatSpeed > 9.0f); // A parked patrol must not brake the other helm.
        CHECK(!insertion.insertionBoatDroppedPlayer && insertion.insertionBoatVisible);
        insertion.StopDrivingBoat();
        const auto parked = insertion.insertionBoatPosition;
        insertion.UpdateInsertionBoat(60.0f);
        CHECK(insertion.insertionBoatPosition.x == parked.x &&
              insertion.insertionBoatPosition.z == parked.z);
        CHECK(insertion.insertionBoatVisible && !insertion.insertionBoatCarryingPlayer);
        CHECK(!insertion.PlayerCanDriveInsertionBoat({1000.0f, 0.0f, 1000.0f}));
        insertion.insertionBoatPhysics.handle = 42;
        insertion.insertionBoatPhysics.hasPose = true;
        insertion.insertionBoatPhysics.rotation = {0,0,0,1};
        const auto physicalPosition = insertion.insertionBoatPosition;
        insertion.UpdateInsertionBoat(0.25f);
        CHECK(insertion.insertionBoatPosition.x == physicalPosition.x &&
              insertion.insertionBoatPosition.y == physicalPosition.y &&
              insertion.insertionBoatPosition.z == physicalPosition.z);
        insertion.insertionBoatPhysics.rotation = {0,0,0.7071068f,0.7071068f};
        CHECK(!insertion.InsertionBoatSupportsPassenger(parked,1.8f));
        insertion.insertionBoatPhysics = {};
        CHECK(insertion.TakeInsertionBoatHelm(parked));
        insertion.SetBoatInput(-1.0f, 0.0f, false);
        for (int i = 0; i < 120; ++i) insertion.UpdateInsertionBoat(1.0f / 60.0f);
        CHECK(insertion.insertionBoatPosition.z < parked.z);
        CHECK(std::abs(insertion.boatSpeed + VehicleSystem::BoatReverseSpeed) < 0.001f);
        auto damage = insertion.DamageInsertionBoatFromEnemyFire(
            VehicleSystem::InsertionBoatMaxHealth);
        CHECK(damage.destroyed && insertion.InsertionBoatIsFoundering());
        CHECK(!insertion.PlayerCanDriveInsertionBoat(insertion.insertionBoatPosition));
        CHECK(!insertion.insertionBoatManualSquadPending);
        const auto wreck = insertion.insertionBoatPosition;
        insertion.UpdateInsertionBoat(0.25f);
        CHECK(insertion.insertionBoatPosition.x == wreck.x &&
              insertion.insertionBoatPosition.z == wreck.z);
        CHECK(insertion.insertionBoatSinkOffset > 0.0f);
        insertion.ResetLevel();
        CHECK(!insertion.drivingInsertionBoat && !insertion.insertionBoatCaptured);
        CHECK(insertion.insertionBoatCarryingPlayer);
        CHECK(insertion.insertionBoatPhase == VehicleSystem::InsertionBoatPhase::Inbound);
        CHECK(insertion.TakeInsertionBoatHelm(insertion.InsertionBoatRidePosition()));
        insertion.DisableInsertionBoat();
        CHECK(!insertion.drivingInsertionBoat && !insertion.insertionBoatCaptured);
        CHECK(!insertion.insertionBoatVisible && !insertion.insertionBoatManualSquadPending);
    }
    {
        VehicleSystem insertion;
        insertion.BeginInsertionBoatRun({0.0f, 0.0f, 180.0f}, 0.0f, 0.0f);
        CHECK(insertion.insertionBoatPassengerPlacementPending);
        constexpr float height = 1.8f;
        const DirectX::XMFLOAT3 stern{0.7f, height + insertion.insertionBoatDeckOffset, -3.0f};
        CHECK(insertion.InsertionBoatSupportsPassenger(stern, height));
        CHECK(!insertion.InsertionBoatSupportsPassenger({2.0f, stern.y, stern.z}, height));
        CHECK(!insertion.InsertionBoatSupportsPassenger({stern.x, stern.y, 5.0f}, height));
        CHECK(!insertion.InsertionBoatSupportsPassenger({stern.x, stern.y - 1.0f, stern.z}, height, true));
        const DirectX::XMFLOAT3 jumping{stern.x, stern.y + 1.3f, stern.z};
        CHECK(!insertion.InsertionBoatSupportsPassenger(jumping, height));
        CHECK(insertion.InsertionBoatSupportsPassenger(jumping, height, true));

        // Translation, turning and bobbing preserve the passenger's chosen
        // local walking position and jump height instead of returning to a seat.
        const auto oldPosition = insertion.insertionBoatPosition;
        const float oldDeck = oldPosition.y + insertion.insertionBoatDeckOffset;
        insertion.insertionBoatPosition = {10.0f, 0.2f, 20.0f};
        insertion.insertionBoatYaw = DirectX::XM_PIDIV2;
        const auto carried = VehicleSystem::CarryBoatPosition(jumping,
            oldPosition, 0.0f, oldDeck, insertion.insertionBoatPosition,
            insertion.insertionBoatYaw, 0.2f + insertion.insertionBoatDeckOffset);
        CHECK(std::abs(carried.x - 7.0f) < 0.001f);
        CHECK(std::abs(carried.z - 19.3f) < 0.001f);
        CHECK(std::abs(carried.y - (jumping.y + 0.2f)) < 0.001f);
        CHECK(insertion.InsertionBoatSupportsPassenger(carried, height, true));
        CHECK(insertion.BailOutOfInsertionBoat());
        CHECK(!insertion.insertionBoatPassengerPlacementPending);
        CHECK(!insertion.insertionBoatCarryingPlayer);
        insertion.ResetLevel();
        CHECK(insertion.insertionBoatPassengerPlacementPending);
        insertion.DisableInsertionBoat();
        CHECK(!insertion.insertionBoatPassengerPlacementPending);
        CHECK(!insertion.InsertionBoatSupportsPassenger(carried, height, true));
    }
    {
        auto traceWake = [](int fps) {
            BoatWakeEmitter emitter;
            std::vector<BoatWakeSample> samples;
            const float dt = 1.0f / fps;
            for (int i = 0; i < fps * 2; ++i)
                emitter.Step({0.0f, 0.0f, i * 12.0f * dt},
                             {0.0f, 0.0f, (i + 1) * 12.0f * dt}, dt, true,
                    [&](const BoatWakeSample& sample) { samples.push_back(sample); });
            return samples;
        };
        const auto slow = traceWake(30), fast = traceWake(120);
        CHECK(slow.size() == fast.size() && slow.size() > 20);
        for (size_t i = 0; i < (std::min)(slow.size(), fast.size()); ++i) {
            CHECK(std::abs(slow[i].position.z - fast[i].position.z) < 0.001f);
            CHECK(slow[i].wave == fast[i].wave);
        }
        BoatWakeEmitter emitter;
        std::vector<BoatWakeSample> samples;
        auto collect = [&](const BoatWakeSample& sample) { samples.push_back(sample); };
        emitter.Step({}, {}, 0.1f, true, collect);
        emitter.Step({}, {1000.0f, 0.0f, 0.0f}, 0.1f, true, collect);
        emitter.Step({}, {0.0f, 0.0f, 5.0f}, 0.0f, true, collect);
        emitter.Step({}, {0.0f, 0.0f, 5.0f}, 0.1f, false, collect);
        CHECK(samples.empty());
        emitter.Step({}, {0.0f, 0.0f, -2.0f}, 0.2f, true, collect);
        CHECK(!samples.empty() && samples.front().direction.y == -1.0f);
        CHECK(samples.front().position.z < 0.0f);
        const float reverseStrength = samples.front().strength;
        samples.clear(); emitter.Reset();
        emitter.Step({}, {2.0f, 0.0f, 0.0f}, 0.1f, true, collect);
        CHECK(!samples.empty() && samples.front().direction.x == 1.0f);
        CHECK(samples.front().strength > reverseStrength);
        samples.clear(); emitter.Reset();
        emitter.Step({}, {20.0f, 0.0f, 0.0f}, 1.0f, true, collect);
        CHECK(samples.size() == 4);
    }
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
    // Entering the rappel asks the owner to hang a rope. The normal route,
    // which lands on its skids, must never ask for one.
    CHECK(fastInsertion.blackHawkRopeSpawnRequested);
    CHECK(!normalInsertion.blackHawkRopeSpawnRequested);
    fastInsertion.blackHawkRopeSpawnRequested = false;
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
    // A completed descent releases the rope rather than leaking the world.
    CHECK(fastInsertion.blackHawkRopeReleaseRequested);

    // The oversized walkable airframe shrinks to exterior size only once it is
    // leaving empty, and only once it is well clear of the drop-off.
    fastInsertion.blackHawkModelScale = 2.0f;
    fastInsertion.blackHawkExteriorScale = 0.5f;
    CHECK(std::abs(fastInsertion.BlackHawkDrawScale() - 2.0f) < 0.001f);
    fastInsertion.UpdateBlackHawk(0.05f);
    CHECK(std::abs(fastInsertion.BlackHawkDrawScale() - 2.0f) < 0.001f);
    for (int stepIndex = 0; stepIndex < 2000; ++stepIndex) {
        const float dx = fastInsertion.blackHawkPosition.x;
        const float dz = fastInsertion.blackHawkPosition.z;
        if (std::sqrt(dx * dx + dz * dz) >
            VehicleSystem::BlackHawkShrinkEndDistance) break;
        const float before = fastInsertion.BlackHawkDrawScale();
        fastInsertion.UpdateBlackHawk(0.05f);
        CHECK(fastInsertion.BlackHawkDrawScale() <= before + 0.0001f);
    }
    CHECK(std::abs(fastInsertion.BlackHawkDrawScale() - 1.0f) < 0.001f);

    // Shot down just after drop-off: shrinks on a clock while falling, then
    // freezes once down (the wreck's collision is baked from that scale).
    VehicleSystem emptyCrash;
    emptyCrash.BeginBlackHawkInsertion({ 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, true);
    emptyCrash.blackHawkModelScale = 2.0f;
    emptyCrash.blackHawkExteriorScale = 0.5f;
    emptyCrash.blackHawkPhase = VehicleSystem::BlackHawkPhase::Departing;
    emptyCrash.blackHawkCarryingPlayer = false;
    emptyCrash.blackHawkPosition = { 0.0f, 400.0f, 0.0f };
    emptyCrash.blackHawkCrashGroundY = 0.0f;
    emptyCrash.BeginBlackHawkCrash();
    emptyCrash.UpdateBlackHawk(VehicleSystem::BlackHawkCrashShrinkTime * 0.5f);
    CHECK(std::abs(emptyCrash.BlackHawkDrawScale() - 1.5f) < 0.001f);
    emptyCrash.UpdateBlackHawk(VehicleSystem::BlackHawkCrashShrinkTime);
    CHECK(std::abs(emptyCrash.BlackHawkDrawScale() - 1.0f) < 0.001f);

    // A crash with the player still aboard keeps the walkable cabin size while
    // it falls, then lands as an exterior-size wreck.
    VehicleSystem crewedCrash;
    crewedCrash.BeginBlackHawkInsertion({ 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, false);
    crewedCrash.blackHawkModelScale = 2.0f;
    crewedCrash.blackHawkExteriorScale = 0.5f;
    CHECK(crewedCrash.blackHawkCarryingPlayer);
    crewedCrash.BeginBlackHawkCrash();
    crewedCrash.UpdateBlackHawk(0.05f);
    CHECK(std::abs(crewedCrash.BlackHawkDrawScale() - 2.0f) < 0.001f);
    for (int stepIndex = 0; stepIndex < 400 &&
         crewedCrash.blackHawkPhase != VehicleSystem::BlackHawkPhase::Down;
         ++stepIndex)
        crewedCrash.UpdateBlackHawk(0.05f);
    CHECK(crewedCrash.blackHawkPhase == VehicleSystem::BlackHawkPhase::Down);
    CHECK(std::abs(crewedCrash.BlackHawkDrawScale() - 1.0f) < 0.001f);

    // Rope cut mid-descent. NotifyBlackHawkRopeCut latches the progress the cut
    // happened at (which the fall damage is scaled from), refuses to fire twice,
    // and refuses to fire at all when no descent is in progress.
    VehicleSystem cutRun;
    cutRun.BeginBlackHawkInsertion({ 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, true);
    CHECK(!cutRun.NotifyBlackHawkRopeCut());   // still inbound, nothing to cut
    for (int stepIndex = 0;
         stepIndex < 1000 &&
             cutRun.blackHawkPhase == VehicleSystem::BlackHawkPhase::Inbound;
         ++stepIndex)
        cutRun.UpdateBlackHawk(0.05f);
    CHECK(cutRun.BlackHawkIsRappelling());
    cutRun.UpdateBlackHawk(VehicleSystem::BlackHawkRappelTime * 0.25f);
    CHECK(cutRun.NotifyBlackHawkRopeCut());
    CHECK(cutRun.blackHawkRopeCut);
    CHECK(std::abs(cutRun.blackHawkRopeCutProgress - 0.25f) < 0.001f);
    CHECK(!cutRun.NotifyBlackHawkRopeCut());   // one cut only
    // The cut releases the player and stops the rope driving the descent, so
    // progress must not keep advancing toward a rope that is no longer there.
    const float progressAtCut = cutRun.blackHawkRappelProgress;
    cutRun.UpdateBlackHawk(VehicleSystem::BlackHawkRappelTime * 0.5f);
    CHECK(std::abs(cutRun.blackHawkRappelProgress - progressAtCut) < 0.001f);
    CHECK(!cutRun.blackHawkCarryingPlayer);
    CHECK(cutRun.blackHawkPhase == VehicleSystem::BlackHawkPhase::Departing);
    // Standing the run down must always ask for a teardown.
    cutRun.DisableBlackHawkInsertion();
    CHECK(cutRun.blackHawkRopeReleaseRequested);
    CHECK(!cutRun.blackHawkRopeCut);

    // Taking the stick mid-approach: the passenger becomes the pilot, the
    // squad stays aboard, and the flight model owns the pose from then on.
    VehicleSystem piloted;
    piloted.BeginBlackHawkInsertion({ 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, false);
    piloted.UpdateBlackHawk(0.5f);
    CHECK(piloted.TakeBlackHawkControls());
    CHECK(!piloted.TakeBlackHawkControls());   // already flown
    CHECK(piloted.blackHawkPhase == VehicleSystem::BlackHawkPhase::PlayerFlown);
    CHECK(!piloted.blackHawkCarryingPlayer);
    CHECK(piloted.blackHawkSquadAboard);
    const float pilotStartY = piloted.blackHawkPosition.y;
    piloted.blackHawkFlightGroundY = 0.0f;
    piloted.blackHawkPilotInput.lift = 1.0f;
    for (int stepIndex = 0; stepIndex < 60; ++stepIndex)
        piloted.UpdateBlackHawk(1.0f / 60.0f);
    CHECK(piloted.blackHawkPosition.y > pilotStartY + 2.0f);
    CHECK(piloted.BlackHawkIsFlying());
    // Forward stick noses the airframe DOWN. It draws nose-on-+Z, where a
    // positive X rotation tips +Z toward -Y, so the drawn pitch is positive.
    piloted.blackHawkPilotInput = {};
    piloted.blackHawkPilotInput.forward = 1.0f;
    for (int stepIndex = 0; stepIndex < 60; ++stepIndex)
        piloted.UpdateBlackHawk(1.0f / 60.0f);
    CHECK(piloted.blackHawkPitch > 0.1f);
    {
        const DirectX::XMVECTOR nose = DirectX::XMVector3TransformNormal(
            DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
            DirectX::XMMatrixRotationRollPitchYaw(piloted.blackHawkPitch,
                piloted.blackHawkYaw, piloted.blackHawkRoll));
        CHECK(DirectX::XMVectorGetY(nose) < -0.1f);
    }
    piloted.blackHawkPilotInput = {};
    // Climbing out leaves it to settle empty, still in the player's hands.
    piloted.ReleaseBlackHawkControls();
    for (int stepIndex = 0; stepIndex < 60 * 30; ++stepIndex)
        piloted.UpdateBlackHawk(1.0f / 60.0f);
    CHECK(piloted.blackHawkLanded);
    CHECK(std::abs(piloted.blackHawkPosition.y) < 0.001f);
    CHECK(piloted.blackHawkPhase == VehicleSystem::BlackHawkPhase::PlayerFlown);
    CHECK(piloted.TakeBlackHawkControls());    // parked: flyable again
    // Shot down at the stick: the pilot is thrown clear rather than strapped in.
    piloted.BeginBlackHawkCrash();
    CHECK(!piloted.blackHawkPiloted);
    CHECK(piloted.blackHawkBailedOut);
    CHECK(!piloted.blackHawkCarryingPlayer);
    CHECK(!piloted.TakeBlackHawkControls());
    // The rope is the one place the stick is out of reach.
    VehicleSystem roped;
    roped.BeginBlackHawkInsertion({ 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, true);
    for (int stepIndex = 0; stepIndex < 1000 &&
         roped.blackHawkPhase == VehicleSystem::BlackHawkPhase::Inbound;
         ++stepIndex)
        roped.UpdateBlackHawk(0.05f);
    CHECK(roped.BlackHawkIsRappelling());
    CHECK(!roped.TakeBlackHawkControls());

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
    CHECK(player.SetAmmo(0, 1, 0));
    CHECK(player.ConsumeAmmo(0));
    CHECK(player.Magazine(0) == 0);
    CHECK(!player.ConsumeAmmo(0));
    CHECK(player.SetAmmo(0, 0, 5));
    CHECK(player.BeginReload(0));
    player.UpdateReload(10.0f);
    CHECK(player.Magazine(0) == 5);
    CHECK(player.Reserve(0) == 0);
    PlayerState demolitionKit;
    CHECK(demolitionKit.Magazine(5) == 1);
    CHECK(demolitionKit.Reserve(5) == 2);
    CHECK(demolitionKit.ReloadTime(5) == 5.0f);
    CHECK(!demolitionKit.BeginReload(5));
    for (int charge = 0; charge < 3; ++charge) {
        CHECK(demolitionKit.ConsumeAmmo(5));
        CHECK(!demolitionKit.ConsumeAmmo(5));
        if (charge == 2) break;
        CHECK(demolitionKit.BeginReload(5));
        CHECK(demolitionKit.reloadingSlot == 5);
        CHECK(demolitionKit.reloadTimer == 5.0f);
        CHECK(!demolitionKit.BeginReload(5));
        demolitionKit.UpdateReload(4.5f);
        CHECK(demolitionKit.Reloading());
        CHECK(!demolitionKit.ConsumeAmmo(5));
        CHECK(demolitionKit.Magazine(5) == 0);
        demolitionKit.UpdateReload(0.5f);
        CHECK(!demolitionKit.Reloading());
        CHECK(demolitionKit.Magazine(5) == 1);
        CHECK(demolitionKit.Reserve(5) == 1 - charge);
    }
    CHECK(!demolitionKit.ConsumeAmmo(5));
    CHECK(!demolitionKit.BeginReload(5));
    CHECK(demolitionKit.ConsumeGrenade());
    CHECK(demolitionKit.ConsumeGrenade());
    CHECK(!demolitionKit.ConsumeGrenade());
    demolitionKit.RestoreAmmo();
    CHECK(demolitionKit.Magazine(5) == 1);
    CHECK(demolitionKit.Reserve(5) == 2);
    CHECK(demolitionKit.grenades == 2);
    CHECK(demolitionKit.SetAmmo(5, 99, 99));
    CHECK(demolitionKit.Magazine(5) == 1);
    CHECK(demolitionKit.Reserve(5) == 2);
    CHECK(PlayerState::kWeaponSlots == 15);
    HumveeCrewSeat crew[4];
    uint8_t occupiedCrewSeats = 0;
    for (int passenger = 0; passenger < HumveeCrewSeat::Capacity; ++passenger) {
        CHECK(crew[passenger].TryBoard(2, occupiedCrewSeats));
        CHECK(crew[passenger].vehicle == 2);
        CHECK(crew[passenger].seat == passenger);
        occupiedCrewSeats |= static_cast<uint8_t>(1u << crew[passenger].seat);
    }
    CHECK(crew[0].Gunner());
    CHECK(!crew[1].Gunner());
    CHECK(!crew[3].TryBoard(2, occupiedCrewSeats));
    CHECK(!crew[0].TryBoard(3, 0));
    crew[0].Clear();
    occupiedCrewSeats &= ~1u;
    CHECK(crew[3].TryBoard(2, occupiedCrewSeats));
    CHECK(crew[3].Gunner());
    HumveeCrewSeat occupiedTurret;
    CHECK(occupiedTurret.TryBoard(4, 1u));
    CHECK(occupiedTurret.seat == 1);
    occupiedTurret.Clear();
    occupiedTurret.ApplyNetwork(4, 2);
    CHECK(occupiedTurret.Mounted() && occupiedTurret.vehicle == 4 && occupiedTurret.seat == 2);
    occupiedTurret.ApplyNetwork(4, HumveeCrewSeat::NoSeat);
    CHECK(!occupiedTurret.Mounted());
    occupiedTurret.ApplyNetwork(HumveeCrewSeat::NoSeat, 0);
    CHECK(!occupiedTurret.Mounted());
    CHECK(MissionLoadout::kWeaponCount == PlayerState::kWeaponSlots);
    const auto* designator = player.weapons.FindWeapon(14);
    CHECK(designator != nullptr);
    if (designator) {
        CHECK(designator->id == "target_designator");
        CHECK(designator->magazineCapacity == 1);
        CHECK(designator->semiAutomatic);
    }
    CHECK(player.SetAmmo(14, 1, 0));
    CHECK(player.ConsumeAmmo(14));
    CHECK(!player.ConsumeAmmo(14));
    CHECK(player.SetAmmo(4, 1, 0));
    CHECK(player.ConsumeAmmo(4));
    CHECK(player.Magazine(4) == 0);
    CHECK(player.SetAmmo(4, 0, 7));
    CHECK(player.BeginReload(4));
    player.UpdateReload(10.0f);
    CHECK(player.Magazine(4) == 7);
    CHECK(player.Reserve(4) == 0);
    CHECK(player.MagazineSize(7) == 1);

    PlayerState fastRappelLoadout;
    CHECK(fastRappelLoadout.SetAmmo(0, 5, 9));
    CHECK(fastRappelLoadout.SetAmmo(2, 1, 5));
    fastRappelLoadout.reloadTimer = 1.0f;
    fastRappelLoadout.reloadingSlot = 0;
    fastRappelLoadout.HalveAmmo();
    CHECK(fastRappelLoadout.Magazine(0) == 2);
    CHECK(fastRappelLoadout.Reserve(0) == 4);
    CHECK(fastRappelLoadout.Magazine(2) == 0);
    CHECK(fastRappelLoadout.Reserve(2) == 2);
    CHECK(!fastRappelLoadout.Reloading());
    CHECK(fastRappelLoadout.reloadingSlot == -1);
    CHECK(player.MaxReserve(7) == 24);

    // AA emplacement. The gun is only a threat if it leads a moving aircraft
    // and slews at a finite rate, so both are pinned here.
    VehicleSystem aa;
    CHECK(!aa.AnyAATurretActive());       // absent until a level places one
    CHECK(aa.PlaceAATurret({ 0.0f, 0.0f, 0.0f }) == 0);
    CHECK(aa.AnyAATurretActive());
    CHECK(aa.aaTurrets.size() == 1);
    CHECK(std::abs(aa.aaTurrets[0].HealthFraction() - 1.0f) < 0.001f);

    // Lead point sits ahead of a crossing target, along its travel.
    const DirectX::XMFLOAT3 crossing{ 0.0f, 40.0f, 90.0f };
    const DirectX::XMFLOAT3 crossingVelocity{ 25.0f, 0.0f, 0.0f };
    const DirectX::XMFLOAT3 lead =
        aa.AATurretLeadPoint(aa.aaTurrets[0], crossing, crossingVelocity, 240.0f);
    CHECK(lead.x > crossing.x);
    CHECK(std::abs(lead.z - crossing.z) < 0.001f);
    // A stationary target needs no lead at all.
    const DirectX::XMFLOAT3 still =
        aa.AATurretLeadPoint(aa.aaTurrets[0], crossing,
                             { 0.0f, 0.0f, 0.0f }, 240.0f);
    CHECK(std::abs(still.x - crossing.x) < 0.001f);

    // The mount cannot snap onto a target behind it: one short tick turns it by
    // at most AATurretYawRate * dt, which is what makes flying wide of the gun
    // a real option rather than a formality.
    aa.aaTurrets[0].yaw = 0.0f;
    aa.UpdateAATurret(aa.aaTurrets[0], 0.05f, { 0.0f, 20.0f, -80.0f }, true);
    CHECK(std::abs(aa.aaTurrets[0].yaw) <=
          VehicleSystem::AATurretYawRate * 0.05f + 0.0001f);
    // No target: the gun holds fire rather than emptying a burst into empty sky.
    CHECK(!aa.UpdateAATurret(aa.aaTurrets[0], 0.05f, {}, false));

    // Several emplacements coexist and take damage independently: killing one
    // must not disarm the rest, which is the whole point of the vector.
    CHECK(aa.PlaceAATurret({ 40.0f, 0.0f, 0.0f }) == 1);
    CHECK(aa.aaTurrets.size() == 2);
    aa.DamageAATurret(0, VehicleSystem::AATurretMaxHealth);
    CHECK(!aa.aaTurrets[0].Active());
    CHECK(aa.aaTurrets[1].Active());
    CHECK(aa.AnyAATurretActive());
    // A dead gun stops shooting even with a target dead ahead.
    CHECK(!aa.UpdateAATurret(aa.aaTurrets[0], 0.05f,
                             { 0.0f, 20.0f, 40.0f }, true));
    // The cap is enforced rather than growing without bound.
    while (aa.aaTurrets.size() < VehicleSystem::kMaxAATurrets)
        aa.PlaceAATurret({ 0.0f, 0.0f, 0.0f });
    CHECK(aa.PlaceAATurret({ 1.0f, 0.0f, 1.0f }) ==
          VehicleSystem::kMaxAATurrets);
    CHECK(aa.aaTurrets.size() == VehicleSystem::kMaxAATurrets);

    // Elevation is clamped to the gun's arc, so it cannot fold over backwards
    // tracking something directly overhead.
    for (int i = 0; i < 200; ++i)
        aa.UpdateAATurret(aa.aaTurrets[1], 0.05f, { 0.0f, 500.0f, 0.1f }, true);
    CHECK(aa.aaTurrets[1].pitch <= VehicleSystem::AATurretMaxPitch + 0.0001f);
    CHECK(aa.aaTurrets[1].pitch >= VehicleSystem::AATurretMinPitch - 0.0001f);

    // The ground dead zone has to clear the closest deployment zone the
    // perimeter ring can produce. On the shipping island that zone sits ~6.7 m
    // from the emplacement, so anything at or under that distance must fall
    // inside the dead zone or landing there means being shot on arrival.
    CHECK(VehicleSystem::AATurretGroundMinRange > 6.7f);
    CHECK(VehicleSystem::AATurretGroundMinRange <
          VehicleSystem::AATurretGroundRange);

    // The gun only engages the player once they are clear of the ground, and
    // the bar for that has to sit above a jump. A jump peaks at
    // JumpStrength^2 / (2 * Gravity) = 5.0^2 / 19.6 = 1.28 m with the camera's
    // shipping constants; if the threshold ever slips under that, hopping past
    // the emplacement would draw AA fire.
    constexpr float kJumpApex = (5.0f * 5.0f) / (2.0f * 9.8f);
    CHECK(VehicleSystem::AATurretMinTargetAltitude > kJumpApex);
    // And it must stay low enough that a rope or a rooftop still counts.
    CHECK(VehicleSystem::AATurretMinTargetAltitude < 10.0f);

    // Destroying it silences it: a dead gun never reports another shot.
    CHECK(!aa.DamageAATurret(
        1, VehicleSystem::AATurretMaxHealth * 0.5f).destroyed);
    CHECK(aa.DamageAATurret(1, VehicleSystem::AATurretMaxHealth).destroyed);
    CHECK(!aa.aaTurrets[1].Active());
    CHECK(!aa.UpdateAATurret(aa.aaTurrets[1], 0.05f,
                             { 0.0f, 40.0f, 40.0f }, true));
    // And a further hit on the wreck is not a second kill.
    CHECK(!aa.DamageAATurret(1, 100.0f).applied);
    // Damaging an index past the end is a no-op rather than a crash.
    CHECK(!aa.DamageAATurret(aa.aaTurrets.size(), 100.0f).applied);

    // ResetLevel clears every emplacement, so the next map inherits none.
    aa.PlaceAATurret({ 5.0f, 1.0f, 5.0f });
    aa.ResetLevel();
    CHECK(aa.aaTurrets.empty());
    CHECK(!aa.AnyAATurretActive());

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
    const DeploymentPlanner::CameraFrame defaultDeploymentFrame =
        DeploymentPlanner::BuildCameraFrame(43.0f, 34.0f);
    CHECK(std::abs(defaultDeploymentFrame.orbitRadius - 95.0f) < 0.001f);
    CHECK(std::abs(defaultDeploymentFrame.height - 62.0f) < 0.001f);
    CHECK(defaultDeploymentFrame.terrainViewRadius > 250.0f);
    const auto farthestOceanCorner = [](const DeploymentPlanner::CameraFrame& frame,
                                        float oceanHalfSpan) {
        return std::sqrt(
            2.0f * oceanHalfSpan * oceanHalfSpan +
            frame.orbitRadius * frame.orbitRadius +
            2.0f * std::sqrt(2.0f) * oceanHalfSpan * frame.orbitRadius +
            frame.height * frame.height);
    };
    CHECK(defaultDeploymentFrame.farPlane >
          farthestOceanCorner(defaultDeploymentFrame, 4096.0f));
    const DeploymentPlanner::CameraFrame largeDeploymentFrame =
        DeploymentPlanner::BuildCameraFrame(180.0f, 240.0f);
    CHECK(largeDeploymentFrame.orbitRadius > defaultDeploymentFrame.orbitRadius);
    CHECK(largeDeploymentFrame.height > defaultDeploymentFrame.height);
    CHECK(largeDeploymentFrame.terrainViewRadius >
          defaultDeploymentFrame.terrainViewRadius);
    CHECK(largeDeploymentFrame.farPlane > defaultDeploymentFrame.farPlane);
    const float largeDeploymentFarthestTerrain = std::sqrt(
        std::pow(largeDeploymentFrame.orbitRadius +
                 largeDeploymentFrame.terrainViewRadius, 2.0f) +
        std::pow(largeDeploymentFrame.height, 2.0f));
    CHECK(largeDeploymentFrame.farPlane > largeDeploymentFarthestTerrain);
    for (const float deploymentRadius : {34.0f, 126.8f, 600.0f}) {
        const DeploymentPlanner::CameraFrame oceanFrame =
            DeploymentPlanner::BuildCameraFrame(
                43.0f, deploymentRadius, 4096.0f);
        CHECK(oceanFrame.farPlane >
              farthestOceanCorner(oceanFrame, 4096.0f));
    }

    const DeploymentPlanner::CameraFrame maximumDeploymentFrame =
        DeploymentPlanner::BuildCameraFrame(43.0f * 12.0f, 600.0f);
    const float maximumTerrainRadius = (std::max)(
        88.0f * 12.0f + 40.0f,
        maximumDeploymentFrame.terrainViewRadius);
    const uint32_t maximumDeploymentGrid =
        DeploymentPlanner::DeploymentTerrainGridSide(
            maximumTerrainRadius);
    CHECK((maximumDeploymentGrid & 1u) == 0u);
    CHECK(DeploymentPlanner::DeploymentTerrainHalfSpan(
              maximumDeploymentGrid) >= maximumTerrainRadius);
    CHECK(DeploymentPlanner::DeploymentTerrainHalfSpan(
              maximumDeploymentGrid - 2u) < maximumTerrainRadius);
    CHECK(std::abs(
        DeploymentPlanner::DeploymentTerrainTileSize / 8.0f - 1.0f) <
        0.001f);
    // The gameplay clipmap (G = 20, 1 m base) must reach a 24x island's
    // seabed edge, or the shelf past the last ring is simply not drawn.
    {
        const float need = 88.0f * 24.0f + 40.0f;
        const uint32_t rings = DeploymentPlanner::TerrainRingCount(
            need, 20u, 1.0f, DeploymentPlanner::MaxTerrainClipmapRings);
        CHECK(DeploymentPlanner::TerrainClipmapHalfSpan(20u, 1.0f, rings) >= need);
    }
    // Planning-map detail: authored spacing wins; Auto keeps small islands at
    // 1 m and coarsens large ones until the grid fits the tile budget.
    CHECK(DeploymentPlanner::DeploymentTerrainTileSizeFor(1096.0f, 2) == 16.0f);
    CHECK(DeploymentPlanner::DeploymentTerrainTileSizeFor(383.0f, 0) == 8.0f);
    const float autoLargeTile =
        DeploymentPlanner::DeploymentTerrainTileSizeFor(1096.0f, 0);
    CHECK(autoLargeTile == 16.0f);
    CHECK(DeploymentPlanner::DeploymentTerrainGridSide(1096.0f, autoLargeTile) <=
          DeploymentPlanner::DeploymentTerrainAutoMaxSide);
    CHECK(DeploymentPlanner::DeploymentTerrainHalfSpan(
              DeploymentPlanner::DeploymentTerrainGridSide(1096.0f, autoLargeTile),
              autoLargeTile) >= 1096.0f);

    DeferredReleaseQueue<int> releases;
    releases.Retire(4, 10);
    releases.Retire(8, 20);
    releases.Collect(3);
    CHECK(releases.PendingCount() == 2);
    releases.Collect(4);
    CHECK(releases.PendingCount() == 1);
    releases.Collect(8);
    CHECK(releases.PendingCount() == 0);

    // A disconnect from the UI can retire a move-only actor before its draw
    // is submitted. An already-completed older fence must not destroy it.
    DeferredReleaseQueue<std::unique_ptr<int>> actors;
    auto actor = std::make_unique<int>(42);
    int* actorAddress = actor.get();
    actors.RetireAfterSubmission(std::move(actor));
    actors.Collect(100);
    CHECK(actors.PendingCount() == 1);
    CHECK(*actorAddress == 42);
    actors.SealSubmission(101);
    actors.Collect(100);
    CHECK(actors.PendingCount() == 1);
    CHECK(*actorAddress == 42);
    actors.Collect(101);
    CHECK(actors.PendingCount() == 0);

    // A later removal remains protected even when an earlier batch completes.
    actors.RetireAfterSubmission(std::make_unique<int>(1));
    actors.SealSubmission(102);
    actors.RetireAfterSubmission(std::make_unique<int>(2));
    actors.Collect(102);
    CHECK(actors.PendingCount() == 1);
    actors.SealSubmission(104);
    actors.Collect(103);
    CHECK(actors.PendingCount() == 1);
    actors.Collect(104);
    CHECK(actors.PendingCount() == 0);

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

    // Radius damage honours the caller's immunity predicate. This is what keeps
    // the comm tower a player-only objective: enemy grenades and stray blasts
    // run through DamagePrefabsInRadius, and without the filter one landing at
    // its feet would fell it and wrongly credit the player for the destruction.
    RuntimeWorld immuneWorld;
    LevelEntity objective;
    objective.id = 700;
    objective.type = LevelEntityType::Prefab;
    objective.prefabId = "props/comm_tower";
    immuneWorld.Level().entities.push_back(objective);
    LevelEntity ordinary;
    ordinary.id = 701;
    ordinary.type = LevelEntityType::Prefab;
    ordinary.prefabId = "test/destructible";
    immuneWorld.Level().entities.push_back(ordinary);
    immuneWorld.Prefabs().destructibles.push_back(
        { 700, { 0.0f, 0.0f, 0.0f }, 100.0f });
    immuneWorld.Prefabs().destructibles.push_back(
        { 701, { 0.0f, 0.0f, 0.0f }, 100.0f });

    // Look entities up by id: the level may carry entities of its own, so
    // front()/back() are not the ones pushed above.
    const auto towerEnabled = [&immuneWorld]() {
        for (const LevelEntity& e : immuneWorld.Level().entities)
            if (e.id == 700) return e.enabled;
        return false;
    };

    // Non-demolition blast (frag grenade, rocket, enemy fire, a crashing
    // helicopter): the objective is skipped, the ordinary prop still dies.
    auto radiusResults = combat.DamagePrefabsInRadius(
        immuneWorld, { 0.0f, 0.0f, 0.0f }, 10.0f, 100000.0f,
        [](uint64_t entityId) { return entityId != 700; });
    bool touchedObjective = false;
    bool destroyedOrdinary = false;
    for (const auto& r : radiusResults) {
        if (r.entityId == 700) touchedObjective = true;
        if (r.entityId == 701 && r.destroyed) destroyedOrdinary = true;
    }
    CHECK(!touchedObjective);
    CHECK(destroyedOrdinary);
    CHECK(towerEnabled());   // tower still standing

    // Remote charge (no predicate): only this brings the objective down.
    radiusResults = combat.DamagePrefabsInRadius(
        immuneWorld, { 0.0f, 0.0f, 0.0f }, 10.0f, 100000.0f);
    bool destroyedObjective = false;
    for (const auto& r : radiusResults)
        if (r.entityId == 700 && r.destroyed) destroyedObjective = true;
    CHECK(destroyedObjective);
    CHECK(!towerEnabled());

    // Restarting the level must bring destroyed props back. DamagePrefab clears
    // entity.enabled, and the restart path snapshots the *live* level, so without
    // re-enabling prefab entities the comm tower (and every barrel destroyed that
    // run) would stay missing for the rest of the session. This mirrors what
    // RestartActiveLevel does before handing the snapshot to StartLevelOne.
    LevelDefinition restarted = immuneWorld.Level();
    for (LevelEntity& entity : restarted.entities)
        if (entity.type == LevelEntityType::Prefab ||
            entity.type == LevelEntityType::Rock)
            entity.enabled = true;
    bool towerBack = false;
    for (const LevelEntity& entity : restarted.entities)
        if (entity.id == 700) towerBack = entity.enabled;
    CHECK(towerBack);
    // And clearing runtime health restores it to full on the next run.
    immuneWorld.Prefabs().ResetGameplayState();
    CHECK(immuneWorld.Prefabs().health.empty());

    // (The always-carried C4 rule lives in GunModel, which pulls in DX12Core and
    // the asset importers -- too heavy for this renderer-free target to link.
    // Verified by inspection and in-game instead.)

    // ---- Enemy reinforcement dropship ---------------------------------------
    {
        VehicleSystem vehicles;
        DirectX::XMFLOAT3 drop{ 99.0f, 99.0f, 99.0f };
        int samples = 0;
        const auto shore = [&](float x, float) {
            ++samples;
            return x <= 36.0f ? 1.0f : -2.0f;
        };
        CHECK(!vehicles.FindDropshipExfilDropPoint(shore, drop));
        CHECK(samples == 0);
        CHECK(drop.x == 99.0f);

        // Search the whole exfil lane even when the insertion ring is large.
        vehicles.PlaceEscapeBoatOnBearing(DirectX::XM_PIDIV2, 0.0f, 616.0f);
        CHECK(vehicles.FindDropshipExfilDropPoint(shore, drop));
        CHECK(std::abs(drop.x - 36.0f) < 0.001f);
        CHECK(std::abs(drop.z) < 0.001f);
        CHECK(drop.y == 1.0f);
        CHECK(samples > 14);
        CHECK(!vehicles.FindDropshipExfilDropPoint(
            [](float, float) { return -2.0f; }, drop));

        // A diagonal exfil keeps its own bearing and chooses the closest shore.
        vehicles.ResetEscapeBoat();
        vehicles.PlaceEscapeBoatOnBearing(DirectX::XM_PI / 4.0f, 0.0f, 50.0f);
        CHECK(vehicles.FindDropshipExfilDropPoint(
            [](float x, float z) {
                return x * x + z * z <= 31.0f * 31.0f ? 0.8f : 0.0f;
            }, drop));
        CHECK(std::abs(drop.x - drop.z) < 0.001f);
        CHECK(std::abs(std::sqrt(drop.x * drop.x + drop.z * drop.z) -
                       30.0f) < 0.001f);
        CHECK(drop.y >= 0.6f);
    }
    // The wave flies in on the shared secondary-helicopter fields, so the state
    // machine has to hand the airframe back cleanly or the patrol path and the
    // dropship fight over the same position every frame.
    {
        VehicleSystem vehicles;
        CHECK(!vehicles.DropshipActive());
        CHECK(vehicles.DropshipAvailable());

        const DirectX::XMFLOAT3 entry{ 200.0f, 30.0f, 0.0f };
        const DirectX::XMFLOAT3 drop{ 0.0f, 0.0f, 0.0f };
        vehicles.BeginDropshipRun(entry, drop, 3);
        CHECK(vehicles.DropshipActive());
        CHECK(vehicles.dropshipTroopsLeft == 3);
        CHECK(vehicles.dropshipWavesCalled == 1);
        // The slot is taken: a second call-in must not preempt the first.
        CHECK(!vehicles.DropshipAvailable());
        vehicles.BeginDropshipRun(entry, drop, 5);
        CHECK(vehicles.dropshipTroopsLeft == 3);   // unchanged
        CHECK(vehicles.dropshipWavesCalled == 1);  // and not counted

        // Fly the whole run at a fixed step, counting what it unloads. The cap
        // guards against a state that never terminates.
        int released = 0;
        int steps = 0;
        while (vehicles.DropshipActive() && steps++ < 4000)
            released += vehicles.UpdateDropship(1.0f / 60.0f, 0.0f);

        CHECK(released == 3);                 // every troop left the craft
        CHECK(vehicles.dropshipTroopsLeft == 0);
        CHECK(!vehicles.DropshipActive());    // and the slot came back
        CHECK(vehicles.DropshipAvailable());
        CHECK(steps < 4000);                  // terminated on its own

        // Second wave is allowed once the first has cleared, and escalates.
        vehicles.BeginDropshipRun(entry, drop, 4);
        CHECK(vehicles.dropshipWavesCalled == 2);

        // A gunship shot down mid-run stops unloading immediately -- the rest of
        // the squad goes down with the aircraft rather than spawning in midair.
        vehicles.secondaryHelicopterDead = true;
        const int afterDeath = vehicles.UpdateDropship(1.0f / 60.0f, 0.0f);
        CHECK(afterDeath == 0);
        CHECK(!vehicles.DropshipActive());
        CHECK(vehicles.dropshipTroopsLeft == 0);
        // And a downed airframe cannot be sent back up.
        CHECK(!vehicles.DropshipAvailable());
        vehicles.BeginDropshipRun(entry, drop, 3);
        CHECK(!vehicles.DropshipActive());

        // Reset clears the wave counter so a restarted run starts from wave 1.
        vehicles.ResetDropship();
        CHECK(vehicles.dropshipWavesCalled == 0);
    }

    // ---- Escape boat ---------------------------------------------------------
    // Exfil sits offshore on a bearing the caller rolls, out past the insertion
    // ring the player deployed from -- the way out is a leg further to sea than
    // the way in, never a walk back to the start.
    {
        VehicleSystem vehicles;
        CHECK(!vehicles.EscapeBoatReady());

        // Placed on an explicit bearing. +X is bearing pi/2 the way the boat
        // measures it (+Z = 0, turning through +X).
        const float defaultDistance =
            VehicleSystem::EscapeBoatDistanceForRing(34.0f);
        vehicles.PlaceEscapeBoatOnBearing(3.14159265f * 0.5f, 0.0f,
                                          defaultDistance);
        CHECK(vehicles.EscapeBoatReady());
        CHECK(std::abs(vehicles.escapeBoatPosition.x - defaultDistance) < 0.001f);
        CHECK(std::abs(vehicles.escapeBoatPosition.z) < 0.001f);
        CHECK(std::abs(vehicles.escapeBoatPosition.y) < 0.001f);
        // On water, not on the beach. The terrain profile crosses the waterline
        // between 40 m (y = +0.04) and 42 m (y = -0.21), so the boat has to sit
        // beyond that band or it grounds on the shelf. The upper bound keeps it
        // swimmable from shore rather than a hike across open sea.
        CHECK(VehicleSystem::EscapeBoatShoreDistance >= 41.0f);
        CHECK(VehicleSystem::EscapeBoatShoreDistance < 70.0f);

        // Always outside the insertion ring, at every authorable radius. This is
        // the whole point of deriving the distance: the exfil must never sit on
        // or inside the ring the player inserted from.
        for (float ringRadius : { 5.0f, 20.0f, 34.0f, 100.0f, 600.0f }) {
            const float distance =
                VehicleSystem::EscapeBoatDistanceForRing(ringRadius);
            CHECK(distance > ringRadius);
            // Never inside the beach shelf, however tight the ring was authored.
            CHECK(distance >= VehicleSystem::EscapeBoatShoreDistance);
        }
        // A wide ring pushes the boat out with it rather than leaving it
        // stranded inside; the default ring keeps the historical deep-water
        // distance because the shelf floor dominates there.
        CHECK(std::abs(VehicleSystem::EscapeBoatDistanceForRing(600.0f) -
                       (600.0f + VehicleSystem::EscapeBoatRingClearance)) < 0.001f);
        CHECK(std::abs(VehicleSystem::EscapeBoatDistanceForRing(34.0f) -
                       VehicleSystem::EscapeBoatShoreDistance) < 0.001f);

        // Idempotent: a later placement must not move an exfil the player may
        // already be swimming toward.
        const float placedX = vehicles.escapeBoatPosition.x;
        vehicles.PlaceEscapeBoatOnBearing(3.14159265f, 0.0f, defaultDistance);
        CHECK(std::abs(vehicles.escapeBoatPosition.x - placedX) < 0.001f);

        // Boarding is a horizontal test: the player may be swimming or on deck,
        // so height must not decide it.
        const DirectX::XMFLOAT3 boat = vehicles.escapeBoatPosition;
        CHECK(vehicles.PlayerCanBoardEscapeBoat(boat));
        CHECK(vehicles.PlayerCanBoardEscapeBoat(
            { boat.x, boat.y + 40.0f, boat.z }));
        CHECK(vehicles.PlayerCanBoardEscapeBoat(
            { boat.x + VehicleSystem::EscapeBoatBoardRadius - 0.5f,
              boat.y, boat.z }));
        CHECK(!vehicles.PlayerCanBoardEscapeBoat(
            { boat.x + VehicleSystem::EscapeBoatBoardRadius + 1.0f,
              boat.y, boat.z }));
        CHECK(!vehicles.PlayerCanBoardEscapeBoat({ 0.0f, 0.0f, 0.0f }));

        // The bob rides the swell rather than drifting the hull away.
        vehicles.UpdateEscapeBoat(0.5f);
        vehicles.UpdateEscapeBoat(0.5f);
        CHECK(std::abs(vehicles.EscapeBoatBobOffset()) < 0.5f);
        CHECK(std::abs(vehicles.escapeBoatPosition.x - placedX) < 0.001f);

        // A boat that is not out cannot be boarded -- the win condition must not
        // fire on a level that never placed one.
        vehicles.ResetEscapeBoat();
        CHECK(!vehicles.EscapeBoatReady());
        CHECK(!vehicles.PlayerCanBoardEscapeBoat(boat));
    }

    // ---- Time of day ---------------------------------------------------------
    // The presets drive the sky, volumetric fog and DDGI off one sun direction,
    // so the values have to be internally consistent: a "night" that leaves the
    // sun above the horizon renders a lit sky over a dark island.
    {
        // Afternoon is the historical look and the default, so it must keep the
        // exact values every level shipped with before the choice existed.
        const TimeOfDaySettings afternoon =
            MakeTimeOfDaySettings(TimeOfDay::Afternoon);
        CHECK(std::abs(afternoon.lightPos.x - 4.735f) < 0.0001f);
        CHECK(std::abs(afternoon.lightPos.y - 3.095f) < 0.0001f);
        CHECK(std::abs(afternoon.lightPos.z + 8.246f) < 0.0001f);
        CHECK(std::abs(afternoon.directionalLightIntensity - 12.18f) < 0.0001f);
        CHECK(std::abs(afternoon.ambientStrength - 0.07f) < 0.0001f);
        CHECK(std::abs(afternoon.volumetricFogDensity - 0.009f) < 0.0001f);
        CHECK(std::abs(afternoon.volumetricFogDistance - 800.0f) < 0.0001f);

        const TimeOfDaySettings noon = MakeTimeOfDaySettings(TimeOfDay::Noon);
        const TimeOfDaySettings dusk = MakeTimeOfDaySettings(TimeOfDay::Dusk);
        const TimeOfDaySettings night = MakeTimeOfDaySettings(TimeOfDay::Night);

        // Sun elevation orders the presets: overhead at noon, on the horizon at
        // dusk, below it at night. This is the property the sky actually reads.
        const float noonSun = TimeOfDaySunElevation(noon);
        const float afternoonSun = TimeOfDaySunElevation(afternoon);
        const float duskSun = TimeOfDaySunElevation(dusk);
        const float nightSun = TimeOfDaySunElevation(night);
        CHECK(noonSun > afternoonSun);
        CHECK(afternoonSun > duskSun);
        CHECK(duskSun > nightSun);
        // Dusk is a low sun, not a set one -- it still lights the island.
        CHECK(duskSun > 0.0f);
        CHECK(duskSun < 0.2f);
        // Night is genuinely below the horizon, which is what makes it night
        // rather than a dimmed afternoon.
        CHECK(nightSun < 0.0f);
        // Noon is high overhead, so shadows fall short rather than long.
        CHECK(noonSun > 0.9f);

        // Key light dims monotonically toward night.
        CHECK(noon.directionalLightIntensity >
              afternoon.directionalLightIntensity);
        CHECK(afternoon.directionalLightIntensity >
              dusk.directionalLightIntensity);
        CHECK(dusk.directionalLightIntensity >
              night.directionalLightIntensity);

        // Visibility, which scales how far enemies can see.
        const float noonVis = TimeOfDayVisibilityFactor(noon);
        const float afternoonVis = TimeOfDayVisibilityFactor(afternoon);
        const float duskVis = TimeOfDayVisibilityFactor(dusk);
        const float nightVis = TimeOfDayVisibilityFactor(night);

        // Never better than clear daylight, never a total blackout: an enemy
        // who cannot see a player in front of them reads as broken, not dark.
        for (float v : { noonVis, afternoonVis, duskVis, nightVis }) {
            CHECK(v <= 1.0f);
            CHECK(v >= 0.25f);
        }

        // Afternoon is the baseline every level was tuned against, so it must
        // stay at full range or existing engagement distances all shift.
        CHECK(afternoonVis == 1.0f);
        CHECK(noonVis == 1.0f);

        // Darker means shorter sight, and night is the dramatic step. Dusk is
        // still daylight, so it should barely differ -- if dusk ever drops far
        // it means the curve got too aggressive and daytime stealth is free.
        CHECK(duskVis <= afternoonVis);
        CHECK(duskVis > 0.75f);
        CHECK(nightVis < duskVis);
        // Night has to be a real change to be worth choosing: at least a
        // halving of how far a bandit picks the player out.
        CHECK(nightVis < 0.5f);

        // Fog pulls sight in independently of the light, which is the property
        // a weather preset would rely on. Same afternoon sun, thicker fog.
        TimeOfDaySettings foggy = afternoon;
        foggy.volumetricFogDensity = afternoon.volumetricFogDensity * 5.0f;
        const float foggyVis = TimeOfDayVisibilityFactor(foggy);
        CHECK(foggyVis < afternoonVis);
        CHECK(foggyVis >= 0.25f);

        // Fog switched off cannot make the world darker than clear.
        TimeOfDaySettings noFog = afternoon;
        noFog.enableVolumetricFog = false;
        CHECK(TimeOfDayVisibilityFactor(noFog) >= afternoonVis);

        // Night keeps a non-zero floor for silhouettes, but is authored close
        // to black so local lights and muzzle flashes define the scene.
        CHECK(night.ambientStrength > 0.0f);
        CHECK(std::abs(night.ambientLightingIntensity) < 0.0001f);
        CHECK(night.directionalLightIntensity < 0.05f);
        CHECK(night.ambientStrength < 0.002f);
        CHECK(night.ambientLightingIntensity < 0.01f);
        // ...but darker than every daylight preset, or it is not night.
        CHECK(night.ambientStrength < dusk.ambientStrength);
        CHECK(night.ambientLightingIntensity < dusk.ambientLightingIntensity);
        // Night fog is off by default, but still owns a denser, neutral-black
        // ground layer for when it is turned on: the exact authored values
        // restored by the deployment screen's Reset Night fog.
        CHECK(!night.enableVolumetricFog);
        CHECK(std::abs(night.volumetricFogDensity - 0.0116f) < 0.0001f);
        CHECK(std::abs(night.volumetricFogAnisotropy - 0.31f) < 0.0001f);
        CHECK(std::abs(night.volumetricFogHeightFalloff - 0.107f) < 0.0001f);
        CHECK(std::abs(night.volumetricFogBaseHeight - 0.4f) < 0.0001f);
        CHECK(std::abs(night.volumetricFogDistance - 800.0f) < 0.0001f);
        CHECK(std::abs(night.volumetricFogTint.x - 5.0f / 255.0f) < 0.0001f);
        CHECK(std::abs(night.volumetricFogTint.y - 5.0f / 255.0f) < 0.0001f);
        CHECK(std::abs(night.volumetricFogTint.z - 5.0f / 255.0f) < 0.0001f);

        // Night sky is near-black, and cool rather than warm: the clear colour
        // is what shows through wherever the atmosphere does not cover.
        CHECK(night.clearColor.x < 0.1f);
        CHECK(night.clearColor.y < 0.1f);
        CHECK(night.clearColor.z < 0.1f);
        CHECK(night.clearColor.z > night.clearColor.x);   // blue-biased
        // Dusk swings warm: red key well above blue.
        CHECK(dusk.lightColor.x > dusk.lightColor.z);
        // Night key is moonlight -- cool, the opposite bias.
        CHECK(night.lightColor.z > night.lightColor.x);

        // Only Night reports as dark, so anything gating on low light (and the
        // deployment screen's warning) fires exactly once.
        CHECK(TimeOfDayIsDark(TimeOfDay::Night));
        CHECK(!TimeOfDayIsDark(TimeOfDay::Dusk));
        CHECK(!TimeOfDayIsDark(TimeOfDay::Noon));
        CHECK(!TimeOfDayIsDark(TimeOfDay::Afternoon));
        CHECK(!TimeOfDayIsDark(TimeOfDay::MaxFidelity));

        // Max Fidelity Lighting: the renderer set comes from Max fidelity, the
        // sun and sky light from the clock, emission off by day and the
        // level's own (negative) at Dusk and Night.
        {
            const TimeOfDaySettings max = MakeTimeOfDaySettings(TimeOfDay::MaxFidelity);
            const TimeOfDaySettings mfNight = MakeMaxFidelityLightingSettings(TimeOfDay::Night);
            const TimeOfDaySettings mfNoon = MakeMaxFidelityLightingSettings(TimeOfDay::Noon);
            const TimeOfDaySettings mfDusk = MakeMaxFidelityLightingSettings(TimeOfDay::Dusk);
            CHECK(mfNight.giIntensity == max.giIntensity);
            CHECK(mfNight.rtReflections && mfNight.cascadeShadows);
            CHECK(mfNight.volumetricFogDensity == max.volumetricFogDensity);
            CHECK(max.volumetricFogDensity == 0.0051f);
            CHECK(mfNight.lightPos.y == night.lightPos.y);
            CHECK(mfNight.directionalLightIntensity == night.directionalLightIntensity);
            CHECK(mfNight.ambientLightingIntensity == night.ambientLightingIntensity);
            CHECK(mfNight.emissiveIntensity < 0.0f);
            CHECK(mfDusk.emissiveIntensity < 0.0f);
            CHECK(mfNoon.emissiveIntensity == max.emissiveIntensity);
            CHECK(mfNoon.lightColor.z == noon.lightColor.z);
            const TimeOfDaySettings mfMax = MakeMaxFidelityLightingSettings(TimeOfDay::MaxFidelity);
            CHECK(mfMax.lightPos.y == max.lightPos.y && mfMax.emissiveIntensity == max.emissiveIntensity);
        }

        // Every preset needs a name and a briefing for the deployment screen,
        // and no two may share a name or the buttons become ambiguous.
        const TimeOfDay all[] = { TimeOfDay::Noon, TimeOfDay::Afternoon,
                                  TimeOfDay::Dusk, TimeOfDay::Night,
                                  TimeOfDay::MaxFidelity };
        for (const TimeOfDay time : all) {
            CHECK(TimeOfDayName(time) != nullptr);
            CHECK(TimeOfDayName(time)[0] != '\0');
            CHECK(TimeOfDayBriefing(time) != nullptr);
            CHECK(TimeOfDayBriefing(time)[0] != '\0');
            // Sun direction must be non-degenerate, or the sky normalises a
            // zero vector and the atmosphere breaks.
            const TimeOfDaySettings s = MakeTimeOfDaySettings(time);
            const float lengthSq = s.lightPos.x * s.lightPos.x +
                                   s.lightPos.y * s.lightPos.y +
                                   s.lightPos.z * s.lightPos.z;
            CHECK(lengthSq > 1e-4f);
        }
        for (size_t i = 0; i < std::size(all); ++i)
            for (size_t j = i + 1; j < std::size(all); ++j)
                CHECK(std::string(TimeOfDayName(all[i])) !=
                      std::string(TimeOfDayName(all[j])));
    }

    // ---- Weather -------------------------------------------------------------
    {
        const WeatherSettings clear =
            MakeWeatherSettings(WeatherState::Clear);
        const WeatherSettings cloudy =
            MakeWeatherSettings(WeatherState::Cloudy);
        const WeatherSettings fog =
            MakeWeatherSettings(WeatherState::Fog);
        const WeatherSettings rain =
            MakeWeatherSettings(WeatherState::Rain);
        const WeatherSettings storm =
            MakeWeatherSettings(WeatherState::Storm);

        CHECK(clear.rainIntensity == 0.0f);
        CHECK(!clear.worldClouds);
        CHECK(!clear.volumetricFog);
        CHECK(cloudy.worldClouds);
        CHECK(cloudy.worldCloudThickness > 0.0f);
        CHECK(fog.volumetricFog);
        CHECK(!fog.worldClouds);
        CHECK(fog.fogDensity > cloudy.fogDensity);
        CHECK(rain.rainIntensity > 0.0f);
        CHECK(storm.rainIntensity > rain.rainIntensity);
        CHECK(storm.fogDensity > rain.fogDensity);
        CHECK(storm.worldCloudCoverage > rain.worldCloudCoverage);

        const WeatherState all[] = {
            WeatherState::Clear, WeatherState::Cloudy, WeatherState::Fog,
            WeatherState::Rain, WeatherState::Storm, WeatherState::Custom
        };
        for (const WeatherState state : all) {
            CHECK(WeatherStateName(state)[0] != '\0');
            CHECK(WeatherStateBriefing(state)[0] != '\0');
        }
        for (size_t i = 0; i < std::size(all); ++i)
            for (size_t j = i + 1; j < std::size(all); ++j)
                CHECK(std::string(WeatherStateName(all[i])) !=
                      std::string(WeatherStateName(all[j])));
    }

    return failures ? 1 : 0;
}
