#pragma once
// A skinned character instance: owns the shared SkinnedModel, an animation
// player, and a per-frame bone-palette upload buffer, and knows how to draw
// itself through the mesh-shader path with skinning enabled.
#include "BanditWeapon.h"
#include "SkinnedFBXImporter.h"
#include "AnimationRuntime.h"
#include "DirectionalLocomotion.h"
#include "MeshShaderDX12.h"
#include "DX12Core.h"
#include "DestructionDX12.h"
#include "NavigationSystem.h"
#include "EngineLogger.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

extern MeshShaderDX12 g_meshShader;

class SkinnedEnemy;

// Occlusion test reused from the existing bullet line-of-sight raycast so
// perception respects the same walls/terrain/trees a shot would.
using EnemyLineOfSightFn = bool(*)(const SkinnedEnemy&, const DirectX::XMFLOAT3&);
extern EnemyLineOfSightFn g_enemyLineOfSightFn;

// How far anything can see this run, as a multiplier on the clear-daylight
// baseline: 1.0 at noon, ~0.25 on a moonless night. Set by main.cpp from the
// chosen time of day (see TimeOfDayVisibilityFactor) rather than computed here,
// so this header keeps knowing nothing about Scene or the atmosphere.
//
// Only sight is scaled. Hearing deliberately is not: darkness and fog hide a
// man, they do not quiet his rifle, and keeping the noise channel at full range
// is what stops a dark preset from being a free win. Sneaking in unseen still
// means not shooting.
extern float g_enemyVisionScale;

// A loud, momentary sound (gunfire, explosion) enemies can hear through walls.
// Populated fresh each frame by main.cpp and drained by every enemy's Update.
// Player-originated shots carry a source bit so a heard shot can select the
// player as the combat target even when another hostile is closer.
struct EnemyNoiseEvent {
    DirectX::XMFLOAT3 position;
    float radius;
    bool playerOwned = false;

    bool AudibleAt(const DirectX::XMFLOAT3& listener) const {
        const float dx = position.x - listener.x;
        const float dy = position.y - listener.y;
        const float dz = position.z - listener.z;
        return dx * dx + dy * dy + dz * dz <= radius * radius;
    }
};
extern std::vector<EnemyNoiseEvent> g_enemyNoiseEvents;

// Declared above EnemyAlertEvent because the alert carries one: an alert is
// addressed to a side, not broadcast to the field.
enum class Faction : uint8_t { Bandit, Marine };

// Squad radio traffic. Pushed by an enemy the instant it takes damage, so
// squadmates within radius are yanked straight to Combat even without their own
// line of sight or hearing check -- getting shot next to a friend is
// unmissable -- and pushed by an actor that spots an enemy actor, which is what
// makes one man's contact the whole squad's contact. Cleared each frame
// alongside noise events.
struct EnemyAlertEvent {
    DirectX::XMFLOAT3 position;   // where the call-out was raised
    float radius;                 // who is close enough to hear it
    // Who is being told. An alert is squad chatter, not a public siren: a
    // marine calling a contact must not also wake the bandits it just spotted,
    // and at squad-intel range an untagged channel would have every casualty on
    // the field putting the other side into combat.
    //
    // Note a networked player body wears Faction::Marine as a render-path
    // artefact (see Multiplayer.h) and is not a squad member; it never reaches
    // this channel because it is skipped before Update and takes zero damage.
    Faction audience = Faction::Bandit;
    // What was spotted, when the alert is a contact call-out rather than a cry
    // of being hit. Without it an alert only sets awareness: a marine told
    // about a contact 60m away would enter Combat still aiming at whatever
    // NearestHostileTarget handed it, which is its own feet when no bandit is
    // in its personal line of sight. Carrying the position is what makes this
    // intel instead of noise.
    DirectX::XMFLOAT3 contact{ 0.0f, 0.0f, 0.0f };
    bool hasContact = false;
};
extern std::vector<EnemyAlertEvent> g_enemyAlertEvents;

// BanditWeapon and its name/parse helpers live in their own header so the level
// format and its tests can use them without pulling DX12 in behind them.

class SkinnedEnemy {
public:
    // Gates the directional blend space, not an IK solve -- the blend space is
    // plain weighted clip blending. On, a body that moves sideways plays the
    // authored strafe cycles; off, it falls back to the forward-only Run/Walk
    // clips and slides when it strafes.
    //
    // Legacy rigs keep the opt-in bake. Models with a complete authored set
    // use it automatically.
    inline static bool directionalLocomotionIK = false;
    SkinnedModel      model;
    AnimationInstance anim;
    DirectX::XMFLOAT3 position{ 0, 0, 0 };
    float             yaw = 0.0f;     // radians, lower-body movement facing
    float             aimYaw = 0.0f;  // radians, upper-body/weapon facing
    float             aimPitch = 0.0f;
    bool              visible = true;
    bool              castsShadow = true;
    float             health = 100.0f;
    // Highest health this enemy has held, tracked as a high-water mark rather
    // than snapshotted at spawn: per-weapon health is assigned after
    // construction (shotgunners 130, snipers 80), so any single capture point
    // would miss one of them. Anything below this means the enemy has been hit.
    float             peakHealth = 0.0f;
    // Incoming damage multiplier. 1.0 is "takes damage as authored"; marines run
    // 0.25 so an ally soaks four times its health bar without the inflated
    // number, which kept tripping up code tuned around a 100 max (see the cover
    // hold in main.cpp). Applied through ScaleIncomingDamage below rather than
    // at each call site, so every source -- bullets, explosions, debris, fire --
    // is reduced by the same factor.
    float             damageTakenScale = 1.0f;
    float             moveSpeed = 1.8f;
    // Asset-space orientation and ground offset.
    // Assimp preserves this UE asset's Z-up skeleton. Rotate +Z onto engine +Y.
    float             rootPitch = -DirectX::XM_PIDIV2;
    float             rootRoll = 0.0f;
    float             modelScale = 0.01f; // UE cm -> engine metres (applied on world)
    float             footOffset = 0.16f;
    // Optional model-space correction for asset-specific authoring axes.
    float             meshPitch = 0.0f;
    float             meshRoll = 0.0f;
    float             meshYaw  = 0.0f;
    // Off by default: the authored cycles already hold a rifle, so the weapon
    // rides the trigger hand (UpdateGunFromHandBone) and the arms play as
    // animated. Set it to solve both wrists onto the gun instead, which is
    // what a rig without an authored rifle pose needs.
    bool              upperBodyGunLayer = false;
    float             leftArmReach = 0.85f;
    // Gun mesh seating relative to the trigger hand, applied along the barrel
    // and the gun's own up axis in UpdateGunFromHands.
    float             gunScale = 0.62f;
    float             gunGripForward = -0.183f;
    float             gunGripRise = -0.04f;
    // Trigger-hand placement relative to the trigger shoulder, used by
    // ComputeGripTargets. gunShoulderOffset only applies when the shoulder
    // bone is missing and the anchor falls back to the body centerline.
    float             gunShoulderOffset = 0.18f;
    float             gunRearGripForward = 0.16f;
    float             gunRearGripInboard = -0.06f;
    float             gunRearGripDrop = -0.18f;
    // Support-hand placement. Distance down the barrel is leftArmReach; these
    // two shift it across and above the barrel in the gun's own frame.
    float             gunForeGripLateral = 0.253f;
    float             gunForeGripRise = -0.206f;
    // Hand-attached weapon placement, used only when the arm IK is off (see
    // UpdateGunFromHandBone). The gun rides the trigger hand's own bone frame,
    // so these are expressed in that bone's space rather than in world axes:
    // the offsets travel and rotate with the wrist, and the Euler angles line
    // the receiver up with however the hand happens to be authored.
    // Tuned live on the debug HUD against the authored Mixamo cycles, which
    // hold the rifle further out and lower than the values these replace.
    float             gunHandOffsetX = 0.282f;
    float             gunHandOffsetY = -0.168f;
    float             gunHandOffsetZ = -0.033f;
    float             gunHandPitchDegrees = -138.5f;
    float             gunHandYawDegrees = 94.9f;
    float             gunHandRollDegrees = 93.6f;
    // Its own scale rather than gunScale: that one is shared with the IK grip
    // path, and the hand mount was tuned slightly smaller. Folding the two
    // together would move the normal shouldered hold to match this.
    float             gunHandScale = 0.60f;
    float             headTorsoYawOffsetDegrees = 20.4f;
    float             maxSpineTwistDegrees = 85.0f;
    float             spineTwistSpeedDegrees = 220.0f;
    float             orbitRadius = 4.8f;
    float             orbitDirection = 1.0f;
    float             fireCooldown = 1.0f;
    // Grenade throwing. Independent of fireCooldown so a grenade never competes
    // with the rifle for the same timer; spawn code randomises the initial value
    // so a squad does not lob in unison.
    float             grenadeCooldown = 8.0f;
    int               spawnSlot = -1;
    // Metres walked since this actor's last footstep sound. Lives on the actor
    // rather than in a side table keyed by pointer or index: the bandit list is
    // compacted as actors die, so any external key would eventually hand one
    // enemy's stride phase to whoever took its slot.
    float             stepDistance = 0.0f;
    DirectX::XMFLOAT3 lastStepPosition{ 0.0f, 0.0f, 0.0f };
    bool              stepTrackingStarted = false;
    bool              turretGunner = false;
    int               mountedVehicleIndex = 0;
    int               burstShotsRemaining = 0;
    // Loadout. Set at spawn; drives engagement range, aim delay, and how main
    // turns a "fired" result into projectiles.
    BanditWeapon      weapon = BanditWeapon::Rifle;
    // Default keeps every existing bandit spawn/call site correct unchanged.
    Faction           faction = Faction::Bandit;
    // Squad callsign, shown on the friendly nameplate. Assigned once at spawn
    // so it stays with this ally for the run rather than being re-rolled every
    // frame the marker is drawn.
    std::string       callsign;
    // This body is another player: its position and facing come from the
    // network, so the AI must not steer it, shoot with it, or path it. Only the
    // animation is driven locally, from the movement flags in the snapshot.
    // Checked at the update call site rather than inside Update, so the
    // single-player path stays exactly as it was.
    bool              networkControlled = false;
    // Which player this body belongs to when networkControlled. Stored on the
    // actor rather than in a side map keyed by pointer: g_bandits owns these
    // and clears the whole vector on a level reset, which would leave any such
    // map holding dangling pointers.
    uint8_t           netPlayerId = 0xFF;
    // Presentation mirrors of the session's authoritative life state, copied in
    // every frame by UpdateMultiplayerBodies. Never written by gameplay: the
    // host owns these, and a local write would be a desync that only this
    // machine can see. Kept on the actor because the render and hit-test paths
    // already hold it and should not reach back into the session.
    bool              netDowned = false;
    float             netHealth = 100.0f;
    // The owner has pressed DEPLOY. Until then the body is parked at the
    // insertion point while its player plans, and no enemy may see it. Starts
    // false so a body is hidden until a snapshot says otherwise.
    bool              netDeployed = false;
    // Replicated aim state. Written by UpdateNetworkedPose and read by the gun
    // layer to pick the raised hold; meaningless on an AI actor, which decides
    // that from its own awareness.
    bool              netAiming = false;
    // Replicated stance, same shape as netAiming. Drives the pose edit in
    // ApplyCrouch, which is also what lowers the hitbox: shots are tested
    // against the posed bones, so a crouched player behind cover is behind it.
    bool              netCrouching = false;
    // Last position this body was placed at by a snapshot, for measuring how
    // fast it is travelling. Only meaningful for a networked body: an AI actor
    // moves itself and already knows its own speed.
    DirectX::XMFLOAT3 netPreviousPosition_{};
    bool              netHasPreviousPosition_ = false;
    // 0 standing .. 1 fully crouched, eased toward netCrouching for a
    // networked body or toward aiCrouching_ for an AI actor deciding to
    // crouch on its own (see RollAiCrouch/UpdateAiCrouch).
    float             crouchBlend_ = 0.0f;
    // True for the frame(s) ComputePose actually played one of the authored
    // crouch clips (CrouchIdleAim/CrouchWalk*) rather than the pose-edit
    // fallback. Set by UpdateStandingPose/UpdateLocomotion/UpdateNetworkedPose
    // before ComputePose runs; read by ApplyCrouch to skip its pelvis drop so
    // the two crouch mechanisms never stack.
    bool              playedCrouchClip_ = false;
    // AI-only: this actor decided on its own to crouch while it fires, as
    // opposed to netCrouching, which is the replicated player stance and must
    // never be touched by AI (see UpdateAiCrouch). False on a networked body.
    bool              aiCrouching_ = false;
    // Time left before a finished volley's crouch decision lapses. Zero once
    // the actor has stopped firing.
    float             aiCrouchTimer_ = 0.0f;
    // True from a volley's first round until its linger runs out: the crouch
    // is rolled once per volley, not per shot.
    bool              aiCrouchVolley_ = false;
    // Stable network identity for an AI actor, assigned at spawn and never
    // reused within a session. Deliberately not the index in g_bandits: that
    // vector is compacted when bodies are removed, which would renumber every
    // actor after the gap and point a client's hit report at the wrong enemy.
    // 0xFFFF means "not replicated" -- the value for every actor in a
    // single-player game, and for player bodies, which replicate separately.
    uint16_t          netEnemyId = 0xFFFF;
    // When set, Patrol/Alert wandering (UpdatePatrolWaypoint) circles this
    // point instead of the actor's own spawn position -- lets a marine loiter
    // near the player instead of near wherever it was placed. Left unset
    // (nullopt) for bandits, who should keep wandering their own spawn point.
    std::optional<DirectX::XMFLOAT3> leashPosition;
    // The player a marine follows, on the host: 0xFF is this machine's own
    // player, anything else the net player whose transport brought it in.
    uint8_t           leashOwner = 0xFF;
    // True after ammo pickup has been spawned on death, prevents duplicate drops.
    bool              ammoPickupSpawned = false;

    // Sniper telegraph: how long the laser paints the player before the shot.
    // Long on purpose -- the beam IS the warning, so the player needs time to
    // break line of sight or take cover after spotting it.
    static constexpr float kSniperLaserWarning = 5.0f;

    bool IsSniper() const { return weapon == BanditWeapon::Sniper; }
    bool IsShotgunner() const { return weapon == BanditWeapon::Shotgun; }

    // Patrol: no target perceived, walking an authored route or wandering near
    // spawn. Alert: heard/glimpsed the player but lost them, investigating the
    // last known position. Combat: player currently perceived; full engagement
    // (today's aim/orbit/cover/fire behavior).
    enum class AwarenessState { Patrol, Alert, Combat };
    AwarenessState Awareness() const { return awareness_; }

    // An occupied insertion craft is an unmistakable battlefield target. This
    // bypasses the pedestrian vision cone while main still performs the real
    // world line-of-sight test before allowing a shot.
    void ForceCombatTarget(const DirectX::XMFLOAT3& target) {
        if (dead_ || held_) return;
        awareness_ = AwarenessState::Combat;
        combatMemoryTimer_ = 4.0f;
        lastKnownTarget_ = target;
    }

    void ForcePlayerGunshotTarget(const DirectX::XMFLOAT3& target) {
        ForceCombatTarget(target);
        playerGunshotMemoryTimer_ = 4.0f;
    }

    bool PlayerGunshotMemoryActive() const {
        return playerGunshotMemoryTimer_ > 0.0f;
    }

    // TEMP DEBUG: exposes the rifle firing gate so the ImGui panel can show
    // why an actor is or is not shooting. Remove once marine fire is verified.
    bool DebugPreparingShot() const { return preparingShot_; }
    float DebugStationaryAimTime() const { return stationaryAimTime_; }
    bool DebugHasCoverTarget() const { return hasCoverTarget_; }
    bool DebugInCover() const { return inCover_; }
    bool DebugHasGunPose() const { return HasGunPose(); }
    int DebugBurstShots() const { return burstShotsRemaining; }
    enum class FireWaitReason : uint8_t {
        None, NoContact, Blocked, MovingToCover, Cooldown, Aiming,
        NoGunPose, Inactive
    };
    FireWaitReason fireWaitReason = FireWaitReason::NoContact;
    bool debugVisibleTarget = false;
    bool debugSquadContact = false;
    float debugTargetDistance = 0.0f;

    // Vision cone parameters for debug visualization. Half-angle in radians
    // (not the stored cosine) so callers can build cone geometry directly.
    //
    // Scaled by the run's visibility, so this is the range perception actually
    // uses and the debug cone drawn from it shrinks at night to match. The
    // scale is clamped rather than trusted: an unset or garbage global would
    // otherwise silently blind every enemy or let them see across the island.
    float VisionRange() const {
        const float scale = (g_enemyVisionScale > 0.05f &&
                             g_enemyVisionScale <= 1.0f)
            ? g_enemyVisionScale : 1.0f;
        return kVisionRange * scale;
    }
    // Sight range against another actor rather than against the player. Built
    // on VisionRange() and not on kVisionRange, so it still folds in
    // g_enemyVisionScale: a night insertion shortens squad-vs-squad engagements
    // in the same proportion it shortens everything else, instead of leaving
    // two squads trading fire across a field they cannot see.
    float ActorEngagementRange() const {
        return VisionRange() * kActorEngagementRangeScale;
    }
    // The clear-daylight range, before visibility scaling. Exposed so UI can
    // quote a distance without duplicating the constant.
    static constexpr float BaseVisionRange() { return kVisionRange; }
    static float AlertBroadcastRadius() { return kAlertBroadcastRadius; }
    static constexpr float GunshotHearingRadius() { return kGunshotHearingRadius; }
    static constexpr float ImpactNoiseRadius() { return kImpactNoiseRadius; }
    float VisionHalfFovRadians() const { return std::acos(kVisionHalfFovCos); }
    bool TargetInVisionCone(const DirectX::XMFLOAT3& target) const {
        if (faction == Faction::Marine || turretGunner) return true;
        const float dx = target.x - position.x;
        const float dz = target.z - position.z;
        const float distanceSq = dx * dx + dz * dz;
        if (distanceSq <= 1e-6f) return false;
        const float dot = (dx * std::sin(yaw) + dz * std::cos(yaw)) /
                          std::sqrt(distanceSq);
        return dot >= kVisionHalfFovCos;
    }

    // Optional authored patrol path. Leave unset and an enemy wanders in a
    // loose loop around its spawn point instead.
    void SetPatrolRoute(const std::vector<DirectX::XMFLOAT3>& route) {
        patrolRoute_ = route;
        patrolIndex_ = 0;
    }

    // Bandits keep the 30m the player fight is tuned around: it is the distance
    // a wounded bandit tries to hold the player at.
    //
    // A marine's figure is not a retreat distance -- marines never retreat (see
    // evasiveRetreat below). It is only ever read as the radius inside which
    // taking cover is worth doing, so it tracks the range the marine actually
    // fights at rather than a constant borrowed from the player fight. Left at
    // 30 it would have meant a marine engaging at 40m never looked for cover.
    float BackoffRange() const {
        return faction == Faction::Marine ? ActorEngagementRange() : 30.0f;
    }

    bool PlayerInBackoffRange(const DirectX::XMFLOAT3& playerPosition) const {
        const float dx = playerPosition.x - position.x;
        const float dz = playerPosition.z - position.z;
        const float range = BackoffRange();
        return dx * dx + dz * dz <= range * range;
    }

    bool NeedsCoverQuery(const DirectX::XMFLOAT3& playerPosition) const {
        if (dead_ || held_ || rappelling_ || turretGunner || hasCoverTarget_ ||
            coverQueryCooldown_ > 0.0f)
            return false;
        // Marines take cover and shoot from it. A marine's cover-query radius
        // IS its engagement range (see BackoffRange), so every target it can
        // perceive is a cover candidate by construction -- there is no distance
        // at which the range test alone will stop it asking. Unguarded that is
        // a re-query every 0.75s, and SetCoverTarget resets stationaryAimTime_,
        // which starves the aim-up so the marine never fires a shot.
        //
        // This guard is what turns that into "pick a spot, hold it, shoot from
        // it": once settled it stops asking, and only looks for fresh cover
        // after the current one is given up.
        if (faction == Faction::Marine && inCover_) return false;
        return PlayerInBackoffRange(playerPosition);
    }

