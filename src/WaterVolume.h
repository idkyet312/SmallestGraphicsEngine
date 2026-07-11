#pragma once

// A self-contained body of water: a transparent box-shaped pool with rigid
// bodies floating in it. Runs its own tiny Box3D world (independent of the
// destruction sim) so the buoyancy behaviour stays isolated and simple.
//
// Box3D has no fluid solver, so buoyancy is faked per body: any floater whose
// centre dips below the water surface gets an upward force proportional to how
// deeply it is submerged (Archimedes), plus drag so it settles and bobs rather
// than oscillating forever.

#include <DirectXMath.h>
#include <box3d/box3d.h>
#include <vector>
#include <cmath>

using namespace DirectX;

struct WaterFloater {
    b3BodyId body;
    XMFLOAT3 half;     // half-extents of the box
    XMFLOAT3 color;
};

struct WaterFloaterItem {
    XMFLOAT4X4 transform;
    XMFLOAT3   color;
};

class WaterVolume {
public:
    // `center` is the centre of the pool footprint; `extents` are the full
    // width/height/depth of the water box. The surface sits at the top of it.
    void Initialize(const XMFLOAT3& center, const XMFLOAT3& extents) {
        Shutdown();
        m_center = center;
        m_extents = extents;
        m_surfaceY = center.y + extents.y * 0.5f;

        b3WorldDef worldDef = b3DefaultWorldDef();
        worldDef.gravity = { 0.0f, -9.81f, 0.0f };
        m_world = b3CreateWorld(&worldDef);
        if (B3_IS_NULL(m_world)) return;

        const float hx = extents.x * 0.5f;
        const float hy = extents.y * 0.5f;
        const float hz = extents.z * 0.5f;
        constexpr float wall = 0.4f;   // thickness of the tank walls

        // Tank: a static box floor plus four static walls so the floaters stay
        // penned inside the pool instead of drifting or falling out.
        auto addStatic = [&](float px, float py, float pz, float ex, float ey, float ez) {
            b3BodyDef bd = b3DefaultBodyDef();
            bd.position = { px, py, pz };
            b3BodyId body = b3CreateBody(m_world, &bd);
            b3BoxHull hull = b3MakeBoxHull(ex, ey, ez);
            b3ShapeDef sd = b3DefaultShapeDef();
            sd.baseMaterial.friction = 0.4f;
            b3CreateHullShape(body, &sd, &hull.base);
        };
        const float floorY = center.y - hy;
        addStatic(center.x, floorY, center.z, hx, wall, hz);                     // floor
        addStatic(center.x - hx, center.y, center.z, wall, hy, hz);              // -X wall
        addStatic(center.x + hx, center.y, center.z, wall, hy, hz);              // +X wall
        addStatic(center.x, center.y, center.z - hz, hx, hy, wall);             // -Z wall
        addStatic(center.x, center.y, center.z + hz, hx, hy, wall);             // +Z wall

        // Floaters: a grid of light wooden crates dropped in above the water so
        // they splash down and bob. Density well below water (1000) so they ride
        // high on the surface.
        const XMFLOAT3 woods[] = {
            { 0.55f, 0.40f, 0.24f }, { 0.62f, 0.47f, 0.30f },
            { 0.48f, 0.33f, 0.20f }, { 0.70f, 0.55f, 0.36f },
        };
        int idx = 0;
        for (int gx = 0; gx < 3; ++gx)
        for (int gz = 0; gz < 3; ++gz) {
            const float fx = center.x + (gx - 1) * (hx * 0.55f);
            const float fz = center.z + (gz - 1) * (hz * 0.55f);
            const float fy = m_surfaceY + 1.5f + gx * 0.4f;   // start above the water
            const float s = 0.35f + 0.12f * ((gx + gz) % 3);
            SpawnFloater({ fx, fy, fz }, { s, s * 0.7f, s }, woods[idx++ % 4]);
        }
    }

