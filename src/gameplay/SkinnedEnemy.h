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
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

extern MeshShaderDX12 g_meshShader;

// Loadout class. Rifle is the original bandit behaviour; the other two change
// engagement range, damage, and the shape of a shot rather than the model.
enum class BanditWeapon {
    Rifle,
    Shotgun,
    Sniper,
};

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
    float             leftArmReach = 0.55f;
    float             headTorsoYawOffsetDegrees = 20.4f;
    float             orbitRadius = 4.8f;
    float             orbitDirection = 1.0f;
    float             fireCooldown = 1.0f;
    // Grenade throwing. Independent of fireCooldown so a grenade never competes
    // with the rifle for the same timer; spawn code randomises the initial value
    // so a squad does not lob in unison.
    float             grenadeCooldown = 8.0f;
    int               spawnSlot = -1;
    bool              turretGunner = false;
    int               mountedVehicleIndex = 0;
    int               burstShotsRemaining = 0;
    // Loadout. Set at spawn; drives engagement range, aim delay, and how main
    // turns a "fired" result into projectiles.
    BanditWeapon      weapon = BanditWeapon::Rifle;

    // Sniper telegraph: how long the laser paints the player before the shot.
    // Long on purpose -- the beam IS the warning, so the player needs time to
    // break line of sight or take cover after spotting it.
    static constexpr float kSniperLaserWarning = 5.0f;

    bool IsSniper() const { return weapon == BanditWeapon::Sniper; }
    bool IsShotgunner() const { return weapon == BanditWeapon::Shotgun; }

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
        }
        // Prime with the bind pose so the very first frame renders upright even
        // before any clip is assigned.
        ConfigureGunLayer();
        ComputePose();
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

    void Update(float dt, const DirectX::XMFLOAT3& target, float groundY) {
        if (dead_) return;
        debrisHitCooldown_ = (std::max)(0.0f, debrisHitCooldown_ - dt);
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
        const float dx = target.x - position.x, dz = target.z - position.z;
        const float distance = std::sqrt(dx*dx + dz*dz);
        if (distance > 0.1f) {
            aimYaw = std::atan2(dx, dz);
            const float gunHeight = position.y + footOffset + 1.48f;
            aimPitch = (std::max)(-0.55f, (std::min)(
                0.55f, std::atan2(target.y - gunHeight, distance)));
        }
        if (preparingShot_) stationaryAimTime_ += dt;
        // A sniper walking while its laser is up would drag the beam across the
        // world and make the telegraph unreadable. Plant it for the wind-up.
        const bool rooted = preparingShot_ || laserCharge_ > 0.0f;
        float speed = 0.0f;
        if (distance > 0.1f && !rooted) {
            const float inv = 1.0f / distance;
            const float inwardX = dx * inv;
            const float inwardZ = dz * inv;
            float moveX = inwardX;
            float moveZ = inwardZ;
            bool orbiting = false;

            if (distance <= orbitRadius + 2.2f) {
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
            XMFLOAT3 requestedDestination = orbiting
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
            const float movementYaw = std::atan2(moveX, moveZ);
            const auto angleDelta = [](float from, float to) {
                return std::atan2(std::sin(to - from), std::cos(to - from));
            };
            // Let legs turn into orbit instead of playing a forward walk while
            // sliding fully sideways. Keep torso close enough to weapon aim for IK.
            const float desiredYaw = orbiting
                ? aimYaw + angleDelta(aimYaw, movementYaw) * 0.48f
                : movementYaw;
            const float turn = angleDelta(yaw, desiredYaw);
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
        ComputePose();
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
        ComputePose();
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
        ComputePose();
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
        Kill(direction, impact, strength, true);
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

    bool NeedsLineOfSightCheck() const {
        if (dead_ || held_ || !visible || !HasGunPose()) return false;
        if (turretGunner) return true;
        // The sniper needs a truthful sight test every frame it is charging, not
        // just on the firing frame: the beam is only fair if breaking cover
        // actually drops the lock.
        if (IsSniper()) return fireCooldown <= 0.0f;
        if (burstShotsRemaining > 0)
            return fireCooldown <= 0.0f;
        return fireCooldown <= 0.0f &&
               preparingShot_ && stationaryAimTime_ >= 2.0f;
    }

    bool TryFireAt(float dt, const DirectX::XMFLOAT3& target,
                   bool hasLineOfSight,
                   DirectX::XMFLOAT3& origin, DirectX::XMFLOAT3& direction) {
        using namespace DirectX;
        if (dead_ || held_ || !visible || !HasGunPose()) return false;
        fireCooldown -= dt;
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
                if (stationaryAimTime_ < 2.0f) return false;
            } else if (fireCooldown > 0.0f) {
                return false;
            }
        }

        origin = AimRayOrigin();
        XMVECTOR aim = XMLoadFloat3(&target) - XMLoadFloat3(&origin);
        if (XMVectorGetX(XMVector3LengthSq(aim)) < 1e-5f) return false;

        auto randomSigned = [] {
            return ((float)std::rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        };
        // Slight human aim error. Bursts remain dangerous without becoming
        // four perfectly accurate automatic turrets. The sniper spent five
        // seconds lining the shot up on a visible beam, so it gets a much
        // tighter cone -- the telegraph is the counterplay, not bad aim.
        const float spread = IsSniper() ? 0.004f : 0.018f;
        const float verticalSpread = IsSniper() ? 0.003f : 0.012f;
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

    // Weapon uses a stable character-facing frame. Arm IK places both hands on
    // this same frame, so walk animation cannot roll the barrel toward ground.
    DirectX::XMMATRIX GunWorldMatrix() const {
        using namespace DirectX;
        if (!HasGunPose()) return XMMatrixIdentity();
        const XMFLOAT3 origin = GunOriginWorld();
        return XMMatrixScaling(0.6f, 0.6f, 0.6f) *
               XMMatrixRotationX(-aimPitch) *
               XMMatrixRotationY(aimYaw) *
               XMMatrixTranslation(origin.x, origin.y, origin.z);
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
            const XMMATRIX recovered =
                XMMatrixInverse(nullptr, XMLoadFloat4x4(&bodyLocal_[bone])) *
                scaledBodyWorld * inverseWorld;
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
               float bodyDamage = 20.0f) {
        if (dead_ || !visible) return false;
        DirectX::XMFLOAT3 impact;
        bool hitHead = false;
        if (!BlocksProjectile(start, end, radius, &impact, &hitHead)) return false;
        if (hitPoint) *hitPoint = impact;
        if (headshot) *headshot = hitHead;
        // Standard rifle balance: one headshot, exactly five body hits from
        // full 100 health.
        health -= hitHead ? health : bodyDamage;
        if (health <= 0.0f) Kill(direction, impact);
        return true;
    }

    bool BlocksProjectile(const DirectX::XMFLOAT3& start,
                          const DirectX::XMFLOAT3& end, float radius,
                          DirectX::XMFLOAT3* hitPoint = nullptr,
                          bool* headshot = nullptr) const {
        using namespace DirectX;
        if (dead_ || !visible) return false;
        const XMVECTOR a = XMLoadFloat3(&start), b = XMLoadFloat3(&end);
        const XMVECTOR ab = b - a;
        const float lengthSq = XMVectorGetX(XMVector3LengthSq(ab));
        auto hitSphere = [&](const XMVECTOR center, float sphereRadius,
                             XMFLOAT3* impact) {
            float t = lengthSq > 1e-6f
                ? XMVectorGetX(XMVector3Dot(center - a, ab)) / lengthSq : 0.0f;
            t = (std::max)(0.0f, (std::min)(1.0f, t));
            const XMVECTOR closest = a + ab * t;
            const float hitRadius = sphereRadius + radius;
            if (XMVectorGetX(XMVector3LengthSq(closest - center)) >
                hitRadius * hitRadius) return false;
            if (impact) XMStoreFloat3(impact, closest);
            return true;
        };

        if (headshot) *headshot = false;
        if (headBone_ >= 0 && static_cast<size_t>(headBone_) < poseGlobals_.size()) {
            XMVECTOR head = XMVector3TransformCoord(
                XMVectorZero(), XMLoadFloat4x4(&poseGlobals_[headBone_]) * WorldMatrix());
            head += XMVectorSet(0.0f, 0.12f, 0.0f, 0.0f);
            if (hitSphere(head, 0.24f, hitPoint)) {
                if (headshot) *headshot = true;
                return true;
            }
        }

        const XMVECTOR body = XMVectorSet(
            position.x, position.y + footOffset + 1.0f, position.z, 0.0f);
        return hitSphere(body, 0.72f, hitPoint);
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
        health -= damage * falloff;

        XMFLOAT3 direction;
        XMStoreFloat3(&direction, away);
        if (health <= 0.0f) {
            XMFLOAT3 impactPosition;
            XMStoreFloat3(&impactPosition, body);
            Kill(direction, impactPosition);
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
        debrisHitCooldown_ = 0.45f;
        if (health <= 0.0f) {
            Kill(direction, impact);
        } else {
            const float push = (std::min)(7.0f, speed * 0.65f);
            knockbackVelocity_.x += direction.x * push;
            knockbackVelocity_.z += direction.z * push;
        }
        return true;
    }

    bool Dead() const { return dead_; }

    bool KillFromRotor(const DirectX::XMFLOAT3& direction,
                       const DirectX::XMFLOAT3& impact) {
        if (dead_ || !visible) return false;
        Kill(direction, impact, 22.0f, true);
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

        // Mesh gets an extra independent rotation (debug) pre-applied in its own
        // local space so it can be aligned against the skeleton overlay.
        const XMMATRIX world = MeshWorldMatrix();
        shader.SetMatrices(world, view, proj, lightSpace);

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
                g_meshShader.Draw(prim.vbv, (UINT)(prim.vertices.size() / 12), prim.indexCount,
                    prim.meshletCount, descA, boundsA, vidxA, triA,
                    paletteAddr, prim.skinBuffer->GetGPUVirtualAddress(),
                    prim.material && prim.material->doubleSided);
            }
            shader.NextDrawCall();
        }
    }

    const std::vector<DirectX::XMFLOAT4X4>& Palette() const { return paletteCPU_; }

private:
    void Kill(const DirectX::XMFLOAT3& impulseDirection,
              const DirectX::XMFLOAT3& impactPosition,
              float impulseMultiplier = 1.0f,
              bool lethalImpact = false) {
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
            const XMMATRIX local = XMMatrixRotationQuaternion(XMLoadFloat4(&spec.rotation)) *
                                   XMMatrixTranslation(spec.center.x, spec.center.y, spec.center.z);
            XMStoreFloat4x4(&bodyLocal_[bone], local);
            const XMMATRIX bodyWorld = local * XMLoadFloat4x4(&globals[bone]) * WorldMatrix();
            XMVECTOR scale, rotation, translation;
            if (!XMMatrixDecompose(&scale, &rotation, &translation, bodyWorld)) continue;
            AuthoredRagdollBody body;
            body.name = spec.bone; body.halfExtent = spec.halfExtent;
            body.radius = spec.radius; body.length = spec.length; body.shape = spec.shape;
            XMStoreFloat3(&body.position, translation);
            XMStoreFloat4(&body.rotation, XMQuaternionNormalize(rotation));
            bodies.push_back(body);
        }
        ragdollId_ = g_destruction.SpawnAuthoredRagdoll(
            bodies, model.ragdoll.constraints, impulseDirection, impactPosition,
            impulseMultiplier, lethalImpact);
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> palette_[FRAME_COUNT];
    void* mapped_[FRAME_COUNT] = {};
    UINT  paletteBytes_ = 0;
    std::vector<DirectX::XMFLOAT4X4> paletteCPU_;
    AnimationInstance upperBodyAnim_;
    std::vector<float> upperBodyMask_;
    std::vector<DirectX::XMFLOAT4> gunPoseOffsets_;
    std::vector<DirectX::XMFLOAT4X4> poseGlobals_;
    std::vector<DirectX::XMFLOAT4X4> deathGlobals_;
    std::vector<DirectX::XMFLOAT4X4> bodyLocal_;
    DirectX::XMFLOAT4X4 deathWorld_ = {};
    DirectX::XMFLOAT3 knockbackVelocity_{ 0.0f, 0.0f, 0.0f };
    float stationaryAimTime_ = 0.0f;
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
    bool mountedFiring_ = false;

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

    void ComputePose() {
        if (upperBodyGunLayer && upperBodyAnim_.clip) {
            anim.ComputeLayeredPalette(model.skeleton, upperBodyAnim_, upperBodyMask_,
                                       gunPoseOffsets_, paletteCPU_, &poseGlobals_);
            ApplyGunIK();
        } else {
            anim.ComputePalette(model.skeleton, paletteCPU_);
            anim.ComputeGlobalMatrices(model.skeleton, poseGlobals_);
        }
    }

    DirectX::XMFLOAT3 GunOriginWorld() const {
        const float sx = std::sin(aimYaw), cz = std::cos(aimYaw);
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

    void ApplyGunIK() {
        using namespace DirectX;
        const int upperR = model.skeleton.Find("upperarm_r");
        const int lowerR = model.skeleton.Find("lowerarm_r");
        const int upperL = model.skeleton.Find("upperarm_l");
        const int lowerL = model.skeleton.Find("lowerarm_l");
        const int handL = model.skeleton.Find("hand_l");
        if (handBone_ < 0 || handL < 0) return;

        // Legs follow locomotion yaw. Twist full spine branch back toward player
        // before arm IK so chest, shoulders, neck, head, and arms share weapon aim.
        const int spine = model.skeleton.Find("spine_01");
        if (spine >= 0) {
            const float upperBodyYaw =
                std::atan2(std::sin(aimYaw - yaw), std::cos(aimYaw - yaw));
            const XMVECTOR pivot =
                XMLoadFloat4x4(&poseGlobals_[spine]).r[3];
            RotateBranchWorld(spine, pivot, XMMatrixRotationY(upperBodyYaw));
        }

        FaceHeadTowardAim();

        const XMFLOAT3 origin = GunOriginWorld();
        const float sx = std::sin(aimYaw), cz = std::cos(aimYaw);
        const float cp = std::cos(aimPitch), sp = std::sin(aimPitch);
        const XMFLOAT3 forward{ sx * cp, sp, cz * cp };
        const XMVECTOR rightGripWorld = XMVectorSet(
            origin.x + cz * 0.08f, origin.y - 0.02f,
            origin.z - sx * 0.08f, 1.0f);
        const XMVECTOR foreGripWorld = XMVectorSet(
            origin.x - cz * 0.07f + forward.x * leftArmReach,
            origin.y - 0.03f + forward.y * leftArmReach,
            origin.z + sx * 0.07f + forward.z * leftArmReach, 1.0f);
        const XMMATRIX inverseWorld = XMMatrixInverse(nullptr, MeshWorldMatrix());
        // UE bone labels appear mirrored after asset-axis conversion. Route the
        // visual trigger arm to rear grip and visual support arm to foregrip.
        SolveArmIK(upperR, lowerR, handBone_,
                   XMVector3TransformCoord(foreGripWorld, inverseWorld));
        SolveArmIK(upperL, lowerL, handL,
                   XMVector3TransformCoord(rightGripWorld, inverseWorld));

        for (size_t bone = 0; bone < poseGlobals_.size(); ++bone) {
            const XMMATRIX skin = XMLoadFloat4x4(&model.skeleton.offset[bone]) *
                                  XMLoadFloat4x4(&poseGlobals_[bone]);
            XMStoreFloat4x4(&paletteCPU_[bone], XMMatrixTranspose(skin));
        }
    }
};
