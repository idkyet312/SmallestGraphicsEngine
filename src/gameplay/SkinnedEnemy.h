#pragma once
// A skinned character instance: owns the shared SkinnedModel, an animation
// player, and a per-frame bone-palette upload buffer, and knows how to draw
// itself through the mesh-shader path with skinning enabled.
#include "SkinnedFBXImporter.h"
#include "AnimationRuntime.h"
#include "MeshShaderDX12.h"
#include "DX12Core.h"
#include "DestructionDX12.h"
#include "NavigationSystem.h"
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
struct EnemyNoiseEvent { DirectX::XMFLOAT3 position; float radius; };
extern std::vector<EnemyNoiseEvent> g_enemyNoiseEvents;

// Pushed by an enemy the instant it takes damage, so squadmates within radius
// are yanked straight to Combat even without their own line of sight or
// hearing check -- getting shot next to a friend is unmissable. Cleared each
// frame alongside noise events.
struct EnemyAlertEvent { DirectX::XMFLOAT3 position; float radius; };
extern std::vector<EnemyAlertEvent> g_enemyAlertEvents;

// Loadout class. Rifle is the original bandit behaviour; the other two change
// engagement range, damage, and the shape of a shot rather than the model.
enum class BanditWeapon {
    Rifle,
    Shotgun,
    Sniper,
};

enum class Faction : uint8_t { Bandit, Marine };

class SkinnedEnemy {
public:
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
    bool              upperBodyGunLayer = true;
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
    // When set, Patrol/Alert wandering (UpdatePatrolWaypoint) circles this
    // point instead of the actor's own spawn position -- lets a marine loiter
    // near the player instead of near wherever it was placed. Left unset
    // (nullopt) for bandits, who should keep wandering their own spawn point.
    std::optional<DirectX::XMFLOAT3> leashPosition;

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

    // TEMP DEBUG: exposes the rifle firing gate so the ImGui panel can show
    // why an actor is or is not shooting. Remove once marine fire is verified.
    bool DebugPreparingShot() const { return preparingShot_; }
    float DebugStationaryAimTime() const { return stationaryAimTime_; }
    bool DebugHasCoverTarget() const { return hasCoverTarget_; }
    bool DebugInCover() const { return inCover_; }
    bool DebugHasGunPose() const { return HasGunPose(); }
    int DebugBurstShots() const { return burstShotsRemaining; }

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
    // The clear-daylight range, before visibility scaling. Exposed so UI can
    // quote a distance without duplicating the constant.
    static constexpr float BaseVisionRange() { return kVisionRange; }
    static float AlertBroadcastRadius() { return kAlertBroadcastRadius; }
    float VisionHalfFovRadians() const { return std::acos(kVisionHalfFovCos); }

    // Optional authored patrol path. Leave unset and an enemy wanders in a
    // loose loop around its spawn point instead.
    void SetPatrolRoute(const std::vector<DirectX::XMFLOAT3>& route) {
        patrolRoute_ = route;
        patrolIndex_ = 0;
    }

