#ifndef SCENE_H
#define SCENE_H

#include "DX12Core.h"
#include "CameraDX12.h"
#include "ClusteredRendererDX12.h"
#include <vector>
#include <algorithm>
#include <cstdlib>
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
    // Grenade: arcs under gravity and detonates (radial blast) on fuse timeout
    // or first impact. Regular bullets leave these at defaults.
    bool     grenade = false;
    bool     rocket = false;
    float    damageMultiplier = 1.0f;
    XMFLOAT3 velocity = { 0, 0, 0 };   // grenades integrate velocity + gravity
    float    fuse = 0.0f;              // seconds until it explodes
    bool     detonate = false;         // set the frame it should explode
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

// One-shot flipbook explosion billboard (8x8 CC0 sheet, see
// models/textures/EXPLOSION_BOOM3_LICENSE.txt). Age drives frame selection.
struct ExplosionFX {
    XMFLOAT3 position;
    float    size;      // world diameter at full bloom
    float    age = 0.0f;
    float    duration = 0.9f;
};

struct ExplosiveBarrel {
    XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 velocity = { 0.0f, 0.0f, 0.0f };
    int hits = 0;
    bool active = true;
    bool held = false;
    bool thrown = false;
    bool burning = false;
    float fuse = 0.0f;
    float fireFxCooldown = 0.0f;
};

struct GunViewModel {
    bool     visible  = true;   // AK47 view model is on by default
    XMFLOAT3 color    = { 0.3f, 0.3f, 0.35f };
    XMFLOAT3 offset   = { 0.28f, -0.24f, 0.40f };
    XMFLOAT3 scale    = { 0.15f, 0.15f, 0.30f };
    XMFLOAT3 rotation = { 0.0f, 180.0f, 0.0f };
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
    // Sighted viewmodel offset, against a hip offset of {0.28, -0.24, 0.40}.
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
    // Far enough to see the sea run out to the horizon; the ocean plane alone is
    // 600 m across, and a 100 m far plane sliced it off in plain view.
    float  cameraFar   = 800.0f;
    // Sub-pixel projection offset used by visibility-buffer TAA. Zero for
    // forward, raytracing, menus, and validation captures.
    XMFLOAT2 temporalJitterPixels = { 0.0f, 0.0f };

    // Main directional / point light
    // Angled, HDR-strength warm sun. Lower elevation gives terrain and props
    // longer modelling shadows while cool sky irradiance keeps them readable.
    XMFLOAT3 lightPos    = { 8.246f, 3.095f, 4.735f };
    XMFLOAT3 lightColor  = { 1.0f, 0.92f, 0.70f };
    float    directionalLightIntensity = 12.18f;
    int      lightType   = 0;
    float    lightConstant  = 1.0f;
    float    lightLinear    = 0.09f;
    float    lightQuadratic = 0.032f;

    // Material defaults
    float ambientStrength   = 0.07f;
    float ambientLightingIntensity = 0.356f;
    float specularStrength  = 0.5f;
    int   specularShininess = 32;
    float shadowBias        = 0.005f;
    bool  enableShadows     = true;
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
    std::vector<ImpactParticle> impactParticles;  // impact smoke puffs
    std::vector<ExplosionFX> explosionFX;         // animated explosion flipbooks
    std::function<void(const XMFLOAT3&, float, bool)> explosionAudioCallback;
    std::vector<ExplosiveBarrel> explosiveBarrels;
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
    float gunRecoilBack      = 0.0f;    // viewmodel translation, local metres
    float gunRecoilKick      = 0.0f;    // viewmodel pitch, degrees
    float recoilPitch        = 0.55f;   // camera climb per shot, degrees
    float recoilYaw          = 0.22f;   // random horizontal camera kick
    float playerMaxHealth    = 100.0f;
    float playerHealth       = 100.0f;
    float playerDamageFlash  = 0.0f;
    bool  playerGodMode      = false;
    // Regenerating health. Any damage restarts the delay, so staying in a
    // firefight never heals you -- breaking contact is what does. The rate is
    // deliberately slower than sustained rifle fire, so regen rewards
    // disengaging without trivialising a fight the player chooses to stand in.
    bool  playerHealthRegen  = true;
    float playerRegenDelay   = 5.0f;   // seconds after last hit before healing
    float playerRegenRate    = 12.0f;  // health per second once it starts
    float playerRegenTimer   = 0.0f;   // counts down to the delay above

