#ifndef SCENE_H
#define SCENE_H

#include "DX12Core.h"
#include "CameraDX12.h"
#include "ClusteredRendererDX12.h"
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cmath>

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
    // Grenade: arcs under gravity and detonates (radial blast) on fuse timeout
    // or first impact. Regular bullets leave these at defaults.
    bool     grenade = false;
    XMFLOAT3 velocity = { 0, 0, 0 };   // grenades integrate velocity + gravity
    float    fuse = 0.0f;              // seconds until it explodes
    bool     detonate = false;         // set the frame it should explode
};

// A particle from a bullet impact: either a rising grey smoke puff or a fast
// ballistic spark/debris shard.
struct ImpactParticle {
    XMFLOAT3 position;
    XMFLOAT3 velocity;
    float    life;      // seconds remaining
    float    maxLife;   // for fade
    float    size;      // smoke: grows; spark: shrinks
    float    growth;    // size delta per second (negative for sparks)
    XMFLOAT3 color;
    bool     spark = false;  // true = ballistic bright shard, false = smoke
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
    float  cameraNear  = 0.1f;
    // Far enough to see the sea run out to the horizon; the ocean plane alone is
    // 600 m across, and a 100 m far plane sliced it off in plain view.
    float  cameraFar   = 800.0f;

    // Main directional / point light
    XMFLOAT3 lightPos    = { -5.0f, 10.0f, -5.0f };
    XMFLOAT3 lightColor  = { 1.15f, 1.08f, 0.96f };
    int      lightType   = 0;
    float    lightConstant  = 1.0f;
    float    lightLinear    = 0.09f;
    float    lightQuadratic = 0.032f;

    // Material defaults
    float ambientStrength   = 0.18f;
    float specularStrength  = 0.5f;
    int   specularShininess = 32;
    float shadowBias        = 0.005f;
    bool  enableShadows     = true;
    XMFLOAT3 shadowCenter    = { 0.0f, 3.0f, 0.0f };
    float shadowOrthoSize    = 30.0f;
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
    float projectileSpeed    = 50.0f;
    float projectileLifetime = 3.0f;
    XMFLOAT3 projectileColor = { 1.0f, 1.0f, 1.0f };
    float projectileScale    = 0.1f;
    bool  autoFire           = true;    // hold mouse to keep firing
    float fireInterval       = 0.1f;    // seconds between auto-fire shots
    float fireCooldown       = 0.0f;    // time left before next shot may fire

    // Grenade (press G): lobbed, arcs under gravity, radial blast on fuse.
    float grenadeThrowSpeed    = 16.0f;  // launch speed along aim
    float grenadeLob           = 3.0f;   // extra upward velocity for the arc
    float grenadeGravityScale  = 1.0f;
    float grenadeGroundY       = 0.15f;  // bounce height
    float grenadeFuse          = 1.8f;   // seconds before it explodes
    float grenadeBlastRadius   = 3.5f;   // radial destruction radius
    float grenadeDamage        = 1.5f;   // per-bond damage in the blast
    float grenadeImpulse       = 120.0f; // shove imparted to loosened pieces
    float grenadeCooldown      = 0.0f;   // input debounce

    // NVIDIA Blast + Box3D destructible house
    bool  useDestruction = true;
    int   destructionGridX = 4;
    int   destructionGridY = 3;
    int   destructionGridZ = 4;
    float destructionDamageRadius = 0.9f;   // tight blast so hits stay local
    float destructionDamage = 0.3f;         // fraction of bond health per shot (~3-4 hits to break)
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

    // DDGI (DX12 backend not wired up yet - irradiance/visibility textures are never
    // created or bound, so sampling them would read garbage descriptor memory)
    bool  useDDGI      = false;
    float giIntensity  = 0.5f;
    float normalBias   = 0.1f;
    float probeSpacing = 2.0f;
    bool  showProbes   = false;

    // Rendering mode
    bool wireframeMode       = false;
    bool useVisibilityBuffer = false; // id Tech VB+Deferred mode
    bool useRaytracing       = false; // DXR raytracing mode

