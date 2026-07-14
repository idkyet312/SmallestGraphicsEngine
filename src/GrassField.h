#pragma once

// Wind-blown grass over the island.
//
// Grass is the cheap half of what makes a jungle read as a jungle: thousands of
// small blades, all leaning together as gusts roll across the terrain. Two
// decisions shape this file.
//
// WHERE THE BLADES GO is decided once, at build time. A blade needs flat-ish
// ground that is above the waterline and off the beach, so each candidate point
// is tested against the terrain sampler -- its height, and its slope (from
// finite differences of that same sampler) -- and rejected if it would grow out
// of the sea, out of the sand, or off a cliff. Survivors keep a random yaw,
// height, lean and phase, so the field never looks stamped from one blade.
//
// HOW THEY MOVE is recomputed every frame on the CPU, into a ring of UPLOAD
// buffers -- exactly what WaterVolume already does for its wave surface. Wind is
// a travelling wave sampled at the blade's own position, so gusts sweep ACROSS
// the field rather than every blade swaying in unison, and the bend is applied
// only to the blade's upper vertices, hinged at the root, so grass bends like
// grass instead of sliding like a decal.
//
// The blades are ordinary triangles in the ordinary object shader -- no new PSO,
// no new vertex shader, and the shared pipeline already rasterises two-sided
// (CULL_MODE_NONE), which is what a flat blade card needs.

#include <DirectXMath.h>
#include "DX12Core.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

using namespace DirectX;

class GrassField {
public:
    // Same layout as the forward renderer's VertexPosNormUV, so grass draws with
    // the ordinary object shader.
    struct GrassVertex {
        XMFLOAT3 position;
        XMFLOAT3 normal;
        XMFLOAT2 texCoord;
        XMFLOAT4 tangent = XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f);
    };

    // Scatter blades across a square of terrain centred on the origin.
    // `sampler` returns terrain height at (x, z); `waterY` is the sea level that
    // blades must stay above.
    void Initialize(std::function<float(float, float)> sampler,
                    float span = 90.0f, int count = 12000, float waterY = 0.0f) {
        Shutdown();
        if (!sampler) return;
        m_terrain = std::move(sampler);
        m_waterY = waterY;

        BuildBlades(span, count);
        if (m_blades.empty()) return;
        if (!BuildBuffers()) { Shutdown(); return; }
        m_ready = true;

        // stdout stays buffered while the app runs, so record what actually got
        // planted -- the reject rate is the only way to tell a bad scatter from a
        // bad span without seeing the screen.
        if (FILE* f = std::fopen("grass_load.log", "w")) {
            std::fprintf(f, "planted=%zu of %d candidates (span=%.1f) verts=%zu tris=%zu\n",
                         m_blades.size(), count, span,
                         m_blades.size() * kVertsPerBlade,
                         m_indices.size() / 3);
            std::fclose(f);
        }
    }

    void Update(float dt) { m_time += dt; }

    // Where the camera is, so distant blades can be skipped. Set before drawing.
    void SetViewer(const XMFLOAT3& eye) { m_eye = eye; }

    // Rewrite this frame's vertices with the current wind, and hand back the view.
    const D3D12_VERTEX_BUFFER_VIEW& UpdateAndGetVBV(UINT frameIndex) {
        const UINT f = frameIndex % kFrames;
        if (m_ready) WriteBlades(f);
        return m_vbv[f];
    }
    const D3D12_INDEX_BUFFER_VIEW& GetIBV() const { return m_ibv; }
    // Only the blades that survived the distance cull are drawn -- see WriteBlades.
    UINT GetIndexCount() const { return m_drawIndices; }
    bool IsInitialized() const { return m_ready; }

    // Wind controls, surfaced to the UI.
    float& WindStrength() { return m_windStrength; }
    float& WindSpeed()    { return m_windSpeed; }

    void Shutdown() {
        for (UINT f = 0; f < kFrames; ++f) {
            if (m_vb[f] && m_mapped[f]) m_vb[f]->Unmap(0, nullptr);
            m_vb[f].Reset();
            m_mapped[f] = nullptr;
        }
        m_ib.Reset();
        m_blades.clear();
        m_indices.clear();
        m_ready = false;
        m_time = 0.0f;
    }

    ~GrassField() { Shutdown(); }

