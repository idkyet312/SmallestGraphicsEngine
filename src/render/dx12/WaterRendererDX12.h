#pragma once

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include "OceanWaveSettings.h"
#include "Scene.h"
#include "WaterVolume.h"
#include "UltraWaterSimulationDX12.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

bool DeploymentPlanningActive();
int DeploymentWaterDebugMode();

class WaterRendererDX12 {
public:
    bool initialized = false;

    void QueueBathymetryRebuild(const WaterBathymetryDesc& desc) {
        ultra_.QueueBathymetryRebuild(desc);
    }
    void QueueInteraction(const WaterInteraction& event) {
        ultra_.QueueInteraction(event);
    }
    void QueueHeightQuery(const WaterHeightQuery& query) {
        ultra_.QueueHeightQuery(query);
    }
    void RefreshUltraWater() { ultra_.RequestRefresh(); }
    bool TryGetHeightResult(uint64_t objectId,
                            WaterHeightResult& result) const {
        return ultra_.TryGetHeightResult(objectId, result);
    }
    bool UltraAvailable() const { return ultra_.Initialized(); }
    void SetUltraDiagnosticMode(UINT mode) {
        ultraDiagnosticMode_ = (std::min)(mode, 8u);
    }
    const std::string& UltraFailureReason() const {
        return ultra_.FailureReason();
    }

    ~WaterRendererDX12() {
        if (constantBuffer_ && mappedConstants_)
            constantBuffer_->Unmap(0, nullptr);
    }

    bool Init(UINT width, UINT height) {
        Shutdown();
        width_ = width;
        height_ = height;

        std::ifstream file("shaders/water_dx12.hlsl");
        if (!file) return false;
        std::stringstream stream;
        stream << file.rdbuf();
        shaderSource_ = stream.str();

        ComPtr<ID3DBlob> vertexShader;
        if (!Compile("VSMain", "vs_5_0", nullptr, vertexShader) ||
            !CreateRootSignature() ||
            !CreatePipelineSet(vertexShader.Get()) ||
            !CreateClipmap() ||
            !CreateConstantBuffer() ||
            !CreateDescriptorHeap() ||
            !CreateSceneCopies()) {
            Shutdown();
            return false;
        }
        ultra_.Init();
        initialized = true;
        return true;
    }

    bool Resize(UINT width, UINT height) {
        if (!initialized || width == 0 || height == 0) return false;
        width_ = width;
        height_ = height;
        hdrSceneCopy_.Reset();
        ldrSceneCopy_.Reset();
        hasHistory_ = false;
        return CreateSceneCopies();
    }

    void Shutdown() {
        initialized = false;
        rootSignature_.Reset();
        hdrMotionPSO_.Reset();
        hdrPSO_.Reset();
        underwaterPSO_.Reset();
        ldrPSO_.Reset();
        ldrMSAADepthPSO_.Reset();
        clipVertexBuffer_.Reset();
        clipIndexBuffer_.Reset();
        ultraClipVertexBuffer_.Reset();
        ultraClipIndexBuffer_.Reset();
        deploymentClipVertexBuffer_.Reset();
        deploymentClipIndexBuffer_.Reset();
        hdrSceneCopy_.Reset();
        ldrSceneCopy_.Reset();
        descriptorHeap_.Reset();
        if (constantBuffer_ && mappedConstants_) {
            constantBuffer_->Unmap(0, nullptr);
            mappedConstants_ = nullptr;
        }
        constantBuffer_.Reset();
        clipVertexView_ = {};
        clipIndexView_ = {};
        clipIndexCount_ = 0;
        ultraClipIndexCount_ = 0;
        deploymentClipVertexView_ = {};
        deploymentClipIndexView_ = {};
        deploymentClipIndexCount_ = 0;
        hasHistory_ = false;
        previousDeploymentPlanning_ = false;
        ultra_.Shutdown();
    }