    void SetCoverTarget(const DirectX::XMFLOAT3& target, float holdSeconds) {
        coverTarget_ = target;
        hasCoverTarget_ = true;
        inCover_ = false;
        coverTravelTime_ = 8.0f;
        coverHoldTime_ = (std::max)(2.5f, holdSeconds);
        coverQueryCooldown_ = 0.75f;
        navigationPath_.clear();
        navigationRepathTimer_ = 0.0f;
        preparingShot_ = false;
        stationaryAimTime_ = 0.0f;
        laserCharge_ = 0.0f;
        burstShotsRemaining = 0;
    }

    void MarkCoverQueryFailed(float retrySeconds = 1.0f) {
        coverQueryCooldown_ = (std::max)(coverQueryCooldown_, retrySeconds);
    }

    bool TakingCover() const { return hasCoverTarget_; }
    bool InCover() const { return hasCoverTarget_ && inCover_; }

    // True while the red beam should be drawn. Laser only exists during the
    // charge; it disappears the instant the shot goes out.
    bool LaserActive() const {
        return IsSniper() && !dead_ && !held_ && visible && laserCharge_ > 0.0f;
    }
    // 0 at first lock, 1 the frame the rifle fires. Renderer ramps the beam.
    float LaserCharge() const {
        return (std::min)(1.0f, laserCharge_ / kSniperLaserWarning);
    }
    // Beam endpoint. Tracks the player continuously, so the dot slides along
    // with them and only the aim delay -- not the aim itself -- can be dodged.
    DirectX::XMFLOAT3 LaserTarget() const { return laserTarget_; }

