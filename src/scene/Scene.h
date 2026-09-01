#ifndef SCENE_H
#define SCENE_H

#include "DX12Core.h"
#include "CameraDX12.h"
#include "ClusteredRendererDX12.h"
#include "PlayerState.h"
#include "MissionSystem.h"
#include "Weather.h"
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <functional>

using namespace DirectX;

// Scene object types
struct SceneObject {
    XMFLOAT3 position = { 0, 0, 0 };
    XMFLOAT3 rotation = { 0, 0, 0 };
    XMFLOAT3 scale    = { 1, 1, 1 };
    XMFLOAT3 color    = { 1, 1, 1 };
    bool     visible  = true;

    XMMATRIX GetModelMatrix() const {
        XMMATRIX m = XMMatrixScaling(scale.x, scale.y, scale.z);
        m = m * XMMatrixRotationX(XMConvertToRadians(rotation.x));
        m = m * XMMatrixRotationY(XMConvertToRadians(rotation.y));
        m = m * XMMatrixRotationZ(XMConvertToRadians(rotation.z));
        m = m * XMMatrixTranslation(position.x, position.y, position.z);
        return m;
    }
};

struct Projectile {
    XMFLOAT3 position;
    XMFLOAT3 previousPosition;
    XMFLOAT3 direction;
    float    speed;
    float    lifetime;
    bool     active;
    bool     hostile = false;
    // Statistics only count projectiles emitted by the local player's weapon.
    // Marine rounds share the friendly collision path, so hostile=false alone
    // cannot identify ownership.
    bool     playerOwned = false;
    bool     accuracyHitRecorded = false;
    // Grenade: arcs under gravity and detonates (radial blast) on fuse timeout
    // or first impact. Regular bullets leave these at defaults.
    bool     grenade = false;
    // F-grabbed grenades keep burning their fuse while main owns their pose.
    // Physics and collision resume when the player throws them back.
    bool     held = false;
    bool     molotov = false;
    bool     vortex = false;
    bool     rocket = false;
    bool     laser = false;
    bool     remoteCharge = false;
    bool     flame = false;
    bool     harpoon = false;
    bool     harpoonExpired = false;
    uint32_t harpoonId = 0;
    uint8_t  harpoonPiercedCount = 0;
    float    damageMultiplier = 1.0f;
    XMFLOAT3 velocity = { 0, 0, 0 };   // grenades integrate velocity + gravity
    float    fuse = 0.0f;              // seconds until it explodes
    float    grenadeCollisionGrace = 0.0f; // clears thrower's body before collision
    uint32_t grenadePhysicsHandle = 0; // live Box3D rigid body after throw
    XMFLOAT4 rotation = { 0, 0, 0, 1 };
    bool     detonate = false;         // set the frame it should explode
    // Explodes on first contact instead of bouncing and burning its fuse. A
    // thrown frag is meant to be bounced around cover, but a missile called in
    // on a map coordinate has to burst where it was aimed.
    bool     impactFuse = false;
    // Called-in strike rather than a thrown frag. It shares the grenade blast
    // path but scales every radius by missileBlastScale, so the strike can be
    // tuned without moving what a hand grenade does.
    bool     missile = false;
    float    fxCooldown = 0.0f;
};

struct LaserBeamFX {
    XMFLOAT3 start = {};
    XMFLOAT3 end = {};
    float life = 0.0f;
    float maxLife = 0.085f;
};

// IR aiming laser. Unlike LaserBeamFX this is not a weapon effect with a
// lifetime -- it is a continuous beam that exists for as long as the goggles are
// up, so the CPU rewrites start/end every frame and `visible` gates the draw.
struct IRLaserFX {
    XMFLOAT3 start = {};
    XMFLOAT3 end = {};
    // Goggle ramp, 0..1. Also the beam's opacity: an IR designator is only
    // visible through an intensifier, so this fades with the NVG blend.
    float visibility = 0.0f;
    bool visible = false;
};

struct HarpoonTetherFX {
    XMFLOAT3 start = {};
    XMFLOAT3 end = {};
    float life = 0.0f;
    float maxLife = 0.32f;
};

struct PinnedHarpoonFX {
    XMFLOAT3 position = {};
    XMFLOAT3 direction = { 0.0f, 0.0f, 1.0f };
    uint32_t harpoonId = 0;
};

struct RemoteCharge {
    XMFLOAT3 position = {};
    XMFLOAT3 normal = { 0.0f, 1.0f, 0.0f };
};

struct FirePatch {
    XMFLOAT3 position = {};
    float life = 0.0f;
    float maxLife = 0.0f;
    float radius = 0.25f;
    float maxRadius = 0.85f;
    float spreadDelay = 0.45f;
    float fxCooldown = 0.0f;
    float structureDamageCooldown = 0.55f;
    uint8_t generation = 0;
    bool hasSpread = false;
};

struct BurningTargetFX {
    XMFLOAT3 position = {};
    float size = 0.8f;
    float intensity = 1.0f;
    float animationTime = 0.0f;
};

struct BurningMaterial {
    uint64_t entityId = 0;
    XMFLOAT3 position = {};
    float life = 0.0f;
    float maxLife = 0.0f;
    float damageCooldown = 0.0f;
    float size = 1.4f;
};

// A particle from a bullet impact: smoke, blood billboard, or spark/debris shard.
struct ImpactParticle {
    XMFLOAT3 position;
    XMFLOAT3 velocity;
    float    life;      // seconds remaining
    float    maxLife;   // for fade
    float    size;      // smoke: grows; spark: shrinks
    float    growth;    // size delta per second (negative for sparks)
    XMFLOAT3 color;
    bool     spark = false;  // true = ballistic bright shard, false = smoke
    bool     blood = false;  // textured blood billboard with ballistic motion
};

// Layered one-shot explosion. The 4x4 smoky sheet supplies the outer fireball;
// an 8x8 white-hot core and pressure wave are added by the renderer.
struct ExplosionFX {
    XMFLOAT3 position;
    float    size;      // world diameter at full bloom
    float    age = 0.0f;
    float    duration = 0.9f;
    float    rotation = 0.0f;
};

struct VortexFX {
    XMFLOAT3 position = {};
    float radius = 7.5f;
    float age = 0.0f;
    float duration = 3.0f;
    float particleCooldown = 0.0f;
};

struct ExplosiveBarrel {
    XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 velocity = { 0.0f, 0.0f, 0.0f };
    XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
    uint32_t physicsHandle = 0;
    int hits = 0;
    bool active = true;
    bool held = false;
    bool thrown = false;
    float vortexHoldTime = 0.0f;
    XMFLOAT3 vortexCenter = { 0.0f, 0.0f, 0.0f };
    bool burning = false;
    float fuse = 0.0f;
    float fireFxCooldown = 0.0f;
};

// A weapon lying in the world, collected by walking over it. The pickup swaps
// itself into whichever slot the player is currently holding, so it costs a
// weapon rather than adding a third -- the loadout stays two wide all run.
//
// One-shot: `collected` latches and is only cleared by a level reset, so a
// pickup cannot be farmed by walking back and forth over it.
struct WeaponPickup {
    XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
    float yawRadians = 0.0f;
    int weapon = 2;             // GunModel weapon id; 2 = RPG-7
    // Magazine + reserve handed over on collection. Independent of PlayerState's
    // per-slot maxima so a pickup can be a partial resupply.
    int magazine = 1;
    int reserve = 4;
    float radius = 2.0f;        // horizontal collection distance, metres
    float verticalRange = 3.0f; // vertical tolerance, so a pickup is not
                                // collectable from a rooftop directly above it
    float bobPhase = 0.0f;      // drives the idle hover/spin so it reads as loot
    bool active = false;
    bool collected = false;
};

struct GunViewModel {
    bool     visible  = true;   // AK47 view model is on by default
    XMFLOAT3 color    = { 0.3f, 0.3f, 0.35f };
    XMFLOAT3 offset   = { 0.11f, -0.20f, 0.33f };
    XMFLOAT3 scale    = { 0.15f, 0.15f, 0.30f };
    XMFLOAT3 rotation = { 0.0f, 180.0f, 0.0f };
};

enum class WaterQuality : uint32_t {
    Low = 0,
    High = 1,
    Ultra = 2
};

// All mutable scene state lives here
struct Scene {
    // Camera - positioned back so the imported model (a small building) is fully framed at spawn
    Camera camera{ XMFLOAT3(0.0f, 1.7f, 20.0f) };
    float  cameraFOV  = 60.0f;
    float  sniperScopeFOV = 15.0f;
    float  sniperScopeBlend = 0.0f;
    // Iron-sight ADS for every non-scoped weapon. Separate from the SVD scope
    // blend: that one swaps to a scope overlay and hides the viewmodel, this one
    // just pulls the gun to centre and tightens the FOV a little.
    float  adsFOV     = 42.0f;   // moderate -- ADS is for accuracy, not zoom
    float  adsBlend   = 0.0f;    // 0 = hip, 1 = fully sighted
    bool   adsActive  = false;
    // Sighted viewmodel offset, against a hip offset of {0.11, -0.20, 0.47}.
    // X overshoots slightly past zero: the weapon mesh sits right of its own
    // origin, so putting the origin on the view axis still leaves the barrel
    // and sights off to the right. Y sits just below the crosshair so the
    // weapon does not cover it; Z is pulled in toward the eye.
    // Live-tunable so the sight alignment can be dialled in from the debug UI
    // instead of a rebuild per nudge.
    float adsOffsetX = -0.01f;
    float adsOffsetY = -0.075f;
    float adsOffsetZ = 0.30f;
    bool   sniperScopeActive = false;
    float  cameraNear  = 0.1f;
    // Far enough for the ordinary camera to see uninterrupted sea to its
    // horizon; the deployment view temporarily extends this when it pulls back.
    float  cameraFar   = 800.0f;
    // Temporary projection extension for views such as deployment planning.
    // Zero preserves the authored gameplay far plane and its depth precision.
    float  cameraFarOverride = 0.0f;
    // Sub-pixel projection offset used by visibility-buffer TAA. Zero for
    // forward, raytracing, menus, and validation captures.
    XMFLOAT2 temporalJitterPixels = { 0.0f, 0.0f };

    // Main directional / point light
    // Angled, HDR-strength warm sun. Lower elevation gives terrain and props
    // longer modelling shadows while cool sky irradiance keeps them readable.
    XMFLOAT3 lightPos    = { 4.735f, 3.095f, -8.246f };
    XMFLOAT3 lightColor  = { 1.0f, 0.92f, 0.70f };
    float    directionalLightIntensity = 12.18f;
    int      lightType   = 0;
    float    lightConstant  = 1.0f;
    float    lightLinear    = 0.09f;
    float    lightQuadratic = 0.032f;

    // Material defaults
    float ambientStrength   = 0.07f;
    float ambientLightingIntensity = 0.42f;
    float specularStrength  = 0.5f;
    int   specularShininess = 32;
    float shadowBias        = 0.005f;
    bool  enableShadows     = true;
    bool  cacheFarShadowCascades = false;
    XMFLOAT3 shadowCenter    = { 0.0f, 3.0f, 0.0f };
    float shadowOrthoSize    = 30.0f;

    XMFLOAT3 EffectiveLightColor() const {
        return {
            lightColor.x * directionalLightIntensity,
            lightColor.y * directionalLightIntensity,
            lightColor.z * directionalLightIntensity
        };
    }
    float shadowDistance     = 40.0f;
    float shadowFarPlane     = 90.0f;

    // Clear color
    XMFLOAT3 clearColor = { 0.35f, 0.58f, 0.82f };

    // Objects
    SceneObject cube1;
    SceneObject cube2;
    SceneObject floor;

    // Animation
    bool  animateLight = false;
    bool  animateCube  = false;
    float animationSpeed = 1.0f;

    // Gun & projectiles
    GunViewModel gun;
    std::vector<Projectile> projectiles;
    LaserBeamFX laserBeam;
    IRLaserFX irLaser;
    HarpoonTetherFX harpoonTether;
    std::vector<PinnedHarpoonFX> pinnedHarpoons;
    std::vector<RemoteCharge> remoteCharges;
    std::vector<ImpactParticle> impactParticles;  // impact smoke puffs
    std::vector<ExplosionFX> explosionFX;         // animated explosion flipbooks
    std::vector<VortexFX> vortexFX;               // active debris-orbit fields
    std::vector<FirePatch> firePatches;            // spreading Molotov ground fire
    std::vector<BurningTargetFX> burningTargets;   // flames attached to actors/trees
    std::vector<BurningMaterial> burningMaterials; // persistent prefab material fire
    std::function<void(const XMFLOAT3&, float, bool)> explosionAudioCallback;
    std::function<void(const XMFLOAT3&)> fireIgnitionAudioCallback;
    std::vector<ExplosiveBarrel> explosiveBarrels;
    std::vector<WeaponPickup> weaponPickups;      // walk-over weapon crates
    float projectileSpeed    = 300.0f;
    float projectileLifetime = 3.0f;
    XMFLOAT3 projectileColor = { 1.0f, 1.0f, 1.0f };
    float projectileScale    = 0.1f;
    float rocketTurnRate     = 2.8f;    // radians/sec; aim crosshair guides missile
    bool  autoFire           = true;    // hold mouse to keep firing
    float fireInterval       = 0.1f;    // seconds between auto-fire shots
    float fireCooldown       = 0.0f;    // time left before next shot may fire
    float muzzleFlashTime    = 0.0f;    // short enough to read as one-frame light
    float muzzleFlashDuration = 0.055f;
    float muzzleFlashScale   = 1.0f;
    float muzzleFlashRotation = 0.0f;
    float flamethrowerAudioTime = 0.0f;
    uint32_t nextHarpoonId = 1;
    bool c4DetonateHeld = false;
    float gunRecoilBack      = 0.0f;    // viewmodel translation, local metres
    float gunRecoilKick      = 0.0f;    // viewmodel pitch, degrees
    // Weapon sway: the gun lags behind the camera when the player turns, then
    // settles back to centre. Degrees of trailing rotation, signed against the
    // turn direction. Previous angles are the only way to recover turn rate --
    // the camera exposes orientation, not angular velocity.
    float gunSwayYaw         = 0.0f;
    float gunSwayPitch       = 0.0f;
    float gunSwayPrevYaw     = 0.0f;
    float gunSwayPrevPitch   = 0.0f;
    bool  gunSwayPrimed      = false;   // skip the first frame's bogus delta
    // Jump/land muzzle pitch, in degrees. A spring: the barrel tips up as the
    // player leaves the ground and noses down on impact, then oscillates back to
    // rest. This rotates the weapon mesh only -- camera aim is never touched, so
    // where the player is actually shooting does not move.
    float gunJumpPitch       = 0.0f;
    float gunJumpVelocity    = 0.0f;
    // The camera zeroes VerticalVelocity the instant it lands, so the speed of
    // impact only exists on the frame before. Keep it to scale the landing dip.
    float gunPrevVerticalVel = 0.0f;
    bool  gunPrevGrounded    = true;
    // Degrees of gun rotation per degree of view turn, the ceiling that keeps a
    // flick from throwing the weapon off screen, and how fast it recentres.
    static constexpr float kGunSwayAmount     = 0.11f;
    static constexpr float kGunSwayMaxDegrees = 0.9f;
    static constexpr float kGunSwayReturnRate = 9.0f;
    // Jump/land spring, working in degrees of muzzle pitch. Stiffness and
    // damping settle it in roughly a third of a second without a second visible
    // bounce. Launch tips the barrel up; landing noses it down harder, scaled by
    // impact speed so a long drop hits more than a hop.
    static constexpr float kGunJumpStiffness   = 150.0f;
    static constexpr float kGunJumpDamping     = 16.0f;
    static constexpr float kGunJumpLaunchKick  = 78.0f;   // deg/s impulse, up
    static constexpr float kGunJumpLandKick    = 132.0f;  // deg/s impulse, down
    static constexpr float kGunJumpMaxDegrees  = 15.0f;
    // Impact speed that produces a full-strength dip; faster hits are clamped.
    static constexpr float kGunJumpRefLandSpeed = 9.0f;
    // Upward speed that separates a real jump from merely leaving the ground.
    // Camera::JumpStrength is 5 m/s, so half of it clears a genuine push off
    // while a walk-off ledge (starting near zero) never reaches it.
    static constexpr float kGunJumpMinLaunchSpeed = 2.5f;
    float recoilPitch        = 0.55f;   // camera climb per shot, degrees
    float recoilYaw          = 0.22f;   // random horizontal camera kick
    // How far the four crosshair arms sit from centre, in pixels beyond their
    // resting gap. Driven by movement, firing and recoil, then eased back down
    // -- the reticle opening up is the readout for "your shots are not going
    // where this is pointing".
    //
    // Held here rather than computed in the HUD because the inputs live in
    // different places (the movement tracker is app-side, recoil is here) and
    // the HUD draw must not be the thing that decides gameplay feel.
    float crosshairSpread    = 0.0f;
    // Where the spread is heading this frame, written by the app each tick from
    // movement + firing state. The eased `crosshairSpread` chases it, so a
    // burst blooms the reticle fast and it settles slowly.
    float crosshairSpreadTarget = 0.0f;

