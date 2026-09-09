#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// ?? globals ??????????????????????????????????????????????????????????????????
static unsigned int SCR_WIDTH  = 1920;
static unsigned int SCR_HEIGHT = 1080;

static Scene               scene;
static ShaderDX12           mainShader;
ImpactParticleRendererDX12  g_particleRenderer;
RainRendererDX12            g_rainRenderer;
CollisionDebugRendererDX12  g_collisionDebugRenderer;
// Draws the volumes ResolvePlayerPrefabCollisions actually pushes the player out
// of. Off by default; toggled from the debug UI.
bool                        g_showCollisionDebug = false;
ProfilerDX12                g_profiler;
static bool                 g_profileDumpEnabled = false;
static bool                 g_profileDumpWritten = false;
static UINT                 g_profileDumpFrame = 0;
static bool                 g_forceTerrainErrorLOD = false;
UINT                        g_forwardDrawCalls = 0;
UINT                        g_shadowDrawCalls = 0;
UINT                        g_visibilityDrawCalls = 0;
UINT                        g_shadowBatches = 0;
UINT                        g_shadowBatchInstances = 0;
UINT                        g_shadowCachedFarCascades = 0;
UINT                        g_shadowRefreshedFarCascades = 0;
MeshShaderDX12              g_meshShader;
bool                        g_useMeshShader = false;
TerrainRendererDX12         g_terrain;
DestructionDX12             g_destruction;
static GameRuntime          g_game;
static EnemySystem          g_enemySystem;
static std::vector<std::unique_ptr<SkinnedEnemy>>& g_bandits =
    g_enemySystem.actors;
static SkinnedEnemy*&       g_heldBandit = g_enemySystem.held;
static size_t&              g_heldBarrelIndex = g_game.combat.heldBarrelIndex;
// Authored C4 brick, replacing the procedural boxes the charge used to be
// drawn as. Shared by the viewmodel, the thrown charge and the placed one.
std::shared_ptr<SceneNode>  g_c4Model;
std::shared_ptr<SceneNode>  g_explosiveBarrelModel;
std::shared_ptr<SceneNode>  g_explosiveBarrelShadowModel;
std::shared_ptr<SceneNode>  g_humveeModel;
std::shared_ptr<SceneNode>  g_humveeShadowModel;
std::shared_ptr<SceneNode>  g_helicopterModel;
std::shared_ptr<SceneNode>  g_boatModel;
std::shared_ptr<SceneNode>  g_boatShadowModel;
// Insertion boat: same hull asset as the patrol boat, drawn with its own pose.
std::shared_ptr<SceneNode>  g_insertionBoatModel;
std::shared_ptr<SceneNode>  g_insertionBoatShadowModel;
// Mirrors g_blackHawkInsertionRestartPending: aiming the run needs the settled
// player spawn and terrain, which are not ready at model load time.
static bool                 g_insertionBoatRestartPending = false;
// Which airframe flies the insertion. Both helicopter runs (landing and fast
// rappel) can use either, so this is deliberately separate from
// LevelInsertionMode rather than doubling that enum -- the aircraft and the
// manner of arrival are independent choices.
enum class InsertionAirframe : int {
    // The rigged UH-60. Its rotor is skinned to a bone and turns in flight.
    BlackHawk = 0,
    // The current airframe. Its rotor disc is a separate mesh parented to a
    // 'Bone' node, so it turns by rotating that node rather than through the
    // skinning palette -- see g_newBlackHawkRotorNode.
    NewBlackHawk = 1
};
static InsertionAirframe    g_insertionAirframe = InsertionAirframe::NewBlackHawk;
// Both airframes stay resident once loaded. g_blackHawkModel below points at
// whichever is selected, so every consumer -- the draw, the bounds, the ride
// point, the collision bake -- keeps working through the existing pointer with
// no per-site branching.
std::shared_ptr<SceneNode>  g_blackHawkAirframeModel[2];
Skeleton                    g_blackHawkAirframeSkeleton[2];
std::shared_ptr<SceneNode>  g_blackHawkModel;
std::shared_ptr<SceneNode>  g_blackHawkShadowModel;
// Rig for the rotor: the GLB's skin, the id of the joint driving the blades,
// and the bone palette the skinning shaders read. Empty for an unrigged model,
// which just leaves the blades static.
// Raw mesh-space position of the PlayerRide empty, kept for the debug readout.
XMFLOAT3                    g_blackHawkRideMeshPosition{};
// Set when a level starts or restarts; consumed once the player spawn and
// terrain are settled, which is when the insertion route can be aimed. Deferred
// because a cold load has no BlackHawk model yet, while a restart has the model
// but never re-runs the load stages.
static bool                 g_blackHawkInsertionRestartPending = false;
Skeleton                    g_blackHawkSkeleton;
static int                  g_blackHawkRotorBone = -1;
// The NewBlackHawk's rotor. That GLB carries a skin whose single joint drives
// nothing -- neither mesh node references the skin and no primitive has
// JOINTS_0/WEIGHTS_0 -- so the palette path cannot turn its blades. What it does
// have is the disc as its own mesh parented to a 'Bone' node, which is cheaper
// anyway: one quaternion write a frame against a node the draw already reads
// (DrawSceneNodeMesh uses globalTransform per node), versus rebuilding and
// uploading a palette. Null for the skinned UH-60, which keeps the palette path.
static std::shared_ptr<SceneNode> g_newBlackHawkRotorNode;
std::vector<XMFLOAT4X4>     g_blackHawkPalette;
static ComPtr<ID3D12Resource> g_blackHawkPaletteBuffer[FRAME_COUNT];
static void*                g_blackHawkPaletteMapped[FRAME_COUNT] = {};
static UINT                 g_blackHawkPaletteBytes = 0;
std::shared_ptr<SceneNode>  g_dandelionModel;
std::vector<DandelionInstance> g_dandelionInstances;
static XMFLOAT3             g_dandelionSourceCenter{};
static float                g_dandelionSourceMinY = 0.0f;
static float                g_dandelionSourceHeight = 1.0f;
// Temporary translation-unit aliases keep the large prototype game loop
// readable while RuntimeWorld remains the sole owner of compiled prefab state.
static std::vector<PrefabRenderBatch>& g_prefabRenderBatches =
    g_game.world.Prefabs().renderBatches;
static std::vector<PrefabCollider>& g_prefabColliders =
    g_game.world.Prefabs().colliders;
static std::vector<CollisionMeshInstance>& g_prefabMeshColliders =
    g_game.world.Prefabs().meshColliders;
