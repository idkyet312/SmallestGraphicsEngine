#pragma once

// A self-contained body of water: an animated, undulating surface mesh with
// rigid bodies that float and bob in it.
//
// Two things run here:
//   1. A wave surface -- a flat grid of vertices whose heights are recomputed on
//      the CPU every frame from a sum of moving directional waves (Gerstner-style
//      swell) shaped by a Gaussian falloff toward the pool edges, so the water is
//      calm against the banks and rolls in the middle like real contained water.
//      Splashes add outward-travelling ring waves. Normals are rebuilt each frame
//      so the surface catches light and looks wet.
//   2. A tiny Box3D world of rigid floaters (crates, planks) plus any destruction
//      debris that falls in. Box3D has no fluid solver, so buoyancy is faked per
//      body against the *live wave height* under it: a body dipping below the
//      local surface gets an Archimedes lift plus drag, and rides the swell.
//
// The mesh lives in a small ring of GPU UPLOAD buffers (one per frame in flight)
// written directly each frame -- cheap for a grid this size.

#include <DirectXMath.h>
#include "DX12Core.h"
#include <box3d/box3d.h>
#include <vector>
#include <cmath>
#include <cfloat>
#include <algorithm>
#include <functional>

using namespace DirectX;

struct WaterFloater {
    b3BodyId body;
    XMFLOAT3 half;     // half-extents of the box
    XMFLOAT3 color;
    bool     wasSubmerged = false;   // for splash-on-entry detection
};

struct WaterFloaterItem {
    XMFLOAT4X4 transform;
    XMFLOAT3   color;
};