    // Trailing "chip" health, in the same units as player.health. Follows the
    // real value down after a delay, so the HUD can show a pale ghost of the
    // damage just taken draining away behind the live bar. That is what makes a
    // hit legible as an amount rather than a bar that is simply shorter now.
    // Snaps up instantly on healing so regen never shows a ghost.
    float healthChip = -1.0f;      // <0 = uninitialised, latched on first update
    float healthChipDelay = 0.0f;  // grace before the ghost starts draining

    // Hit marker: the four diagonal ticks that flick over the crosshair when a
    // shot connects. Counts down to zero; the HUD fades and shrinks it in.
    float hitMarkerTime = 0.0f;
    float hitMarkerDuration = 0.32f;
    // A killing blow draws the marker in red and holds it longer, so "hit" and
    // "dropped him" are distinguishable without reading a counter.
    bool hitMarkerLethal = false;

    void TriggerHitMarker(bool lethal = false) {
        // A lethal marker outranks a plain one already on screen, but a plain
        // hit landing after a kill must not downgrade it mid-flash -- bursts
        // routinely land another round on a body the same frame it dies.
        if (lethal) hitMarkerLethal = true;
        else if (hitMarkerTime > 0.0f && hitMarkerLethal) return;
        else hitMarkerLethal = false;
        hitMarkerDuration = lethal ? 0.45f : 0.32f;
        hitMarkerTime = hitMarkerDuration;
    }
    PlayerState player;

    bool AmmoEnforced() const { return player.AmmoEnforced(); }
    bool Reloading() const { return player.Reloading(); }
    void RestoreAmmo() { player.RestoreAmmo(); }
    bool BeginReload(int slot) { return player.BeginReload(slot); }
    void UpdateReload(float dt) { player.UpdateReload(dt); }
    bool ConsumeAmmo(int slot) { return player.ConsumeAmmo(slot); }

    // Grenade (press G): lobbed, arcs under gravity, radial blast on fuse.
    float grenadeThrowSpeed    = 16.0f;  // launch speed along aim
    float grenadeLob           = 3.0f;   // extra upward velocity for the arc
    float grenadeGravityScale  = 1.0f;
    float grenadeGroundY       = 0.15f;  // bounce height
    float grenadeFuse          = 2.0f;   // timer-only detonation
    // The default explosion radius every blast is measured against: debris, FX
    // and crater -- NOT the kill radius, which is grenadeEnemyRadius below.
    // Each explosion type scales this by its own factor (see grenadeBlastScale
    // and missileBlastScale), so retuning the base moves them all together
    // instead of needing one edit per weapon.
    float explosionBlastRadius = 3.5f;
    // A thrown frag is the small end of the scale: half the default.
    float grenadeBlastScale    = 0.5f;
    float grenadeDamage        = 1.5f;   // per-bond damage in the blast
    float grenadeImpulse       = 120.0f; // original debris impulse
    float grenadeEnemyRadius   = 7.0f;
    float grenadeEnemyDamage   = 500.0f; // 20% edge falloff still deals 100
    float grenadeEnemyImpulse  = 100.0f;
    float grenadeEnemyPush     = 9.0f;   // survivor knockback speed in m/s
    // Blast damage to the player at the centre, falling off to 0 at
    // grenadeEnemyRadius. Enemies lob these, so a direct hit should hurt badly
    // without being an instant kill from full health.
    float grenadePlayerDamage  = 75.0f;
    // Multiplies every radius of a called-in missile strike's blast: debris,
    // enemy reach, FX and terrain crater. 1.0 is the default explosion radius
    // above; the deployment-screen slider drives this.
    float missileBlastScale    = 2.0f;
    // Crater cut shape. The stamp is a boolean-style subtraction rather than a
    // smooth dent, so these describe an actual profile: how deep the floor
    // sits, how much of the radius stays flat before the wall starts, and how
    // abruptly that wall turns up (>1 = sharper, closer to a vertical face).
    float craterDepth          = 1.6f;   // metres at the floor
    // Share of the radius that is flat floor: the inner (floor) radius is
    // exactly half the outer, so the wall occupies the outer half.
    float craterFloorFraction  = 0.5f;
    float craterWallSharpness  = 2.6f;   // wall exponent
    float grenadeCooldown      = 0.0f;   // input debounce
    GrenadeType selectedGrenade = GrenadeType::Frag;
    float molotovFireDuration = 9.0f;
    float molotovDamagePerSecond = 40.0f;
    // Difficulty dial from the deployment screen: scales every source of enemy
    // damage against the player. 1.0 is the balance the levels were tuned at.
    // Applied through DamagePlayerFromEnemy, never to self-inflicted damage.
    float enemyDamageMultiplier = 1.0f;
    float molotovMaterialDamagePerSecond = 34.0f;
    float molotovDamageCooldown = 0.0f;
    float vortexRadius = 7.5f;
    float vortexDuration = 3.0f;
    float playerBurnTime = 0.0f;
    // Returns rendered ground height at world XZ. Installed by main so Scene
    // does not depend on terrain renderer implementation.
    std::function<float(float, float)> grenadeGroundHeight;

    // NVIDIA Blast + Box3D destructible house
    bool  useDestruction = true;
    bool  showHelicopter = true;   // draw + simulate the hovering attack heli
    // Whether the Humvee spotlight draws a shaft in the volumetric fog.
    // Surfaces are lit either way; this only controls the glow in the air.
    bool  spotlightVolumetric = false;
    bool  enableMSAA = true;
    bool  enableGrassMSAA = true;
    bool  enableFXAA = false;
    bool  enablePhysicalAtmosphere = true;
    // Selects the baked 3D noise raymarch. Off keeps the legacy 2D cloud slab
    // as a cheap fallback, so the toggle never removes weather entirely.
    bool  enableVolumetricClouds = false;
    float atmosphereRayleighStrength = 1.35f;
    float atmosphereMieStrength = 0.80f;
    float atmosphereMieAnisotropy = 0.76f;
    float atmosphereAerialDensity = 0.00f;
    float atmosphereCloudCoverage = 0.47f;
    float atmosphereCloudDensity = 0.86f;
    float atmosphereCloudBaseHeight = 1240.0f;
    float atmosphereCloudThickness = 1530.0f;
    // -- Weather ---------------------------------------------------------
    WeatherState weatherState = WeatherState::Cloudy;
    // Rainfall, 0 clear to 1 downpour. Drives the rain renderer's drop count
    // and, through the deployment screen, the fog density that decides how far
    // anything can see -- so weather is a gameplay setting, not only a visual
    // one. Off by default: every level was authored dry.
    float rainIntensity = 0.0f;
    // Horizontal drift applied to falling drops, world units/sec. Rain is
    // stretched along its travel direction, so this visibly slants the sheet
    // rather than only sliding it sideways.
    XMFLOAT2 windVelocity = { 1.6f, 0.7f };

    bool  enableVolumetricFog = true;
    // Doubles the froxel grid to 128x72x96. The default 64x36x48 gives one fog
    // sample per ~30x30 screen pixels at 1080p, which makes sun shafts through
    // palm fronds look blocky. High quality is the default; the UI can still
    // disable it on slower GPUs.
    bool  volumetricFogHighRes = true;
    // How sun shafts are produced.
    //
    // Volumetric marches the froxel grid: correct occlusion from any geometry,
    // shafts visible with the sun off-screen, but the grid is only 64 (or 128)
    // froxels wide, so a column straddling a silhouette mixes the shadowed and
    // unoccluded ray and bleeds brightness across the edge.
    //
    // Faux is a screen-space radial blur from the sun with a per-tap depth test.
    // It has no grid, so it cannot bleed or quantise, but it needs the sun on
    // screen and cannot show a shaft that starts behind the camera.
    enum class LightShaftMode { Volumetric = 0, Faux = 1, Off = 2 };
    LightShaftMode lightShaftMode = LightShaftMode::Volumetric;
    float lightShaftDensity = 0.85f;   // how far along the sun ray taps march
    float lightShaftDecay = 0.96f;     // per-tap falloff; <1 shortens the streak
    float lightShaftWeight = 0.85f;    // overall contribution of the tap sum
    float lightShaftExposure = 0.32f;  // final scale before it is added
    float lightShaftIntensity = 1.0f;  // user-facing master strength
    float volumetricFogDensity = 0.009f;
    float volumetricFogAnisotropy = 0.82f;
    float volumetricFogHeightFalloff = 0.045f; // fog thins above the undergrowth, not over treetops
    float volumetricFogBaseHeight = 0.4f;      // haze pools low in the valley floor
    // World clouds live inside the froxel volume, so scene depth occludes them
    // and the player can pass through their 3D density. When enabled they
    // replace the sky-pass cloud layer instead of drawing a duplicate behind it.
    bool  enableFlyableClouds = true;
    float flyableCloudBaseHeight = 9.0f;
    float flyableCloudThickness = 30.0f;
    float flyableCloudDensity = 2.250f;
    float flyableCloudCoverage = 1.960f;
    // Match the ordinary camera far plane so the volume does not end in a
    // visible ring inside the playable view.
    float volumetricFogDistance = 800.0f;
    XMFLOAT3 volumetricFogTint = {
        168.0f / 255.0f, 181.0f / 255.0f, 176.0f / 255.0f
    };