// Entities that own a mesh collider. Both colliders are emitted per entity, so
// the bounds box would otherwise shadow the geometry it wraps: the box is hit at
// or before the true surface, and nearest-wins would stop every shot at the
// airport's outer shell. Segment queries skip the box for these entities;
// fire spread and the navmesh deliberately do not, which is how they keep the
// conservative volume they want.
static std::unordered_set<uint64_t> g_meshCollisionEntities;
static std::vector<PrefabLightInstance>& g_prefabLightInstances =
    g_game.world.Prefabs().lights;
static std::vector<PrefabAudioEmitter>& g_prefabAudioEmitters =
    g_game.world.Prefabs().audioEmitters;
static std::vector<PrefabSpawnPoint>& g_prefabSpawnPoints =
    g_game.world.Prefabs().spawnPoints;
static std::vector<PrefabArmoryShop>& g_prefabArmoryShops =
    g_game.world.Prefabs().armoryShops;
static std::vector<PrefabTravelPoint>& g_prefabTravelPoints =
    g_game.world.Prefabs().travelPoints;
static std::vector<PrefabDestructibleInstance>& g_prefabDestructibles =
    g_game.world.Prefabs().destructibles;
static std::unordered_map<uint64_t, float>& g_prefabHealth =
    g_game.world.Prefabs().health;
struct PrefabAudioPlayer {
    size_t emitterIndex = 0;
    std::unique_ptr<GunAudio> player;
};
static std::vector<PrefabAudioPlayer> g_prefabAudioPlayers;
static PrefabRegistry       g_prefabRegistry;
// Canonical asset roots, matching what AssetRegistry scans. PrefabRegistry's
// own defaults ("prefabs"/"models") describe a layout this project does not
// use, so refreshing without these scans two empty directories and every
// prefab lookup misses.
static const std::filesystem::path kPrefabRoot = "Content/Prefabs";
static const std::filesystem::path kModelRoot = "Content/Models";
// The comm tower is a steel lattice ~26 m tall, so it neither sounds nor dies
// like the rocks that share the prefab path: bullets ring off it, and bringing it
// down is a landmark event rather than a puff of dust. Identified by prefab id so
// no new entity type or engine plumbing is needed.
static constexpr const char* kCommTowerPrefabId = "props/comm_tower";
static constexpr const char* kFencePrefabId = "props/fence";
static constexpr const char* kFencePanelPrefabId = "props/fence_panel";
// Both fence assets are single authored panels of the same size and build, so
// they share the runtime-cut break path rather than the banded tower one.
static bool IsFencePrefab(const std::string& prefabId) {
    return prefabId == kFencePrefabId || prefabId == kFencePanelPrefabId;
}
// The watchtower is deliberately absent: it is a fixed piece of level
// architecture that stays standing, so it draws and collides as an ordinary
// static prefab rather than entering the destruction model at all.
static bool IsNvBlastStructurePrefab(const std::string& prefabId) {
    return prefabId == kCommTowerPrefabId || IsFencePrefab(prefabId);
}
// Defined with the prefab damage code further down; needed earlier by the
// burning-material tick, which must not let fire fell a player objective.
static bool FindCommTower(uint64_t entityId, DirectX::XMFLOAT3& base);

// Timed objective: an aircraft the player has to destroy before it leaves.
// Identified by prefab id, exactly like the comm tower, so it needs no new
// LevelEntityType and no editor work -- drop the prefab into a level and it is
// an objective.
//
// The source GLB carries no animation or skin (verified: 38 static mesh nodes,
// zero animation channels), and GLBImporter loads meshes and skins only. The
// takeoff is therefore driven procedurally off the batch's draw transform:
// hold, accelerate down the runway, rotate, then climb straight out.
static constexpr const char* kObjectivePlanePrefabId = "props/objective_plane";
// Time from level start until the aircraft begins its roll.
static constexpr float kObjectivePlaneHoldSeconds = 120.0f;
// How long the roll/rotate/climb takes once started. After this it is gone.
static constexpr float kObjectivePlaneTakeoffSeconds = 14.0f;
static constexpr float kObjectivePlaneTaxiSpeed = 34.0f;   // m/s at rotation
static constexpr float kObjectivePlaneClimbRate = 11.0f;   // m/s once airborne
// Crash-out, matching the Black Hawk's numbers so a downed aircraft reads the
// same way: gravity pulls it in while it tumbles, and it is on the ground in a
// couple of seconds rather than gliding away.
static constexpr float kObjectivePlaneCrashGravity = 9.81f;
static constexpr float kObjectivePlaneCrashPitchRate = 0.42f;
static constexpr float kObjectivePlaneCrashRollRate = 0.78f;
static constexpr float kObjectivePlaneCrashYawRate = 0.55f;
// Bleed off forward speed on the way down so it plants near where it was hit
// instead of carrying its full takeoff velocity across the map.
static constexpr float kObjectivePlaneCrashDrag = 0.6f;

struct ObjectivePlaneState {
    uint64_t entityId = 0;
    // Authored placement, captured once so the flight path is relative to it and
    // a rebuild of the render batches cannot make the aircraft jump.
    //
    // XMFLOAT4X4, not XMMATRIX. XMMATRIX is four __m128 and needs 16-byte
    // alignment; putting one after a uint64_t inside a struct held in a
    // std::vector puts it at offset 8, and the aligned SIMD load then faults.
    // (PrefabRenderBatch gets away with std::vector<XMMATRIX> because there the
    // matrix IS the element type, so the allocation is aligned to it.) Load with
    // XMLoadFloat4x4 at the point of use, which is the unaligned-safe path and
    // the convention the rest of this file already follows for stored matrices.
    DirectX::XMFLOAT4X4 baseTransform{};
    DirectX::XMFLOAT3 basePosition{};
    // Unit horizontal heading the aircraft rolls along, taken from the model's
    // own long axis rotated by the authored placement. Stored as a vector, not
    // an angle: the takeoff translates along it directly, so there is no
    // sin/cos reconstruction to disagree with the model's actual facing.
    DirectX::XMFLOAT3 forward{ 0.0f, 0.0f, 1.0f };
    float holdTimer = 0.0f;
    float takeoffTimer = 0.0f;
    bool rolling = false;
    bool escaped = false;
    bool destroyed = false;
    // Crash state, mirroring the Black Hawk's: once hit the airframe keeps its
    // last pose and falls under gravity along the velocity it was carrying,
    // tumbling, until it reaches the terrain under it.
    bool crashing = false;
    bool crashed = false;
    DirectX::XMFLOAT3 crashPosition{};
    DirectX::XMFLOAT3 crashVelocity{};
    float crashPitch = 0.0f;
    float crashRoll = 0.0f;
    float crashYaw = 0.0f;
    float crashGroundY = 0.0f;
    // Half the model's larger horizontal extent, in world units. The wreck
    // settles this far above the terrain instead of dropping its origin onto it:
    // the origin sits inside the fuselage, so resting it on the ground buried
    // the airframe up to the wings.
    float groundClearance = 1.0f;
    // How far the airframe reaches horizontally from its origin, in world units,
    // and how far it reaches vertically.
    //
    // Explosives are tested against these rather than against the origin alone.
    // The generic blast path (CombatSystem::DamagePrefabsInRadius) measures
    // centre-to-entity-origin, which works for a crate but not for an aircraft:
    // this asset spans 29.0 m across the wings and 22.3 m down the fuselage, so
    // the skin is up to ~14.5 m from the origin while a C4 blast reaches 5.4 m.
    // A charge stuck to a wing or the tail therefore missed the test entirely
    // and the plane shrugged off a demolition charge planted directly on it.
    float blastReachHorizontal = 7.0f;
    float blastReachVertical = 4.0f;
};
static std::vector<ObjectivePlaneState> g_objectivePlanes;
// Set when an aircraft clears the map, so the mission can report the failure.
static bool g_objectivePlaneEscaped = false;

