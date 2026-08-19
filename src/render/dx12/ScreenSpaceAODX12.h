#ifndef SCREEN_SPACE_AO_DX12_H
#define SCREEN_SPACE_AO_DX12_H

#include "DX12Core.h"
#include "Scene.h"
#include "ShaderCacheDX12.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

class ScreenSpaceAODX12 {
public:
    // Two compiled variants of the same shader source, selected live by
    // Scene::optimizedAmbientOcclusion so the GTAO/contact-shadow arithmetic
    // optimizations can be measured against the original in the profiler.
    static constexpr UINT kAOVariantBaseline = 0;
    static constexpr UINT kAOVariantOptimized = 1;
    static constexpr UINT kAOVariantCount = 2;

    bool initialized = false;

    ~ScreenSpaceAODX12() {
        if (constantBuffer_ && mappedConstants_) constantBuffer_->Unmap(0, nullptr);
    }

    bool Init() {
        std::ifstream file("shaders/screen_space_ao.hlsl");
        if (!file) return false;
        std::stringstream stream;
        stream << file.rdbuf();
        const std::string source = stream.str();
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                           D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ComPtr<ID3DBlob> vs, errors;
        if (!Compile(source, "VSMain", "vs_5_0", flags, vs, errors) ||
            !CreateRootSignature())
            return false;

        // Compile the pixel shaders twice: once with the arithmetic
        // optimizations disabled (bit-identical to the pre-optimization
        // shader) and once with them on. Scene::optimizedAmbientOcclusion
        // selects between the two pipeline sets at draw time so the pass can
        // be A/B'd live in the profiler overlay without a rebuild. The shader
        // cache keys on defines, so each variant caches independently.
        for (UINT variant = 0; variant < kAOVariantCount; ++variant) {
            const D3D_SHADER_MACRO defines[] = {
                { "SGE_AO_OPTIMIZED", variant == kAOVariantOptimized ? "1" : "0" },
                { nullptr, nullptr }
            };
            ComPtr<ID3DBlob> gtao, gtaoMSAA, composite, compositeMSAA;
            ComPtr<ID3DBlob> bentGtao, bentGtaoMSAA;
            ComPtr<ID3DBlob> bentComposite, bentCompositeMSAA;
            if (!Compile(source, "PSGTAO", "ps_5_0", flags, gtao, errors,
                         defines) ||
                !Compile(source, "PSGTAOMSAA", "ps_5_0", flags, gtaoMSAA,
                         errors, defines) ||
                !Compile(source, "PSBlurComposite", "ps_5_0", flags,
                         composite, errors, defines) ||
                !Compile(source, "PSBlurCompositeMSAA", "ps_5_0", flags,
                         compositeMSAA, errors, defines) ||
                !Compile(source, "PSBentGTAO", "ps_5_0", flags,
                         bentGtao, errors, defines) ||
                !Compile(source, "PSBentGTAOMSAA", "ps_5_0", flags,
                         bentGtaoMSAA, errors, defines) ||
                !Compile(source, "PSBentBlurComposite", "ps_5_0", flags,
                         bentComposite, errors, defines) ||
                !Compile(source, "PSBentBlurCompositeMSAA", "ps_5_0", flags,
                         bentCompositeMSAA, errors, defines) ||
                !CreatePipelines(variant, vs.Get(), gtao.Get(), gtaoMSAA.Get(),
                                 composite.Get(), compositeMSAA.Get()) ||
                !CreateTemporalPipelines(
                    variant, vs.Get(), bentGtao.Get(), bentGtaoMSAA.Get(),
                    bentComposite.Get(), bentCompositeMSAA.Get()))
                return false;
        }
        if (!CreateResources()) return false;
        initialized = true;
        return true;
    }

    // True when Scene's half-res choice differs from what the AO targets were
    // built for. The frame loop polls this before BeginFrame, drains the GPU,
    // then calls ApplyTraceResolutionChange -- releasing the old targets only
    // once no frame in flight can still be reading them.
    bool TraceResolutionChangePending() const {
        return initialized && pendingHalfResolution_ != traceHalfResolution_;
    }