    void ApplyWeatherPreset(WeatherState state) {
        weatherState = state;
        if (state == WeatherState::Custom) return;

        const WeatherSettings weather = MakeWeatherSettings(state);
        rainIntensity = weather.rainIntensity;
        windVelocity = weather.windVelocity;
        atmosphereCloudCoverage = weather.skyCloudCoverage;
        atmosphereCloudDensity = weather.skyCloudDensity;
        atmosphereCloudBaseHeight = weather.skyCloudBaseHeight;
        atmosphereCloudThickness = weather.skyCloudThickness;
        enableFlyableClouds = weather.worldClouds;
        flyableCloudBaseHeight = weather.worldCloudBaseHeight;
        flyableCloudThickness = weather.worldCloudThickness;
        flyableCloudDensity = weather.worldCloudDensity;
        flyableCloudCoverage = weather.worldCloudCoverage;
        enableVolumetricFog = weather.volumetricFog;
        volumetricFogDensity = weather.fogDensity;
        volumetricFogAnisotropy = weather.fogAnisotropy;
        volumetricFogHeightFalloff = weather.fogHeightFalloff;
        volumetricFogBaseHeight = weather.fogBaseHeight;
        volumetricFogDistance = weather.fogDistance;
        volumetricFogTint = weather.fogTint;
    }
    bool  enableAmbientOcclusion = true;
    float ambientOcclusionRadius = 0.69f;
    float ambientOcclusionStrength = 2.80f;
    float ambientOcclusionBias = 0.035f;
    float contactShadowStrength = 0.65f;
    // Test the contact-shadow occluder slab in linear depth instead of device
    // depth.
    //
    // The device-depth form is distance-dependent to the point of being three
    // different effects in one frame: on a 0.1/800 non-reversed projection the
    // fixed epsilon is ~4 mm of world space at 1 m and ~29 m at 100 m. Once it
    // outgrows the march step the receiver falls inside its own slab and flat
    // ground self-shadows into a second, sun-offset copy of the caster; further
    // out the test can never pass and contact shadows stop along an
    // iso-distance contour, which on flat terrain reads as a hard straight line
    // anchored to no geometry.
    //
    // Keep the legacy path behind the toggle for A/B diagnosis, but use the
    // distance-stable linear test by default.
    bool  contactShadowLinearDepth = true;
    // Accumulate directional GTAO through motion/depth/normal history. Off by
    // default so the established single-frame path remains the baseline.
    bool  temporalBentNormalGTAO = false;
    // A/B switch for the GTAO/contact-shadow arithmetic optimizations: hoisted
    // loop invariants, multiply chains in place of pow(), and rsqrt-based
    // distance math. Both variants are compiled from the same source behind
    // SGE_AO_OPTIMIZED, so this swaps pipelines rather than recompiling.
    //
    // The optimizations are output-preserving in intent, so the two paths
    // should look identical while differing only in cost. Kept as a toggle
    // because an offline A/B on the smoke-test scene could not separate them
    // from run-to-run noise: the pass is dominated by scattered depth fetches,
    // not arithmetic. Measure it on a real view via the profiler overlay.
    bool  optimizedAmbientOcclusion = true;
    // Trace ambient occlusion at half resolution and upsample in the existing
    // bilateral composite, which keeps running at full resolution. Quarters the
    // AO trace's pixel count -- the dominant cost of the pass, since it is
    // bound by scattered depth fetches rather than arithmetic.
    //
    // A previous attempt at this was reverted for horizontal banding. That was
    // a texel-addressing bug, not an inherent limitation: half-res pixel
    // centres land exactly on full-res texel boundaries, where a point sampler
    // picks between two neighbours by float rounding. The shader now re-centres
    // source fetches (TraceUVToSourceUV) so each lands mid-texel. On by
    // default: the trace is the pass bottleneck and half res cuts it ~52%.
    bool  halfResolutionAO = true;
    // The independently resolved 4x grass depth can drive GTAO/contact so grass
    // receives and casts the screen-space effect. On by default: grass that is
    // absent from the AO depth neither occludes nor is occluded, so blades sit
    // on the ground without contact darkening. Falls back automatically when
    // 4x MSAA grass or the visibility buffer is unavailable (see the guards at
    // the GTAO dispatch), so enabling it here cannot break the other renderers.
    bool  grassInScreenSpaceAO = true;
    bool  enableScreenSpaceReflections = true;
    float screenSpaceReflectionStrength = 0.35f;
    float screenSpaceReflectionDistance = 55.0f;
    float screenSpaceReflectionThickness = 0.08f;
    WaterQuality waterQuality = WaterQuality::High;
    // High-path wave controls. Multipliers over the authored CalmTropical
    // spectrum rather than absolute metres, so the relative shape of the swell
    // and its shorter components is preserved as the sliders move. They are
    // applied where the wave constants are uploaded, which happens every frame,
    // so edits show up live without rebuilding the water volume.
    float highWaterWaveHeight = 1.0f;
    float highWaterWaveScale = 1.0f;
    float highWaterWaveSpeed = 1.0f;
    float highWaterChoppiness = 1.0f;
    float highWaterMicroDetail = 1.0f;
    float highWaterFoamStrength = 1.0f;
    // Master strength for High's near-shore wave model. The ocean keeps a
    // dominant offshore swell -- every train holds its authored bearing in deep
    // water -- and the coast only asserts itself as the bed shallows. Each
    // train starts responding at roughly half its own wavelength of depth, so
    // long swell turns first and short chop stays deep-water until it is nearly
    // ashore.
    //
    // One weight drives the whole set, because physically they are one
    // phenomenon: refraction toward the shore normal, wavelength compression
    // from finite-depth dispersion, shoaling amplitude gain, phase irregularity
    // so fronts arrive ragged rather than as concentric arcs, and breaking at
    // the 0.39 * depth limit. 0 restores the pure authored spectrum.
    //
    // Rendering-only. The bathymetry lives in a GPU texture the CPU buoyancy
    // query cannot sample, so above zero the drawn surface and the one floating
    // objects sit on decorrelate -- measured at ~85% of wave height by 7 degrees
    // of rotation, since two sine fields on different bearings share no crests.
    // The decorrelation is confined to the shoaling band now rather than the
    // whole ocean, but it is still there: keep it low where a boat has to float
    // convincingly, or use Ultra, which reads its heights back from the GPU.
    float highWaterShoreRefraction = 0.0f;
    // Height half of the same near-shore model, on the same per-train depth
    // ramp. Wavelength compression from finite-depth dispersion, shoaling
    // amplitude gain, crest steepening, and the 0.39 * depth breaker cap.
    //
    // Separate from the bearing half above because the two fail differently.
    // Refraction rotates crests, which is what decorrelates CPU buoyancy -- a
    // boat ends up on a surface whose waves point somewhere else. Flattening
    // only ever lowers the surface, and only where the bed is shallow enough
    // that a boat is aground anyway, so it is safe to leave on. It is also what
    // keeps unbounded amplitude out of ankle-deep water, where neighbouring
    // crests fold through each other.
    //
    // Default 0.30 rather than full: enough to calm the shallows without
    // flattening the shorebreak, which is where the surf actually reads.
    float highWaterShoreFlatten = 0.30f;
    float ultraWaterWaveHeight = 1.0f;
    float ultraWaterWaveScale = 1.0f;
    float ultraWaterWaveSpeed = 1.0f;
    float ultraWaterDirection = 0.0f;
    float ultraWaterChoppiness = 1.0f;
    float ultraWaterSurfStrength = 1.0f;
    float ultraWaterFoamStrength = 1.0f;
    float ultraWaterCoastDamping = 1.0f;
    bool  ultraWaterRefreshRequested = false;
    int   destructionGridX = 4;
    int   destructionGridY = 3;
    int   destructionGridZ = 4;
    float destructionDamageRadius = 0.9f;   // tight blast so hits stay local
    float destructionDamage = 0.5f;         // bullets weaken twice, then break on third hit
    float destructionBulletImpulse = 260.0f;
    bool  rebuildDestructionRequested = false;
    bool  showDestructionDebug = false;
    bool  showRagdollPhysicsShapes = false;

    // Palm trees. Damage here is absolute (against a section's health), not the
    // 0..1 bond fraction destructionDamage uses -- different system, different
    // units. Two default hits sever one trunk section.
    float treeDamagePerShot = 15.0f;

    // Clustered renderer
    ClusteredRendererDX12 clusteredRenderer;
    bool useClusteredRendering = true;

    // Demo lights (off by default - the shader sums every active light per
    // pixel with no per-cluster culling applied, so a large count washes out
    // the scene; enable from the UI to see the clustered-lighting showcase)
    int   numDemoLights       = 0;
    float demoLightRadius     = 8.0f;
    float demoLightIntensity  = 1.5f;
    bool  animateDemoLights   = true;

    // Dynamic diffuse probe grid for low-frequency bounced lighting.
    bool  useDDGI      = true;
    float giIntensity  = 0.45f;
    float giMaxDistance = 24.0f;
    float normalBias   = 0.18f;
    float probeSpacing = 5.0f;
    bool  showProbes   = false;

    // Rendering mode
    bool wireframeMode       = false;
    bool useVisibilityBuffer = true; // Fast hybrid default; M toggles Forward fallback
    bool useRaytracing       = false; // DXR raytracing mode

    // Upgraded visuals: the hybrid ray-traced tier layered on top of the
    // visibility buffer. Temporarily on by default while the RT/SVGF path is
    // being tuned; DXR Tier 1.1 (inline RayQuery) still gates it on
    // the hardware actually reporting that. Everything under this flag degrades
    // to the existing raster path when it is off, so the default frame is
    // byte-for-byte what it was before.
    //
    // Structured as a cheap-tier-first hybrid rather than "ray trace
    // everything": screen-space, probes and temporal history resolve most
    // pixels, and rays are spent only where those report low confidence. See
    // enhancedRayFraction for what that costs in practice.
    // Off by default: the same state F5 leaves the game in. RT is opt-in, so a
    // fresh launch runs the raster path and the ray cost is only paid once the
    // player asks for it.
    bool enhancedVisuals     = false;
    // Sub-toggles, so the expensive parts can be bisected when profiling. Each
    // is a no-op unless enhancedVisuals is on.
    bool enhancedRTShadows   = false;  // RayQuery sun shadows, replaces CSM
    bool enhancedRayClassify = true;   // Spend rays only on low-confidence pixels
    // Stochastic RT reflections: one GGX ray per pixel per frame. Enabled for
    // the current SVGF tuning pass; the raw signal is noisy on purpose and
    // wants the temporal denoiser to resolve it.
    bool enhancedRTReflections = false;
    // Refit the TLAS instance transforms every frame, so moving actors are
    // traced where they are rather than where they stood when the acceleration
    // structure was first built.
    //
    // With this off the TLAS is a one-time snapshot: helicopters and vehicles
    // cast a shadow from a position they have left, and appear in no reflection
    // at all once they move. That is what made RT shadows look detached from
    // dynamic geometry. The refit is a PERFORM_UPDATE over instance descriptors
    // only -- BLASes are untouched, so it costs far less than a rebuild.
    //
    // Kept as a toggle because it is the bisection tool for "is this artefact a
    // stale acceleration structure or a shading bug": turning it off restores
    // the previous static behaviour exactly.
    bool enhancedTLASRefit = true;
    // Confidence below which a pixel is handed to RT. Raising it traces more
    // pixels (better, slower). Live-tunable so the split can be dialled in
    // against a real scene rather than guessed.
    float enhancedConfidenceThreshold = 1.0f;
    // Read back from the classify pass: fraction of pixels routed to RT last
    // frame. Displayed in the UI as the headline "is classification earning its
    // keep" number -- expect 0.05-0.20 on typical scenes.
    float enhancedRayFraction = 0.0f;

    // Bindless material textures: SceneMaterial geometry samples its maps
    // through ResourceDescriptorHeap[] instead of a per-draw descriptor table.
    // This removes the 64-distinct-texture ceiling the visibility resolve
    // inherits from its fixed-size table, and drops one root-descriptor-table
    // set per textured draw.
    //
    // Requested by default when SM 6.6 and Resource Binding Tier 3 are present;
    // runtime capability checks fall back to the legacy heaps and shaders on
    // unsupported adapters.
    bool bindlessMaterials   = true;

    // DXGI present sync interval: 0 uncapped (tearing allowed), 1 every vblank,
    // 2+ divides the refresh rate (2 = half, 3 = a third...). Kept as the raw
    // interval rather than a bool so the UI can expose the divisors.
    int vsyncInterval        = 0;

    // Draws a sphere where the BlackHawk's "PlayerRide" empty resolves to, for
    // checking the authored ride point against the airframe.
    bool showBlackHawkRideMarker = false;

    // Mesh-shader tessellated terrain (replaces the flat floor plane when on)
    bool  useMeshTerrain     = true;
    float terrainHeightScale = 3.057f;
    // Island builder extent, driven by the level definition. tilesX*tileSize is
    // the drawn ground width; island scale grows the coastline so ocean rings it.
    uint32_t terrainTilesX   = 16;
    uint32_t terrainTilesZ   = 16;
    // Extra terrain relief: a low-frequency octave for broad landforms, a
    // high-frequency one for surface break-up, and macro normal perturbation
    // that survives past the close-range detail fade. Off by default -- it
    // changes the heightfield, and collision is sampled from the same function.
    bool  terrainDetailRelief = false;
    // Hollow-inspired projected-error tessellation and stitched tile edges.
    // Opt-in so the legacy distance LOD remains the default rendering path.
    bool  terrainErrorLOD      = false;
    // Flat authoring plane: no fbm relief, no pool basin, no coast falloff.
    // Driven by the level definition's terrainFlat.
    bool  terrainFlat         = false;
    float terrainIslandScaleX = 1.0f;   // per-axis coastline stretch
    float terrainIslandScaleZ = 1.0f;
    // Grid min-corner offset in tiles; 0 = centered on origin (legacy).
    int32_t terrainOriginTileX = 0;
    int32_t terrainOriginTileZ = 0;

    // Z key: wireframe for the mesh-shader pipelines (meshlets + terrain)
    bool meshletWireframe = false;

    Scene() {
        cube1.position = { 0.0f, 1.0f, 0.0f };
        cube1.scale    = { 2.0f, 2.0f, 2.0f };
        cube1.color    = { 0.85f, 0.25f, 0.3f };

        cube2.position = { -3.0f, 0.5f, 2.0f };
        cube2.scale    = { 1.0f, 1.0f, 1.0f };
        cube2.rotation = { 0.0f, 45.0f, 0.0f };
        cube2.color    = { 0.3f, 0.5f, 0.85f };
        cube2.visible  = false;

        // Clear diagonal gaps between center and four houses. Barrel center is
        // 0.75 m above the authored 2.5 m building pad.
        explosiveBarrels = {
            {{ 4.6f, 3.25f,  4.6f}},
            {{-4.6f, 3.25f,  4.6f}},
            {{ 4.6f, 3.25f, -4.6f}},
            {{-4.6f, 3.25f, -4.6f}},
        };

        floor.color    = { 1.0f, 1.0f, 1.0f };
    }

    void InitLights() {
        clusteredRenderer.init();
        RebuildDemoLights();
    }

    void RebuildDemoLights() {
        clusteredRenderer.clearLights();
        for (int i = 0; i < numDemoLights; i++) {
            float angle = (float)i / numDemoLights * XM_2PI;
            XMFLOAT3 pos(cosf(angle) * 8.0f, 4.0f, sinf(angle) * 8.0f);
            XMFLOAT3 c;
            c.x = sinf(angle) * 0.5f + 0.5f;
            c.y = sinf(angle + 2.094f) * 0.5f + 0.5f;
            c.z = sinf(angle + 4.189f) * 0.5f + 0.5f;
            clusteredRenderer.addLight(pos, c, demoLightRadius, demoLightIntensity);
        }
    }

    void ResetLevelRuntimeState() {
        projectiles.clear();
        laserBeam.life = 0.0f;
        harpoonTether.life = 0.0f;
        pinnedHarpoons.clear();
        remoteCharges.clear();
        flamethrowerAudioTime = 0.0f;
        nextHarpoonId = 1;
        c4DetonateHeld = false;
        impactParticles.clear();
        explosionFX.clear();
        vortexFX.clear();
        firePatches.clear();
        burningTargets.clear();
        burningMaterials.clear();
        explosiveBarrels = {
            {{ 4.6f, 3.25f,  4.6f}},
            {{-4.6f, 3.25f,  4.6f}},
            {{ 4.6f, 3.25f, -4.6f}},
            {{-4.6f, 3.25f, -4.6f}},
        };
        // Dropped rather than re-armed: the level load that follows re-places
        // whatever pickups the map authors, and keeping stale ones here would
        // leave a collected rocket floating on a map that never had one.
        weaponPickups.clear();
        fireCooldown = 0.0f;
        muzzleFlashTime = 0.0f;
        muzzleFlashScale = 1.0f;
        muzzleFlashRotation = 0.0f;
        gunRecoilBack = 0.0f;
        gunRecoilKick = 0.0f;
        gunSwayYaw = gunSwayPitch = 0.0f;
        gunJumpPitch = gunJumpVelocity = 0.0f;
        gunPrevVerticalVel = 0.0f;
        // Assume grounded: a fresh spawn must not read as a landing.
        gunPrevGrounded = true;
        // Re-prime: the camera is about to be re-placed, and that jump must not
        // register as a turn the player made.
        gunSwayPrimed = false;
        // Or the reticle would start a fresh run still blown open from the last
        // burst of the previous one.
        crosshairSpread = 0.0f;
        crosshairSpreadTarget = 0.0f;
        hitMarkerTime = 0.0f;
        hitMarkerLethal = false;
        // Re-latches to full health on the next update rather than ghosting the
        // previous run's last hit down from wherever it stopped.
        healthChip = -1.0f;
        healthChipDelay = 0.0f;
        grenadeCooldown = 0.0f;
        molotovDamageCooldown = 0.0f;
        playerBurnTime = 0.0f;
        selectedGrenade = GrenadeType::Frag;
        player.damageFlash = 0.0f;
        player.regenTimer = 0.0f;
        adsBlend = 0.0f;
        adsActive = false;
    }