// One prefab placement that simulates as a rigid body instead of standing
// still, created for any prefab carrying a "rigidBody" component.
//
// The body owns the pose once it exists, so this keeps what the body cannot
// rebuild for itself: the authored scale/shear that has to be re-applied on top
// of the simulated rotation, and the entity id used to find the render instance
// and collider again after a prefab rebuild.
struct PrefabRigidBodyState {
    uint64_t entityId = 0;
    std::string prefabId;
    uint32_t physicsHandle = 0;
    // Authored transform with translation and yaw stripped out, so composing it
    // under the body's pose restores the placement's scale without fighting the
    // simulated position. Same alignment reasoning as ObjectivePlaneState.
    DirectX::XMFLOAT4X4 scaleTransform{};
    // Bounds centre in local space, scaled. The body is created at the centre of
    // the collision box, but the model draws from its own origin, which for the
    // container sits at a corner -- so the offset is carried here and undone
    // when the pose is written back.
    DirectX::XMFLOAT3 centerOffset{};
    DirectX::XMFLOAT3 halfExtents{};
    // Skips the transform write while the body sleeps, which is the common case
    // for a yard of containers nobody has touched.
    bool asleep = false;
};
static std::vector<PrefabRigidBodyState> g_prefabRigidBodies;
static AssetRegistry        g_assetRegistry;
static AssetWatcher         g_assetWatcher;
struct PrefabModelCacheEntry {
    std::vector<PrefabRenderBatch::LodModel> automaticLods;
    std::shared_ptr<SceneNode> model;
    XMFLOAT3 boundsMinimum{};
    XMFLOAT3 boundsMaximum{};
    // Per-triangle collision, built only for prefabs whose collision shape is
    // "mesh". Held by shared_ptr because RetiredPrefabResources moves this whole
    // map into a deferred-release queue: instances borrow a raw pointer into the
    // tree and must stay valid across that retire.
    std::shared_ptr<CollisionMesh> collisionMesh;
};
static std::unordered_map<std::string, PrefabModelCacheEntry> g_prefabModelCache;
// A source model can be rejected after its importer has already recorded GPU
// uploads (the authored AA turret's missing pose node is one example). Keep such
// models alive until the cold-load fence drain instead of deleting resources
// still referenced by the open command list.
static std::vector<std::shared_ptr<SceneNode>> g_rejectedUploadModels;
struct RetiredPrefabResources {
    std::vector<PrefabRenderBatch> renderBatches;
    std::unordered_map<std::string, PrefabModelCacheEntry> models;
};
static DeferredReleaseQueue<RetiredPrefabResources>
    g_retiredPrefabResources;
static std::unordered_set<std::string> g_prefabMeshFallbackWarnings;
static bool                 g_prefabRebuildRequested = true;
// Which call site last asked for a rebuild. A full rebuild reloads every model
// and re-inits every audio player, so when one shows up in a frame spike the
// first question is always who asked for it -- and the flag alone cannot say.
// Points at a string literal; never freed.
static const char*          g_prefabRebuildReason = "startup";
static bool                 g_prefabRuntimeSmokeEnabled = false;
static bool                 g_prefabRuntimeSmokeChecked = false;
// SGE_COLLISION_TEST: load the Training Range, report the airport's collision
// tree once the prefab rebuild settles, then quit. The interesting numbers are
// logged by LoadPrefabModel itself; this only decides when to stop.
static bool                 g_collisionSmokeEnabled = false;
static bool                 g_collisionSmokeChecked = false;
static UINT                 g_collisionBudgetFrames = 0;
// SGE_TRAVEL_TEST: load the Base, walk the player onto the transport Black
// Hawk's boarding point, and drive the destination board through the same
// functions the E key does -- proximity, open, select, level swap. The point is
// that a screenshot cannot say whether the prompt logic and the departure
// actually fire, so each stage logs its own answer and the exit code carries
// the verdict.
static bool                 g_travelSmokeEnabled = false;
static int                  g_travelSmokeStage = 0;
static UINT                 g_travelSmokeFrames = 0;
// SGE_SHOTGUN_TEST: fire buckshot into the helideck, the terrain and the
// transport helicopter from the frame loop. The shotgun is the only weapon that
// puts eight projectiles in the air per trigger pull, so it is the one that
// exercises many simultaneous hits resolving against the same geometry in a
// single frame -- which is what the crash report describes.
static bool                 g_shotgunSmokeEnabled = false;
static UINT                 g_shotgunSmokeFrames = 0;
static UINT                 g_shotgunSmokeShots = 0;
static bool                 g_ddgiCornellTestMode = false;
static bool                 g_ddgiCornellPreviousTemporalEffects = false;
static bool                 g_ddgiCornellPreviousAnimateDemoLights = true;
static constexpr XMFLOAT3   g_ddgiCornellLightPosition =
    { 0.0f, 6.55f, 3.6f };
static constexpr XMFLOAT3   g_ddgiCornellLightColor =
    { 1.0f, 0.86f, 0.66f };
