#ifndef SCENE_H
#define SCENE_H

#include "DX12Core.h"
#include "CameraDX12.h"
#include "ClusteredRendererDX12.h"
#include <vector>

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
};

struct GunViewModel {
    bool     visible  = false;
    XMFLOAT3 color    = { 0.3f, 0.3f, 0.35f };
    XMFLOAT3 offset   = { 0.3f, -0.25f, 0.5f };
    XMFLOAT3 scale    = { 0.15f, 0.15f, 0.15f };
    XMFLOAT3 rotation = { 0.0f, 180.0f, 0.0f };
};

// All mutable scene state lives here
struct Scene {
    // Camera - positioned back so the imported model (a small building) is fully framed at spawn
    Camera camera{ XMFLOAT3(0.0f, 1.7f, 20.0f) };
    float  cameraFOV  = 60.0f;
    float  cameraNear  = 0.1f;
    float  cameraFar   = 100.0f;

    // Main directional / point light
    XMFLOAT3 lightPos    = { -5.0f, 10.0f, -5.0f };
    XMFLOAT3 lightColor  = { 1.15f, 1.08f, 0.96f };
    int      lightType   = 0;
    float    lightConstant  = 1.0f;
    float    lightLinear    = 0.09f;
    float    lightQuadratic = 0.032f;

    // Material defaults
    float ambientStrength   = 0.42f;
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
    float projectileSpeed    = 50.0f;
    float projectileLifetime = 3.0f;
    XMFLOAT3 projectileColor = { 1.0f, 0.8f, 0.0f };
    float projectileScale    = 0.1f;

    // NVIDIA Blast + Box3D destructible house
    bool  useDestruction = true;
    int   destructionGridX = 4;
    int   destructionGridY = 3;
    int   destructionGridZ = 4;
    float destructionDamageRadius = 1.75f;
    float destructionDamage = 2.0f;
    float destructionBulletImpulse = 260.0f;
    bool  rebuildDestructionRequested = false;

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
            p.position.x += p.direction.x * p.speed * dt;
            p.position.y += p.direction.y * p.speed * dt;
            p.position.z += p.direction.z * p.speed * dt;
            p.lifetime -= dt;
            if (p.lifetime <= 0.0f) p.active = false;
        }
        projectiles.erase(
            std::remove_if(projectiles.begin(), projectiles.end(),
                [](const Projectile& p) { return !p.active; }),
            projectiles.end());
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
};

#endif // SCENE_H
