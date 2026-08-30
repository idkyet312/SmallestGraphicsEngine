#ifndef TERRAIN_RENDERER_DX12_H
#define TERRAIN_RENDERER_DX12_H

#include "MeshShaderDX12.h" // MeshPSOSubobjectDX12 template + ShaderDX12
#include "GLBImporter.h"
#include "LevelDefinition.h"
#include "TerrainStampLibrary.h"
#include "TextureUploadArenaDX12.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

// Procedural heightfield terrain drawn entirely on the GPU through the
// amplification/mesh shader pipeline: terrain_as.hlsl culls tiles and picks a
// tessellation LOD, terrain_ms.hlsl generates the displaced grid per tile.
// Shares the main root signature and the meshlet path's pixel shader
// (mesh_ps.cso), so terrain receives the full PBR/shadow/DDGI/SH lighting.
class TerrainRendererDX12 {
public:
    // Matches TerrainParams (b6, root param 8).
    struct Params {
        UINT tilesX = 16;
        UINT tilesZ = 16;
        float tileSize = 8.0f;
        float heightScale = 3.057f;
        // Distance at which tessellation starts dropping. Must sit past the
        // half-span of ring 0 (G/2 * tileSize = 10 m at the 20-tile/1 m clipmap
        // default) or the finest ring is already coarsening before the next ring
        // takes over, which reads as detail vanishing a few metres from the
        // camera.
        float lodNear = 44.0f;
        float lodStep = 34.0f;
        float skirtDepth = 1.0f;
        float flattenRadius = 14.0f;
        float islandScaleX = 1.0f;   // per-axis coastline stretch (X)
        float islandScaleZ = 1.0f;   // per-axis coastline stretch (Z)
        UINT sculptCount = 0;
        float sculptMaxDisplacement = 0.0f;
        // Grid min-corner offset in tiles; 0 keeps the legacy centered grid.
        int originTileX = 0;
        int originTileZ = 0;
        // 0 = smooth radial coast (editor islands, stretches cleanly per axis);
        // 1 = the Level-1 stress island (warped coast, NW bay, SE headland, 8
        // building pads). Those features use fixed world positions that fragment
        // when the coast is stretched, so they are stress-mode only.
        UINT terrainStyle = 0;
        // Extra relief detail: a low-frequency octave for broad landforms and a
        // high-frequency one for close-up break-up, plus macro normal
        // perturbation in the terrain shader. 0 = the original single-scale
        // noise. Off by default so existing levels keep the exact heightfield
        // they were authored and collided against.
        UINT detailRelief = 1;
    };
    // Root param 8 is 16 DWORDs; Draw uploads 16. Keep the struct exactly that
    // size so the upload never reads past it or shifts the cbuffer layout.
    static_assert(sizeof(Params) == 16 * sizeof(UINT),
                  "TerrainParams must be exactly 16 DWORDs (matches root const upload)");

    struct SculptGPU {
        float x, z, radius;
        UINT operation;
        float value, strength;
        UINT textureIndex = UINT_MAX;
        float rotation = 0.0f;
        float replace = 0.0f;
        float baseHeight = 0.0f;
    };
    // Must match TerrainSculptStamp in terrain_ms.hlsl -- a StructuredBuffer
    // read with a mismatched stride silently shears every stamp after the first.
    static_assert(sizeof(SculptGPU) == 40);

    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12PipelineState> psoWireframe;
    ComPtr<ID3D12PipelineState> psoMSAA;
    ComPtr<ID3D12PipelineState> psoWireframeMSAA;
    ComPtr<ID3D12PipelineState> psoHDR;
    ComPtr<ID3D12PipelineState> psoWireframeHDR;
    // Writes terrain IDs into the R32G32_UINT visibility buffer instead of
    // shading. Optional: if this PSO is missing the forward path still runs.
    ComPtr<ID3D12PipelineState> psoVisibility;
    // Depth only, no pixel shader and no render target, for the shadow passes.
    // Terrain was previously absent from shadow rendering entirely, so hills
    // blocked nothing -- barely noticeable for the sun, which comes in at a
    // shallow angle, but obvious under a searchlight pointed straight down.
    // Optional like the visibility PSO: without it terrain simply casts none.
    ComPtr<ID3D12PipelineState> psoShadow;
    ComPtr<ID3D12GraphicsCommandList6> commandList6;
    ComPtr<ID3D12Resource> terrainAlbedoArray;
    ComPtr<ID3D12Resource> terrainNormalArray;
    ComPtr<ID3D12Resource> terrainRoughnessArray;
    // Per-level painted layer weights (RGBA = grass/dirt/sand/rock). Null until
    // a level supplies one, which is the common case; the resolve then keeps
    // using purely procedural weights.
    ComPtr<ID3D12Resource> terrainSplatMap;
    ComPtr<ID3D12Resource> terrainSplatUpload;
    UINT terrainSplatResolution = 0;
    // Staged paint waiting for a frame to record its copy. See
    // StageTerrainSplatMap for why the upload cannot happen at call time.
    std::vector<uint8_t> pendingSplatPixels_;
    UINT pendingSplatResolution_ = 0;
    bool splatUploadPending_ = false;
    // Replaced splat textures awaiting the GPU to finish with them. Released
    // once no in-flight frame can still reference them.
    struct RetiredSplat {
        ComPtr<ID3D12Resource> texture;
        ComPtr<ID3D12Resource> upload;
        UINT framesRemaining;
    };
    std::vector<RetiredSplat> retiredSplatMaps_;
    std::array<ComPtr<ID3D12Resource>, FRAME_COUNT> sculptBuffers;
    std::array<ComPtr<ID3D12Resource>, FRAME_COUNT> stampAtlasBuffers;
    std::array<uint64_t, FRAME_COUNT> uploadedSculptRevision_{};
    std::array<uint64_t, FRAME_COUNT> uploadedStampAtlasRevision_{};
    std::array<ComPtr<ID3D12Resource>, 3> terrainUploads;
    D3D12_GPU_DESCRIPTOR_HANDLE terrainTextureTable{};
    bool supported = false;
    bool msaaSupported = false;
    bool msaaEnabled = false;
    bool wireframe = false; // Z key: draw terrain tiles as wireframe
    bool hdrTargetEnabled = false;

    void ReleaseUploadHeaps() {
        for (auto& upload : terrainUploads) upload.Reset();
    }