static constexpr float      g_ddgiCornellLightRadius = 11.0f;
static constexpr float      g_ddgiCornellLightIntensity = 24.0f;
static std::shared_ptr<SceneNode> g_ddgiCornellModel;
std::shared_ptr<SceneNode>  g_helicopterMainRotorNode;
std::shared_ptr<SceneNode>  g_helicopterTailRotorNode;
// The reinforcement dropship gets its own copy of the airframe, so its rotors
// turn on their own angle instead of borrowing the patrol gunship's nodes. A
// shallow clone shares every vertex buffer and material with the original --
// only the node transforms are independent, which is exactly the difference
// that matters. Without this, one dead helicopter froze BOTH sets of blades,
// because a single pair of rotor nodes was being posed for two aircraft.
std::shared_ptr<SceneNode>  g_secondaryHelicopterModel;
std::shared_ptr<SceneNode>  g_secondaryHelicopterMainRotorNode;
std::shared_ptr<SceneNode>  g_secondaryHelicopterTailRotorNode;
static float                g_secondaryHelicopterRotorSpeedScale = 1.0f;
static float                g_secondaryHelicopterMainRotorAngle = 0.0f;
static float                g_secondaryHelicopterTailRotorAngle = 0.0f;
std::shared_ptr<SceneNode>  g_humveeTurretNode;
static XMFLOAT3&            g_humveeModelCenter = g_game.vehicles.humveeModelCenter;
static float&               g_humveeModelMinY = g_game.vehicles.humveeModelMinY;
static float&               g_humveeModelScale = g_game.vehicles.humveeModelScale;
static XMFLOAT3&            g_helicopterModelCenter = g_game.vehicles.helicopterModelCenter;
static float&               g_helicopterModelScale = g_game.vehicles.helicopterModelScale;
static float&               g_helicopterLevelScale = g_game.vehicles.helicopterLevelScale;
static float&               g_helicopterMainRotorAngle = g_game.vehicles.helicopterMainRotorAngle;
static float&               g_helicopterTailRotorAngle = g_game.vehicles.helicopterTailRotorAngle;
static float&               g_helicopterRotorSpeedScale =
    g_game.vehicles.helicopterRotorSpeedScale;
static float&               g_helicopterYaw = g_game.vehicles.helicopterYaw;
static float&               g_helicopterPitch = g_game.vehicles.helicopterPitch;
static float&               g_helicopterRoll = g_game.vehicles.helicopterRoll;
static float&               g_helicopterHoverTime = g_game.vehicles.helicopterHoverTime;
static float&               g_helicopterFireCooldown = g_game.vehicles.helicopterFireCooldown;
static float&               g_helicopterFireCycleTime = g_game.vehicles.helicopterFireCycleTime;
static XMFLOAT3&            g_helicopterPosition = g_game.vehicles.helicopterPosition;
static XMFLOAT3&            g_helicopterSpawn = g_game.vehicles.helicopterSpawn;
static XMFLOAT3&            g_secondaryHelicopterPosition =
    g_game.vehicles.secondaryHelicopterPosition;
constexpr float             kHelicopterPatrolRadius = 16.0f;
constexpr float             kHelicopterEngagementRange = 90.0f;
static float&               g_secondaryHelicopterYaw = g_game.vehicles.secondaryHelicopterYaw;
static float&               g_secondaryHelicopterPitch = g_game.vehicles.secondaryHelicopterPitch;
static float&               g_secondaryHelicopterRoll = g_game.vehicles.secondaryHelicopterRoll;
static float&               g_secondaryHelicopterHoverTime = g_game.vehicles.secondaryHelicopterHoverTime;
static float&               g_secondaryHelicopterFireCooldown =
    g_game.vehicles.secondaryHelicopterFireCooldown;
static float&               g_secondaryHelicopterFireCycleTime =
    g_game.vehicles.secondaryHelicopterFireCycleTime;
constexpr float             kHelicopterMaxHealth = VehicleSystem::HelicopterMaxHealth;
constexpr float             kRocketHelicopterDamage = kHelicopterMaxHealth * 0.5f;
static float&               g_secondaryHelicopterHealth = g_game.vehicles.secondaryHelicopterHealth;
static bool&                g_secondaryHelicopterDead = g_game.vehicles.secondaryHelicopterDead;
static bool&                g_secondaryHelicopterCrashed = g_game.vehicles.secondaryHelicopterCrashed;
static XMFLOAT3&            g_secondaryHelicopterCrashVelocity =
    g_game.vehicles.secondaryHelicopterCrashVelocity;
static XMFLOAT3&            g_secondaryHumveePosition = g_game.vehicles.secondaryHumveePosition;
static float&               g_helicopterHealth = g_game.vehicles.helicopterHealth;
static bool&                g_helicopterDead = g_game.vehicles.helicopterDead;
static bool&                g_helicopterCrashed = g_game.vehicles.helicopterCrashed;
static XMFLOAT3&            g_helicopterCrashVelocity = g_game.vehicles.helicopterCrashVelocity;
static XMFLOAT3&            g_humveeTurretLocal = g_game.vehicles.humveeTurretLocal;
static bool&                g_drivingHumvee = g_game.vehicles.drivingHumvee;
static bool&                g_savedGunVisible = g_game.vehicles.savedGunVisible;
static XMFLOAT3&            g_previousHumveePosition = g_game.vehicles.previousHumveePosition;
static bool&                g_previousHumveePositionValid =
    g_game.vehicles.previousHumveePositionValid;
