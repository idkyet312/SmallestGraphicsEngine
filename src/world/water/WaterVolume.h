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
#include "OceanWaveSettings.h"
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
    // Surface mesh resolution (cells per side). Call before Initialize.
    void SetGridResolution(int n) { m_requestedGridN = std::max(8, n); }
    void SetOceanProfile(bool enabled = true) { m_oceanProfile = enabled; }
    void SetOceanWaveSettings(const OceanWaveSettings& settings) {
        m_oceanSettings = settings;
    }
    const OceanWaveSettings& GetOceanWaveSettings() const {
        return m_oceanSettings;
    }

    void Initialize(const XMFLOAT3& center, const XMFLOAT3& extents,
                    const std::function<float(float, float)>& terrainHeight = {}) {
        Shutdown();
        m_center = center;
        m_extents = extents;
        m_surfaceY = center.y + extents.y * 0.5f;

        // ---- wave surface mesh ----
        // Grid resolution is per-volume: an ocean spanning hundreds of metres needs
        // far more cells than a small pool to keep the quads (and so the waves) at
        // a sane size. Cost is CPU-side only -- the surface is rewritten each frame.
        m_gridN = m_requestedGridN;
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
    // Recompute the surface every m_updateInterval calls and return the view of
    // the most recently WRITTEN buffer -- never the raw frame slot, which on a
    // skipped frame would hold a surface from kFrames ago and visibly jump.
    // Slots advance only on write, so with 2 frames in flight and 3 slots the
    // earliest rewrite of a buffer is well after the GPU last read it.
    const D3D12_VERTEX_BUFFER_VIEW& UpdateAndGetVBV(UINT frameIndex) {
        (void)frameIndex;
        if (m_meshReady && (m_updateCounter++ % m_updateInterval) == 0) {
            m_currentSlot = (m_currentSlot + 1) % kFrames;
            WriteSurface(m_currentSlot);
        }
        return m_vbv[m_currentSlot];
    }

    // The same view without the write. A second camera drawing this volume in
    // the same frame -- the sniper scope -- must not advance the counter: doing
    // so would rotate the slot twice per frame, turning the update interval
    // into full rate and rewriting a buffer the main view's draw still reads.
    const D3D12_VERTEX_BUFFER_VIEW& GetCurrentVBV() const {
        return m_vbv[m_currentSlot];
    }

    // Recompute the surface every n-th rendered frame (1 = every frame). The
    // big ocean grid is CPU-priced (16k sine evals per write); the swell moves
    // slowly enough that half rate is imperceptible.
    void SetUpdateInterval(UINT n) { m_updateInterval = (std::max)(1u, n); }
    UINT GetIndexCount() const { return (UINT)m_indices.size(); }
    const D3D12_INDEX_BUFFER_VIEW& GetIBV() const { return m_ibv; }

    // Drop a stone: an outward-travelling ring wave centred at world (x,z).
    void Splash(float x, float z, float strength = 1.0f) {
        m_ripples.push_back({ x, z, m_time, strength });
        if (m_ripples.size() > 24) m_ripples.erase(m_ripples.begin());
    }

    // Swept bullet test against the live wavy surface. Endpoint signs catch a
    // crossing even when a fast round travels several metres in one frame.
    bool ShootSurface(const XMFLOAT3& start, const XMFLOAT3& end,
                      XMFLOAT3& hit, float strength = 0.38f) {
        if (!IsInitialized()) return false;

        // March the segment instead of only testing its endpoints. A round at
        // 300 m/s covers ~5 m per frame; on a shallow, long-range shot both
        // endpoints can sit above the surface while the middle of the segment
        // passes through a wave crest, and a pure endpoint-sign test misses
        // that entirely -- which is why splashes only appeared on steep or
        // close shots. Sampling along the span catches the crossing wherever
        // it happens. Same shape as the terrain sweep: step, then bisect.
        const float dx = end.x - start.x;
        const float dy = end.y - start.y;
        const float dz = end.z - start.z;
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (length < 1e-6f) return false;
        const int steps = (std::max)(4, (std::min)(32,
            static_cast<int>(std::ceil(length / 0.35f))));

        auto signedDepth = [&](float t) {
            const float px = start.x + dx * t;
            const float py = start.y + dy * t;
            const float pz = start.z + dz * t;
            return py - (m_surfaceY + WaveHeightAt(px, pz));
        };

        float previousT = 0.0f;
        float d0 = signedDepth(0.0f);
        float crossLo = -1.0f, crossHi = -1.0f;
        for (int step = 1; step <= steps; ++step) {
            const float t = static_cast<float>(step) / static_cast<float>(steps);
            const float d = signedDepth(t);
            if (d0 * d <= 0.0f && std::abs(d0 - d) > 1e-6f) {
                crossLo = previousT;
                crossHi = t;
                break;
            }
            previousT = t;
            d0 = d;
        }
        if (crossHi < 0.0f) return false;   // never broke the surface

        // Bisect the bracketed span down to the actual crossing point.
        for (int refine = 0; refine < 6; ++refine) {
            const float mid = (crossLo + crossHi) * 0.5f;
            if (signedDepth(crossLo) * signedDepth(mid) <= 0.0f) crossHi = mid;
            else crossLo = mid;
        }
        const float t = (crossLo + crossHi) * 0.5f;
        const float x = start.x + dx * t;
        const float z = start.z + dz * t;
        const float halfX = m_extents.x * 0.5f;
        const float halfZ = m_extents.z * 0.5f;
        if (x < m_center.x - halfX || x > m_center.x + halfX ||
            z < m_center.z - halfZ || z > m_center.z + halfZ) return false;

        hit = { x, m_surfaceY + WaveHeightAt(x, z), z };
        Splash(x, z, strength);
        return true;
    }

    // Surface elevation above rest at world (x,z): calm-edged swell plus decaying
    // circular ripples. Drives buoyancy (the render mesh wants the slope too, so
    // it calls WaveHeightAndSlopeAt directly). Defined in terms of that one
    // function so the two cannot drift out of step.
    float WaveHeightAt(float x, float z) const {
        float dhdx, dhdz;
        return WaveHeightAndSlopeAt(x, z, dhdx, dhdz);
    }

    // Height AND its slope at (x,z), in one pass.
    //
    // The mesh needs a normal per vertex, and the obvious way to get one is to
    // sample the height four more times around the point and take a finite
    // difference. That made the surface cost FIVE height evaluations per vertex --
    // and on the 129x129 ocean grid, each one being a handful of sin/exp calls,
    // that was the single most expensive thing on the CPU each frame.
    //
    // But this height field is a closed form: a sum of sines times a Gaussian bell,
    // plus ripple terms. Its derivative is just as closed-form, so the gradient can
    // be accumulated alongside the height for a few extra multiplies -- one
    // evaluation instead of five, and it is exact rather than a difference
    // approximation.
    //
    // Returns h; writes dh/dx and dh/dz to the out-params.
    float WaveHeightAndSlopeAt(float x, float z, float& dhdx, float& dhdz) const {
        const float lx = x - m_center.x;
        const float lz = z - m_center.z;
        const float hx = m_extents.x * 0.5f;
        const float hz = m_extents.z * 0.5f;

        // Pools taper at their banks. Open ocean uses an unbounded shared wave
        // field so CPU hit/buoyancy queries match the GPU clipmap surface.
        const float nx = hx > 0.0f ? lx / hx : 0.0f;
        const float nz = hz > 0.0f ? lz / hz : 0.0f;
        const float bell = m_oceanProfile
            ? 1.0f : std::exp(-2.2f * (nx * nx + nz * nz));
        // d(bell)/dlx and /dlz. (nx = lx/hx, so dnx/dlx = 1/hx.)
        const float dbell_dx = hx > 0.0f ? bell * -4.4f * nx / hx : 0.0f;
        const float dbell_dz = hz > 0.0f ? bell * -4.4f * nz / hz : 0.0f;

        // The swell, and its gradient, before the bell is applied. Each term is
        // A*sin(P), so d/dlx = A*cos(P) * dP/dlx.
        float s = 0.0f, dsdx = 0.0f, dsdz = 0.0f;
        auto wave = [&](float amp, float kx, float kz, float phase) {
            const float p = kx * lx + kz * lz + phase;
            const float c = amp * std::cos(p);
            s    += amp * std::sin(p);
            dsdx += c * kx;
            dsdz += c * kz;
        };
        if (m_oceanProfile) {
            s = m_oceanSettings.EvaluateHeightAndSlope(
                lx, lz, m_time, dsdx, dsdz);
        } else {
            wave(0.14f, 0.90f, 0.00f,  1.30f * m_time);
            wave(0.10f, 0.60f, 0.70f, -1.70f * m_time);
            wave(0.06f, 1.60f, 1.60f,  2.40f * m_time);
            wave(0.04f, 2.30f, 1.30f, -2.90f * m_time);
        }

        // Product rule for (swell * bell).
        float h = s * bell;
        dhdx = dsdx * bell + s * dbell_dx;
        dhdz = dsdz * bell + s * dbell_dz;

        // Ripples: r * sin(6*band) * exp(-a*1.2) * exp(-band^2 * 1.5), where
        // band = dist - front and d(band)/dx = dx/dist. Chain rule through band.
        for (const Ripple& r : m_ripples) {
            const float age = m_time - r.t0;
            if (age <= 0.0f) continue;
            const float dx = x - r.x, dz = z - r.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            if (dist < 1e-5f) continue;              // gradient is undefined at the centre
            const float front = 3.0f * age;
            const float band = dist - front;
            const float decay = std::exp(-age * 1.2f);
            const float env = decay * std::exp(-band * band * 1.5f);
            const float amp = r.strength * 0.5f;
            const float sn = std::sin(6.0f * band);
            const float cs = std::cos(6.0f * band);

            h += amp * sn * env;
            // d/dband of [sin(6b) * exp(-1.5 b^2)] = 6cos(6b)*e + sin(6b)*(-3b)*e
            const float dband = amp * env * (6.0f * cs - 3.0f * band * sn);
            dhdx += dband * (dx / dist);
            dhdz += dband * (dz / dist);
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
    float    GetTime() const { return m_time; }
    bool     IsInitialized() const { return m_meshReady || !B3_IS_NULL(m_world); }

    void ResetSurface() {
        m_ripples.clear();
        m_accumulator = 0.0f;
        m_time = 0.0f;
        m_updateCounter = 0;
        m_currentSlot = 0;
    }

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

        // One height evaluation per vertex, with the normal coming from the exact
        // gradient rather than four extra samples -- see WaveHeightAndSlopeAt. On
        // the 129x129 ocean that is 16,641 evaluations a frame instead of 83,205.
        for (int iz = 0; iz <= n; ++iz)
        for (int ix = 0; ix <= n; ++ix) {
            const float x = worldX(ix);
            const float z = worldZ(iz);

            float dhdx = 0.0f, dhdz = 0.0f;
            const float y = m_surfaceY + WaveHeightAndSlopeAt(x, z, dhdx, dhdz);

            // The surface is y = h(x,z), so its normal is (-dh/dx, 1, -dh/dz).
            XMVECTOR nrm = XMVector3Normalize(XMVectorSet(-dhdx, 1.0f, -dhdz, 0.0f));
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
    UINT m_updateInterval = 1;   // recompute every n-th frame
    UINT m_updateCounter = 0;
    UINT m_currentSlot = 0;      // last-written buffer; VBV always points here
    int  m_gridN = 48;
    int  m_requestedGridN = 48;   // survives Shutdown(); applied on Initialize
    bool m_oceanProfile = false;  // broad swells; survives Shutdown()
    OceanWaveSettings m_oceanSettings =
        OceanWaveSettings::CalmTropical();

    // ---- shared ----
    XMFLOAT3 m_center{};
    XMFLOAT3 m_extents{};
    float m_surfaceY = 0.0f;
    float m_time = 0.0f;
};