    float BackoffRange() const {
        return 30.0f;
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
        // Marines take cover and shoot from it. The hazard is that vision range
        // (28) is shorter than BackoffRange (30), so any target a marine can
        // see is also inside the cover-query range -- left unguarded it would
        // re-query every 0.75s and SetCoverTarget's stationaryAimTime_ reset
        // would starve the aim-up so it never fired. Once settled in cover it
        // stops asking for a new spot and holds there shooting; it only looks
        // for fresh cover after the current one is given up.
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

    void PlayClip(const std::string& name) {
        if (const AnimationClip* c = model.FindClip(name); c && anim.clip != c) anim.Play(c);
    }

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

    void Update(float dt, const DirectX::XMFLOAT3& target, float groundY) {
        if (dead_) return;
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

        const bool perceived = PerceivePlayer(target);
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
            PlayClip(moveSpeedThisTick > 0.01f ? "Walk" : "Idle");
            anim.Advance(dt);
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
            if (inCover_) {
                const float turn = std::atan2(
                    std::sin(aimYaw - yaw), std::cos(aimYaw - yaw));
                const float maxTurn = 5.5f * dt;
                yaw += (std::max)(-maxTurn, (std::min)(maxTurn, turn));
            }
        }
        if (preparingShot_) stationaryAimTime_ += dt;
        // A sniper walking while its laser is up would drag the beam across the
        // world and make the telegraph unreadable. Plant it for the wind-up.
        const bool rooted = !hasCoverTarget_ &&
            (preparingShot_ || laserCharge_ > 0.0f);
        float speed = 0.0f;
        const bool movingToCover = hasCoverTarget_ && !inCover_;
        const float safeDistance = BackoffRange();
        // Backing off is bandit behavior for keeping the player at arm's
        // length. A marine that did it would retreat from every bandit it
        // spots (vision range 28 is inside the 30 backoff range), never
        // closing to engage -- allies push in and orbit instead.
        // Track the peak before comparing, so an undamaged enemy always reads as
        // full and the very first frame cannot register as a hit.
        if (health > peakHealth) peakHealth = health;
        // Only a wounded bandit gives ground. At full health he stands and
        // fights, so backing off reads as a reaction to being hit rather than
        // the default opening move.
        const bool damaged = health < peakHealth;
        const bool evasiveRetreat = !hasCoverTarget_ && damaged &&
            faction == Faction::Bandit && distance < safeDistance;
        // Bandits hold at their safe distance; marines always close on the
        // target so they can actually orbit and shoot it. An unhurt bandit is
        // excluded: this flag suppresses the whole movement branch below, so
        // holding it while he no longer retreats would freeze him on the spot
        // instead of letting him orbit and fight.
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
            } else if (distance <= orbitRadius + 2.2f) {
                // Grounded version of old hover-enemy controller: preserve a
                // combat ring while moving tangentially around player.
                const float tangentX = -inwardZ * orbitDirection;
                const float tangentZ =  inwardX * orbitDirection;
                const float radial = (std::max)(-0.7f,
                    (std::min)(0.9f, (distance - orbitRadius) * 0.75f));
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
            const auto angleDelta = [](float from, float to) {
                return std::atan2(std::sin(to - from), std::cos(to - from));
            };
            // Legs always face the player regardless of travel direction (strafe,
            // retreat, or approach), so body and aim yaw already agree and the
            // spine barely needs to twist.
            const float turn = angleDelta(yaw, aimYaw);
            const float maxTurn = 5.5f * dt;
            yaw += (std::max)(-maxTurn, (std::min)(maxTurn, turn));
        }
        const bool running = speed > moveSpeed * 1.2f;
        PlayClip(running ? "Run" : speed > 0.01f ? "Walk" : "Idle");
        const float referenceSpeed = running ? moveSpeed * 1.65f : moveSpeed;
        const float playbackRate = speed > 0.01f
            ? (std::max)(0.75f, (std::min)(1.15f, speed / referenceSpeed))
            : 1.0f;
        anim.Advance(dt * playbackRate);
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
        Kill(direction, impact, strength, true,
             RagdollImpactSource::Throw, "pelvis");
        return true;
    }

    bool HasGunPose() const {
        return upperBodyGunLayer && !dead_ && handBone_ >= 0 &&
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
    bool PerceivePlayer(const DirectX::XMFLOAT3& target) const {
        if (dead_ || held_) return false;
        if (turretGunner) return true;
        const float dx = target.x - position.x, dz = target.z - position.z;
        const float distSq = dx * dx + dz * dz;
        const float sightRange = VisionRange();
        if (distSq <= sightRange * sightRange && distSq > 1e-6f) {
            const float invLen = 1.0f / std::sqrt(distSq);
            const float facingX = std::sin(yaw), facingZ = std::cos(yaw);
            const float dot = (dx * invLen) * facingX + (dz * invLen) * facingZ;
            // Marines get squad awareness -- range and the occlusion raycast
            // still apply, only the forward cone is waived. A follower's yaw
            // tracks whatever it is walking toward, so a cone would blind it to
            // exactly the flanking bandits it exists to deal with.
            const bool ignoreFov = faction == Faction::Marine;
            if (dot >= kVisionHalfFovCos || ignoreFov) {
                if (g_enemyLineOfSightFn && g_enemyLineOfSightFn(*this, target))
                    return true;
            }
        }
        for (const EnemyNoiseEvent& noise : g_enemyNoiseEvents) {
            const float nx = noise.position.x - position.x;
            const float nz = noise.position.z - position.z;
            const float radius = noise.radius;
            if (nx * nx + nz * nz <= radius * radius) return true;
        }
        for (const EnemyAlertEvent& alert : g_enemyAlertEvents) {
            const float ax = alert.position.x - position.x;
            const float az = alert.position.z - position.z;
            const float radius = alert.radius;
            if (ax * ax + az * az <= radius * radius) return true;
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
        // Marines miss a lot on purpose: allies that shot as well as bandits
        // trivialized fights the player is supposed to carry. Only the cone is
        // widened -- damage is untouched, so a landed hit still does the normal
        // 20 and five connected hits still kill. The budget is ~30 rounds
        // fired per kill, i.e. roughly one shot in six connects.
        //
        // The offset below is added to a non-normalized aim vector, so a fixed
        // spread shrinks with range -- marines would spray point-blank and
        // tighten up at distance, backwards from how bad aim reads. Scaling by
        // the range makes it a true angular cone that holds the same hit rate
        // at every distance, which is what makes a shots-per-kill budget mean
        // anything.
        if (faction == Faction::Marine) {
            // Hit chance falls as the cone area grows, so ~1-in-6 needs the
            // linear spread at roughly sqrt(6) times the width that would put
            // the cone edge on a torso.
            constexpr float kMarineSpreadScale = 6.0f;
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
        if (IsSniper()) {
            attackEventPending_ = true;
            fireCooldown = 4.5f + ((float)std::rand() / RAND_MAX) * 2.5f;
            return true;
        }
        if (IsShotgunner()) {
            attackEventPending_ = true;
            fireCooldown = 1.5f + ((float)std::rand() / RAND_MAX) * 1.1f;
            preparingShot_ = false;
            stationaryAimTime_ = 0.0f;
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
        } else {
            fireCooldown = 1.2f + ((float)std::rand() / RAND_MAX) * 2.8f;
            preparingShot_ = false;
            stationaryAimTime_ = 0.0f;
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
               bool allowHeadshotKill = true) {
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
        const float appliedDamage = hitHead && allowHeadshotKill ? health : bodyDamage;
        health -= appliedDamage;
        RegisterThreat(appliedDamage);
        if (health <= 0.0f)
            Kill(direction, impact, 1.0f, false,
                 RagdollImpactSource::Bullet, hitBody);
        return true;
    }

    bool HitByHarpoon(const DirectX::XMFLOAT3& start,
                      const DirectX::XMFLOAT3& end,
                      const DirectX::XMFLOAT3& direction, float radius,
                      DirectX::XMFLOAT3* hitPoint = nullptr,
                      std::string* hitBone = nullptr) {
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
             RagdollImpactSource::Harpoon, struckBody);
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
                        float damage, float pushSpeed) {
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
        const float appliedDamage = damage * falloff;
        health -= appliedDamage;
        RegisterThreat(appliedDamage);

        XMFLOAT3 direction;
        XMStoreFloat3(&direction, away);
        if (health <= 0.0f) {
            XMFLOAT3 impactPosition;
            XMStoreFloat3(&impactPosition, body);
            Kill(direction, impactPosition, 1.0f, false,
                 RagdollImpactSource::Explosion, "pelvis");
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
                           DirectX::XMFLOAT3* hitPoint = nullptr) {
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

        const float damage = debris.lethalImpact ? health :
            (std::min)(80.0f, (std::max)(8.0f,
                (speed - 2.5f) * 7.0f +
                std::sqrt((std::max)(0.05f, debris.mass)) * 3.0f));
        health -= damage;
        RegisterThreat(damage);
        debrisHitCooldown_ = 0.45f;
        if (health <= 0.0f) {
            Kill(direction, impact, 1.0f, debris.lethalImpact,
                 RagdollImpactSource::Debris);
        } else {
            const float push = (std::min)(7.0f, speed * 0.65f);
            knockbackVelocity_.x += direction.x * push;
            knockbackVelocity_.z += direction.z * push;
        }
        return true;
    }

    bool Dead() const { return dead_; }
    uint32_t RagdollId() const { return ragdollId_; }

    bool Ignite(float duration = 5.5f) {
        if (dead_ || !visible || duration <= 0.0f) return false;
        const bool newlyIgnited = burnTime_ <= 0.0f;
        burnTime_ = (std::max)(burnTime_, duration);
        if (newlyIgnited) burnSpreadCooldown_ = 0.55f;
        return newlyIgnited;
    }

    bool UpdateBurning(float dt, float damagePerSecond) {
        if (dead_ || burnTime_ <= 0.0f || dt <= 0.0f) return false;
        burnTime_ = (std::max)(0.0f, burnTime_ - dt);
        burnSpreadCooldown_ -= dt;
        health -= (std::max)(0.0f, damagePerSecond) * dt;
        if (health > 0.0f) return false;
        const DirectX::XMFLOAT3 upward{ 0.0f, 1.0f, 0.0f };
        const DirectX::XMFLOAT3 impact{
            position.x, position.y + footOffset + 1.0f, position.z };
        Kill(upward, impact, 0.35f);
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

    bool ConsumeDeathEvent() {
        const bool pending = deathEventPending_;
        deathEventPending_ = false;
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
                    // pins it to a wall. Dead bodies are few and already visible
                    // candidates, so draw every posed meshlet.
                    !dead_, dead_,
                    prevPaletteAddr);
            }
            shader.NextDrawCall();
        }
    }

    const std::vector<DirectX::XMFLOAT4X4>& Palette() const { return paletteCPU_; }

private:
    void Kill(const DirectX::XMFLOAT3& impulseDirection,
              const DirectX::XMFLOAT3& impactPosition,
              float impulseMultiplier = 1.0f,
              bool lethalImpact = false,
              RagdollImpactSource source = RagdollImpactSource::Bullet,
              const std::string& struckBone = {}) {
        using namespace DirectX;
        dead_ = true;
        held_ = false;
        deathEventPending_ = true;
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
    bool spawnCaptured_ = false;
    DirectX::XMFLOAT3 spawnPosition_{ 0.0f, 0.0f, 0.0f };
    std::vector<DirectX::XMFLOAT3> patrolRoute_;
    size_t patrolIndex_ = 0;
    DirectX::XMFLOAT3 patrolWaypoint_{ 0.0f, 0.0f, 0.0f };
    float patrolPauseTimer_ = 0.0f;
    static constexpr float kVisionRange = 28.0f;
    static constexpr float kVisionHalfFovCos = 0.173648f; // cos(80 deg): 160 deg cone
    static constexpr float kAlertBroadcastRadius = 19.0f;

    void RegisterThreat(float damage) {
        if (damage <= 0.0f || dead_) return;
        coverQueryCooldown_ = (std::min)(coverQueryCooldown_, 0.12f);
        // Getting shot is unmissable even without line of sight or hearing the
        // shot itself: snap straight to Combat and pull in nearby squadmates.
        awareness_ = AwarenessState::Combat;
        combatMemoryTimer_ = 4.0f;
        g_enemyAlertEvents.push_back({ position, kAlertBroadcastRadius });
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
        SetPoseOffset("spine_02", -3.0f, 0.0f, 0.0f);
        SetPoseOffset("spine_03", -4.0f, 0.0f, 0.0f);
    }

    void ComputePose(float dt) {
        previousPoseGlobals_ = poseGlobals_;
        previousPoseWorld_ = poseWorld_;
        previousPoseDt_ = dt;
        if (model.valid) {
            DirectX::XMStoreFloat4x4(&previousMeshWorld_, MeshWorldMatrix());
        }
        if (upperBodyGunLayer && upperBodyAnim_.clip) {
            anim.ComputeLayeredPalette(model.skeleton, upperBodyAnim_, upperBodyMask_,
                                       gunPoseOffsets_, paletteCPU_, &poseGlobals_);
            ApplyGunIK(dt);
        } else {
            anim.ComputePalette(model.skeleton, paletteCPU_);
            anim.ComputeGlobalMatrices(model.skeleton, poseGlobals_);
        }
        DirectX::XMStoreFloat4x4(&poseWorld_, WorldMatrix());
    }

    DirectX::XMFLOAT3 GunOriginWorld() const {
        return GunOriginWorld(aimYaw);
    }

    DirectX::XMFLOAT3 GunOriginWorld(float yawForGun) const {
        const float sx = std::sin(yawForGun), cz = std::cos(yawForGun);
        return { position.x + cz * 0.12f + sx * 0.10f,
                 position.y + footOffset + 1.48f,
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
        if (awareness_ != AwarenessState::Combat) {
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