static float&               g_humveeHouseImpactCooldown = g_game.vehicles.humveeHouseImpactCooldown;
static XMFLOAT3&            g_humveeAimPoint = g_game.vehicles.humveeAimPoint;
static float&               g_humveeTurretYaw = g_game.vehicles.humveeTurretYaw;
static float&               g_humveeTurretFireCooldown = g_game.vehicles.humveeTurretFireCooldown;
static XMFLOAT3             g_boatModelCenter{};
static float                g_boatModelMinY = 0.0f;
static float                g_boatModelScale = 1.0f;
static XMFLOAT3&            g_blackHawkModelCenter = g_game.vehicles.blackHawkModelCenter;
static float&               g_blackHawkModelMinY = g_game.vehicles.blackHawkModelMinY;
static float&               g_blackHawkModelScale = g_game.vehicles.blackHawkModelScale;
static XMFLOAT3&            g_blackHawkPosition = g_game.vehicles.blackHawkPosition;
// Height above the drop-off below which the insertion's rotor wash reaches the
// ground. Slightly above the 7 m touchdown hover, so the grass is already
// flattening as it settles rather than snapping flat on arrival.
constexpr float             kBlackHawkWashHeight = 26.0f;
static float&               g_blackHawkYaw = g_game.vehicles.blackHawkYaw;
static float&               g_blackHawkRotorSpin = g_game.vehicles.blackHawkRotorSpin;
static XMFLOAT3&            g_boatPosition = g_game.vehicles.boatPosition;
static XMFLOAT3&            g_boatCenter = g_game.vehicles.boatCenter;
static float&               g_boatYaw = g_game.vehicles.boatYaw;
static float&               g_boatRoll = g_game.vehicles.boatRoll;
static float&               g_boatPatrolTime = g_game.vehicles.boatPatrolTime;
static float&               g_boatHealth = g_game.vehicles.boatHealth;
static bool&                g_boatDead = g_game.vehicles.boatDead;
static bool&                g_boatSunk = g_game.vehicles.boatSunk;
static float&               g_boatSinkDepth = g_game.vehicles.boatSinkDepth;
constexpr float              kBoatMaxHealth = VehicleSystem::BoatMaxHealth;
// Shoreline (TerrainRendererDX12 island falloff) flattens into beach around
// 28-43 units out and only reaches open seabed past ~88. Patrol well clear of
// the beach/surf so the boat reads as sailing open water, not beached.
constexpr float              kBoatPatrolRadius = 60.0f;
constexpr float              kBoatDeckHalfBeam = 1.5f;
constexpr float              kBoatDeckHalfLength = 4.5f;
constexpr float              kBoatHullHeight = 1.1f;
constexpr float              kBoatDeckOffset = 0.10f;
SkinnedModel                g_banditModel;
bool                        g_banditLoaded = false;
SkinnedModel                g_marineModel;
float                       g_banditLeftArmReach = 0.85f;
float                       g_banditHeadYawOffsetDegrees = 20.4f;
// Rifle grip tuning, pushed to every bandit each frame so the sliders in
// BanditDebugText retune the hold live. Defaults mirror SkinnedEnemy's.
float                       g_banditGunScale = 0.62f;
float                       g_banditGunGripForward = -0.183f;
float                       g_banditGunGripRise = -0.04f;
float                       g_banditGunRearGripForward = 0.16f;
float                       g_banditGunRearGripInboard = -0.06f;
float                       g_banditGunRearGripDrop = -0.18f;
float                       g_banditGunForeGripLateral = 0.253f;
float                       g_banditGunForeGripRise = -0.206f;
bool                        g_showEnemyVisionCones = false;
// Impact decal debug. The marks are a per-pixel volume test with no geometry of
// their own, so when one lands wrong there is nothing to inspect -- this draws
// the discs the shader is actually testing against.
bool                        g_showDecalDebug = false;
// Bullet-hole rendering is experimental and bindless-only. Keep it opt-in so
// the normal bindless material path does not depend on the decal resources.
bool                        g_impactDecalsEnabled = false;
bool                        g_impactDecalCutouts = false;
// Freeze ageing so a mark can be walked around and inspected without it fading
// out mid-investigation.
bool                        g_freezeDecalAging = false;
// Enemy scatter test mode. When on, every bandit is moved to a random walkable
// point on the navmesh once the squad is spawned and before the player deploys,
// so a run does not always open against the same authored EnemySpawn layout.
// Off by default: it deliberately ignores level authoring.
bool                        g_scatterEnemiesOnNavmesh = false;
// 0 = reseed from the clock on every scatter. Any other value is used verbatim,
// so a layout that produced an interesting run can be replayed exactly.
unsigned int                g_scatterEnemiesSeed = 0;
// The seed the last scatter actually ran with, surfaced in the debug UI so a
// clock-seeded layout can be pinned after the fact.
unsigned int                g_scatterEnemiesLastSeed = 0;
// Seed of the last deployment-screen randomize roll. Distinct from the scatter
// seed above: this one covers the whole roll (time, weather, fog and layout),
// and the scatter seed is one value derived from it. 0 = never rolled.
unsigned int                g_lastDeploymentRollSeed = 0;
// Time of day for the next run, chosen on the deployment screen and applied at
// DEPLOY. Afternoon is the look every level shipped with before the choice
// existed, so it stays the default.
TimeOfDay                   g_selectedTimeOfDay = TimeOfDay::Afternoon;
// Player-tunable volumetric fog, kept per time of day. Each preset authors a
// look that only holds together as a set (a night density over a noon sun reads
// as smog), so an edit made under one sun must not follow the player to
// another: every time of day carries its own override, seeded from that
// preset's authored values and edited independently.
struct VolumetricFogSettings {
    bool enabled;
    float density;
    float anisotropy;
    float heightFalloff;
    float baseHeight;
    float distance;
    XMFLOAT3 tint;
};

static VolumetricFogSettings MakeDefaultVolumetricFogSettings(TimeOfDay time) {
    const TimeOfDaySettings preset = MakeTimeOfDaySettings(time);
    return {preset.enableVolumetricFog,
            preset.volumetricFogDensity,
            preset.volumetricFogAnisotropy,
            preset.volumetricFogHeightFalloff,
            preset.volumetricFogBaseHeight,
            preset.volumetricFogDistance,
            preset.volumetricFogTint};
}

constexpr int kTimeOfDayCount = 4;

// Indexed by TimeOfDay. Written by the deployment-screen sliders and read back
// by ApplyTimeOfDay, so a tuned look survives switching away and back.
VolumetricFogSettings g_volumetricFogByTime[kTimeOfDayCount] = {
    MakeDefaultVolumetricFogSettings(TimeOfDay::Noon),
    MakeDefaultVolumetricFogSettings(TimeOfDay::Afternoon),
    MakeDefaultVolumetricFogSettings(TimeOfDay::Dusk),
    MakeDefaultVolumetricFogSettings(TimeOfDay::Night),
};

static VolumetricFogSettings& VolumetricFogFor(TimeOfDay time) {
    const int index = static_cast<int>(time);
    return g_volumetricFogByTime[
        (index >= 0 && index < kTimeOfDayCount) ? index
                                                : static_cast<int>(TimeOfDay::Afternoon)];
}