    bool Init(const SkinnedModel& m) {
        model = m;
        if (!model.valid) return false;
        rootPitch = model.rootPitch;
        footOffset = model.groundOffset;
        // A failed bake is not fatal -- the fallback in UpdateNetworkedPose and
        // UpdateLocomotion both cope -- but it is invisible from the outside,
        // and the symptom (a body stuck in its bind pose) reads as a rendering
        // or networking fault rather than a missing clip. Say which it is.
        if (!locomotion_.Initialize(model.skeleton, model.clips,
                                    model.rebasedClips) &&
            model.authoredDirectional) {
            std::string names;
            for (const AnimationClip& clip : model.clips) {
                if (!names.empty()) names += ", ";
                names += clip.name;
            }
            SGE_LOG("LogAnimation", EngineLog::Level::Warning,
                    "directional blend space unavailable for a body with "
                    "authored cycles; it will fall back to Idle. clips=" +
                    names);
        }
        // One palette upload buffer per in-flight frame so we never overwrite a
        // palette the GPU is still reading.
        const UINT bytes = (UINT)(model.skeleton.BoneCount() * sizeof(DirectX::XMFLOAT4X4));
        paletteBytes_ = bytes;
        D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (UINT i = 0; i < FRAME_COUNT; ++i) {
            if (FAILED(g_dx12.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&palette_[i]))))
                return false;
            D3D12_RANGE none{ 0, 0 };
            if (FAILED(palette_[i]->Map(0, &none, &mapped_[i]))) return false;
            // Previous-frame bone palette for motion vectors.
            if (FAILED(g_dx12.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&palettePrevious_[i]))))
                return false;
            if (FAILED(palettePrevious_[i]->Map(0, &none, &mappedPrevious_[i])))
                return false;
        }
        // Prime with the bind pose so the very first frame renders upright even
        // before any clip is assigned.
        ConfigureGunLayer();
        ComputePose(0.0f);
        return true;
    }

    // Advances a body whose position and facing came from the network instead
    // of from the AI. The ordinary Update path is skipped for those, and that
    // is what normally drives the clip and the skinning pose -- without this a
    // remote player would slide around frozen in its bind pose.
    void UpdateNetworkedPose(float dt, bool moving, bool sprinting,
                             bool aiming = false, bool crouching = false) {
        playedCrouchClip_ = false;
        netAiming = aiming;
        netCrouching = crouching && !netDowned;
        // About the pace the owner's own eye drops at (CameraDX12 SetCrouching
        // moves 0.75 m at 5 m/s), so the body and the view it stands for
        // arrive together.
        const float crouchStep = (std::min)(1.0f, dt * 6.5f);
        crouchBlend_ += ((netCrouching ? 1.0f : 0.0f) - crouchBlend_) * crouchStep;
        if (crouchBlend_ < 0.001f) crouchBlend_ = 0.0f;
        // Kept on the signature for the callers, but not read: the blend space
        // picks walk or run from the speed measured below, which is what the
        // legs have to match. A sprint flag held against a wall would otherwise
        // run the legs in place.
        (void)sprinting;
        if (netDowned) {
            // Hold whatever pose they were in and let the roll below lay the
            // body out. Advancing the clip would have a downed player jogging
            // on their side.
            EaseDownedRoll(dt, true);
            ComputePose(dt);
            return;
        }
        EaseDownedRoll(dt, false);
        // `moving` is the sender's input axes, not their travel: a player
        // holding W against a wall, a crate or another body reports moving
        // forever while the snapshots put them in the same spot. Stand them up
        // once the position says they have stopped.
        const bool netStill = MeasureStillness(dt, position);
        // Deliberately NOT "Run"/"Walk": neither rig has a clip by either name.
        // Both the marine and the bandit carry the Mixamo set, whose cycles are
        // named RunForwardSource..RunRightSource and are consumed by the blend
        // space below rather than played directly. PlayClip is a silent no-op
        // on a name it cannot find, so asking for "Walk" here assigned nothing
        // at all and left a standing remote player in its bind pose.
        //
        // Idle is the one name that does exist on both rigs. It is seeded only
        // in the fallback below, not here: while the blend space is live it
        // owns anim.clip, and seeding Idle every frame would swap the clip and
        // reset its time on every frame the body is moving.

        // How fast the body is actually travelling. A networked body is moved
        // by snapshots, so nothing else here measures it; the blend space needs
        // it to pick the gait and the cadence the legs step at.
        //
        // Speed comes from the position delta because that is the only honest
        // source: the snapshot carries where they are, not how fast.
        const float inverseDt = dt > 1e-5f ? 1.0f / dt : 0.0f;
        const float dx = position.x - netPreviousPosition_.x;
        const float dz = position.z - netPreviousPosition_.z;
        const float travelled = std::sqrt(dx * dx + dz * dz) * inverseDt;
        // First frame after a spawn or a teleport has no previous position
        // worth differencing -- it would read as an enormous speed.
        const float speed = netHasPreviousPosition_ ? travelled : 0.0f;
        netPreviousPosition_ = position;
        netHasPreviousPosition_ = true;

        const bool travelling = moving && !netStill && speed > 0.01f &&
                                speed < kTeleportSpeed;
        const float c = std::cos(yaw), s = std::sin(yaw);
        const float vx = travelling ? dx * inverseDt : 0.0f;
        const float vz = travelling ? dz * inverseDt : 0.0f;
        // Same discrete crouch cycles as the AI path (UpdateLocomotion), keyed
        // off crouchBlend_ rather than netCrouching directly so a body only
        // switches once the pose has actually eased into the crouch -- a
        // networked player un-crouching mid-stride would otherwise pop
        // straight from a crouch-walk frame to a standing one.
        if (travelling && crouchBlend_ > 0.5f) {
            if (const AnimationClip* crouchWalk = model.FindClip(
                    CrouchWalkClipName(vx * c - vz * s, vx * s + vz * c))) {
                directionalMoving_ = true;
                if (anim.clip != crouchWalk) anim.Play(crouchWalk);
                anim.loop = true;
                anim.Advance(dt);
                playedCrouchClip_ = true;
                ComputePose(dt);
                return;
            }
        } else if (!travelling && crouchBlend_ > 0.5f) {
            if (const AnimationClip* crouchIdle = model.FindClip("CrouchIdleAim")) {
                directionalMoving_ = false;
                if (anim.clip != crouchIdle) anim.Play(crouchIdle);
                anim.loop = true;
                anim.Advance(dt);
                playedCrouchClip_ = true;
                ComputePose(dt);
                return;
            }
        }

        // Gated on the blend space actually being usable, not just on the model
        // claiming authored cycles. Initialize needs all nine clips -- the two
        // gaits in four directions plus Idle -- and gives up if the bake missed
        // any of them, after which Update returns null on every call. Keying
        // this on authoredDirectional alone made the else branch unreachable
        // for exactly the bodies whose bake had failed, so nothing advanced a
        // clip and the body rendered its bind pose forever.
        if (model.authoredDirectional && locomotion_.Ready()) {
            directionalMoving_ = travelling;
            if (const AnimationClip* pose = locomotion_.Update(
                    dt, vx * c - vz * s, vx * s + vz * c, moveSpeed)) {
                if (anim.clip != pose) anim.Play(pose);
            }
        } else {
            // No usable blend space: stand in Idle rather than the bind pose.
            // A standing clip, so it plays at its own rate -- the travel-speed
            // scaling above is for gait cycles.
            PlayClip("Idle");
            directionalMoving_ = travelling;
            anim.Advance(dt);
        }
        ComputePose(dt);
    }

    // Tips a downed player onto their side, and stands them back up on revive.
    //
    // Deliberately not a ragdoll. DestructionDX12 has SpawnAuthoredRagdoll and
    // no counterpart, so a body handed to the solver can never be taken back --
    // and a downed player is someone who has to stand up again. There is also
    // no downed or prone clip in the marine's animation set (idle, walk, run
    // and jumps only), so a frozen pose rolled onto its side is what is
    // available without new content. It reads as stiff; it is also the only
    // option here that is reversible.
    void EaseDownedRoll(float dt, bool down) {
        constexpr float kDownedRoll = DirectX::XM_PIDIV2;
        // Standing footOffset lifts the model so its feet meet the ground. On
        // its side that same lift floats the body, so it drops toward zero.
        const float kStandingFootOffset = model.groundOffset;
        constexpr float kDownedFootOffset = 0.02f;
        // Roughly half a second either way: fast enough to read as falling
        // over rather than sinking, slow enough not to pop.
        const float step = (std::min)(1.0f, dt * 2.5f);
        const float targetRoll = down ? kDownedRoll : 0.0f;
        const float targetFoot = down ? kDownedFootOffset : kStandingFootOffset;
        rootRoll += (targetRoll - rootRoll) * step;
        footOffset += (targetFoot - footOffset) * step;
    }

    // AI-only crouch decision, rolled once per volley so a shooter is not
    // flickering between standing and crouched on a per-shot coin flip.
    // netCrouching is
    // deliberately untouched: it is the player's own replicated stance
    // (Multiplayer.h/NetSession.h), and this must never fight it or be fought
    // by it. Called from Update every frame for an AI actor; a networked
    // player body never reaches Update (main.cpp skips networkControlled
    // actors before the AI loop), so the two crouch sources cannot collide,
    // but the flag is guarded here too in case that ever changes.
    //
    // Firing only: the roll happens when a volley starts (firingTimer_ goes
    // live, which TryFireAt holds across the gaps inside a burst) and the
    // stance drops once the volley has been over for kCrouchLinger. Aiming,
    // winding up or advancing without shooting never crouches. Keyed on the
    // shot itself rather than awareness_, so marines -- which fire on bandits
    // without going through the player-awareness states -- crouch too.
    void UpdateAiCrouch(float dt) {
        if (networkControlled || dead_) return;
        // Covers the pause between a volley's last round and the next pose,
        // so the body does not bob up and straight back down.
        constexpr float kCrouchLinger = 0.4f;
        if (firingTimer_ > 0.0f) {
            if (!aiCrouchVolley_) {
                aiCrouchVolley_ = true;
                // 30-40%: enough to see a crouching shooter fairly often
                // without every firefight looking the same way. Does not
                // touch spread or damage -- TryFireAt's cone is unchanged --
                // so a crouched shooter is exactly as dodgeable as a standing
                // one, just posed differently.
                const float chance = 0.30f +
                    (float)std::rand() / RAND_MAX * 0.10f;
                aiCrouching_ = std::rand() / (float)RAND_MAX < chance;
            }
            aiCrouchTimer_ = kCrouchLinger;
        } else if (aiCrouchTimer_ > 0.0f) {
            aiCrouchTimer_ -= dt;
        } else {
            aiCrouchVolley_ = false;
            aiCrouching_ = false;
        }
        // Same ease rate UpdateNetworkedPose uses, so an AI actor's crouch
        // reads at the same speed a replicated player's does.
        const float crouchStep = (std::min)(1.0f, dt * 6.5f);
        crouchBlend_ += ((aiCrouching_ ? 1.0f : 0.0f) - crouchBlend_) * crouchStep;
        if (crouchBlend_ < 0.001f) crouchBlend_ = 0.0f;
    }

    // Crouch as a pose edit on top of whatever the clip produced. This is the
    // fallback for the poses that have no authored crouch clip -- firing,
    // reload, throw, run -- so the pelvis drops, the legs fold with the
    // two-bone solver the arms use to keep the feet where the clip planted
    // them, and the spine leans in over them. Runs before the gun IK, which
    // anchors on the shoulder bone, so the rifle comes down with the chest.
    //
    // Skipped when the clip just played this frame is itself one of the
    // authored crouch clips (see UpdateStandingPose/UpdateLocomotion): those
    // already show a crouched pose, and folding this on top would double the
    // pelvis drop and bury the knees.
    //
    // Drop and lean together take the head down ~0.6 m against the owner's
    // 0.75 m eye drop; the pelvis cannot go further without the knees running
    // out of reach, and a deeper lean folds the chest onto the thighs.
    // Disabled: the authored crouch clips carry the pose now. The procedural
    // drop/lean layer made standing clips look half-squatted. crouchBlend_
    // still drives the hitbox and muzzle height.
    static constexpr bool kProceduralCrouch = false;
    void ApplyCrouch() {
        using namespace DirectX;
        if (!kProceduralCrouch) return;
        if (crouchBlend_ <= 0.0f || poseGlobals_.empty() || playedCrouchClip_)
            return;
        const Skeleton& skel = model.skeleton;
        const int pelvis = skel.Find("pelvis");
        const int spine = skel.Find("spine_01");
        const int thighs[2] = { skel.Find("thigh_l"), skel.Find("thigh_r") };
        const int calves[2] = { skel.Find("calf_l"), skel.Find("calf_r") };
        const int feet[2] = { skel.Find("foot_l"), skel.Find("foot_r") };
        if (pelvis < 0) return;
        for (int leg = 0; leg < 2; ++leg)
            if (thighs[leg] < 0 || calves[leg] < 0 || feet[leg] < 0) return;

        constexpr float kPelvisDrop = 0.42f;
        constexpr float kSpineLean = 0.38f;   // radians, forward
        // World down taken into the pose's space rather than assumed: the rig
        // is axis-converted, so its local "down" is not -Y.
        const XMMATRIX world = MeshWorldMatrix();
        const XMMATRIX inverseWorld = XMMatrixInverse(nullptr, world);
        const XMVECTOR dropModel = XMVector3TransformNormal(
            XMVectorSet(0.0f, -kPelvisDrop * crouchBlend_, 0.0f, 0.0f),
            inverseWorld);

        XMMATRIX footBefore[2];
        for (int leg = 0; leg < 2; ++leg)
            footBefore[leg] = XMLoadFloat4x4(&poseGlobals_[feet[leg]]);

        const XMMATRIX lower = XMMatrixTranslationFromVector(dropModel);
        for (size_t bone = 0; bone < poseGlobals_.size(); ++bone) {
            if (!IsDescendant(static_cast<int>(bone), pelvis)) continue;
            XMStoreFloat4x4(&poseGlobals_[bone],
                XMLoadFloat4x4(&poseGlobals_[bone]) * lower);
        }
        for (int leg = 0; leg < 2; ++leg) {
            SolveArmIK(thighs[leg], calves[leg], feet[leg], footBefore[leg].r[3]);
            // The solver swings the foot with the shin. Put it back exactly as
            // the clip had it -- same place, flat on the same ground.
            const XMMATRIX restore = XMMatrixInverse(nullptr,
                XMLoadFloat4x4(&poseGlobals_[feet[leg]])) * footBefore[leg];
            for (size_t bone = 0; bone < poseGlobals_.size(); ++bone) {
                if (!IsDescendant(static_cast<int>(bone), feet[leg])) continue;
                XMStoreFloat4x4(&poseGlobals_[bone],
                    XMLoadFloat4x4(&poseGlobals_[bone]) * restore);
            }
        }
        if (spine >= 0) {
            // Body right, the same axis ComputeGripTargets uses; a positive
            // turn about it tips the chest toward the facing direction.
            const XMVECTOR right =
                XMVectorSet(std::cos(yaw), 0.0f, -std::sin(yaw), 0.0f);
            RotateBranchWorld(spine, XMLoadFloat4x4(&poseGlobals_[spine]).r[3],
                XMMatrixRotationAxis(right, kSpineLean * crouchBlend_));
        }
        // The IK path rebuilds the palette again after this; the paths without
        // it draw straight from this one.
        for (size_t bone = 0; bone < poseGlobals_.size(); ++bone) {
            const XMMATRIX skin = XMLoadFloat4x4(&skel.offset[bone]) *
                                  XMLoadFloat4x4(&poseGlobals_[bone]);
            XMStoreFloat4x4(&paletteCPU_[bone], XMMatrixTranspose(skin));
        }
    }

    void PlayClip(const std::string& name) {
        if (const AnimationClip* c = model.FindClip(name); c && anim.clip != c) anim.Play(c);
    }

    // Damage a remote player's round did to this actor, applied host-side.
    //
    // The shooter already ran the geometry test against the body it could see,
    // so this deliberately does not re-test it: re-running the sweep here would
    // resolve against the host's position rather than the one the client aimed
    // at, and every shot at a moving target would miss. Everything after the
    // test is the same path a local shot takes, so a client's kill produces the
    // same ragdoll, death event and payout as the host's own.
    void ApplyNetworkBodyDamage(float damage,
                                const DirectX::XMFLOAT3& direction,
                                const DirectX::XMFLOAT3& impact,
                                bool fromPlayer = false) {
        if (dead_) return;
        const float applied = ScaleIncomingDamage(damage);
        health -= applied;
        RegisterThreat(applied);
        if (health <= 0.0f)
            Kill(direction, impact, 1.0f, false,
                 RagdollImpactSource::Bullet, {}, fromPlayer);
    }

    // As above for a headshot, which is lethal regardless of remaining health.
    void KillFromNetworkHeadshot(const DirectX::XMFLOAT3& direction,
                                 const DirectX::XMFLOAT3& impact,
                                 bool fromPlayer = false) {
        if (dead_) return;
        health = 0.0f;
        Kill(direction, impact, 1.0f, true, RagdollImpactSource::Bullet, {},
             fromPlayer);
    }

    // Client-side: the host says this actor is dead, so drop it the same way a
    // local kill would -- ragdoll, death event, audio and payout all through
    // the existing paths rather than a second notion of "dead" the rest of the
    // game would have to learn about. Kill itself is private, and should stay
    // that way: this is the one narrow door the network is allowed through.
    void KillFromNetwork(const DirectX::XMFLOAT3& direction,
                         const DirectX::XMFLOAT3& impact,
                         bool fromPlayer = false) {
        if (dead_) return;
        health = 0.0f;
        Kill(direction, impact, 1.0f, false, RagdollImpactSource::Bullet, {},
             fromPlayer);
    }

    // Whether this actor should read as moving on another machine. Taken from
    // the clip actually playing rather than from a position delta: the receiver
    // would have to derive that a frame late, which shows up as the animation
    // starting after the body has already set off.
    bool NetworkMoving() const {
        if (model.authoredDirectional) return directionalMoving_;
        if (!anim.clip) return false;
        const AnimationClip* idle = model.FindClip("Idle");
        return anim.clip != idle;
    }

    // The AI's own crouch-while-firing decision (see UpdateAiCrouch), for
    // PublishHostEnemies to put on the wire. False for a networked body: its
    // aiCrouching_ is never set, since Update -- the only writer -- is never
    // called on one (main.cpp skips networkControlled actors before the AI
    // loop runs).
    bool AiCrouching() const { return aiCrouching_; }

    // Shared world matrix for both the skinned mesh and the skeleton overlay:
    // native (cm) space -> scaled to metres -> oriented (roll/pitch/yaw) ->
    // placed on the ground. Mesh and joints use the SAME matrix so they align.
    DirectX::XMMATRIX WorldMatrix() const {
        using namespace DirectX;
        return XMMatrixScaling(modelScale, modelScale, modelScale) *
               XMMatrixRotationZ(rootRoll) *
               XMMatrixRotationX(rootPitch) *
               XMMatrixRotationY(yaw) *
               XMMatrixTranslation(position.x, position.y + footOffset, position.z);
    }

    DirectX::XMMATRIX MeshWorldMatrix() const {
        using namespace DirectX;
        return XMMatrixRotationZ(meshRoll) * XMMatrixRotationX(meshPitch) *
               XMMatrixRotationY(meshYaw) * WorldMatrix();
    }

    bool CanRender() const { return visible && model.valid && !paletteCPU_.empty(); }

    D3D12_GPU_VIRTUAL_ADDRESS UploadPalette() {
        if (!CanRender()) return 0;
        const UINT frame = g_dx12.frameIndex % FRAME_COUNT;
        memcpy(mapped_[frame], paletteCPU_.data(), paletteBytes_);
        return palette_[frame]->GetGPUVirtualAddress();
    }

    D3D12_GPU_VIRTUAL_ADDRESS UploadPreviousPalette() {
        if (!CanRender()) return 0;
        const UINT frame = g_dx12.frameIndex % FRAME_COUNT;
        if (!previousPoseGlobals_.empty()) {
            memcpy(mappedPrevious_[frame], previousPoseGlobals_.data(), paletteBytes_);
        } else {
            // No history yet (first frame): use current pose so motion is zero.
            memcpy(mappedPrevious_[frame], paletteCPU_.data(), paletteBytes_);
        }
        return palettePrevious_[frame]->GetGPUVirtualAddress();
    }

    // targetIsActor forwards to PerceivePlayer to select the sight range, and
    // gates the squad-intel broadcast below. Defaulted so any caller that has
    // not been taught the difference gets the short player range.
    void Update(float dt, const DirectX::XMFLOAT3& target, float groundY,
                bool targetIsActor = false) {
        if (dead_) return;
        playerGunshotMemoryTimer_ =
            (std::max)(0.0f, playerGunshotMemoryTimer_ - dt);
        spotBroadcastCooldown_ =
            (std::max)(0.0f, spotBroadcastCooldown_ - dt);
        const DirectX::XMFLOAT3 locomotionStart = position;
        debrisHitCooldown_ = (std::max)(0.0f, debrisHitCooldown_ - dt);
        coverQueryCooldown_ = (std::max)(0.0f, coverQueryCooldown_ - dt);
        position.y = groundY;
        position.x += knockbackVelocity_.x * dt;
        position.z += knockbackVelocity_.z * dt;
        const float knockbackDamping = std::exp(-4.8f * dt);
        knockbackVelocity_.x *= knockbackDamping;
        knockbackVelocity_.z *= knockbackDamping;
        if (knockbackVelocity_.x * knockbackVelocity_.x +
            knockbackVelocity_.z * knockbackVelocity_.z < 0.0025f) {
            knockbackVelocity_.x = 0.0f;
            knockbackVelocity_.z = 0.0f;
        }
        navigationRepathTimer_ -= dt;
        if (!spawnCaptured_) {
            spawnPosition_ = position;
            patrolWaypoint_ = position;
            spawnCaptured_ = true;
        }

        const bool perceived = PerceivePlayer(target, targetIsActor);
        switch (awareness_) {
        case AwarenessState::Patrol:
            if (perceived) {
                awareness_ = AwarenessState::Combat;
                combatMemoryTimer_ = 4.0f;
                lastKnownTarget_ = target;
            }
            break;
        case AwarenessState::Alert:
            if (perceived) {
                awareness_ = AwarenessState::Combat;
                combatMemoryTimer_ = 4.0f;
                lastKnownTarget_ = target;
            } else {
                alertTimer_ -= dt;
                if (alertTimer_ <= 0.0f) awareness_ = AwarenessState::Patrol;
            }
            break;
        case AwarenessState::Combat:
            if (perceived) {
                combatMemoryTimer_ = 4.0f;
                lastKnownTarget_ = target;
            } else {
                combatMemoryTimer_ -= dt;
                if (combatMemoryTimer_ <= 0.0f) {
                    awareness_ = AwarenessState::Alert;
                    alertTimer_ = 6.0f;
                }
            }
            break;
        }

        // Squad intel. An actor holding a live contact on another actor calls
        // it out, so the rest of its side acts on a target only one of them can
        // actually see. Gated on targetIsActor: a bandit that spots the PLAYER
        // must not publish, because that would turn every sighting into a
        // squad-wide alert and quietly rewrite the stealth game.
        //
        // Broadcast from the spotter's position rather than the contact's: the
        // radius is "who is close enough to the man making the call", and
        // anchoring it to the enemy would instead alert whoever is standing
        // near the enemy, which is usually the enemy's own side.
        if (awareness_ == AwarenessState::Combat && perceived && targetIsActor &&
            spotBroadcastCooldown_ <= 0.0f) {
            g_enemyAlertEvents.push_back(
                { position, kSquadIntelRadius, faction, target, true });
            spotBroadcastCooldown_ = 2.0f;
        }

        if (awareness_ != AwarenessState::Combat) {
            aimPitch = 0.0f;
            DirectX::XMFLOAT3 moveTarget = position;
            bool haveMoveTarget = false;
            // A leashed follower stays with its anchor even while Alert --
            // investigating a last-known position for six seconds is enemy
            // behavior, and would leave a marine standing in a field while the
            // player walks off.
            if (awareness_ == AwarenessState::Alert && !leashPosition) {
                moveTarget = lastKnownTarget_;
                haveMoveTarget = true;
            } else {
                haveMoveTarget = UpdatePatrolWaypoint(dt, moveTarget);
            }
            float moveSpeedThisTick = 0.0f;
            if (haveMoveTarget) {
                const float pdx = moveTarget.x - position.x;
                const float pdz = moveTarget.z - position.z;
                const float pdist = std::sqrt(pdx * pdx + pdz * pdz);
                if (pdist > 0.35f) {
                    yaw = std::atan2(pdx, pdz);
                    aimYaw = yaw;
                    moveSpeedThisTick = moveSpeed *
                        (awareness_ == AwarenessState::Alert ? 1.2f : 1.0f);
                    const DirectX::XMFLOAT3 dest{ moveTarget.x, position.y, moveTarget.z };
                    const float navDx = dest.x - navigationDestination_.x;
                    const float navDz = dest.z - navigationDestination_.z;
                    if (g_navigation.Ready() &&
                        (navigationRepathTimer_ <= 0.0f ||
                         navDx * navDx + navDz * navDz > 2.25f)) {
                        if (g_navigation.FindPath(position, dest, navigationPath_)) {
                            navigationWaypoint_ = navigationPath_.size() > 1 ? 1 : 0;
                            navigationDestination_ = dest;
                        } else {
                            navigationPath_.clear();
                            navigationWaypoint_ = 0;
                        }
                        navigationRepathTimer_ = 0.45f +
                            ((float)std::rand() / (float)RAND_MAX) * 0.18f;
                    }
                    float moveX = pdx / (std::max)(pdist, 0.001f);
                    float moveZ = pdz / (std::max)(pdist, 0.001f);
                    while (navigationWaypoint_ < navigationPath_.size()) {
                        const float wx = navigationPath_[navigationWaypoint_].x - position.x;
                        const float wz = navigationPath_[navigationWaypoint_].z - position.z;
                        if (wx * wx + wz * wz > 0.30f) break;
                        ++navigationWaypoint_;
                    }
                    if (navigationWaypoint_ < navigationPath_.size()) {
                        const float wx = navigationPath_[navigationWaypoint_].x - position.x;
                        const float wz = navigationPath_[navigationWaypoint_].z - position.z;
                        const float wd = std::sqrt(wx * wx + wz * wz);
                        if (wd > 0.001f) { moveX = wx / wd; moveZ = wz / wd; }
                    }
                    const float travel = (std::min)(moveSpeedThisTick * dt, 0.45f);
                    position.x += moveX * travel;
                    position.z += moveZ * travel;
                }
            }
            UpdateLocomotion(dt, locomotionStart, moveSpeedThisTick);
            ComputePose(dt);
            return;
        }

        const float dx = target.x - position.x, dz = target.z - position.z;
        const float distance = std::sqrt(dx*dx + dz*dz);
        if (hasCoverTarget_) {
            const float coverDx = coverTarget_.x - position.x;
            const float coverDz = coverTarget_.z - position.z;
            const float coverDistanceSq = coverDx * coverDx + coverDz * coverDz;
            if (!inCover_) {
                coverTravelTime_ -= dt;
                if (coverDistanceSq <= 0.65f * 0.65f) inCover_ = true;
            } else {
                coverHoldTime_ -= dt;
            }
            if (coverTravelTime_ <= 0.0f || (inCover_ && coverHoldTime_ <= 0.0f)) {
                hasCoverTarget_ = false;
                inCover_ = false;
                // Survival remains the standing order. Query another cover
                // position immediately instead of returning to pursuit.
                navigationPath_.clear();
            }
        }
        if (distance > 0.1f) {
            aimYaw = std::atan2(dx, dz);
            const float gunHeight = position.y + footOffset + 1.48f;
            aimPitch = (std::max)(-0.55f, (std::min)(
                0.55f, std::atan2(target.y - gunHeight, distance)));
            // Turn the body onto the target. This used to live in the movement
            // branch and in the in-cover branch only, so an actor that was
            // stationary for any other reason kept whatever yaw it last had and
            // fired across its own shoulder: rooted for a shot wind-up, holding
            // at its safe distance, or simply already standing where it wanted
            // to be. Those are exactly the moments it is shooting, so the wrong
            // facing was most visible when it mattered most.
            //
            // Run unconditionally here instead, where aimYaw is set -- the
            // movement branch below no longer turns, and the upper body still
            // twists the remainder through the spine so a small correction
            // reads as a look rather than a pivot.
            const float turn = std::atan2(
                std::sin(aimYaw - yaw), std::cos(aimYaw - yaw));
            const float maxTurn = 5.5f * dt;
            yaw += (std::max)(-maxTurn, (std::min)(maxTurn, turn));
        }
        if (preparingShot_) stationaryAimTime_ += dt;
        // A sniper walking while its laser is up would drag the beam across the
        // world and make the telegraph unreadable. Plant it for the wind-up.
        const bool rooted = !hasCoverTarget_ &&
            (preparingShot_ || laserCharge_ > 0.0f);
        UpdateAiCrouch(dt);
        float speed = 0.0f;
        const bool movingToCover = hasCoverTarget_ && !inCover_;
        const float safeDistance = BackoffRange();
        // The ring this actor tries to fight from.
        //
        // Bandits keep their authored per-loadout radius: the shotgunner's 2.6
        // is the entire reason that class works, and the sniper's 26 is what
        // keeps its telegraph survivable.
        //
        // Marines instead stand off at a fraction of the distance they can see
        // a bandit from, which is what makes them read as units rather than as
        // a mob -- acquire, halt, engage, instead of jogging into knife range
        // first. Two thirds rather than the full range on purpose: at the very
        // edge of sight the line-of-sight ray is long and the contact is
        // fragile, so a unit that halts out there spends the fight losing and
        // re-acquiring the same target. The floor at orbitRadius means a deep
        // night, where g_enemyVisionScale collapses the engagement range, falls
        // back to the old close ring rather than trying to orbit at 3m.
        const float standoffRadius = faction == Faction::Marine
            ? (std::max)(orbitRadius, ActorEngagementRange() * 0.66f)
            : orbitRadius;
        // Backing off is bandit behavior for keeping the player at arm's
        // length. Marines are the aggressor in the marine-vs-bandit fight and
        // the player's fire support; one that gave ground would simply walk
        // backwards out of its own engagement range and never shoot. Allies
        // take up a standoff and orbit there instead.
        // Track the peak before comparing, so an undamaged enemy always reads as
        // full and the very first frame cannot register as a hit.
        if (health > peakHealth) peakHealth = health;
        // Only a wounded bandit gives ground. At full health he stands and
        // fights, so backing off reads as a reaction to being hit rather than
        // the default opening move.
        const bool damaged = health < peakHealth;
        const bool evasiveRetreat = !hasCoverTarget_ && damaged &&
            faction == Faction::Bandit && distance < safeDistance;
        // Bandits hold at their safe distance; marines move to their standoff
        // and orbit there, which is their own way of holding a range. An unhurt
        // bandit is excluded: this flag suppresses the whole movement branch
        // below, so holding it while he no longer retreats would freeze him on
        // the spot instead of letting him orbit and fight.
        const bool holdingSafeRange = !hasCoverTarget_ && !evasiveRetreat &&
            damaged && faction == Faction::Bandit;
        if ((distance > 0.1f || movingToCover) && !rooted && !inCover_ &&
            !holdingSafeRange) {
            const float inv = distance > 0.001f ? 1.0f / distance : 0.0f;
            const float inwardX = dx * inv;
            const float inwardZ = dz * inv;
            float moveX = inwardX;
            float moveZ = inwardZ;
            bool orbiting = false;

            if (movingToCover) {
                const float coverDx = coverTarget_.x - position.x;
                const float coverDz = coverTarget_.z - position.z;
                const float coverDistance = std::sqrt(
                    coverDx * coverDx + coverDz * coverDz);
                if (coverDistance > 0.001f) {
                    moveX = coverDx / coverDistance;
                    moveZ = coverDz / coverDistance;
                }
                speed = moveSpeed * 1.65f;
            } else if (evasiveRetreat) {
                moveX = -inwardX;
                moveZ = -inwardZ;
                speed = moveSpeed * 1.65f;
            } else if (distance <= standoffRadius + 2.2f) {
                // Grounded version of old hover-enemy controller: preserve a
                // combat ring while moving tangentially around player.
                const float tangentX = -inwardZ * orbitDirection;
                const float tangentZ =  inwardX * orbitDirection;
                const float radial = (std::max)(-0.7f,
                    (std::min)(0.9f, (distance - standoffRadius) * 0.75f));
                moveX = tangentX + inwardX * radial;
                moveZ = tangentZ + inwardZ * radial;
                const float moveLength = std::sqrt(moveX*moveX + moveZ*moveZ);
                if (moveLength > 0.001f) {
                    moveX /= moveLength;
                    moveZ /= moveLength;
                }
                speed = moveSpeed * 0.9f;
                orbiting = true;
            } else {
                speed = distance > 11.0f ? moveSpeed * 1.65f : moveSpeed;
            }
            // Crouched but not rooted -- e.g. easing into or out of the aim-up
            // window while still closing distance. Slow rather than snap back
            // to standing speed, matching the crouch-walk clips picked in
            // UpdateLocomotion (CrouchWalkClipName) below.
            if (aiCrouching_) speed *= 0.5f;

            // Detour supplies corridor-safe steering. Near combat ring, query a
            // short tangent destination; farther away, path toward player.
            XMFLOAT3 requestedDestination = movingToCover
                ? coverTarget_
                : evasiveRetreat
                    ? XMFLOAT3(position.x + moveX * 4.5f, position.y,
                               position.z + moveZ * 4.5f)
                : orbiting
                    ? XMFLOAT3(position.x + moveX * 3.0f, position.y,
                               position.z + moveZ * 3.0f)
                    : XMFLOAT3(target.x, position.y, target.z);
            const float navDx = requestedDestination.x - navigationDestination_.x;
            const float navDz = requestedDestination.z - navigationDestination_.z;
            if (g_navigation.Ready() &&
                (navigationRepathTimer_ <= 0.0f || navDx*navDx + navDz*navDz > 2.25f)) {
                if (g_navigation.FindPath(position, requestedDestination, navigationPath_)) {
                    navigationWaypoint_ = navigationPath_.size() > 1 ? 1 : 0;
                    navigationDestination_ = requestedDestination;
                } else {
                    navigationPath_.clear();
                    navigationWaypoint_ = 0;
                }
                navigationRepathTimer_ = 0.45f +
                    ((float)std::rand() / (float)RAND_MAX) * 0.18f;
            }
            while (navigationWaypoint_ < navigationPath_.size()) {
                const float wx = navigationPath_[navigationWaypoint_].x - position.x;
                const float wz = navigationPath_[navigationWaypoint_].z - position.z;
                if (wx*wx + wz*wz > 0.30f) break;
                ++navigationWaypoint_;
            }
            if (navigationWaypoint_ < navigationPath_.size()) {
                const float wx = navigationPath_[navigationWaypoint_].x - position.x;
                const float wz = navigationPath_[navigationWaypoint_].z - position.z;
                const float waypointDistance = std::sqrt(wx*wx + wz*wz);
                if (waypointDistance > 0.001f) {
                    moveX = wx / waypointDistance;
                    moveZ = wz / waypointDistance;
                }
            }

            // Asset loading can make one frame several seconds long. Never let
            // that frame overshoot through the player and spawn behind them.
            const float travel = (std::min)(speed * dt, 0.45f);
            position.x += moveX * travel;
            position.z += moveZ * travel;
            // Facing is turned onto the target where aimYaw is set, for every
            // actor rather than only a moving one; local velocity selects the
            // leg gait, so a strafing orbit still plays sideways steps.
        }
        const bool running = speed > moveSpeed * 1.2f;
        const float referenceSpeed = running ? moveSpeed * 1.65f : moveSpeed;
        const float playbackRate = speed > 0.01f
            ? (std::max)(0.75f, (std::min)(1.15f, speed / referenceSpeed)) : 1.0f;
        UpdateLocomotion(dt, locomotionStart, speed, playbackRate);
        ComputePose(dt);
    }

    void HoldAt(float dt, const DirectX::XMFLOAT3& holdPosition, float facingYaw) {
        if (dead_) return;
        held_ = true;
        position = holdPosition;
        yaw = facingYaw;
        aimYaw = facingYaw;
        aimPitch = 0.0f;
        knockbackVelocity_ = { 0.0f, 0.0f, 0.0f };
        preparingShot_ = false;
        stationaryAimTime_ = 0.0f;
        laserCharge_ = 0.0f;
        burstShotsRemaining = 0;
        navigationPath_.clear();
        PlayClip("Idle");
        anim.Advance(dt * 0.35f);
        ComputePose(dt);
    }

    void UpdateMounted(float dt, const DirectX::XMFLOAT3& mountPosition,
                       const DirectX::XMFLOAT3& target) {
        if (dead_) return;
        position = mountPosition;
        knockbackVelocity_ = { 0.0f, 0.0f, 0.0f };
        const float dx = target.x - position.x;
        const float dz = target.z - position.z;
        const float horizontalDistance = std::sqrt(dx*dx + dz*dz);
        if (horizontalDistance > 0.1f) {
            aimYaw = std::atan2(dx, dz);
            const float gunHeight = position.y + footOffset + 1.48f;
            aimPitch = (std::max)(-0.55f, (std::min)(
                0.55f, std::atan2(target.y - gunHeight, horizontalDistance)));
            yaw = aimYaw;
        }
        if (preparingShot_) stationaryAimTime_ += dt;
        navigationPath_.clear();
        PlayClip("Idle");
        anim.Advance(dt);
        ComputePose(dt);
    }

    // ---- Rappel descent -----------------------------------------------------
    // A reinforcement roping down from the dropship. While descending the actor
    // is off the AI path entirely: it cannot shoot, cannot be pushed around, and
    // holds its own Y rather than being snapped to the terrain the way Update()
    // does. The rope itself is drawn by the app layer -- this only owns the
    // actor's descent along it.
    //
    // Modelled on UpdateMounted (pin position, skip AI, still pose the model)
    // because that is the established way to take an actor off the ground path
    // without inventing a second update contract.
    static constexpr float RappelDescentSpeed = 7.5f;
    // Held at the bottom before the actor is released to the AI, so a squad does
    // not sprint off the instant its boots touch down.
    static constexpr float RappelReleaseDelay = 0.28f;

    // Puts the actor on the rope at `from`, descending to ground level.
    void BeginRappel(const DirectX::XMFLOAT3& from, float facingYaw) {
        if (dead_) return;
        rappelling_ = true;
        rappelReleaseTimer_ = RappelReleaseDelay;
        position = from;
        yaw = facingYaw;
        aimYaw = facingYaw;
        aimPitch = 0.0f;
        knockbackVelocity_ = { 0.0f, 0.0f, 0.0f };
        preparingShot_ = false;
        stationaryAimTime_ = 0.0f;
        burstShotsRemaining = 0;
        navigationPath_.clear();
    }

    bool Rappelling() const { return rappelling_ && !dead_; }

    // Steps the descent. Returns true once the actor has landed and been
    // released, so the caller can hand it back to the normal AI update.
    //
    // Dying on the rope drops the actor immediately -- the ragdoll takes over
    // from wherever it was, which is the whole point of shooting someone on a
    // rope.
    bool UpdateRappel(float dt, float groundY) {
        if (dead_) { rappelling_ = false; return true; }
        if (!rappelling_) return true;

        if (position.y > groundY) {
            position.y = (std::max)(groundY, position.y - RappelDescentSpeed * dt);
            // Feet-first, hanging: no walk cycle while on the rope.
            PlayClip("Idle");
            anim.Advance(dt * 0.4f);
            ComputePose(dt);
            return false;
        }

        position.y = groundY;
        rappelReleaseTimer_ -= dt;
        if (rappelReleaseTimer_ > 0.0f) {
            PlayClip("Idle");
            anim.Advance(dt);
            ComputePose(dt);
            return false;
        }
        rappelling_ = false;
        return true;
    }

    // Re-anchors patrol and leash to wherever the actor now stands. Update()
    // captures the spawn once, on its first tick; anything that teleports an
    // actor after that (the navmesh scatter test mode) has to clear the capture
    // or the actor keeps wandering back toward a spawn it no longer occupies.
    void ResetSpawnAnchor() {
        spawnCaptured_ = false;
        navigationPath_.clear();
        navigationWaypoint_ = 0;
    }

    void SetHeld(bool held) {
        if (dead_) return;
        held_ = held;
        preparingShot_ = false;
        stationaryAimTime_ = 0.0f;
        laserCharge_ = 0.0f;
        burstShotsRemaining = 0;
    }

    bool Held() const { return held_ && !dead_; }

    bool Throw(const DirectX::XMFLOAT3& direction, float strength = 16.0f) {
        if (dead_ || !held_) return false;
        const DirectX::XMFLOAT3 impact = {
            position.x, position.y + footOffset + 1.15f, position.z };
        // Throwing a held body is a direct player input, so the credit is not
        // a parameter -- there is no other way to reach this.
        Kill(direction, impact, strength, true,
             RagdollImpactSource::Throw, "pelvis", true);
        return true;
    }

    // With the arm IK off the weapon is parented to the trigger hand instead
    // of solved onto both, so it still has a valid pose and still draws --
    // gunWorld_ is written either way. The pose arrays being populated is the
    // real requirement, and that holds in both paths.
    bool HasGunPose() const {
        return !dead_ && handBone_ >= 0 &&
               static_cast<size_t>(handBone_) < poseGlobals_.size();
    }

    DirectX::XMFLOAT3 AimRayOrigin() const {
        DirectX::XMFLOAT3 origin = GunOriginWorld();
        const float sx = std::sin(aimYaw), cz = std::cos(aimYaw);
        const float cp = std::cos(aimPitch), sp = std::sin(aimPitch);
        origin.x += sx * cp * 0.78f;
        origin.y += sp * 0.78f;
        origin.z += cz * cp * 0.78f;
        return origin;
    }

    // Vision cone + hearing gate. Cheap distance/angle checks first, the
    // occlusion raycast only when a target already falls inside range and
    // FOV -- mirrors NeedsLineOfSightCheck's cheap-before-expensive ordering.
    // Turret gunners keep their original always-aware behavior unchanged.
    // target is whatever main.cpp decides this actor's nearest hostile is --
    // the player's position for a bandit, or a marine's position for a bandit
    // targeting an ally, or a bandit's position for a marine. No player-specific
    // logic lives in here despite the name.
    //
    // targetIsActor says whether `target` is another soldier rather than the
    // player, which selects the sight range. The name of this function is
    // already a lie; this parameter is what keeps the lie harmless. A
    // soldier-shaped target at 60m is a legitimate contact, the player at 60m
    // is not, and collapsing the two would hand every bandit a player-detection
    // range no stealth, fog or night preset was ever balanced against. It
    // defaults false so the short, safe range is what an un-updated caller gets.
    bool PerceivePlayer(const DirectX::XMFLOAT3& target,
                        bool targetIsActor = false) const {
        if (dead_ || held_) return false;
        const float dx = target.x - position.x, dz = target.z - position.z;
        const float distSq = dx * dx + dz * dz;
        // A turret gunner used to `return true` here unconditionally, which was
        // unlimited range: it entered combat the instant a level loaded, from
        // anywhere on the island, through terrain. It keeps its advantages --
        // no forward cone (the turret traverses) and a much longer reach than a
        // man on foot, since it is elevated behind a mounted optic -- but it now
        // has to actually be able to see you or hear a nearby gunshot.
        if (turretGunner) {
            // Turrets traverse freely when they see a target, but hearing is
            // still what lets a nearby shot wake one behind cover. Keep this
            // before the visual range gate so the turret does not lose the
            // shared gunshot channel used by infantry.
            for (const EnemyNoiseEvent& noise : g_enemyNoiseEvents) {
                if (noise.AudibleAt(position)) return true;
            }
            if (distSq > kTurretGunnerVisionRange * kTurretGunnerVisionRange)
                return false;
            if (distSq <= 1e-6f) return true;
            return !g_enemyLineOfSightFn ||
                   g_enemyLineOfSightFn(*this, target);
        }
        const float sightRange =
            targetIsActor ? ActorEngagementRange() : VisionRange();
        if (distSq <= sightRange * sightRange && distSq > 1e-6f) {
            const float invLen = 1.0f / std::sqrt(distSq);
            const float facingX = std::sin(yaw), facingZ = std::cos(yaw);
            const float dot = (dx * invLen) * facingX + (dz * invLen) * facingZ;
            // Marines waive the cone entirely, which at 200 degrees now only
            // covers the wedge directly behind them. Kept anyway: a follower's
            // yaw tracks whatever it is walking toward, so the one bandit a
            // marine is most likely to have at its back is the one that flanked
            // the squad -- exactly the contact an ally exists to deal with.
            // Range and the occlusion raycast still apply, so this is squad
            // awareness rather than x-ray vision.
            const bool ignoreFov = faction == Faction::Marine;
            if (dot >= kVisionHalfFovCos || ignoreFov) {
                if (g_enemyLineOfSightFn && g_enemyLineOfSightFn(*this, target))
                    return true;
            }
        }
        for (const EnemyNoiseEvent& noise : g_enemyNoiseEvents) {
            if (noise.AudibleAt(position)) return true;
        }
        for (const EnemyAlertEvent& alert : g_enemyAlertEvents) {
            // Only own-side traffic. An enemy casualty is not a friendly
            // call-out, and without this filter the intel channel would have
            // each side waking the other.
            if (alert.audience != faction) continue;
            const float ax = alert.position.x - position.x;
            const float az = alert.position.z - position.z;
            const float radius = alert.radius;
            if (ax * ax + az * az <= radius * radius) return true;
        }
        return false;
    }

    // Nearest contact this actor's own side has called out to it this frame.
    //
    // Read by the target-selection code in main.cpp, which is the only place
    // that can act on it: Update receives its target already decided, and the
    // combat aim uses that parameter rather than lastKnownTarget_, so an actor
    // cannot adopt a handed-over contact from inside its own update.
    //
    // The spotter already paid for the range test and the line-of-sight ray.
    // Whoever receives this gets the contact for free, which is the whole
    // point -- one man seeing the enemy is the squad seeing the enemy. Walls
    // still matter, because only a spotter with a clear line ever publishes,
    // and the receiver still needs its own line of sight before it may fire.
    bool SquadContact(DirectX::XMFLOAT3& outContact) const {
        if (dead_ || held_) return false;
        bool found = false;
        float bestDistSq = FLT_MAX;
        for (const EnemyAlertEvent& alert : g_enemyAlertEvents) {
            if (!alert.hasContact || alert.audience != faction) continue;
            const float ax = alert.position.x - position.x;
            const float az = alert.position.z - position.z;
            if (ax * ax + az * az > alert.radius * alert.radius) continue;
            const float cx = alert.contact.x - position.x;
            const float cz = alert.contact.z - position.z;
            const float d = cx * cx + cz * cz;
            if (d < bestDistSq) {
                bestDistSq = d;
                outContact = alert.contact;
                found = true;
            }
        }
        return found;
    }

    bool HeardPlayerGunshot() const {
        if (dead_ || held_) return false;
        for (const EnemyNoiseEvent& noise : g_enemyNoiseEvents) {
            if (!noise.playerOwned) continue;
            if (noise.AudibleAt(position)) return true;
        }
        return false;
    }

    // Returns true and writes `outTarget` when there's somewhere to walk this
    // tick; false means stand idle (mid-pause, or no route/navmesh yet).
    bool UpdatePatrolWaypoint(float dt, DirectX::XMFLOAT3& outTarget) {
        // Leashed (a marine following the player): walk straight at the anchor
        // whenever it drifts beyond the follow distance, and hold position
        // inside it. Checked before the pause timer and the authored route so
        // neither can strand a follower -- a stale pause from earlier wandering
        // would otherwise freeze it for seconds while the player walks away.
        if (leashPosition) {
            constexpr float kFollowDistance = 4.0f;
            patrolPauseTimer_ = 0.0f;
            const float ldx = leashPosition->x - position.x;
            const float ldz = leashPosition->z - position.z;
            if (ldx * ldx + ldz * ldz <= kFollowDistance * kFollowDistance)
                return false;
            outTarget = *leashPosition;
            return true;
        }
        if (patrolPauseTimer_ > 0.0f) { patrolPauseTimer_ -= dt; return false; }
        if (!patrolRoute_.empty()) {
            if (patrolIndex_ >= patrolRoute_.size()) patrolIndex_ = 0;
            outTarget = patrolRoute_[patrolIndex_];
            const float dx = outTarget.x - position.x, dz = outTarget.z - position.z;
            if (dx * dx + dz * dz <= 0.6f * 0.6f) {
                patrolIndex_ = (patrolIndex_ + 1) % patrolRoute_.size();
                patrolPauseTimer_ = 2.0f + ((float)std::rand() / RAND_MAX) * 2.0f;
                return false;
            }
            return true;
        }
        // No authored route: wander in a loose loop around the spawn point
        // (a bandit holding the ground it was placed on).
        const float dx = patrolWaypoint_.x - position.x;
        const float dz = patrolWaypoint_.z - position.z;
        if (dx * dx + dz * dz <= 0.6f * 0.6f) {
            const float angle = ((float)std::rand() / RAND_MAX) * 6.2831853f;
            const float radius = 3.0f + ((float)std::rand() / RAND_MAX) * 5.0f;
            patrolWaypoint_ = { spawnPosition_.x + std::cos(angle) * radius,
                                spawnPosition_.y,
                                spawnPosition_.z + std::sin(angle) * radius };
            patrolPauseTimer_ = 2.0f + ((float)std::rand() / RAND_MAX) * 3.0f;
            return false;
        }
        outTarget = patrolWaypoint_;
        return true;
    }

    // Bandits telegraph for two seconds so the player can react. Nobody has to
    // dodge friendly fire, so allies snap up far faster.
    float AimUpSeconds() const {
        return faction == Faction::Marine ? 0.5f : 2.0f;
    }

    bool NeedsLineOfSightCheck() const {
        if (awareness_ != AwarenessState::Combat && !turretGunner) return false;
        if (dead_ || held_ || rappelling_ || !visible || !HasGunPose())
            return false;
        if (turretGunner) return true;
        // The sniper needs a truthful sight test every frame it is charging, not
        // just on the firing frame: the beam is only fair if breaking cover
        // actually drops the lock.
        if (IsSniper()) return fireCooldown <= 0.0f;
        if (burstShotsRemaining > 0)
            return fireCooldown <= 0.0f;
        return fireCooldown <= 0.0f &&
               preparingShot_ && stationaryAimTime_ >= AimUpSeconds();
    }

    // Play the throw wind-up. Called by whoever actually spawned the grenade,
    // so the body and the round can never disagree about whether a throw
    // happened -- the decision, the roll and the cooldown all live with the
    // caller, and this is only the animation.
    //
    // Cancels the firing pose outright: the burst that led into the throw is
    // over, and leaving its timer running would put the rifle back up the
    // moment the wind-up ends.
    void PlayGrenadeThrow() {
        if (dead_ || held_ || rappelling_) return;
        throwPlaying_ = true;
        firingTimer_ = 0.0f;
    }

    // targetVelocity lets the shot be led: the aim point becomes where the
    // target will be when the round lands, not where it is now. Defaults to
    // zero, which reproduces the old aim-at-the-current-position behaviour.
    bool TryFireAt(float dt, const DirectX::XMFLOAT3& target,
                   bool hasLineOfSight,
                   DirectX::XMFLOAT3& origin, DirectX::XMFLOAT3& direction,
                   const DirectX::XMFLOAT3& targetVelocity = { 0.0f, 0.0f, 0.0f },
                   float projectileSpeed = 0.0f) {
        using namespace DirectX;
        if (awareness_ != AwarenessState::Combat && !turretGunner) return false;
        if (dead_ || held_ || rappelling_ || !visible || !HasGunPose())
            return false;
        fireCooldown -= dt;
        if (hasCoverTarget_ && !inCover_) {
            preparingShot_ = false;
            stationaryAimTime_ = 0.0f;
            laserCharge_ = 0.0f;
            burstShotsRemaining = 0;
            return false;
        }
        if (turretGunner) {
            if (!hasLineOfSight) {
                if (!mountedFiring_) {
                    mountedSightTime_ = 0.0f;
                    mountedLostSightTime_ = 0.0f;
                    fireCooldown = 0.0f;
                    return false;
                }
                mountedLostSightTime_ += dt;
                if (mountedLostSightTime_ >= 3.0f) {
                    mountedSightTime_ = 0.0f;
                    mountedLostSightTime_ = 0.0f;
                    mountedFiring_ = false;
                    fireCooldown = 0.0f;
                    return false;
                }
            } else {
                mountedLostSightTime_ = 0.0f;
                if (mountedSightTime_ <= 0.0f)
                    spottedEventPending_ = true;
                mountedSightTime_ += dt;
                if (mountedSightTime_ < 2.0f)
                    return false;
            }
            if (fireCooldown > 0.0f) return false;
        } else if (IsSniper()) {
            // Losing sight cancels the charge outright rather than pausing it, so
            // ducking behind cover buys a fresh five seconds instead of a shot
            // the moment the player leans back out.
            if (!hasLineOfSight) {
                laserCharge_ = 0.0f;
                preparingShot_ = false;
                stationaryAimTime_ = 0.0f;
                return false;
            }
            if (fireCooldown > 0.0f) {
                laserCharge_ = 0.0f;
                return false;
            }
            if (laserCharge_ <= 0.0f) spottedEventPending_ = true;
            // Keep the beam glued to the player through the whole wind-up.
            laserTarget_ = target;
            laserCharge_ += dt;
            preparingShot_ = true;
            if (laserCharge_ < kSniperLaserWarning) return false;
            laserCharge_ = 0.0f;
            preparingShot_ = false;
        } else {
            if (!hasLineOfSight) {
                burstShotsRemaining = 0;
                preparingShot_ = false;
                stationaryAimTime_ = 0.0f;
                return false;
            }
            if (burstShotsRemaining <= 0) {
                if (fireCooldown > 0.0f) return false;
                if (!preparingShot_) {
                    preparingShot_ = true;
                    stationaryAimTime_ = 0.0f;
                    spottedEventPending_ = true;
                    return false;
                }
                if (stationaryAimTime_ < AimUpSeconds()) return false;
            } else if (fireCooldown > 0.0f) {
                return false;
            }
        }

        origin = AimRayOrigin();

        // Lead the target so the round arrives where it is going. Two
        // fixed-point iterations; the flight time barely shifts once the aim
        // point moves, so this settles immediately.
        //
        // The sniper's laser is deliberately excluded: laserTarget_ above stays
        // glued to the real position, and the beam showing one point while the
        // round flies at another would break the telegraph that is the entire
        // counterplay to that shot.
        DirectX::XMFLOAT3 aimPoint = target;
        // Snipers fire at 2.2x, matching the speedMultiplier the caller passes
        // to SpawnHostileProjectile for that archetype -- a faster round needs
        // proportionally less lead.
        const float leadSpeed = projectileSpeed * (IsSniper() ? 2.2f : 1.0f);
        if (leadSpeed > 0.001f) {
            for (int i = 0; i < 2; ++i) {
                const float dx = aimPoint.x - origin.x;
                const float dy = aimPoint.y - origin.y;
                const float dz = aimPoint.z - origin.z;
                const float t =
                    std::sqrt(dx * dx + dy * dy + dz * dz) / leadSpeed;
                aimPoint = { target.x + targetVelocity.x * t,
                             target.y + targetVelocity.y * t,
                             target.z + targetVelocity.z * t };
            }
        }

        XMVECTOR aim = XMLoadFloat3(&aimPoint) - XMLoadFloat3(&origin);
        if (XMVectorGetX(XMVector3LengthSq(aim)) < 1e-5f) return false;

        auto randomSigned = [] {
            return ((float)std::rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        };
        // Slight human aim error. Bursts remain dangerous without becoming
        // four perfectly accurate automatic turrets. The sniper spent five
        // seconds lining the shot up on a visible beam, so it gets a much
        // tighter cone -- the telegraph is the counterplay, not bad aim.
        float spread = IsSniper() ? 0.004f : 0.018f;
        float verticalSpread = IsSniper() ? 0.003f : 0.012f;
        // Marines miss on purpose: allies that shot as well as bandits
        // trivialized fights the player is supposed to carry. Only the cone is
        // widened -- damage is untouched, so a landed hit still does the normal
        // 20 and five connected hits still kill.
        //
        // The offset below is added to a non-normalized aim vector, so a fixed
        // spread shrinks with range -- marines would spray point-blank and
        // tighten up at distance, backwards from how bad aim reads. Scaling by
        // the range makes it a true angular cone that holds the same hit rate
        // at every distance, which is what makes a shots-per-kill budget mean
        // anything.
        if (faction == Faction::Marine) {
            // Hit chance falls with the square of the cone width, so this is
            // the one number that sets how long a marine-vs-bandit firefight
            // runs. Cut from 6.0: at that width a squad could trade fire with a
            // bandit group for most of a minute without either side dropping,
            // which read as two mobs standing in a field rather than a
            // contact. 3.5 is roughly three times the hit rate -- half-angle
            // 0.018*3.5 = 0.063 rad, about 3.6 degrees -- while staying far
            // wider than the player's near-zero cone, which is what keeps the
            // fight the player's to carry.
            constexpr float kMarineSpreadScale = 3.5f;
            const float range = std::sqrt(
                XMVectorGetX(XMVector3LengthSq(aim)));
            spread *= kMarineSpreadScale * range;
            verticalSpread *= kMarineSpreadScale * range;
        }
        aim += XMVectorSet(randomSigned() * spread,
                           randomSigned() * verticalSpread,
                           randomSigned() * spread, 0.0f);
        XMStoreFloat3(&direction, XMVector3Normalize(aim));

        if (turretGunner) {
            if (!mountedFiring_) {
                mountedFiring_ = true;
                attackEventPending_ = true;
            }
            fireCooldown = 0.12f;
            return true;
        }

        // Single-shot loadouts bypass the burst machinery: one trigger pull, then
        // a long recovery. Bolt cycling and shell pumping are what keeps them
        // from out-damaging the rifle despite hitting far harder per shot.
        // Single shots get the pose too, but only briefly: what follows is a
        // bolt cycle or a pump, not more firing, so it reads as the one round
        // going out rather than as a volley.
        if (IsSniper()) {
            attackEventPending_ = true;
            fireCooldown = 4.5f + ((float)std::rand() / RAND_MAX) * 2.5f;
            firingTimer_ = 0.3f;
            return true;
        }
        if (IsShotgunner()) {
            attackEventPending_ = true;
            fireCooldown = 1.5f + ((float)std::rand() / RAND_MAX) * 1.1f;
            preparingShot_ = false;
            stationaryAimTime_ = 0.0f;
            firingTimer_ = 0.3f;
            return true;
        }

        if (burstShotsRemaining <= 0) {
            // Longer volleys: 5-8 rounds. The between-burst pause is unchanged,
            // so the rifle leans harder on suppressing bursts the player has to
            // wait out rather than on firing more often.
            burstShotsRemaining = 5 + std::rand() % 4;
            attackEventPending_ = true;
        }
        --burstShotsRemaining;
        if (burstShotsRemaining > 0) {
            fireCooldown = 0.11f + ((float)std::rand() / RAND_MAX) * 0.12f;
            // Hold the firing pose past the gap to the next round, so a burst
            // reads as one continuous action instead of flickering back to the
            // idle between shots. The longest gap is 0.23s; the margin covers
            // it and drops out on its own once the volley stops.
            firingTimer_ = 0.35f;
        } else {
            fireCooldown = 1.2f + ((float)std::rand() / RAND_MAX) * 2.8f;
            preparingShot_ = false;
            stationaryAimTime_ = 0.0f;
            // Shorter than the mid-burst hold: the round still needs to read as
            // fired, but the reload below is queued on this same frame and the
            // firing pose outranks it, so a long tail here would just delay it.
            firingTimer_ = 0.15f;
            // The volley that just ended emptied the magazine, and the pause
            // before the next one is the window the reload plays in. Requested
            // rather than started here: the clip only reads while the body is
            // standing, and this fires whether or not it is, so UpdateLocomotion
            // decides. It stays pending until then, so a bandit that breaks
            // cover mid-burst reloads once it settles rather than losing it.
            reloadPending_ = kReloadEnabled;
        }
        return true;
    }

    // Weapon frame is seated on the posed trigger hand (see
    // UpdateGunFromHands, run at the end of every ApplyGunIK path), so the gun
    // travels with the arms -- inheriting arm swing, spine twist and
    // locomotion bob -- instead of floating at an independently computed spot.
    // Its orientation stays on the aimed body angles the mesh was authored
    // against, which is what keeps the weapon reading upright and untilted.
    DirectX::XMMATRIX GunWorldMatrix() const {
        using namespace DirectX;
        if (!HasGunPose()) return XMMatrixIdentity();
        return XMLoadFloat4x4(&gunWorld_);
    }

    // Where the gun was on the last frame it drew. Death hides the held gun
    // (HasGunPose needs !dead_) without touching gunWorld_, so this is the pose
    // to drop it from. False when the gun was never posed.
    bool LastGunWorldMatrix(DirectX::XMMATRIX& out) const {
        if (handBone_ < 0 ||
            static_cast<size_t>(handBone_) >= poseGlobals_.size())
            return false;
        out = DirectX::XMLoadFloat4x4(&gunWorld_);
        return true;
    }

    void SyncRagdoll() {
        using namespace DirectX;
        if (!dead_ || ragdollId_ == UINT32_MAX || deathGlobals_.empty()) return;
        std::vector<AuthoredRagdollPose> pose;
        if (!g_destruction.GetAuthoredRagdollPose(ragdollId_, pose)) return;

        const size_t count = model.skeleton.BoneCount();
        std::vector<XMFLOAT4X4> globals(count);
        std::vector<uint8_t> driven(count, 0);
        const XMMATRIX inverseWorld = XMMatrixInverse(nullptr, XMLoadFloat4x4(&deathWorld_));
        for (const AuthoredRagdollPose& body : pose) {
            const int bone = model.skeleton.Find(body.bone);
            if (bone < 0 || static_cast<size_t>(bone) >= bodyLocal_.size()) continue;
            // Box3D stores a rigid transform, so decomposition at spawn discarded
            // the FBX centimetres-to-metres scale. Restore that scale before
            // converting the body back into model space; otherwise inverseWorld
            // expands every driven bone basis by 100x.
            const XMMATRIX scaledBodyWorld =
                XMMatrixScaling(modelScale, modelScale, modelScale) *
                XMLoadFloat4x4(&body.bodyTransform);
            const XMMATRIX recovered = scaledBodyWorld * inverseWorld;
            XMVECTOR scale, rotation, translation;
            if (!XMMatrixDecompose(&scale, &rotation, &translation, recovered)) continue;
            XMStoreFloat4x4(&globals[bone],
                XMMatrixRotationQuaternion(rotation) * XMMatrixTranslationFromVector(translation));
            driven[bone] = 1;
        }

        for (size_t bone = 0; bone < count; ++bone) {
            if (driven[bone]) continue;
            const int parent = model.skeleton.parent[bone];
            if (parent < 0) {
                globals[bone] = deathGlobals_[bone];
                continue;
            }
            const XMMATRIX deathLocal = XMLoadFloat4x4(&deathGlobals_[bone]) *
                XMMatrixInverse(nullptr, XMLoadFloat4x4(&deathGlobals_[parent]));
            XMStoreFloat4x4(&globals[bone], deathLocal * XMLoadFloat4x4(&globals[parent]));
        }

        paletteCPU_.resize(count);
        for (size_t bone = 0; bone < count; ++bone) {
            const XMMATRIX skin = XMLoadFloat4x4(&model.skeleton.offset[bone]) *
                                  XMLoadFloat4x4(&globals[bone]);
            XMStoreFloat4x4(&paletteCPU_[bone], XMMatrixTranspose(skin));
        }
    }

    bool Shoot(const DirectX::XMFLOAT3& start, const DirectX::XMFLOAT3& end,
               const DirectX::XMFLOAT3& direction, float radius,
               DirectX::XMFLOAT3* hitPoint = nullptr,
               bool* headshot = nullptr,
               float bodyDamage = 20.0f,
               bool allowHeadshotKill = true,
               bool fromPlayer = false) {
        if (dead_ || !visible) return false;
        DirectX::XMFLOAT3 impact;
        bool hitHead = false;
        std::string hitBody;
        if (!BlocksProjectile(start, end, radius, &impact, &hitHead,
                              &hitBody)) return false;
        if (hitPoint) *hitPoint = impact;
        if (headshot) *headshot = hitHead;
        // Standard rifle balance: one headshot, exactly five body hits from
        // full 100 health.
        const bool lethalHeadshot = hitHead && allowHeadshotKill;
        const float appliedDamage = ScaleIncomingDamage(
            lethalHeadshot ? health : bodyDamage, lethalHeadshot);
        health -= appliedDamage;
        RegisterThreat(appliedDamage);
        if (health <= 0.0f)
            Kill(direction, impact, 1.0f, false,
                 RagdollImpactSource::Bullet, hitBody, fromPlayer);
        return true;
    }

    bool HitByHarpoon(const DirectX::XMFLOAT3& start,
                      const DirectX::XMFLOAT3& end,
                      const DirectX::XMFLOAT3& direction, float radius,
                      DirectX::XMFLOAT3* hitPoint = nullptr,
                      std::string* hitBone = nullptr,
                      bool fromPlayer = false) {
        if (dead_ || !visible) return false;
        DirectX::XMFLOAT3 impact;
        std::string struckBody;
        if (!BlocksProjectile(start, end, radius, &impact, nullptr,
                              &struckBody)) return false;
        if (hitPoint) *hitPoint = impact;
        if (hitBone) *hitBone = struckBody;
        const float remainingHealth = health;
        health = 0.0f;
        RegisterThreat(remainingHealth);
        // Harpoons always transition directly into the authored physics pose.
        // The projectile attachment takes ownership of movement immediately.
        Kill(direction, impact, 6.0f, false,
             RagdollImpactSource::Harpoon, struckBody, fromPlayer);
        return true;
    }

    bool BlocksProjectile(const DirectX::XMFLOAT3& start,
                          const DirectX::XMFLOAT3& end, float radius,
                          DirectX::XMFLOAT3* hitPoint = nullptr,
                          bool* headshot = nullptr,
                          std::string* hitBone = nullptr) const {
        using namespace DirectX;
        if (dead_ || !visible) return false;
        const XMVECTOR a = XMLoadFloat3(&start);
        const XMVECTOR b = XMLoadFloat3(&end);
        const XMVECTOR ab = b - a;
        const float lengthSq = XMVectorGetX(XMVector3LengthSq(ab));
        float bestT = FLT_MAX;
        std::string bestBone;

        auto sphereT = [&](FXMVECTOR center, float sphereRadius, float& t) {
            t = lengthSq > 1e-6f
                ? XMVectorGetX(XMVector3Dot(center - a, ab)) / lengthSq : 0.0f;
            t = (std::max)(0.0f, (std::min)(1.0f, t));
            const float expanded = sphereRadius + radius;
            return XMVectorGetX(XMVector3LengthSq(a + ab*t - center)) <=
                   expanded * expanded;
        };
        auto boxT = [&](FXMMATRIX shapeWorld, const XMFLOAT3& half, float& t) {
            const XMMATRIX inverse = XMMatrixInverse(nullptr, shapeWorld);
            XMFLOAT3 localStart, localEnd;
            XMStoreFloat3(&localStart, XMVector3TransformCoord(a, inverse));
            XMStoreFloat3(&localEnd, XMVector3TransformCoord(b, inverse));
            const float s[3] = { localStart.x, localStart.y, localStart.z };
            const float d[3] = { localEnd.x-localStart.x,
                                 localEnd.y-localStart.y,
                                 localEnd.z-localStart.z };
            const float h[3] = { half.x+radius, half.y+radius, half.z+radius };
            float lo = 0.0f, hi = 1.0f;
            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(d[axis]) < 1e-6f) {
                    if (s[axis] < -h[axis] || s[axis] > h[axis]) return false;
                    continue;
                }
                float t0 = (-h[axis]-s[axis])/d[axis];
                float t1 = ( h[axis]-s[axis])/d[axis];
                if (t0 > t1) std::swap(t0, t1);
                lo = (std::max)(lo, t0); hi = (std::min)(hi, t1);
                if (lo > hi) return false;
            }
            t = lo;
            return true;
        };
        auto capsuleT = [&](FXMVECTOR c0, FXMVECTOR c1,
                            float capsuleRadius, float& t) {
            const XMVECTOR v = c1-c0;
            const XMVECTOR w = a-c0;
            const float aa = lengthSq;
            const float bb = XMVectorGetX(XMVector3Dot(ab, v));
            const float cc = XMVectorGetX(XMVector3Dot(v, v));
            const float dd = XMVectorGetX(XMVector3Dot(ab, w));
            const float ee = XMVectorGetX(XMVector3Dot(v, w));
            const float denom = aa*cc-bb*bb;
            float shotT = denom > 1e-6f ? (bb*ee-cc*dd)/denom : 0.0f;
            shotT = (std::max)(0.0f, (std::min)(1.0f, shotT));
            float limbT = cc > 1e-6f ? (bb*shotT+ee)/cc : 0.0f;
            limbT = (std::max)(0.0f, (std::min)(1.0f, limbT));
            if (aa > 1e-6f)
                shotT = (std::max)(0.0f, (std::min)(1.0f,
                    (bb*limbT-dd)/aa));
            const XMVECTOR shotPoint = a+ab*shotT;
            const XMVECTOR limbPoint = c0+v*limbT;
            const float expanded = capsuleRadius+radius;
            if (XMVectorGetX(XMVector3LengthSq(shotPoint-limbPoint)) >
                expanded*expanded) return false;
            t = shotT;
            return true;
        };

        for (const RagdollBodySpec& body : model.ragdoll.bodies) {
            const int bone = model.skeleton.Find(body.bone);
            if (bone < 0 || (size_t)bone >= poseGlobals_.size()) continue;
            const XMMATRIX scaledBoneWorld =
                XMLoadFloat4x4(&poseGlobals_[bone]) * WorldMatrix();
            XMVECTOR boneScale, boneRotation, boneTranslation;
            if (!XMMatrixDecompose(&boneScale, &boneRotation,
                                   &boneTranslation, scaledBoneWorld)) continue;
            const XMMATRIX boneWorld = XMMatrixRotationQuaternion(
                XMQuaternionNormalize(boneRotation)) *
                XMMatrixTranslationFromVector(boneTranslation);
            for (const RagdollShapeSpec& shape : body.shapes) {
                const XMMATRIX shapeWorld =
                    XMMatrixRotationQuaternion(XMLoadFloat4(&shape.rotation)) *
                    XMMatrixTranslation(shape.center.x, shape.center.y,
                                        shape.center.z) * boneWorld;
                float t = FLT_MAX;
                bool hit = false;
                if (shape.type == RagdollShapeType::Box) {
                    hit = boxT(shapeWorld, shape.halfExtent, t);
                } else if (shape.type == RagdollShapeType::Sphere) {
                    hit = sphereT(XMVector3TransformCoord(XMVectorZero(),
                                  shapeWorld), shape.radius, t);
                } else {
                    hit = capsuleT(
                        XMVector3TransformCoord(XMVectorSet(0,-shape.length*0.5f,0,1),
                                                shapeWorld),
                        XMVector3TransformCoord(XMVectorSet(0, shape.length*0.5f,0,1),
                                                shapeWorld),
                        shape.radius, t);
                }
                if (hit && t < bestT) {
                    bestT = t;
                    bestBone = body.bone;
                }
            }
        }
        if (bestT == FLT_MAX) return false;
        if (hitPoint) XMStoreFloat3(hitPoint, a + ab*bestT);
        if (hitBone) *hitBone = bestBone;
        if (headshot) *headshot = bestBone.find("head") != std::string::npos;
        return true;
    }

    bool ApplyExplosion(const DirectX::XMFLOAT3& center, float radius,
                        float damage, float pushSpeed,
                        bool fromPlayer = false) {
        using namespace DirectX;
        if (dead_ || held_ || !visible || radius <= 0.0f) return false;
        const XMVECTOR blast = XMLoadFloat3(&center);
        const XMVECTOR body = XMVectorSet(
            position.x, position.y + footOffset + 1.0f, position.z, 0.0f);
        XMVECTOR away = body - blast;
        const float distance = XMVectorGetX(XMVector3Length(away));
        if (distance > radius) return false;

        if (distance < 0.001f) away = XMVectorSet(0.0f, 0.4f, 1.0f, 0.0f);
        away = XMVector3Normalize(away + XMVectorSet(0.0f, 0.35f, 0.0f, 0.0f));
        const float falloff = (std::max)(0.2f, 1.0f - distance / radius);
        const float appliedDamage = ScaleIncomingDamage(damage * falloff);
        health -= appliedDamage;
        RegisterThreat(appliedDamage);

        XMFLOAT3 direction;
        XMStoreFloat3(&direction, away);
        if (health <= 0.0f) {
            XMFLOAT3 impactPosition;
            XMStoreFloat3(&impactPosition, body);
            Kill(direction, impactPosition, 1.0f, false,
                 RagdollImpactSource::Explosion, "pelvis", fromPlayer);
        } else {
            knockbackVelocity_.x += direction.x * pushSpeed * falloff;
            knockbackVelocity_.z += direction.z * pushSpeed * falloff;
            const float velocitySq =
                knockbackVelocity_.x * knockbackVelocity_.x +
                knockbackVelocity_.z * knockbackVelocity_.z;
            const float maxSpeed = pushSpeed * 1.25f;
            if (velocitySq > maxSpeed * maxSpeed) {
                const float scale = maxSpeed / std::sqrt(velocitySq);
                knockbackVelocity_.x *= scale;
                knockbackVelocity_.z *= scale;
            }
        }
        return true;
    }

    bool ApplyDebrisImpact(const DestructionDebrisHazard& debris,
                           DirectX::XMFLOAT3* hitPoint = nullptr,
                           bool fromPlayer = false) {
        using namespace DirectX;
        if (dead_ || held_ || !visible || debrisHitCooldown_ > 0.0f) return false;

        const float bodyBottom = position.y + footOffset + 0.10f;
        const float bodyTop = bodyBottom + 1.85f;
        if (debris.worldMax.y < bodyBottom || debris.worldMin.y > bodyTop) return false;
        const float closestX = (std::max)(debris.worldMin.x,
            (std::min)(position.x, debris.worldMax.x));
        const float closestZ = (std::max)(debris.worldMin.z,
            (std::min)(position.z, debris.worldMax.z));
        const float dx = position.x - closestX;
        const float dz = position.z - closestZ;
        constexpr float bodyRadius = 0.62f;
        if (dx * dx + dz * dz > bodyRadius * bodyRadius) return false;

        const float speed = std::sqrt(
            debris.velocity.x * debris.velocity.x +
            debris.velocity.y * debris.velocity.y +
            debris.velocity.z * debris.velocity.z);
        if (speed < 2.5f) return false;

        XMVECTOR directionVector = XMLoadFloat3(&debris.velocity);
        if (XMVectorGetX(XMVector3LengthSq(directionVector)) < 0.0001f)
            directionVector = XMVectorSet(0.0f, 0.2f, 1.0f, 0.0f);
        directionVector = XMVector3Normalize(directionVector);
        XMFLOAT3 direction;
        XMStoreFloat3(&direction, directionVector);

        XMFLOAT3 impact(closestX,
            (std::max)(bodyBottom, (std::min)(debris.worldCenter.y, bodyTop)),
            closestZ);
        if (hitPoint) *hitPoint = impact;

        const float damage = ScaleIncomingDamage(
            debris.lethalImpact ? health :
                (std::min)(80.0f, (std::max)(8.0f,
                    (speed - 2.5f) * 7.0f +
                    std::sqrt((std::max)(0.05f, debris.mass)) * 3.0f)),
            debris.lethalImpact);
        health -= damage;
        RegisterThreat(damage);
        debrisHitCooldown_ = 0.45f;
        if (health <= 0.0f) {
            Kill(direction, impact, 1.0f, debris.lethalImpact,
                 RagdollImpactSource::Debris, {}, fromPlayer);
        } else {
            const float push = (std::min)(7.0f, speed * 0.65f);
            knockbackVelocity_.x += direction.x * push;
            knockbackVelocity_.z += direction.z * push;
        }
        return true;
    }

    // Who gets credit for this body on the wire. kNoKiller until something
    // kills it; kLocalPlayerKiller when this machine's player did, which the
    // host rewrites to a real id before publishing. A client reads it back off
    // the snapshot and pays out only when it names them.
    static constexpr uint8_t kNoKiller = 0xFF;
    static constexpr uint8_t kLocalPlayerKiller = 0xFE;
    uint8_t netKiller = kNoKiller;

    bool Dead() const { return dead_; }
    uint32_t RagdollId() const { return ragdollId_; }

    // `fromPlayer` is latched rather than applied, because the kill arrives
    // seconds later in UpdateBurning with nothing left in scope to ask. It
    // only ever climbs: a body the player set alight stays credited even if an
    // ownerless patch tops the burn up afterwards.
    //
    // Fire that spreads body-to-body carries no credit at all -- the patch
    // SpawnCarriedFire drops has no owner, so the chain of custody ends after
    // one hop, and the player is not paid for a blaze that spread on its own.
    bool Ignite(float duration = 5.5f, bool fromPlayer = false) {
        if (dead_ || !visible || duration <= 0.0f) return false;
        const bool newlyIgnited = burnTime_ <= 0.0f;
        burnTime_ = (std::max)(burnTime_, duration);
        if (fromPlayer) burnCreditedToPlayer_ = true;
        if (newlyIgnited) burnSpreadCooldown_ = 0.55f;
        return newlyIgnited;
    }

    bool UpdateBurning(float dt, float damagePerSecond) {
        if (dead_ || burnTime_ <= 0.0f || dt <= 0.0f) return false;
        burnTime_ = (std::max)(0.0f, burnTime_ - dt);
        burnSpreadCooldown_ -= dt;
        health -= ScaleIncomingDamage((std::max)(0.0f, damagePerSecond) * dt);
        if (health > 0.0f) return false;
        const DirectX::XMFLOAT3 upward{ 0.0f, 1.0f, 0.0f };
        const DirectX::XMFLOAT3 impact{
            position.x, position.y + footOffset + 1.0f, position.z };
        Kill(upward, impact, 0.35f, false, RagdollImpactSource::Bullet, {},
             burnCreditedToPlayer_);
        burnTime_ = 0.0f;
        return true;
    }

    bool Burning() const { return !dead_ && burnTime_ > 0.0f; }
    float BurnFraction() const {
        return (std::min)(1.0f, burnTime_ / 5.5f);
    }
    bool ConsumeBurnSpreadEvent() {
        if (!Burning() || burnSpreadCooldown_ > 0.0f) return false;
        burnSpreadCooldown_ = 1.05f;
        return true;
    }

    bool KillFromRotor(const DirectX::XMFLOAT3& direction,
                       const DirectX::XMFLOAT3& impact) {
        if (dead_ || !visible) return false;
        Kill(direction, impact, 22.0f, true,
             RagdollImpactSource::Debris);
        return true;
    }

    bool ConsumeSpottedEvent() {
        const bool pending = spottedEventPending_;
        spottedEventPending_ = false;
        return pending;
    }

    bool ConsumeAttackEvent() {
        const bool pending = attackEventPending_;
        attackEventPending_ = false;
        return pending;
    }

    // Hands the payout credit out with the event rather than through a second
    // getter: one consume means the two can never disagree about which death
    // they are describing.
    bool ConsumeDeathEvent(bool* playerCredit = nullptr) {
        const bool pending = deathEventPending_;
        if (playerCredit) *playerCredit = pending && killCreditPending_;
        deathEventPending_ = false;
        killCreditPending_ = false;
        return pending;
    }

    // Uploads this frame's palette and draws every skinned primitive. Mirrors
    // DrawSceneNode's material setup but routes through g_meshShader.Draw with
    // the palette + skin SRV addresses so the mesh shader skins on the GPU.
    void Draw(ShaderDX12& shader, const DirectX::XMMATRIX& view,
              const DirectX::XMMATRIX& proj, const DirectX::XMMATRIX& lightSpace) {
        using namespace DirectX;
        if (!CanRender()) return;
        const D3D12_GPU_VIRTUAL_ADDRESS paletteAddr = UploadPalette();
        const D3D12_GPU_VIRTUAL_ADDRESS prevPaletteAddr = UploadPreviousPalette();

        // Mesh gets an extra independent rotation (debug) pre-applied in its own
        // local space so it can be aligned against the skeleton overlay.
        const XMMATRIX world = MeshWorldMatrix();
        const XMMATRIX prevWorld = XMLoadFloat4x4(&previousMeshWorld_);
        shader.SetMatrices(world, view, proj, lightSpace, {}, prevWorld);

        // Corpses skip meshlet culling because their bind-pose bounds no longer
        // describe the ragdoll pose, but that costs a full meshlet dispatch per
        // body. Only pay it for bodies near the camera, where a dropped limb is
        // actually visible; distant corpses keep the coarse frustum test. The
        // camera sits at the translation of the inverted view matrix.
        bool nearCorpse = false;
        if (dead_) {
            const XMMATRIX inverseView = XMMatrixInverse(nullptr, view);
            const XMVECTOR eye = inverseView.r[3];
            const XMVECTOR toBody = XMVectorSet(position.x, position.y,
                                                position.z, 1.0f) - eye;
            constexpr float kNoCullDistance = 30.0f;
            nearCorpse = XMVectorGetX(XMVector3LengthSq(toBody)) <=
                         kNoCullDistance * kNoCullDistance;
        }

        for (const auto& prim : model.node->mesh->primitives) {
            if (prim.vbv.BufferLocation == 0 || !prim.skinBuffer) continue;
            shader.Use(false);
            if (prim.material) {
                XMFLOAT3 color(prim.material->baseColorFactor.x,
                               prim.material->baseColorFactor.y,
                               prim.material->baseColorFactor.z);
                shader.SetObjectMaterial(color,
                    prim.material->baseColorTexture != nullptr,
                    prim.material->normalTexture != nullptr,
                    prim.material->metallicFactor, prim.material->roughnessFactor,
                    prim.material->baseColorTexture.Get(),
                    prim.material->normalTexture.Get(),
                    prim.material->metallicRoughnessTexture.Get(),
                    prim.material->roughnessOnlyTexture, 1.0f,
                    prim.material->alphaCutout,
                    prim.material.get(), prim.material->alphaFromLuminance,
                    prim.material->ambientScale,
                    prim.material->occlusionStrength,
                    prim.material->normalYSign,
                    prim.material->viewFillStrength);
            } else {
                shader.SetObjectColor(XMFLOAT3(0.7f, 0.7f, 0.72f));
            }

            const D3D12_GPU_VIRTUAL_ADDRESS descA = prim.meshletDescBuffer ? prim.meshletDescBuffer->GetGPUVirtualAddress() : 0;
            const D3D12_GPU_VIRTUAL_ADDRESS boundsA = prim.meshletBoundsBuffer ? prim.meshletBoundsBuffer->GetGPUVirtualAddress() : 0;
            const D3D12_GPU_VIRTUAL_ADDRESS vidxA = prim.meshletVertexIndexBuffer ? prim.meshletVertexIndexBuffer->GetGPUVirtualAddress() : 0;
            const D3D12_GPU_VIRTUAL_ADDRESS triA = prim.meshletTriangleBuffer ? prim.meshletTriangleBuffer->GetGPUVirtualAddress() : 0;
            if (g_meshShader.CanDraw(prim.meshletCount, descA, boundsA, vidxA, triA)) {
                // SetObjectMaterial may select a different root signature than
                // the previous scene mesh used. The mesh PSO must follow that
                // per-primitive selection or bindless texture indices are read
                // by the legacy pixel-shader variant (and vice versa).
                g_meshShader.SetBindlessActive(shader.BindlessDrawActive());
                g_meshShader.Draw(prim.vbv, (UINT)(prim.vertices.size() / 12), prim.indexCount,
                    prim.meshletCount, descA, boundsA, vidxA, triA,
                    paletteAddr, prim.skinBuffer->GetGPUVirtualAddress(),
                    prim.material && prim.material->doubleSided,
                    // A ragdoll can be carried far from deathWorld_ while its
                    // meshlet bounds remain in the bind pose. Those stale bounds
                    // can reject an on-screen corpse, especially after a harpoon
                    // pins it to a wall. Draw every posed meshlet for bodies
                    // within kNoCullDistance; beyond that a missing cluster is
                    // too small to read, so the coarse frustum test is kept.
                    !dead_, nearCorpse,
                    prevPaletteAddr);
            }
            shader.NextDrawCall();
        }
    }

    const std::vector<DirectX::XMFLOAT4X4>& Palette() const { return paletteCPU_; }