    void DamagePlayer(float damage) {
        if (player.godMode || damage <= 0.0f || player.health <= 0.0f) return;
        player.health = (std::max)(0.0f, player.health - damage);

        // Severity drives every hit reaction, on a curve rather than a ratio.
        //
        // A plain damage/reference ratio does not work across this range: the
        // sources span 2.4 (rifle round) to 100 (point-blank C4), a factor of
        // 40. Divide by a quarter of max health and everything above 25 damage
        // saturates, so a grenade and a C4 charge feel identical; divide by full
        // health instead and a rifle round lands at 0.024 and feels like
        // nothing. The square root compresses the top of the range while
        // lifting the bottom, keeping all of it distinguishable.
        //
        // Held rather than overwritten while a stronger flash is still fading,
        // so a graze cannot cancel the kick from a grenade.
        const float normalized =
            damage / (std::max)(1.0f, player.maxHealth);
        const float severity = (std::min)(1.0f, std::sqrt(normalized));
        player.damageFlashSeverity =
            (std::max)(player.damageFlashSeverity, severity);
        // Bigger hits flash longer as well as harder.
        player.damageFlash = (std::max)(player.damageFlash,
                                        0.22f + severity * 0.30f);
        // Hold the chip bar at its pre-hit length for a moment. Re-armed on
        // every hit, so sustained fire keeps the ghost visible instead of it
        // draining between rounds.
        healthChipDelay = 0.32f;

        // Camera kick, scaled the same way. Reuses the explosion trauma channel
        // the shake already reads, so a hit physically jolts the view instead of
        // only tinting it. Capped below a full explosion: taking a rifle round
        // should stagger the aim, not throw it off the target entirely.
        camera.AddHitTrauma(0.14f + severity * 0.42f);

        player.damageFlash = (std::min)(player.damageFlash, 0.55f);
        // Restart the hold-off on every hit, including the one that kills, so a
        // revive does not inherit a nearly expired timer.
        player.regenTimer = player.regenDelay;
    }

    // Damage with a known origin: records the direction so the HUD can show
    // where it came from. Everything else is identical to DamagePlayer.
    void DamagePlayerFrom(float damage, const XMFLOAT3& source) {
        if (player.godMode || damage <= 0.0f || player.health <= 0.0f) return;
        const float dx = source.x - camera.Position.x;
        const float dz = source.z - camera.Position.z;
        const float lengthSq = dx * dx + dz * dz;
        if (lengthSq > 1e-4f) {
            const float inverse = 1.0f / std::sqrt(lengthSq);
            player.lastHitDirX = dx * inverse;
            player.lastHitDirZ = dz * inverse;
            player.hitIndicator = 1.0f;
        }
        DamagePlayer(damage);
    }

    // Damage dealt to the player *by the enemy* -- bullets, grenades, molotovs.
    // Scaled by the deployment screen's difficulty multiplier.
    //
    // Deliberately separate from DamagePlayer rather than folding the multiplier
    // in there, because several callers are not the enemy hurting the player:
    // fall damage from a cut rappel rope, riding a helicopter into the ground,
    // going down with the sinking boat, and barrels the player usually shot
    // themselves. Scaling those would make a "harder enemies" dial quietly
    // change how far the player can safely fall, which is not what it says.
    void DamagePlayerFromEnemy(float damage) {
        DamagePlayer(damage * enemyDamageMultiplier);
    }

    // Enemy damage with a known origin, for the directional hit indicator.
    void DamagePlayerFromEnemyAt(float damage, const XMFLOAT3& source) {
        DamagePlayerFrom(damage * enemyDamageMultiplier, source);
    }

    void RestorePlayerHealth() {
        player.health = player.maxHealth;
        player.damageFlash = 0.0f;
        player.regenTimer = 0.0f;
        // Clear the hit reactions too, or a respawn inherits the vignette,
        // heartbeat and direction wedge from however the last life ended.
        player.damageFlashSeverity = 0.0f;
        player.hitIndicator = 0.0f;
        player.lowHealthPulse = 0.0f;
        player.lastHitDirX = 0.0f;
        player.lastHitDirZ = 0.0f;
        // Ammo rides along with health: every caller (level start, editor play,
        // the debug Restore button) wants a fully kitted player, and refilling
        // here keeps the two from drifting apart.
        RestoreAmmo();
    }

    bool HitPlayerProjectile(Projectile& p) {
        if (!p.hostile || !p.active || p.grenade) return false;
        const XMVECTOR a = XMLoadFloat3(&p.previousPosition);
        const XMVECTOR b = XMLoadFloat3(&p.position);
        const XMVECTOR chest = XMLoadFloat3(&camera.Position) +
            XMVectorSet(0.0f, -0.35f, 0.0f, 0.0f);
        const XMVECTOR ab = b - a;
        const float denom = XMVectorGetX(XMVector3LengthSq(ab));
        float t = denom > 1e-6f
            ? XMVectorGetX(XMVector3Dot(chest - a, ab)) / denom : 0.0f;
        t = (std::max)(0.0f, (std::min)(1.0f, t));
        const XMVECTOR closest = a + ab * t;
        if (XMVectorGetX(XMVector3LengthSq(chest - closest)) >= 0.45f * 0.45f)
            return false;
        XMStoreFloat3(&p.position, closest);
        // Rifle rounds stay at the long-standing 2.4 chip damage. Enemy shotgun
        // pellets and sniper rounds carry a multiplier so one hit means something
        // without needing a separate hit path.
        //
        // Back along the round's own travel to find the shooter: the projectile
        // carries no origin, but its direction is exactly the bearing the shot
        // came from, which is all the indicator needs.
        const XMFLOAT3 origin{ p.position.x - p.direction.x * 8.0f,
                               p.position.y - p.direction.y * 8.0f,
                               p.position.z - p.direction.z * 8.0f };
        DamagePlayerFromEnemyAt(2.4f * p.damageMultiplier, origin);
        p.active = false;
        return true;
    }