// Pushes one time's fog override onto the live scene. Used both by
// ApplyTimeOfDay and by the deployment sliders, which edit the currently
// selected time and expect the change to show immediately.
static void ApplyVolumetricFogSettings(const VolumetricFogSettings& fog) {
    scene.enableVolumetricFog = fog.enabled;
    scene.volumetricFogDensity = fog.density;
    scene.volumetricFogAnisotropy = fog.anisotropy;
    scene.volumetricFogHeightFalloff = fog.heightFalloff;
    scene.volumetricFogBaseHeight = fog.baseHeight;
    scene.volumetricFogDistance = fog.distance;
    scene.volumetricFogTint = fog.tint;
}
// Draws every live bandit as a red dot on the deployment map. On by default:
// the point of randomising the squad is being able to see what you rolled
// before committing to a zone.
bool                        g_showEnemyDotsOnDeployScreen = true;
// Friendly marines, as blue dots. Separate from the hostile toggle: turning off
// the enemy intel to plan a blind insertion should not also hide your own squad.
bool                        g_showAllyDotsOnDeployScreen = true;
static uint32_t&            g_banditSpawnSerial = g_enemySystem.spawnSerial;
// Impact decals: bullet holes and scorch marks, oldest evicted once the buffer
// is full. 64 matches the shader's array, and the whole list is re-uploaded each
// frame -- at this size that is cheaper than tracking dirty ranges.
// ImpactDecal itself is declared in ForwardRenderer.h, which uploads the list.
std::vector<ImpactDecal>    g_impactDecals;
constexpr size_t            kMaxImpactDecals = 64;
// Marks stay for a firefight, then fade over the last quarter of their life so
// they disappear without popping. Non-constexpr so the renderer can extern it.
extern const float          kImpactDecalLifetime;
const float                 kImpactDecalLifetime = 45.0f;

static void SpawnImpactDecal(const XMFLOAT3& position, const XMFLOAT3& normal,
                             float radius, float collisionRadius = 0.0f,
                             bool attachToDestructible = false) {
    if (!g_impactDecalsEnabled) return;
    const float lengthSq = normal.x * normal.x + normal.y * normal.y +
                           normal.z * normal.z;
    if (lengthSq < 1e-6f) return;   // no surface to project onto
    const float inv = 1.0f / std::sqrt(lengthSq);
    ImpactDecal decal;
    // The swept-sphere collision reports the projectile centre when its radius
    // first touches the inflated chunk bounds. Advance by that radius to reach
    // the visible surface, then bias the decal volume slightly into it.
    const float surfaceOffset = collisionRadius + 0.005f;
    decal.position = { position.x - normal.x * inv * surfaceOffset,
                       position.y - normal.y * inv * surfaceOffset,
                       position.z - normal.z * inv * surfaceOffset };
    decal.normal = { normal.x * inv, normal.y * inv, normal.z * inv };
    decal.radius = radius;
    decal.age = 0.0f;
    if (attachToDestructible)
        g_destruction.CaptureLastHitAttachment(
            decal.position, decal.normal, decal.parent);
    if (g_impactDecals.size() >= kMaxImpactDecals)
        g_impactDecals.erase(g_impactDecals.begin());
    g_impactDecals.push_back(decal);
}

// One place builds the GPU list; the forward path and the visibility resolve
// both upload the same thing, so a mark never appears in one renderer only.
std::vector<ImpactDecalDataDX12> BuildImpactDecalGPUList() {
    std::vector<ImpactDecalDataDX12> out;
    if (!g_impactDecalsEnabled) return out;
    out.reserve(g_impactDecals.size());
    for (const ImpactDecal& decal : g_impactDecals) {
        const float life = decal.age / kImpactDecalLifetime;
        const float strength = life < 0.75f
            ? 1.0f : 1.0f - (life - 0.75f) / 0.25f;
        if (strength <= 0.001f) continue;
        ImpactDecalDataDX12 gpu;
        gpu.position = decal.position;
        gpu.radius = decal.radius;
        gpu.normal = decal.normal;
        gpu.strength = strength;
        out.push_back(gpu);
    }
    return out;
}

static void UpdateImpactDecals(float dt) {
    if (!g_impactDecalsEnabled) {
        g_impactDecals.clear();
        return;
    }
    for (ImpactDecal& decal : g_impactDecals) {
        if (decal.parent.IsValid() &&
            !g_destruction.ResolveAttachment(
                decal.parent, decal.position, decal.normal)) {
            // The owning chunk was removed (for example by a hard laser cut),
            // so the mark leaves with it rather than hanging in mid-air.
            decal.age = kImpactDecalLifetime;
            continue;
        }
        if (!g_freezeDecalAging) decal.age += dt;
    }
    g_impactDecals.erase(
        std::remove_if(g_impactDecals.begin(), g_impactDecals.end(),
            [](const ImpactDecal& d) { return d.age >= kImpactDecalLifetime; }),
        g_impactDecals.end());
}