    void Render(const Scene& scene,
                WaterVolume& ocean,
                WaterVolume& pool,
                ID3D12Resource* target,
                D3D12_CPU_DESCRIPTOR_HANDLE targetRTV,
                ID3D12Resource* opaqueDepth,
                ID3D12Resource* environment,
                bool hdrTarget,
                ID3D12Resource* motionTarget = nullptr,
                D3D12_CPU_DESCRIPTOR_HANDLE motionRTV = {},
                D3D12_RESOURCE_STATES depthState =
                    D3D12_RESOURCE_STATE_DEPTH_WRITE,
                ProfilerDX12* profiler = nullptr) {
        if (!initialized || !target || !targetRTV.ptr || !opaqueDepth ||
            (!ocean.IsInitialized() && !pool.IsInitialized())) return;

        const bool depthMSAA = opaqueDepth->GetDesc().SampleDesc.Count > 1;
        const bool writeMotion =
            hdrTarget && motionTarget && motionRTV.ptr && !depthMSAA;
        ID3D12PipelineState* pso = nullptr;
        if (hdrTarget)
            pso = writeMotion ? hdrMotionPSO_.Get() : hdrPSO_.Get();
        else
            pso = depthMSAA ? ldrMSAADepthPSO_.Get() : ldrPSO_.Get();
        if (!pso) return;

        ID3D12Resource* sceneCopy =
            hdrTarget ? hdrSceneCopy_.Get() : ldrSceneCopy_.Get();
        if (!sceneCopy) return;

        const bool ultraActive = hdrTarget &&
            scene.waterQuality == WaterQuality::Ultra &&
            ultra_.Initialized();
        if (scene.waterQuality != WaterQuality::Low && ultra_.Initialized())
            ultra_.PrepareBathymetry();
        if (ultraActive) {
            const float deltaTime = hasHistory_
                ? (std::max)(0.0f, ocean.GetTime() - previousOceanTime_)
                : 0.0f;
            UltraWaterTuning tuning;
            tuning.waveHeight = scene.ultraWaterWaveHeight;
            tuning.waveScale = scene.ultraWaterWaveScale;
            tuning.waveSpeed = scene.ultraWaterWaveSpeed;
            tuning.directionRadians = scene.ultraWaterDirection;
            tuning.choppiness = scene.ultraWaterChoppiness;
            tuning.surfStrength = scene.ultraWaterSurfStrength;
            tuning.foamStrength = scene.ultraWaterFoamStrength;
            tuning.coastDamping = scene.ultraWaterCoastDamping;
            ultra_.Update(
                deltaTime, ocean.GetTime(), ocean.GetSurfaceY(), tuning,
                profiler);
        }

        ID3D12GraphicsCommandList* list = g_dx12.commandList.Get();
        Transition(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(sceneCopy, target);
        Transition(list, target, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        Transition(list, sceneCopy, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(list, opaqueDepth, depthState,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (writeMotion)
            Transition(list, motionTarget,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);

        UpdateDescriptors(sceneCopy, opaqueDepth, environment, 0);
        ID3D12DescriptorHeap* heaps[] = { descriptorHeap_.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(rootSignature_.Get());
        list->SetPipelineState(pso);
        list->SetGraphicsRootDescriptorTable(
            1, DescriptorGPU(DescriptorTableBase(0)));
        list->RSSetViewports(1, &g_dx12.viewport);
        list->RSSetScissorRects(1, &g_dx12.scissorRect);
        if (writeMotion) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { targetRTV, motionRTV };
            list->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
        } else {
            list->OMSetRenderTargets(1, &targetRTV, FALSE, nullptr);
        }

        const XMMATRIX view = scene.GetViewMatrix();
        const XMMATRIX projection = scene.GetProjectionMatrix();
        const XMMATRIX viewProjection = view * projection;
        const bool deploymentPlanning = DeploymentPlanningActive();
        const float snap = 0.25f;
        const XMFLOAT2 clipCenter = deploymentPlanning
            ? XMFLOAT2{ 0.0f, 0.0f }
            : XMFLOAT2{
                std::floor(scene.camera.Position.x / snap) * snap,
                std::floor(scene.camera.Position.z / snap) * snap
            };
        if (deploymentPlanning != previousDeploymentPlanning_)
            hasHistory_ = false;
        if (!hasHistory_) {
            previousViewProjection_ = viewProjection;
            previousClipCenter_ = clipCenter;
            previousOceanTime_ = ocean.GetTime();
        }

        if (ocean.IsInitialized()) {
            Constants constants = BuildConstants(
                scene, ocean, viewProjection, clipCenter, true, ultraActive);
            BindConstants(constants, 0);
            const D3D12_VERTEX_BUFFER_VIEW& oceanVB = deploymentPlanning
                ? deploymentClipVertexView_
                : ultraActive ? ultraClipVertexView_ : clipVertexView_;
            const D3D12_INDEX_BUFFER_VIEW& oceanIB = deploymentPlanning
                ? deploymentClipIndexView_
                : ultraActive ? ultraClipIndexView_ : clipIndexView_;
            const UINT oceanIndexCount = deploymentPlanning
                ? deploymentClipIndexCount_
                : ultraActive ? ultraClipIndexCount_ : clipIndexCount_;
            list->IASetVertexBuffers(0, 1, &oceanVB);
            list->IASetIndexBuffer(&oceanIB);
            list->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawIndexedInstanced(oceanIndexCount, 1, 0, 0, 0);
        }

        if (pool.IsInitialized()) {
            const D3D12_VERTEX_BUFFER_VIEW& vbv =
                pool.UpdateAndGetVBV(g_dx12.frameIndex);
            const D3D12_INDEX_BUFFER_VIEW& ibv = pool.GetIBV();
            const UINT indexCount = pool.GetIndexCount();
            if (vbv.BufferLocation && indexCount) {
                Constants constants = BuildConstants(
                    scene, pool, viewProjection, clipCenter, false, false);
                BindConstants(constants, 1);
                list->IASetVertexBuffers(0, 1, &vbv);
                list->IASetIndexBuffer(&ibv);
                list->IASetPrimitiveTopology(
                    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
            }
        }

        if (writeMotion)
            Transition(list, motionTarget, D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(list, opaqueDepth,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   depthState);
        Transition(list, sceneCopy,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);

        previousViewProjection_ = viewProjection;
        previousClipCenter_ = clipCenter;
        previousOceanTime_ = ocean.GetTime();
        previousDeploymentPlanning_ = deploymentPlanning;
        hasHistory_ = true;
    }

    void RenderUnderwater(const Scene& scene,
                          const WaterVolume& ocean,
                          ID3D12Resource* target,
                          D3D12_CPU_DESCRIPTOR_HANDLE targetRTV,
                          ID3D12Resource* opaqueDepth,
                          ID3D12Resource* environment,
                          D3D12_RESOURCE_STATES depthState =
                              D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        if (!initialized || !underwaterPSO_ || !target || !targetRTV.ptr ||
            !opaqueDepth || !ocean.IsInitialized() ||
            scene.waterQuality != WaterQuality::Ultra || !ultra_.Ready())
            return;

        if (scene.camera.Position.y >= ocean.GetSurfaceY() - 0.02f) return;

        ID3D12GraphicsCommandList* list = g_dx12.commandList.Get();
        Transition(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(hdrSceneCopy_.Get(), target);
        Transition(list, target, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        Transition(list, hdrSceneCopy_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(list, opaqueDepth, depthState,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        UpdateDescriptors(hdrSceneCopy_.Get(), opaqueDepth, environment, 1);
        ID3D12DescriptorHeap* heaps[] = { descriptorHeap_.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(rootSignature_.Get());
        list->SetPipelineState(underwaterPSO_.Get());
        list->SetGraphicsRootDescriptorTable(
            1, DescriptorGPU(DescriptorTableBase(1)));

        const XMMATRIX viewProjection =
            scene.GetViewMatrix() * scene.GetProjectionMatrix();
        const float snap = 0.25f;
        const XMFLOAT2 clipCenter = {
            std::floor(scene.camera.Position.x / snap) * snap,
            std::floor(scene.camera.Position.z / snap) * snap
        };
        const Constants constants = BuildConstants(
            scene, ocean, viewProjection, clipCenter, true, true);
        BindConstants(constants, 2);
        list->RSSetViewports(1, &g_dx12.viewport);
        list->RSSetScissorRects(1, &g_dx12.scissorRect);
        list->OMSetRenderTargets(1, &targetRTV, FALSE, nullptr);
        list->IASetVertexBuffers(0, 0, nullptr);
        list->IASetIndexBuffer(nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);

        Transition(list, opaqueDepth,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, depthState);
        Transition(list, hdrSceneCopy_.Get(),
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
    }

private:
    static constexpr UINT kDescriptorsPerFrame = 8;
    static constexpr UINT kDescriptorTablesPerFrame = 2;
    static constexpr UINT kDrawsPerFrame = 3;

    struct ClipVertex {
        XMFLOAT3 position;
        XMFLOAT3 normal;
        XMFLOAT2 texCoord;
        XMFLOAT4 tangent;
    };

    struct Constants {
        XMFLOAT4X4 viewProjection;
        XMFLOAT4X4 previousViewProjection;
        XMFLOAT4X4 inverseViewProjection;
        XMFLOAT4 cameraTime;
        XMFLOAT4 previousCameraTime;
        XMFLOAT4 screenParams;
        XMFLOAT4 volume0;
        XMFLOAT4 volume1;
        XMFLOAT4 clipmapParams;
        XMFLOAT4 opticalParams;
        XMFLOAT4 absorption;
        XMFLOAT4 shallowScatter;
        XMFLOAT4 deepScatter;
        XMFLOAT4 lightDirection;
        XMFLOAT4 lightColor;
        XMFLOAT4 waves[OceanWaveSettings::WaveCount];
        XMFLOAT4 waveExtra[OceanWaveSettings::WaveCount];
        XMFLOAT4 ultraBounds;
        XMFLOAT4 ultraSimulation;
        XMFLOAT4 ultraDebug;
        XMFLOAT4 highWaveParams;
        XMFLOAT4 highShoreParams;
    };
    static_assert(sizeof(Constants) == 720,
                  "WaterConstants must match water_dx12.hlsl exactly");

    static UINT AlignConstantSize(UINT size) {
        return (size + 255u) & ~255u;
    }

    bool Compile(const char* entry, const char* target,
                 const D3D_SHADER_MACRO* defines,
                 ComPtr<ID3DBlob>& bytecode) {
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                           D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = ShaderCacheDX12::CompileCached(
            shaderSource_.data(), shaderSource_.size(),
            "water_dx12.hlsl", defines, nullptr, entry, target,
            flags, 0, &bytecode, &errors);
        if (FAILED(hr) && errors)
            std::cerr << static_cast<const char*>(
                errors->GetBufferPointer());
        return SUCCEEDED(hr);
    }

    bool CreateRootSignature() {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = kDescriptorsPerFrame;
        range.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER roots[2] = {};
        roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        roots[0].Descriptor.ShaderRegister = 0;
        roots[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        roots[1].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[1].DescriptorTable.NumDescriptorRanges = 1;
        roots[1].DescriptorTable.pDescriptorRanges = &range;
        roots[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samplers[3] = {};
        samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        for (UINT i = 0; i < 3; ++i) {
            samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister = i;
            samplers[i].ShaderVisibility =
                D3D12_SHADER_VISIBILITY_ALL;
        }
        samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = _countof(roots);
        desc.pParameters = roots;
        desc.NumStaticSamplers = _countof(samplers);
        desc.pStaticSamplers = samplers;
        desc.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3D12SerializeRootSignature(
                &desc, D3D_ROOT_SIGNATURE_VERSION_1,
                &blob, &errors))) return false;
        return SUCCEEDED(g_dx12.device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature_)));
    }

    bool CreatePipelineSet(ID3DBlob* vertexShader) {
        const D3D_SHADER_MACRO hdrMotionDefines[] = {
            {"WATER_HDR_TARGET", "1"},
            {"WATER_MOTION_OUTPUT", "1"},
            {nullptr, nullptr}
        };
        const D3D_SHADER_MACRO hdrDefines[] = {
            {"WATER_HDR_TARGET", "1"},
            {nullptr, nullptr}
        };
        const D3D_SHADER_MACRO ldrMSAADefines[] = {
            {"WATER_DEPTH_MSAA", "1"},
            {nullptr, nullptr}
        };
        ComPtr<ID3DBlob> hdrMotion;
        ComPtr<ID3DBlob> hdr;
        ComPtr<ID3DBlob> ldr;
        ComPtr<ID3DBlob> ldrMSAA;
        ComPtr<ID3DBlob> underwaterVS;
        ComPtr<ID3DBlob> underwaterPS;
        if (!Compile("PSMain", "ps_5_0", hdrMotionDefines, hdrMotion) ||
            !Compile("PSMain", "ps_5_0", hdrDefines, hdr) ||
            !Compile("PSMain", "ps_5_0", nullptr, ldr) ||
            !Compile("PSMain", "ps_5_0", ldrMSAADefines, ldrMSAA) ||
            !Compile("UnderwaterVS", "vs_5_0", nullptr, underwaterVS) ||
            !Compile("UnderwaterPS", "ps_5_0", hdrDefines, underwaterPS))
            return false;
        return CreatePipeline(vertexShader, hdrMotion.Get(),
                              DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                              hdrMotionPSO_) &&
               CreatePipeline(vertexShader, hdr.Get(),
                              DXGI_FORMAT_R16G16B16A16_FLOAT, false,
                              hdrPSO_) &&
               CreatePipeline(vertexShader, ldr.Get(),
                              DXGI_FORMAT_R8G8B8A8_UNORM, false,
                              ldrPSO_) &&
               CreatePipeline(vertexShader, ldrMSAA.Get(),
                              DXGI_FORMAT_R8G8B8A8_UNORM, false,
                              ldrMSAADepthPSO_) &&
               CreateUnderwaterPipeline(
                   underwaterVS.Get(), underwaterPS.Get());
    }

    bool CreateUnderwaterPipeline(ID3DBlob* vertexShader,
                                  ID3DBlob* pixelShader) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = rootSignature_.Get();
        desc.VS = { vertexShader->GetBufferPointer(),
                    vertexShader->GetBufferSize() };
        desc.PS = { pixelShader->GetBufferPointer(),
                    pixelShader->GetBufferSize() };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        return SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
            &desc, IID_PPV_ARGS(&underwaterPSO_)));
    }

    bool CreatePipeline(ID3DBlob* vertexShader, ID3DBlob* pixelShader,
                        DXGI_FORMAT colorFormat, bool motion,
                        ComPtr<ID3D12PipelineState>& output) {
        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,
             0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT,
             0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,
             0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
             0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.InputLayout = { layout, _countof(layout) };
        desc.pRootSignature = rootSignature_.Get();
        desc.VS = {
            vertexShader->GetBufferPointer(),
            vertexShader->GetBufferSize()
        };
        desc.PS = {
            pixelShader->GetBufferPointer(),
            pixelShader->GetBufferSize()
        };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        if (motion)
            desc.BlendState.RenderTarget[1].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_RED |
                D3D12_COLOR_WRITE_ENABLE_GREEN;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = motion ? 2 : 1;
        desc.RTVFormats[0] = colorFormat;
        if (motion)
            desc.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
        desc.SampleDesc.Count = 1;
        return SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
            &desc, IID_PPV_ARGS(&output)));
    }

    bool CreateClipmap() {
        // The final ring is visual-only horizon coverage. Its vertices are
        // pinned inside the far clip in water_dx12.hlsl, so the ordinary 800 m
        // camera far plane can retain its depth precision without exposing a
        // strip of sky between the finite ocean and the geometric horizon.
        // Ring extents, tuned so wave geometry survives into the midfield.
        //
        // Straight doubling put the 31 m swell -- the longest authored train --
        // beyond its 4-cells-per-wavelength limit at 128 m, and the 4.5 m chop
        // at 32 m, so everything past a couple of hundred metres flattened to
        // interpolated glass no matter how the waves themselves were authored.
        // Widening ring 0 does not fix that: with a geometric progression the
        // ring where cells reach a given size is fixed, so pushing the start
        // out just trades near detail for far (measured: identical 128 m reach).
        //
        // What actually moves it is growing more slowly through the band that
        // matters. Rings 3-11 ramp at about 1.6-1.75x instead of 2x, which puts
        // three extra rings between 56 m and 700 m and carries the swell to
        // 260 m and the 11 m components to 96 m -- roughly double the reach for
        // 27% more vertices. Denser rings buy the same thing at 2.2x the cost.
        //
        // Past 4 km the progression opens up hard: the wave fade in
        // water_dx12.hlsl has already flattened the surface by then, so those
        // rings are horizon coverage and their cell size does not matter.
        static constexpr std::array<float, 14> gameplayExtents = {
            8.0f, 16.0f, 32.0f, 56.0f, 96.0f, 160.0f, 260.0f, 420.0f,
            700.0f, 1200.0f, 2100.0f, 4096.0f, 16384.0f, 1048576.0f };
        // Deployment evaluates its wave normal and crest per pixel. A flat
        // full-span grid therefore has no geometric LOD rings to reveal as the
        // camera orbits, while its subdivisions keep homogeneous clipping
        // reliable near the ocean horizon.
        return CreateLegacyClipmapMesh(
                   64, gameplayExtents, clipVertexBuffer_, clipIndexBuffer_,
                   clipVertexView_, clipIndexView_, clipIndexCount_) &&
               CreateClipmapMesh(
                   64, gameplayExtents,
                   ultraClipVertexBuffer_, ultraClipIndexBuffer_,
                   ultraClipVertexView_, ultraClipIndexView_,
                   ultraClipIndexCount_) &&
               CreateGridMesh(
                   128, 4096.0f,
                   deploymentClipVertexBuffer_, deploymentClipIndexBuffer_,
                   deploymentClipVertexView_, deploymentClipIndexView_,
                   deploymentClipIndexCount_);
    }

    bool CreateGridMesh(
        int gridCells,
        float halfSpan,
        ComPtr<ID3D12Resource>& vertexBuffer,
        ComPtr<ID3D12Resource>& indexBuffer,
        D3D12_VERTEX_BUFFER_VIEW& vertexView,
        D3D12_INDEX_BUFFER_VIEW& indexView,
        UINT& indexCount) {
        std::vector<ClipVertex> vertices;
        std::vector<uint32_t> indices;
        vertices.reserve(static_cast<size_t>(gridCells + 1) *
                         static_cast<size_t>(gridCells + 1));
        indices.reserve(static_cast<size_t>(gridCells) * gridCells * 6);

        for (int z = 0; z <= gridCells; ++z) {
            for (int x = 0; x <= gridCells; ++x) {
                const float u = static_cast<float>(x) / gridCells;
                const float v = static_cast<float>(z) / gridCells;
                ClipVertex vertex = {};
                vertex.position = {
                    (u * 2.0f - 1.0f) * halfSpan,
                    0.0f,
                    (v * 2.0f - 1.0f) * halfSpan
                };
                vertex.normal = { 0.0f, 1.0f, 0.0f };
                vertex.texCoord = { u, v };
                vertex.tangent = { 1.0f, 0.0f, 0.0f, 0.0f };
                vertices.push_back(vertex);
            }
        }
        for (int z = 0; z < gridCells; ++z) {
            for (int x = 0; x < gridCells; ++x) {
                const uint32_t a =
                    static_cast<uint32_t>(z * (gridCells + 1) + x);
                const uint32_t b = a + 1;
                const uint32_t c = a + gridCells + 1;
                const uint32_t d = c + 1;
                indices.insert(indices.end(), {a, c, b, b, c, d});
            }
        }

        if (!CreateUploadBuffer(
                vertices.data(),
                static_cast<UINT>(vertices.size() * sizeof(ClipVertex)),
                vertexBuffer)) return false;
        if (!CreateUploadBuffer(
                indices.data(),
                static_cast<UINT>(indices.size() * sizeof(uint32_t)),
                indexBuffer)) return false;
        vertexView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vertexView.SizeInBytes =
            static_cast<UINT>(vertices.size() * sizeof(ClipVertex));
        vertexView.StrideInBytes = sizeof(ClipVertex);
        indexView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
        indexView.SizeInBytes =
            static_cast<UINT>(indices.size() * sizeof(uint32_t));
        indexView.Format = DXGI_FORMAT_R32_UINT;
        indexCount = static_cast<UINT>(indices.size());
        return true;
    }

    template <size_t LevelCount>
    bool CreateLegacyClipmapMesh(
        int gridCells,
        const std::array<float, LevelCount>& outerExtents,
        ComPtr<ID3D12Resource>& vertexBuffer,
        ComPtr<ID3D12Resource>& indexBuffer,
        D3D12_VERTEX_BUFFER_VIEW& vertexView,
        D3D12_INDEX_BUFFER_VIEW& indexView,
        UINT& indexCount) {
        const int halfCells = gridCells / 2;
        const int holeHalfCells = gridCells / 4;
        std::vector<ClipVertex> vertices;
        std::vector<uint32_t> indices;
        vertices.reserve(LevelCount * static_cast<size_t>(gridCells + 1) *
                         (gridCells + 1));
        for (size_t level = 0; level < LevelCount; ++level) {
            const float outer = outerExtents[level];
            const float inner = level == 0 ? 0.0f : outerExtents[level - 1];
            const uint32_t baseVertex =
                static_cast<uint32_t>(vertices.size());
            auto coordinate = [&](int index) {
                const int signedCell = index - halfCells;
                const float sign = signedCell < 0 ? -1.0f : 1.0f;
                const float magnitude =
                    static_cast<float>(std::abs(signedCell));
                if (level == 0) return outer * signedCell / halfCells;
                if (magnitude <= holeHalfCells)
                    return sign * inner * magnitude / holeHalfCells;
                const float t = (magnitude - holeHalfCells) /
                                (halfCells - holeHalfCells);
                return sign * (inner + (outer - inner) * t);
            };
            for (int z = 0; z <= gridCells; ++z) {
                for (int x = 0; x <= gridCells; ++x) {
                    ClipVertex vertex = {};
                    vertex.position = {coordinate(x), 0.0f, coordinate(z)};
                    vertex.normal = {0.0f, 1.0f, 0.0f};
                    vertex.texCoord = {static_cast<float>(x) / gridCells,
                                       static_cast<float>(z) / gridCells};
                    vertex.tangent = {1.0f, 0.0f, 0.0f,
                                      static_cast<float>(level)};
                    vertices.push_back(vertex);
                }
            }
            for (int z = 0; z < gridCells; ++z) {
                for (int x = 0; x < gridCells; ++x) {
                    if (level > 0) {
                        const float cx = std::abs(x + 0.5f - halfCells);
                        const float cz = std::abs(z + 0.5f - halfCells);
                        if (cx < holeHalfCells && cz < holeHalfCells) continue;
                    }
                    const uint32_t a =
                        baseVertex + z * (gridCells + 1) + x;
                    const uint32_t b = a + 1;
                    const uint32_t c = a + gridCells + 1;
                    const uint32_t d = c + 1;
                    indices.insert(indices.end(), {a, c, b, b, c, d});
                }
            }
        }
        if (!CreateUploadBuffer(vertices.data(), static_cast<UINT>(
                vertices.size() * sizeof(ClipVertex)), vertexBuffer) ||
            !CreateUploadBuffer(indices.data(), static_cast<UINT>(
                indices.size() * sizeof(uint32_t)), indexBuffer)) return false;
        vertexView = {vertexBuffer->GetGPUVirtualAddress(),
            static_cast<UINT>(vertices.size() * sizeof(ClipVertex)),
            sizeof(ClipVertex)};
        indexView = {indexBuffer->GetGPUVirtualAddress(),
            static_cast<UINT>(indices.size() * sizeof(uint32_t)),
            DXGI_FORMAT_R32_UINT};
        indexCount = static_cast<UINT>(indices.size());
        return true;
    }

    template <size_t LevelCount>
    bool CreateClipmapMesh(
        int gridCells,
        const std::array<float, LevelCount>& outerExtents,
        ComPtr<ID3D12Resource>& vertexBuffer,
        ComPtr<ID3D12Resource>& indexBuffer,
        D3D12_VERTEX_BUFFER_VIEW& vertexView,
        D3D12_INDEX_BUFFER_VIEW& indexView,
        UINT& indexCount) {
        std::vector<ClipVertex> vertices;
        std::vector<uint32_t> indices;
        constexpr int radialCells = 16;
        auto mix = [](float a, float b, float t) {
            return a + (b - a) * t;
        };
        vertices.reserve(static_cast<size_t>(gridCells + 1) *
            (gridCells + 1) + (LevelCount - 1) * 4ull *
            (gridCells + 1) * (radialCells + 1));

        auto addVertex = [&](float x, float z, float u, float v,
                             size_t level) {
            ClipVertex vertex = {};
            vertex.position = {x, 0.0f, z};
            vertex.normal = {0.0f, 1.0f, 0.0f};
            vertex.texCoord = {u, v};
            vertex.tangent = {1.0f, 0.0f, 0.0f,
                              static_cast<float>(level)};
            vertices.push_back(vertex);
        };

        // One dense centre grid. Every outer level is four trapezoids whose
        // inner and outer edges both have gridCells segments. The old square
        // annulus squeezed the inner edge into half that count, leaving a 2:1
        // T-junction at every ring boundary; displaced beach waves made those
        // gaps read as separating water tiles.
        const float centreExtent = outerExtents[0];
        const uint32_t centreBase = static_cast<uint32_t>(vertices.size());
        for (int z = 0; z <= gridCells; ++z) {
            for (int x = 0; x <= gridCells; ++x) {
                const float u = static_cast<float>(x) / gridCells;
                const float v = static_cast<float>(z) / gridCells;
                addVertex(mix(-centreExtent, centreExtent, u),
                          mix(-centreExtent, centreExtent, v), u, v, 0);
            }
        }
        for (int z = 0; z < gridCells; ++z) {
            for (int x = 0; x < gridCells; ++x) {
                const uint32_t a = centreBase + z * (gridCells + 1) + x;
                const uint32_t b = a + 1;
                const uint32_t c = a + gridCells + 1;
                const uint32_t d = c + 1;
                indices.insert(indices.end(), {a, c, b, b, c, d});
            }
        }

        auto addPatch = [&](size_t level, const XMFLOAT2& innerA,
                            const XMFLOAT2& innerB, const XMFLOAT2& outerA,
                            const XMFLOAT2& outerB) {
            const uint32_t base = static_cast<uint32_t>(vertices.size());
            for (int r = 0; r <= radialCells; ++r) {
                const float radial = static_cast<float>(r) / radialCells;
                for (int edge = 0; edge <= gridCells; ++edge) {
                    const float u = static_cast<float>(edge) / gridCells;
                    const float innerX = mix(innerA.x, innerB.x, u);
                    const float innerZ = mix(innerA.y, innerB.y, u);
                    const float outerX = mix(outerA.x, outerB.x, u);
                    const float outerZ = mix(outerA.y, outerB.y, u);
                    addVertex(mix(innerX, outerX, radial),
                              mix(innerZ, outerZ, radial), u, radial, level);
                }
            }
            for (int r = 0; r < radialCells; ++r) {
                for (int edge = 0; edge < gridCells; ++edge) {
                    const uint32_t a =
                        base + r * (gridCells + 1) + edge;
                    const uint32_t b = a + 1;
                    const uint32_t c = a + gridCells + 1;
                    const uint32_t d = c + 1;
                    indices.insert(indices.end(), {a, c, b, b, c, d});
                }
            }
        };

        for (size_t level = 1; level < LevelCount; ++level) {
            const float inner = outerExtents[level - 1];
            const float outer = outerExtents[level];
            addPatch(level, {-inner, inner}, {inner, inner},
                      {-outer, outer}, {outer, outer});
            addPatch(level, {inner, inner}, {inner, -inner},
                      {outer, outer}, {outer, -outer});
            addPatch(level, {inner, -inner}, {-inner, -inner},
                      {outer, -outer}, {-outer, -outer});
            addPatch(level, {-inner, -inner}, {-inner, inner},
                      {-outer, -outer}, {-outer, outer});
        }

        if (!CreateUploadBuffer(
                vertices.data(),
                static_cast<UINT>(vertices.size() * sizeof(ClipVertex)),
                vertexBuffer)) return false;
        if (!CreateUploadBuffer(
                indices.data(),
                static_cast<UINT>(indices.size() * sizeof(uint32_t)),
                indexBuffer)) return false;
        vertexView.BufferLocation =
            vertexBuffer->GetGPUVirtualAddress();
        vertexView.SizeInBytes =
            static_cast<UINT>(vertices.size() * sizeof(ClipVertex));
        vertexView.StrideInBytes = sizeof(ClipVertex);
        indexView.BufferLocation =
            indexBuffer->GetGPUVirtualAddress();
        indexView.SizeInBytes =
            static_cast<UINT>(indices.size() * sizeof(uint32_t));
        indexView.Format = DXGI_FORMAT_R32_UINT;
        indexCount = static_cast<UINT>(indices.size());
        return true;
    }

    bool CreateUploadBuffer(const void* data, UINT size,
                            ComPtr<ID3D12Resource>& output) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&output)))) return false;
        void* mapped = nullptr;
        if (FAILED(output->Map(0, nullptr, &mapped))) return false;
        memcpy(mapped, data, size);
        output->Unmap(0, nullptr);
        return true;
    }