    bool Init(ShaderDX12& shader) {
        // Mesh shader tier support was already verified by MeshShaderDX12::Init;
        // if that failed these .cso files won't exist either, so just try to load.
        if (FAILED(g_dx12.commandList.As(&commandList6))) return false;

        ComPtr<ID3DBlob> as, ms, ps, hdrPs;
        if (FAILED(ReadCompiledShaderDX12(L"shaders/terrain_as.cso", &as))) {
            std::cerr << "Terrain amplification shader DXIL missing: shaders/terrain_as.cso\n";
            return false;
        }
        if (FAILED(ReadCompiledShaderDX12(L"shaders/terrain_ms.cso", &ms))) {
            std::cerr << "Terrain mesh shader DXIL missing: shaders/terrain_ms.cso\n";
            return false;
        }
        if (FAILED(ReadCompiledShaderDX12(L"shaders/terrain_ps.cso", &ps))) {
            std::cerr << "Terrain PBR pixel shader missing: shaders/terrain_ps.cso\n";
            return false;
        }
        if (FAILED(ReadCompiledShaderDX12(L"shaders/terrain_ps_hdr.cso", &hdrPs))) {
            std::cerr << "HDR terrain PBR pixel shader missing: shaders/terrain_ps_hdr.cso\n";
            return false;
        }
        if (!shader.rootSignature) return false;

        using Root = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*>;
        using AS = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE>;
        using MS = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE>;
        using PS = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>;
        using Raster = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>;
        using Blend = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC>;
        using Depth = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC>;
        using Sample = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC>;
        using Mask = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT>;
        using RT = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>;
        using DS = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>;
        struct alignas(8) Stream { Root root; AS as; MS ms; PS ps; Raster raster; Blend blend; Depth depth; Sample sample; Mask mask; RT rt; DS ds; } stream;
        stream.root.value = shader.rootSignature.Get();
        stream.as.value = { as->GetBufferPointer(), as->GetBufferSize() };
        stream.ms.value = { ms->GetBufferPointer(), ms->GetBufferSize() };
        stream.ps.value = { ps->GetBufferPointer(), ps->GetBufferSize() };
        stream.raster.value.FillMode = D3D12_FILL_MODE_SOLID;
        stream.raster.value.CullMode = D3D12_CULL_MODE_NONE;
        stream.raster.value.DepthClipEnable = TRUE;
        stream.blend.value.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        stream.depth.value.DepthEnable = TRUE;
        stream.depth.value.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        stream.depth.value.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        stream.sample.value.Count = 1;
        stream.mask.value = UINT_MAX;
        stream.rt.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        stream.rt.value.NumRenderTargets = 1;
        stream.ds.value = DXGI_FORMAT_D32_FLOAT;
        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = { sizeof(stream), &stream };
        ComPtr<ID3D12Device2> device2;
        if (FAILED(g_dx12.device.As(&device2))) return false;
        HRESULT psoHr = device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pso));
        if (FAILED(psoHr)) {
            std::cerr << "Terrain PSO creation failed: 0x" << std::hex << psoHr << std::dec << "\n";
            return false;
        }
        stream.rt.value.RTFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        stream.ps.value = { hdrPs->GetBufferPointer(), hdrPs->GetBufferSize() };
        if (FAILED(device2->CreatePipelineState(
                &streamDesc, IID_PPV_ARGS(&psoHDR)))) return false;
        stream.ps.value = { ps->GetBufferPointer(), ps->GetBufferSize() };
        stream.rt.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        stream.sample.value.Count = MSAADX12::SampleCount;
        stream.raster.value.MultisampleEnable = TRUE;
        msaaSupported = SUCCEEDED(
            device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&psoMSAA)));
        stream.sample.value.Count = 1;
        stream.raster.value.MultisampleEnable = FALSE;

        // Visibility variant: same AS/MS, a pixel shader that writes only IDs,
        // and the R32G32_UINT visibility target. Built last among the solid
        // PSOs so the raster/depth state above is still the default one.
        // Failure is non-fatal -- terrain then simply stays on the forward path.
        {
            ComPtr<ID3DBlob> visPs;
            if (SUCCEEDED(ReadCompiledShaderDX12(
                    L"shaders/terrain_visibility_ps.cso", &visPs))) {
                stream.ps.value = { visPs->GetBufferPointer(),
                                    visPs->GetBufferSize() };
                stream.rt.value.RTFormats[0] = DXGI_FORMAT_R32G32_UINT;
                if (FAILED(device2->CreatePipelineState(
                        &streamDesc, IID_PPV_ARGS(&psoVisibility)))) {
                    std::cerr << "Terrain visibility PSO creation failed "
                                 "(non-fatal; terrain stays forward)\n";
                    psoVisibility.Reset();
                }
            }
            stream.ps.value = { ps->GetBufferPointer(), ps->GetBufferSize() };
            stream.rt.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        }

        // Depth-only shadow PSO: same AS/MS, no pixel shader, no render target.
        // Depth bias matches DepthOnlyShaderDX12's so terrain acne behaves the
        // same as every other caster's.
        {
            const D3D12_SHADER_BYTECODE noPixelShader = {};
            stream.ps.value = noPixelShader;
            stream.rt.value.NumRenderTargets = 0;
            stream.rt.value.RTFormats[0] = DXGI_FORMAT_UNKNOWN;
            stream.raster.value.DepthBias = 1000;
            stream.raster.value.SlopeScaledDepthBias = 1.0f;
            if (FAILED(device2->CreatePipelineState(
                    &streamDesc, IID_PPV_ARGS(&psoShadow)))) {
                std::cerr << "Terrain shadow PSO creation failed "
                             "(non-fatal; terrain casts no shadow)\n";
                psoShadow.Reset();
            }
            stream.raster.value.DepthBias = 0;
            stream.raster.value.SlopeScaledDepthBias = 0.0f;
            stream.rt.value.NumRenderTargets = 1;
            stream.ps.value = { ps->GetBufferPointer(), ps->GetBufferSize() };
            stream.rt.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        }

        stream.raster.value.FillMode = D3D12_FILL_MODE_WIREFRAME;
        ComPtr<ID3DBlob> wirePs;
        if (SUCCEEDED(ReadCompiledShaderDX12(L"shaders/wire_green_ps.cso", &wirePs))) {
            stream.ps.value = { wirePs->GetBufferPointer(), wirePs->GetBufferSize() };
        }
        if (FAILED(device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&psoWireframe)))) {
            std::cerr << "Terrain wireframe PSO creation failed (non-fatal)\n";
        }
        stream.rt.value.RTFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (FAILED(device2->CreatePipelineState(
                &streamDesc, IID_PPV_ARGS(&psoWireframeHDR))))
            psoWireframeHDR.Reset();
        stream.rt.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (msaaSupported) {
            stream.sample.value.Count = MSAADX12::SampleCount;
            stream.raster.value.MultisampleEnable = TRUE;
            if (FAILED(device2->CreatePipelineState(
                    &streamDesc, IID_PPV_ARGS(&psoWireframeMSAA)))) {
                psoWireframeMSAA.Reset();
            }
            stream.sample.value.Count = 1;
            stream.raster.value.MultisampleEnable = FALSE;
        }

        if (!CreateTerrainTextureArrays(shader) || !CreateSculptBuffers()) return false;

        // SGE_SPLAT_TEST=1 uploads a rock/grass checkerboard covering the whole
        // island. It exists to validate the world->UV mapping independently of
        // the editor brush, so "wrong UVs" and "wrong brush" stay
        // distinguishable. Off unless the variable is set.
        if (GetEnvironmentVariableA("SGE_SPLAT_TEST", nullptr, 0) > 0) {
            constexpr UINT kTestRes = 512;
            // 32 squares across a ~176 m island is ~5.5 m per square: several
            // are visible at once from eye height. 8 was one square per 22 m,
            // which reads as a stray terrain edge rather than a grid.
            constexpr UINT kSquares = 32;
            std::vector<uint8_t> pattern(
                static_cast<size_t>(kTestRes) * kTestRes * 4, 0);
            for (UINT y = 0; y < kTestRes; ++y) {
                for (UINT x = 0; x < kTestRes; ++x) {
                    const UINT cx = x * kSquares / kTestRes;
                    const UINT cy = y * kSquares / kTestRes;
                    uint8_t* texel =
                        &pattern[(static_cast<size_t>(y) * kTestRes + x) * 4];
                    // Rock vs sand, not rock vs grass: most of this island is
                    // procedurally grass already, so grass squares would be
                    // invisible and only half the checker would show.
                    if (((cx + cy) & 1) != 0) texel[3] = 255;  // rock
                    else                      texel[2] = 255;  // sand
                }
            }
            if (!UploadTerrainSplatMap(pattern.data(), kTestRes))
                std::cerr << "Terrain splat: test pattern upload failed\n";
        }

        supported = true;
        return true;
    }

    void SetSculptStamps(const std::vector<TerrainSculptStamp>& stamps) {
        // Keep the NEWEST stamps when over capacity: runtime craters are pushed
        // in chronological order, so trimming the front means the most recent
        // explosion always leaves a hole instead of silently doing nothing.
        const size_t keep = (std::min)(stamps.size(), kMaxTerrainSculptStamps);
        s_sculptStamps.assign(stamps.end() - keep, stamps.end());
        for (const TerrainSculptStamp& stamp : s_sculptStamps)
            if (stamp.operation == TerrainSculptOperation::Heightmap)
                EnsureHeightStampLoaded(stamp.texture);
        m_sculptMaxDisplacement = 0.0f;
        for (const TerrainSculptStamp& stamp : s_sculptStamps) {
            if (stamp.operation == TerrainSculptOperation::Add ||
                (stamp.operation == TerrainSculptOperation::Heightmap &&
                 stamp.replace <= 0.0f))
                m_sculptMaxDisplacement += std::abs(stamp.value);
            else if (stamp.operation == TerrainSculptOperation::Heightmap)
                // A replace stamp pulls the ground all the way to baseHeight,
                // so its reach is that offset plus the relief, not just value.
                m_sculptMaxDisplacement = (std::max)(m_sculptMaxDisplacement,
                    std::abs(stamp.baseHeight) + std::abs(stamp.value) + 12.0f);
            else
                m_sculptMaxDisplacement = (std::max)(m_sculptMaxDisplacement,
                    std::abs(stamp.value) + 12.0f);
        }
        ++s_sculptRevision;
    }

    void SetMSAAEnabled(bool enabled) {
        msaaEnabled = enabled && msaaSupported;
    }

    void SetHDRTargetEnabled(bool enabled) { hdrTargetEnabled = enabled; }

    // terrainStyle is a bit field, not an enum:
    //   bit 0 (1) = stress island layout (warped coast, bay, headland, pads)
    //   bit 1 (2) = clipmap topology (tilesX = ring grid G, tilesZ = ring count)
    // These are independent, so a stress island drawn as a clipmap is style 3.
    // Equality tests against 1 silently dropped the coastline the moment the
    // clipmap bit was set; always test the bit.
    static constexpr UINT kStyleStressIsland = 1u;
    static constexpr UINT kStyleClipmap = 2u;
    // bit 2 (4) = flat authoring plane: skip the fbm relief, the pool basin and
    // the coast falloff so the ground is a level plane to sculpt on. Packed as
    // a style bit because Params is pinned to 16 DWORDs by the root-constant
    // upload and has no spare slot for another field.
    static constexpr UINT kStyleFlat = 4u;
    // bit 3 (8) = deployment overview. The amplification shader forces mesh
    // LOD 0 and morph 0. CurrentTerrainParams also selects a uniform-density
    // grid for this mode, so "LOD 0" cannot still conceal larger outer tiles.
    static constexpr UINT kStyleDeploymentOverview = 8u;
    static bool IsFlat(UINT terrainStyle) {
        return (terrainStyle & kStyleFlat) != 0u;
    }
    static bool IsStressIsland(UINT terrainStyle) {
        return (terrainStyle & kStyleStressIsland) != 0u;
    }
    static bool IsClipmap(UINT terrainStyle) {
        return (terrainStyle & kStyleClipmap) != 0u;
    }

    static uint64_t SculptRevision() { return s_sculptRevision; }

    // CPU mirror of terrain_ms.hlsl's height function (hash21/noise2/fbm/
    // TerrainHeight), used for walking collision. Keep the two in sync - any
    // drift puts the camera above or inside the rendered ground.
    static float HeightAt(const Params& params, float x, float z) {
        return HeightAt(params, x, z, s_sculptStamps);
    }

    static float HeightAt(const Params& params, float x, float z,
        const std::vector<TerrainSculptStamp>& sculpt) {
        auto hashUint = [](uint32_t value) {
            value ^= value >> 16;
            value *= 0x7feb352du;
            value ^= value >> 15;
            value *= 0x846ca68bu;
            value ^= value >> 16;
            return value;
        };
        auto hash21 = [&](float px, float py) {
            const uint32_t cellX = static_cast<uint32_t>(
                static_cast<int32_t>(px));
            const uint32_t cellY = static_cast<uint32_t>(
                static_cast<int32_t>(py));
            const uint32_t value = hashUint(
                cellX ^ (hashUint(cellY) + 0x9e3779b9u));
            return static_cast<float>(value & 0x00ffffffu) *
                   (1.0f / 16777216.0f);
        };
        auto noise2 = [&](float px, float py) {
            float ix = floorf(px), iy = floorf(py);
            float fx = px - ix, fy = py - iy;
            fx = fx * fx * (3.0f - 2.0f * fx);
            fy = fy * fy * (3.0f - 2.0f * fy);
            float a = hash21(ix, iy);
            float b = hash21(ix + 1.0f, iy);
            float c = hash21(ix, iy + 1.0f);
            float d = hash21(ix + 1.0f, iy + 1.0f);
            return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
        };

        // Clipmap mode covers a large camera-centred area, so there is no fixed
        // grid extent to clamp to - the island falloff below sinks the ground to
        // seabed past the shore. Only the legacy uniform grid has a hard edge.
        constexpr UINT kClipmapFlag = 2u;
        if ((params.terrainStyle & kClipmapFlag) == 0u) {
            // Outside the tiled extent there is no drawn ground; treat as
            // level 0. The grid's min corner is offset by originTile* so an
            // edge-extended island can grow off-center; match the shaders.
            const float minX = (static_cast<float>(params.originTileX) -
                params.tilesX * 0.5f) * params.tileSize;
            const float minZ = (static_cast<float>(params.originTileZ) -
                params.tilesZ * 0.5f) * params.tileSize;
            const float maxX = minX + params.tilesX * params.tileSize;
            const float maxZ = minZ + params.tilesZ * params.tileSize;
            if (x < minX || x >= maxX || z < minZ || z >= maxZ) return 0.0f;
        }

        // Flat authoring plane: skip every procedural landform and let the
        // sculpt stamps be the only thing that shapes the ground. Mirrors the
        // early-out in terrain_ms.hlsl's TerrainHeight -- collision is sampled
        // from here while the surface is displaced there, so the two branches
        // have to appear at the same point in the chain.
        if (IsFlat(params.terrainStyle))
            return ApplySculpt(2.5f /* landLift */, x, z, sculpt);

        float px = x * 0.08f, py = z * 0.08f;
        float sum = 0.0f, amp = 0.5f;
        for (int i = 0; i < 5; ++i) {
            sum += noise2(px, py) * amp;
            const float nextX = px * 1.616f - py * 1.212f;
            const float nextY = px * 1.212f + py * 1.616f;
            px = nextX; py = nextY;
            amp *= 0.5f;
        }
        float h = sum * params.heightScale;
        // Must match terrain_ms.hlsl's TerrainHeight: collision is sampled from
        // here while the visible surface is displaced there, so any divergence
        // puts the player above or inside the ground.
        if (params.detailRelief != 0) {
            h += noise2(x * 0.015f, z * 0.015f) * params.heightScale * 1.55f;
            h += noise2(x * 0.42f, z * 0.42f) * params.heightScale * 0.075f;
        }

        float dist = sqrtf(x * x + z * z);
        float t = (dist - params.flattenRadius) / params.flattenRadius; // (b-a)==flattenRadius
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        float mask = t * t * (3.0f - 2.0f * t);
        h *= mask;

        // Pool basin carve -- must match terrain_ms.hlsl's TerrainHeight and the
        // pool spawned in main.cpp.
        constexpr float poolCx = -22.0f, poolCz = -20.0f;
        constexpr float poolRadius = 4.2f, poolRim = 7.0f, poolDepth = 3.0f;
        float pd = sqrtf((x - poolCx) * (x - poolCx) + (z - poolCz) * (z - poolCz));
        float bt = (pd - poolRadius) / (poolRim - poolRadius);
        bt = bt < 0.0f ? 0.0f : (bt > 1.0f ? 1.0f : bt);
        float basin = 1.0f - bt * bt * (3.0f - 2.0f * bt);   // 1 at centre -> 0 past rim
        h -= poolDepth * basin;

        // Island falloff -- must match terrain_ms.hlsl's TerrainHeight. Inland
        // relief first flattens into a broad beach shelf. That shelf slopes
        // gently through sea level, then curves down into the full-depth seabed.
        constexpr float landLift = 2.5f, seabed = -6.0f;
        constexpr float beachStart = 28.0f, beachShelf = 35.0f;
        constexpr float beachWaterline = 43.0f;
        // Must match terrain_ms.hlsl's kShoreOuter. Widened from 52 so the
        // seabed slopes away over 45 m instead of dropping 5.75 m in 9 m.
        constexpr float shoreOuter = 88.0f;
        constexpr float beachHigh = 0.65f, beachLow = -0.25f;
        // Per-axis island size: normalise the coordinate by each axis' scale so
        // the coastline stretches independently on X and Z (oval / strip). The
        // shore thresholds stay at their base radii in this normalised space.
        const float sx = (std::max)(0.01f, params.islandScaleX);
        const float sz = (std::max)(0.01f, params.islandScaleZ);
        const float nx = x / sx, nz = z / sz;
        const float maxScale = (std::max)(params.islandScaleX, params.islandScaleZ);
        // Warp an oval coastline, then cut a northwest bay and extend a
        // southeast headland. This avoids the artificial circular-atoll shape.
        float coastDistance = sqrtf(nx * nx + nz * nz);
        auto coastLobe = [](float distance, float radius) {
            float t = distance / radius;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            return 1.0f - t * t * (3.0f - 2.0f * t);
        };
        if (IsStressIsland(params.terrainStyle) && maxScale > 1.5f) {
            const float warpedX = nx + sinf(nz * 0.055f) * 7.0f +
                sinf((nx + nz) * 0.025f) * 4.0f;
            const float warpedZ = nz + sinf(nx * 0.047f) * 6.0f -
                sinf((nx - nz) * 0.031f) * 3.0f;
            coastDistance = sqrtf(
                warpedX * warpedX * 0.92f * 0.92f +
                warpedZ * warpedZ * 1.06f * 1.06f);
            coastDistance += coastLobe(
                sqrtf((nx + 55.0f) * (nx + 55.0f) +
                      (nz - 15.0f) * (nz - 15.0f)), 22.0f) * 13.0f;
            coastDistance -= coastLobe(
                sqrtf((nx - 35.0f) * (nx - 35.0f) +
                      (nz + 55.0f) * (nz + 55.0f)), 26.0f) * 11.0f;
        }
        float land = h + landLift;
        float beachFlattenT =
            (coastDistance - beachStart) / (beachShelf - beachStart);
        beachFlattenT = beachFlattenT < 0.0f ? 0.0f :
            (beachFlattenT > 1.0f ? 1.0f : beachFlattenT);
        const float beachBlend = beachFlattenT * beachFlattenT *
            (3.0f - 2.0f * beachFlattenT);
        float st = (coastDistance - beachShelf) /
                   (beachWaterline - beachShelf);
        st = st < 0.0f ? 0.0f : (st > 1.0f ? 1.0f : st);
        const float beachT = st * st * (3.0f - 2.0f * st);
        const float beachHeight =
            beachHigh + (beachLow - beachHigh) * beachT;
        h = land + (beachHeight - land) * beachBlend;

        float ut = (coastDistance - beachWaterline) /
                   (shoreOuter - beachWaterline);
        ut = ut < 0.0f ? 0.0f : (ut > 1.0f ? 1.0f : ut);
        const float underwater = ut * ut * (3.0f - 2.0f * ut);
        h = h + (seabed - h) * underwater;

        // Flat arenas under each house compound, applied LAST so neither noise nor
        // the pool rim can dent them. Must match terrain_ms.hlsl's TerrainHeight;
        // padHeight is Ground::kBuildingPadY (world/terrain/GroundLevel.h), which the houses
        // and roofs are built from -- change it there and here together.
        constexpr float padRadius = 14.0f, padFade = 18.0f, padHeight = 2.5f;
        static constexpr float padCenters[8][2] = {
            {  0.0f,   0.0f }, { 42.0f,   0.0f },
            {-42.0f,   0.0f }, {  0.0f,  42.0f },
            { 42.0f,  42.0f }, {-42.0f,  42.0f },
            {  0.0f, -42.0f }, { 42.0f, -42.0f }
        };
        const int padCount = (IsStressIsland(params.terrainStyle) && maxScale > 1.5f) ? 8 : 1;
        for (int i = 0; i < padCount; ++i) {
            float dpx = x - padCenters[i][0], dpz = z - padCenters[i][1];
            float dpad = sqrtf(dpx * dpx + dpz * dpz);
            float pt = (dpad - padRadius) / (padFade - padRadius);
            pt = pt < 0.0f ? 0.0f : (pt > 1.0f ? 1.0f : pt);
            float pad = 1.0f - pt * pt * (3.0f - 2.0f * pt);
            h = h + (padHeight - h) * pad;
        }
        return ApplySculpt(h, x, z, sculpt);
    }

    // Applies every live sculpt stamp to an already-shaped ground height.
    // Shared by the procedural island and the flat authoring plane so the two
    // cannot drift. Must match ApplySculpt in terrain_ms.hlsl.
    static float ApplySculpt(float h, float x, float z,
        const std::vector<TerrainSculptStamp>& sculpt) {
        for (const TerrainSculptStamp& stamp : sculpt) {
            if (stamp.operation == TerrainSculptOperation::Heightmap) {
                float relief = 0.0f, coverage = 0.0f;
                SampleHeightStamp(stamp, x, z, relief, coverage);
                // Additive adds relief on top of the ground; replace swaps the
                // ground for baseHeight + relief. Both fade out with the same
                // edge coverage, so a replace stamp blends into the terrain
                // around it instead of leaving a cliff at its border.
                const float blend = coverage * stamp.replace;
                h = h + relief + (stamp.baseHeight - h) * blend;
                continue;
            }
            const float dx = x - stamp.x;
            const float dz = z - stamp.z;
            const float distance = sqrtf(dx * dx + dz * dz);
            float weight = 1.0f - distance / stamp.radius;
            weight = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
            weight = weight * weight * (3.0f - 2.0f * weight);
            if (stamp.operation == TerrainSculptOperation::Add)
                h += stamp.value * weight;
            else {
                const float blend = (std::min)(1.0f, stamp.strength * weight);
                h += (stamp.value - h) * blend;
            }
        }
        return h;
    }

    bool CreateSculptBuffers() {
        EnsureStampLibrary();
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = sizeof(SculptGPU) * kMaxTerrainSculptStamps;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
            if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&sculptBuffers[frame])))) return false;
            UploadSculptStamps(frame);
        }
        desc.Width = static_cast<UINT64>(kMaxTerrainStampTextures) *
            kTerrainStampResolution * kTerrainStampResolution * sizeof(uint16_t);
        for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
            if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&stampAtlasBuffers[frame])))) return false;
            UploadStampAtlas(frame);
        }
        return true;
    }

    void UploadSculptStamps(UINT frame) {
        if (frame >= FRAME_COUNT || !sculptBuffers[frame] ||
            uploadedSculptRevision_[frame] == s_sculptRevision) return;
        SculptGPU* destination = nullptr;
        if (FAILED(sculptBuffers[frame]->Map(0, nullptr,
                reinterpret_cast<void**>(&destination)))) return;
        std::memset(destination, 0, sizeof(SculptGPU) * kMaxTerrainSculptStamps);
        for (size_t i = 0; i < s_sculptStamps.size(); ++i) {
            const TerrainSculptStamp& source = s_sculptStamps[i];
            destination[i] = { source.x, source.z, source.radius,
                static_cast<UINT>(source.operation), source.value, source.strength,
                FindHeightStampLayer(source.texture), source.rotation,
                source.replace, source.baseHeight };
        }
        sculptBuffers[frame]->Unmap(0, nullptr);
        uploadedSculptRevision_[frame] = s_sculptRevision;
    }

    void UploadStampAtlas(UINT frame) {
        if (frame >= FRAME_COUNT || !stampAtlasBuffers[frame] ||
            uploadedStampAtlasRevision_[frame] == s_stampAtlasRevision)
            return;
        void* destination = nullptr;
        D3D12_RANGE readRange{ 0, 0 };
        if (FAILED(stampAtlasBuffers[frame]->Map(0, &readRange, &destination)))
            return;
        const size_t bytes = s_stampAtlas.size() * sizeof(uint16_t);
        std::memcpy(destination, s_stampAtlas.data(), bytes);
        D3D12_RANGE writtenRange{ 0, bytes };
        stampAtlasBuffers[frame]->Unmap(0, &writtenRange);
        uploadedStampAtlasRevision_[frame] = s_stampAtlasRevision;
    }

    bool CreateTextureArray(const std::vector<uint8_t>& pixels, UINT side,
                            UINT layers, bool srgb,
                            ComPtr<ID3D12Resource>& texture,
                            ComPtr<ID3D12Resource>& upload) {
        UINT mipLevels = 1;
        for (UINT mipSide = side; mipSide > 1; mipSide >>= 1) ++mipLevels;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = side;
        desc.Height = side;
        desc.DepthOrArraySize = static_cast<UINT16>(layers);
        desc.MipLevels = mipLevels;
        desc.Format = srgb ? DXGI_FORMAT_R8G8B8A8_TYPELESS
                           : DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;

        D3D12_HEAP_PROPERTIES defaultHeap = {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&texture)))) return false;

        const UINT subresourceCount = layers * mipLevels;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresourceCount);
        std::vector<UINT> rows(subresourceCount);
        std::vector<UINT64> rowBytes(subresourceCount);
        UINT64 uploadBytes = 0;
        g_dx12.device->GetCopyableFootprints(
            &desc, 0, subresourceCount, 0, layouts.data(), rows.data(),
            rowBytes.data(), &uploadBytes);
        ID3D12Resource* uploadResource = nullptr;
        uint8_t* mapped = nullptr;
        uint64_t pooledOffset = 0;
        const bool pooled = IsTextureUploadArenaActiveDX12();
        if (pooled) {
            const TextureUploadAllocationDX12 allocation =
                AllocateTextureUploadDX12(g_dx12.device.Get(), uploadBytes);
            if (!allocation) return false;
            uploadResource = allocation.resource;
            mapped = allocation.cpuAddress;
            pooledOffset = allocation.offset;
            upload.Reset();
        } else {
            D3D12_RESOURCE_DESC uploadDesc = {};
            uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            uploadDesc.Width = uploadBytes;
            uploadDesc.Height = 1;
            uploadDesc.DepthOrArraySize = 1;
            uploadDesc.MipLevels = 1;
            uploadDesc.SampleDesc.Count = 1;
            uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES uploadHeap = {};
            uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&upload)))) return false;
            if (FAILED(upload->Map(
                    0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
            uploadResource = upload.Get();
        }
        const size_t sourceLayerPitch = static_cast<size_t>(side) * side * 4;
        std::vector<std::vector<uint8_t>> mipData(subresourceCount);
        for (UINT layer = 0; layer < layers; ++layer) {
            mipData[layer * mipLevels].assign(
                pixels.begin() + sourceLayerPitch * layer,
                pixels.begin() + sourceLayerPitch * (layer + 1));
            UINT previousSide = side;
            for (UINT mip = 1; mip < mipLevels; ++mip) {
                const UINT mipSide = (std::max)(1u, previousSide / 2);
                const auto& previous = mipData[layer * mipLevels + mip - 1];
                auto& current = mipData[layer * mipLevels + mip];
                current.resize(static_cast<size_t>(mipSide) * mipSide * 4);
                for (UINT y = 0; y < mipSide; ++y) for (UINT x = 0; x < mipSide; ++x)
                    for (UINT channel = 0; channel < 4; ++channel) {
                        UINT sum = 0;
                        for (UINT oy = 0; oy < 2; ++oy) for (UINT ox = 0; ox < 2; ++ox) {
                            const UINT sx = (std::min)(previousSide - 1, x * 2 + ox);
                            const UINT sy = (std::min)(previousSide - 1, y * 2 + oy);
                            sum += previous[(static_cast<size_t>(sy) * previousSide + sx) * 4 + channel];
                        }
                        current[(static_cast<size_t>(y) * mipSide + x) * 4 + channel] =
                            static_cast<uint8_t>((sum + 2) / 4);
                    }
                previousSide = mipSide;
            }
        }
        for (UINT subresource = 0; subresource < subresourceCount; ++subresource) {
            const UINT mip = subresource % mipLevels;
            const UINT mipSide = (std::max)(1u, side >> mip);
            const size_t sourceRowPitch = static_cast<size_t>(mipSide) * 4;
            uint8_t* destination = mapped + layouts[subresource].Offset;
            const uint8_t* source = mipData[subresource].data();
            for (UINT row = 0; row < rows[subresource]; ++row)
                std::memcpy(destination + static_cast<size_t>(row) *
                    layouts[subresource].Footprint.RowPitch,
                    source + static_cast<size_t>(row) * sourceRowPitch,
                    sourceRowPitch);
        }
        if (pooled) {
            for (auto& layout : layouts) layout.Offset += pooledOffset;
        } else {
            upload->Unmap(0, nullptr);
        }

        for (UINT subresource = 0; subresource < subresourceCount; ++subresource) {
            D3D12_TEXTURE_COPY_LOCATION destination = {};
            destination.pResource = texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = subresource;
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = uploadResource;
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layouts[subresource];
            g_dx12.commandList->CopyTextureRegion(
                &destination, 0, 0, 0, &source, nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_dx12.commandList->ResourceBarrier(1, &barrier);
        return true;
    }

    // Stages a level's painted layer weights for upload. The GPU copy itself is
    // deferred to FlushPendingSplatUpload(), which runs during frame recording.
    //
    // Callers reach this from the editor sync, which is *not* inside the
    // frame's command-list recording; issuing CopyTextureRegion there would
    // record into a closed or foreign list, and WaitForGPU() mid-frame would
    // stall on work that has not been submitted. Sculpting has the same
    // constraint and solves it the same way -- SetSculptStamps only touches CPU
    // state and the GPU work happens later in the frame.
    void StageTerrainSplatMap(const uint8_t* rgba, UINT resolution) {
        if (!rgba || resolution == 0) {
            pendingSplatPixels_.clear();
            pendingSplatResolution_ = 0;
        } else {
            pendingSplatPixels_.assign(
                rgba, rgba + static_cast<size_t>(resolution) * resolution * 4u);
            pendingSplatResolution_ = resolution;
        }
        splatUploadPending_ = true;
    }

    // Performs any staged splat upload. Safe to call once per frame while the
    // command list is open; a no-op when nothing is pending.
    void FlushPendingSplatUpload() {
        // Age out replaced textures first: this runs once per frame regardless
        // of whether new paint arrived, so retirement makes progress even when
        // the user stops painting.
        for (size_t i = retiredSplatMaps_.size(); i-- > 0;) {
            if (--retiredSplatMaps_[i].framesRemaining == 0)
                retiredSplatMaps_.erase(retiredSplatMaps_.begin() + i);
        }
        if (!splatUploadPending_) return;
        splatUploadPending_ = false;
        UploadTerrainSplatMap(
            pendingSplatResolution_ ? pendingSplatPixels_.data() : nullptr,
            pendingSplatResolution_);
        // The staged copy has served its purpose; the pixels also live in the
        // LevelDefinition, so holding a second megabyte here is waste.
        pendingSplatPixels_.clear();
        pendingSplatPixels_.shrink_to_fit();
    }

    // Uploads a level's painted layer weights. Single mip and single slice:
    // the resolve reads it with SampleLevel(..., 0) at roughly one texel per
    // 0.35 m, so mips would only blur brush edges that are already feathered.
    //
    // Called once per stroke end rather than per stamp. It flushes, because the
    // previous texture may still be referenced by in-flight frames; recreating
    // rather than updating in place keeps that simple, and strokes are rare.
    bool UploadTerrainSplatMap(const uint8_t* rgba, UINT resolution) {
        // The previous texture may still be referenced by frames in flight, so
        // it is parked in a retirement list rather than released here. This runs
        // during command-list recording, where WaitForGPU() would signal the
        // queue and corrupt the frame's fence bookkeeping; the list is drained
        // FRAME_COUNT frames later, by which point no frame can still read it.
        if (terrainSplatMap) {
            retiredSplatMaps_.push_back(
                { terrainSplatMap, terrainSplatUpload, FRAME_COUNT + 1u });
        }
        terrainSplatMap.Reset();
        terrainSplatUpload.Reset();

        if (!rgba || resolution == 0) {
            // Dropping the map returns the level to purely procedural weights.
            terrainSplatResolution = 0;
            return true;
        }

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = resolution;
        desc.Height = resolution;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        // UNORM, never sRGB: these are blend weights, not colour.
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;

        D3D12_HEAP_PROPERTIES defaultHeap = {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&terrainSplatMap))))
            return false;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
        UINT rows = 0;
        UINT64 rowBytes = 0, uploadBytes = 0;
        g_dx12.device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &rows,
                                             &rowBytes, &uploadBytes);

        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadBytes;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&terrainSplatUpload)))) {
            terrainSplatMap.Reset();
            return false;
        }

        uint8_t* mapped = nullptr;
        if (FAILED(terrainSplatUpload->Map(
                0, nullptr, reinterpret_cast<void**>(&mapped)))) {
            terrainSplatMap.Reset();
            terrainSplatUpload.Reset();
            return false;
        }
        // Row pitch is 256-aligned by D3D12, so this copies row by row rather
        // than as one block.
        const size_t sourceRowPitch = static_cast<size_t>(resolution) * 4;
        for (UINT row = 0; row < rows; ++row)
            std::memcpy(mapped + static_cast<size_t>(row) *
                            layout.Footprint.RowPitch,
                        rgba + static_cast<size_t>(row) * sourceRowPitch,
                        sourceRowPitch);
        terrainSplatUpload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = terrainSplatMap.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = terrainSplatUpload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = layout;
        g_dx12.commandList->CopyTextureRegion(&destination, 0, 0, 0, &source,
                                              nullptr);

        // The resolve is a compute shader, so this lands in NON_PIXEL unlike
        // the forward-sampled layer arrays above.
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = terrainSplatMap.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_dx12.commandList->ResourceBarrier(1, &barrier);

        terrainSplatResolution = resolution;
        return true;
    }

    bool CreateTerrainTextureArrays(ShaderDX12& shader) {
        // Preserve 2K terrain scans at native resolution. Sand relies on fine
        // wind-carved ridges that disappeared in the old 1K array.
        constexpr UINT side = 2048;
        constexpr UINT layers = 4;
        constexpr UINT bytesPerPixel = 4;
        const size_t layerBytes = static_cast<size_t>(side) * side * bytesPerPixel;
        std::array<std::vector<uint8_t>, 3> maps;
        for (auto& map : maps) map.resize(layerBytes * layers);

        const std::array<std::array<float, 3>, layers> baseColors = {{
            {{ 0.25f, 0.43f, 0.12f }},
            {{ 0.34f, 0.20f, 0.10f }},
            {{ 0.72f, 0.58f, 0.36f }},
            {{ 0.31f, 0.32f, 0.30f }}
        }};
        const std::array<float, layers> baseRoughness = {
            0.88f, 0.94f, 0.82f, 0.76f
        };
        auto height = [](int x, int y, UINT layer) {
            constexpr float twoPi = 6.28318530718f;
            const float fx = static_cast<float>(x & 255) / 256.0f;
            const float fy = static_cast<float>(y & 255) / 256.0f;
            const float phase = static_cast<float>(layer) * 1.731f;
            float value = 0.50f;
            value += std::sin((fx * (5.0f + layer) + phase) * twoPi) * 0.18f;
            value += std::sin((fy * (7.0f + layer * 2.0f) - phase) * twoPi) * 0.14f;
            value += std::sin(((fx + fy) * 17.0f + phase * 0.37f) * twoPi) * 0.08f;
            value += std::sin(((fx - fy) * 31.0f - phase * 0.61f) * twoPi) * 0.035f;
            return value;
        };
        auto byte = [](float value) {
            return static_cast<uint8_t>((std::max)(0.0f,
                (std::min)(1.0f, value)) * 255.0f + 0.5f);
        };

        for (UINT layer = 0; layer < layers; ++layer) {
            for (UINT y = 0; y < side; ++y) for (UINT x = 0; x < side; ++x) {
                const float h = height(static_cast<int>(x), static_cast<int>(y), layer);
                const float dx = height(static_cast<int>(x) + 1,
                    static_cast<int>(y), layer) - height(static_cast<int>(x) - 1,
                    static_cast<int>(y), layer);
                const float dy = height(static_cast<int>(x),
                    static_cast<int>(y) + 1, layer) - height(static_cast<int>(x),
                    static_cast<int>(y) - 1, layer);
                const float strength = layer == 2 ? 2.0f : (layer == 3 ? 5.5f : 3.8f);
                XMVECTOR vectorNormal = XMVector3Normalize(XMVectorSet(
                    -dx * strength, -dy * strength, 1.0f, 0.0f));
                XMFLOAT3 normal;
                XMStoreFloat3(&normal, vectorNormal);
                const size_t offset = static_cast<size_t>(layer) * layerBytes +
                    (static_cast<size_t>(y) * side + x) * bytesPerPixel;
                const float variation = 0.76f + h * 0.43f;
                // Pixel shader decodes albedo from sRGB. Encode procedural linear
                // colors here; writing linear values directly caused a second
                // gamma operation and crushed dirt/rock almost to black.
                maps[0][offset + 0] = byte(std::pow(
                    (std::min)(1.0f, baseColors[layer][0] * variation), 1.0f / 2.2f));
                maps[0][offset + 1] = byte(std::pow(
                    (std::min)(1.0f, baseColors[layer][1] * variation), 1.0f / 2.2f));
                maps[0][offset + 2] = byte(std::pow(
                    (std::min)(1.0f, baseColors[layer][2] * variation), 1.0f / 2.2f));
                maps[0][offset + 3] = 255;
                maps[1][offset + 0] = byte(normal.x * 0.5f + 0.5f);
                maps[1][offset + 1] = byte(normal.y * 0.5f + 0.5f);
                maps[1][offset + 2] = byte(normal.z * 0.5f + 0.5f);
                maps[1][offset + 3] = 255;
                const uint8_t rough = byte(
                    baseRoughness[layer] + (h - 0.5f) * 0.12f);
                maps[2][offset + 0] = 255;
                maps[2][offset + 1] = rough;
                maps[2][offset + 2] = 0;
                maps[2][offset + 3] = 255;
            }
        }

        // Replace all generated fallback slices with Poly Haven CC0 scans.
        auto resolveTerrainMap = [](const char* folder, const char* file) {
            for (const std::filesystem::path root : {
                    std::filesystem::path("Content/Models"),
                    std::filesystem::path("build/Content/Models"),
                    std::filesystem::path("../Content/Models") }) {
                const std::filesystem::path path = root / folder / file;
                if (std::filesystem::exists(path)) return path.string();
            }
            return std::string(file);
        };
        auto loadTerrainSlice = [&](const char* folder, const char* file,
                                    UINT layer, std::vector<uint8_t>& target,
                                    bool normalSource, bool roughnessSource,
                                    bool ambientOcclusionSource = false) {
            std::vector<unsigned char> source;
            int width = 0, heightPixels = 0;
            if (!GLBImporter::LoadPixelsRGBA(
                    resolveTerrainMap(folder, file), source, width, heightPixels) ||
                width <= 0 || heightPixels <= 0) return false;
            for (UINT y = 0; y < side; ++y) for (UINT x = 0; x < side; ++x) {
                const int sx0 = static_cast<int>((uint64_t)x * width / side);
                const int sx1 = (std::max)(sx0 + 1,
                    static_cast<int>((uint64_t)(x + 1) * width / side));
                const int sy0 = static_cast<int>((uint64_t)y * heightPixels / side);
                const int sy1 = (std::max)(sy0 + 1,
                    static_cast<int>((uint64_t)(y + 1) * heightPixels / side));
                uint64_t sums[3] = {};
                uint64_t count = 0;
                for (int sy = sy0; sy < (std::min)(sy1, heightPixels); ++sy)
                    for (int sx = sx0; sx < (std::min)(sx1, width); ++sx) {
                        const size_t sourceOffset =
                            (static_cast<size_t>(sy) * width + sx) * 4;
                        sums[0] += source[sourceOffset + 0];
                        sums[1] += source[sourceOffset + 1];
                        sums[2] += source[sourceOffset + 2];
                        ++count;
                }
                const size_t destination =
                    static_cast<size_t>(layer) * layerBytes +
                    (static_cast<size_t>(y) * side + x) * 4;
                if (ambientOcclusionSource) {
                    target[destination + 0] =
                        static_cast<uint8_t>(sums[0] / count);
                } else if (roughnessSource) {
                    target[destination + 0] = 255;
                    target[destination + 1] = static_cast<uint8_t>(sums[0] / count);
                    target[destination + 2] = 0;
                } else if (normalSource) {
                    XMVECTOR n = XMVector3Normalize(XMVectorSet(
                        static_cast<float>(sums[0] / count) / 127.5f - 1.0f,
                        static_cast<float>(sums[1] / count) / 127.5f - 1.0f,
                        static_cast<float>(sums[2] / count) / 127.5f - 1.0f,
                        0.0f));
                    XMFLOAT3 decoded;
                    XMStoreFloat3(&decoded, n);
                    target[destination + 0] = byte(decoded.x * 0.5f + 0.5f);
                    target[destination + 1] = byte(decoded.y * 0.5f + 0.5f);
                    target[destination + 2] = byte(decoded.z * 0.5f + 0.5f);
                } else {
                    const float exposure = layer == 0 ? 1.25f : 1.0f;
                    target[destination + 0] = byte(
                        static_cast<float>(sums[0] / count) / 255.0f * exposure);
                    target[destination + 1] = byte(
                        static_cast<float>(sums[1] / count) / 255.0f * exposure);
                    target[destination + 2] = byte(
                        static_cast<float>(sums[2] / count) / 255.0f * exposure);
                }
                target[destination + 3] = 255;
            }
            return true;
        };
        struct TerrainAsset {
            const char* folder;
            const char* albedo;
            const char* normal;
            const char* roughness;
            const char* ambientOcclusion;
        };
        static constexpr TerrainAsset assets[layers] = {
            { "Grass3/Grass004_2K-JPG", "Grass004_2K-JPG_Color.jpg",
              "Grass004_2K-JPG_NormalGL.jpg",
              "Grass004_2K-JPG_Roughness.jpg",
              "Grass004_2K-JPG_AmbientOcclusion.jpg" },
            { "terrain/dirt_floor", "dirt_floor_diff_1k.png",
              "dirt_floor_nor_gl_1k.png", "dirt_floor_rough_1k.png", nullptr },
            { "terrain/aerial_beach_01", "aerial_beach_01_diff_2k.png",
              "aerial_beach_01_nor_gl_2k.png",
              "aerial_beach_01_rough_2k.png",
              "aerial_beach_01_ao_2k.png" },
            { "terrain/dark_rock", "dark_rock_diff_1k.png",
              "dark_rock_nor_gl_1k.png", "dark_rock_rough_1k.png", nullptr }
        };
        for (UINT layer = 0; layer < layers; ++layer) {
            const bool albedoLoaded = loadTerrainSlice(
                assets[layer].folder, assets[layer].albedo, layer,
                maps[0], false, false);
            const bool normalLoaded = loadTerrainSlice(
                assets[layer].folder, assets[layer].normal, layer,
                maps[1], true, false);
            const bool roughnessLoaded = loadTerrainSlice(
                assets[layer].folder, assets[layer].roughness, layer,
                maps[2], false, true);
            const bool aoLoaded = !assets[layer].ambientOcclusion ||
                loadTerrainSlice(assets[layer].folder,
                    assets[layer].ambientOcclusion, layer,
                    maps[2], false, false, true);
            if (!albedoLoaded || !normalLoaded || !roughnessLoaded ||
                !aoLoaded) {
                std::cerr << "Terrain PBR: " << assets[layer].folder
                          << " maps missing (albedo=" << albedoLoaded
                          << ", normal=" << normalLoaded
                          << ", roughness=" << roughnessLoaded
                          << ", ao=" << aoLoaded
                          << "); using generated fallback slice\n";
            }
        }

        if (!CreateTextureArray(maps[0], side, layers, true,
                                terrainAlbedoArray, terrainUploads[0])) {
            std::cerr << "Terrain PBR: failed to create albedo texture array\n";
            return false;
        }
        if (!CreateTextureArray(maps[1], side, layers, false,
                                terrainNormalArray, terrainUploads[1])) {
            std::cerr << "Terrain PBR: failed to create normal texture array\n";
            return false;
        }
        if (!CreateTextureArray(maps[2], side, layers, false,
                                terrainRoughnessArray, terrainUploads[2])) {
            std::cerr << "Terrain PBR: failed to create roughness texture array\n";
            return false;
        }

        const UINT slot = shader.ReservePersistentMaterialSrvs();
        if (slot == ~0u) {
            std::cerr << "Terrain PBR: persistent descriptor allocation failed\n";
            return false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        ShaderDX12::SrvHandlesAt(slot, cpu, terrainTextureTable);
        const UINT stride = g_dx12.cbvSrvUavDescriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.MipLevels = terrainAlbedoArray->GetDesc().MipLevels;
        srv.Texture2DArray.ArraySize = layers;
        g_dx12.device->CreateShaderResourceView(terrainAlbedoArray.Get(), &srv, cpu);
        cpu.ptr += stride;
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        g_dx12.device->CreateShaderResourceView(terrainNormalArray.Get(), &srv, cpu);
        cpu.ptr += stride;
        g_dx12.device->CreateShaderResourceView(terrainRoughnessArray.Get(), &srv, cpu);
        return true;
    }

    // Caller must have bound matrices (SetMatrices, model = identity) and the
    // terrain material (SetObjectMaterial) beforehand, same as any other draw.
    void Draw(ShaderDX12& shader, const Params& params) {
        if (!supported) return;
        shader.RebindGraphicsResourceTables();
        commandList6->SetGraphicsRootDescriptorTable(7, terrainTextureTable);
        ID3D12PipelineState* solid = hdrTargetEnabled
            ? psoHDR.Get() : (msaaEnabled ? psoMSAA.Get() : pso.Get());
        ID3D12PipelineState* wire = hdrTargetEnabled
            ? psoWireframeHDR.Get()
            : (msaaEnabled ? psoWireframeMSAA.Get() : psoWireframe.Get());
        commandList6->SetPipelineState((wireframe && wire) ? wire : solid);
        Params drawParams = params;
        drawParams.sculptCount = static_cast<UINT>(s_sculptStamps.size());
        drawParams.sculptMaxDisplacement = m_sculptMaxDisplacement;
        UploadSculptStamps(g_dx12.frameIndex);
        UploadStampAtlas(g_dx12.frameIndex);
        commandList6->SetGraphicsRoot32BitConstants(8, 16, &drawParams, 0);
        commandList6->SetGraphicsRootShaderResourceView(
            13, sculptBuffers[g_dx12.frameIndex]->GetGPUVirtualAddress());
        commandList6->SetGraphicsRootShaderResourceView(
            14, stampAtlasBuffers[g_dx12.frameIndex]->GetGPUVirtualAddress());
        commandList6->DispatchMesh((TileCount(params) + 31) / 32, 1, 1);
    }

    bool VisibilitySupported() const {
        return supported && psoVisibility;
    }

    // Rasterizes terrain into the visibility buffer: same amplification/mesh
    // shaders and therefore the same clipmap, culling and LOD as Draw(), but
    // the pixel shader writes a reserved ID plus the packed geometric normal
    // instead of shading. The resolve rebuilds the surface from that and depth.
    //
    // Returns false when the visibility PSO is unavailable, which is the signal
    // to fall back to the forward terrain draw for this frame.
    // Switches the command list to the main graphics root signature, which the
    // terrain AS/MS need and the visibility pass does not use.
    //
    // The visibility pass runs under visPassRootSig: four root parameters, no
    // camera, no terrain params, no sculpt buffer. Terrain writes slots 7, 8
    // and 13, which do not exist there -- an out-of-range root write, and what
    // hung the GPU. The caller binds matrices/camera after this returns, so
    // those land against the signature that actually declares them.
    bool PrepareVisibilityRootSignature(ShaderDX12& shader) {
        if (!VisibilitySupported() || !shader.rootSignature) return false;
        commandList6->SetGraphicsRootSignature(shader.rootSignature.Get());
        return true;
    }

    // Depth-only terrain for a shadow pass.
    //
    // Runs under the main graphics root signature, not DepthOnlyShaderDX12's:
    // the terrain amplification and mesh shaders read the camera, terrain
    // params and sculpt buffers through slots that only exist there. The
    // caller therefore binds matrices with lightSpace as the view-projection
    // through ShaderDX12 before calling this, and must restore the depth
    // shader's root signature afterwards for the remaining casters.
    //
    // Culling still runs against whatever the AS was given, so a light-space
    // matrix here culls tiles to the light's frustum, which is what makes the
    // pass affordable for a spot caster covering a small cone.
    bool DrawShadow(ShaderDX12& shader, const Params& params) {
        if (!supported || !psoShadow || !shader.rootSignature) return false;
        shader.RebindGraphicsResourceTables();
        commandList6->SetGraphicsRootDescriptorTable(7, terrainTextureTable);
        commandList6->SetPipelineState(psoShadow.Get());
        Params drawParams = params;
        drawParams.sculptCount = static_cast<UINT>(s_sculptStamps.size());
        drawParams.sculptMaxDisplacement = m_sculptMaxDisplacement;
        UploadSculptStamps(g_dx12.frameIndex);
        UploadStampAtlas(g_dx12.frameIndex);
        commandList6->SetGraphicsRoot32BitConstants(8, 16, &drawParams, 0);
        commandList6->SetGraphicsRootShaderResourceView(
            13, sculptBuffers[g_dx12.frameIndex]->GetGPUVirtualAddress());
        commandList6->SetGraphicsRootShaderResourceView(
            14, stampAtlasBuffers[g_dx12.frameIndex]->GetGPUVirtualAddress());
        commandList6->DispatchMesh((TileCount(params) + 31) / 32, 1, 1);
        return true;
    }

    bool DrawVisibility(ShaderDX12& shader, const Params& params) {
        if (!VisibilitySupported() || !shader.rootSignature) return false;
        // The visibility pixel shader samples nothing, but the amplification and
        // mesh shaders still read the matrix/camera/sculpt bindings through the
        // shared root signature, so the same tables Draw() needs are bound here.
        shader.RebindGraphicsResourceTables();
        commandList6->SetGraphicsRootDescriptorTable(7, terrainTextureTable);
        commandList6->SetPipelineState(psoVisibility.Get());
        Params drawParams = params;
        drawParams.sculptCount = static_cast<UINT>(s_sculptStamps.size());
        drawParams.sculptMaxDisplacement = m_sculptMaxDisplacement;
        UploadSculptStamps(g_dx12.frameIndex);
        UploadStampAtlas(g_dx12.frameIndex);
        commandList6->SetGraphicsRoot32BitConstants(8, 16, &drawParams, 0);
        commandList6->SetGraphicsRootShaderResourceView(
            13, sculptBuffers[g_dx12.frameIndex]->GetGPUVirtualAddress());
        commandList6->SetGraphicsRootShaderResourceView(
            14, stampAtlasBuffers[g_dx12.frameIndex]->GetGPUVirtualAddress());
        commandList6->DispatchMesh((TileCount(params) + 31) / 32, 1, 1);
        return true;
    }

    // Clipmap (terrainStyle bit 1): tilesX=ring grid G, tilesZ=ring count R.
    // Total tiles = ring0 (G*G) + (R-1) hollow rings (G*G - (G/2)^2 each).
    static UINT TileCount(const Params& params) {
        constexpr UINT kClipmapFlag = 2u;
        if ((params.terrainStyle & kClipmapFlag) != 0u) {
            const UINT G = params.tilesX, R = params.tilesZ;
            const UINT ring0 = G * G;
            const UINT ringN = ring0 - (G / 2) * (G / 2);
            return ring0 + (R > 0 ? (R - 1) * ringN : 0);
        }
        return params.tilesX * params.tilesZ;
    }

private:
    static void EnsureStampLibrary() {
        if (!s_stampNames.empty() || !s_stampAtlas.empty()) return;
        s_stampNames = DiscoverTerrainStampNames();
        s_stampLoadState.assign(s_stampNames.size(), 0u);
        s_stampWriteTimes.assign(
            s_stampNames.size(), std::filesystem::file_time_type{});
        s_stampAtlas.assign(static_cast<size_t>(kMaxTerrainStampTextures) *
            kTerrainStampResolution * kTerrainStampResolution, 32768u);
    }

    static UINT FindHeightStampLayer(const std::string& texture) {
        EnsureStampLibrary();
        const auto found = std::find(
            s_stampNames.begin(), s_stampNames.end(), texture);
        if (found == s_stampNames.end() || *found != texture)
            return UINT_MAX;
        const size_t layer = static_cast<size_t>(found - s_stampNames.begin());
        return s_stampLoadState[layer] == 1u ? static_cast<UINT>(layer) : UINT_MAX;
    }

    static UINT EnsureHeightStampLoaded(const std::string& texture) {
        EnsureStampLibrary();
        if (!IsTerrainStampFilename(texture)) return UINT_MAX;
        auto found = std::find(
            s_stampNames.begin(), s_stampNames.end(), texture);
        if (found == s_stampNames.end()) {
            // Editor bakes are written after the startup directory scan. Keep
            // existing layer indices stable and append the new file instead of
            // sorting it into the middle of the already-populated GPU atlas.
            if (s_stampNames.size() >= kMaxTerrainStampTextures)
                return UINT_MAX;
            s_stampNames.push_back(texture);
            s_stampLoadState.push_back(0u);
            s_stampWriteTimes.push_back(
                std::filesystem::file_time_type{});
            found = s_stampNames.end() - 1;
        }
        const size_t layer = static_cast<size_t>(found - s_stampNames.begin());
        const std::filesystem::path path = TerrainStampDirectory() / texture;
        std::error_code writeTimeError;
        const std::filesystem::file_time_type writeTime =
            std::filesystem::last_write_time(path, writeTimeError);
        const bool unchanged = !writeTimeError &&
            s_stampWriteTimes[layer] == writeTime;
        if (s_stampLoadState[layer] == 1u &&
            (unchanged || writeTimeError))
            return static_cast<UINT>(layer);
        if (s_stampLoadState[layer] == 2u &&
            (unchanged || writeTimeError))
            return UINT_MAX;

        std::vector<uint16_t> source;
        int width = 0, height = 0;
        if (!GLBImporter::LoadPixelsGray16(path.string(), source, width, height) ||
            width <= 0 || height <= 0) {
            s_stampLoadState[layer] = 2u;
            if (!writeTimeError) s_stampWriteTimes[layer] = writeTime;
            return UINT_MAX;
        }

        const size_t layerOffset = layer * kTerrainStampResolution *
            kTerrainStampResolution;
        // Box filter over each output texel's exact source footprint. This used
        // to take a fixed 4x4 stratified tap, which was sized for 4096->256; at
        // 512 that covers a quarter of the 8x8 footprint and starts aliasing
        // again. Averaging the real footprint stays correct whatever the source
        // dimensions and atlas resolution are, and first use of a stamp reads
        // each source texel exactly once.
        for (uint32_t y = 0; y < kTerrainStampResolution; ++y) {
            const int y0 = static_cast<int>(static_cast<uint64_t>(y) * height /
                                            kTerrainStampResolution);
            const int y1 = (std::max)(y0 + 1,
                static_cast<int>(static_cast<uint64_t>(y + 1) * height /
                                 kTerrainStampResolution));
            for (uint32_t x = 0; x < kTerrainStampResolution; ++x) {
                const int x0 = static_cast<int>(static_cast<uint64_t>(x) * width /
                                                kTerrainStampResolution);
                const int x1 = (std::max)(x0 + 1,
                    static_cast<int>(static_cast<uint64_t>(x + 1) * width /
                                     kTerrainStampResolution));
                uint64_t sum = 0;
                uint32_t count = 0;
                for (int sy = y0; sy < y1 && sy < height; ++sy) {
                    for (int sx = x0; sx < x1 && sx < width; ++sx) {
                        sum += source[static_cast<size_t>(sy) * width + sx];
                        ++count;
                    }
                }
                s_stampAtlas[layerOffset +
                    static_cast<size_t>(y) * kTerrainStampResolution + x] =
                    count ? static_cast<uint16_t>((sum + count / 2) / count)
                          : uint16_t{ 32768u };
            }
        }
        s_stampLoadState[layer] = 1u;
        if (!writeTimeError) s_stampWriteTimes[layer] = writeTime;
        ++s_stampAtlasRevision;
        return static_cast<UINT>(layer);
    }

    // outRelief is the stamp's displacement in metres; outCoverage is how much
    // of the square this point is inside (1 in the middle, feathering to 0 at
    // the border). Replace mode needs the coverage separately -- it has to know
    // where the stamp *is*, not just how tall it is, so flat parts of a
    // heightmap still overwrite the ground under them.
    static void SampleHeightStamp(const TerrainSculptStamp& stamp,
                                  float x, float z,
                                  float& outRelief, float& outCoverage) {
        outRelief = 0.0f;
        outCoverage = 0.0f;
        const UINT layer = FindHeightStampLayer(stamp.texture);
        if (layer == UINT_MAX || stamp.radius <= 0.0f) return;
        const float angle = DirectX::XMConvertToRadians(stamp.rotation);
        const float cosine = cosf(angle), sine = sinf(angle);
        const float dx = x - stamp.x, dz = z - stamp.z;
        const float localX = dx * cosine + dz * sine;
        const float localZ = -dx * sine + dz * cosine;
        const float u = localX / (stamp.radius * 2.0f) + 0.5f;
        const float v = localZ / (stamp.radius * 2.0f) + 0.5f;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return;

        const float fx = u * (kTerrainStampResolution - 1u);
        const float fy = v * (kTerrainStampResolution - 1u);
        const uint32_t x0 = static_cast<uint32_t>(fx);
        const uint32_t y0 = static_cast<uint32_t>(fy);
        const uint32_t x1 = (std::min)(x0 + 1u, kTerrainStampResolution - 1u);
        const uint32_t y1 = (std::min)(y0 + 1u, kTerrainStampResolution - 1u);
        const size_t base = static_cast<size_t>(layer) *
            kTerrainStampResolution * kTerrainStampResolution;
        const auto texel = [&](uint32_t tx, uint32_t ty) {
            return static_cast<float>(s_stampAtlas[base +
                static_cast<size_t>(ty) * kTerrainStampResolution + tx]);
        };
        const float tx = fx - x0, ty = fy - y0;
        const float upper = texel(x0, y0) +
            (texel(x1, y0) - texel(x0, y0)) * tx;
        const float lower = texel(x0, y1) +
            (texel(x1, y1) - texel(x0, y1)) * tx;
        const float normalized = (upper + (lower - upper) * ty) /
            65535.0f * 2.0f - 1.0f;
        float edge = ((std::max)(std::abs(localX), std::abs(localZ)) /
                      stamp.radius - 0.82f) / 0.18f;
        edge = edge < 0.0f ? 0.0f : (edge > 1.0f ? 1.0f : edge);
        edge = 1.0f - edge * edge * (3.0f - 2.0f * edge);
        outCoverage = edge;
        outRelief = normalized * stamp.value * edge;
    }

    inline static std::vector<TerrainSculptStamp> s_sculptStamps;
    inline static std::vector<std::string> s_stampNames;
    inline static std::vector<uint8_t> s_stampLoadState;
    inline static std::vector<std::filesystem::file_time_type>
        s_stampWriteTimes;
    inline static std::vector<uint16_t> s_stampAtlas;
    inline static uint64_t s_sculptRevision = 1;
    inline static uint64_t s_stampAtlasRevision = 1;
    float m_sculptMaxDisplacement = 0.0f;
};

#endif // TERRAIN_RENDERER_DX12_H
