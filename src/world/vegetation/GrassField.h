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
// HOW THEY MOVE happens entirely in grass_vs.hlsl, on GPU-instanced geometry.
// Everything here is STATIC: written once at load, never touched again.
//
// The field is ONE blade -- 8 vertices, 18 indices -- drawn once per blade with
// DrawIndexedInstanced. What makes a blade its own blade (root, height, facing,
// lean, phase) lives in a structured buffer the vertex shader indexes by
// SV_InstanceID, and the shader applies the wind, the bend and the distance fade.
//
// It took three versions to get here, and the two dead ends are worth recording:
//
//   1. Simulating every near-camera blade in scalar C++ each frame and streaming
//      the result into an upload buffer -- the same trick WaterVolume uses for its
//      waves. It cost ~11 ms/frame, more than the entire rest of the scene put
//      together (161 -> 58 FPS). No amount of culling fixed the shape of that: the
//      work was per-blade, per-frame, on the wrong processor.
//
//   2. Moving the wind to the vertex shader, but keeping a unique vertex per blade
//      vertex. That made the geometry static, but left 1.4M vertices (~60 MB) to
//      fetch, and drawing the whole field while letting the shader shrink distant
//      blades to nothing was SLOWER than the CPU version it replaced -- a
//      zero-height blade still costs vertex shading and triangle setup.
//
// Hence both halves of what is here now: instancing (so a blade's data is read
// once, not eight times) and cell culling (so distant grass is never submitted at
// all, rather than merely being invisible).
//
// The blades draw with the ordinary pixel shader through a grass-specific PSO that
// swaps in the wind vertex shader. The shared pipeline already rasterises
// two-sided (CULL_MODE_NONE), which is what a flat blade card needs.