    void SpawnFloater(const XMFLOAT3& position, const XMFLOAT3& half, const XMFLOAT3& color) {
        if (B3_IS_NULL(m_world)) return;
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position = { position.x, position.y, position.z };
        bd.linearDamping = 0.1f;
        bd.angularDamping = 0.3f;
        b3BodyId body = b3CreateBody(m_world, &bd);
        b3ShapeDef sd = b3DefaultShapeDef();
        sd.density = 400.0f;                 // < water density -> floats
        sd.baseMaterial.friction = 0.4f;
        sd.baseMaterial.restitution = 0.05f;
        b3BoxHull hull = b3MakeBoxHull(half.x, half.y, half.z);
        b3CreateHullShape(body, &sd, &hull.base);
        m_floaters.push_back({ body, half, color });
    }

    void Update(float dt) {
        if (B3_IS_NULL(m_world)) return;
        m_accumulator = std::min(0.1f, m_accumulator + dt);
        constexpr float step = 1.0f / 60.0f;
        while (m_accumulator >= step) {
            ApplyBuoyancy();
            b3World_Step(m_world, step, 4);
            m_accumulator -= step;
        }
        RebuildItems();
    }

    const std::vector<WaterFloaterItem>& GetFloaterItems() const { return m_items; }

    // The water box itself, for the transparent render pass.
    XMFLOAT3 GetCenter() const { return m_center; }
    XMFLOAT3 GetExtents() const { return m_extents; }
    bool IsInitialized() const { return !B3_IS_NULL(m_world); }

    void Shutdown() {
        if (!B3_IS_NULL(m_world)) b3DestroyWorld(m_world);
        m_world = b3_nullWorldId;
        m_floaters.clear();
        m_items.clear();
        m_accumulator = 0.0f;
    }

    ~WaterVolume() { Shutdown(); }

private:
    // Archimedes buoyancy + drag per floater. Approximate the submerged volume
    // by how far the box's vertical span sits below the surface.
    void ApplyBuoyancy() {
        constexpr float kWaterDensity = 1000.0f;   // kg/m^3
        constexpr float kGravity = 9.81f;
        for (const WaterFloater& f : m_floaters) {
            const b3Pos p = b3Body_GetPosition(f.body);
            const float top = (float)p.y + f.half.y;
            const float bottom = (float)p.y - f.half.y;
            const float boxHeight = std::max(1e-4f, top - bottom);
            // Fraction of the box's height that is below the water surface.
            float submerged = (m_surfaceY - bottom) / boxHeight;
            submerged = std::max(0.0f, std::min(1.0f, submerged));
            if (submerged <= 0.0f) continue;   // fully out of the water

            const float volume = f.half.x * 2.0f * f.half.y * 2.0f * f.half.z * 2.0f;
            const float displaced = volume * submerged;
            const float buoyancy = kWaterDensity * kGravity * displaced;

            const b3Vec3 vel = b3Body_GetLinearVelocity(f.body);
            // Vertical drag damps the bob; horizontal drag makes the water feel
            // viscous so crates drift to rest instead of sliding forever.
            const float vDrag = -vel.y * 120.0f * submerged;
            const b3Vec3 force = {
                -vel.x * 40.0f * submerged,
                buoyancy + vDrag,
                -vel.z * 40.0f * submerged
            };
            b3Body_ApplyForceToCenter(f.body, force, true);
        }
    }

    void RebuildItems() {
        m_items.clear();
        m_items.reserve(m_floaters.size());
        for (const WaterFloater& f : m_floaters) {
            const b3Pos p = b3Body_GetPosition(f.body);
            const b3Quat q = b3Body_GetRotation(f.body);
            XMVECTOR rot = XMVectorSet(q.v.x, q.v.y, q.v.z, q.s);
            XMMATRIX t = XMMatrixScaling(f.half.x * 2.0f, f.half.y * 2.0f, f.half.z * 2.0f) *
                XMMatrixRotationQuaternion(rot) *
                XMMatrixTranslation((float)p.x, (float)p.y, (float)p.z);
            WaterFloaterItem item;
            XMStoreFloat4x4(&item.transform, t);
            item.color = f.color;
            m_items.push_back(item);
        }
    }

    b3WorldId m_world = b3_nullWorldId;
    std::vector<WaterFloater> m_floaters;
    std::vector<WaterFloaterItem> m_items;
    XMFLOAT3 m_center{};
    XMFLOAT3 m_extents{};
    float m_surfaceY = 0.0f;
    float m_accumulator = 0.0f;
};