    // Mesh-shader tessellated terrain (replaces the flat floor plane when on)
    bool  useMeshTerrain     = true;
    float terrainHeightScale = 5.0f;

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
            XMFLOAT3 pos(cosf(angle) * 8.0f, 2.0f, sinf(angle) * 8.0f);
            XMFLOAT3 c;
            c.x = sinf(angle) * 0.5f + 0.5f;
            c.y = sinf(angle + 2.094f) * 0.5f + 0.5f;
            c.z = sinf(angle + 4.189f) * 0.5f + 0.5f;
            clusteredRenderer.addLight(pos, c, demoLightRadius, demoLightIntensity);
        }
    }

    void Update(float dt, float currentTime) {
        camera.Update(dt);

        // Animate demo lights
        if (animateDemoLights) {
            for (int i = 0; i < clusteredRenderer.getTotalLightCount(); i++) {
                float angle = (float)i / clusteredRenderer.getTotalLightCount() * XM_2PI + currentTime;
                float r = 8.0f;
                XMFLOAT3 pos(cosf(angle) * r,
                             2.0f + sinf(currentTime * 2.0f + angle) * 1.0f,
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
                // Ballistic arc: integrate velocity under gravity, then bounce
                // off the ground with damping so it settles before the fuse.
                p.velocity.y += -9.81f * grenadeGravityScale * dt;
                p.position.x += p.velocity.x * dt;
                p.position.y += p.velocity.y * dt;
                p.position.z += p.velocity.z * dt;
                if (p.position.y < grenadeGroundY) {
                    p.position.y = grenadeGroundY;
                    p.velocity.y = -p.velocity.y * 0.4f;      // bounce
                    p.velocity.x *= 0.7f; p.velocity.z *= 0.7f; // friction
                }
                p.fuse -= dt;
                if (p.fuse <= 0.0f) { p.detonate = true; p.active = false; }
            } else {
                p.position.x += p.direction.x * p.speed * dt;
                p.position.y += p.direction.y * p.speed * dt;
                p.position.z += p.direction.z * p.speed * dt;
                p.lifetime -= dt;
                if (p.lifetime <= 0.0f) p.active = false;
            }
        }
        // Keep detonating grenades for one more frame so the game loop can read
        // p.detonate; drop the rest.
        projectiles.erase(
            std::remove_if(projectiles.begin(), projectiles.end(),
                [](const Projectile& p) { return !p.active && !p.detonate; }),
            projectiles.end());

        // Impact particles: sparks fly ballistically under gravity; smoke rises
        // and expands. Both fade with life.
        for (auto& ip : impactParticles) {
            if (ip.spark) {
                ip.velocity.y += -22.0f * dt;          // gravity pulls sparks down
                ip.velocity.x *= 0.99f; ip.velocity.z *= 0.99f;
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

    void ShootProjectile() {
        Projectile p;
        p.position  = camera.Position;
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

    // Build matrices
    XMMATRIX GetViewMatrix()       const { return const_cast<Camera&>(camera).GetViewMatrix(); }
    XMMATRIX GetProjectionMatrix() const {
        return XMMatrixPerspectiveFovLH(
            XMConvertToRadians(cameraFOV),
            (float)g_dx12.screenWidth / (float)g_dx12.screenHeight,
            cameraNear, cameraFar);
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

        const XMVECTOR gp = camPos + camFront * gun.offset.z
                                   + camRight * gun.offset.x
                                   + camUp    * gun.offset.y;

        // Orthonormal basis: rows right/up/front, translation at the gun spot.
        XMMATRIX basis = XMMatrixIdentity();
        basis.r[0] = XMVectorSetW(camRight, 0.0f);
        basis.r[1] = XMVectorSetW(camUp, 0.0f);
        basis.r[2] = XMVectorSetW(camFront, 0.0f);
        basis.r[3] = XMVectorSetW(gp, 1.0f);
        return basis;
    }

    // Overall size of the M4 view model (local units before the base transform).
    float GunModelScale() const { return gun.scale.z * 3.0f; }
};

#endif // SCENE_H