#include <DirectXMath.h>
#include "DX12Core.h"
#include "StaticBufferDX12.h"
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
    struct Exclusion {
        float minX = 0.0f;
        float minZ = 0.0f;
        float maxX = 0.0f;
        float maxZ = 0.0f;
        // Optional circular test. Zero keeps the plain rectangle used by
        // building footprints; a positive radius makes the rect a bounding box
        // and the circle the real shape, so a painted brush clears a disc
        // instead of the square around it.
        float centerX = 0.0f;
        float centerZ = 0.0f;
        float radius = 0.0f;
    };

    struct AuthoredPatch {
        float x = 0.0f;
        float z = 0.0f;
        float radius = 1.0f;
        float density = 1.0f;
        uint32_t seed = 1;
    };

    // Authored blade template extracted from models/grass2/grass/allGrass_001.obj.
    // Shape stores height, edge, forward curvature, and width profile.
    struct GrassVertex {
        XMFLOAT4 shape; // (t, side, forward offset / height, width / root width)
    };

    // Everything that makes one blade different from another. The vertex shader
    // reads this by SV_InstanceID; the layout must match grass_vs.hlsl's
    // BladeInstance exactly, padded to a 16-byte multiple.
    struct BladeInstance {
        XMFLOAT3 root;     // world position of the base
        float    height;
        XMFLOAT2 dir;      // facing (width axis)
        XMFLOAT2 lean;     // resting lean, as a tip offset in blade-height units
        float    width;
        float    phase;
        XMFLOAT2 pad{};
    };
    static_assert(sizeof(BladeInstance) == 48, "must match the HLSL BladeInstance");

    // The level's painted terrain splatmap, so scatter follows what the artist
    // painted instead of only sine waves and slope.
    //
    // The map is the same RGBA the terrain resolve samples: channels are
    // (grass, dirt, sand, rock) and an untouched texel is (0,0,0,0). That zero
    // is what lets the procedural terrain weights pass through unchanged. Where
    // the artist HAS painted, those weights blend toward the authored channels:
    // paint rock and foliage stops growing there.
    //
    // Set before Initialize; the scatter is baked once, so repainting requires
    // an environment rebuild, exactly as moving a building exclusion does.
    void SetSplatMap(const uint8_t* rgba, uint32_t resolution,
                     float halfExtentX, float halfExtentZ) {
        m_splat.clear();
        m_splatResolution = 0;
        if (!rgba || resolution == 0 ||
            halfExtentX <= 0.0f || halfExtentZ <= 0.0f)
            return;
        m_splat.assign(rgba, rgba + static_cast<size_t>(resolution) *
                                        resolution * 4u);
        m_splatResolution = resolution;
        m_splatHalfExtentX = halfExtentX;
        m_splatHalfExtentZ = halfExtentZ;
    }

    // Scatter blades across a square of terrain centred on the origin.
    // `sampler` returns terrain height at (x, z); `waterY` is the sea level that
    // blades must stay above.
    // `blocked`, when set, rejects a tuft the rectangle list cannot describe --
    // a prefab whose real collision is a triangle mesh. Its bounds box covers
    // the whole footprint including the open ground inside it, so excluding by
    // box strips grass from an entire apron; this asks the mesh itself instead.
    // Called per tuft centre during the build only, never per frame.
    void Initialize(std::function<float(float, float)> sampler,
                    float span = 90.0f, int count = 12000, float waterY = 0.0f,
                    const std::vector<Exclusion>& exclusions = {},
                    const std::vector<AuthoredPatch>& authoredPatches = {},
                    bool showAuthoredPaths = false,
                    std::function<bool(float, float)> blocked = {}) {
        Shutdown();
        if (!sampler) return;
        m_terrain = std::move(sampler);
        m_waterY = waterY;
        m_exclusions = exclusions;
        m_authoredPatches = authoredPatches;
        m_showAuthoredPaths = showAuthoredPaths;
        m_blocked = std::move(blocked);

        BuildBlades(span, count);
        if (m_blades.empty()) return;
        if (!BuildBuffers()) { Shutdown(); return; }
        m_ready = true;

        // stdout stays buffered while the app runs, so record what actually got
        // planted -- the reject rate is the only way to tell a bad scatter from a
        // bad span without seeing the screen.
        if (FILE* f = std::fopen("grass_load.log", "w")) {
            std::fprintf(f, "planted=%zu of %d candidates (span=%.1f) cells=%zu "
                            "templateVerts=%d instanceBytes=%zu\n",
                         m_blades.size(), count, span, m_cells.size(),
                         kVertsPerBlade,
                         m_blades.size() * sizeof(BladeInstance));
            std::fclose(f);
        }
    }

    void Update(float dt) {
        m_time += dt;

        const XMFLOAT2 player(m_playerPosition.x, m_playerPosition.z);
        if (!PlayerPushActive()) {
            m_playerTrailPosition = player;
            m_playerTrailInitialized = false;
            return;
        }
        if (!m_playerTrailInitialized) {
            m_playerTrailPosition = player;
            m_playerTrailInitialized = true;
            return;
        }

        // A lagging interaction point turns the circular footprint into a short
        // swept capsule. When movement stops it catches up exponentially, so the
        // wake closes smoothly instead of every passed blade snapping upright.
        const float follow = 1.0f - std::exp(
            -(std::max)(dt, 0.0f) * 4.0f);
        m_playerTrailPosition.x +=
            (player.x - m_playerTrailPosition.x) * follow;
        m_playerTrailPosition.y +=
            (player.y - m_playerTrailPosition.y) * follow;

        // A hitch or teleport must not leave a field-wide interaction segment.
        const float dx = player.x - m_playerTrailPosition.x;
        const float dz = player.y - m_playerTrailPosition.y;
        const float distance = std::sqrt(dx * dx + dz * dz);
        const float maxTrail = (std::max)(m_playerPushRadius * 0.85f, 0.25f);
        if (distance > maxTrail) {
            const float scale = maxTrail / distance;
            m_playerTrailPosition.x = player.x - dx * scale;
            m_playerTrailPosition.y = player.y - dz * scale;
        }
    }

    // Where the camera is. Feeds the shader's distance fade and the cell cull.
    void SetViewer(const XMFLOAT3& eye) { m_eye = eye; }

    // The blade TEMPLATE: 8 verts, 18 indices, shared by every blade in the field.
    // Everything that makes a blade individual lives in the instance buffer below.
    const D3D12_VERTEX_BUFFER_VIEW& GetVBV() const { return m_vbv; }
    const D3D12_INDEX_BUFFER_VIEW& GetIBV() const { return m_ibv; }
    static constexpr UINT IndexCount() { return kIndicesPerBlade; }

    // The per-blade data, as a root SRV (t6) the vertex shader indexes with
    // SV_InstanceID.
    D3D12_GPU_VIRTUAL_ADDRESS GetInstanceBufferAddress() const {
        return m_instances ? m_instances->GetGPUVirtualAddress() : 0;
    }

    // One INSTANCE range per patch of field near the camera. The renderer issues a
    // draw per entry, and distant patches are simply never submitted. This is a few
    // dozen tests over CELLS, not a walk over every blade, so the per-frame CPU cost
    // is negligible -- which is the whole point.
    struct DrawRange { UINT firstInstance, instanceCount; };
    void GetVisible(std::vector<DrawRange>& out) const {
        out.clear();
        // A cell is drawn if any blade in it could be inside the draw radius, so
        // test the cell's centre against the radius plus the cell's corner reach.
        const float reach = m_drawDistance + kCellSize * 0.7072f;   // half-diagonal
        const float reachSq = reach * reach;
        for (const Cell& c : m_cells) {
            const float dx = c.cx - m_eye.x;
            const float dz = c.cz - m_eye.z;
            if (dx * dx + dz * dz > reachSq) continue;
            if (RuntimeCellExcluded(c.cx, c.cz)) continue;
            // Density trims each cell's contiguous instance run. Blades are
            // stored tuft-contiguous, so a fractional cut drops whole tufts and
            // reads as natural thinning -- no buffer rebuild, safe to change
            // while frames are in flight.
            const UINT count = (std::max)(1u,
                (UINT)((float)c.bladeCount * m_density));
            out.push_back({ c.firstBlade, count });
        }
    }

    void AddRuntimeExclusion(float x, float z, float radius) {
        if (radius <= 0.0f) return;
        m_runtimeExclusions.push_back({ x, z, radius });
    }

    void ClearRuntimeExclusions() { m_runtimeExclusions.clear(); }

    bool RuntimeExcluded(float x, float z, float padding = 0.0f) const {
        for (const RuntimeExclusion& exclusion : m_runtimeExclusions) {
            const float dx = x - exclusion.x;
            const float dz = z - exclusion.z;
            const float reach = exclusion.radius + padding;
            if (dx * dx + dz * dz <= reach * reach) return true;
        }
        return false;
    }

    // The 19 root constants (b6) the grass vertex shader reads. Laid out to match
    // grass_vs.hlsl's GrassParams exactly.
    struct Params {
        float time;
        float windStrength;
        float windSpeed;
        float eyeX;
        float eyeZ;
        float drawDistance;
        float fadeBand;
        // The patch's first blade. SV_InstanceID does not pick up
        // StartInstanceLocation for a structured buffer, so the shader adds this
        // itself -- see grass_vs.hlsl. Set per draw.
        UINT  firstBlade;
        float helicopterX;
        float helicopterZ;
        float helicopterWindRadius;
        float helicopterWindStrength;
        // World-space height of one screen pixel per unit of view distance.
        // Keeps distant moving blades rasterizable instead of subpixel flicker.
        float pixelWorldScale;
        // The player, as a wash source that shoves blades radially outward.
        // Kept separate from the helicopter slot: the two overlap constantly
        // (the player runs under the insertion bird), and folding them into one
        // source would make whichever is nearer snap the field back and forth.
        float playerX;
        float playerZ;
        // Tip offset, in blade-height units, applied at the player's feet.
        // Zero disables the push, which is how the CPU switches it off while
        // the player is airborne or standing above the field.
        float playerPushStrength;
        float playerPushRadius;
        // The trailing end of the swept interaction capsule. It follows the
        // player with a short delay so passed vegetation recovers gradually.
        float playerTrailX;
        float playerTrailZ;
    };
    Params GetParams(float verticalFovDegrees, float viewportHeight) const {
        Params p;
        p.time = m_time;
        p.windStrength = m_windStrength;
        p.windSpeed = m_windSpeed;
        p.eyeX = m_eye.x;
        p.eyeZ = m_eye.z;
        p.drawDistance = m_drawDistance;
        p.fadeBand = m_fadeBand;
        p.firstBlade = 0;      // the renderer overwrites this per patch
        p.helicopterX = m_helicopterPosition.x;
        p.helicopterZ = m_helicopterPosition.z;
        p.helicopterWindRadius = m_helicopterWindRadius;
        p.helicopterWindStrength = m_helicopterWindStrength;
        p.pixelWorldScale = 2.0f * std::tan(
            XMConvertToRadians(verticalFovDegrees) * 0.5f) /
            (std::max)(viewportHeight, 1.0f);
        p.playerX = m_playerPosition.x;
        p.playerZ = m_playerPosition.z;
        p.playerPushStrength = PlayerPushActive() ? m_playerPushStrength : 0.0f;
        p.playerPushRadius = m_playerPushRadius;
        p.playerTrailX = m_playerTrailInitialized
            ? m_playerTrailPosition.x : m_playerPosition.x;
        p.playerTrailZ = m_playerTrailInitialized
            ? m_playerTrailPosition.y : m_playerPosition.z;
        return p;
    }
    bool IsInitialized() const { return m_ready; }

    // Density is non-zero only where grass is the dominant rendered terrain
    // layer. Dandelions share this query so every kind of ground foliage obeys
    // the same terrain-material boundary as the blade scatter.
    float TerrainGrassDensity(float x, float z) const {
        return m_terrain ? GrassTerrainDensity(x, z) : 0.0f;
    }

    // Wind controls, surfaced to the UI.
    float& WindStrength() { return m_windStrength; }
    float& WindSpeed()    { return m_windSpeed; }
    void SetHelicopterWind(const XMFLOAT3& position, bool enabled) {
        m_helicopterPosition = position;
        m_helicopterWindStrength = enabled ? 1.8f : 0.0f;
    }
    // Blades near the player bend away from them. Pass the player's FEET, not
    // the eye: the push is a ground-plane effect and the eye sits a body-height
    // above the blades being parted. `enabled` is the caller's grounded test: a
    // player mid-jump or on a rooftop is not touching the field under them.
    void SetPlayerPush(const XMFLOAT3& feetPosition, bool enabled) {
        m_playerPosition = feetPosition;
        m_playerPushActive = enabled;
    }
    // Interaction toggle and push shape, surfaced alongside the wind controls.
    bool& PlayerPushEnabled()    { return m_playerPushEnabled; }
    float& PlayerPushRadius()   { return m_playerPushRadius; }
    float& PlayerPushStrength() { return m_playerPushStrength; }

    // The same push the grass shaders apply, evaluated on the CPU so the other
    // ground foliage can lean with it. Dandelions are single instanced models
    // whose only per-plant data is a world matrix, so they bend by tilting that
    // matrix rather than by a vertex shader of their own -- and this is the one
    // definition both paths read, so they cannot drift apart.
    //
    // Returns a tip offset in PLANT-HEIGHT units, pointing away from the player.
    // Mirrors PlayerPush in grass_vs.hlsl.
    XMFLOAT2 PlayerPushAt(float x, float z) const {
        if (!PlayerPushActive())
            return XMFLOAT2(0.0f, 0.0f);
        const float trailX = m_playerTrailInitialized
            ? m_playerTrailPosition.x : m_playerPosition.x;
        const float trailZ = m_playerTrailInitialized
            ? m_playerTrailPosition.y : m_playerPosition.z;
        const float segmentX = trailX - m_playerPosition.x;
        const float segmentZ = trailZ - m_playerPosition.z;
        const float segmentLengthSq =
            segmentX * segmentX + segmentZ * segmentZ;
        const float fromPlayerX = x - m_playerPosition.x;
        const float fromPlayerZ = z - m_playerPosition.z;
        const float along = segmentLengthSq > 1e-6f
            ? std::clamp((fromPlayerX * segmentX + fromPlayerZ * segmentZ) /
                         segmentLengthSq, 0.0f, 1.0f)
            : 0.0f;
        const float nearestX = m_playerPosition.x + segmentX * along;
        const float nearestZ = m_playerPosition.z + segmentZ * along;
        const float ax = x - nearestX;
        const float az = z - nearestZ;
        const float distance = std::sqrt(ax * ax + az * az);
        const float radius = (std::max)(m_playerPushRadius, 1e-3f);
        if (distance >= radius) return XMFLOAT2(0.0f, 0.0f);
        const float contact = 1.0f - distance / radius;
        const float falloff = contact * contact * (3.0f - 2.0f * contact);
        // Convert authored force to a bounded bend. This keeps strong settings
        // from flattening a large, perfectly uniform disc of vegetation.
        const float force = falloff * m_playerPushStrength;
        const float scale = force / (0.5f + force);
        if (distance <= 1e-3f) {
            if (segmentLengthSq <= 1e-6f) return XMFLOAT2(scale, 0.0f);
            const float inverseLength = 1.0f / std::sqrt(segmentLengthSq);
            return XMFLOAT2(-segmentZ * inverseLength * scale,
                            segmentX * inverseLength * scale);
        }
        return XMFLOAT2(ax / distance * scale, az / distance * scale);
    }
    // True while the push is doing anything at all, so callers can skip the
    // per-instance work entirely on the frames it cannot matter.
    bool PlayerPushActive() const {
        return m_playerPushEnabled && m_playerPushActive &&
               m_playerPushStrength > 0.0f;
    }
    const XMFLOAT3& PlayerPushPosition() const { return m_playerPosition; }
    float PlayerPushRadiusValue() const { return m_playerPushRadius; }
    // Perf controls, surfaced to the UI. Density trims instances per cell;
    // draw distance feeds both the cell cull and the shader's fade.
    float& Density()      { return m_density; }
    float& DrawDistance() { return m_drawDistance; }
    // Material controls. These feed the grass-specific pixel shader every draw,
    // so editor changes are immediate and require no blade-buffer rebuild.
    XMFLOAT3& Albedo()             { return m_albedo; }
    float& Roughness()             { return m_roughness; }
    float& AmbientScale()          { return m_ambientScale; }
    float& DirectLightScale()      { return m_directLightScale; }
    float& TransmissionStrength()  { return m_transmissionStrength; }
    float& ColorVariation()        { return m_colorVariation; }
    // How strongly a blade's orientation darkens it. 1 is the physical
    // response; 0 lights every blade as though it faced the sun, removing the
    // bright/dark split across the field.
    float& NormalFalloff()         { return m_normalFalloff; }
    void ResetMaterial() {
        m_albedo = XMFLOAT3(0.078431f, 0.078431f, 0.078431f);
        m_roughness = 1.0f;
        m_ambientScale = 1.239f;
        m_directLightScale = 2.0f;
        m_transmissionStrength = 0.776f;
        m_colorVariation = 0.85f;
        m_normalFalloff = 1.0f;
    }
    // Authored curved cards overlap into large repeated silhouettes on nearby
    // vehicles and buildings. Grass receives lighting but never enters CSMs.
    bool CastShadows() const { return false; }
    float& ShadowDensity(){ return m_shadowDensity; }
    size_t PlantedCount() const { return m_blades.size(); }

    void Shutdown() {
        m_vb.Reset();
        m_ib.Reset();
        m_instances.Reset();
        m_blades.clear();
        m_cells.clear();
        m_exclusions.clear();
        m_runtimeExclusions.clear();
        m_ready = false;
        m_time = 0.0f;
    }

    ~GrassField() { Shutdown(); }

