#ifndef TERRAIN_RENDERER_DX12_H
#define TERRAIN_RENDERER_DX12_H

#include "MeshShaderDX12.h" // MeshPSOSubobjectDX12 template + ShaderDX12

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
        float islandScale = 1.0f;
    };

    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12PipelineState> psoWireframe;
    ComPtr<ID3D12PipelineState> psoMSAA;
    ComPtr<ID3D12PipelineState> psoWireframeMSAA;
    ComPtr<ID3D12PipelineState> psoHDR;
    ComPtr<ID3D12PipelineState> psoWireframeHDR;
    ComPtr<ID3D12GraphicsCommandList6> commandList6;
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
        if (FAILED(ReadCompiledShaderDX12(L"shaders/mesh_ps.cso", &ps))) {
            std::cerr << "Mesh pixel shader DXIL missing: shaders/mesh_ps.cso\n";
            return false;
        }
        if (FAILED(ReadCompiledShaderDX12(L"shaders/mesh_ps_hdr.cso", &hdrPs))) {
            std::cerr << "HDR terrain pixel shader DXIL missing: shaders/mesh_ps_hdr.cso\n";
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

        supported = true;
        return true;
    }

    void SetMSAAEnabled(bool enabled) {
        msaaEnabled = enabled && msaaSupported;
    }

    void SetHDRTargetEnabled(bool enabled) { hdrTargetEnabled = enabled; }

    // CPU mirror of terrain_ms.hlsl's height function (hash21/noise2/fbm/
    // TerrainHeight), used for walking collision. Keep the two in sync - any
    // drift puts the camera above or inside the rendered ground.
    static float HeightAt(const Params& params, float x, float z) {
        auto fract = [](float v) { return v - floorf(v); };
        auto hash21 = [&](float px, float py) {
            px = fract(px * 123.34f);
            py = fract(py * 456.21f);
            float d = px * (px + 45.32f) + py * (py + 45.32f);
            px += d; py += d;
            return fract(px * py);
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
        float halfX = params.tilesX * params.tileSize * 0.5f;
        float halfZ = params.tilesZ * params.tileSize * 0.5f;
        if (x < -halfX || x >= halfX || z < -halfZ || z >= halfZ) return 0.0f;

        float px = x * 0.08f, py = z * 0.08f;
        float sum = 0.0f, amp = 0.5f;
        for (int i = 0; i < 5; ++i) {
            sum += noise2(px, py) * amp;
            px *= 2.02f; py *= 2.02f;
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
        const float shoreInner = 34.0f * params.islandScale;
        const float shoreOuter = 52.0f * params.islandScale;
        // Warp an oval coastline, then cut a northwest bay and extend a
        // southeast headland. This avoids the artificial circular-atoll shape.
        float coastDistance = sqrtf(x * x + z * z);
        auto coastLobe = [](float distance, float radius) {
            float t = distance / radius;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            return 1.0f - t * t * (3.0f - 2.0f * t);
        };
        if (params.islandScale > 1.5f) {
            const float warpedX = x + sinf(z * 0.055f) * 7.0f +
                sinf((x + z) * 0.025f) * 4.0f;
            const float warpedZ = z + sinf(x * 0.047f) * 6.0f -
                sinf((x - z) * 0.031f) * 3.0f;
            coastDistance = sqrtf(
                warpedX * warpedX * 0.92f * 0.92f +
                warpedZ * warpedZ * 1.06f * 1.06f);
            coastDistance += coastLobe(
                sqrtf((x + 55.0f) * (x + 55.0f) +
                      (z - 15.0f) * (z - 15.0f)), 22.0f) * 13.0f;
            coastDistance -= coastLobe(
                sqrtf((x - 35.0f) * (x - 35.0f) +
                      (z + 55.0f) * (z + 55.0f)), 26.0f) * 11.0f;
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
        const int padCount = params.islandScale > 1.5f ? 8 : 1;
        for (int i = 0; i < padCount; ++i) {
            float dpx = x - padCenters[i][0], dpz = z - padCenters[i][1];
            float dpad = sqrtf(dpx * dpx + dpz * dpz);
            float pt = (dpad - padRadius) / (padFade - padRadius);
            pt = pt < 0.0f ? 0.0f : (pt > 1.0f ? 1.0f : pt);
            float pad = 1.0f - pt * pt * (3.0f - 2.0f * pt);
            h = h + (padHeight - h) * pad;
        }
        return h;
    }

    // Caller must have bound matrices (SetMatrices, model = identity) and the
    // terrain material (SetObjectMaterial) beforehand, same as any other draw.
    void Draw(const Params& params) {
        if (!supported) return;
        ID3D12PipelineState* solid = hdrTargetEnabled
            ? psoHDR.Get() : (msaaEnabled ? psoMSAA.Get() : pso.Get());
        ID3D12PipelineState* wire = hdrTargetEnabled
            ? psoWireframeHDR.Get()
            : (msaaEnabled ? psoWireframeMSAA.Get() : psoWireframe.Get());
        commandList6->SetPipelineState((wireframe && wire) ? wire : solid);
        commandList6->SetGraphicsRoot32BitConstants(8, 9, &params, 0);
        const UINT tileCount = params.tilesX * params.tilesZ;
        commandList6->DispatchMesh((tileCount + 31) / 32, 1, 1);
    }
};

#endif // TERRAIN_RENDERER_DX12_H