private:
    static constexpr UINT kFrames = 3;         // frames in flight
    static constexpr int  kSegments = 3;       // vertical segments per blade
    // A blade is a strip: (kSegments + 1) rows of 2 vertices, tapering to a point.
    static constexpr int  kVertsPerBlade = (kSegments + 1) * 2;
    static constexpr int  kIndicesPerBlade = kSegments * 6;

    // One blade, as decided at build time. Everything here is constant; only the
    // wind bend applied on top of it changes per frame.
    struct Blade {
        float x = 0.0f, z = 0.0f, baseY = 0.0f;
        float height = 0.5f;
        float width = 0.04f;
        float dirX = 1.0f, dirZ = 0.0f;   // the blade's facing (its width axis)
        float phase = 0.0f;               // per-blade offset so gusts desynchronise
        float lean = 0.0f;                // resting lean, so the field isn't a lawn
        float leanX = 0.0f, leanZ = 0.0f; // which way that resting lean points
        float tint = 1.0f;                // per-blade shade, via vertex UV.y
    };

    // Cheap deterministic hash -> [0, 1). A fixed seed keeps the field identical
    // across runs, which matters: the blades are baked once and never re-scattered.
    static float Rand(uint32_t& s) {
        s = s * 1664525u + 1013904223u;
        return (float)((s >> 8) & 0xFFFFFF) / (float)0x1000000;
    }

    // Can a tuft grow here? Rejects the sea, the wet sand at the waterline, and
    // any face too steep to hold grass (slope from a central difference on the
    // same sampler the terrain is drawn from, so the test matches what you see).
    bool Plantable(float x, float z) const {
        const float y = m_terrain(x, z);
        if (y < m_waterY + kShoreMargin) return false;

        constexpr float e = 0.5f;
        const float dx = (m_terrain(x + e, z) - m_terrain(x - e, z)) / (2.0f * e);
        const float dz = (m_terrain(x, z + e) - m_terrain(x, z - e)) / (2.0f * e);
        return std::sqrt(dx * dx + dz * dz) <= kMaxSlope;
    }

    void BuildBlades(float span, int count) {
        uint32_t seed = 0x9E3779B9u;
        const float half = span * 0.5f;
        m_blades.reserve(count);

        // Blades come in TUFTS, not as an even scatter. An even scatter of thin
        // blades reads as sparse weeds on bare dirt no matter how many you throw
        // at it -- the eye sees the gaps. Clumping the same budget into tufts
        // fills those gaps: each clump is dense enough to hide the ground under
        // it, and the bare earth between clumps then reads as intentional.
        const int tufts = std::max(1, count / kBladesPerTuft);

        for (int t = 0; t < tufts; ++t) {
            const float cx = (Rand(seed) * 2.0f - 1.0f) * half;
            const float cz = (Rand(seed) * 2.0f - 1.0f) * half;

            // Test the CLUMP's centre once, not every blade: if the middle of the
            // tuft is in the sea or on a cliff, the whole tuft is rejected.
            if (!Plantable(cx, cz)) continue;

            for (int i = 0; i < kBladesPerTuft; ++i) {
                // Blades scatter around the tuft centre, densest in the middle.
                const float a = Rand(seed) * XM_2PI;
                const float r = kTuftRadius * std::sqrt(Rand(seed));
                const float x = cx + std::cos(a) * r;
                const float z = cz + std::sin(a) * r;
                const float y = m_terrain(x, z);
                if (y < m_waterY + kShoreMargin) continue;

                Blade b;
                b.x = x;
                b.z = z;
                b.baseY = y;
                b.height = 0.38f + Rand(seed) * 0.42f;
                // Narrow: at 3-5 cm the blades were reading as floppy leaves rather
                // than grass. Real blades are thin, and thinness is most of what
                // sells a field as grass at a distance.
                b.width  = 0.012f + Rand(seed) * 0.010f;

                const float yaw = Rand(seed) * XM_2PI;
                b.dirX = std::cos(yaw);
                b.dirZ = std::sin(yaw);

                // Only a small per-blade phase jitter. The gust itself is sampled
                // at the blade's position, so a tuft's blades already move nearly
                // together; a full random phase here would shake each blade
                // independently and destroy that, leaving a field that shimmers
                // instead of one the wind moves through.
                b.phase = (Rand(seed) - 0.5f) * 0.6f;

                // A resting lean, biased outward from the tuft centre so a clump
                // fans open like a real tussock instead of standing as a bundle of
                // parallel spikes.
                const float leanYaw = a + (Rand(seed) - 0.5f) * 1.2f;
                b.lean  = 0.05f + Rand(seed) * 0.20f;
                b.leanX = std::cos(leanYaw);
                b.leanZ = std::sin(leanYaw);

                b.tint = 0.75f + Rand(seed) * 0.5f;

                m_blades.push_back(b);
            }
        }

        // The index buffer is static: blade topology never changes, only the
        // vertex positions do.
        m_indices.clear();
        m_indices.reserve(m_blades.size() * kIndicesPerBlade);
        for (size_t b = 0; b < m_blades.size(); ++b) {
            const uint32_t base = (uint32_t)(b * kVertsPerBlade);
            for (int s = 0; s < kSegments; ++s) {
                const uint32_t r0 = base + s * 2;      // this row: left, right
                const uint32_t r1 = base + (s + 1) * 2; // row above
                m_indices.push_back(r0);     m_indices.push_back(r1);     m_indices.push_back(r0 + 1);
                m_indices.push_back(r0 + 1); m_indices.push_back(r1);     m_indices.push_back(r1 + 1);
            }
        }
    }

    bool BuildBuffers() {
        const UINT vbSize = (UINT)(m_blades.size() * kVertsPerBlade * sizeof(GrassVertex));
        for (UINT f = 0; f < kFrames; ++f) {
            if (!MakeUploadBuffer(vbSize, m_vb[f], m_mapped[f])) return false;
            m_vbv[f].BufferLocation = m_vb[f]->GetGPUVirtualAddress();
            m_vbv[f].SizeInBytes = vbSize;
            m_vbv[f].StrideInBytes = sizeof(GrassVertex);
        }

        const UINT ibSize = (UINT)(m_indices.size() * sizeof(uint32_t));
        void* ibMapped = nullptr;
        if (!MakeUploadBuffer(ibSize, m_ib, ibMapped)) return false;
        std::memcpy(ibMapped, m_indices.data(), ibSize);
        m_ib->Unmap(0, nullptr);
        m_ibv.BufferLocation = m_ib->GetGPUVirtualAddress();
        m_ibv.SizeInBytes = ibSize;
        m_ibv.Format = DXGI_FORMAT_R32_UINT;
        return true;
    }

    static bool MakeUploadBuffer(UINT size, Microsoft::WRL::ComPtr<ID3D12Resource>& buffer,
                                 void*& mapped) {
        if (size == 0) return false;
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

    // Wind at a point: two travelling waves crossing the field at an angle, so
    // gusts sweep over the grass instead of pulsing everywhere at once. Returns
    // a signed bend amount; the caller decides which way it pushes.
    float WindAt(float x, float z, float phase) const {
        const float t = m_time * m_windSpeed;
        const float a = std::sin((x + z) * 0.18f + t + phase);
        const float b = std::sin((x * 0.31f - z * 0.13f) + t * 0.63f);
        // Bias positive: real wind blows one way and gusts on top of that, rather
        // than swinging symmetrically back and forth.
        return (0.55f + 0.45f * a) * (0.7f + 0.3f * b);
    }

    void WriteBlades(UINT f) {
        if (!m_mapped[f]) return;
        GrassVertex* out = (GrassVertex*)m_mapped[f];

        // The prevailing wind direction, slowly wandering so the field never
        // settles into an obviously repeating pattern.
        const float dirAng = std::sin(m_time * 0.07f) * 0.5f;
        const float wx = std::cos(dirAng);
        const float wz = std::sin(dirAng);

        // Every blade written here is re-simulated on the CPU, so writing all of
        // them costs the same whether they are underfoot or 80 m away and a
        // fraction of a pixel tall. Rebuilding the whole field was ~11 ms/frame --
        // more than the entire rest of the scene. So only blades within
        // kDrawDistance are written, COMPACTED to the front of the buffer: because
        // every blade has identical topology, the static index buffer already
        // describes them, and the draw simply stops after however many were kept.
        const float cullSq = kDrawDistance * kDrawDistance;

        size_t written = 0;
        size_t v = 0;
        for (const Blade& b : m_blades) {
            const float ddx = b.x - m_eye.x;
            const float ddz = b.z - m_eye.z;
            const float distSq = ddx * ddx + ddz * ddz;
            if (distSq > cullSq) continue;
            ++written;

            // Cutting blades off at a hard radius makes a ring of grass pop in and
            // out as you walk. Instead, shrink them to nothing over the last few
            // metres before the cull, so they sink into the ground rather than
            // blinking out of existence.
            float fade = 1.0f;
            const float fadeStart = kDrawDistance - kFadeBand;
            if (distSq > fadeStart * fadeStart) {
                const float d = std::sqrt(distSq);
                fade = std::max(0.0f, (kDrawDistance - d) / kFadeBand);
            }
            const float height = b.height * fade;

            const float bend = WindAt(b.x, b.z, b.phase) * m_windStrength;

            // Total horizontal displacement of the blade TIP: its resting lean,
            // plus the wind pushing along the prevailing direction. Expressed as a
            // FRACTION of the blade's height, so it must stay below 1 -- a tip that
            // travels further than the blade is long has nowhere to bend to, and
            // the droop term below would fold the blade through its own root.
            float tipX = b.leanX * b.lean + wx * bend;
            float tipZ = b.leanZ * b.lean + wz * bend;
            const float tipLen = std::sqrt(tipX * tipX + tipZ * tipZ);
            if (tipLen > kMaxBend) {
                const float s = kMaxBend / tipLen;
                tipX *= s;
                tipZ *= s;
            }

            for (int s = 0; s <= kSegments; ++s) {
                const float t = (float)s / kSegments;      // 0 at root, 1 at tip

                // Hinge at the root: displacement grows with t^2, so the base stays
                // planted and the blade curves over rather than shearing rigidly.
                const float k = t * t;
                const float offX = tipX * k;
                const float offZ = tipZ * k;

                // Bending shortens the blade's vertical reach -- without this the
                // grass stretches as it leans.
                const float droop = std::sqrt(std::max(0.0f, 1.0f - (offX * offX + offZ * offZ)));
                const float y = b.baseY + height * t * droop;

                // Taper to a point at the tip.
                const float w = b.width * (1.0f - t * 0.85f);

                const float cx = b.x + offX;
                const float cz = b.z + offZ;

                // The blade's surface normal: perpendicular to both its width axis
                // and the direction it is currently leaning, so lighting responds
                // to the wind instead of staying flat.
                XMVECTOR side = XMVectorSet(b.dirX, 0.0f, b.dirZ, 0.0f);
                XMVECTOR up   = XMVector3Normalize(XMVectorSet(tipX, 1.0f, tipZ, 0.0f));
                XMVECTOR nrm  = XMVector3Cross(side, up);
                if (XMVectorGetX(XMVector3LengthSq(nrm)) < 1e-6f)
                    nrm = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                nrm = XMVector3Normalize(nrm);
                // Grass reads better lit from above: tilt the normal skyward so a
                // field of vertical cards doesn't go black under a high sun.
                nrm = XMVector3Normalize(XMVectorAdd(nrm, XMVectorSet(0.0f, 0.9f, 0.0f, 0.0f)));
                XMFLOAT3 nf; XMStoreFloat3(&nf, nrm);

                XMFLOAT3 tf(b.dirX, 0.0f, b.dirZ);

                for (int e = 0; e < 2; ++e) {          // left, right edge
                    const float sgn = e ? 1.0f : -1.0f;
                    GrassVertex& gv = out[v++];
                    gv.position = XMFLOAT3(cx + b.dirX * w * sgn, y, cz + b.dirZ * w * sgn);
                    gv.normal = nf;
                    // u across the blade, v up it. The shader has no grass texture,
                    // so v doubles as the shading gradient (dark at the root,
                    // bright at the tip) and the blade's own tint rides in on it.
                    gv.texCoord = XMFLOAT2(e ? 1.0f : 0.0f, t * b.tint);
                    gv.tangent = XMFLOAT4(tf.x, tf.y, tf.z, 1.0f);
                }
            }
        }

        m_drawIndices = (UINT)(written * kIndicesPerBlade);
    }

    // A blade must clear the waterline by this much to be planted, keeping grass
    // off the wet sand.
    static constexpr float kShoreMargin = 0.9f;
    // Steepest ground grass will grow on (rise over run).
    static constexpr float kMaxSlope = 0.6f;
    // How far the tip may travel sideways, as a fraction of the blade's height.
    // Past ~0.8 a blade is bent nearly flat and starts to look broken.
    static constexpr float kMaxBend = 0.75f;
    // Blades are clumped into tufts rather than scattered evenly -- see BuildBlades.
    static constexpr int   kBladesPerTuft = 14;
    static constexpr float kTuftRadius    = 0.28f;   // metres
    // Blades beyond this are neither simulated nor drawn. At this range a blade is
    // a sliver a pixel or two tall, and paying full CPU wind for it is pure waste:
    // the whole field costs ~11 ms/frame to rebuild, so what this radius excludes
    // is most of the frame budget.
    static constexpr float kDrawDistance  = 28.0f;
    // Blades shrink to nothing over the last few metres, so the cull edge does not
    // read as a ring of grass popping in and out as the player walks.
    static constexpr float kFadeBand      = 6.0f;

    std::function<float(float, float)> m_terrain;
    std::vector<Blade>    m_blades;
    std::vector<uint32_t> m_indices;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_vb[kFrames];
    void*                                  m_mapped[kFrames] = {};
    D3D12_VERTEX_BUFFER_VIEW               m_vbv[kFrames] = {};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ib;
    D3D12_INDEX_BUFFER_VIEW                m_ibv = {};

    XMFLOAT3 m_eye{};          // camera position, for the distance cull
    UINT     m_drawIndices = 0; // indices actually written this frame

    float m_time = 0.0f;
    float m_waterY = 0.0f;
    float m_windStrength = 0.28f;
    float m_windSpeed = 1.6f;
    bool  m_ready = false;
};

extern GrassField g_grass;