GunAudio                    g_gunAudio;
GunAudio                    g_rpgFireAudio;
GunAudio                    g_reloadAudio;
GunAudio                    g_explosionAudio;
GunAudio                    g_grenadeExplosionAudio;
GunAudio                    g_fireLoopAudio;
GunAudio                    g_fireIgnitionAudio;
std::array<GunAudio, 3>     g_destructionBreakAudio;
std::array<GunAudio, 3>     g_destructionImpactAudio;
static float                g_destructionBreakAudioCooldown = 0.0f;
static float                g_destructionImpactAudioCooldown = 0.0f;
struct PendingExplosionAudio {
    float delay = 0.0f;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool grenade = false;
};
std::vector<PendingExplosionAudio> g_pendingExplosionAudio;
GunAudio                    g_hitAudio;
// Bullet striking sheet metal: enemy fire hitting a vehicle hull, or the
// player's rounds hitting a metal roof. Pitch is randomised per shot so a
// sustained burst does not sound like one sample retriggering.
GunAudio                    g_metalHitAudio;
// Metal-hit pitch window. Randomised per shot inside this range so a sustained
// burst reads as many separate impacts rather than one sample retriggering.
static constexpr float      kMetalHitPitchMin = 0.80f;
static constexpr float      kMetalHitPitchMax = 1.00f;
GunAudio                    g_banditSpottedAudio1;
GunAudio                    g_banditSpottedAudio2;
// Commander callout on the deployment planning screen. Voices bus, so the
// dialogue slider moves it, and 2D: it is radio chatter, not a world sound.
GunAudio                    g_readyToDropAudio;
bool                        g_readyToDropPlayed = false;
// Background score for the front end: the main menu and the deployment planning
// screen. Looped rather than one-shot so it covers however long the player
// spends on either, and stopped the moment play begins.
GunAudio                    g_menuMusicAudio;
bool                        g_menuMusicPlaying = false;
constexpr float             kMenuMusicVolume = 0.5f;
// Under gameplay the score sits back so it does not crowd the mix.
constexpr float             kInGameMusicVolume = 0.4f;
// Gain currently applied to the loop, so a change can be detected while the
// track is already running. Negative means nothing has been set yet.
float                       g_menuMusicLevel = -1.0f;
// Set when the deployment callout fires, so the score restarts from the top
// alongside it rather than on a screen edge the player never hears.
bool                        g_menuMusicRestartRequested = false;
// Set once the comm tower comes down: the score opens up to full for the rest
// of the run.
bool                        g_commTowerMusicSwell = false;
// Commander's next order, played on the training range once the guard is down.
// 2D like the deployment callout: it is radio, not a voice in the world.
GunAudio                    g_plantC4TowerAudio;
// Commander confirming the ride is on station. Watches the escape boat's own
// active flag rather than hooking the two placement call sites, so no path that
// spawns an exfil can bring one in silently.
GunAudio                    g_exfilHereAudio;
// Queued when the comm tower falls rather than when the boat appears: the ride
// is called in as a consequence of the objective, so the line lands on the
// collapse instead of waiting for the player to notice a hull offshore.
// Negative means nothing is queued.
float                       g_exfilHereDelay = -1.0f;
constexpr float             kExfilHereDelay = 6.0f;
// Sign-off on the win screen. OpenWinScreen is the one way in, so it plays
// there rather than off a per-frame screen test that would retrigger.
GunAudio                    g_greatJobAudio;
bool                        g_plantC4Played = false;
// A beat between the kill and the order, so the callout does not step on the
// death itself. Negative means nothing is queued.
float                       g_plantC4TowerDelay = -1.0f;
constexpr float             kPlantC4TowerDelay = 1.0f;
GunAudio                    g_banditAttackAudio;
GunAudio                    g_banditDeathAudio;
GunAudio                    g_banditHitVoiceAudio;
GunAudio                    g_helicopterHoverAudio;
// Cockpit alarm on the insertion BlackHawk, looped while it is critically
// damaged so the player hears the failure before they see the ground.
GunAudio                    g_blackHawkAlarmAudio;
// Footstep variations, one GunAudio per sample. A GunAudio owns exactly one
// decoded buffer, so alternation has to come from picking between instances
// rather than from one instance holding a set.
//
// Two recordings is few enough that a plain random pick would audibly repeat
// the same file back to back; PlayFootstep below avoids that, and layers a
// pitch jitter on top so even a repeat is not identical.
constexpr int               kFootstepVariantCount = 2;
GunAudio                    g_footstepAudio[kFootstepVariantCount];
// Which variant played last, so the next step can avoid it.
static int                  g_lastFootstepVariant = -1;
// Distance walked since the last step sound. A stride-length accumulator rather
// than a timer, so footfalls stay locked to actual movement: a timer keeps
// ticking when the player walks into a wall, and desyncs the moment sprint
// changes their speed. Enemies carry their own copy of this on the actor (see
// SkinnedEnemy::stepDistance), which cannot go stale when the list is compacted.
static float                g_playerStepDistance = 0.0f;
// Metres between footfalls. Sprinting lengthens the stride but raises the
// cadence more, so the interval shortens.
static constexpr float      kStepStrideWalk = 2.1f;
static constexpr float      kStepStrideSprint = 2.6f;
// Exhaustion breathing, played when the sprint meter runs dry.
//
// Not spatialised, for the same reason the player's own boots are not: it comes
// from the listener's own head, where panning means nothing.
GunAudio                    g_breathingAudio;
// The clip measures 3.170 s (stereo, 44.1 kHz, decoded with the engine's own
// stb_vorbis path), so a retrigger any sooner would stack a second copy over
// the first. The gain is high because the source is quiet: it peaks at 1636 of
// 32767, about 20x below the shipped effects, so it needs lifting to sit in the
// mix at all. At 5.2 the peak reaches 26% of full scale, still well clear of
// clipping -- GunAudio::kMaxPlayGain had to be raised past 4.0 to allow it.
//
// Pitched down so the breath reads as a heavier, more tired body. XAudio2's
// frequency ratio also stretches the clip, so a 0.8 ratio makes the 3.170 s
// sample play for 3.963 s; the retrigger interval below is derived from the
// stretched length, not the raw one, or breaths would overlap.
static constexpr float      kBreathingClipSeconds = 3.170f;
static constexpr float      kBreathingPitch = 0.80f;
static constexpr float      kBreathingRepeatSeconds =
    kBreathingClipSeconds / kBreathingPitch + 0.20f;
static constexpr float      kBreathingGain = 5.2f;
static float                g_breathingCooldown = 0.0f;
// --- Sprint stamina ---
//
// Held in seconds of meter rather than an abstract 0..100, so the constants
// here stay directly readable. The meter holds kStaminaMaxSeconds and drains at
// kStaminaDrainRate per second, so a continuous sprint lasts
// kStaminaMaxSeconds / kStaminaDrainRate seconds -- 3.0 s at the current
// values. A full refill takes kStaminaMaxSeconds / kStaminaRecoveryRate
// seconds of not sprinting.
static constexpr float      kStaminaMaxSeconds = 6.0f;
// Meter-seconds spent per real second of sprinting. At 2.0 the bar empties in
// half the time it takes to refill a comparable amount, which is what makes a
// sprint a decision rather than a default. Raised from 1.0 (a plain dt drain).
static constexpr float      kStaminaDrainRate = 2.0f;
// Recovery is slower than the drain, so a chased player cannot tap shift
// forever, but it is not so slow that a normal traversal is spent walking.
static constexpr float      kStaminaRecoveryRate = 0.55f;
// Grace before recovery starts, so releasing shift for a moment mid-fight does
// not immediately refund the sprint.
static constexpr float      kStaminaRecoveryDelay = 1.1f;
// Once empty, sprinting is locked out until this much has been rebuilt. Without
// it the player would flutter in and out of sprint one frame at a time the
// instant the meter touched zero.
static constexpr float      kStaminaRecoveredToSprint = 1.5f;
// Breathing starts once the meter falls under half, before it is spent, so the
// player hears the exertion building rather than only hearing the moment they
// run dry. Expressed as a fraction of the maximum, not a duration, because it
// is a "how much is left" question and must follow kStaminaMaxSeconds if that
// is retuned.
//
// Sits above kStaminaRecoveredToSprint (1.5 s of 6.0 s, 25%), so a player
// recovering from empty is still breathing when sprint unlocks again and the
// sound tails off only once they are genuinely rested.
static constexpr float      kStaminaBreathingFraction = 0.50f;
static constexpr float      kStaminaBreathingSeconds =
    kStaminaMaxSeconds * kStaminaBreathingFraction;