    bool CreateConstantBuffer() {
        constantStride_ = AlignConstantSize(sizeof(Constants));
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = static_cast<UINT64>(
            constantStride_) * FRAME_COUNT * kDrawsPerFrame;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&constantBuffer_)))) return false;
        return SUCCEEDED(constantBuffer_->Map(
            0, nullptr,
            reinterpret_cast<void**>(&mappedConstants_)));
    }

    bool CreateDescriptorHeap() {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = kDescriptorsPerFrame * FRAME_COUNT *
                              kDescriptorTablesPerFrame;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &desc, IID_PPV_ARGS(&descriptorHeap_)))) return false;
        descriptorSize_ =
            g_dx12.device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }

    bool CreateSceneCopies() {
        return CreateSceneCopy(
                   DXGI_FORMAT_R16G16B16A16_FLOAT, hdrSceneCopy_) &&
               CreateSceneCopy(
                   DXGI_FORMAT_R8G8B8A8_UNORM, ldrSceneCopy_);
    }

    bool CreateSceneCopy(DXGI_FORMAT format,
                         ComPtr<ID3D12Resource>& output) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width_;
        desc.Height = height_;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&output)))) return false;
        return true;
    }

    void UpdateDescriptors(ID3D12Resource* sceneCopy,
                           ID3D12Resource* depth,
                           ID3D12Resource* environment,
                           UINT tableIndex) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            DescriptorCPU(DescriptorTableBase(tableIndex));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = sceneCopy->GetDesc().Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(
            sceneCopy, &srv, handle);
        handle.ptr += descriptorSize_;

        srv = {};
        srv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        if (depth->GetDesc().SampleDesc.Count > 1) {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        } else {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
        }
        g_dx12.device->CreateShaderResourceView(depth, &srv, handle);
        handle.ptr += descriptorSize_;

        srv = {};
        srv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = environment
            ? environment->GetDesc().Format
            : DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = environment
            ? environment->GetDesc().MipLevels : 1;
        g_dx12.device->CreateShaderResourceView(
            environment, &srv, handle);

        auto createUltraSRV = [&](ID3D12Resource* resource,
                                  DXGI_FORMAT format, bool array) {
            handle.ptr += descriptorSize_;
            D3D12_SHADER_RESOURCE_VIEW_DESC ultraSRV = {};
            ultraSRV.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            ultraSRV.Format = format;
            ultraSRV.ViewDimension = array
                ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                : D3D12_SRV_DIMENSION_TEXTURE2D;
            if (array) {
                ultraSRV.Texture2DArray.MipLevels = 1;
                ultraSRV.Texture2DArray.ArraySize =
                    UltraWaterSimulationDX12::CascadeCount;
            } else {
                ultraSRV.Texture2D.MipLevels = 1;
            }
            g_dx12.device->CreateShaderResourceView(
                resource, &ultraSRV, handle);
        };
        createUltraSRV(ultra_.Ready() ? ultra_.CurrentSpectrum() : nullptr,
                       DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        createUltraSRV(ultra_.Ready() ? ultra_.PreviousSpectrum() : nullptr,
                       DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        createUltraSRV(ultra_.Ready() ? ultra_.CurrentCoast() : nullptr,
                       DXGI_FORMAT_R16G16B16A16_FLOAT, false);
        createUltraSRV(ultra_.Ready() ? ultra_.PreviousCoast() : nullptr,
                       DXGI_FORMAT_R16G16B16A16_FLOAT, false);
        createUltraSRV(ultra_.Ready() ? ultra_.Bathymetry() : nullptr,
                       DXGI_FORMAT_R32_FLOAT, false);
    }

    Constants BuildConstants(const Scene& scene,
                             const WaterVolume& volume,
                             const XMMATRIX& currentViewProjection,
                             const XMFLOAT2& clipCenter,
                             bool ocean,
                             bool ultraActive) const {
        Constants constants = {};
        XMStoreFloat4x4(
            &constants.viewProjection,
            XMMatrixTranspose(currentViewProjection));
        XMStoreFloat4x4(
            &constants.previousViewProjection,
            XMMatrixTranspose(previousViewProjection_));
        XMStoreFloat4x4(
            &constants.inverseViewProjection,
            XMMatrixTranspose(XMMatrixInverse(
                nullptr, currentViewProjection)));
        constants.cameraTime = {
            scene.camera.Position.x,
            scene.camera.Position.y,
            scene.camera.Position.z,
            volume.GetTime()
        };
        constants.previousCameraTime = {
            previousClipCenter_.x,
            previousClipCenter_.y,
            scene.camera.Position.z,
            ocean ? previousOceanTime_ : volume.GetTime()
        };
        constants.screenParams = {
            static_cast<float>(width_),
            static_cast<float>(height_),
            1.0f / width_,
            1.0f / height_
        };
        const XMFLOAT3 center = volume.GetCenter();
        const XMFLOAT3 extents = volume.GetExtents();
        constants.volume0 = {
            center.x, volume.GetSurfaceY(), center.z,
            ocean ? 1.0f : 0.0f
        };
        constants.volume1 = {
            extents.x * 0.5f, extents.z * 0.5f,
            ocean && DeploymentPlanningActive()
                ? 1.0f + static_cast<float>(DeploymentWaterDebugMode())
                : 0.0f,
            ultraActive ? 2.0f :
                (scene.waterQuality != WaterQuality::Low ? 1.0f : 0.0f)
        };

        const OceanWaveSettings& settings =
            volume.GetOceanWaveSettings();
        constants.clipmapParams = {
            clipCenter.x, clipCenter.y,
            settings.microNormalStrength,
            ocean ? 0.105f : 0.085f
        };
        constants.opticalParams = {
            settings.foamDepth, settings.foamCrest,
            ocean ? 10.0f : 14.0f,
            ultraActive ? 0.94f :
                (scene.waterQuality != WaterQuality::Low ? 0.86f : 0.45f)
        };
        constants.absorption = {
            settings.absorption.x,
            settings.absorption.y,
            settings.absorption.z, 0.0f
        };
        constants.shallowScatter = {
            settings.shallowScatter.x,
            settings.shallowScatter.y,
            settings.shallowScatter.z, 0.0f
        };
        constants.deepScatter = {
            settings.deepScatter.x,
            settings.deepScatter.y,
            settings.deepScatter.z, 0.0f
        };
        XMVECTOR light = XMVector3Normalize(
            XMLoadFloat3(&scene.lightPos));
        XMFLOAT3 lightDirection;
        XMStoreFloat3(&lightDirection, light);
        // Water samples the raw HDRI rather than the exposed sky render. Match
        // the sky's below-horizon fade here so its reflection does not remain
        // photographic-white after the visible sky has become night.
        const float nightBlend = (std::max)(0.0f, (std::min)(1.0f,
            (-lightDirection.y - 0.10f) / 0.18f));
        const float reflectionIntensity =
            1.0f + (0.006f - 1.0f) * nightBlend;
        const float waterIntensity =
            1.0f + (0.03f - 1.0f) * nightBlend;
        constants.lightDirection = {
            lightDirection.x, lightDirection.y, lightDirection.z,
            reflectionIntensity
        };
        const XMFLOAT3 effectiveLight = scene.EffectiveLightColor();
        constants.lightColor = {
            effectiveLight.x, effectiveLight.y, effectiveLight.z,
            waterIntensity
        };
        // High-path sliders scale the authored spectrum. Read from the volume's
        // own settings, not from Scene, so this matches exactly what
        // EvaluateHeightAndSlope uses for buoyancy. Ocean only: pools and other
        // small volumes keep their authored surface, which the ocean sliders
        // have no business resizing.
        const float waveHeightScale = ocean
            ? (std::max)(0.0f, settings.heightScale) : 1.0f;
        const float waveScale = ocean
            ? (std::max)(0.05f, settings.lengthScale) : 1.0f;
        const float choppinessScale = ocean
            ? (std::max)(0.0f, scene.highWaterChoppiness) : 1.0f;
        for (size_t i = 0; i < OceanWaveSettings::WaveCount; ++i) {
            const OceanWave& wave = settings.waves[i];
            constants.waves[i] = {
                wave.direction.x, wave.direction.y,
                wave.amplitude * waveHeightScale,
                wave.wavelength * waveScale
            };
            // Gerstner surfaces self-intersect into loops once the sum of
            // steepness * amplitude * k passes 1. Height scales amplitude and
            // wave scale divides k, so the product moves with height/scale;
            // clamp steepness so a slider at its maximum sharpens crests
            // instead of pinching them inside out.
            const float k = DirectX::XM_2PI /
                (std::max)(wave.wavelength * waveScale, 0.1f);
            const float amplitude = wave.amplitude * waveHeightScale;
            const float budget = amplitude * k * OceanWaveSettings::WaveCount;
            const float maxSteepness = budget > 1e-4f
                ? (std::min)(1.0f, 0.92f / budget) : 1.0f;
            constants.waveExtra[i] = {
                (std::min)(wave.steepness * choppinessScale, maxSteepness),
                0.0f, 0.0f, 0.0f
            };
        }
        constants.ultraBounds = ultra_.Bounds();
        constants.ultraSimulation = {
            static_cast<float>(ultra_.CoastResolution()),
            static_cast<float>(ultra_.BathymetryResolution()),
            ocean && ultra_.Ready() ? 1.0f : 0.0f,
            ultraActive ? 1.0f : 0.0f };
        constants.ultraDebug = {
            static_cast<float>(ultraDiagnosticMode_), 0.0f, 0.0f, 0.0f };
        constants.highWaveParams = {
            ocean ? (std::max)(0.0f, settings.speedScale) : 1.0f,
            ocean ? (std::max)(0.0f, scene.highWaterMicroDetail) : 1.0f,
            ocean ? (std::max)(0.0f, scene.highWaterFoamStrength) : 1.0f,
            // Bearing half: how far waves turn toward the coast, plus the
            // wavefront irregularity that keeps the bent crests from reading
            // as concentric arcs. Pools have no bed to refract against.
            ocean ? std::clamp(scene.highWaterShoreRefraction, 0.0f, 1.0f)
                  : 0.0f };
        // Height half: wavelength compression, shoaling gain, steepening and
        // the breaker cap. Pools have no bed to shoal over, so they keep their
        // authored amplitude.
        constants.highShoreParams = {
            ocean ? std::clamp(scene.highWaterShoreFlatten, 0.0f, 1.0f) : 0.0f,
            0.0f, 0.0f, 0.0f };
        return constants;
    }

    void BindConstants(const Constants& constants, UINT drawIndex) {
        const UINT slot =
            g_dx12.frameIndex * kDrawsPerFrame + drawIndex;
        uint8_t* destination =
            mappedConstants_ + static_cast<size_t>(slot) * constantStride_;
        memcpy(destination, &constants, sizeof(constants));
        g_dx12.commandList->SetGraphicsRootConstantBufferView(
            0, constantBuffer_->GetGPUVirtualAddress() +
               static_cast<UINT64>(slot) * constantStride_);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DescriptorCPU(UINT index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * descriptorSize_;
        return handle;
    }

    UINT DescriptorTableBase(UINT tableIndex) const {
        return (g_dx12.frameIndex * kDescriptorTablesPerFrame + tableIndex) *
               kDescriptorsPerFrame;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE DescriptorGPU(UINT index) const {
        D3D12_GPU_DESCRIPTOR_HANDLE handle =
            descriptorHeap_->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * descriptorSize_;
        return handle;
    }

    static void Transition(ID3D12GraphicsCommandList* list,
                           ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after) {
        if (!resource || before == after) return;
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }

    UINT width_ = 0;
    UINT height_ = 0;
    UINT descriptorSize_ = 0;
    UINT constantStride_ = 0;
    UINT clipIndexCount_ = 0;
    UINT ultraClipIndexCount_ = 0;
    UINT deploymentClipIndexCount_ = 0;
    std::string shaderSource_;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> hdrMotionPSO_;
    ComPtr<ID3D12PipelineState> hdrPSO_;
    ComPtr<ID3D12PipelineState> underwaterPSO_;
    ComPtr<ID3D12PipelineState> ldrPSO_;
    ComPtr<ID3D12PipelineState> ldrMSAADepthPSO_;
    ComPtr<ID3D12Resource> clipVertexBuffer_;
    ComPtr<ID3D12Resource> clipIndexBuffer_;
    ComPtr<ID3D12Resource> ultraClipVertexBuffer_;
    ComPtr<ID3D12Resource> ultraClipIndexBuffer_;
    ComPtr<ID3D12Resource> deploymentClipVertexBuffer_;
    ComPtr<ID3D12Resource> deploymentClipIndexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW clipVertexView_ = {};
    D3D12_INDEX_BUFFER_VIEW clipIndexView_ = {};
    D3D12_VERTEX_BUFFER_VIEW ultraClipVertexView_ = {};
    D3D12_INDEX_BUFFER_VIEW ultraClipIndexView_ = {};
    D3D12_VERTEX_BUFFER_VIEW deploymentClipVertexView_ = {};
    D3D12_INDEX_BUFFER_VIEW deploymentClipIndexView_ = {};
    ComPtr<ID3D12Resource> hdrSceneCopy_;
    ComPtr<ID3D12Resource> ldrSceneCopy_;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    ComPtr<ID3D12Resource> constantBuffer_;
    uint8_t* mappedConstants_ = nullptr;
    bool hasHistory_ = false;
    bool previousDeploymentPlanning_ = false;
    XMMATRIX previousViewProjection_ = XMMatrixIdentity();
    XMFLOAT2 previousClipCenter_ = { 0.0f, 0.0f };
    float previousOceanTime_ = 0.0f;
    UINT ultraDiagnosticMode_ = 0;
    UltraWaterSimulationDX12 ultra_;
};