    // Ammo, per weapon slot (0 = AK47, 1 = shotgun, 2 = RPG-7, 3 = SVD) to match
    // GunModel::SelectedWeapon(). Only enforced when playerGodMode is false --
    // god mode keeps the old unlimited-fire behaviour so sandbox/debug levels
    // are unchanged. magazine = rounds in the gun, reserve = spare rounds.
    static constexpr int kWeaponSlots = 4;
    int magazineSize[kWeaponSlots] = { 30,  8,  1, 10 };
    int maxReserve  [kWeaponSlots] = { 240, 64, 8, 80 };
    // Tuned around the reload clip (Content/Audio/... , 1.512s) so the
    // sound lands close to when the magazine actually seats. The RPG and
    // shotgun run longer deliberately -- they read as heavier reloads, and the
    // audio simply finishes a little early there.
    float reloadTime[kWeaponSlots] = { 1.55f, 2.4f, 2.8f, 1.75f };
    int   magazine  [kWeaponSlots] = { 30,  8,  1, 10 };
    int   reserve   [kWeaponSlots] = { 120, 32, 4, 40 };
    float reloadTimer = 0.0f;   // >0 while reloading; blocks firing
    int   reloadingSlot = -1;   // slot being reloaded, -1 when idle

    bool AmmoEnforced() const { return !playerGodMode; }
    bool Reloading() const { return reloadTimer > 0.0f; }

    // Refill everything to a full magazine + full reserve. Used on level start
    // so a fresh run never begins dry.
    void RestoreAmmo() {
        for (int i = 0; i < kWeaponSlots; ++i) {
            magazine[i] = magazineSize[i];
            reserve[i] = maxReserve[i];
        }
        reloadTimer = 0.0f;
        reloadingSlot = -1;
    }

    // Begin a reload of `slot`. No-op when god mode is on, already reloading,
    // the magazine is full, or there is nothing in reserve to load.
    bool BeginReload(int slot) {
        if (!AmmoEnforced() || Reloading()) return false;
        if (slot < 0 || slot >= kWeaponSlots) return false;
        if (magazine[slot] >= magazineSize[slot] || reserve[slot] <= 0)
            return false;
        reloadingSlot = slot;
        reloadTimer = reloadTime[slot];
        return true;
    }

    // Drive the reload timer. Ammo moves from reserve to magazine only on
    // completion, so interrupting a reload (weapon swap) loses no rounds.
    void UpdateReload(float dt) {
        if (!Reloading()) return;
        reloadTimer -= dt;
        if (reloadTimer > 0.0f) return;
        reloadTimer = 0.0f;
        const int slot = reloadingSlot;
        reloadingSlot = -1;
        if (slot < 0 || slot >= kWeaponSlots) return;
        const int needed = magazineSize[slot] - magazine[slot];
        const int moved = (needed < reserve[slot]) ? needed : reserve[slot];
        magazine[slot] += moved;
        reserve[slot] -= moved;
    }

    // Consume one round from `slot`. Returns false (and fires nothing) when the
    // magazine is empty, so the caller can skip the shot and its cooldown.
    bool ConsumeAmmo(int slot) {
        if (!AmmoEnforced()) return true;
        if (slot < 0 || slot >= kWeaponSlots) return true;
        if (Reloading() || magazine[slot] <= 0) return false;
        --magazine[slot];
        return true;
    }

    // Grenade (press G): lobbed, arcs under gravity, radial blast on fuse.
    float grenadeThrowSpeed    = 16.0f;  // launch speed along aim
    float grenadeLob           = 3.0f;   // extra upward velocity for the arc
    float grenadeGravityScale  = 1.0f;
    float grenadeGroundY       = 0.15f;  // bounce height
    float grenadeFuse          = 2.0f;   // timer-only detonation
    float grenadeBlastRadius   = 3.5f;   // original debris radius
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
    float grenadeCooldown      = 0.0f;   // input debounce
    // Returns rendered ground height at world XZ. Installed by main so Scene
    // does not depend on terrain renderer implementation.
    std::function<float(float, float)> grenadeGroundHeight;

    // NVIDIA Blast + Box3D destructible house
    bool  useDestruction = true;
    bool  showHelicopter = true;   // draw + simulate the hovering attack heli
    bool  enableMSAA = true;
    bool  enableGrassMSAA = true;
    bool  enableFXAA = false;
    bool  enableVolumetricFog = true;
    float volumetricFogDensity = 0.0040f;      // lighter haze -> distant palms stay readable
    float volumetricFogAnisotropy = 0.42f;     // stronger forward scatter -> backlit haze
    float volumetricFogHeightFalloff = 0.045f; // fog thins above the undergrowth, not over treetops
    float volumetricFogBaseHeight = 0.4f;      // haze pools low in the valley floor
    float volumetricFogDistance = 240.0f;      // pull back so far trees don't wash out
    XMFLOAT3 volumetricFogTint = { 0.60f, 0.72f, 0.66f }; // warm-green humid haze
    bool  enableAmbientOcclusion = true;
    float ambientOcclusionRadius = 0.12f;
    float ambientOcclusionStrength = 1.96f;
    float ambientOcclusionBias = 0.035f;
    float contactShadowStrength = 0.0f;
    int   destructionGridX = 4;
    int   destructionGridY = 3;
    int   destructionGridZ = 4;
    float destructionDamageRadius = 0.9f;   // tight blast so hits stay local
    float destructionDamage = 0.5f;         // bullets weaken once, then break on second hit
    float destructionBulletImpulse = 260.0f;
    bool  rebuildDestructionRequested = false;
    bool  showDestructionDebug = false;

