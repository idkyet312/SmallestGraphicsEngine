#pragma once

#include <DirectXMath.h>
#include <d3d12.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "NvBlastTkEvent.h"
#include "SceneGraph.h"
#include "SkinnedTypes.h"

// Installed by the renderer at startup. Destruction calls it as merged batch
// nodes are retired so their visibility-buffer mesh slots can be recycled;
// destruction itself has no renderer dependency. Left null, batches simply
// leak their slots, which is what happened before this existed.
extern std::function<void(const std::shared_ptr<SceneNode>&)>
    g_releaseVisibilityGeometry;

struct DestructionRenderItem {
    std::shared_ptr<SceneNode> node;
    DirectX::XMFLOAT4X4 transform;
    // World-space bounding sphere so render passes can cull without walking the
    // node's geometry. Radius is inflated 10% against edge pop-in.
    DirectX::XMFLOAT3 sphereCenter = {};
    float sphereRadius = 0.0f;
};

struct DestructionRenderBatch {
    // Colour mesh is merged by material; shadow mesh is fully flattened.
    std::shared_ptr<SceneNode> colourNode;
    std::shared_ptr<SceneNode> shadowNode;
    DirectX::XMFLOAT4X4 transform;
    DirectX::XMFLOAT3 sphereCenter = {};
    float sphereRadius = 0.0f;
    uint32_t chunkCount = 0;
};

struct DestructionDebrisHazard {
    DirectX::XMFLOAT3 worldMin;
    DirectX::XMFLOAT3 worldMax;
    DirectX::XMFLOAT3 worldCenter;
    DirectX::XMFLOAT3 velocity;
    float mass = 0.0f;
    bool lethalImpact = false;
};

struct TinyDebrisParticle {
    DirectX::XMFLOAT3 position = {};
    DirectX::XMFLOAT3 velocity = {};
    float size = 0.08f;
};

struct DestructionBurningPoint {
    DirectX::XMFLOAT3 position = {};
    float size = 1.4f;
    float intensity = 1.0f;
};

struct DestructionCollisionSoundEvent {
    DirectX::XMFLOAT3 position = {};
    float approachSpeed = 0.0f;
};

struct DestructionBodyPose {
    DirectX::XMFLOAT3 position = {};
    DirectX::XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
    DirectX::XMFLOAT3 linearVelocity = {};
};

struct DestructionStressStats {
    bool running = false;
    float elapsedSeconds = 0.0f;
    uint32_t sampledFrames = 0;
    uint32_t peakActors = 0;
    uint32_t peakAwakeActors = 0;
    uint32_t tinyParticles = 0;
    uint32_t collisionLodBodies = 0;
    uint32_t frozenBodies = 0;
    uint64_t renderRebuilds = 0;
    double triggerMilliseconds = 0.0;
    double averageFrameMilliseconds = 0.0;
    double peakFrameMilliseconds = 0.0;
    double averageUpdateMilliseconds = 0.0;
    double peakUpdateMilliseconds = 0.0;
    double peakPhysicsMilliseconds = 0.0;
    double peakRenderRebuildMilliseconds = 0.0;
};

struct RagdollRenderItem {
    DirectX::XMFLOAT4X4 transform;
    DirectX::XMFLOAT3 color;
    uint8_t shape = 1; // 0 box, 1 capsule, 2 sphere
    DirectX::XMFLOAT3 sphereCenter = {};
    float sphereRadius = 0.0f;
};

struct EnemyGunRenderItem {
    DirectX::XMFLOAT4X4 transform;
};

struct EnemyShot {
    DirectX::XMFLOAT3 origin;
    DirectX::XMFLOAT3 direction;
};

struct AuthoredRagdollBody {
    std::string name;
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT4 rotation;
    std::vector<RagdollShapeSpec> shapes;
    DirectX::XMFLOAT3 linearVelocity = {};
    DirectX::XMFLOAT3 angularVelocity = {};
    float targetMass = 1.0f;
};

struct RagdollPhysicsDebugShape {
    DirectX::XMFLOAT4X4 transform;
    RagdollShapeType type = RagdollShapeType::Capsule;
    DirectX::XMFLOAT3 halfExtent = {};
    float radius = 0.0f;
    float length = 0.0f;
};

enum class RagdollImpactSource : uint8_t {
    Bullet, Explosion, Throw, Harpoon, Debris
};