class WaterVolume {
public:
    // Vertex layout mirrors the forward renderer's VertexPosNormUV so the water
    // grid can be drawn with the ordinary object shader.
    struct WaterVertex {
        XMFLOAT3 position;
        XMFLOAT3 normal;
        XMFLOAT2 texCoord;
        XMFLOAT4 tangent = XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f);
    };

    // `center` is the centre of the pool footprint; `extents` are the full
    // width/height/depth of the water box. The surface sits at the top of it.
    //
    // `terrainHeight`, if supplied, is the ground-height sampler (the CPU mirror
    // of the terrain shader). When given, the pool floor/walls are replaced by a
    // static heightfield of the *actual dug basin*, so crates and debris collide
    // with the sloped terrain instead of an invisible box.
    void Initialize(const XMFLOAT3& center, const XMFLOAT3& extents,
                    const std::function<float(float, float)>& terrainHeight = {}) {
        Shutdown();
        m_center = center;
        m_extents = extents;
        m_surfaceY = center.y + extents.y * 0.5f;

        // ---- wave surface mesh ----
        m_gridN = 48;
        BuildTopology();
        AllocateBuffers();
        m_meshReady = (m_buffers[0] != nullptr);

        // ---- rigid-body world ----
        b3WorldDef worldDef = b3DefaultWorldDef();
        worldDef.gravity = { 0.0f, -9.81f, 0.0f };
        m_world = b3CreateWorld(&worldDef);
        if (B3_IS_NULL(m_world)) return;

        const float hx = extents.x * 0.5f;
        const float hy = extents.y * 0.5f;
        const float hz = extents.z * 0.5f;
        constexpr float wall = 0.4f;   // thickness of the tank walls

        auto addStatic = [&](float px, float py, float pz, float ex, float ey, float ez) {
            b3BodyDef bd = b3DefaultBodyDef();
            bd.position = { px, py, pz };
            b3BodyId body = b3CreateBody(m_world, &bd);
            b3BoxHull hull = b3MakeBoxHull(ex, ey, ez);
            b3ShapeDef sd = b3DefaultShapeDef();
            sd.baseMaterial.friction = 0.6f;
            b3CreateHullShape(body, &sd, &hull.base);
        };

        if (terrainHeight) {
            // Heightfield basin: a grid of static boxes whose tops sit at the
            // terrain height, so the sloped dug-out ground is solid. Boxes span a
            // bit past the water footprint so debris can't squeeze over the rim.
            constexpr int cells = 20;
            const float pad = 3.0f;
            const float spanX = extents.x + pad * 2.0f;
            const float spanZ = extents.z + pad * 2.0f;
            const float cx = spanX / cells, cz = spanZ / cells;
            const float x0 = center.x - spanX * 0.5f + cx * 0.5f;
            const float z0 = center.z - spanZ * 0.5f + cz * 0.5f;
            constexpr float thick = 3.0f;   // box half-height; buries the base deep
            for (int gz = 0; gz < cells; ++gz)
            for (int gx = 0; gx < cells; ++gx) {
                const float px = x0 + gx * cx;
                const float pz = z0 + gz * cz;
                const float gy = terrainHeight(px, pz);      // ground surface here
                // Box top aligned to ground: centre = top - halfHeight.
                addStatic(px, gy - thick, pz, cx * 0.5f + 0.02f, thick, cz * 0.5f + 0.02f);
            }
        } else {
            // Fallback flat tank: a floor plus four walls.
            const float floorY = center.y - hy;
            addStatic(center.x, floorY, center.z, hx, wall, hz);
            addStatic(center.x - hx, center.y, center.z, wall, hy, hz);
            addStatic(center.x + hx, center.y, center.z, wall, hy, hz);
            addStatic(center.x, center.y, center.z - hz, hx, hy, wall);
            addStatic(center.x, center.y, center.z + hz, hx, hy, wall);
        }
        // No floaters: the pool is just the wave surface (crates removed).
    }

    b3BodyId SpawnFloater(const XMFLOAT3& position, const XMFLOAT3& half,
                          const XMFLOAT3& color, float density = 400.0f) {
        if (B3_IS_NULL(m_world)) return b3_nullBodyId;
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position = { position.x, position.y, position.z };
        bd.linearDamping = 0.1f;
        bd.angularDamping = 0.3f;
        b3BodyId body = b3CreateBody(m_world, &bd);
        b3ShapeDef sd = b3DefaultShapeDef();
        sd.density = density;                 // < water density (1000) -> floats
        sd.baseMaterial.friction = 0.4f;
        sd.baseMaterial.restitution = 0.05f;
        b3BoxHull hull = b3MakeBoxHull(half.x, half.y, half.z);
        b3CreateHullShape(body, &sd, &hull.base);
        m_floaters.push_back({ body, half, color, false });
        return body;
    }

    void Update(float dt) {
        if (B3_IS_NULL(m_world)) { m_time += dt; PruneRipples(); return; }
        m_accumulator = std::min(0.1f, m_accumulator + dt);
        constexpr float step = 1.0f / 60.0f;
        while (m_accumulator >= step) {
            m_time += step;
            ApplyBuoyancy();
            b3World_Step(m_world, step, 4);
            m_accumulator -= step;
        }
        PruneRipples();
        RebuildItems();
    }

    const std::vector<WaterFloaterItem>& GetFloaterItems() const { return m_items; }

    // --- wave surface access (renderer side) ---
    // Recompute the surface into `frameIndex`'s buffer and return its view.
    const D3D12_VERTEX_BUFFER_VIEW& UpdateAndGetVBV(UINT frameIndex) {
        const UINT f = frameIndex % kFrames;
        if (m_meshReady) WriteSurface(f);
        return m_vbv[f];
    }
    UINT GetIndexCount() const { return (UINT)m_indices.size(); }
    const D3D12_INDEX_BUFFER_VIEW& GetIBV() const { return m_ibv; }

    // Drop a stone: an outward-travelling ring wave centred at world (x,z).
    void Splash(float x, float z, float strength = 1.0f) {
        m_ripples.push_back({ x, z, m_time, strength });
        if (m_ripples.size() > 24) m_ripples.erase(m_ripples.begin());
    }

    // Surface elevation above rest at world (x,z): calm-edged swell plus decaying
    // circular ripples. Drives both buoyancy and the render mesh.
    float WaveHeightAt(float x, float z) const {
        const float lx = x - m_center.x;
        const float lz = z - m_center.z;
        const float hx = m_extents.x * 0.5f;
        const float hz = m_extents.z * 0.5f;

        // Gaussian bell: waves tallest at the centre, tapering to zero at the
        // banks so the surface meets the walls flush.
        const float nx = hx > 0.0f ? lx / hx : 0.0f;
        const float nz = hz > 0.0f ? lz / hz : 0.0f;
        const float bell = std::exp(-2.2f * (nx * nx + nz * nz));

        float h = 0.0f;
        h += 0.14f * std::sin(0.90f * lx + 1.30f * m_time);
        h += 0.10f * std::sin(0.70f * lz - 1.70f * m_time + 0.6f * lx);
        h += 0.06f * std::sin(1.60f * (lx + lz) + 2.40f * m_time);
        h += 0.04f * std::sin(2.30f * lx - 2.90f * m_time + 1.3f * lz);
        h *= bell;

        for (const Ripple& r : m_ripples) {
            const float age = m_time - r.t0;
            if (age <= 0.0f) continue;
            const float dx = x - r.x, dz = z - r.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const float front = 3.0f * age;              // ring radius
            const float band = dist - front;
            const float env = std::exp(-age * 1.2f) * std::exp(-band * band * 1.5f);
            h += r.strength * 0.5f * std::sin(6.0f * band) * env;
        }
        return h;
    }

    // Shoot the crates: if the segment start->end passes within `radius` of a
    // floater, shove that floater along `direction` and splash. Returns true.
    bool ShootFloaters(const XMFLOAT3& start, const XMFLOAT3& end,
                       const XMFLOAT3& direction, float radius, float impulse) {
        if (B3_IS_NULL(m_world)) return false;
        const XMVECTOR a = XMLoadFloat3(&start);
        const XMVECTOR b = XMLoadFloat3(&end);
        const XMVECTOR ab = b - a;
        const float abLenSq = std::max(1e-6f, XMVectorGetX(XMVector3LengthSq(ab)));
        bool hit = false;
        float bestT = FLT_MAX;
        WaterFloater* target = nullptr;
        for (WaterFloater& f : m_floaters) {
            const b3Pos p = b3Body_GetPosition(f.body);
            const XMVECTOR c = XMVectorSet((float)p.x, (float)p.y, (float)p.z, 0.0f);
            float t = XMVectorGetX(XMVector3Dot(c - a, ab)) / abLenSq;
            t = std::max(0.0f, std::min(1.0f, t));
            const XMVECTOR closest = a + ab * t;
            const float distSq = XMVectorGetX(XMVector3LengthSq(c - closest));
            const float reach = radius + std::max(f.half.x, std::max(f.half.y, f.half.z));
            if (distSq <= reach * reach && t < bestT) { bestT = t; target = &f; hit = true; }
        }
        if (hit && target) {
            const float mass = std::max(0.05f, b3Body_GetMass(target->body));
            const float mag = std::min(impulse, 12.0f * mass);
            const b3Vec3 imp = { direction.x * mag, direction.y * mag + 0.2f * mag, direction.z * mag };
            b3Body_ApplyLinearImpulseToCenter(target->body, imp, true);
            const b3Pos tp = b3Body_GetPosition(target->body);
            Splash((float)tp.x, (float)tp.z, 1.2f);
        }
        return hit;
    }

    XMFLOAT3 GetCenter() const { return m_center; }
    XMFLOAT3 GetExtents() const { return m_extents; }
    float    GetSurfaceY() const { return m_surfaceY; }
    bool     IsInitialized() const { return m_meshReady || !B3_IS_NULL(m_world); }

    void Shutdown() {
        if (!B3_IS_NULL(m_world)) b3DestroyWorld(m_world);
        m_world = b3_nullWorldId;
        m_floaters.clear();
        m_items.clear();
        for (UINT f = 0; f < kFrames; ++f) {
            if (m_mapped[f] && m_buffers[f]) m_buffers[f]->Unmap(0, nullptr);
            m_mapped[f] = nullptr;
            m_buffers[f].Reset();
            m_vbv[f] = {};
        }
        m_indexBuffer.Reset();
        m_ibv = {};
        m_indices.clear();
        m_ripples.clear();
        m_accumulator = 0.0f;
        m_time = 0.0f;
        m_meshReady = false;
    }

    ~WaterVolume() { Shutdown(); }