    void Update(float dt, float currentTime) {
        camera.Update(dt);

        muzzleFlashTime = (std::max)(0.0f, muzzleFlashTime - dt);
        molotovDamageCooldown = (std::max)(
            0.0f, molotovDamageCooldown - dt);
        player.damageFlash = (std::max)(0.0f, player.damageFlash - dt);
        // Severity follows the flash down so the next hit starts from a clean
        // slate; without this a single grenade would keep every later graze at
        // grenade strength for the rest of the run.
        if (player.damageFlash <= 0.0f) player.damageFlashSeverity = 0.0f;
        // Slower than the flash on purpose -- see the field comment.
        player.hitIndicator = (std::max)(0.0f, player.hitIndicator - dt * 0.55f);

        // Low-health ramp. Eases in and out rather than snapping at the
        // threshold, so crossing it is a mood change and not a light switch.
        {
            const float fraction = player.maxHealth > 0.0f
                ? player.health / player.maxHealth : 1.0f;
            constexpr float kLowHealthThreshold = 0.35f;
            const float target = (player.health > 0.0f &&
                                  fraction < kLowHealthThreshold)
                ? 1.0f - fraction / kLowHealthThreshold
                : 0.0f;
            const float rate = target > player.lowHealthPulse ? 2.4f : 1.1f;
            player.lowHealthPulse += (target - player.lowHealthPulse) *
                                     (std::min)(1.0f, rate * dt);
        }
        if (playerBurnTime > 0.0f) {
            playerBurnTime = (std::max)(0.0f, playerBurnTime - dt);
            DamagePlayerFromEnemy(molotovDamagePerSecond * 0.62f * dt);
        }

        // Health regen. Death is final: at zero health the timer stops rather
        // than quietly healing a corpse back to fighting strength.
        if (player.healthRegen && player.health > 0.0f &&
            player.health < player.maxHealth) {
            player.regenTimer = (std::max)(0.0f, player.regenTimer - dt);
            if (player.regenTimer <= 0.0f) {
                player.health = (std::min)(player.maxHealth,
                    player.health + player.HealthRegenPerSecond() * dt);
            }
        }
        // Sharp impulse, quick mechanical return. Camera aim stays displaced,
        // so automatic fire climbs unless the player actively compensates.
        gunRecoilBack = (std::max)(0.0f, gunRecoilBack - 1.45f * dt);
        gunRecoilKick = (std::max)(0.0f, gunRecoilKick - 95.0f * dt);

        // Weapon sway. Turning the camera leaves the gun behind for a moment,
        // then it eases back to centre -- weight, not a wobble. The offset is
        // driven by how far the view turned this frame, not by a clock, so it
        // only appears when the player actually moves the mouse.
        {
            float deltaYaw = camera.Yaw - gunSwayPrevYaw;
            // Yaw is unbounded and wraps; a wrap would otherwise read as one
            // enormous turn and slam the gun to its clamp.
            while (deltaYaw >  180.0f) deltaYaw -= 360.0f;
            while (deltaYaw < -180.0f) deltaYaw += 360.0f;
            const float deltaPitch = camera.Pitch - gunSwayPrevPitch;
            gunSwayPrevYaw   = camera.Yaw;
            gunSwayPrevPitch = camera.Pitch;

            if (!gunSwayPrimed) {
                // First frame after a load or teleport: the previous angles were
                // never valid, so adopt them without swaying.
                gunSwayPrimed = true;
            } else if (dt > 0.0f) {
                // Aiming tightens the weapon against the shoulder, so sway all
                // but disappears down the sights where it would hurt most.
                const float sighted =
                    (std::min)(1.0f, (std::max)(0.0f, adsBlend));
                const float amount = kGunSwayAmount * (1.0f - 0.82f * sighted);
                gunSwayYaw   -= deltaYaw   * amount;
                gunSwayPitch -= deltaPitch * amount;
                // Clamp before the return so a fast flick cannot fling the gun
                // out of frame, and the recovery always starts from a sane pose.
                gunSwayYaw   = (std::min)(kGunSwayMaxDegrees,
                               (std::max)(-kGunSwayMaxDegrees, gunSwayYaw));
                gunSwayPitch = (std::min)(kGunSwayMaxDegrees,
                               (std::max)(-kGunSwayMaxDegrees, gunSwayPitch));
                // Exponential return keeps the settle frame-rate independent.
                const float settle = std::exp(-kGunSwayReturnRate * dt);
                gunSwayYaw   *= settle;
                gunSwayPitch *= settle;
            }
        }

        // Jump and land. Leaving the ground kicks the weapon up as the player's
        // hands trail the launch; hitting the ground drives it down under the
        // impact, harder the faster the fall. Between the two it is a damped
        // spring, so the motion carries through instead of snapping to rest.
        {
            const bool grounded = camera.IsGrounded;
            if (gunPrevGrounded && !grounded) {
                // Only a real push off the ground kicks the gun up. Walking off
                // a kerb or a single frame of physics jitter also clears
                // IsGrounded, but leaves with no upward speed -- treating those
                // as jumps flicked the weapon at random while just walking.
                if (camera.VerticalVelocity > kGunJumpMinLaunchSpeed)
                    gunJumpVelocity += kGunJumpLaunchKick;
            } else if (!gunPrevGrounded && grounded) {
                // Land: downward, scaled by how fast the player was falling on
                // the frame before touchdown -- the camera has already cleared
                // VerticalVelocity by now. A step down barely registers; a roof
                // drop drives the gun to its stop.
                const float impact =
                    (std::min)(1.0f, (std::max)(0.0f, -gunPrevVerticalVel) /
                                         kGunJumpRefLandSpeed);
                gunJumpVelocity -= kGunJumpLandKick * impact;
            }
            gunPrevGrounded = grounded;
            gunPrevVerticalVel = camera.VerticalVelocity;

            if (dt > 0.0f) {
                // Semi-implicit Euler: velocity first, then position, so the
                // spring stays stable at the frame rates the game actually runs.
                gunJumpVelocity += (-kGunJumpStiffness * gunJumpPitch -
                                    kGunJumpDamping * gunJumpVelocity) * dt;
                gunJumpPitch += gunJumpVelocity * dt;
                if (gunJumpPitch > kGunJumpMaxDegrees) {
                    gunJumpPitch = kGunJumpMaxDegrees;
                    if (gunJumpVelocity > 0.0f) gunJumpVelocity = 0.0f;
                } else if (gunJumpPitch < -kGunJumpMaxDegrees) {
                    gunJumpPitch = -kGunJumpMaxDegrees;
                    if (gunJumpVelocity < 0.0f) gunJumpVelocity = 0.0f;
                }
            }
        }

        // Crosshair bloom chases its target asymmetrically: opening is nearly
        // instant so the first shot of a burst is visible, closing is slow so
        // the player has to actually wait out the recovery rather than tapping
        // through it. Exponential easing keeps both frame-rate independent.
        {
            const float rate = crosshairSpreadTarget > crosshairSpread
                ? 22.0f     // bloom
                : 5.5f;     // recover
            const float blend = 1.0f - std::exp(-rate * (std::max)(0.0f, dt));
            crosshairSpread += (crosshairSpreadTarget - crosshairSpread) * blend;
            if (crosshairSpread < 0.01f) crosshairSpread = 0.0f;
        }

        if (hitMarkerTime > 0.0f) {
            hitMarkerTime = (std::max)(0.0f, hitMarkerTime - dt);
            if (hitMarkerTime <= 0.0f) hitMarkerLethal = false;
        }

        // Chip health. Holds at the pre-hit value briefly so the eye can catch
        // how much was lost, then drains to meet the real bar.
        if (healthChip < 0.0f) {
            healthChip = player.health;      // first frame: no ghost
        } else if (player.health >= healthChip) {
            healthChip = player.health;      // healing/regen: no ghost
            healthChipDelay = 0.0f;
        } else {
            if (healthChipDelay > 0.0f) {
                healthChipDelay = (std::max)(0.0f, healthChipDelay - dt);
            } else {
                // Proportional drain with a floor, so a small chip still
                // finishes promptly and a large one does not crawl.
                const float speed =
                    (std::max)(18.0f, (healthChip - player.health) * 3.2f);
                healthChip = (std::max)(player.health, healthChip - speed * dt);
            }
        }

        // Animate demo lights
        if (animateDemoLights) {
            for (int i = 0; i < clusteredRenderer.getTotalLightCount(); i++) {
                float angle = (float)i / clusteredRenderer.getTotalLightCount() * XM_2PI + currentTime;
                float r = 8.0f;
                XMFLOAT3 pos(cosf(angle) * r,
                             4.0f + sinf(currentTime * 2.0f + angle) * 1.0f,
                             sinf(angle) * r);
                clusteredRenderer.updateLight(i, pos);
            }
        }

        if (animateLight) {
            lightPos.x = cosf(currentTime * animationSpeed) * 10.0f;
            lightPos.z = sinf(currentTime * animationSpeed) * 10.0f;
        }

        if (animateCube) {
            cube1.rotation.y = currentTime * 50.0f * animationSpeed;
        }

        // Projectiles
        for (auto& p : projectiles) {
            if (!p.active) continue;
            p.previousPosition = p.position;
            if (p.remoteCharge) {
                p.velocity.y += -9.81f * dt;
                p.position.x += p.velocity.x * dt;
                p.position.y += p.velocity.y * dt;
                p.position.z += p.velocity.z * dt;
                p.lifetime -= dt;
                if (p.lifetime <= 0.0f) p.active = false;
            } else if (p.grenade) {
                // Frag grenades bounce until fuse expiry. Molotov and vortex
                // grenades trigger immediately on first ground contact.
                p.grenadeCollisionGrace = (std::max)(
                    0.0f, p.grenadeCollisionGrace - dt);
                // Box3D owns gravity, contact response, friction, spin, and
                // sleeping once the runtime creates the thrown rigid body.
                // Keep the original ground fallback for levels without physics.
                if (!p.held && p.grenadePhysicsHandle == 0) {
                    p.velocity.y += -9.81f * grenadeGravityScale * dt;
                    p.position.x += p.velocity.x * dt;
                    p.position.y += p.velocity.y * dt;
                    p.position.z += p.velocity.z * dt;
                    const float surfaceY = grenadeGroundHeight
                        ? grenadeGroundHeight(p.position.x, p.position.z) : 0.0f;
                    const float bounceY = surfaceY + grenadeGroundY;
                    if (p.position.y < bounceY) {
                        p.position.y = bounceY;
                        if (p.molotov || p.vortex || p.impactFuse) {
                            p.detonate = true;
                            p.active = false;
                        } else if (p.velocity.y < 0.0f) {
                            p.velocity.y = -p.velocity.y * 0.4f;
                        }
                        if (!p.molotov && !p.vortex && !p.impactFuse) {
                            p.velocity.x *= 0.7f;
                            p.velocity.z *= 0.7f;
                        }
                    }
                }
                if (p.molotov && p.active) {
                    p.fxCooldown -= dt;
                    if (p.fxCooldown <= 0.0f) {
                        p.fxCooldown = 0.045f;
                        ImpactParticle wick;
                        wick.position = {
                            p.position.x, p.position.y + 0.18f, p.position.z };
                        wick.velocity = { 0.0f, 2.5f, 0.0f };
                        wick.maxLife = wick.life = 0.20f;
                        wick.size = 0.055f;
                        wick.growth = -0.08f;
                        wick.color = { 1.0f, 0.25f, 0.01f };
                        wick.spark = true;
                        impactParticles.push_back(wick);
                    }
                }
                p.fuse -= dt;
                if (p.fuse <= 0.0f && !p.detonate) {
                    p.held = false;
                    p.detonate = true;
                    p.active = false;
                }
            } else if (p.harpoon) {
                if (!p.harpoonExpired) {
                    p.position.x += p.direction.x * p.speed * dt;
                    p.position.y += p.direction.y * p.speed * dt;
                    p.position.z += p.direction.z * p.speed * dt;
                }
                p.lifetime -= dt;
                if (p.lifetime <= 0.0f) p.harpoonExpired = true;
            } else {
                if (p.rocket) {
                    // Battlefield 2-style wire guidance: rocket bends toward the
                    // player's current crosshair, with finite steering authority.
                    const XMVECTOR aim = XMLoadFloat3(&camera.Position) +
                                         XMLoadFloat3(&camera.Front) * 500.0f;
                    const XMVECTOR position = XMLoadFloat3(&p.position);
                    const XMVECTOR current = XMVector3Normalize(XMLoadFloat3(&p.direction));
                    const XMVECTOR desired = XMVector3Normalize(aim - position);
                    const float dot = (std::max)(-1.0f, (std::min)(1.0f,
                        XMVectorGetX(XMVector3Dot(current, desired))));
                    const float angle = std::acos(dot);
                    const float blend = angle > 1e-4f
                        ? (std::min)(1.0f, rocketTurnRate * dt / angle) : 1.0f;
                    XMStoreFloat3(&p.direction,
                        XMVector3Normalize(XMVectorLerp(current, desired, blend)));
                }
                p.position.x += p.direction.x * p.speed * dt;
                p.position.y += p.direction.y * p.speed * dt;
                p.position.z += p.direction.z * p.speed * dt;
                p.lifetime -= dt;
                if (p.lifetime <= 0.0f) {
                    p.active = false;
                    if (p.rocket) p.detonate = true;
                }

            }
        }
        // Keep detonating grenades for one more frame so the game loop can read
        // p.detonate; drop the rest.
        projectiles.erase(
            std::remove_if(projectiles.begin(), projectiles.end(),
                [](const Projectile& p) { return !p.active && !p.detonate; }),
            projectiles.end());
        laserBeam.life = (std::max)(0.0f, laserBeam.life - dt);
        harpoonTether.life = (std::max)(0.0f, harpoonTether.life - dt);
        flamethrowerAudioTime = (std::max)(
            0.0f, flamethrowerAudioTime - dt);

        // Impact particles: sparks/blood fall; smoke rises and expands.
        for (auto& ip : impactParticles) {
            if (ip.spark) {
                ip.velocity.y += -22.0f * dt;          // gravity pulls sparks down
                ip.velocity.x *= 0.99f; ip.velocity.z *= 0.99f;
            } else if (ip.blood) {
                ip.velocity.y += -5.5f * dt;
                ip.velocity.x *= 0.93f; ip.velocity.y *= 0.96f; ip.velocity.z *= 0.93f;
            } else {
                ip.velocity.y += 0.6f * dt;            // buoyancy: smoke rises
                // Turbulence: a little wandering push so the plume curls and
                // billows instead of drifting in a straight line.
                auto jit = [&]() { return ((float)std::rand() / RAND_MAX * 2.0f - 1.0f); };
                ip.velocity.x += jit() * 1.2f * dt;
                ip.velocity.z += jit() * 1.2f * dt;
                ip.velocity.x *= 0.94f; ip.velocity.z *= 0.94f; ip.velocity.y *= 0.97f;
            }
            ip.position.x += ip.velocity.x * dt;
            ip.position.y += ip.velocity.y * dt;
            ip.position.z += ip.velocity.z * dt;
            ip.size = std::max(0.0f, ip.size + ip.growth * dt);
            ip.life -= dt;
        }
        impactParticles.erase(
            std::remove_if(impactParticles.begin(), impactParticles.end(),
                [](const ImpactParticle& p) { return p.life <= 0.0f; }),
            impactParticles.end());

        for (auto& fx : explosionFX) fx.age += dt;
        explosionFX.erase(
            std::remove_if(explosionFX.begin(), explosionFX.end(),
                [](const ExplosionFX& fx) { return fx.age >= fx.duration; }),
            explosionFX.end());

        for (VortexFX& fx : vortexFX) {
            fx.age += dt;
            fx.particleCooldown -= dt;
            if (fx.particleCooldown > 0.0f) continue;
            // Sparse sparks only. Physical debris carries the effect; flooding
            // the shell with cards made the vortex look voxelized.
            fx.particleCooldown = 0.09f;
            for (int particleIndex = 0; particleIndex < 2; ++particleIndex) {
                const float phase = fx.age * 5.8f +
                    XM_PI * static_cast<float>(particleIndex);
                const float latitudePhase = fx.age * 2.3f +
                    particleIndex * 2.1f;
                const float latitude = std::sin(latitudePhase) * 0.82f;
                const float latitudeRadius = std::cos(latitude);
                const float ring = fx.radius * (0.30f + particleIndex * 0.13f);
                const float centerY = fx.position.y + fx.radius * 0.35f;
                ImpactParticle spark;
                spark.position = {
                    fx.position.x + std::cos(phase) * latitudeRadius * ring,
                    centerY + std::sin(latitude) * ring,
                    fx.position.z + std::sin(phase) * latitudeRadius * ring };
                spark.velocity = {
                    -std::sin(phase) * latitudeRadius * 3.4f,
                    std::cos(latitudePhase) * 2.1f,
                     std::cos(phase) * latitudeRadius * 3.4f };
                spark.maxLife = spark.life = 0.34f;
                spark.size = 0.055f;
                spark.growth = -0.09f;
                spark.color = particleIndex & 1
                    ? XMFLOAT3{ 0.18f, 0.75f, 1.0f }
                    : XMFLOAT3{ 0.62f, 0.18f, 1.0f };
                spark.spark = true;
                impactParticles.push_back(spark);
            }
        }
        vortexFX.erase(
            std::remove_if(vortexFX.begin(), vortexFX.end(),
                [](const VortexFX& fx) { return fx.age >= fx.duration; }),
            vortexFX.end());

        std::vector<FirePatch> spread;
        for (FirePatch& fire : firePatches) {
            fire.life -= dt;
            fire.spreadDelay -= dt;
            fire.fxCooldown -= dt;
            fire.structureDamageCooldown -= dt;
            fire.radius = (std::min)(fire.maxRadius, fire.radius + dt * 0.72f);

            if (fire.fxCooldown <= 0.0f) {
                fire.fxCooldown = 0.075f;
                auto randomSigned = []() {
                    return ((float)std::rand() / RAND_MAX) * 2.0f - 1.0f;
                };
                ImpactParticle ember;
                ember.position = {
                    fire.position.x + randomSigned() * fire.radius * 0.55f,
                    fire.position.y + 0.10f,
                    fire.position.z + randomSigned() * fire.radius * 0.55f };
                ember.velocity = { randomSigned() * 0.28f,
                    2.4f + std::abs(randomSigned()) * 1.6f,
                    randomSigned() * 0.28f };
                ember.maxLife = ember.life = 0.18f +
                    std::abs(randomSigned()) * 0.24f;
                ember.size = 0.035f + std::abs(randomSigned()) * 0.055f;
                ember.growth = -0.07f;
                ember.color = { 1.0f,
                    0.18f + std::abs(randomSigned()) * 0.34f, 0.008f };
                ember.spark = true;
                impactParticles.push_back(ember);
                if ((std::rand() % 7) == 0)
                    SpawnSmokeBurst(fire.position, fire.radius * 0.16f, 0.12f);
            }

            if (!fire.hasSpread && fire.spreadDelay <= 0.0f &&
                fire.generation < 2) {
                fire.hasSpread = true;
                const int children = fire.generation == 0 ? 6 : 2;
                for (int childIndex = 0; childIndex < children; ++childIndex) {
                    if (firePatches.size() + spread.size() >= 64) break;
                    const float jitter = ((float)std::rand() / RAND_MAX - 0.5f) * 0.55f;
                    const float angle = XM_2PI * childIndex / children + jitter;
                    const XMFLOAT2 radial{ std::cos(angle), std::sin(angle) };
                    const float windAngle = currentTime * 0.075f + 0.65f;
                    XMFLOAT2 direction{
                        radial.x * 0.78f + std::cos(windAngle) * 0.45f,
                        radial.y * 0.78f + std::sin(windAngle) * 0.45f };
                    const float directionLength = std::sqrt(
                        direction.x * direction.x + direction.y * direction.y);
                    if (directionLength > 0.001f) {
                        direction.x /= directionLength;
                        direction.y /= directionLength;
                    }
                    const float distance = 0.95f +
                        ((float)std::rand() / RAND_MAX) * 0.55f;
                    const float x = fire.position.x + direction.x * distance;
                    const float z = fire.position.z + direction.y * distance;
                    const float ground = grenadeGroundHeight
                        ? grenadeGroundHeight(x, z) : 0.0f;
                    if (ground < -0.35f) continue;
                    bool overlaps = false;
                    for (const FirePatch& existing : firePatches) {
                        const float dx = existing.position.x - x;
                        const float dz = existing.position.z - z;
                        if (dx * dx + dz * dz < 0.36f * 0.36f) {
                            overlaps = true;
                            break;
                        }
                    }
                    if (!overlaps) {
                        for (const FirePatch& pending : spread) {
                            const float dx = pending.position.x - x;
                            const float dz = pending.position.z - z;
                            if (dx * dx + dz * dz < 0.36f * 0.36f) {
                                overlaps = true;
                                break;
                            }
                        }
                    }
                    if (overlaps) continue;
                    FirePatch child;
                    child.position = { x, ground + 0.05f, z };
                    child.maxLife = child.life = molotovFireDuration -
                        (fire.generation + 1) * 0.8f;
                    child.radius = 0.22f;
                    child.maxRadius = fire.generation == 0 ? 0.78f : 0.64f;
                    child.spreadDelay = 0.45f +
                        ((float)std::rand() / RAND_MAX) * 0.45f;
                    child.generation = fire.generation + 1;
                    spread.push_back(child);
                }
            }
        }
        firePatches.insert(firePatches.end(), spread.begin(), spread.end());
        firePatches.erase(
            std::remove_if(firePatches.begin(), firePatches.end(),
                [](const FirePatch& fire) { return fire.life <= 0.0f; }),
            firePatches.end());
        for (BurningMaterial& material : burningMaterials) {
            material.life -= dt;
            material.damageCooldown = (std::max)(
                0.0f, material.damageCooldown - dt);
        }
        burningMaterials.erase(
            std::remove_if(burningMaterials.begin(), burningMaterials.end(),
                [](const BurningMaterial& material) {
                    return material.life <= 0.0f;
                }),
            burningMaterials.end());
        if (impactParticles.size() > 1000)
            impactParticles.erase(impactParticles.begin(),
                impactParticles.begin() + (impactParticles.size() - 1000));
    }