private:
    // `playerCredit` says the local player caused this death, and it defaults
    // to false on purpose: a route nobody thought about pays nothing rather
    // than paying wrongly. Only the call sites that can actually see a player
    // behind the damage pass true.
    void Kill(const DirectX::XMFLOAT3& impulseDirection,
              const DirectX::XMFLOAT3& impactPosition,
              float impulseMultiplier = 1.0f,
              bool lethalImpact = false,
              RagdollImpactSource source = RagdollImpactSource::Bullet,
              const std::string& struckBone = {},
              bool playerCredit = false) {
        using namespace DirectX;
        dead_ = true;
        held_ = false;
        // A burst that ran dry on the frame the body died leaves a reload
        // queued; the ragdoll drives the pose from here, so drop it rather
        // than let a revived or re-held actor play it much later.
        reloadPending_ = false;
        reloadPlaying_ = false;
        throwPlaying_ = false;
        firingTimer_ = 0.0f;
        deathEventPending_ = true;
        killCreditPending_ = playerCredit;
        // Sticky, unlike the one-shot above, because the host publishes this
        // long after the death event has been consumed. kLocalPlayerKiller is a
        // placeholder: SkinnedEnemy has no business knowing net player ids, so
        // the host swaps it for its own id as it fills the snapshot.
        if (playerCredit) netKiller = kLocalPlayerKiller;
        laserCharge_ = 0.0f;
        std::vector<XMFLOAT4X4> globals = poseGlobals_;
        if (globals.empty()) anim.ComputeGlobalMatrices(model.skeleton, globals);
        deathGlobals_ = globals;
        bodyLocal_.assign(model.skeleton.BoneCount(), XMFLOAT4X4{});
        XMStoreFloat4x4(&deathWorld_, WorldMatrix());
        std::vector<AuthoredRagdollBody> bodies;
        bodies.reserve(model.ragdoll.bodies.size());
        for (const RagdollBodySpec& spec : model.ragdoll.bodies) {
            const int bone = model.skeleton.Find(spec.bone);
            if (bone < 0 || (size_t)bone >= globals.size()) continue;
            const XMMATRIX bodyWorld = XMLoadFloat4x4(&globals[bone]) * WorldMatrix();
            XMVECTOR scale, rotation, translation;
            if (!XMMatrixDecompose(&scale, &rotation, &translation, bodyWorld)) continue;
            AuthoredRagdollBody body;
            body.name = spec.bone;
            body.shapes = spec.shapes;
            body.targetMass = 78.0f * spec.massFraction;
            XMStoreFloat3(&body.position, translation);
            XMStoreFloat4(&body.rotation, XMQuaternionNormalize(rotation));

            if (previousPoseDt_ > 1e-4f && previousPoseDt_ <= 0.1f &&
                (size_t)bone < previousPoseGlobals_.size()) {
                const XMMATRIX previousWorld =
                    XMLoadFloat4x4(&previousPoseGlobals_[bone]) *
                    XMLoadFloat4x4(&previousPoseWorld_);
                XMVECTOR previousScale, previousRotation, previousTranslation;
                if (XMMatrixDecompose(&previousScale, &previousRotation,
                                      &previousTranslation, previousWorld)) {
                    XMVECTOR linear = (translation - previousTranslation) /
                                      previousPoseDt_;
                    const float linearSpeed = XMVectorGetX(XMVector3Length(linear));
                    if (linearSpeed > 7.0f) linear *= 7.0f / linearSpeed;
                    XMStoreFloat3(&body.linearVelocity, linear);

                    XMVECTOR delta = XMQuaternionNormalize(XMQuaternionMultiply(
                        XMQuaternionInverse(previousRotation), rotation));
                    if (XMVectorGetW(delta) < 0.0f) delta = XMVectorNegate(delta);
                    const float w = (std::max)(-1.0f, (std::min)(1.0f,
                        XMVectorGetW(delta)));
                    float angle = 2.0f * std::acos(w);
                    const float sinHalf = std::sqrt((std::max)(0.0f, 1.0f - w*w));
                    XMVECTOR angular = XMVectorZero();
                    if (sinHalf > 1e-4f)
                        angular = XMVectorSet(
                            XMVectorGetX(delta) / sinHalf,
                            XMVectorGetY(delta) / sinHalf,
                            XMVectorGetZ(delta) / sinHalf, 0.0f) *
                            (angle / previousPoseDt_);
                    const float angularSpeed = XMVectorGetX(XMVector3Length(angular));
                    if (angularSpeed > 20.0f) angular *= 20.0f / angularSpeed;
                    XMStoreFloat3(&body.angularVelocity, angular);
                }
            }
            bodies.push_back(body);
        }
        RagdollImpact impact;
        impact.source = source;
        impact.bodyName = struckBone;
        impact.position = impactPosition;
        impact.direction = impulseDirection;
        impact.impulseMultiplier = impulseMultiplier;
        impact.lethalHazard = lethalImpact;
        ragdollId_ = g_destruction.SpawnAuthoredRagdoll(
            bodies, model.ragdoll.constraints, impact);
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> palette_[FRAME_COUNT];
    void* mapped_[FRAME_COUNT] = {};
    Microsoft::WRL::ComPtr<ID3D12Resource> palettePrevious_[FRAME_COUNT];
    void* mappedPrevious_[FRAME_COUNT] = {};
    UINT  paletteBytes_ = 0;
    std::vector<DirectX::XMFLOAT4X4> paletteCPU_;
    AnimationInstance upperBodyAnim_;
    LocomotionBlendSpace locomotion_;
    bool directionalMoving_ = false;
    // Where the body was when locomotion was last evaluated, and how long it
    // has covered no ground since. Shared by the AI and networked paths: both
    // ask for a gait from intent, and only the position says whether the body
    // is really going anywhere.
    DirectX::XMFLOAT3 locomotionPreviousSample_{};
    bool  hasLocomotionHistory_ = false;
    float stillTime_ = 0.0f;

    // Off for now: the firing pose carries the whole engagement and the reload
    // cut into it at every burst end. The clip is still loaded and the state
    // below still works -- flip this back to re-enable it.
    static constexpr bool kReloadEnabled = false;

    // Set when a burst runs dry, cleared once the reload has actually played.
    bool reloadPending_ = false;
    bool reloadPlaying_ = false;
    // The throw animation, driven exactly like the reload above but triggered
    // from outside: the grenade is spawned by the caller that rolled for it,
    // and this is the body catching up with the round already in the air.
    bool throwPlaying_ = false;
    // Seconds of firing pose still owed, counted down every frame. A timer
    // rather than a flag because rounds arrive discretely while the pose has to
    // span the gaps between them.
    float firingTimer_ = 0.0f;

    // Runs the throw to its end once started, over a moving body and ahead of
    // everything else. A grenade leaves the hand at one instant, so unlike the
    // reload this cannot be deferred until the actor happens to stand still --
    // by then the grenade is already mid-air and the wind-up reads as a body
    // miming a throw it did not make.
    bool UpdateThrowPose(float dt) {
        if (!throwPlaying_) return false;
        if (dead_ || held_ || rappelling_) { throwPlaying_ = false; return false; }
        const AnimationClip* toss = model.FindClip("ThrowGrenade");
        if (!toss) { throwPlaying_ = false; return false; }
        if (anim.clip != toss) { anim.Play(toss); anim.loop = false; }
        anim.Advance(dt);
        // Non-looping, so time saturates at the end rather than wrapping.
        if (anim.time < toss->duration) return true;
        // Play() leaves `loop` alone, so hand it back or every cycle after
        // this one sits frozen on its last frame.
        throwPlaying_ = false;
        anim.loop = true;
        return false;
    }

    // True while a shot is recent enough to still be reading as fired.
    //
    // Unlike the reload this is not gated on standing: a bandit firing as it
    // advances should still shoulder the rifle, so the caller runs this ahead
    // of the stillness test and the pose plays over a moving body.
    //
    // There is no authored crouch-fire clip, and with the procedural
    // pelvis-drop layer disabled (kProceduralCrouch) there is nothing left to
    // bend the standing Fire clip into a crouch -- it would just play
    // standing while crouchBlend_ silently lowered the muzzle, which reads as
    // a floating gun. CrouchIdleAim is the nearer authored pose instead:
    // still holding a weapon at the ready, just already crouched.
    bool UpdateFiringPose(float dt) {
        firingTimer_ = (std::max)(0.0f, firingTimer_ - dt);
        if (firingTimer_ <= 0.0f) return false;
        if (dead_ || held_ || rappelling_) return false;
        const AnimationClip* crouchIdle = crouchBlend_ > 0.5f
            ? model.FindClip("CrouchIdleAim") : nullptr;
        const AnimationClip* fire = crouchIdle ? crouchIdle : model.FindClip("Fire");
        if (!fire) return false;
        // Firing outranks a queued reload -- the magazine is not empty until
        // the volley actually stops -- but it must not eat it: the request
        // stays pending and plays once the timer runs out. Interrupting a
        // reload mid-clip hands `loop` back, since Play() leaves it alone and
        // the reload had cleared it.
        reloadPlaying_ = false;
        if (anim.clip != fire) anim.Play(fire);
        anim.loop = true;
        anim.Advance(dt);
        playedCrouchClip_ = crouchIdle != nullptr;
        return true;
    }

    // Picks the pose a standing body holds, and returns true once it has taken
    // the frame over -- the caller then leaves the gait alone entirely.
    //
    // Three poses, in priority order: the one-shot reload, then the shouldered
    // aim once the player is known about, then the lowered carry before that.
    // Awareness rather than distance decides between the two idles, so a bandit
    // that has heard something but not found it (Alert) is already up and
    // holding, which is what makes the lowered stance read as "hasn't seen you"
    // rather than as a range check.
    bool UpdateStandingPose(float dt) {
        const AnimationClip* reload = model.FindClip("Reload");
        if (reloadPlaying_ && reload && anim.clip == reload) {
            anim.Advance(dt);
            // Non-looping, so time saturates at the end rather than wrapping.
            if (anim.time < reload->duration) return true;
            // Play() does not touch `loop`, so the flag has to be handed back
            // or every cycle that follows would sit frozen on its last frame.
            reloadPlaying_ = false;
            anim.loop = true;
        } else if (reloadPending_ && reload && !dead_ && !held_ && !rappelling_) {
            reloadPending_ = false;
            reloadPlaying_ = true;
            anim.Play(reload);
            anim.loop = false;
            anim.Advance(dt);
            return true;
        }

        // Crouched and holding: the authored aiming-crouch pose beats both
        // standing idles whenever it is actually loaded. Gated well past the
        // ease-in (0.5, versus ApplyCrouch's own > 0) so a body only just
        // starting to crouch does not pop straight to the crouched clip
        // before the transition reads as one.
        if (const AnimationClip* crouchIdle = model.FindClip("CrouchIdleAim");
            crouchIdle && crouchBlend_ > 0.5f) {
            if (anim.clip != crouchIdle) { anim.Play(crouchIdle); anim.loop = true; }
            anim.Advance(dt);
            playedCrouchClip_ = true;
            return true;
        }

        // Only the undetected stance is new behaviour; everything else keeps
        // playing the aim idle the gun overlay was built against.
        const bool detected = awareness_ != AwarenessState::Patrol;
        const AnimationClip* relaxed = model.FindClip("IdleRelaxed");
        if (detected || !relaxed) return false;
        if (anim.clip != relaxed) { anim.Play(relaxed); anim.loop = true; }
        anim.Advance(dt);
        return true;
    }

    // Picks the crouch-walk clip for the dominant local-space travel
    // direction, mirroring the forward/backward/left/right convention
    // DirectionalLocomotion bakes the run blend space with. There is no
    // crouch run and no blending between the four -- just four discrete
    // cycles -- so the loudest axis wins outright rather than shading toward
    // a diagonal.
    static const char* CrouchWalkClipName(float localRight, float localForward) {
        if (std::abs(localForward) >= std::abs(localRight))
            return localForward >= 0.0f ? "CrouchWalkForward" : "CrouchWalkBackward";
        return localRight >= 0.0f ? "CrouchWalkRight" : "CrouchWalkLeft";
    }

    void UpdateLocomotion(float dt, const DirectX::XMFLOAT3& start,
                          float requestedSpeed, float fallbackPlaybackRate = 1.0f) {
        // Cleared here rather than at the top of ComputePose: this function
        // and everything it calls (UpdateFiringPose, UpdateStandingPose, and
        // this function's own crouch-walk branch below) are the only places
        // that can set it for an AI-driven or networked actor, and all of
        // them run once per frame ahead of ComputePose, so clearing at this
        // shared entry point cannot miss a frame the way clearing after the
        // fact could.
        playedCrouchClip_ = false;
        const float inverseDt = dt > 1e-5f ? 1.0f / dt : 0.0f;
        float vx = (position.x - start.x) * inverseDt;
        float vz = (position.z - start.z) * inverseDt;
        // What the steering asked for is not what the body did. Three things
        // run after Update and can cancel the move outright -- the ledge
        // revert, the prefab pushout and the vehicle pushout -- so an actor
        // pressed into a wall or stopped at the edge of a watchtower deck kept
        // a full walk cycle while its feet covered no ground. Frame-over-frame
        // displacement is measured from the position this Update started at
        // versus the one before, which is after all of those corrections.
        const bool still = MeasureStillness(dt, start);
        if (still) { vx = 0.0f; vz = 0.0f; }
        directionalMoving_ = vx * vx + vz * vz > 0.01f;

        // A standing body is the only one that can be showing a standing pose,
        // so the reload and the two idles are resolved before the blend space
        // is asked for a gait. Moving cancels a running reload outright rather
        // than letting it play on over a run: the clip is authored standing.
        // Firing is checked first and without the stillness gate, so a bandit
        // shooting on the move keeps the rifle up instead of dropping back to a
        // plain run. It also counts the timer down, so it has to run every
        // frame rather than only while standing.
        // Ahead of the firing pose: a thrower has committed to the grenade and
        // is not shooting during the wind-up, and the firing timer is often
        // still counting down from the burst that preceded the throw.
        if (UpdateThrowPose(dt)) return;
        if (UpdateFiringPose(dt)) return;
        if (still && UpdateStandingPose(dt)) return;
        if (reloadPlaying_) { reloadPlaying_ = false; anim.loop = true; }

        // Crouch-walking: four discrete authored cycles rather than the
        // directional blend space, so this is checked ahead of it. Only while
        // actually moving -- a still crouched body already took the
        // CrouchIdleAim branch in UpdateStandingPose above.
        if (!still && crouchBlend_ > 0.5f) {
            const float c = std::cos(yaw), s = std::sin(yaw);
            const float localRight = vx * c - vz * s;
            const float localForward = vx * s + vz * c;
            if (const AnimationClip* crouchWalk =
                    model.FindClip(CrouchWalkClipName(localRight, localForward))) {
                if (anim.clip != crouchWalk) anim.Play(crouchWalk);
                anim.loop = true;
                anim.Advance(dt);
                playedCrouchClip_ = true;
                return;
            }
        }

        const float c = std::cos(yaw), s = std::sin(yaw);
        const AnimationClip* pose = (model.authoredDirectional || directionalLocomotionIK)
            ? locomotion_.Update(dt, vx * c - vz * s, vx * s + vz * c, moveSpeed)
            : nullptr;
        if (pose) {
            if (anim.clip != pose) anim.Play(pose);
        } else {
            const float gaitSpeed = still ? 0.0f : requestedSpeed;
            PlayClip(gaitSpeed > moveSpeed * 1.2f ? "Run" :
                     gaitSpeed > 0.01f ? "Walk" : "Idle");
            anim.Advance(dt * (still ? 1.0f : fallbackPlaybackRate));
        }
    }

    // Speed under which a body counts as standing, and how long it has to stay
    // there before the legs stop. The hold keeps a walk from flickering off
    // during the frame a direction is reversed or a pushout briefly eats the
    // step, and keeps the gait starting on the frame the actor sets off rather
    // than one frame later.
    static constexpr float kStillSpeed = 0.2f;
    static constexpr float kStillHold = 0.12f;
    // Anything faster than this is a teleport, a rappel drop or a respawn, not
    // travel: the history restarts instead of reading as a sprint.
    static constexpr float kTeleportSpeed = 30.0f;

    // True once the body has actually been standing for kStillHold, whatever
    // the AI or the network says it is doing.
    bool MeasureStillness(float dt, const DirectX::XMFLOAT3& sample) {
        const float dx = sample.x - locomotionPreviousSample_.x;
        const float dz = sample.z - locomotionPreviousSample_.z;
        const bool hadHistory = hasLocomotionHistory_;
        locomotionPreviousSample_ = sample;
        hasLocomotionHistory_ = true;
        if (!hadHistory || dt <= 1e-5f) { stillTime_ = 0.0f; return false; }
        const float speed = std::sqrt(dx * dx + dz * dz) / dt;
        if (speed > kTeleportSpeed) { stillTime_ = 0.0f; return false; }
        if (speed > kStillSpeed) { stillTime_ = 0.0f; return false; }
        stillTime_ += dt;
        return stillTime_ >= kStillHold;
    }
    std::vector<float> upperBodyMask_;
    std::vector<DirectX::XMFLOAT4> gunPoseOffsets_;
    std::vector<DirectX::XMFLOAT4X4> poseGlobals_;
    std::vector<DirectX::XMFLOAT4X4> previousPoseGlobals_;
    float spineTwistCurrent_ = 0.0f;
    // World yaw/pitch the gun mesh renders at. Set by ApplyGunIK each frame to
    // match wherever the arm IK just placed the hands, so GunWorldMatrix never
    // computes a position independent of the actual arm pose.
    float gunYaw_ = 0.0f;
    float gunPitch_ = 0.0f;
    // Gun frame in world space, rebuilt from the posed hand bones after each
    // arm IK solve. Identity until the first ApplyGunIK; HasGunPose() gates
    // every read, and that requires the pose arrays to be populated.
    DirectX::XMFLOAT4X4 gunWorld_ = {};
    std::vector<DirectX::XMFLOAT4X4> deathGlobals_;
    std::vector<DirectX::XMFLOAT4X4> bodyLocal_;
    DirectX::XMFLOAT4X4 poseWorld_ = {};
    DirectX::XMFLOAT4X4 previousPoseWorld_ = {};
    DirectX::XMFLOAT4X4 previousMeshWorld_ = {};
    float previousPoseDt_ = 0.0f;
    DirectX::XMFLOAT4X4 deathWorld_ = {};
    DirectX::XMFLOAT3 knockbackVelocity_{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 coverTarget_{ 0.0f, 0.0f, 0.0f };
    float coverQueryCooldown_ = 0.0f;
    float coverTravelTime_ = 0.0f;
    float coverHoldTime_ = 0.0f;
    float stationaryAimTime_ = 0.0f;
    float burnTime_ = 0.0f;
    float burnSpreadCooldown_ = 0.0f;
    float mountedSightTime_ = 0.0f;
    float mountedLostSightTime_ = 0.0f;
    float debrisHitCooldown_ = 0.0f;
    std::vector<DirectX::XMFLOAT3> navigationPath_;
    size_t navigationWaypoint_ = 0;
    float navigationRepathTimer_ = 0.0f;
    DirectX::XMFLOAT3 navigationDestination_{};
    float laserCharge_ = 0.0f;
    DirectX::XMFLOAT3 laserTarget_{ 0.0f, 0.0f, 0.0f };
    bool preparingShot_ = false;
    bool spottedEventPending_ = false;
    bool attackEventPending_ = false;
    bool deathEventPending_ = false;
    // Rides alongside the death event and is cleared with it, so a payout can
    // never be read twice or read after the event has been consumed.
    bool killCreditPending_ = false;
    // Set when the local player personally lit this body, so the delayed burn
    // death can still be credited. Never cleared by Ignite: see the note there.
    bool burnCreditedToPlayer_ = false;
    uint32_t ragdollId_ = UINT32_MAX;
    int handBone_ = -1;
    int headBone_ = -1;
    bool dead_ = false;
    bool held_ = false;
    // On the dropship rope, descending. See BeginRappel/UpdateRappel.
    bool rappelling_ = false;
    float rappelReleaseTimer_ = 0.0f;
    bool mountedFiring_ = false;
    bool hasCoverTarget_ = false;
    bool inCover_ = false;

    // Perception / patrol state. See AwarenessState for the meaning of each.
    AwarenessState awareness_ = AwarenessState::Patrol;
    DirectX::XMFLOAT3 lastKnownTarget_{ 0.0f, 0.0f, 0.0f };
    float alertTimer_ = 0.0f;
    float combatMemoryTimer_ = 0.0f;
    // Throttles the spotted-contact broadcast. Re-announcing a contact every
    // frame for as long as it stays visible would push one event per actor per
    // frame, and every other actor scans the whole list -- that is quadratic in
    // squad size against a 256-marine cap. One call-out every couple of seconds
    // is what a squad actually does on the radio, and is frequent enough to
    // keep a listener's 4s combat memory topped up without a gap.
    float spotBroadcastCooldown_ = 0.0f;
    float playerGunshotMemoryTimer_ = 0.0f;
    bool spawnCaptured_ = false;
    DirectX::XMFLOAT3 spawnPosition_{ 0.0f, 0.0f, 0.0f };
    std::vector<DirectX::XMFLOAT3> patrolRoute_;
    size_t patrolIndex_ = 0;
    DirectX::XMFLOAT3 patrolWaypoint_{ 0.0f, 0.0f, 0.0f };
    float patrolPauseTimer_ = 0.0f;
    static constexpr float kVisionRange = 28.0f;
    // How much further an actor picks out another *actor* than it picks out the
    // player. A squad in the field is a bigger contact than one man: several
    // bodies moving together, upright, in a different uniform, usually shooting.
    // The player is a single target that can stop, crouch and use the terrain,
    // and the whole stealth game is tuned around the bare kVisionRange.
    //
    // Scaling here rather than raising kVisionRange is what keeps the two
    // independent. kVisionRange stays the number the stealth balance, the debug
    // cone in WorldOverlays.h and the time-of-day readout in Menus.h all quote;
    // none of them shift because two squads can now see each other across a
    // field. Raising the shared constant instead would have handed every bandit
    // a 63m player-detection range that no fog or night preset was balanced for.
    static constexpr float kActorEngagementRangeScale = 2.25f;
    // Mounted-optic reach for a turret gunner. Deliberately not scaled by
    // g_enemyVisionScale: darkness and fog already gate the gunner through the
    // line-of-sight check, and folding the scale in here would drop its night
    // range below a foot bandit's daylight range, which reads as broken rather
    // than as stealth working.
    static constexpr float kTurretGunnerVisionRange = 70.0f;
    // cos(100 deg): a 200 degree cone, reaching past the shoulder line on both
    // sides so only a target almost directly astern is unseen. Wide on purpose:
    // a soldier is not a camera, and a 160 degree cone let a man walk diagonally
    // across another's front at conversational distance without being noticed.
    // The blind wedge behind is what keeps a rear approach worth making.
    static constexpr float kVisionHalfFovCos = -0.173648f;
    static constexpr float kAlertBroadcastRadius = 19.0f;
    // A spotted-contact call-out carries further than an "I'm hit" shout,
    // because it is a radio report rather than a yell: a contact one man sees
    // is a contact the fireteam acts on even spread across a compound. Matched
    // to the engagement range so a squad fighting at a 40m standoff is not
    // silently split into the men who know and the men who do not.
    static constexpr float kSquadIntelRadius = 70.0f;
    static constexpr float kGunshotHearingRadius = 40.0f;
    // How far a landing round is heard from where it struck. Short on purpose:
    // the muzzle report (kGunshotHearingRadius) is what wakes a whole compound,
    // this only wakes the man standing by the wall the player just hit. It is
    // deliberately not scaled by a suppressor -- muffling the muzzle does
    // nothing to the crack at the far end of the shot.
    static constexpr float kImpactNoiseRadius = 10.0f;

    // Applies damageTakenScale to an incoming hit.
    //
    // `guaranteedLethal` marks damage the caller computed AS the remaining
    // health to force a kill -- a headshot, or debris flagged lethalImpact.
    // Those must not be scaled: a quarter of "exactly enough to kill" is a
    // survivable wound, which would turn a clean headshot into a graze. The
    // resistance is meant to make an ally durable under fire, not immune to
    // being shot in the head.
    float ScaleIncomingDamage(float damage, bool guaranteedLethal = false) const {
        if (guaranteedLethal) return damage;
        return damage * (std::max)(0.0f, damageTakenScale);
    }

    void RegisterThreat(float damage) {
        if (damage <= 0.0f || dead_) return;
        coverQueryCooldown_ = (std::min)(coverQueryCooldown_, 0.12f);
        // Getting shot is unmissable even without line of sight or hearing the
        // shot itself: snap straight to Combat and pull in nearby squadmates.
        awareness_ = AwarenessState::Combat;
        combatMemoryTimer_ = 4.0f;
        // No contact position: being shot tells you that YOU are under fire,
        // not where the shooter is. Inventing a direction here would hand the
        // squad a target the casualty never actually saw.
        g_enemyAlertEvents.push_back(
            { position, kAlertBroadcastRadius, faction, {}, false });
    }

    static bool ContainsNoCase(const std::string& value, const char* needle) {
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find(needle) != std::string::npos;
    }

    void SetPoseOffset(const char* boneName, float pitch, float yaw, float roll) {
        using namespace DirectX;
        const int bone = model.skeleton.Find(boneName);
        if (bone < 0) return;
        XMStoreFloat4(&gunPoseOffsets_[bone], XMQuaternionRotationRollPitchYaw(
            XMConvertToRadians(pitch), XMConvertToRadians(yaw), XMConvertToRadians(roll)));
    }

    void ConfigureGunLayer() {
        using namespace DirectX;
        const size_t count = model.skeleton.BoneCount();
        upperBodyMask_.assign(count, 0.0f);
        gunPoseOffsets_.assign(count, XMFLOAT4(0, 0, 0, 1));
        handBone_ = model.skeleton.Find("hand_r");
        headBone_ = model.skeleton.Find("head");
        if (headBone_ < 0) {
            for (size_t bone = 0; bone < count; ++bone) {
                if (ContainsNoCase(model.skeleton.names[bone], "head") &&
                    !ContainsNoCase(model.skeleton.names[bone], "end")) {
                    headBone_ = static_cast<int>(bone);
                    break;
                }
            }
        }

        int spine = model.skeleton.Find("spine_01");
        for (size_t bone = 0; bone < count; ++bone) {
            for (int p = static_cast<int>(bone); p >= 0; p = model.skeleton.parent[p]) {
                if (p == spine) { upperBodyMask_[bone] = 1.0f; break; }
            }
            if (ContainsNoCase(model.skeleton.names[bone], "spine_01")) upperBodyMask_[bone] = 0.35f;
            if (ContainsNoCase(model.skeleton.names[bone], "spine_02")) upperBodyMask_[bone] = 0.65f;

        }

        if (const AnimationClip* idle = model.FindClip("Idle")) {
            upperBodyAnim_.Play(idle);
            upperBodyAnim_.loop = false;
        }

        // Rifle-ready additive pose. Only masked upper-body bones receive it.
        if (!model.authoredDirectional) {
            SetPoseOffset("spine_02", -3.0f, 0.0f, 0.0f);
            SetPoseOffset("spine_03", -4.0f, 0.0f, 0.0f);
        }
    }

    // The upper body a crouched body should show: always the standing aim
    // pose, per the user's decision -- Fire while a shot is still reading as
    // fired (matches UpdateFiringPose's own gate), the shouldered aim Idle
    // otherwise. Never the crouch clip's own arms: measured with
    // InspectCrouchHands, the crouch clips are an unarmed/different pose
    // (hand_r sits near the spine rather than out along a rifle barrel), so
    // using their arms floated the gun and put the hands nowhere near it.
    const AnimationClip* StandingAimOverlay() const {
        if (firingTimer_ > 0.0f)
            if (const AnimationClip* fire = model.FindClip("Fire")) return fire;
        return model.FindClip("Idle");
    }

    void ComputePose(float dt) {
        previousPoseGlobals_ = poseGlobals_;
        previousPoseWorld_ = poseWorld_;
        previousPoseDt_ = dt;
        if (model.valid) {
            DirectX::XMStoreFloat4x4(&previousMeshWorld_, MeshWorldMatrix());
        }
        // Crouched: pelvis height/rotation and the legs come from whatever
        // crouch clip anim is currently playing (playedCrouchClip_), but the
        // torso, arms and head are always layered in from the standing aim
        // pose -- never bent, leaned or replaced -- using the same
        // upperBodyMask_/ComputeLayeredPalette the (non-authored-directional)
        // gun layer already uses. upperBodyAnim_ is repurposed as that
        // overlay's instance here; it is otherwise only read by the
        // non-authoredDirectional branch below, so stealing it for one frame
        // does not disturb that path.
        const AnimationClip* crouchOverlay = (playedCrouchClip_ && crouchBlend_ > 0.0f)
            ? StandingAimOverlay() : nullptr;
        if (crouchOverlay) {
            if (upperBodyAnim_.clip != crouchOverlay) upperBodyAnim_.Play(crouchOverlay);
            upperBodyAnim_.loop = true;
            upperBodyAnim_.Advance(dt);
        }
        if (model.authoredDirectional) {
            if (crouchOverlay) {
                anim.ComputeLayeredPalette(model.skeleton, upperBodyAnim_,
                                           upperBodyMask_, gunPoseOffsets_,
                                           paletteCPU_, &poseGlobals_);
            } else {
                // These clips contain a complete rifle-running pose. Keep
                // their torso motion instead of replacing it with the legacy
                // idle layer.
                anim.ComputePalette(model.skeleton, paletteCPU_);
                anim.ComputeGlobalMatrices(model.skeleton, poseGlobals_);
            }
            ApplyCrouch();
            if (upperBodyGunLayer) ApplyGunIK(dt);
            else UpdateGunFromHandBone(model.skeleton.Find("hand_l"));
        } else if (upperBodyGunLayer && upperBodyAnim_.clip) {
            anim.ComputeLayeredPalette(model.skeleton, upperBodyAnim_, upperBodyMask_,
                                       gunPoseOffsets_, paletteCPU_, &poseGlobals_);
            ApplyCrouch();
            ApplyGunIK(dt);
        } else {
            anim.ComputePalette(model.skeleton, paletteCPU_);
            anim.ComputeGlobalMatrices(model.skeleton, poseGlobals_);
            ApplyCrouch();
            // No IK to hang the weapon off, so parent it to the hand instead
            // of leaving the actor empty-handed.
            UpdateGunFromHandBone(model.skeleton.Find("hand_l"));
        }
        DirectX::XMStoreFloat4x4(&poseWorld_, WorldMatrix());
    }

    DirectX::XMFLOAT3 GunOriginWorld() const {
        return GunOriginWorld(aimYaw);
    }

    DirectX::XMFLOAT3 GunOriginWorld(float yawForGun) const {
        const float sx = std::sin(yawForGun), cz = std::cos(yawForGun);
        // Matches ApplyCrouch's ~0.42m pelvis drop plus the spine lean, scaled
        // by the same crouchBlend_ that drives the pose, so a crouched shooter
        // (player-controlled or AI) fires from where it is actually aiming
        // rather than from a standing eye height. Deliberately not tied to a
        // separate hitbox height: shots are tested against the posed bones
        // already, so this only has to match the muzzle, not add a new one.
        return { position.x + cz * 0.12f + sx * 0.10f,
                 position.y + footOffset + 1.48f - 0.42f * crouchBlend_,
                 position.z - sx * 0.12f + cz * 0.10f };
    }

    bool IsDescendant(int bone, int ancestor) const {
        for (int p = bone; p >= 0; p = model.skeleton.parent[p])
            if (p == ancestor) return true;
        return false;
    }

    static DirectX::XMMATRIX RotationFromTo(DirectX::FXMVECTOR from,
                                             DirectX::FXMVECTOR to) {
        using namespace DirectX;
        const XMVECTOR a = XMVector3Normalize(from);
        const XMVECTOR b = XMVector3Normalize(to);
        const float dot = (std::max)(-1.0f, (std::min)(1.0f,
            XMVectorGetX(XMVector3Dot(a, b))));
        if (dot > 0.9999f) return XMMatrixIdentity();
        XMVECTOR axis = XMVector3Cross(a, b);
        if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-6f)
            axis = XMVectorSet(0, 1, 0, 0);
        return XMMatrixRotationAxis(XMVector3Normalize(axis), std::acos(dot));
    }

    void RotateBranch(int root, DirectX::FXMVECTOR pivot,
                      DirectX::CXMMATRIX rotation) {
        using namespace DirectX;
        XMFLOAT3 p; XMStoreFloat3(&p, pivot);
        const XMMATRIX delta = XMMatrixTranslation(-p.x, -p.y, -p.z) *
                               rotation * XMMatrixTranslation(p.x, p.y, p.z);
        for (size_t bone = 0; bone < poseGlobals_.size(); ++bone) {
            if (!IsDescendant(static_cast<int>(bone), root)) continue;
            XMStoreFloat4x4(&poseGlobals_[bone],
                XMLoadFloat4x4(&poseGlobals_[bone]) * delta);
        }
    }

    void RotateBranchWorld(int root, DirectX::FXMVECTOR pivotModel,
                           DirectX::CXMMATRIX rotationWorld) {
        using namespace DirectX;
        const XMMATRIX world = MeshWorldMatrix();
        const XMMATRIX inverseWorld = XMMatrixInverse(nullptr, world);
        const XMVECTOR pivotWorld = XMVector3TransformCoord(pivotModel, world);
        XMFLOAT3 p; XMStoreFloat3(&p, pivotWorld);
        const XMMATRIX deltaWorld =
            XMMatrixTranslation(-p.x, -p.y, -p.z) *
            rotationWorld *
            XMMatrixTranslation(p.x, p.y, p.z);
        for (size_t bone = 0; bone < poseGlobals_.size(); ++bone) {
            if (!IsDescendant(static_cast<int>(bone), root)) continue;
            XMStoreFloat4x4(&poseGlobals_[bone],
                XMLoadFloat4x4(&poseGlobals_[bone]) *
                world * deltaWorld * inverseWorld);
        }
    }

    void FaceHeadTowardAim() {
        using namespace DirectX;
        if (headBone_ < 0 || static_cast<size_t>(headBone_) >= poseGlobals_.size())
            return;
        const int parent = model.skeleton.parent[headBone_];
        if (parent < 0 || static_cast<size_t>(parent) >= poseGlobals_.size())
            return;

        // Remove sideways head turns authored into idle/walk poses. Rebuild the
        // head from its bind transform relative to the currently aimed neck, so
        // it stays locked to torso direction instead of running a separate
        // look-at solver.
        const XMMATRIX currentHead = XMLoadFloat4x4(&poseGlobals_[headBone_]);
        const XMMATRIX straightHead =
            XMLoadFloat4x4(&model.skeleton.localBind[headBone_]) *
            XMLoadFloat4x4(&poseGlobals_[parent]);
        const XMMATRIX straighten =
            XMMatrixInverse(nullptr, currentHead) * straightHead;
        for (size_t bone = 0; bone < poseGlobals_.size(); ++bone) {
            if (!IsDescendant(static_cast<int>(bone), headBone_)) continue;
            XMStoreFloat4x4(&poseGlobals_[bone],
                XMLoadFloat4x4(&poseGlobals_[bone]) * straighten);
        }

        // Asset-specific bind offset: its neutral face sits about 18 degrees to
        // the right of its torso. Counter-rotate once around the neck vertical.
        const XMVECTOR pivot = XMLoadFloat4x4(&poseGlobals_[headBone_]).r[3];
        RotateBranchWorld(headBone_, pivot,
            XMMatrixRotationY(XMConvertToRadians(headTorsoYawOffsetDegrees)));
    }

    void SolveArmIK(int upper, int lower, int hand, DirectX::FXMVECTOR target) {
        using namespace DirectX;
        if (upper < 0 || lower < 0 || hand < 0) return;
        auto positionOf = [&](int bone) {
            return XMLoadFloat4x4(&poseGlobals_[bone]).r[3];
        };
        const XMVECTOR shoulder = positionOf(upper);
        XMVECTOR elbow = positionOf(lower);
        XMVECTOR handPos = positionOf(hand);
        const float upperLength = XMVectorGetX(XMVector3Length(elbow - shoulder));
        const float lowerLength = XMVectorGetX(XMVector3Length(handPos - elbow));
        XMVECTOR aim = target - shoulder;
        float distance = XMVectorGetX(XMVector3Length(aim));
        if (upperLength < 1e-3f || lowerLength < 1e-3f || distance < 1e-3f) return;
        const XMVECTOR direction = XMVector3Normalize(aim);
        distance = (std::min)(distance, upperLength + lowerLength - 0.01f);
        const float along = (upperLength * upperLength - lowerLength * lowerLength +
                             distance * distance) / (2.0f * distance);
        const float height = std::sqrt((std::max)(0.0f,
            upperLength * upperLength - along * along));
        XMVECTOR bend = (elbow - shoulder) - direction *
            XMVector3Dot(elbow - shoulder, direction);
        if (XMVectorGetX(XMVector3LengthSq(bend)) < 1e-5f)
            bend = XMVector3Cross(direction, XMVectorSet(0, 0, 1, 0));
        bend = XMVector3Normalize(bend);
        const XMVECTOR desiredElbow = shoulder + direction * along + bend * height;

        RotateBranch(upper, shoulder,
            RotationFromTo(elbow - shoulder, desiredElbow - shoulder));
        elbow = positionOf(lower);
        handPos = positionOf(hand);
        RotateBranch(lower, elbow, RotationFromTo(handPos - elbow, target - elbow));
    }

    // Position the gun on the trigger hand the IK just posed, keeping the
    // mesh's original orientation convention.
    //
    // Split responsibility on purpose: the origin tracks the hand, so the
    // weapon travels with the arms, while the orientation stays on the aimed
    // body angles the mesh was authored against. Taking orientation from the
    // hand-to-hand vector instead re-rolled the mesh and let small IK
    // differences between the two wrists tilt the weapon off true.
    //
    // Bone routing follows ApplyGunIK: after this asset's axis conversion the
    // UE labels read mirrored, so hand_l is the rear (trigger) hand.
    void UpdateGunFromHands(int rearHand) {
        using namespace DirectX;
        if (rearHand < 0) return;
        if (static_cast<size_t>(rearHand) >= poseGlobals_.size()) return;

        const XMMATRIX world = MeshWorldMatrix();
        const XMVECTOR rear = XMVector3TransformCoord(
            XMLoadFloat4x4(&poseGlobals_[rearHand]).r[3], world);

        // Orientation uses the original RotationX(-pitch) * RotationY(yaw)
        // convention off the aimed body angles, not a basis built from the
        // hand-to-hand vector. Deriving it from the hands re-rolled the mesh
        // and let small IK differences between the wrists tilt the weapon;
        // the body angles are what the mesh was authored against, so the gun
        // reads the way it originally did. Only the origin comes from the
        // hands, which is what keeps it travelling with the arms.
        const float cp = std::cos(gunPitch_), sp = std::sin(gunPitch_);
        const float sy = std::sin(gunYaw_), cy = std::cos(gunYaw_);
        const XMVECTOR forward = XMVectorSet(sy * cp, sp, cy * cp, 0.0f);
        const XMVECTOR up = XMVectorSet(-sy * sp, cp, -cy * sp, 0.0f);

        // Seat the receiver slightly forward of and below the trigger hand so
        // the grip sits inside the fist rather than at its pivot.
        const XMVECTOR origin = rear + forward * gunGripForward + up * gunGripRise;

        XMStoreFloat4x4(&gunWorld_,
                        XMMatrixScaling(gunScale, gunScale, gunScale) *
                        XMMatrixRotationX(-gunPitch_) *
                        XMMatrixRotationY(gunYaw_) *
                        XMMatrixTranslationFromVector(origin));
    }

    // Parent the gun to the trigger hand's bone frame, for when the arm IK is
    // off and the authored arms are playing untouched.
    //
    // UpdateGunFromHands takes only the origin from the hand and builds
    // orientation from the aimed body angles, which is right when the IK has
    // just placed both wrists on a rifle held along that aim. With the IK off
    // there is no such guarantee -- the arms are wherever the clip puts them --
    // so the aim angles would leave the weapon floating at the hand's position
    // in an unrelated attitude. Here the whole transform, rotation included,
    // comes off the bone, which is what makes the gun actually stick through
    // reloads, sprints and gestures.
    //
    // Bone routing matches ApplyGunIK: after this asset's axis conversion the
    // UE labels read mirrored, so hand_l is the trigger hand.
    void UpdateGunFromHandBone(int triggerHand) {
        using namespace DirectX;
        if (triggerHand < 0) return;
        if (static_cast<size_t>(triggerHand) >= poseGlobals_.size()) return;

        // The bone frame carries the mesh's centimetre scale, which would
        // multiply into the gun on top of gunScale. Take rotation and
        // translation and drop the scale.
        const XMMATRIX handWorld =
            XMLoadFloat4x4(&poseGlobals_[triggerHand]) * MeshWorldMatrix();
        XMVECTOR handScale, handRotation, handTranslation;
        if (!XMMatrixDecompose(&handScale, &handRotation, &handTranslation,
                               handWorld))
            return;
        const XMMATRIX handFrame =
            XMMatrixRotationQuaternion(handRotation) *
            XMMatrixTranslationFromVector(handTranslation);

        // Offsets are in the hand's frame, so they stay put as the wrist turns.
        const XMMATRIX local =
            XMMatrixScaling(gunHandScale, gunHandScale, gunHandScale) *
            XMMatrixRotationRollPitchYaw(
                XMConvertToRadians(gunHandPitchDegrees),
                XMConvertToRadians(gunHandYawDegrees),
                XMConvertToRadians(gunHandRollDegrees)) *
            XMMatrixTranslation(gunHandOffsetX, gunHandOffsetY, gunHandOffsetZ);

        XMStoreFloat4x4(&gunWorld_, local * handFrame);

        // Keep the muzzle angles the tracer and laser read in step with where
        // the weapon actually points now, rather than leaving them on the last
        // IK frame's aim.
        const XMVECTOR barrel = XMVector3Normalize(
            XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
                                     XMLoadFloat4x4(&gunWorld_)));
        XMFLOAT3 direction;
        XMStoreFloat3(&direction, barrel);
        gunYaw_ = std::atan2(direction.x, direction.z);
        gunPitch_ = std::asin((std::max)(-1.0f, (std::min)(1.0f, direction.y)));
    }

    // Where the two hands should sit to hold a rifle aimed along yaw/pitch.
    //
    // Anchored to the trigger-side shoulder rather than a fixed point on the
    // body centerline: a centerline anchor pulled the trigger hand across the
    // chest, which hunched the shoulders and left the rifle lying diagonally
    // across the body instead of shouldered. Starting from the actual posed
    // shoulder keeps the near arm relaxed at its own side and lets the barrel
    // run parallel to the aim rather than across it.
    void ComputeGripTargets(float gunYaw, float gunPitch,
                            DirectX::XMVECTOR& rearGripWorld,
                            DirectX::XMVECTOR& foreGripWorld) const {
        using namespace DirectX;
        const float sx = std::sin(gunYaw), cz = std::cos(gunYaw);
        const float cp = std::cos(gunPitch), sp = std::sin(gunPitch);
        // Aim direction, and the horizontal axis to the trigger side of it.
        const XMVECTOR forward = XMVectorSet(sx * cp, sp, cz * cp, 0.0f);
        const XMVECTOR right = XMVectorSet(cz, 0.0f, -sx, 0.0f);

        // Shoulder height on the body centerline, then stepped out to the
        // trigger shoulder. Falls back to a measured offset when the shoulder
        // bone is missing so the grip never collapses to the centerline.
        const int shoulder = model.skeleton.Find("upperarm_l");
        XMVECTOR anchor;
        if (shoulder >= 0 &&
            static_cast<size_t>(shoulder) < poseGlobals_.size()) {
            anchor = XMVector3TransformCoord(
                XMLoadFloat4x4(&poseGlobals_[shoulder]).r[3], MeshWorldMatrix());
        } else {
            anchor = XMVectorSet(position.x, position.y + footOffset + 1.40f,
                                 position.z, 1.0f);
            anchor = anchor + right * gunShoulderOffset;
        }

        // Trigger hand rides just below and slightly ahead of the shoulder,
        // pulled a little inboard so the stock meets the chest.
        rearGripWorld = anchor
                      + forward * gunRearGripForward
                      + right * gunRearGripInboard
                      + XMVectorSet(0.0f, gunRearGripDrop, 0.0f, 0.0f);

        // Support hand out along the barrel, then offset across and above it.
        // Those two offsets are applied in the gun's own frame (barrel-relative
        // right/up), not world axes, so the fore hand keeps its position on the
        // weapon at any pitch instead of sliding off as the muzzle rises.
        const XMVECTOR barrelUp = XMVectorSet(-sx * sp, cp, -cz * sp, 0.0f);
        foreGripWorld = rearGripWorld
                      + forward * leftArmReach
                      + right * gunForeGripLateral
                      + barrelUp * gunForeGripRise;
    }

    void ApplyGunIK(float dt) {
        using namespace DirectX;
        const int upperR = model.skeleton.Find("upperarm_r");
        const int lowerR = model.skeleton.Find("lowerarm_r");
        const int upperL = model.skeleton.Find("upperarm_l");
        const int lowerL = model.skeleton.Find("lowerarm_l");
        const int handL = model.skeleton.Find("hand_l");
        if (handBone_ < 0 || handL < 0) return;

        // Outside Combat there's no confirmed target to aim at: relax the
        // torso (ease spineTwistCurrent_ back to neutral) and hold the gun
        // low against the body instead of raised, but keep solving the arm
        // IK toward that lowered grip so the hands -- and the separately
        // rendered gun mesh, which reads gunYaw_/gunPitch_ -- stay together.
        // A networked player body never reaches Combat: the AI that raises
        // awareness is skipped for it entirely, so without the replicated aim
        // below it carried its weapon lowered no matter what its owner was
        // doing. The player's own sighted state stands in for the perception
        // this body does not run.
        if (awareness_ != AwarenessState::Combat && !netAiming) {
            const int spine = model.skeleton.Find("spine_01");
            if (spine >= 0) {
                const float maxStep = XMConvertToRadians(spineTwistSpeedDegrees) * dt;
                const float delta = -spineTwistCurrent_;
                spineTwistCurrent_ += (std::max)(-maxStep, (std::min)(maxStep, delta));
                if (spineTwistCurrent_ != 0.0f) {
                    const XMVECTOR pivot =
                        XMLoadFloat4x4(&poseGlobals_[spine]).r[3];
                    RotateBranchWorld(spine, pivot, XMMatrixRotationY(spineTwistCurrent_));
                }
            }

            gunYaw_ = yaw + spineTwistCurrent_;
            gunPitch_ = -0.55f; // muzzle down and forward, carried at ease
            XMVECTOR rightGripWorld, foreGripWorld;
            ComputeGripTargets(gunYaw_, gunPitch_, rightGripWorld, foreGripWorld);
            const XMMATRIX inverseWorld = XMMatrixInverse(nullptr, MeshWorldMatrix());
            SolveArmIK(upperR, lowerR, handBone_,
                       XMVector3TransformCoord(foreGripWorld, inverseWorld));
            SolveArmIK(upperL, lowerL, handL,
                       XMVector3TransformCoord(rightGripWorld, inverseWorld));
            UpdateGunFromHands(handL);

            for (size_t bone = 0; bone < poseGlobals_.size(); ++bone) {
                const XMMATRIX skin = XMLoadFloat4x4(&model.skeleton.offset[bone]) *
                                      XMLoadFloat4x4(&poseGlobals_[bone]);
                XMStoreFloat4x4(&paletteCPU_[bone], XMMatrixTranspose(skin));
            }
            return;
        }

        // Legs follow locomotion yaw. Twist full spine branch back toward player
        // before arm IK so chest, shoulders, neck, head, and arms share weapon aim.
        // The target angle is clamped to a natural range and eased toward over
        // time so the torso doesn't instantly snap to large twists; it still
        // reaches full aim angle given a few frames, so the gun keeps tracking.
        // While sprinting the legs already turn to face the player (see
        // movement code), so this target normally stays near zero; still
        // computed live (not forced to zero) so the held aim keeps tracking
        // if the player moves during the sprint.
        const int spine = model.skeleton.Find("spine_01");
        if (spine >= 0) {
            float targetYaw =
                std::atan2(std::sin(aimYaw - yaw), std::cos(aimYaw - yaw));
            const float limit = XMConvertToRadians(maxSpineTwistDegrees);
            targetYaw = (std::max)(-limit, (std::min)(limit, targetYaw));
            const float maxStep = XMConvertToRadians(spineTwistSpeedDegrees) * dt;
            const float delta = targetYaw - spineTwistCurrent_;
            spineTwistCurrent_ += (std::max)(-maxStep, (std::min)(maxStep, delta));
            const XMVECTOR pivot =
                XMLoadFloat4x4(&poseGlobals_[spine]).r[3];
            RotateBranchWorld(spine, pivot, XMMatrixRotationY(spineTwistCurrent_));
        }

        FaceHeadTowardAim();

        // Arms/gun must match the torso direction the spine twist actually
        // produced, not the raw aim vector, or the IK targets fight the pose
        // and the elbows/wrists distort.
        gunYaw_ = yaw + spineTwistCurrent_;
        gunPitch_ = aimPitch;
        XMVECTOR rightGripWorld, foreGripWorld;
        ComputeGripTargets(gunYaw_, gunPitch_, rightGripWorld, foreGripWorld);
        // Arms hold the gun aimed at the target while in Combat.
        const XMMATRIX inverseWorld = XMMatrixInverse(nullptr, MeshWorldMatrix());
        // UE bone labels appear mirrored after asset-axis conversion. Route the
        // visual trigger arm to rear grip and visual support arm to foregrip.
        SolveArmIK(upperR, lowerR, handBone_,
                   XMVector3TransformCoord(foreGripWorld, inverseWorld));
        SolveArmIK(upperL, lowerL, handL,
                   XMVector3TransformCoord(rightGripWorld, inverseWorld));
        UpdateGunFromHands(handL);

        for (size_t bone = 0; bone < poseGlobals_.size(); ++bone) {
            const XMMATRIX skin = XMLoadFloat4x4(&model.skeleton.offset[bone]) *
                                  XMLoadFloat4x4(&poseGlobals_[bone]);
            XMStoreFloat4x4(&paletteCPU_[bone], XMMatrixTranspose(skin));
        }
    }
};
