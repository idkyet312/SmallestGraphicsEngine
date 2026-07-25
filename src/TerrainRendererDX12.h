#ifndef TERRAIN_RENDERER_DX12_H
#define TERRAIN_RENDERER_DX12_H

#include "MeshShaderDX12.h" // MeshPSOSubobjectDX12 template + ShaderDX12
#include "GLBImporter.h"
#include "LevelDefinition.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
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
        float heightScale = 5.0f;
        float lodNear = 24.0f;
        float lodStep = 28.0f;
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
    };
    // Root param 8 is 15 DWORDs; Draw uploads 15. Keep the struct exactly that
    // size so the upload never reads past it or shifts the cbuffer layout.
    static_assert(sizeof(Params) == 15 * sizeof(UINT),
                  "TerrainParams must be exactly 15 DWORDs (matches root const upload)");

    struct SculptGPU {
        float x, z, radius;
        UINT operation;
        float value, strength;
        float padding[2] = {};
    };
    static_assert(sizeof(SculptGPU) == 32);

    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12PipelineState> psoWireframe;
    ComPtr<ID3D12PipelineState> psoMSAA;
    ComPtr<ID3D12PipelineState> psoWireframeMSAA;
    ComPtr<ID3D12PipelineState> psoHDR;
    ComPtr<ID3D12PipelineState> psoWireframeHDR;
    ComPtr<ID3D12GraphicsCommandList6> commandList6;
    ComPtr<ID3D12Resource> terrainAlbedoArray;
    ComPtr<ID3D12Resource> terrainNormalArray;
    ComPtr<ID3D12Resource> terrainRoughnessArray;
    std::array<ComPtr<ID3D12Resource>, FRAME_COUNT> sculptBuffers;
    std::array<ComPtr<ID3D12Resource>, 3> terrainUploads;
    D3D12_GPU_DESCRIPTOR_HANDLE terrainTextureTable{};
    bool supported = false;
    bool msaaSupported = false;
    bool msaaEnabled = false;
    bool wireframe = false; // Z key: draw terrain tiles as wireframe
    bool hdrTargetEnabled = false;

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
        supported = true;
        return true;
    }

    void SetSculptStamps(const std::vector<TerrainSculptStamp>& stamps) {
        s_sculptStamps.assign(stamps.begin(), stamps.begin() +
            (std::min)(stamps.size(), static_cast<size_t>(256)));
        m_sculptMaxDisplacement = 0.0f;
        for (const TerrainSculptStamp& stamp : s_sculptStamps) {
            if (stamp.operation == TerrainSculptOperation::Add)
                m_sculptMaxDisplacement += std::abs(stamp.value);
            else
                m_sculptMaxDisplacement = (std::max)(m_sculptMaxDisplacement,
                    std::abs(stamp.value) + 12.0f);
        }
    }

    void SetMSAAEnabled(bool enabled) {
        msaaEnabled = enabled && msaaSupported;
    }

    void SetHDRTargetEnabled(bool enabled) { hdrTargetEnabled = enabled; }

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

        // Outside the tiled extent there is no drawn ground; treat as level 0.
        // The grid's min corner is offset by originTile* so an edge-extended
        // island can grow off-center; match terrain_as/ms.hlsl's tile origin.
        const float minX = (static_cast<float>(params.originTileX) -
            params.tilesX * 0.5f) * params.tileSize;
        const float minZ = (static_cast<float>(params.originTileZ) -
            params.tilesZ * 0.5f) * params.tileSize;
        const float maxX = minX + params.tilesX * params.tileSize;
        const float maxZ = minZ + params.tilesZ * params.tileSize;
        if (x < minX || x >= maxX || z < minZ || z >= maxZ) return 0.0f;

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

        // Island falloff -- must match terrain_ms.hlsl's TerrainHeight. Land is
        // lifted above sea level (y = 0), then ramps down to a seabed past the
        // shore, so the island is ringed by ocean.
        constexpr float landLift = 2.5f, seabed = -6.0f;
        constexpr float shoreInner = 34.0f;
        constexpr float shoreOuter = 52.0f;
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
        if (params.terrainStyle == 1u && maxScale > 1.5f) {
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
        float st = (coastDistance - shoreInner) / (shoreOuter - shoreInner);
        st = st < 0.0f ? 0.0f : (st > 1.0f ? 1.0f : st);
        float shore = st * st * (3.0f - 2.0f * st);          // smoothstep
        float land = h + landLift;
        h = land + (seabed - land) * shore;                  // lerp(land, seabed, shore)

        // Flat arenas under each house compound, applied LAST so neither noise nor
        // the pool rim can dent them. Must match terrain_ms.hlsl's TerrainHeight;
        // padHeight is Ground::kBuildingPadY (src/GroundLevel.h), which the houses
        // and roofs are built from -- change it there and here together.
        constexpr float padRadius = 14.0f, padFade = 18.0f, padHeight = 2.5f;
        static constexpr float padCenters[8][2] = {
            {  0.0f,   0.0f }, { 42.0f,   0.0f },
            {-42.0f,   0.0f }, {  0.0f,  42.0f },
            { 42.0f,  42.0f }, {-42.0f,  42.0f },
            {  0.0f, -42.0f }, { 42.0f, -42.0f }
        };
        const int padCount = (params.terrainStyle == 1u && maxScale > 1.5f) ? 8 : 1;
        for (int i = 0; i < padCount; ++i) {
            float dpx = x - padCenters[i][0], dpz = z - padCenters[i][1];
            float dpad = sqrtf(dpx * dpx + dpz * dpz);
            float pt = (dpad - padRadius) / (padFade - padRadius);
            pt = pt < 0.0f ? 0.0f : (pt > 1.0f ? 1.0f : pt);
            float pad = 1.0f - pt * pt * (3.0f - 2.0f * pt);
            h = h + (padHeight - h) * pad;
        }
        for (const TerrainSculptStamp& stamp : sculpt) {
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
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = sizeof(SculptGPU) * 256;
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
        return true;
    }

    void UploadSculptStamps(UINT frame) {
        if (frame >= FRAME_COUNT || !sculptBuffers[frame]) return;
        SculptGPU* destination = nullptr;
        if (FAILED(sculptBuffers[frame]->Map(0, nullptr,
                reinterpret_cast<void**>(&destination)))) return;
        std::memset(destination, 0, sizeof(SculptGPU) * 256);
        for (size_t i = 0; i < s_sculptStamps.size(); ++i) {
            const TerrainSculptStamp& source = s_sculptStamps[i];
            destination[i] = { source.x, source.z, source.radius,
                static_cast<UINT>(source.operation), source.value, source.strength };
        }
        sculptBuffers[frame]->Unmap(0, nullptr);
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

        uint8_t* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
            return false;
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
        upload->Unmap(0, nullptr);

        for (UINT subresource = 0; subresource < subresourceCount; ++subresource) {
            D3D12_TEXTURE_COPY_LOCATION destination = {};
            destination.pResource = texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = subresource;
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = upload.Get();
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

    bool CreateTerrainTextureArrays(ShaderDX12& shader) {
        // Keep the authored 1K scans intact. The previous 256px arrays erased
        // most fine ground detail before mip filtering even began.
        constexpr UINT side = 1024;
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
                    std::filesystem::path("models"),
                    std::filesystem::path("build/models"),
                    std::filesystem::path("../models") }) {
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
            { "terrain/coast_sand_01", "coast_sand_01_diff_1k.png",
              "coast_sand_01_nor_gl_1k.png",
              "coast_sand_01_rough_1k.png", nullptr },
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
        commandList6->SetGraphicsRoot32BitConstants(8, 15, &drawParams, 0);
        commandList6->SetGraphicsRootShaderResourceView(
            13, sculptBuffers[g_dx12.frameIndex]->GetGPUVirtualAddress());
        const UINT tileCount = params.tilesX * params.tilesZ;
        commandList6->DispatchMesh((tileCount + 31) / 32, 1, 1);
    }

private:
    inline static std::vector<TerrainSculptStamp> s_sculptStamps;
    float m_sculptMaxDisplacement = 0.0f;
};

#endif // TERRAIN_RENDERER_DX12_H
