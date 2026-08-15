#ifndef RAIN_RENDERER_DX12_H
#define RAIN_RENDERER_DX12_H

#include "ShaderDX12.h"
#include "MSAADX12.h"
#include "Scene.h"
#include <fstream>
#include <sstream>

// Falling rain as a world-anchored particle volume.
//
// Structured deliberately unlike ImpactParticleRendererDX12, which uploads one
// instance record per particle from a CPU-simulated list and caps out at 1024.
// Rain needs an order of magnitude more drops and none of them need individual
// state: a drop's position is a pure function of its instance id and the clock.
// So there is no instance buffer at all -- one DrawInstanced with a vertex
// count of 18 and an instance count of however many drops the intensity calls
// for. Each instance is a narrow triangular prism, not a camera-facing quad,
// and the only per-frame upload is the constant buffer.
//
// Drawn after the opaque scene and the water so it can depth-test against both,
// and before the fog and post chain so the fog tints it like anything else in
// the world.
struct alignas(256) RainFrameDX12 {
    XMMATRIX viewProjection;
    XMFLOAT3 lightDirection;
    float    time;
    XMFLOAT3 windVelocity;
    float    intensity;
    XMFLOAT3 tint;
    float    fallSpeed;
    float    worldExtentX;
    float    worldExtentZ;
    float    dropLength;
    float    dropRadius;
    float    opacity;
    float    worldBottom;
    float    worldHeight;
    float    padding;
};
// Each float3 is followed by a float, so every one packs into a single 16-byte
// register exactly as HLSL lays the matching cbuffer out. Reordering a field on
// one side and not the other reads garbage rather than failing, so pin the size
// the layout implies: 64 for the matrix plus five 16-byte rows.
static_assert(offsetof(RainFrameDX12, lightDirection) == 64);
static_assert(offsetof(RainFrameDX12, windVelocity) == 80);
static_assert(offsetof(RainFrameDX12, tint) == 96);
static_assert(offsetof(RainFrameDX12, worldExtentX) == 112);
static_assert(offsetof(RainFrameDX12, opacity) == 128);

class RainRendererDX12 {
public:
    // Drops at full intensity. Each is 6 vertices with no instance data, so
    // this is cheap enough to leave high and scale down rather than resize
    // any buffer when the weather changes.
    static constexpr UINT MaxDrops = 96000;
    static constexpr UINT VerticesPerDrop = 18;
    bool initialized = false;