    // PRECONDITION: caller has drained every frame slot (WaitForGPUAllFrames).
    void ApplyTraceResolutionChange() {
        if (!TraceResolutionChangePending()) return;
        traceHalfResolution_ = pendingHalfResolution_;
        rawAO_.Reset();
        rawRtvHeap_.Reset();
        EnsureRawTarget();
    }

    bool HasTemporalBentNormalHistory(UINT nextFrame) const {
        return historyValid_ && aoHistory_[historyReadIndex_] &&
            targetWidth_ == TraceWidth() &&
            targetHeight_ == TraceHeight() &&
            lastTemporalFrame_ + 1u == nextFrame;
    }

    ID3D12Resource* GetTemporalBentNormalHistory() const {
        return historyValid_ ? aoHistory_[historyReadIndex_].Get() : nullptr;
    }

    void Render(const Scene& scene, ID3D12Resource* depthResource,
                bool multisampledDepth, D3D12_CPU_DESCRIPTOR_HANDLE targetRtv = {},
                 bool hdrTarget = false,
                 ID3D12Resource* staticCasterDepth = nullptr,
                 ID3D12Resource* normalRoughness = nullptr,
                 bool depthAlreadyReadable = false,
                 ID3D12Resource* grassCoverage = nullptr,
                 bool contactAppliedInDirectLighting = false,
                 UINT temporalNoiseFrame = 0,
                 ID3D12Resource* motionVectors = nullptr,
                 D3D12_RESOURCE_STATES motionState =
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                 bool temporalBentNormal = false,
                 bool bentAmbientAppliedInLighting = false) {
        if (!initialized || !depthResource || !g_dx12.commandList) return;
        // Latch the resolution choice for the whole frame. The temporal bent
        // normal path keeps its full-res history contract, so half res applies
        // only to the scalar trace.
        // Do NOT change trace resolution here. EnsureRawTarget would have to
        // release rawAO_ while an older frame slot is still sampling it, which
        // page-faults the GPU. TraceResolutionChangePending() is polled at the
        // frame boundary instead, where the caller can drain safely.
        const bool wantHalf =
            scene.halfResolutionAO && !(temporalBentNormal && motionVectors);
        if (wantHalf != traceHalfResolution_ && !rawAO_)
            traceHalfResolution_ = wantHalf;
        pendingHalfResolution_ = wantHalf;
        if (!EnsureRawTarget()) return;
        bool temporalActive = temporalBentNormal && motionVectors;
        if (temporalActive && !EnsureTemporalTargets())
            temporalActive = false;
        if (temporalActive != temporalEnabledLastFrame_) {
            historyValid_ = false;
            temporalEnabledLastFrame_ = temporalActive;
        }
        if (temporalActive && historyValid_ &&
            temporalNoiseFrame != lastTemporalFrame_ + 1u)
            historyValid_ = false;
        const UINT historyWriteIndex = historyReadIndex_ ^ 1u;
        Update(scene, depthResource, multisampledDepth, staticCasterDepth,
               normalRoughness, grassCoverage,
               contactAppliedInDirectLighting, temporalNoiseFrame,
               temporalActive ? aoHistory_[historyReadIndex_].Get() : nullptr,
               temporalActive ? motionVectors : nullptr,
               temporalActive, historyValid_, bentAmbientAppliedInLighting);

        ID3D12GraphicsCommandList* list = g_dx12.commandList.Get();
        // GrassMSAADX12's combined depth is produced by compute and handed off
        // in PIXEL_SHADER_RESOURCE. Its owner keeps that state for water/fog;
        // only transition depth resources whose contract starts at DEPTH_WRITE.
        if (!depthAlreadyReadable)
            Transition(list, depthResource, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (staticCasterDepth && staticCasterDepth != depthResource)
            Transition(list, staticCasterDepth,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (normalRoughness)
            Transition(list, normalRoughness,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (temporalActive)
            Transition(list, motionVectors, motionState,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        ID3D12DescriptorHeap* activeDescriptorHeap =
            descriptorHeaps_[g_dx12.frameIndex % FRAME_COUNT].Get();
        ID3D12DescriptorHeap* heaps[] = { activeDescriptorHeap };
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(rootSignature_.Get());
        list->SetGraphicsRootConstantBufferView(
            0, constantBuffer_->GetGPUVirtualAddress() +
               g_dx12.frameIndex * 256u);
        list->SetGraphicsRootDescriptorTable(
            1, activeDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // Trace viewport: covers rawAO_, which may be half size.
        const D3D12_VIEWPORT traceViewport = {
            0.0f, 0.0f, static_cast<float>(targetWidth_),
            static_cast<float>(targetHeight_), 0.0f, 1.0f };
        const D3D12_RECT traceScissor = {
            0, 0, static_cast<LONG>(targetWidth_),
            static_cast<LONG>(targetHeight_) };
        list->RSSetViewports(1, &traceViewport);
        list->RSSetScissorRects(1, &traceScissor);

        ID3D12Resource* rawSignal = temporalActive ? bentAO_.Get() : rawAO_.Get();
        Transition(list, rawSignal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rawRtv = temporalActive
            ? temporalRtvHeap_->GetCPUDescriptorHandleForHeapStart()
            : rawRtvHeap_->GetCPUDescriptorHandleForHeapStart();
        // Live A/B: pick the optimized or the original shader variant.
        const UINT aoVariant = scene.optimizedAmbientOcclusion
            ? kAOVariantOptimized : kAOVariantBaseline;
        list->OMSetRenderTargets(1, &rawRtv, FALSE, nullptr);
        list->SetPipelineState(temporalActive
            ? (multisampledDepth ? bentGtaoMSAAPipeline_[aoVariant].Get()
                                 : bentGtaoPipeline_[aoVariant].Get())
            : (multisampledDepth ? gtaoMSAAPipeline_[aoVariant].Get()
                                 : gtaoPipeline_[aoVariant].Get()));
        list->DrawInstanced(3, 1, 0, 0);
        Transition(list, rawSignal, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // The composite always writes the full-resolution target, so restore
        // the screen viewport after a half-res trace.
        list->RSSetViewports(1, &g_dx12.viewport);
        list->RSSetScissorRects(1, &g_dx12.scissorRect);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = targetRtv.ptr ? targetRtv :
            GetCPUDescriptorHandle(g_dx12.rtvHeap.Get(),
                                   g_dx12.rtvDescriptorSize,
                                   g_dx12.frameIndex);
        if (temporalActive) {
            Transition(list, aoHistory_[historyWriteIndex].Get(),
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_CPU_DESCRIPTOR_HANDLE historyRtv =
                temporalRtvHeap_->GetCPUDescriptorHandleForHeapStart();
            historyRtv.ptr += static_cast<SIZE_T>(
                g_dx12.device->GetDescriptorHandleIncrementSize(
                    D3D12_DESCRIPTOR_HEAP_TYPE_RTV)) *
                (historyWriteIndex + 1u);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { rtv, historyRtv };
            list->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
            list->SetPipelineState(hdrTarget
                ? bentCompositeHDRPipeline_[aoVariant].Get()
                : (multisampledDepth
                       ? bentCompositeMSAAPipeline_[aoVariant].Get()
                       : bentCompositePipeline_[aoVariant].Get()));
        } else {
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->SetPipelineState(
                hdrTarget ? compositeHDRPipeline_[aoVariant].Get() :
                (multisampledDepth ? compositeMSAAPipeline_[aoVariant].Get()
                                   : compositePipeline_[aoVariant].Get()));
        }
        list->DrawInstanced(3, 1, 0, 0);
        if (temporalActive) {
            Transition(list, aoHistory_[historyWriteIndex].Get(),
                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            historyReadIndex_ = historyWriteIndex;
            historyValid_ = true;
            lastTemporalFrame_ = temporalNoiseFrame;
        } else {
            historyValid_ = false;
            lastTemporalFrame_ = UINT_MAX;
        }

        if (!depthAlreadyReadable)
            Transition(list, depthResource,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (staticCasterDepth && staticCasterDepth != depthResource)
            Transition(list, staticCasterDepth,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (normalRoughness)
            Transition(list, normalRoughness,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (temporalActive)
            Transition(list, motionVectors,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       motionState);
    }

private:
    // AO trace resolution. Half res halves both axes; the composite that reads
    // the result always runs at full screen resolution.
    UINT TraceWidth() const {
        return traceHalfResolution_
            ? (std::max)(1u, g_dx12.screenWidth / 2u) : g_dx12.screenWidth;
    }
    UINT TraceHeight() const {
        return traceHalfResolution_
            ? (std::max)(1u, g_dx12.screenHeight / 2u) : g_dx12.screenHeight;
    }

    struct Constants {
        XMFLOAT4X4 inverseViewProjection;
        XMFLOAT4X4 viewProjection;
        XMFLOAT4 cameraNearFar;
        XMFLOAT4 lightDirection;
        XMFLOAT4 aoParams;
        XMFLOAT4 screenParams;
        XMFLOAT4 filterParams;
        XMFLOAT4 contactParams;
        // AO trace resolution; equals screenParams when tracing at full res.
        XMFLOAT4 traceParams;
    };

    bool Compile(const std::string& source, const char* entry, const char* target,
                 UINT flags, ComPtr<ID3DBlob>& output,
                 ComPtr<ID3DBlob>& errors,
                 const D3D_SHADER_MACRO* defines = nullptr) {
        errors.Reset();
        HRESULT hr = ShaderCacheDX12::CompileCached(source.data(), source.size(),
            "screen_space_ao.hlsl", defines, nullptr, entry, target, flags, 0,
            &output, &errors);
        if (FAILED(hr) && errors)
            std::cerr << static_cast<const char*>(errors->GetBufferPointer());
        return SUCCEEDED(hr);
    }

    bool CreateRootSignature() {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 9;
        range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER roots[2] = {};
        roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        roots[0].Descriptor.ShaderRegister = 0;
        roots[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[1].DescriptorTable.NumDescriptorRanges = 1;
        roots[1].DescriptorTable.pDescriptorRanges = &range;
        roots[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        for (UINT i = 0; i < 2; ++i) {
            samplers[i].AddressU = samplers[i].AddressV =
                samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister = i;
            samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = 2;
        desc.pParameters = roots;
        desc.NumStaticSamplers = 2;
        desc.pStaticSamplers = samplers;
        desc.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(
                &desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
            return false;
        return SUCCEEDED(g_dx12.device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature_)));
    }

    bool CreatePipelines(UINT variant,
                         ID3DBlob* vs, ID3DBlob* gtao, ID3DBlob* gtaoMSAA,
                         ID3DBlob* composite, ID3DBlob* compositeMSAA) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = rootSignature_.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { gtao->GetBufferPointer(), gtao->GetBufferSize() };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&gtaoPipeline_[variant])))) return false;

        desc.PS = { gtaoMSAA->GetBufferPointer(), gtaoMSAA->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&gtaoMSAAPipeline_[variant])))) return false;

        auto& blend = desc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ZERO;
        blend.DestBlend = D3D12_BLEND_SRC_COLOR;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED |
            D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.PS = { composite->GetBufferPointer(), composite->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&compositePipeline_[variant])))) return false;
        desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&compositeHDRPipeline_[variant])))) return false;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.PS = { compositeMSAA->GetBufferPointer(),
                    compositeMSAA->GetBufferSize() };
        return SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
            &desc, IID_PPV_ARGS(&compositeMSAAPipeline_[variant])));
    }

    bool CreateTemporalPipelines(
            UINT variant,
            ID3DBlob* vs, ID3DBlob* gtao, ID3DBlob* gtaoMSAA,
            ID3DBlob* composite, ID3DBlob* compositeMSAA) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = rootSignature_.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { gtao->GetBufferPointer(), gtao->GetBufferSize() };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&bentGtaoPipeline_[variant])))) return false;

        desc.PS = { gtaoMSAA->GetBufferPointer(),
                    gtaoMSAA->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&bentGtaoMSAAPipeline_[variant])))) return false;

        desc.NumRenderTargets = 2;
        desc.BlendState.IndependentBlendEnable = TRUE;
        auto& blend = desc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ZERO;
        blend.DestBlend = D3D12_BLEND_SRC_COLOR;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED |
            D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        auto& historyBlend = desc.BlendState.RenderTarget[1];
        historyBlend.BlendEnable = FALSE;
        historyBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.PS = { composite->GetBufferPointer(),
                    composite->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&bentCompositePipeline_[variant])))) return false;
        desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&bentCompositeHDRPipeline_[variant])))) return false;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.PS = { compositeMSAA->GetBufferPointer(),
                    compositeMSAA->GetBufferSize() };
        return SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
            &desc, IID_PPV_ARGS(&bentCompositeMSAAPipeline_[variant])));
    }

    bool CreateResources() {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 9;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        for (UINT i = 0; i < FRAME_COUNT; ++i) {
            if (FAILED(g_dx12.device->CreateDescriptorHeap(
                    &heapDesc, IID_PPV_ARGS(&descriptorHeaps_[i]))))
                return false;
        }
        descriptorSize_ = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 256u * FRAME_COUNT;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&constantBuffer_)))) return false;
        if (FAILED(constantBuffer_->Map(
                0, nullptr, reinterpret_cast<void**>(&mappedConstants_))))
            return false;
        return CreateRawTarget();
    }

    bool EnsureRawTarget() {
        // TraceWidth/Height follow traceHalfResolution_, which only changes at
        // a drained frame boundary, so reaching the reset path here means a
        // genuine screen resize (already drained by the swapchain path) rather
        // than a live toggle.
        if (rawAO_ && targetWidth_ == TraceWidth() &&
            targetHeight_ == TraceHeight()) return true;
        rawAO_.Reset();
        rawRtvHeap_.Reset();
        bentAO_.Reset();
        aoHistory_[0].Reset();
        aoHistory_[1].Reset();
        temporalRtvHeap_.Reset();
        historyReadIndex_ = 0;
        historyValid_ = false;
        lastTemporalFrame_ = UINT_MAX;
        return CreateRawTarget();
    }

    bool EnsureTemporalTargets() {
        if (bentAO_ && aoHistory_[0] && aoHistory_[1] && temporalRtvHeap_)
            return true;
        return CreateTemporalTargets();
    }

    // The AO trace target. Its resolution is screen size scaled by
    // Scene::halfResolutionAO, which is why the shader carries a separate
    // traceParams: the trace steps its taps in this space while the depth and
    // normal textures it reads stay full resolution.
    //
    // An earlier half-res attempt was reverted for "evenly spaced horizontal
    // bands that scaled with AO strength". The cause was not the dither (whose
    // row-to-row variation is identical at either resolution) but texel
    // addressing: a half-res pixel centre (j+0.5)/540 maps to source row 1.0,
    // 3.0, 5.0 -- exactly on texel boundaries, where a point sampler picks one
    // of two neighbours by float rounding, banding whole scanlines. The shader
    // now re-centres every source fetch via TraceUVToSourceUV, which lands each
    // sample at frac 0.5 of a source texel and removes the tie.
    bool CreateRawTarget() {
        targetWidth_ = TraceWidth();
        targetHeight_ = TraceHeight();
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = targetWidth_;
        desc.Height = targetHeight_;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&rawAO_)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeap = {};
        rtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeap.NumDescriptors = 1;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &rtvHeap, IID_PPV_ARGS(&rawRtvHeap_)))) return false;
        D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
        rtv.Format = DXGI_FORMAT_R8_UNORM;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_dx12.device->CreateRenderTargetView(
            rawAO_.Get(), &rtv,
            rawRtvHeap_->GetCPUDescriptorHandleForHeapStart());
        return true;
    }

    bool CreateTemporalTargets() {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = targetWidth_;
        desc.Height = targetHeight_;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&bentAO_)))) return false;
        for (UINT i = 0; i < 2; ++i) {
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&aoHistory_[i])))) return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeap = {};
        rtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeap.NumDescriptors = 3;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &rtvHeap, IID_PPV_ARGS(&temporalRtvHeap_)))) return false;
        D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
        rtv.Format = desc.Format;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            temporalRtvHeap_->GetCPUDescriptorHandleForHeapStart();
        const UINT stride = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        g_dx12.device->CreateRenderTargetView(bentAO_.Get(), &rtv, handle);
        for (UINT i = 0; i < 2; ++i) {
            handle.ptr += stride;
            g_dx12.device->CreateRenderTargetView(
                aoHistory_[i].Get(), &rtv, handle);
        }
        historyReadIndex_ = 0;
        historyValid_ = false;
        return true;
    }

    void Update(const Scene& scene, ID3D12Resource* depth, bool multisampled,
                ID3D12Resource* staticCasterDepth,
                ID3D12Resource* normalRoughness,
                ID3D12Resource* grassCoverage,
                bool contactAppliedInDirectLighting,
                UINT temporalNoiseFrame,
                ID3D12Resource* aoHistory,
                ID3D12Resource* motionVectors,
                bool temporalActive,
                bool temporalHistoryValid,
                bool bentAmbientAppliedInLighting) {
        Constants constants = {};
        XMMATRIX vp = scene.GetViewMatrix() * scene.GetProjectionMatrix();
        XMStoreFloat4x4(&constants.inverseViewProjection,
            XMMatrixTranspose(XMMatrixInverse(nullptr, vp)));
        XMStoreFloat4x4(&constants.viewProjection, XMMatrixTranspose(vp));
        constants.cameraNearFar = {
            scene.camera.Position.x, scene.camera.Position.y,
            scene.camera.Position.z, scene.cameraNear };
        XMVECTOR light = XMVector3Normalize(XMLoadFloat3(&scene.lightPos));
        XMFLOAT3 lightDirection;
        XMStoreFloat3(&lightDirection, light);
        constants.lightDirection = {
            lightDirection.x, lightDirection.y, lightDirection.z,
            scene.contactShadowStrength };
        constants.aoParams = {
            scene.ambientOcclusionRadius, scene.ambientOcclusionStrength,
            scene.ambientOcclusionBias, scene.cameraFar };
        constants.screenParams = {
            static_cast<float>(g_dx12.screenWidth),
            static_cast<float>(g_dx12.screenHeight),
            1.0f / g_dx12.screenWidth, 1.0f / g_dx12.screenHeight };
        // .z claims one of the two reserved pad slots, so the constant buffer
        // size and every field offset before it are unchanged.
        constants.filterParams = {
            staticCasterDepth && staticCasterDepth != depth ? 1.0f : 0.0f,
            normalRoughness ? 1.0f : 0.0f,
            scene.contactShadowLinearDepth ? 1.0f : 0.0f,
            temporalActive
                ? (temporalHistoryValid ? 0.90f : -0.90f) : 0.0f };
        constants.contactParams = {
            grassCoverage ? 1.0f : 0.0f,
            contactAppliedInDirectLighting ? 1.0f : 0.0f,
            static_cast<float>(temporalNoiseFrame),
            bentAmbientAppliedInLighting ? 1.0f : 0.0f };
        // Trace resolution. Equal to screenParams at full res, which makes
        // TraceUVToSourceUV the identity and leaves the full-res path
        // arithmetically unchanged.
        const float traceW = static_cast<float>(TraceWidth());
        const float traceH = static_cast<float>(TraceHeight());
        constants.traceParams = { traceW, traceH, 1.0f / traceW, 1.0f / traceH };
        std::memcpy(mappedConstants_ + g_dx12.frameIndex * 256u,
                    &constants, sizeof(constants));

        auto handle = descriptorHeaps_[g_dx12.frameIndex % FRAME_COUNT]
            ->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(
            multisampled ? nullptr : depth, &srv, handle);
        handle.ptr += descriptorSize_;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        srv.Texture2D.MipLevels = 0;
        g_dx12.device->CreateShaderResourceView(
            multisampled ? depth : nullptr, &srv, handle);
        handle.ptr += descriptorSize_;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(
            staticCasterDepth && staticCasterDepth != depth
                ? staticCasterDepth : nullptr,
            &srv, handle);
        handle.ptr += descriptorSize_;
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        g_dx12.device->CreateShaderResourceView(
            normalRoughness, &srv, handle);
        handle.ptr += descriptorSize_;
        srv.Format = DXGI_FORMAT_R8_UNORM;
        g_dx12.device->CreateShaderResourceView(rawAO_.Get(), &srv, handle);
        handle.ptr += descriptorSize_;
        srv.Format = DXGI_FORMAT_R8_UNORM;
        g_dx12.device->CreateShaderResourceView(
            grassCoverage, &srv, handle);
        handle.ptr += descriptorSize_;
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        g_dx12.device->CreateShaderResourceView(aoHistory, &srv, handle);
        handle.ptr += descriptorSize_;
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        g_dx12.device->CreateShaderResourceView(
            motionVectors, &srv, handle);
        handle.ptr += descriptorSize_;
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        g_dx12.device->CreateShaderResourceView(
            temporalActive ? bentAO_.Get() : nullptr, &srv, handle);
    }

    static void Transition(ID3D12GraphicsCommandList* list,
                           ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }

    ComPtr<ID3D12RootSignature> rootSignature_;
    // Indexed by AO variant: [kAOVariantBaseline] is the pre-optimization
    // shader, [kAOVariantOptimized] the optimized one.
    ComPtr<ID3D12PipelineState> gtaoPipeline_[kAOVariantCount];
    ComPtr<ID3D12PipelineState> gtaoMSAAPipeline_[kAOVariantCount];
    ComPtr<ID3D12PipelineState> compositePipeline_[kAOVariantCount];
    ComPtr<ID3D12PipelineState> compositeHDRPipeline_[kAOVariantCount];
    ComPtr<ID3D12PipelineState> compositeMSAAPipeline_[kAOVariantCount];
    ComPtr<ID3D12PipelineState> bentGtaoPipeline_[kAOVariantCount];
    ComPtr<ID3D12PipelineState> bentGtaoMSAAPipeline_[kAOVariantCount];
    ComPtr<ID3D12PipelineState> bentCompositePipeline_[kAOVariantCount];
    ComPtr<ID3D12PipelineState> bentCompositeHDRPipeline_[kAOVariantCount];
    ComPtr<ID3D12PipelineState> bentCompositeMSAAPipeline_[kAOVariantCount];
    ComPtr<ID3D12DescriptorHeap> descriptorHeaps_[FRAME_COUNT];
    ComPtr<ID3D12DescriptorHeap> rawRtvHeap_;
    ComPtr<ID3D12DescriptorHeap> temporalRtvHeap_;
    // Mirrors Scene::halfResolutionAO, latched at the start of Render so the
    // target size, viewport and constants all agree within a frame.
    bool traceHalfResolution_ = false;
    // What Scene asked for this frame; applied at the next frame boundary.
    bool pendingHalfResolution_ = false;
    ComPtr<ID3D12Resource> constantBuffer_, rawAO_;
    ComPtr<ID3D12Resource> bentAO_, aoHistory_[2];
    BYTE* mappedConstants_ = nullptr;
    UINT descriptorSize_ = 0;
    UINT targetWidth_ = 0;
    UINT targetHeight_ = 0;
    UINT historyReadIndex_ = 0;
    bool historyValid_ = false;
    bool temporalEnabledLastFrame_ = false;
    UINT lastTemporalFrame_ = UINT_MAX;
};

#endif