struct RagdollImpact {
    RagdollImpactSource source = RagdollImpactSource::Bullet;
    std::string bodyName;
    DirectX::XMFLOAT3 position = {};
    DirectX::XMFLOAT3 direction = { 0,0,1 };
    float impulseMultiplier = 1.0f;
    bool lethalHazard = false;
};

struct AuthoredRagdollPose {
    std::string bone;
    DirectX::XMFLOAT4X4 bodyTransform;
};

// Snapshot of the live Blast/Box3D state for on-screen debug drawing.
struct DestructionDebugChunk {
    DirectX::XMFLOAT3 worldMin;   // AABB corners already in world space
    DirectX::XMFLOAT3 worldMax;
    DirectX::XMFLOAT3 worldCenter;
    bool support = false;         // anchored to the world
    bool dynamic = false;         // owning actor is simulated
};

struct DestructionDebugBond {
    DirectX::XMFLOAT3 a;          // world-space chunk centers the bond joins
    DirectX::XMFLOAT3 b;
    bool broken = false;         // healthy vs. severed
    float health = 0.0f;         // live bond health (0 = gone, kBondHealth = full)
    float healthFraction = 0.0f; // health normalized to [0,1]
};

struct DestructionDebugData {
    std::vector<DestructionDebugChunk> chunks;
    std::vector<DestructionDebugBond> bonds;
    DirectX::XMFLOAT3 lastHit = {};
    bool hasHit = false;
    float hitRadius = 0.0f;
    uint32_t actorCount = 0;
    uint32_t dynamicActorCount = 0;
};

class DestructionDX12 final : public Nv::Blast::TkEventListener {
public:
    DestructionDX12();
    ~DestructionDX12();