private:
    struct RuntimeExclusion {
        float x = 0.0f;
        float z = 0.0f;
        float radius = 0.0f;
    };

    bool RuntimeCellExcluded(float centerX, float centerZ) const {
        constexpr float halfCell = kCellSize * 0.5f;
        for (const RuntimeExclusion& exclusion : m_runtimeExclusions) {
            const float dx = (std::max)(
                std::abs(exclusion.x - centerX) - halfCell, 0.0f);
            const float dz = (std::max)(
                std::abs(exclusion.z - centerZ) - halfCell, 0.0f);
            if (dx * dx + dz * dz <= exclusion.radius * exclusion.radius)
                return true;
        }
        return false;
    }

    static constexpr int  kSegments = 4;       // authored vertical segments
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
    };

    // A square patch of the field, holding a contiguous run of INSTANCES.
    //
    // The blades are static, so the only way to stop paying for the ones you cannot
    // see is to not draw them -- shrinking them to zero height in the vertex shader
    // still costs full vertex shading and triangle setup, which is what made the
    // first GPU version slower than the CPU one it replaced. Sorting blades into
    // cells at build time makes each cell one contiguous instance range, so the
    // field becomes a few dozen instanced draws and the far ones are just skipped.
    struct Cell {
        float  cx = 0.0f, cz = 0.0f;   // centre, world space
        UINT   firstBlade = 0;
        UINT   bladeCount = 0;
    };

    // Cheap deterministic hash -> [0, 1). A fixed seed keeps the field identical
    // across runs, which matters: the blades are baked once and never re-scattered.
    static float Rand(uint32_t& s) {
        s = s * 1664525u + 1013904223u;
        return (float)((s >> 8) & 0xFFFFFF) / (float)0x1000000;
    }

    static float SmoothStep(float edge0, float edge1, float value) {
        const float t = std::clamp(
            (value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // CPU copy of MatVarNoise/TerrainBlendNoise. Foliage is baked at load time,
    // so matching the terrain's deterministic layer noise avoids a texture
    // read or per-frame material test for every blade.
    static uint32_t MatVarHashUint(uint32_t value) {
        value ^= value >> 16;
        value *= 0x7feb352du;
        value ^= value >> 15;
        value *= 0x846ca68bu;
        value ^= value >> 16;
        return value;
    }

    static float MatVarHash(int32_t x, int32_t y, int32_t z) {
        const uint32_t yz = MatVarHashUint(
            static_cast<uint32_t>(y) +
            MatVarHashUint(static_cast<uint32_t>(z)));
        const uint32_t value = MatVarHashUint(
            static_cast<uint32_t>(x) ^ (yz + 0x9e3779b9u));
        return static_cast<float>(value & 0x00ffffffu) *
               (1.0f / 16777216.0f);
    }

    static float MatVarNoise(float x, float y, float z) {
        const int32_t ix = static_cast<int32_t>(std::floor(x));
        const int32_t iy = static_cast<int32_t>(std::floor(y));
        const int32_t iz = static_cast<int32_t>(std::floor(z));
        float fx = x - std::floor(x);
        float fy = y - std::floor(y);
        float fz = z - std::floor(z);
        fx = fx * fx * (3.0f - 2.0f * fx);
        fy = fy * fy * (3.0f - 2.0f * fy);
        fz = fz * fz * (3.0f - 2.0f * fz);
        const float n00 = MatVarHash(ix, iy, iz) +
            (MatVarHash(ix + 1, iy, iz) - MatVarHash(ix, iy, iz)) * fx;
        const float n10 = MatVarHash(ix, iy + 1, iz) +
            (MatVarHash(ix + 1, iy + 1, iz) - MatVarHash(ix, iy + 1, iz)) * fx;
        const float n01 = MatVarHash(ix, iy, iz + 1) +
            (MatVarHash(ix + 1, iy, iz + 1) - MatVarHash(ix, iy, iz + 1)) * fx;
        const float n11 = MatVarHash(ix, iy + 1, iz + 1) +
            (MatVarHash(ix + 1, iy + 1, iz + 1) -
             MatVarHash(ix, iy + 1, iz + 1)) * fx;
        const float nearNoise = n00 + (n10 - n00) * fy;
        const float farNoise = n01 + (n11 - n01) * fy;
        return nearNoise + (farNoise - nearNoise) * fz;
    }

    static float TerrainBlendNoise(float x, float z) {
        const float low = MatVarNoise(x * 0.075f, z * 0.075f, 3.17f);
        const float high = MatVarNoise(x * 0.23f, z * 0.23f, 11.4f);
        return low * 0.72f + high * 0.28f;
    }

    // Bilinearly samples the raw UNORM channels before applying the painted
    // override, matching the clamp sampler and lerp in TerrainVBLayerWeights.
    void ApplyPaintedWeights(float x, float z, float& grass, float& dirt,
                             float& sand, float& rock) const {
        if (m_splatResolution == 0) return;
        const float u = (x / (2.0f * m_splatHalfExtentX) + 0.5f) *
                        m_splatResolution - 0.5f;
        const float v = (z / (2.0f * m_splatHalfExtentZ) + 0.5f) *
                        m_splatResolution - 0.5f;
        const int last = static_cast<int>(m_splatResolution) - 1;
        const int x0 = std::clamp(static_cast<int>(std::floor(u)), 0, last);
        const int z0 = std::clamp(static_cast<int>(std::floor(v)), 0, last);
        const int x1 = std::clamp(x0 + 1, 0, last);
        const int z1 = std::clamp(z0 + 1, 0, last);
        const float fx = std::clamp(u - std::floor(u), 0.0f, 1.0f);
        const float fz = std::clamp(v - std::floor(v), 0.0f, 1.0f);

        const auto channel = [&](int tx, int tz, size_t component) {
            return m_splat[(static_cast<size_t>(tz) * m_splatResolution + tx) *
                           4u + component] * (1.0f / 255.0f);
        };
        const auto sample = [&](size_t component) {
            const float top = channel(x0, z0, component) +
                (channel(x1, z0, component) - channel(x0, z0, component)) * fx;
            const float bottom = channel(x0, z1, component) +
                (channel(x1, z1, component) - channel(x0, z1, component)) * fx;
            return top + (bottom - top) * fz;
        };
        const float paintedGrass = sample(0);
        const float paintedDirt = sample(1);
        const float paintedSand = sample(2);
        const float paintedRock = sample(3);
        const float total = paintedGrass + paintedDirt + paintedSand + paintedRock;
        const float coverage = (std::max)({
            paintedGrass, paintedDirt, paintedSand, paintedRock });
        if (coverage <= kSplatCoverageEpsilon || total <= 0.0f) return;
        const float blend = std::clamp(coverage, 0.0f, 1.0f);
        grass += (paintedGrass / total - grass) * blend;
        dirt += (paintedDirt / total - dirt) * blend;
        sand += (paintedSand / total - sand) * blend;
        rock += (paintedRock / total - rock) * blend;
    }

    // Mirrors TerrainLayerWeights/TerrainVBLayerWeights through the normalized
    // layer weights. Height-blend texture samples are deliberately excluded:
    // they add centimetre-scale interlock inside the same material region and
    // are not a stable place to decide whether a plant can grow.
    float GrassTerrainDensity(float x, float z) const {
        constexpr float e = 0.35f;
        const float y = m_terrain(x, z);
        const float nx = m_terrain(x - e, z) - m_terrain(x + e, z);
        const float ny = 2.0f * e;
        const float nz = m_terrain(x, z - e) - m_terrain(x, z + e);
        const float normalY = ny / std::sqrt(nx * nx + ny * ny + nz * nz);
        const float slope = 1.0f - std::clamp(std::abs(normalY), 0.0f, 1.0f);
        const float noise = TerrainBlendNoise(x, z);
        const float noisyHeight = y + (noise - 0.5f) * 1.5f;

        float rock = SmoothStep(0.30f, 0.68f,
            slope + (noise - 0.5f) * 0.12f);
        const float flat = 1.0f - SmoothStep(0.18f, 0.52f, slope);
        const float beachCore =
            (1.0f - SmoothStep(0.80f, 1.20f, y)) * flat;
        const float beachTransition =
            (1.0f - SmoothStep(0.75f, 2.25f, noisyHeight)) * flat;
        float sand = (std::max)(beachCore, beachTransition);
        float grass = SmoothStep(1.45f, 2.35f, noisyHeight) * flat;
        float dirt = 0.08f + SmoothStep(0.58f, 0.79f, noise) * 0.40f * flat +
                     SmoothStep(0.12f, 0.46f, slope) * 0.32f;

        if (m_showAuthoredPaths) {
            const float axisDistance = (std::min)(std::abs(x), std::abs(z));
            const float pathReach = (std::max)(std::abs(x), std::abs(z));
            float path = (1.0f - SmoothStep(0.72f, 1.28f, axisDistance)) *
                         (1.0f - SmoothStep(13.2f, 15.5f, pathReach));
            path *= 0.82f + TerrainBlendNoise(
                x * 1.8f + 29.0f, z * 1.8f + 29.0f) * 0.18f;
            grass *= 1.0f - path * 0.92f;
            dirt += path * 2.6f;
        }

        sand *= 1.0f - rock;
        dirt *= (1.0f - rock) * (1.0f - sand);
        grass *= (1.0f - rock) * (1.0f - sand);
        grass = std::pow(grass + 0.0001f, 1.35f);
        dirt = std::pow(dirt + 0.0001f, 1.35f);
        sand = std::pow(sand + 0.0001f, 1.35f);
        rock = std::pow(rock + 0.0001f, 1.35f);
        const float total = grass + dirt + sand + rock;
        grass /= total;
        dirt /= total;
        sand /= total;
        rock /= total;
        ApplyPaintedWeights(x, z, grass, dirt, sand, rock);

        const float nonGrass = (std::max)({ dirt, sand, rock });
        if (grass <= nonGrass) return 0.0f;
        // Where grass is the material, plant it fully. The raw weight is a
        // blend fraction against the layers it beat, so ground the terrain
        // draws as solid grass still returned ~0.9 and threw away a tenth of
        // its tufts for nothing -- and near a boundary, far more. Remapping the
        // winning margin to full density keeps the material edges exactly where
        // the terrain shader puts them (a tuft that loses still returns 0) while
        // making the interior actually dense. The 1.6x gain reaches full density
        // a little inside the boundary rather than only at a pure 1.0 weight,
        // which is what leaves a soft fringe instead of a hard mown line.
        return std::clamp((grass - nonGrass) * 1.6f + grass * 0.35f, 0.0f, 1.0f);
    }

    // Can a tuft grow here? Rejects the sea, the wet sand at the waterline, and
    // any face too steep to hold grass (slope from a central difference on the
    // same sampler the terrain is drawn from, so the test matches what you see).
    bool Plantable(float x, float z) const {
        if (Excluded(x, z)) return false;
        if (m_blocked && m_blocked(x, z)) return false;
        const float y = m_terrain(x, z);
        if (y < m_waterY + kShoreMargin) return false;

        constexpr float e = 0.5f;
        const float dx = (m_terrain(x + e, z) - m_terrain(x - e, z)) / (2.0f * e);
        const float dz = (m_terrain(x, z + e) - m_terrain(x, z - e)) / (2.0f * e);
        return std::sqrt(dx * dx + dz * dz) <= kMaxSlope;
    }

    bool Excluded(float x, float z) const {
        for (const Exclusion& exclusion : m_exclusions) {
            // The rect is tested first either way: for a circular exclusion it
            // is the bounding box, so this rejects most blades before the
            // distance test runs.
            if (x < exclusion.minX || x > exclusion.maxX ||
                z < exclusion.minZ || z > exclusion.maxZ)
                continue;
            if (exclusion.radius <= 0.0f) return true;
            const float dx = x - exclusion.centerX;
            const float dz = z - exclusion.centerZ;
            if (dx * dx + dz * dz <= exclusion.radius * exclusion.radius)
                return true;
        }
        return false;
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

            // Keep the playable centre dense, then progressively thin the far
            // edge. Authored patches below intentionally bypass this.
            //
            // Tied to the scatter span rather than a fixed 90 m: on an island
            // long enough to need a wider span (islandv3 reaches +/-121 m on Z)
            // a fixed radius thinned the outer half of the map to 15% even
            // though the splatmap painted grass all the way out. Starting the
            // falloff at 90% of the half-span keeps the original behaviour at
            // the default 100 m span and scales it with anything larger.
            const float radialDistance = std::sqrt(cx * cx + cz * cz);
            const float falloffStart = half * 0.9f;
            const float falloffRange = (std::max)(half * 0.3f, 1.0f);
            const float falloffT = std::clamp(
                (radialDistance - falloffStart) / falloffRange, 0.0f, 1.0f);
            const float smoothFalloff =
                falloffT * falloffT * (3.0f - 2.0f * falloffT);
            const float radialDensity = 1.0f - smoothFalloff * 0.85f;

            // Broad overlapping waves leave irregular open ground between dense
            // stands. This avoids the uniform lawn look without adding a texture
            // lookup or making individual tufts flicker between rebuilds.
            const float densityWaveA = 0.5f + 0.5f *
                std::sin(cx * 0.105f + std::sin(cz * 0.073f) * 1.6f);
            const float densityWaveB = 0.5f + 0.5f *
                std::sin(cz * 0.061f - std::cos(cx * 0.047f) * 1.9f);
            const float densityShape = std::clamp(
                densityWaveA * 0.62f + densityWaveB * 0.38f, 0.0f, 1.0f);
            const float macroDensity = 0.76f + 0.24f *
                (densityShape * densityShape *
                 (3.0f - 2.0f * densityShape));
            const float terrainDensity = GrassTerrainDensity(cx, cz);
            if (Rand(seed) > radialDensity * macroDensity * terrainDensity)
                continue;

            // Test the CLUMP's centre once, not every blade: if the middle of the
            // tuft is in the sea or on a cliff, the whole tuft is rejected.
            if (!Plantable(cx, cz)) continue;

            for (int i = 0; i < kBladesPerTuft; ++i) {
                // Blades scatter around the tuft centre, densest in the middle.
                const float a = Rand(seed) * XM_2PI;
                const float r = kTuftRadius * std::sqrt(Rand(seed));
                const float x = cx + std::cos(a) * r;
                const float z = cz + std::sin(a) * r;
                if (Excluded(x, z)) continue;
                // A tuft is ~0.35 m across, wide enough to straddle a material
                // boundary. Re-testing per blade keeps dirt, sand, and rock
                // clean instead of leaving the whole tuft on the grass side.
                if (Rand(seed) > GrassTerrainDensity(x, z)) continue;
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

                m_blades.push_back(b);
            }
        }

        // Editor-authored patches add deterministic dense tufts without replacing
        // the base island scatter. Patch radius and density come from entity scale.
        for (const AuthoredPatch& patch : m_authoredPatches) {
            uint32_t patchSeed = patch.seed ? patch.seed : 1u;
            const float radius = (std::max)(0.15f, patch.radius);
            const float density = std::clamp(patch.density, 0.05f, 4.0f);
            const int patchTufts = std::clamp(static_cast<int>(
                XM_PI * radius * radius * 2.8f * density), 1, 12000);
            for (int t = 0; t < patchTufts; ++t) {
                const float clusterAngle = Rand(patchSeed) * XM_2PI;
                const float clusterRadius = std::sqrt(Rand(patchSeed)) * radius;
                const float cx = patch.x + std::cos(clusterAngle) * clusterRadius;
                const float cz = patch.z + std::sin(clusterAngle) * clusterRadius;
                if (Rand(patchSeed) > GrassTerrainDensity(cx, cz)) continue;
                if (!Plantable(cx, cz)) continue;
                for (int i = 0; i < kBladesPerTuft; ++i) {
                    const float angle = Rand(patchSeed) * XM_2PI;
                    const float r = kTuftRadius * std::sqrt(Rand(patchSeed));
                    const float x = cx + std::cos(angle) * r;
                    const float z = cz + std::sin(angle) * r;
                    if (Excluded(x, z)) continue;
                    if (Rand(patchSeed) > GrassTerrainDensity(x, z)) continue;
                    const float y = m_terrain(x, z);
                    if (y < m_waterY + kShoreMargin) continue;
                    Blade blade;
                    blade.x = x;
                    blade.z = z;
                    blade.baseY = y;
                    blade.height = 0.38f + Rand(patchSeed) * 0.42f;
                    blade.width = 0.012f + Rand(patchSeed) * 0.010f;
                    const float yaw = Rand(patchSeed) * XM_2PI;
                    blade.dirX = std::cos(yaw);
                    blade.dirZ = std::sin(yaw);
                    blade.phase = (Rand(patchSeed) - 0.5f) * 0.6f;
                    const float leanYaw = angle + (Rand(patchSeed) - 0.5f) * 1.2f;
                    blade.lean = 0.05f + Rand(patchSeed) * 0.20f;
                    blade.leanX = std::cos(leanYaw);
                    blade.leanZ = std::sin(leanYaw);
                    m_blades.push_back(blade);
                }
            }
        }

        // Sort the blades into spatial cells, so that each cell owns one contiguous
        // run of INSTANCES and can be drawn or skipped on its own. Without this the
        // field is a single draw and the GPU pays for every blade on the island,
        // however far away it is.
        SortIntoCells(span);
    }

    // Reorder m_blades so that blades sharing a grid cell are adjacent, and record
    // each cell's index range. Called once, before the buffers are built, so the
    // ordering is baked into the static geometry.
    void SortIntoCells(float span) {
        m_cells.clear();
        if (m_blades.empty()) return;

        float half = span * 0.5f;
        for (const Blade& blade : m_blades)
            half = (std::max)(half, (std::max)(std::abs(blade.x), std::abs(blade.z)) +
                kCellSize);
        half = std::ceil(half / kCellSize) * kCellSize;
        const int n = std::max(1, (int)std::ceil((half * 2.0f) / kCellSize));
        const auto cellOf = [&](const Blade& b) {
            const int ix = std::clamp((int)((b.x + half) / kCellSize), 0, n - 1);
            const int iz = std::clamp((int)((b.z + half) / kCellSize), 0, n - 1);
            return (size_t)iz * n + ix;
        };

        // Counting sort into cell order: one pass to size each bucket, one to fill.
        std::vector<size_t> counts((size_t)n * n, 0);
        for (const Blade& b : m_blades) ++counts[cellOf(b)];

        std::vector<size_t> starts((size_t)n * n, 0);
        size_t running = 0;
        for (size_t c = 0; c < counts.size(); ++c) {
            starts[c] = running;
            running += counts[c];
        }

        std::vector<Blade> sorted(m_blades.size());
        std::vector<size_t> cursor = starts;
        for (const Blade& b : m_blades) sorted[cursor[cellOf(b)]++] = b;
        m_blades.swap(sorted);

        // Record the non-empty cells. Each is a contiguous run of instances, drawn
        // with one DrawIndexedInstanced whose StartInstanceLocation is firstBlade.
        for (int iz = 0; iz < n; ++iz)
        for (int ix = 0; ix < n; ++ix) {
            const size_t c = (size_t)iz * n + ix;
            if (counts[c] == 0) continue;
            Cell cell;
            cell.cx = -half + (ix + 0.5f) * kCellSize;
            cell.cz = -half + (iz + 0.5f) * kCellSize;
            cell.firstBlade = (UINT)starts[c];
            cell.bladeCount = (UINT)counts[c];
            m_cells.push_back(cell);
        }
    }

    // Write every blade's REST pose once. Nothing here is ever rewritten: the wind,
    // the bend and the distance fade all happen in grass_vs.hlsl now, so this runs
    // a single time at load and the per-frame CPU cost of the grass is zero.
    //
    // What the vertex actually carries is not the blade's final shape but the
    // inputs the shader needs to rebuild it -- see the encoding note at the top of
    // grass_vs.hlsl. Grass draws untextured, so the pixel shader reads neither
    // texCoord nor tangent, and both are free to carry per-blade constants.
    bool BuildBuffers() {
        // First blade from allGrass_001.obj, normalized from centimetres. Reusing
        // one authored blade as the instanced template avoids multiplying the OBJ's
        // 836-blade clump by every procedural placement.
        static constexpr float authoredRows[kSegments + 1][3] = {
            { 0.0000000f, 0.0000000f, 1.0000000f },
            { 0.3607508f, 0.1504676f, 0.9375900f },
            { 0.6090932f, 0.3003972f, 0.6735600f },
            { 0.8640370f, 0.5956549f, 0.3752100f },
            { 1.0000000f, 0.8813891f, 0.2461600f },
        };
        GrassVertex verts[kVertsPerBlade];
        std::vector<uint32_t> idx;
        idx.reserve(kIndicesPerBlade);

        int v = 0;
        for (int s = 0; s <= kSegments; ++s) {
            const float t = authoredRows[s][0];
            const float forward = authoredRows[s][1];
            const float widthScale = authoredRows[s][2];
            verts[v++].shape = XMFLOAT4(t, -1.0f, forward, widthScale);
            verts[v++].shape = XMFLOAT4(t, +1.0f, forward, widthScale);
        }
        for (int s = 0; s < kSegments; ++s) {
            const uint32_t r0 = s * 2;          // this row: left, right
            const uint32_t r1 = (s + 1) * 2;    // the row above
            idx.push_back(r0);     idx.push_back(r1);     idx.push_back(r0 + 1);
            idx.push_back(r0 + 1); idx.push_back(r1);     idx.push_back(r1 + 1);
        }

        const UINT vbSize = (UINT)sizeof(verts);
        if (!CreateStaticBufferDX12(g_dx12.device.Get(), verts, vbSize,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, m_vb,
                "GrassVertexBuffer"))
            return false;
        m_vbv.BufferLocation = m_vb->GetGPUVirtualAddress();
        m_vbv.SizeInBytes = vbSize;
        m_vbv.StrideInBytes = sizeof(GrassVertex);

        const UINT ibSize = (UINT)(idx.size() * sizeof(uint32_t));
        if (!CreateStaticBufferDX12(g_dx12.device.Get(), idx.data(), ibSize,
                D3D12_RESOURCE_STATE_INDEX_BUFFER, m_ib,
                "GrassIndexBuffer"))
            return false;
        m_ibv.BufferLocation = m_ib->GetGPUVirtualAddress();
        m_ibv.SizeInBytes = ibSize;
        m_ibv.Format = DXGI_FORMAT_R32_UINT;

        // The instance buffer: one entry per blade, in the cell-sorted order the
        // draw ranges refer to. This is the only place the field's real size shows
        // up -- ~48 bytes a blade rather than eight duplicated 44-byte vertices.
        std::vector<BladeInstance> inst;
        inst.reserve(m_blades.size());
        for (const Blade& b : m_blades) {
            BladeInstance bi;
            bi.root   = XMFLOAT3(b.x, b.baseY, b.z);
            bi.height = b.height;
            bi.dir    = XMFLOAT2(b.dirX, b.dirZ);
            bi.lean   = XMFLOAT2(b.leanX * b.lean, b.leanZ * b.lean);
            bi.width  = b.width;
            bi.phase  = b.phase;
            inst.push_back(bi);
        }

        const UINT instSize = (UINT)(inst.size() * sizeof(BladeInstance));
        if (!CreateStaticBufferDX12(g_dx12.device.Get(), inst.data(), instSize,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_instances,
                "GrassInstanceBuffer"))
            return false;
        return true;
    }

    // Terrain's noisy sand transition reaches 2.25 m above the waterline. Keep
    // blade roots above it so distant minimum-width cards cannot become black
    // dashes across the beach.
    static constexpr float kShoreMargin = 2.35f;
    // Steepest ground grass will grow on (rise over run).
    static constexpr float kMaxSlope = 1.10f;
    // Below this painted total a splat texel counts as untouched, so the
    // procedural scatter passes through. One 8-bit step is 1/255 ~= 0.0039.
    static constexpr float kSplatCoverageEpsilon = 0.002f;
    // Blades are clumped into tufts rather than scattered evenly -- see BuildBlades.
    static constexpr int   kBladesPerTuft = 8;
    static constexpr float kTuftRadius    = 0.18f;   // metres
    // Blades shrink to nothing as they approach this range, so distant grass costs
    // no pixels and the field has no hard edge. Enforced in the vertex shader --
    // the geometry is static, so there is nothing to cull on the CPU. Runtime-
    // tunable (with kFadeBand and density) via the UI perf sliders.
    float m_drawDistance = 60.0f;
    // Width of that shrink, so the fade edge does not read as a ring of grass
    // popping in and out as the player walks.
    float m_fadeBand = 6.0f;
    // Fraction of each cell's blades actually drawn (1 = all).
    float m_density = 1.0f;
    // Sparse grass casters still produce visible cascade/cell bands across the
    // terrain. Keep blade lighting, but leave terrain shadowing to solid props.
    float m_shadowDensity = 0.28f;
    XMFLOAT3 m_albedo = { 0.078431f, 0.078431f, 0.078431f };
    float m_roughness = 1.0f;
    float m_ambientScale = 1.239f;
    float m_directLightScale = 2.0f;
    float m_transmissionStrength = 0.776f;
    float m_normalFalloff = 1.0f;
    float m_colorVariation = 0.85f;
    // Side of one draw-cell. Small enough that the cells hug the draw radius
    // without dragging in much grass the shader would only fade away, large enough
    // that the whole field stays a few dozen draws rather than hundreds.
    static constexpr float kCellSize      = 8.0f;

    std::function<float(float, float)> m_terrain;
    std::function<bool(float, float)> m_blocked;
    std::vector<Exclusion> m_exclusions;
    std::vector<RuntimeExclusion> m_runtimeExclusions;
    std::vector<AuthoredPatch> m_authoredPatches;
    std::vector<Blade> m_blades;
    std::vector<Cell>  m_cells;

    // Static geometry: written once at load, never touched again. m_vb/m_ib hold
    // the single blade template; m_instances holds the whole field.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vb;
    D3D12_VERTEX_BUFFER_VIEW               m_vbv = {};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ib;
    D3D12_INDEX_BUFFER_VIEW                m_ibv = {};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_instances;

    XMFLOAT3 m_eye{};          // camera position, for the shader's distance fade

    // Painted terrain splatmap (RGBA = grass/dirt/sand/rock), empty when the
    // level has none. Read only during the scatter, never per frame.
    std::vector<uint8_t> m_splat;
    uint32_t m_splatResolution = 0;
    float m_splatHalfExtentX = 1.0f;
    float m_splatHalfExtentZ = 1.0f;
    bool m_showAuthoredPaths = false;

    float m_time = 0.0f;
    float m_waterY = 0.0f;
    float m_windStrength = 0.85f;
    float m_windSpeed = 1.1f;
    XMFLOAT3 m_helicopterPosition{};
    float m_helicopterWindRadius = 22.0f;
    float m_helicopterWindStrength = 0.0f;
    // Player interaction fills b6 to 19 DWORDs; both raster and shadow root
    // signatures carry the same complete constants block.
    XMFLOAT3 m_playerPosition{};
    XMFLOAT2 m_playerTrailPosition{};
    float m_playerPushRadius = 2.30f;
    float m_playerPushStrength = 1.5f;
    bool  m_playerPushEnabled = true;
    bool  m_playerPushActive = false;
    bool  m_playerTrailInitialized = false;
    bool  m_ready = false;
};

extern GrassField g_grass;