// Not static: the HUD reads these through the externs in EngineUI.h.
float                       g_staminaSeconds = kStaminaMaxSeconds;
static float                g_staminaRecoveryDelay = 0.0f;
// True from the moment the meter empties until it has recovered enough to
// sprint again. Read by the HUD to colour the bar and by the input path to
// refuse the sprint multiplier.
bool                        g_staminaExhausted = false;
// Mirrors kStaminaMaxSeconds for the HUD, which needs the denominator to draw
// the meter as a fraction but cannot see a constant defined in this file.
extern const float          kStaminaMaxSecondsUI = kStaminaMaxSeconds;
static float&               g_banditVoiceCooldown = g_enemySystem.voiceCooldown;
static float&               g_banditPainCooldown = g_enemySystem.painCooldown;
// Lockout on the player's own hit sound, so a burst lands as one impact rather
// than several overlapping copies of the same sample.
static float                g_playerPainCooldown = 0.0f;
static float&               g_fleshHitPitchMin = g_game.combat.fleshHitPitchMin;
static float&               g_fleshHitPitchMax = g_game.combat.fleshHitPitchMax;
static bool&                g_suppressFireUntilMouseRelease =
    g_game.combat.suppressFireUntilMouseRelease;
bool                        g_stressTestMode = false;
bool                        g_emptyLevelMode = false;
// The training range is a bare range: no Humvee, no patrol boat. Both vehicles
// are persistent world fixtures rather than level entities -- the Humvee falls
// back to a hardcoded spawn at the origin when a level authors none -- so
// leaving them out of the level file is not enough to keep them off the map.
bool                        g_trainingRangeMode = false;
// The home base: a walkable hub, not a mission. Suppresses the same world
// fixtures the training range does, and additionally the whole deployment flow
// -- there is no insertion to plan, no timer to run and nothing to shoot, so
// the player simply spawns on foot and walks around.
bool                        g_baseMode = false;
// Whether the loaded level places a Humvee entity. The vehicle is authored
// content like any prop: no level gets one unless it asks for one.
//
// Previously the Humvee spawned unconditionally, and primaryHumveeSpawn keeps
// its {0, 3.45, 0} struct default when no entity sets it -- so every level
// without one still got a Humvee at the world origin, complete with physics, a
// shadow and a nav obstacle. Level 1's template authors its own, so it is
// unaffected.
bool                        g_levelPlacesHumvee = false;
static bool                 g_levelPatrolBoatEnabled = true;
// Mouse-walk test mode (F10). Holding the right mouse button walks the player
// forward, and aiming down sights is suppressed for as long as the mode is on --
// the same button cannot both drive and aim. WASD still works; this is an extra
// way in, for testing traversal one-handed rather than a replacement scheme.
//
// Off by default: right mouse aims down sights, which is what the button means
// everywhere else. F10 turns the walk mode on when traversal needs testing
// one-handed.
bool                        g_mouseWalkTestMode = false;
// Whether shift-sprint was held on the last input poll. Read by the viewmodel
// so the run animation can turn over at sprint cadence, and by the HUD's sprint
// indicator. Kept as state rather than re-polling the key at those call sites:
// they would then be sampled a frame apart, and the movement multiplier, the leg
// speed and the readout must all come from the same press to stay in step.
//
// Non-static so EngineUI.h can read it; see the extern there.
bool                        g_playerSprinting = false;
NavigationSystem            g_navigation;
static LevelEditor          g_levelEditor;
static Camera               g_editorCameraSnapshot;
bool                        g_customLevelMode = false;
bool                        g_terrainInVisibilityBuffer = false;
bool                        g_destructionInVisibilityBuffer = false;
// Set when a runtime crater or gouge changes the terrain height field. The
// visibility resolve reads terrain history across frames, so it must drop that
// history once the surface underneath it moves. Consumed in the render loop.
bool                        g_terrainDeformedThisFrame = false;
// Editor fly-camera speed multiplier, adjusted with the mouse wheel (Unreal
// style). Persists across frames; clamped to a sane range.
static float                g_editorCameraSpeed = 1.0f;
static bool                 g_pendingEnvironmentRebuild = false;
static bool                 g_editorVisualRefreshRequested = false;
static bool                 g_editorFullReconcileRequested = false;
static bool                 g_editorFullReconcileInFlight = false;
static std::string          g_activeCustomLevelName;
static std::string          g_mainMenuLevelStatus;
static XMFLOAT3&            g_primaryHumveeSpawn = g_game.vehicles.primaryHumveeSpawn;
static float&               g_primaryHumveeYaw = g_game.vehicles.primaryHumveeYaw;
static std::vector<Transform> g_levelHumveeSpawns;
static constexpr size_t kNoHumvee = SIZE_MAX;
static size_t g_activeHumveeIndex = kNoHumvee;
struct HumveeGameplayState {
    XMFLOAT3 previousPosition{};
    bool previousPositionValid = false;
    float houseImpactCooldown = 0.0f;
    XMFLOAT3 aimPoint{};
    float turretYaw = 0.0f;
    float turretFireCooldown = 0.0f;
};
static std::vector<HumveeGameplayState> g_humveeGameplay;
static std::shared_ptr<SceneNode> g_houseTemplate;
static std::shared_ptr<SceneNode> g_editorWoodHousePreviewModel;
static std::shared_ptr<SceneNode> g_editorMetalHousePreviewModel;
static bool g_editorPreviousDestructionEnabled = true;

static constexpr size_t kEnemiesPerSpawner = 2;
struct CompoundCenter { float x, z; };
static constexpr std::array<CompoundCenter, 8> kStressCompoundCenters = {{
    {  0.0f,   0.0f },
    { 42.0f,   0.0f },
    {-42.0f,   0.0f },
    {  0.0f,  42.0f },
    { 42.0f,  42.0f },
    {-42.0f,  42.0f },
    {  0.0f, -42.0f },
    { 42.0f, -42.0f },
}};
static constexpr size_t kSpawnersPerCompound = 4;
static constexpr size_t kSpawnerCount =
    kStressCompoundCenters.size() * kSpawnersPerCompound;
static constexpr size_t kStressHouseCount = 29;
static constexpr size_t kStressBanditCount = 58;
static_assert(kStressHouseCount <= kStressCompoundCenters.size() *
              kSpawnersPerCompound);
static_assert(kStressBanditCount <= kSpawnerCount * kEnemiesPerSpawner);