    bool Initialize(const std::shared_ptr<SceneNode>& mergedModel,
                    ID3D12Device* device, int gridX = 4, int gridY = 3, int gridZ = 4);
    void Shutdown();
    void Reset();
    void Update(float dt);
    bool InitializeVehicle(const DirectX::XMFLOAT3& chassisCenter,
                           float yawRadians = 0.0f);
    void SetVehicleInput(float throttle, float steering, bool brake);
    bool GetVehicleTransform(DirectX::XMFLOAT4X4& transform,
                             DirectX::XMFLOAT3* position = nullptr,
                             DirectX::XMFLOAT3* forward = nullptr,
                             DirectX::XMFLOAT3* linearVelocity = nullptr) const;
    bool VehicleReady() const;
    void SetEnemyTarget(const DirectX::XMFLOAT3& target);
    std::vector<EnemyShot> DrainEnemyShots();
    uint32_t SpawnAuthoredRagdoll(const std::vector<AuthoredRagdollBody>& bodies,
                                  const std::vector<RagdollConstraintSpec>& constraints,
                                  const RagdollImpact& impact);
    bool GetAuthoredRagdollPose(uint32_t ragdollId,
                               std::vector<AuthoredRagdollPose>& pose) const;
    bool HitTest(const DirectX::XMFLOAT3& worldPosition, float radius,
                 DirectX::XMFLOAT3& hitPosition) const;
    bool HitTestSegment(const DirectX::XMFLOAT3& worldStart,
                        const DirectX::XMFLOAT3& worldEnd, float radius,
                        DirectX::XMFLOAT3& hitPosition,
                        uint32_t ignoredHarpoonId = 0) const;
    // Chunks whose node name contains this marker are objective geometry: blasts
    // and radial damage from anything other than a deliberate call pass straight
    // through them. Lets the comm tower stand in the enemy helicopter's patrol
    // path (and beside explosive barrels) without being felled by either.
    static constexpr const char* ProtectedChunkMarker = "#Protected";
    // True when the chunk nearest `worldPosition` is a corrugated metal roof
    // sheet, so a caller can pick the right impact sound for the surface it
    // just hit. Cheap nearest-cell lookup; no physics query.
    bool IsMetalSheetAt(const DirectX::XMFLOAT3& worldPosition) const;
    // True when the chunk the last hit test resolved is objective geometry
    // (ProtectedChunkMarker). Such a hit must not chip the chunk directly: the
    // caller routes the damage to the owning prefab's health instead, so the
    // structure only comes apart once that health is spent.
    bool IsProtectedChunkAt(const DirectX::XMFLOAT3& worldPosition) const;
    // `sparesProtected` marks the damage as indirect (spreading fire, debris
    // impact), which leaves ProtectedChunkMarker geometry untouched. A direct
    // player hit leaves it false so the player can still cut those chunks.
    void ApplyRadialDamage(const DirectX::XMFLOAT3& worldPosition,
                           float radius, float damage = 2.0f,
                           bool sparesProtected = false);
    // Laser-only hard cut: immediately severs the exact impacted chunk. Support
    // cells and already-detached single chunks do not resist this path.
    //
    // `allowProtected` is the demolition opt-in for ProtectedChunkMarker
    // geometry. It defaults to false so this stays safe for every ordinary
    // caller: only the authorised demolition of an objective (a comm tower with a
    // charge planted on it) passes true. Without that, a laser cut or any future
    // caller could carve up a structure that is meant to be invulnerable.
    void DestroyChunkAt(const DirectX::XMFLOAT3& worldPosition, float radius,
                        bool allowProtected = false);
    // Demolition of objective geometry: clears ProtectedChunkMarker status on
    // every chunk within `radius`, drops the anchoring of any support chunk
    // among them, and severs the lot so the whole structure comes down.
    //
    // DestroyChunkAt only ever frees the single nearest chunk, so felling a
    // 12-band mast that way left half of it standing -- and still protected,
    // which meant permanently invulnerable. This releases the geometry instead:
    // once cleared, the pieces are ordinary destructible structure.
    //
    // One-way and deliberate. Nothing re-protects a released chunk, and only an
    // authorised demolition (a comm tower with a charge planted on it) calls it.
    void ReleaseProtectedChunks(const DirectX::XMFLOAT3& worldPosition,
                                float radius);
    // Attaches persistent fire to the impacted Blast chunk. Attachment follows
    // that chunk through actor splits and physics motion.
    void IgniteChunkAt(const DirectX::XMFLOAT3& worldPosition);
    std::vector<DestructionBurningPoint> GetBurningChunkPoints() const;
    // Grenade-style explosion: breaks every piece whose centre is within radius
    // of the blast (a whole sphere of the building), then shoves the freed
    // fragments radially outward from the blast centre.
    void ApplyExplosion(const DirectX::XMFLOAT3& worldPosition,
                        float radius, float damage, float impulse);
    // Fully severs every house chunk intersecting the sphere, including
    // supports, then holds freed debris in a controlled orbit for `duration`.
    // At expiry the orbit velocity is released with an outward/upward kick.
    void StartVortex(const DirectX::XMFLOAT3& worldPosition,
                     float radius, float duration = 3.0f);
    uint32_t CreateExplosiveBarrelBody(
        const DirectX::XMFLOAT3& worldPosition);
    bool GetExplosiveBarrelPose(uint32_t handle,
                                DestructionBodyPose& pose) const;
    bool SetExplosiveBarrelVelocity(
        uint32_t handle, const DirectX::XMFLOAT3& linearVelocity,
        const DirectX::XMFLOAT3& angularVelocity = {});
    void DestroyExplosiveBarrelBody(uint32_t handle);
    std::vector<uint32_t> DrainExplosiveBarrelImpactEvents();
    uint32_t CreateGrenadeBody(
        const DirectX::XMFLOAT3& worldPosition,
        const DirectX::XMFLOAT3& linearVelocity,
        bool capsuleShape = false, float gravityScale = 1.0f);
    bool GetGrenadeBodyPose(uint32_t handle,
                            DestructionBodyPose& pose) const;
    bool ResolveGrenadeBodyCollision(
        uint32_t handle, const DirectX::XMFLOAT3& position,
        const DirectX::XMFLOAT3& surfaceNormal);
    void DestroyGrenadeBody(uint32_t handle);
    std::vector<uint32_t> DrainGrenadeContactEvents();
    void ApplyRagdollExplosion(const DirectX::XMFLOAT3& worldPosition,
                               float radius, float impulse);
    bool ApplyImpulse(const DirectX::XMFLOAT3& worldPosition,
                      const DirectX::XMFLOAT3& worldDirection,
                      float impulseStrength, float hitRadius = 0.5f);
    // Yanks nearby dynamic debris, ragdolls, and physics barrels toward target.
    bool ApplyHarpoonPull(const DirectX::XMFLOAT3& worldPosition,
                          const DirectX::XMFLOAT3& target,
                          float impulseStrength = 85.0f,
                          float hitRadius = 1.0f);
    bool AttachRagdollToHarpoon(uint32_t ragdollId, uint32_t harpoonId,
                                const DirectX::XMFLOAT3& impactPosition,
                                float shaftOffset,
                                const std::string& struckBone = {});
    void MoveHarpoonRagdolls(uint32_t harpoonId,
                             const DirectX::XMFLOAT3& harpoonPosition,
                             const DirectX::XMFLOAT3& direction);
    void PinHarpoonRagdolls(uint32_t harpoonId,
                            const DirectX::XMFLOAT3& impactPosition,
                            const DirectX::XMFLOAT3& direction,
                            bool attachToLastDestructible = false);
    bool GetPinnedHarpoonPose(uint32_t harpoonId,
                              DirectX::XMFLOAT3& position,
                              DirectX::XMFLOAT3& direction) const;
    void ReleaseHarpoonRagdolls(uint32_t harpoonId,
                                const DirectX::XMFLOAT3& direction,
                                float speed = 12.0f);
    // Resolves the player against destruction/ragdoll boxes. Walls push the eye
    // out horizontally; low boxes the player is standing over raise `floorY` (so
    // the caller can stand the player on top) instead of shoving them sideways.
    // `collideVehicle` blocks the player against the humvee chassis; pass false
    // while driving, when the camera legitimately sits inside that hull.
    void ResolvePlayerCollision(DirectX::XMFLOAT3& eyePosition, float& floorY,
                                float radius = 0.35f, float height = 1.7f,
                                bool collideVehicle = true);
    // Define a water region (AABB, with the surface at max.y). Dynamic
    // fragments knocked into it get buoyancy so house debris floats.
    void SetWaterRegion(const DirectX::XMFLOAT3& minCorner,
                        const DirectX::XMFLOAT3& maxCorner);
    // Supply the terrain-height sampler (CPU mirror of the terrain shader) so
    // debris collides with the real ground surface instead of a flat plane.
    // Rebuilds the static ground collider as a heightfield. Call after Initialize.
    void SetTerrainSampler(std::function<float(float, float)> sampler);
    // Callback invoked (x, z, strength) when a fragment or ragdoll part first
    // breaks the water surface, so the caller can spawn a splash ripple.
    void SetSplashCallback(std::function<void(float, float, float)> cb);
    // Take and clear the world positions where the building fractured pieces
    // loose since the last call, so the caller can spawn smoke at each break.
    std::vector<DirectX::XMFLOAT3> DrainBreakPoints();
    // Take hard collision events involving at least one destruction fragment.
    // Caller applies distance falloff, variation, and playback rate limiting.
    std::vector<DestructionCollisionSoundEvent> DrainCollisionSoundEvents();
    // Snapshot of awake, fast-moving destructible chunks and authored ragdoll
    // limbs. Used by gameplay to make physical bodies strike characters.
    std::vector<DestructionDebrisHazard> GetDangerousDebris(
        float minimumSpeed = 2.5f) const;
    std::vector<TinyDebrisParticle> DrainTinyDebrisParticles();
    void StartCollapseStressBenchmark();
    DestructionStressStats GetStressStats() const;