    bool Init() {
        std::ifstream shaderFile("shaders/rain.hlsl");
        if (!shaderFile) return false;
        std::stringstream shaderText;
        shaderText << shaderFile.rdbuf();
        const std::string source = shaderText.str();

        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        ComPtr<ID3DBlob> vs, ps, errors;
        HRESULT hr = ShaderCacheDX12::CompileCached(
            source.data(), source.size(), "rain.hlsl", nullptr, nullptr,
            "VSMain", "vs_5_0", flags, 0, &vs, &errors);
        if (FAILED(hr)) {
            if (errors)
                std::cerr << static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }
        errors.Reset();
        hr = ShaderCacheDX12::CompileCached(
            source.data(), source.size(), "rain.hlsl", nullptr, nullptr,
            "PSMain", "ps_5_0", flags, 0, &ps, &errors);
        if (FAILED(hr)) {
            if (errors)
                std::cerr << static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }

        D3D12_ROOT_PARAMETER root = {};
        root.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root.Descriptor.ShaderRegister = 0;
        root.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 1;
        rootDesc.pParameters = &root;
        rootDesc.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized;
        if (FAILED(D3D12SerializeRootSignature(
                &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
            return false;
        if (FAILED(g_dx12.device->CreateRootSignature(
                0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(&rootSignature_))))
            return false;

        if (!CreatePipelines(vs.Get(), ps.Get())) return false;
        if (!frames_.Create(FRAME_COUNT)) return false;
        initialized = true;
        return true;
    }

    // intensity 0 skips the pass entirely. hdrTarget/msaaTarget pick the
    // matching pipeline, the same way the impact particles do -- a PSO whose
    // format does not match the bound target is a device removal, not a
    // visual glitch.
    void Render(const Scene& scene, float time, float intensity,
                const XMFLOAT3& windVelocity, bool hdrTarget, bool msaaTarget,
                UINT frame) {
        if (!initialized || intensity <= 0.001f) return;

        RainFrameDX12 data = {};
        const XMMATRIX view = scene.GetViewMatrix();
        data.viewProjection =
            XMMatrixTranspose(view * scene.GetProjectionMatrix());
        XMStoreFloat3(&data.lightDirection, XMVector3Normalize(
            XMLoadFloat3(&scene.lightPos)));
        data.time = time;
        data.intensity = intensity;
        data.fallSpeed = kFallSpeed;
        data.windVelocity = windVelocity;
        data.worldExtentX = kBaseWorldExtent * (std::max)(
            0.5f, (std::min)(scene.terrainIslandScaleX, 12.0f));
        data.worldExtentZ = kBaseWorldExtent * (std::max)(
            0.5f, (std::min)(scene.terrainIslandScaleZ, 12.0f));
        data.dropLength = kDropLength;
        data.dropRadius = kDropRadius;
        data.worldBottom = kWorldBottom;
        data.worldHeight = kWorldHeight;
        // Rain is lit by the sky, not by its own colour, so it takes the
        // ambient level with it: a downpour at noon is bright grey, the same
        // rain at night is barely visible except against lights.
        //
        // The floor has to stay well under the night ambient (0.004) or it
        // becomes the only term that matters and night rain renders at day
        // brightness. 0.03 leaves the streaks as faint silhouettes -- readable
        // against muzzle flashes and local lights, invisible against the sky --
        // while daylight is unchanged, since 0.34 ambient upward already
        // saturates the sum.
        const float ambient = (std::min)(
            1.0f, 0.03f + scene.ambientLightingIntensity * 1.9f);
        data.tint = { 0.62f * ambient, 0.70f * ambient, 0.80f * ambient };
        data.opacity = kOpacity;
        frames_.CopyData(frame, data);

        // Fewer drops in light rain rather than the same drops faded out --
        // a drizzle is sparse, not transparent.
        const UINT drops = (std::max)(1u, static_cast<UINT>(
            MaxDrops * (std::min)(1.0f, intensity)));

        ID3D12GraphicsCommandList* commandList = g_dx12.commandList.Get();
        commandList->SetGraphicsRootSignature(rootSignature_.Get());
        commandList->SetGraphicsRootConstantBufferView(
            0, frames_.GetGPUAddress(frame));
        commandList->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const UINT target = hdrTarget ? 1u : (msaaTarget ? 2u : 0u);
        commandList->SetPipelineState(pipelines_[target].Get());
        commandList->DrawInstanced(VerticesPerDrop, drops, 0, 0);
    }

private:
    // Tuning. Held here rather than in Scene because these describe what rain
    // is, not what this run's weather is -- intensity and wind are the dials
    // the game actually turns.
    static constexpr float kFallSpeed = 54.0f;   // 3x the original 18 m/s rate
    // The procedural shore ends around 88 m at scale 1. The extra margin keeps
    // the precipitation boundary beyond the water immediately around it.
    static constexpr float kBaseWorldExtent = 104.0f;
    static constexpr float kWorldBottom = -12.0f;
    static constexpr float kWorldHeight = 220.0f;
    static constexpr float kDropLength = 0.85f;
    static constexpr float kDropRadius = 0.018f;
    static constexpr float kOpacity = 0.55f;

    bool CreatePipelines(ID3DBlob* vs, ID3DBlob* ps) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = rootSignature_.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        // Premultiplied alpha: the shader already scales the tint by alpha, so
        // overlapping streaks accumulate without the double-darkening that
        // straight src-alpha blending gives on a stack of transparent quads.
        desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        desc.BlendState.RenderTarget[0].DestBlendAlpha =
            D3D12_BLEND_INV_SRC_ALPHA;
        desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        // Tests against the scene so rain falls behind walls, but writes no
        // depth: thousands of translucent streaks must not occlude each other.
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        // 0: LDR swapchain, 1: HDR visibility target, 2: MSAA forward.
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&pipelines_[0])))) return false;
        desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&pipelines_[1])))) return false;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = MSAADX12::SampleCount;
        desc.RasterizerState.MultisampleEnable = TRUE;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&pipelines_[2])))) return false;
        return true;
    }

    ComPtr<ID3D12RootSignature> rootSignature_;
    std::array<ComPtr<ID3D12PipelineState>, 3> pipelines_;
    UploadBuffer<RainFrameDX12> frames_;
};

#endif