    // Kick off one animated explosion flipbook centred on `center`.
    // `size` is the billboard's full-bloom world diameter.
    void SpawnExplosionFX(const XMFLOAT3& center, float size,
                          float duration = 0.9f, bool grenade = false) {
        ExplosionFX fx;
        fx.position = center;
        fx.size = size;
        fx.duration = duration;
        fx.rotation = ((float)std::rand() / RAND_MAX) * XM_2PI;
        explosionFX.push_back(fx);
        camera.ApplyExplosionImpulse(center, size);
        if (explosionAudioCallback) explosionAudioCallback(center, size, grenade);

        SpawnSmokeBurst(center, size * 0.18f, 1.35f);
        auto randomSigned = []() {
            return ((float)std::rand() / RAND_MAX) * 2.0f - 1.0f;
        };
        for (int i = 0; i < 34; ++i) {
            ImpactParticle spark;
            spark.position = center;
            spark.velocity = {
                randomSigned() * size * 1.8f,
                2.5f + std::abs(randomSigned()) * size * 2.2f,
                randomSigned() * size * 1.8f };
            spark.maxLife = spark.life = 0.35f + std::abs(randomSigned()) * 0.75f;
            spark.size = 0.04f + std::abs(randomSigned()) * 0.09f;
            spark.growth = -spark.size * 0.72f;
            spark.color = { 1.0f, 0.20f + std::abs(randomSigned()) * 0.34f, 0.01f };
            spark.spark = true;
            impactParticles.push_back(spark);
        }

        // Fast, ground-hugging dust ring. Its lateral velocity makes the blast
        // read in world space even when the fireball billboard faces the camera.
        constexpr int dustCount = 20;
        for (int i = 0; i < dustCount; ++i) {
            const float angle = XM_2PI * ((float)i + 0.35f * randomSigned()) /
                                (float)dustCount;
            const float radial = size * (0.08f + 0.025f * std::abs(randomSigned()));
            const float speed = size * (0.75f + 0.25f * std::abs(randomSigned()));
            ImpactParticle dust;
            dust.position = { center.x + std::cos(angle) * radial,
                              center.y + 0.06f + std::abs(randomSigned()) * size * 0.035f,
                              center.z + std::sin(angle) * radial };
            dust.velocity = { std::cos(angle) * speed,
                              0.15f + std::abs(randomSigned()) * 0.35f,
                              std::sin(angle) * speed };
            dust.maxLife = dust.life = 1.0f + std::abs(randomSigned()) * 0.8f;
            dust.size = size * (0.035f + std::abs(randomSigned()) * 0.025f);
            dust.growth = size * (0.10f + std::abs(randomSigned()) * 0.06f);
            const float grey = 0.16f + std::abs(randomSigned()) * 0.10f;
            dust.color = { grey * 1.10f, grey, grey * 0.85f };
            dust.spark = false;
            impactParticles.push_back(dust);
        }
        if (impactParticles.size() > 1000)
            impactParticles.erase(impactParticles.begin(),
                impactParticles.begin() + (impactParticles.size() - 1000));
    }

    // Bullet impact: just a soft smoke puff kicked off the surface (no sparks).
    // `normal` points back out of the surface (reverse of the bullet's travel).
    void SpawnBulletImpact(const XMFLOAT3& point, const XMFLOAT3& normal) {
        const XMFLOAT3 at{ point.x + normal.x * 0.1f,
                           point.y + normal.y * 0.1f,
                           point.z + normal.z * 0.1f };
        SpawnSmokeBurst(at, 0.35f, 0.5f);
    }

    // A rolling cloud of smoke for when things break: a dense, long-lived,
    // billowing plume centred on `center`. `radius` sizes the spread, `intensity`
    // (~0.3 dust puff .. ~1.5 big explosion) scales puff count/size/darkness.
    // Cores start dark (sooty) and lighten as they expand and thin out.
    void SpawnSmokeBurst(const XMFLOAT3& center, float radius, float intensity = 1.0f) {
        auto rnd = [&]() { return (float)std::rand() / RAND_MAX * 2.0f - 1.0f; };
        int spawned = 0;

        const int puffs = std::max(1, (int)(10 * intensity));
        for (int i = 0; i < puffs; ++i) {
            ImpactParticle sp;
            // Seed puffs across a rough sphere so the cloud has body.
            sp.position = { center.x + rnd() * radius,
                            center.y + std::abs(rnd()) * radius * 0.7f,
                            center.z + rnd() * radius };
            // Roll outward and up; bigger bursts push harder.
            const float out = 1.5f + 2.5f * intensity;
            sp.velocity = { rnd() * out,
                            std::abs(rnd()) * out * 0.6f + 0.8f * intensity,
                            rnd() * out };
            sp.maxLife = sp.life = 1.2f + std::abs(rnd()) * (1.4f + intensity);
            sp.size    = radius * (0.35f + std::abs(rnd()) * 0.4f);
            sp.growth  = (0.5f + std::abs(rnd()) * 0.7f) * intensity;   // billow out
            // Sooty dark grey core; larger/darker for stronger bursts.
            const float g = 0.10f + std::abs(rnd()) * 0.18f;
            sp.color = { g, g, g };
            sp.spark = false;
            impactParticles.push_back(sp); ++spawned;
        }

        if (impactParticles.size() > 800)
            impactParticles.erase(impactParticles.begin(), impactParticles.begin() + spawned);
    }

    // Thin smoke curling out of a fresh bullet hole. Deliberately not
    // SpawnSmokeBurst: that seeds a sphere and rolls outward for an explosion,
    // which at hole scale reads as a puff of dust in mid-air. This drifts off
    // the surface along its normal and rises, so it stays attached to the mark.
    void SpawnBulletHoleSmoke(const XMFLOAT3& position, const XMFLOAT3& normal,
                              float intensity = 1.0f) {
        auto rnd = [&]() { return (float)std::rand() / RAND_MAX * 2.0f - 1.0f; };
        auto unit = [&]() { return (float)std::rand() / RAND_MAX; };
        const int puffs = (std::max)(1, (int)(3.0f * intensity));
        int spawned = 0;
        for (int i = 0; i < puffs; ++i) {
            ImpactParticle sp;
            // Start just off the surface, jittered across the hole's mouth
            // rather than around a sphere.
            constexpr float kMouth = 0.035f;
            sp.position = {
                position.x + normal.x * 0.02f + rnd() * kMouth,
                position.y + normal.y * 0.02f + rnd() * kMouth,
                position.z + normal.z * 0.02f + rnd() * kMouth };
            // Mostly straight out of the hole, with buoyancy taking over as it
            // leaves. Slow: fast wisps look like steam jets.
            const float out = 0.22f + unit() * 0.28f;
            sp.velocity = { normal.x * out + rnd() * 0.06f,
                            normal.y * out + 0.20f + unit() * 0.18f,
                            normal.z * out + rnd() * 0.06f };
            sp.maxLife = sp.life = 0.55f + unit() * 0.75f;
            sp.size    = 0.018f + unit() * 0.022f;
            sp.growth  = 0.25f + unit() * 0.35f;
            // Pale grey: burnt propellant, lighter than the sooty explosion
            // smoke so it stays visible against dark walls.
            const float g = 0.34f + unit() * 0.22f;
            sp.color = { g, g, g };
            sp.spark = false;
            impactParticles.push_back(sp); ++spawned;
        }
        if (impactParticles.size() > 800)
            impactParticles.erase(impactParticles.begin(),
                                  impactParticles.begin() + spawned);
    }

    void TriggerMuzzleFlash(float durationScale, float sizeScale) {
        muzzleFlashTime = muzzleFlashDuration * durationScale;
        muzzleFlashScale = sizeScale;
        muzzleFlashRotation = ((float)std::rand() / RAND_MAX) * XM_2PI;
    }

    void SpawnWeaponSmoke(const XMFLOAT3& muzzle, const XMFLOAT3& direction,
                          float intensity) {
        const int puffCount = (std::max)(1, (int)std::ceil(intensity));
        auto unit = [&]() { return (float)std::rand() / RAND_MAX; };
        auto signedUnit = [&]() { return unit() * 2.0f - 1.0f; };
        for (int puffIndex = 0; puffIndex < puffCount; ++puffIndex) {
            const float forwardOffset = 0.025f + unit() * 0.075f;
            ImpactParticle smoke = {};
            smoke.position = {
                muzzle.x + direction.x * forwardOffset + signedUnit() * 0.012f,
                muzzle.y + direction.y * forwardOffset + signedUnit() * 0.010f,
                muzzle.z + direction.z * forwardOffset + signedUnit() * 0.012f };
            const float forwardSpeed = 0.35f + unit() * (0.65f + intensity * 0.2f);
            smoke.velocity = {
                direction.x * forwardSpeed + signedUnit() * 0.08f,
                direction.y * forwardSpeed + 0.18f + unit() * 0.22f,
                direction.z * forwardSpeed + signedUnit() * 0.08f };
            smoke.maxLife = smoke.life = 0.34f + unit() * 0.30f;
            smoke.size = 0.028f * std::sqrt(intensity) * (0.8f + unit() * 0.5f);
            smoke.growth = 0.10f + intensity * 0.035f + unit() * 0.045f;
            const float grey = 0.58f + unit() * 0.18f;
            smoke.color = { grey, grey * 0.98f, grey * 0.94f };
            impactParticles.push_back(smoke);
        }
        if (impactParticles.size() > 900) {
            impactParticles.erase(impactParticles.begin(),
                impactParticles.begin() + (impactParticles.size() - 900));
        }
    }

    // A round breaking the surface throws water up. The ring wave the water
    // sim raises is a flat deformation -- correct, but it reads as nothing at
    // a distance -- so the visible part is here: a narrow column of droplets
    // kicked straight up, plus a low outward crown around the entry point.
    //
    // Droplets are spawned as sparks. Not for the brightness, but for the
    // motion: the spark branch of the particle integrator is the only one with
    // real gravity (-22) and near-zero drag, which is exactly the ballistic arc
    // water needs. The colour makes it read as water rather than embers.
    void SpawnWaterSplash(const XMFLOAT3& point, float strength = 1.0f) {
        auto rnd = [&]() { return (float)std::rand() / RAND_MAX * 2.0f - 1.0f; };
        int spawned = 0;

        // Droplets are small in world units, so a splash 150 m out lands well
        // under a pixel and reads as nothing. Grow them with distance to hold
        // a roughly constant on-screen size -- the splash stays legible at
        // range without looking oversized underfoot. Capped so a shot at the
        // horizon does not throw a house-sized plume.
        const float ddx = point.x - camera.Position.x;
        const float ddy = point.y - camera.Position.y;
        const float ddz = point.z - camera.Position.z;
        const float distance = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
        const float sizeScale =
            (std::min)(6.0f, (std::max)(1.0f, distance / 18.0f));

        // Central column: fast, near-vertical, the part that carries height.
        const int jets = (std::max)(3, (int)(7 * strength));
        for (int i = 0; i < jets; ++i) {
            ImpactParticle drop;
            drop.position = { point.x + rnd() * 0.10f,
                              point.y + 0.04f,
                              point.z + rnd() * 0.10f };
            // Height scales with distance for the same reason size does: the
            // column has to subtend a usable angle to be seen at all. Square
            // root so it grows more gently than the droplets themselves.
            const float reach = std::sqrt(sizeScale);
            const float up = (3.4f + std::abs(rnd()) * 2.6f) * strength * reach;
            drop.velocity = { rnd() * 0.9f * reach, up, rnd() * 0.9f * reach };
            drop.maxLife = drop.life = (0.45f + std::abs(rnd()) * 0.35f) * reach;
            drop.size = (0.05f + std::abs(rnd()) * 0.05f) * sizeScale;
            drop.growth = -drop.size * 0.35f;
            // Pale blue-white: foam, not clear water.
            const float w = 0.72f + std::abs(rnd()) * 0.28f;
            drop.color = { w * 0.80f, w * 0.92f, w };
            drop.spark = true;
            impactParticles.push_back(drop); ++spawned;
        }

        // Crown: slower, thrown outward and low, so the splash has a base
        // instead of being a bare vertical line.
        const int crown = (std::max)(4, (int)(9 * strength));
        for (int i = 0; i < crown; ++i) {
            const float angle = XM_2PI * ((float)i + 0.4f * rnd()) / (float)crown;
            const float out = (1.5f + std::abs(rnd()) * 1.4f) * strength *
                              std::sqrt(sizeScale);
            ImpactParticle drop;
            drop.position = { point.x + std::cos(angle) * 0.12f,
                              point.y + 0.03f,
                              point.z + std::sin(angle) * 0.12f };
            drop.velocity = { std::cos(angle) * out,
                              1.1f + std::abs(rnd()) * 1.3f,
                              std::sin(angle) * out };
            drop.maxLife = drop.life =
                (0.35f + std::abs(rnd()) * 0.30f) * std::sqrt(sizeScale);
            drop.size = (0.04f + std::abs(rnd()) * 0.04f) * sizeScale;
            drop.growth = -drop.size * 0.30f;
            const float w = 0.70f + std::abs(rnd()) * 0.30f;
            drop.color = { w * 0.78f, w * 0.90f, w };
            drop.spark = true;
            impactParticles.push_back(drop); ++spawned;
        }

        if (impactParticles.size() > 1000)
            impactParticles.erase(impactParticles.begin(),
                impactParticles.begin() + spawned);
    }

    void SpawnBloodBurst(const XMFLOAT3& point, const XMFLOAT3& normal) {
        auto rnd = [&]() { return (float)std::rand() / RAND_MAX * 2.0f - 1.0f; };
        constexpr int droplets = 8;
        for (int i = 0; i < droplets; ++i) {
            ImpactParticle blood;
            blood.position = {
                point.x + normal.x * 0.08f + rnd() * 0.08f,
                point.y + normal.y * 0.08f + rnd() * 0.08f,
                point.z + normal.z * 0.08f + rnd() * 0.08f
            };
            const float push = 0.8f + std::abs(rnd()) * 1.8f;
            blood.velocity = {
                normal.x * push + rnd() * 1.2f,
                normal.y * push + std::abs(rnd()) * 1.4f,
                normal.z * push + rnd() * 1.2f
            };
            blood.maxLife = blood.life = 0.32f + std::abs(rnd()) * 0.38f;
            blood.size = 0.12f + std::abs(rnd()) * 0.18f;
            blood.growth = 0.22f + std::abs(rnd()) * 0.28f;
            blood.color = { 0.30f, 0.18f, 0.18f };
            blood.blood = true;
            impactParticles.push_back(blood);
        }
        if (impactParticles.size() > 800)
            impactParticles.erase(impactParticles.begin(),
                                  impactParticles.begin() + droplets);
    }

    // Half-angle, in radians, that the current crosshair bloom stands for. The
    // reticle is the promise; this is what makes it true -- both read the same
    // `crosshairSpread`, so an arm pushed N pixels out and the cone a bullet can
    // land in cannot drift apart.
    //
    // Tuned against the reticle's own 15 px cap: full bloom is ~0.030 rad, a bit
    // over half the shotgun's fixed 0.055 pellet spread. A sprinting rifle burst
    // should be bad, not useless.
    float CurrentShotSpreadRadians() const {
        constexpr float kRadiansPerPixel = 0.0020f;
        return (std::max)(0.0f, crosshairSpread) * kRadiansPerPixel;
    }