    bool IsInitialized() const;
    uint32_t GetChunkCount() const;
    uint32_t GetActorCount() const;
    uint64_t GetRenderItemRebuildCount() const;
    uint64_t GetBatchGeometryRebuildCount() const;
    // External quality ceiling driven by the adaptive Forward Extensions tier.
    // 1.0 removes the cap and restores the unmodified controller behaviour.
    void SetAdaptiveQualityCeiling(float ceiling);
    float GetQualityScale() const;
    uint32_t GetAwakeActorCount() const;
    uint32_t GetLowMotionActorCount() const;
    uint32_t GetSpatialBatchCount() const;
    uint32_t GetCollisionLodActorCount() const;
    uint32_t GetFrozenActorCount() const;
    bool IsBatchBuildPending() const;
    const std::vector<DestructionRenderItem>& GetRenderItems() const;
    const std::vector<DestructionRenderBatch>& GetRenderBatches() const;
    const std::vector<RagdollRenderItem>& GetRagdollRenderItems() const;
    std::vector<RagdollPhysicsDebugShape> GetRagdollPhysicsDebugShapes() const;
    const std::vector<EnemyGunRenderItem>& GetEnemyGunRenderItems() const;
    DestructionDebugData GetDebugData() const;

    void receive(const Nv::Blast::TkEvent* events, uint32_t eventCount) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

extern DestructionDX12 g_destruction;