    // Palm trees. Damage here is absolute (against a section's health), not the
    // 0..1 bond fraction destructionDamage uses -- different system, different
    // units. ~3 hits to sever one trunk section.
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

    // Mesh-shader tessellated terrain (replaces the flat floor plane when on)
    bool  useMeshTerrain     = true;
    float terrainHeightScale = 5.0f;
    // Island builder extent, driven by the level definition. tilesX*tileSize is
    // the drawn ground width; island scale grows the coastline so ocean rings it.
    uint32_t terrainTilesX   = 16;
    uint32_t terrainTilesZ   = 16;
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
        impactParticles.clear();
        explosionFX.clear();
        explosiveBarrels = {
            {{ 4.6f, 3.25f,  4.6f}},
            {{-4.6f, 3.25f,  4.6f}},
            {{ 4.6f, 3.25f, -4.6f}},
            {{-4.6f, 3.25f, -4.6f}},
        };
        fireCooldown = 0.0f;
        muzzleFlashTime = 0.0f;
        gunRecoilBack = 0.0f;
        gunRecoilKick = 0.0f;
        grenadeCooldown = 0.0f;
        playerDamageFlash = 0.0f;
        playerRegenTimer = 0.0f;
        adsBlend = 0.0f;
        adsActive = false;
    }

    void DamagePlayer(float damage) {
        if (playerGodMode || damage <= 0.0f || playerHealth <= 0.0f) return;
        playerHealth = (std::max)(0.0f, playerHealth - damage);
        playerDamageFlash = 0.22f;
        // Restart the hold-off on every hit, including the one that kills, so a
        // revive does not inherit a nearly expired timer.
        playerRegenTimer = playerRegenDelay;
    }

    void RestorePlayerHealth() {
        playerHealth = playerMaxHealth;
        playerDamageFlash = 0.0f;
        playerRegenTimer = 0.0f;
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
        DamagePlayer(2.4f * p.damageMultiplier);
        p.active = false;
        return true;
    }

    void Update(float dt, float currentTime) {
        camera.Update(dt);

        muzzleFlashTime = (std::max)(0.0f, muzzleFlashTime - dt);
        playerDamageFlash = (std::max)(0.0f, playerDamageFlash - dt);

        // Health regen. Death is final: at zero health the timer stops rather
        // than quietly healing a corpse back to fighting strength.
        if (playerHealthRegen && playerHealth > 0.0f &&
            playerHealth < playerMaxHealth) {
            playerRegenTimer = (std::max)(0.0f, playerRegenTimer - dt);
            if (playerRegenTimer <= 0.0f) {
                playerHealth = (std::min)(playerMaxHealth,
                                          playerHealth + playerRegenRate * dt);
            }
        }
        // Sharp impulse, quick mechanical return. Camera aim stays displaced,
        // so automatic fire climbs unless the player actively compensates.
        gunRecoilBack = (std::max)(0.0f, gunRecoilBack - 1.45f * dt);
        gunRecoilKick = (std::max)(0.0f, gunRecoilKick - 95.0f * dt);

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
            if (p.grenade) {
                // Ballistic arc with a damped ground bounce. Only the fuse can
                // detonate it; impacts never shorten the two-second timer.
                p.velocity.y += -9.81f * grenadeGravityScale * dt;
                p.position.x += p.velocity.x * dt;
                p.position.y += p.velocity.y * dt;
                p.position.z += p.velocity.z * dt;
                const float surfaceY = grenadeGroundHeight
                    ? grenadeGroundHeight(p.position.x, p.position.z) : 0.0f;
                const float bounceY = surfaceY + grenadeGroundY;
                if (p.position.y < bounceY) {
                    p.position.y = bounceY;
                    if (p.velocity.y < 0.0f)
                        p.velocity.y = -p.velocity.y * 0.4f;
                    p.velocity.x *= 0.7f;
                    p.velocity.z *= 0.7f;
                }
                p.fuse -= dt;
                if (p.fuse <= 0.0f) { p.detonate = true; p.active = false; }
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
    }

    // Kick off one animated explosion flipbook centred on `center`.
    // `size` is the billboard's full-bloom world diameter.
    void SpawnExplosionFX(const XMFLOAT3& center, float size,
                          float duration = 0.9f, bool grenade = false) {
        ExplosionFX fx;
        fx.position = center;
        fx.size = size;
        fx.duration = duration;
        explosionFX.push_back(fx);
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

    void ShootProjectile() {
        // Bullets already travel exactly along the view axis, so sights cannot
        // tighten grouping. What they buy is recoil control: sighted fire is
        // almost perfectly flat, so holding the sights on a target is the way
        // to land sustained fire, and hipfire is the spray option.
        const float recoilScale = 1.0f - 0.90f * adsBlend;
        const float randomYaw = (((float)std::rand() / RAND_MAX) * 2.0f - 1.0f) *
                                recoilYaw * recoilScale;
        camera.ApplyRecoil(recoilPitch * recoilScale, randomYaw);
        gunRecoilBack = (std::min)(0.12f, gunRecoilBack + 0.075f);
        gunRecoilKick = (std::min)(8.0f, gunRecoilKick + 4.2f * recoilScale);
        muzzleFlashTime = muzzleFlashDuration;

        Projectile p;
        p.position  = GetMuzzleWorldPosition();
        p.previousPosition = p.position;
        p.direction = camera.Front;
        p.speed     = projectileSpeed;
        p.lifetime  = projectileLifetime;
        p.active    = true;
        projectiles.push_back(p);
    }

    void ThrowGrenade() {
        Projectile p;
        p.position  = camera.Position;
        p.previousPosition = p.position;
        p.direction = camera.Front;
        p.grenade   = true;
        p.active    = true;
        p.fuse      = grenadeFuse;
        // Launch along the aim direction plus a slight upward lob.
        p.velocity  = { camera.Front.x * grenadeThrowSpeed,
                        camera.Front.y * grenadeThrowSpeed + grenadeLob,
                        camera.Front.z * grenadeThrowSpeed };
        projectiles.push_back(p);
    }

    void ShootSniperProjectile() {
        const XMFLOAT3 aimDirection = camera.Front;
        camera.ApplyRecoil(recoilPitch * 4.2f, 0.0f);
        gunRecoilBack = (std::min)(0.16f, gunRecoilBack + 0.12f);
        gunRecoilKick = (std::min)(12.0f, gunRecoilKick + 7.0f);
        muzzleFlashTime = muzzleFlashDuration * 1.35f;

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
        gunRecoilBack = (std::min)(0.20f, gunRecoilBack + 0.16f);
        gunRecoilKick = (std::min)(14.0f, gunRecoilKick + 9.0f);
        muzzleFlashTime = muzzleFlashDuration * 1.8f;

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
        return sighted + (sniperScopeFOV - sighted) * sniperScopeBlend;
    }
    float ScopeLookScale() const {
        // Sights slow the turn rate proportionally to the zoom so the sensitivity
        // at the sight picture feels the same as at the hip.
        return (1.0f - 0.72f * sniperScopeBlend) * (1.0f - 0.30f * adsBlend);
    }
    XMMATRIX GetViewMatrix()       const { return const_cast<Camera&>(camera).GetViewMatrix(); }
    XMMATRIX GetProjectionMatrix() const {
        XMMATRIX projection = XMMatrixPerspectiveFovLH(
            XMConvertToRadians(EffectiveCameraFOV()),
            (float)g_dx12.screenWidth / (float)g_dx12.screenHeight,
            cameraNear, cameraFar);
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
        const XMVECTOR camPos   = XMLoadFloat3(&camera.Position);
        const XMVECTOR camFront = XMVector3Normalize(XMLoadFloat3(&camera.Front));
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
        return XMMatrixRotationX(XMConvertToRadians(-gunRecoilKick)) * basis;
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
        gunRecoilBack = (std::min)(0.16f, gunRecoilBack + 0.13f);
        gunRecoilKick = (std::min)(11.0f, gunRecoilKick + 7.5f);
        muzzleFlashTime = muzzleFlashDuration * 1.35f;

        const XMFLOAT3 muzzle = GetMuzzleWorldPosition();
        const XMVECTOR cameraFront = XMLoadFloat3(&camera.Front);
        const XMVECTOR cameraUp = XMLoadFloat3(&camera.Up);
        const XMVECTOR cameraRight = XMVector3Normalize(
            XMVector3Cross(cameraUp, cameraFront));
        constexpr int pelletCount = 8;
        constexpr float spread = 0.055f;
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