    // Scatters a direction inside the cone the reticle is currently showing.
    // Square distribution rather than a disc: it matches the crosshair, which is
    // four arms on two axes, not a ring.
    XMFLOAT3 ApplyShotSpread(const XMFLOAT3& direction) const {
        const float spread = CurrentShotSpreadRadians();
        if (spread <= 0.0001f) return direction;
        const XMVECTOR forward = XMVector3Normalize(XMLoadFloat3(&direction));
        const XMVECTOR up = XMLoadFloat3(&camera.Up);
        XMVECTOR right = XMVector3Cross(up, forward);
        // Looking straight up or down collapses the cross product; fall back to
        // a world axis so the shot still scatters instead of going NaN.
        if (XMVectorGetX(XMVector3LengthSq(right)) < 1e-6f)
            right = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        right = XMVector3Normalize(right);
        const XMVECTOR trueUp = XMVector3Normalize(XMVector3Cross(forward, right));
        const float sideways = (((float)std::rand() / RAND_MAX) * 2.0f - 1.0f) * spread;
        const float vertical = (((float)std::rand() / RAND_MAX) * 2.0f - 1.0f) * spread;
        XMFLOAT3 result;
        XMStoreFloat3(&result, XMVector3Normalize(
            forward + right * sideways + trueUp * vertical));
        return result;
    }

    void ShootProjectile() {
        // Bullets leave inside the cone the reticle is showing, so sights
        // tighten grouping in two ways: the bloom itself collapses under ADS,
        // and the kick halves. Sighted fire is steadier but still climbs -- the
        // recoil has to be fought either way, just less hard down the sights.
        const float recoilScale = 1.0f - 0.50f * adsBlend;
        const float randomYaw = (((float)std::rand() / RAND_MAX) * 2.0f - 1.0f) *
                                recoilYaw * recoilScale;
        camera.ApplyRecoil(recoilPitch * recoilScale, randomYaw);
        // Carbine tap. Scaled by the same recoilScale the aim kick uses, so
        // the shake tracks the climb whatever that scale ends up being.
        camera.AddFireTrauma(0.045f * recoilScale);
        gunRecoilBack = (std::min)(0.12f, gunRecoilBack + 0.075f);
        gunRecoilKick = (std::min)(8.0f, gunRecoilKick + 4.2f * recoilScale);
        TriggerMuzzleFlash(1.0f, 1.0f);
        SpawnWeaponSmoke(GetMuzzleWorldPosition(), camera.Front, 1.0f);

        Projectile p;
        p.position  = GetMuzzleWorldPosition();
        p.previousPosition = p.position;
        p.direction = ApplyShotSpread(camera.Front);
        p.speed     = projectileSpeed;
        p.lifetime  = projectileLifetime;
        p.active    = true;
        projectiles.push_back(p);
    }

    void ShootLaserProjectile() {
        gunRecoilBack = (std::min)(0.055f, gunRecoilBack + 0.012f);
        gunRecoilKick = (std::min)(2.0f, gunRecoilKick + 0.18f);

        Projectile p = {};
        p.position = p.previousPosition = GetMuzzleWorldPosition();
        p.direction = camera.Front;
        p.speed = 1800.0f;
        p.lifetime = 0.075f;
        p.active = true;
        p.laser = true;
        p.damageMultiplier = 6.0f;
        projectiles.push_back(p);

        laserBeam.start = p.position;
        laserBeam.end = {
            p.position.x + p.direction.x * 135.0f,
            p.position.y + p.direction.y * 135.0f,
            p.position.z + p.direction.z * 135.0f };
        laserBeam.life = laserBeam.maxLife;
    }

    void StopLaserBeamAt(const XMFLOAT3& hit) {
        laserBeam.end = hit;
        laserBeam.life = laserBeam.maxLife;
    }

    void ShootHarpoonProjectile() {
        camera.ApplyRecoil(recoilPitch * 1.8f, 0.0f);
        // Harpoon gun: a spring-driven launch rather than a powder charge, so
        // it thumps rather than cracks.
        camera.AddFireTrauma(0.06f);
        gunRecoilBack = (std::min)(0.14f, gunRecoilBack + 0.11f);
        gunRecoilKick = (std::min)(8.0f, gunRecoilKick + 3.2f);
        TriggerMuzzleFlash(0.9f, 0.72f);
        SpawnWeaponSmoke(GetMuzzleWorldPosition(), camera.Front, 0.55f);

        Projectile p = {};
        p.position = p.previousPosition = GetMuzzleWorldPosition();
        p.direction = camera.Front;
        p.speed = 92.0f;
        p.lifetime = 0.85f;
        p.active = true;
        p.harpoon = true;
        p.harpoonId = nextHarpoonId++;
        if (nextHarpoonId == 0) nextHarpoonId = 1;
        p.damageMultiplier = 1.25f;
        projectiles.push_back(p);
    }

    void ShowHarpoonTether(const XMFLOAT3& hit) {
        harpoonTether.start = GetMuzzleWorldPosition();
        harpoonTether.end = hit;
        harpoonTether.life = harpoonTether.maxLife;
    }

    void PinHarpoon(const XMFLOAT3& hit, const XMFLOAT3& direction,
                    uint32_t harpoonId = 0) {
        if (pinnedHarpoons.size() >= 24)
            pinnedHarpoons.erase(pinnedHarpoons.begin());
        pinnedHarpoons.push_back({ hit, direction, harpoonId });
        ShowHarpoonTether(hit);
    }

    void ThrowRemoteCharge() {
        Projectile p = {};
        p.position = p.previousPosition = GetMuzzleWorldPosition();
        p.direction = camera.Front;
        p.velocity = {
            camera.Front.x * 18.0f,
            camera.Front.y * 18.0f + 2.2f,
            camera.Front.z * 18.0f };
        p.lifetime = 4.0f;
        p.active = true;
        p.remoteCharge = true;
        projectiles.push_back(p);
        gunRecoilBack = (std::min)(0.08f, gunRecoilBack + 0.035f);
    }

    void StickRemoteCharge(const XMFLOAT3& position, const XMFLOAT3& normal) {
        if (remoteCharges.size() >= 12) remoteCharges.erase(remoteCharges.begin());
        RemoteCharge charge;
        charge.position = {
            position.x + normal.x * 0.035f,
            position.y + normal.y * 0.035f,
            position.z + normal.z * 0.035f };
        charge.normal = normal;
        remoteCharges.push_back(charge);
    }

    void DetonateRemoteCharges() {
        for (const RemoteCharge& charge : remoteCharges) {
            Projectile blast = {};
            blast.position = blast.previousPosition = charge.position;
            blast.grenade = true;
            blast.remoteCharge = true;
            blast.detonate = true;
            blast.active = false;
            projectiles.push_back(blast);
        }
        remoteCharges.clear();
    }