private:
    static constexpr UINT kFrames = 3;   // frames in flight
    struct Ripple { float x, z, t0, strength; };

    void PruneRipples() {
        m_ripples.erase(
            std::remove_if(m_ripples.begin(), m_ripples.end(),
                [&](const Ripple& r) { return m_time - r.t0 > 4.0f; }),
            m_ripples.end());
    }

    // Archimedes buoyancy + drag per floater, against the *live wave surface*
    // under each body, so crates ride and bob on the swell.
    void ApplyBuoyancy() {
        constexpr float kWaterDensity = 1000.0f;   // kg/m^3
        constexpr float kGravity = 9.81f;
        constexpr float kEps = 0.4f;                // wave-slope sample spacing
        for (WaterFloater& f : m_floaters) {
            const b3Pos p = b3Body_GetPosition(f.body);
            const float surfY = m_surfaceY + WaveHeightAt((float)p.x, (float)p.z);
            const float top = (float)p.y + f.half.y;
            const float bottom = (float)p.y - f.half.y;
            const float boxHeight = std::max(1e-4f, top - bottom);
            float submerged = (surfY - bottom) / boxHeight;
            submerged = std::max(0.0f, std::min(1.0f, submerged));

            const b3Vec3 vel = b3Body_GetLinearVelocity(f.body);
            // Splash on entry: first frame this body's base dips under the surface
            // while dropping fast, kick a ripple scaled by impact speed.
            const bool nowIn = submerged > 0.0f;
            if (nowIn && !f.wasSubmerged && vel.y < -1.5f)
                Splash((float)p.x, (float)p.z, std::min(2.0f, -vel.y * 0.15f));
            f.wasSubmerged = nowIn;
            if (!nowIn) continue;   // fully out of the water

            const float volume = f.half.x * 2.0f * f.half.y * 2.0f * f.half.z * 2.0f;
            const float displaced = volume * submerged;
            const float buoyancy = kWaterDensity * kGravity * displaced;

            // Wave slope: floaters slide "downhill" so they drift into troughs.
            const float gx = WaveHeightAt((float)p.x + kEps, (float)p.z) -
                             WaveHeightAt((float)p.x - kEps, (float)p.z);
            const float gz = WaveHeightAt((float)p.x, (float)p.z + kEps) -
                             WaveHeightAt((float)p.x, (float)p.z - kEps);
            const float push = kWaterDensity * kGravity * displaced * 0.5f;

            const float vDrag = -vel.y * 120.0f * submerged;
            const b3Vec3 force = {
                -vel.x * 40.0f * submerged - push * gx / (2.0f * kEps),
                buoyancy + vDrag,
                -vel.z * 40.0f * submerged - push * gz / (2.0f * kEps)
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

    // ---- wave mesh internals ----

    void BuildTopology() {
        m_indices.clear();
        const int n = m_gridN;
        for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const uint32_t a = (uint32_t)(z * (n + 1) + x);
            const uint32_t b = a + 1;
            const uint32_t c = a + (n + 1);
            const uint32_t d = c + 1;
            m_indices.push_back(a); m_indices.push_back(c); m_indices.push_back(b);
            m_indices.push_back(b); m_indices.push_back(c); m_indices.push_back(d);
        }
    }

    void AllocateBuffers() {
        const UINT vcount = (UINT)((m_gridN + 1) * (m_gridN + 1));
        const UINT vsize = vcount * (UINT)sizeof(WaterVertex);
        for (UINT f = 0; f < kFrames; ++f) {
            if (!MakeUploadBuffer(vsize, m_buffers[f], m_mapped[f])) return;
            m_vbv[f].BufferLocation = m_buffers[f]->GetGPUVirtualAddress();
            m_vbv[f].SizeInBytes = vsize;
            m_vbv[f].StrideInBytes = sizeof(WaterVertex);
        }
        const UINT isize = (UINT)(m_indices.size() * sizeof(uint32_t));
        void* imap = nullptr;
        if (!MakeUploadBuffer(isize, m_indexBuffer, imap)) return;
        memcpy(imap, m_indices.data(), isize);
        m_indexBuffer->Unmap(0, nullptr);
        m_ibv.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
        m_ibv.SizeInBytes = isize;
        m_ibv.Format = DXGI_FORMAT_R32_UINT;
    }

    static bool MakeUploadBuffer(UINT size, ComPtr<ID3D12Resource>& buffer, void*& mapped) {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer));
        if (FAILED(hr)) return false;
        D3D12_RANGE r = { 0, 0 };
        return SUCCEEDED(buffer->Map(0, &r, &mapped));
    }

    void WriteSurface(UINT f) {
        if (!m_mapped[f]) return;
        const int n = m_gridN;
        const float hx = m_extents.x * 0.5f;
        const float hz = m_extents.z * 0.5f;
        WaterVertex* out = (WaterVertex*)m_mapped[f];

        auto worldX = [&](int ix) { return m_center.x - hx + (m_extents.x * ix) / n; };
        auto worldZ = [&](int iz) { return m_center.z - hz + (m_extents.z * iz) / n; };
        auto surf = [&](int ix, int iz) { return m_surfaceY + WaveHeightAt(worldX(ix), worldZ(iz)); };

        for (int iz = 0; iz <= n; ++iz)
        for (int ix = 0; ix <= n; ++ix) {
            const float x = worldX(ix);
            const float z = worldZ(iz);
            const float y = surf(ix, iz);

            const int xm = ix > 0 ? ix - 1 : ix, xp = ix < n ? ix + 1 : ix;
            const int zm = iz > 0 ? iz - 1 : iz, zp = iz < n ? iz + 1 : iz;
            const float dhx = (surf(xp, iz) - surf(xm, iz));
            const float dhz = (surf(ix, zp) - surf(ix, zm));
            const float ddx = (worldX(xp) - worldX(xm));
            const float ddz = (worldZ(zp) - worldZ(zm));
            XMVECTOR nrm = XMVector3Normalize(XMVectorSet(
                ddx != 0.0f ? -dhx / ddx : 0.0f, 1.0f,
                ddz != 0.0f ? -dhz / ddz : 0.0f, 0.0f));
            XMFLOAT3 nf; XMStoreFloat3(&nf, nrm);

            WaterVertex& v = out[iz * (n + 1) + ix];
            v.position = { x, y, z };
            v.normal = nf;
            v.texCoord = { (float)ix / n, (float)iz / n };
            v.tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
        }
    }

    // ---- rigid-body state ----
    b3WorldId m_world = b3_nullWorldId;
    std::vector<WaterFloater> m_floaters;
    std::vector<WaterFloaterItem> m_items;
    float m_accumulator = 0.0f;

    // ---- wave mesh state ----
    ComPtr<ID3D12Resource>   m_buffers[kFrames];
    void*                    m_mapped[kFrames] = {};
    D3D12_VERTEX_BUFFER_VIEW m_vbv[kFrames] = {};
    ComPtr<ID3D12Resource>   m_indexBuffer;
    D3D12_INDEX_BUFFER_VIEW  m_ibv = {};
    std::vector<uint32_t>    m_indices;
    std::vector<Ripple>      m_ripples;
    bool m_meshReady = false;
    int  m_gridN = 48;

    // ---- shared ----
    XMFLOAT3 m_center{};
    XMFLOAT3 m_extents{};
    float m_surfaceY = 0.0f;
    float m_time = 0.0f;
};