    void ShootFlameBurst() {
        const XMFLOAT3 muzzle = GetMuzzleWorldPosition();
        const XMVECTOR forward = XMLoadFloat3(&camera.Front);
        const XMVECTOR up = XMLoadFloat3(&camera.Up);
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));
        for (int flameIndex = 0; flameIndex < 2; ++flameIndex) {
            const float jitterX = (((float)std::rand() / RAND_MAX) * 2.0f - 1.0f) * 0.045f;
            const float jitterY = (((float)std::rand() / RAND_MAX) * 2.0f - 1.0f) * 0.035f;
            const XMVECTOR direction = XMVector3Normalize(
                forward + right * jitterX + up * jitterY);
            Projectile p = {};
            p.position = p.previousPosition = muzzle;
            XMStoreFloat3(&p.direction, direction);
            p.speed = 24.0f + ((float)std::rand() / RAND_MAX) * 5.0f;
            p.lifetime = 0.55f;
            p.active = true;
            p.flame = true;
            p.damageMultiplier = 0.18f;
            projectiles.push_back(p);
        }
        if (flamethrowerAudioTime <= 0.0f && fireIgnitionAudioCallback)
            fireIgnitionAudioCallback(muzzle);
        flamethrowerAudioTime = 0.14f;
        gunRecoilBack = (std::min)(0.045f, gunRecoilBack + 0.006f);
    }

    void ThrowGrenade() {
        Projectile p;
        p.position  = camera.Position;
        p.previousPosition = p.position;
        p.direction = camera.Front;
        p.grenade   = true;
        p.molotov   = selectedGrenade == GrenadeType::Molotov;
        p.vortex    = selectedGrenade == GrenadeType::Vortex;
        p.active    = true;
        p.fuse      = p.molotov ? 4.0f : grenadeFuse;
        p.grenadeCollisionGrace = 0.18f;
        // Launch along the aim direction plus a slight upward lob.
        p.velocity  = { camera.Front.x * grenadeThrowSpeed,
                        camera.Front.y * grenadeThrowSpeed + grenadeLob,
                        camera.Front.z * grenadeThrowSpeed };
        projectiles.push_back(p);
    }

    void SpawnVortexFX(const XMFLOAT3& center) {
        if (vortexFX.size() >= 4) vortexFX.erase(vortexFX.begin());
        vortexFX.push_back({ center, vortexRadius, 0.0f, vortexDuration, 0.0f });
        camera.ApplyExplosionImpulse(center, 2.0f);
    }

    void SpawnMolotovFire(const XMFLOAT3& impact) {
        const float ground = grenadeGroundHeight
            ? grenadeGroundHeight(impact.x, impact.z) : impact.y;
        if (ground < -0.35f) return;
        FirePatch seed;
        seed.position = { impact.x, ground + 0.05f, impact.z };
        seed.maxLife = seed.life = molotovFireDuration;
        seed.radius = 0.32f;
        seed.maxRadius = 1.05f;
        seed.spreadDelay = 0.30f;
        if (firePatches.size() >= 64)
            firePatches.erase(firePatches.begin());
        firePatches.push_back(seed);
        if (fireIgnitionAudioCallback)
            fireIgnitionAudioCallback(seed.position);

        camera.ApplyExplosionImpulse(seed.position, 1.35f);
        for (int i = 0; i < 18; ++i) {
            ImpactParticle ember;
            const float angle = XM_2PI * (float)i / 18.0f;
            const float speed = 1.2f + ((float)std::rand() / RAND_MAX) * 2.2f;
            ember.position = { impact.x, ground + 0.18f, impact.z };
            ember.velocity = { std::cos(angle) * speed,
                2.0f + ((float)std::rand() / RAND_MAX) * 2.8f,
                std::sin(angle) * speed };
            ember.maxLife = ember.life = 0.30f +
                ((float)std::rand() / RAND_MAX) * 0.45f;
            ember.size = 0.04f + ((float)std::rand() / RAND_MAX) * 0.07f;
            ember.growth = -0.06f;
            ember.color = { 1.0f, 0.22f, 0.008f };
            ember.spark = true;
            impactParticles.push_back(ember);
        }
        SpawnSmokeBurst(seed.position, 0.28f, 0.32f);
    }

    bool SpawnCarriedFire(const XMFLOAT3& position) {
        if (firePatches.size() >= 64) return false;
        const float ground = grenadeGroundHeight
            ? grenadeGroundHeight(position.x, position.z) : position.y;
        if (ground < -0.35f) return false;
        for (const FirePatch& existing : firePatches) {
            const float dx = existing.position.x - position.x;
            const float dz = existing.position.z - position.z;
            if (dx * dx + dz * dz < 0.85f * 0.85f) return false;
        }
        FirePatch carried;
        carried.position = { position.x, ground + 0.05f, position.z };
        carried.maxLife = carried.life = molotovFireDuration * 0.72f;
        carried.radius = 0.20f;
        carried.maxRadius = 0.68f;
        carried.spreadDelay = 0.55f;
        carried.generation = 1;
        firePatches.push_back(carried);
        return true;
    }

    void IgniteMaterial(uint64_t entityId, const XMFLOAT3& position,
                        float size = 1.4f) {
        if (entityId == 0) return;
        for (BurningMaterial& material : burningMaterials) {
            if (material.entityId != entityId) continue;
            material.position = position;
            material.life = material.maxLife = 8.0f;
            material.size = (std::max)(material.size, size);
            return;
        }
        if (burningMaterials.size() >= 32)
            burningMaterials.erase(burningMaterials.begin());
        BurningMaterial material;
        material.entityId = entityId;
        material.position = position;
        material.life = material.maxLife = 8.0f;
        material.size = size;
        burningMaterials.push_back(material);
    }

    // suppressed shrinks the flash and softens the recoil rather than changing
    // the round: a can traps most of the muzzle blast, which is what produces
    // both the flash and part of the kick. Damage and velocity are untouched --
    // the suppressed rifle hits exactly as hard as the bare one.
    void ShootSniperProjectile(bool suppressed = false) {
        // No spread, ever. The SVD puts its round exactly where the crosshair
        // sits regardless of stance or movement -- a marksman rifle whose shot
        // wanders is not one, and the 5x damage multiplier only means something
        // if the shot is trusted. The reticle may bloom while moving; this
        // weapon deliberately does not honour it.
        const XMFLOAT3 aimDirection = camera.Front;
        camera.ApplyRecoil(recoilPitch * (suppressed ? 3.6f : 4.2f), 0.0f);
        // Marksman rifle. A shade less than the shotgun despite the bigger aim
        // kick: this weapon is fired from a scope, where a shaking view is far
        // more disruptive than it is at the hip.
        camera.AddFireTrauma(suppressed ? 0.085f : 0.10f);
        gunRecoilBack = (std::min)(0.16f, gunRecoilBack + 0.12f);
        gunRecoilKick = (std::min)(12.0f, gunRecoilKick + 7.0f);
        // A suppressed muzzle still flares, just far less. Killing it outright
        // would make the rifle read as a laser rather than a firearm, and at
        // night the faint flash is a fair tell for an enemy who is looking.
        if (suppressed) TriggerMuzzleFlash(0.55f, 0.35f);
        else TriggerMuzzleFlash(1.35f, 1.35f);
        SpawnWeaponSmoke(GetMuzzleWorldPosition(), camera.Front,
                         suppressed ? 0.7f : 1.5f);

        Projectile p = {};
        p.position = p.previousPosition = GetMuzzleWorldPosition();
        p.direction = aimDirection;
        p.speed = 650.0f;
        p.lifetime = 4.0f;
        p.active = true;
        p.damageMultiplier = 5.0f;
        projectiles.push_back(p);
    }

    void ShootRocket() {
        camera.ApplyRecoil(recoilPitch * 4.0f, 0.0f);
        // Launcher backblast: the hardest shove of any weapon here, and the one
        // shot where a real jolt is expected. Still under the firing ceiling --
        // the rocket's own detonation supplies the big shake a moment later.
        camera.AddFireTrauma(0.19f);
        gunRecoilBack = (std::min)(0.20f, gunRecoilBack + 0.16f);
        gunRecoilKick = (std::min)(14.0f, gunRecoilKick + 9.0f);
        TriggerMuzzleFlash(1.8f, 1.75f);
        SpawnWeaponSmoke(GetMuzzleWorldPosition(), camera.Front, 2.4f);

        Projectile p = {};
        p.position = p.previousPosition = GetMuzzleWorldPosition();
        p.direction = camera.Front;
        p.speed = 42.0f;
        p.lifetime = 6.0f;
        p.active = true;
        p.rocket = true;
        projectiles.push_back(p);
    }

    // Build matrices
    void UpdateSniperScope(bool active, float dt) {
        // The scope overlay hides the view model outright, which would defeat
        // the point of ejecting to look at it.
        if (ejected) active = false;
        sniperScopeActive = active;
        const float target = active ? 1.0f : 0.0f;
        // Matches the iron-sight raise rate so swapping to the SVD does not
        // change how fast the weapon comes up.
        const float response = (std::min)(1.0f, (std::max)(0.0f, dt) * 24.0f);
        sniperScopeBlend += (target - sniperScopeBlend) * response;
        if (!active && sniperScopeBlend < 0.001f) sniperScopeBlend = 0.0f;
    }
    // Iron sights. The two blends are mutually exclusive by construction: only
    // the SVD ever requests the scope, so a weapon is either scoped or sighted.
    void UpdateAimDownSights(bool active, float dt) {
        // Aiming while ejected would slide the parked weapon onto a view axis
        // the player is no longer looking down, so the inspection camera always
        // sees the neutral hip pose.
        if (ejected) active = false;
        adsActive = active;
        const float target = active ? 1.0f : 0.0f;
        // Snappy on purpose: ADS is used mid-fight, and a slow raise makes the
        // weapon feel heavy to bring up.
        const float response = (std::min)(1.0f, (std::max)(0.0f, dt) * 20.0f);
        adsBlend += (target - adsBlend) * response;
        if (!active && adsBlend < 0.001f) adsBlend = 0.0f;
    }
    float EffectiveCameraFOV() const {
        // Scope wins when both are somehow non-zero, since it is the narrower
        // of the two and blending them would land between the sights.
        const float sighted = cameraFOV + (adsFOV - cameraFOV) * adsBlend;
        return sighted + (sniperScopeFOV - sighted) * sniperScopeBlend +
               camera.ExplosionFovKick();
    }
    float ScopeLookScale() const {
        // Sights slow the turn rate proportionally to the zoom so the sensitivity
        // at the sight picture feels the same as at the hip.
        return (1.0f - 0.72f * sniperScopeBlend) * (1.0f - 0.30f * adsBlend);
    }
    float EffectiveCameraFarPlane() const {
        return (std::max)(cameraFar, cameraFarOverride);
    }
    XMMATRIX GetViewMatrix()       const { return const_cast<Camera&>(camera).GetViewMatrix(); }
    XMMATRIX GetProjectionMatrix() const {
        XMMATRIX projection = GetUnjitteredProjectionMatrix();
        if (g_dx12.screenWidth > 0 && g_dx12.screenHeight > 0) {
            const float jitterX = 2.0f * temporalJitterPixels.x /
                                  static_cast<float>(g_dx12.screenWidth);
            const float jitterY = -2.0f * temporalJitterPixels.y /
                                  static_cast<float>(g_dx12.screenHeight);
            projection.r[2] = XMVectorAdd(
                projection.r[2], XMVectorSet(jitterX, jitterY, 0.0f, 0.0f));
        }
        return projection;
    }

    // Projection without the TAA sub-pixel offset.
    //
    // Jitter is only safe on geometry TAA can reproject: the resolve pass
    // writes motion vectors for visibility-buffer surfaces, so their history
    // lands on the right texel and the offset averages out into antialiasing.
    // Geometry drawn in the forward extension pass -- the weapon viewmodel,
    // skinned actors, foliage -- emits no motion vectors, so the jitter has
    // nothing to cancel it and reads as the whole view shaking.
    //
    // Those passes use this instead. Remove the distinction only once the
    // extensions emit real per-object motion.
    XMMATRIX GetUnjitteredProjectionMatrix() const {
        return XMMatrixPerspectiveFovLH(
            XMConvertToRadians(EffectiveCameraFOV()),
            (float)g_dx12.screenWidth / (float)g_dx12.screenHeight,
            cameraNear, EffectiveCameraFarPlane());
    }

    // -- Ejected free camera (Unreal's F8) --------------------------------------
    //
    // Detaches the rendering camera from the player so the view model can be
    // inspected from outside. The player's viewpoint at the moment of ejecting is
    // frozen in ejectAnchor*, and the weapon and arms keep building their
    // transform from THAT, so they stay behind with the body while `camera` flies
    // away. Pressing the key again drops the camera back where it was.
    bool     ejected = false;
    XMFLOAT3 ejectAnchorPosition = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 ejectAnchorFront    = { 0.0f, 0.0f, 1.0f };
    // The player camera is restored from these on re-attach.
    XMFLOAT3 ejectReturnPosition = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 ejectReturnFront    = { 0.0f, 0.0f, 1.0f };
    float    ejectReturnYaw = 0.0f;
    float    ejectReturnPitch = 0.0f;
    bool     ejectReturnFPSMode = true;

    // Where the view model anchors: the live camera normally, the frozen player
    // viewpoint while ejected.
    const XMFLOAT3& ViewmodelAnchorPosition() const {
        return ejected ? ejectAnchorPosition : camera.Position;
    }
    const XMFLOAT3& ViewmodelAnchorFront() const {
        return ejected ? ejectAnchorFront : camera.Front;
    }

    void ToggleEjectedCamera() {
        if (!ejected) {
            // Freeze the viewpoint the weapon hangs off, and remember where to
            // put the player back.
            ejectAnchorPosition = ejectReturnPosition = camera.Position;
            ejectAnchorFront = ejectReturnFront = camera.Front;
            ejectReturnYaw      = camera.Yaw;
            ejectReturnPitch    = camera.Pitch;
            // Free flight: gravity and the XZ-plane walk constraint would fight
            // an inspection camera, so leave FPS mode for the duration.
            ejectReturnFPSMode  = camera.FPSMode;
            camera.FPSMode      = false;
            ejected = true;
        } else {
            camera.Position = ejectReturnPosition;
            camera.Yaw      = ejectReturnYaw;
            camera.Pitch    = ejectReturnPitch;
            // Front is normally recomputed from yaw/pitch inside the camera, but
            // that helper is private; restoring the saved vector is equivalent
            // and keeps the camera header untouched.
            camera.Front    = ejectReturnFront;
            camera.FPSMode  = ejectReturnFPSMode;
            ejected = false;
        }
    }

    // Gun world-space model matrix
    XMMATRIX GetGunModelMatrix() const {
        XMVECTOR camPos   = XMLoadFloat3(&camera.Position);
        XMVECTOR camFront = XMLoadFloat3(&camera.Front);
        XMVECTOR camRight = XMVector3Cross(XMLoadFloat3(&camera.Up), camFront);
        XMVECTOR camUp    = XMLoadFloat3(&camera.Up);
        XMVECTOR gp = camPos + camFront * gun.offset.z + camRight * gun.offset.x + camUp * gun.offset.y;
        XMFLOAT3 gunPos; XMStoreFloat3(&gunPos, gp);
        XMMATRIX m = XMMatrixScaling(gun.scale.x, gun.scale.y * 2.0f, gun.scale.z * 3.0f);
        m = m * XMMatrixRotationY(XMConvertToRadians(gun.rotation.y) + camera.Yaw * 0.0174533f);
        m = m * XMMatrixTranslation(gunPos.x, gunPos.y, gunPos.z);
        return m;
    }

    // Base view-model transform: places the weapon in front of the camera and
    // orients its local space so +X = right, +Y = up, +Z = forward (down the
    // barrel). The gun's geometry is laid out in this local space by the
    // renderer. No non-uniform stretch here (unlike the legacy cube matrix).
    //
    // The basis must be built from the camera's TRUE up, not Camera::Up -- that
    // member is a fixed world-up (0,1,0) and never tilts with pitch. Using it
    // directly gave a non-orthogonal frame, so looking up or down sheared the
    // weapon instead of pitching it with the view. Re-derive right and up from
    // Front (Gram-Schmidt) so the gun rigidly follows the camera in yaw AND pitch.
    XMMATRIX GetGunBaseMatrix() const {
        // While ejected the weapon must stay parked at the body the player left
        // behind, not ride the free camera -- otherwise flying out to inspect the
        // view model just drags it along and you never see it from outside.
        const XMVECTOR camPos   = XMLoadFloat3(&ViewmodelAnchorPosition());
        const XMVECTOR camFront = XMVector3Normalize(XMLoadFloat3(&ViewmodelAnchorFront()));
        const XMVECTOR worldUp  = XMLoadFloat3(&camera.Up);

        // Left-handed frame (the view matrix is LookAtLH): right = up x front.
        XMVECTOR camRight = XMVector3Cross(worldUp, camFront);
        // Looking straight up or down makes that cross product vanish; fall back
        // to a stable axis so the gun does not flip or disappear at the poles.
        if (XMVectorGetX(XMVector3LengthSq(camRight)) < 1e-6f)
            camRight = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        camRight = XMVector3Normalize(camRight);

        // True camera up, perpendicular to both: this is what tilts with pitch.
        const XMVECTOR camUp = XMVector3Normalize(XMVector3Cross(camFront, camRight));

        // Aiming slides the weapon from its hip offset onto the view axis, so
        // the sights line up with the crosshair. X goes to zero (centred), Y
        // rises to eye level, Z pushes slightly forward to bring the rear sight
        // closer to the camera.
        const float hipToSights = (std::min)(1.0f, (std::max)(0.0f, adsBlend));
        const float offsetX =
            gun.offset.x + (adsOffsetX - gun.offset.x) * hipToSights;
        const float offsetY = gun.offset.y + (adsOffsetY - gun.offset.y) * hipToSights;
        const float offsetZ = gun.offset.z + (adsOffsetZ - gun.offset.z) * hipToSights;

        const XMVECTOR gp = camPos + camFront * (offsetZ - gunRecoilBack)
                                   + camRight * offsetX
                                   + camUp    * (offsetY + gunRecoilBack * 0.18f);

        // Orthonormal basis: rows right/up/front, translation at the gun spot.
        XMMATRIX basis = XMMatrixIdentity();
        basis.r[0] = XMVectorSetW(camRight, 0.0f);
        basis.r[1] = XMVectorSetW(camUp, 0.0f);
        basis.r[2] = XMVectorSetW(camFront, 0.0f);
        basis.r[3] = XMVectorSetW(gp, 1.0f);
        // Sway rotates in the camera's own frame, so it composes with recoil
        // before the basis takes the whole thing into world space. Yaw drags the
        // muzzle across the screen, pitch trails vertical aim; a touch of roll
        // on the yaw sells the weight without needing its own state.
        const XMMATRIX sway =
            XMMatrixRotationZ(XMConvertToRadians(gunSwayYaw * -0.35f)) *
            XMMatrixRotationX(XMConvertToRadians(gunSwayPitch)) *
            XMMatrixRotationY(XMConvertToRadians(gunSwayYaw));
        // Jump pitch tips the mesh only. Aiming pins the weapon to the shoulder,
        // so it is mostly damped out down the sights for the same reason sway
        // is. Negative X rotates the muzzle up, matching the recoil convention
        // directly above, so a positive spring value reads as barrel-up.
        const float jumpVisible = gunJumpPitch * (1.0f - 0.75f * hipToSights);
        return XMMatrixRotationX(
                   XMConvertToRadians(-gunRecoilKick - jumpVisible)) *
               sway * basis;
    }

    void SpawnHostileProjectile(const XMFLOAT3& origin, const XMFLOAT3& direction,
                                float damageMultiplier = 1.0f,
                                float speedMultiplier = 1.0f) {
        Projectile p = {};
        p.position = p.previousPosition = origin;
        p.direction = direction;
        p.speed = projectileSpeed * speedMultiplier;
        p.lifetime = projectileLifetime;
        p.active = true;
        p.hostile = true;
        p.damageMultiplier = damageMultiplier;
        projectiles.push_back(p);
    }

    void SpawnPlayerProjectile(const XMFLOAT3& origin,
                               const XMFLOAT3& direction,
                               float speedMultiplier = 1.0f) {
        Projectile p = {};
        p.position = p.previousPosition = origin;
        p.direction = direction;
        p.speed = projectileSpeed * speedMultiplier;
        p.lifetime = projectileLifetime;
        p.active = true;
        projectiles.push_back(p);
    }

    void ShootShotgun() {
        const float randomYaw = (((float)std::rand() / RAND_MAX) * 2.0f - 1.0f) *
                                recoilYaw * 2.2f;
        camera.ApplyRecoil(recoilPitch * 2.8f, randomYaw);
        // Shotgun: heaviest per-shot kick in the rack, and slow enough between
        // shots that the shake fully decays rather than stacking.
        camera.AddFireTrauma(0.13f);
        gunRecoilBack = (std::min)(0.16f, gunRecoilBack + 0.13f);
        gunRecoilKick = (std::min)(11.0f, gunRecoilKick + 7.5f);
        TriggerMuzzleFlash(1.35f, 1.45f);
        SpawnWeaponSmoke(GetMuzzleWorldPosition(), camera.Front, 1.8f);

        const XMFLOAT3 muzzle = GetMuzzleWorldPosition();
        const XMVECTOR cameraFront = XMLoadFloat3(&camera.Front);
        const XMVECTOR cameraUp = XMLoadFloat3(&camera.Up);
        const XMVECTOR cameraRight = XMVector3Normalize(
            XMVector3Cross(cameraUp, cameraFront));
        constexpr int pelletCount = 8;
        // The buckshot pattern is a property of the barrel, so it stays fixed --
        // the reticle bloom widens it rather than replacing it. Half weight,
        // because a weapon whose whole identity is a wide pattern gains less
        // from an unsteady stance than a rifle loses.
        const float spread = 0.055f + CurrentShotSpreadRadians() * 0.5f;
        for (int pellet = 0; pellet < pelletCount; ++pellet) {
            const float right = (((float)std::rand() / RAND_MAX) * 2.0f - 1.0f) * spread;
            const float up = (((float)std::rand() / RAND_MAX) * 2.0f - 1.0f) * spread;
            XMVECTOR direction =
                cameraFront + cameraRight * right + cameraUp * up;

            Projectile p;
            p.position = p.previousPosition = muzzle;
            XMStoreFloat3(&p.direction, XMVector3Normalize(direction));
            p.speed = projectileSpeed;
            p.lifetime = projectileLifetime;
            p.active = true;
            projectiles.push_back(p);
        }
    }

    XMFLOAT3 GetMuzzleWorldPosition() const {
        const float S = GunModelScale();
        const XMVECTOR local = XMVectorSet(0.0f, 0.01f * S, 0.83f * S, 1.0f);
        XMFLOAT3 result;
        XMStoreFloat3(&result, XMVector3TransformCoord(local, GetGunBaseMatrix()));
        return result;
    }

    // Overall size of the M4 view model (local units before the base transform).
    float GunModelScale() const { return gun.scale.z * 3.0f; }
};

#endif // SCENE_H
