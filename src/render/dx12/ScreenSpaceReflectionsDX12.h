#ifndef SCREEN_SPACE_REFLECTIONS_DX12_H
#define SCREEN_SPACE_REFLECTIONS_DX12_H

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include "Scene.h"
#include <d3dcompiler.h>
#include <cstring>
#include <fstream>
#include <sstream>

class ScreenSpaceReflectionsDX12 {
public:
    bool initialized = false;

    ~ScreenSpaceReflectionsDX12() {
        if (constantBuffer_ && mappedConstants_) constantBuffer_->Unmap(0, nullptr);
    }

    bool Init() {
        std::ifstream file("shaders/screen_space_reflections.hlsl");
        if (!file) return false;
        std::stringstream stream;
        stream << file.rdbuf();
        const std::string source = stream.str();
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                           D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ComPtr<ID3DBlob> vs, ps, ngPs, compositePs, errors;
        if (!Compile(source, "VSMain", "vs_5_0", flags, vs, errors) ||
            !Compile(source, "PSMain", "ps_5_0", flags, ps, errors) ||
            !Compile(source, "PSTraceNG", "ps_5_0", flags, ngPs, errors) ||
            !Compile(source, "PSCompositeNG", "ps_5_0", flags, compositePs,
                     errors) ||
            !CreateRootSignature() ||
            !CreatePipeline(vs.Get(), ps.Get(), ngPs.Get(),
                            compositePs.Get()) ||
            !CreateResources()) return false;
        initialized = true;
        return true;
    }

    void Render(const Scene& scene, ID3D12Resource* hdrSourceTarget,
                D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv,
                ID3D12Resource* depth,
                ID3D12Resource* normalRoughness) {
        if (!initialized || !hdrSourceTarget || !hdrRtv.ptr || !depth ||
            !g_dx12.commandList || !EnsureSceneCopy()) return;
        Update(scene, depth, normalRoughness);
        ID3D12GraphicsCommandList* list = g_dx12.commandList.Get();
        // With every contribution switched off there is nothing to trace, so
        // fall back to the default mirror path rather than spending the trace
        // and composite draws producing black.
        const bool ngActive = scene.screenSpaceRTEnabled &&
            (scene.screenSpaceRTSpecular || scene.screenSpaceRTGI ||
             scene.screenSpaceRTAO);

        Transition(list, hdrSourceTarget, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(sceneCopy_.Get(), hdrSourceTarget);
        Transition(list, hdrSourceTarget, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        Transition(list, sceneCopy_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(list, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (normalRoughness)
            Transition(list, normalRoughness,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        list->RSSetViewports(1, &g_dx12.viewport);
        list->RSSetScissorRects(1, &g_dx12.scissorRect);

        // The NG path traces once, into its own history target. That result is
        // then composited into the scene by a second, trace-free pass. Tracing
        // straight into the additively-blended scene target would leave nothing
        // readable as next frame's history -- the scene target already holds
        // the lit image -- and re-tracing to produce one would double the cost
        // of the most expensive pass in the feature.
        if (ngActive) {
            // Ping-pong the two accumulation buffers: the trace reads last
            // frame's and writes this frame's. Reading and writing one texture
            // in a single draw is undefined, which is why there are two.
            const UINT writeIndex = ngWriteIndex_;
            Transition(list, ngAccum_[writeIndex].Get(),
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->SetPipelineState(ngPipeline_.Get());
            list->SetGraphicsRootSignature(rootSignature_.Get());
            ID3D12DescriptorHeap* ngHeaps[] = { descriptorHeap_.Get() };
            list->SetDescriptorHeaps(1, ngHeaps);
            list->SetGraphicsRootConstantBufferView(
                0, constantBuffer_->GetGPUVirtualAddress() +
                   g_dx12.frameIndex * 256u);
            // Trace reads last frame's buffer: the variant that is not being
            // written.
            list->SetGraphicsRootDescriptorTable(
                1, VariantTable(writeIndex ^ 1u));
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            D3D12_CPU_DESCRIPTOR_HANDLE accumRtv =
                ngAccumRTV_->GetCPUDescriptorHandleForHeapStart();
            accumRtv.ptr += static_cast<SIZE_T>(rtvSize_) * writeIndex;
            list->OMSetRenderTargets(1, &accumRtv, FALSE, nullptr);
            list->DrawInstanced(3, 1, 0, 0);
            Transition(list, ngAccum_[writeIndex].Get(),
                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ngHistoryValid_ = true;
            ++ngFrameIndex_;
        }

        list->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);
        list->SetPipelineState(ngActive ? ngCompositePipeline_.Get()
                                        : pipeline_.Get());
        list->SetGraphicsRootSignature(rootSignature_.Get());
        ID3D12DescriptorHeap* heaps[] = { descriptorHeap_.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootConstantBufferView(
            0, constantBuffer_->GetGPUVirtualAddress() +
               g_dx12.frameIndex * 256u);
        // The composite reads the buffer the trace just wrote; the default path
        // does not use t3 at all, so either variant serves it.
        list->SetGraphicsRootDescriptorTable(1, VariantTable(ngWriteIndex_));
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);

        if (ngActive) {
            // The freshly traced result becomes next frame's history.
            ngWriteIndex_ ^= 1u;
        } else {
            // History goes stale the moment the feature is switched off, so a
            // later re-enable must not blend against a frame from minutes ago.
            ngHistoryValid_ = false;
        }

        Transition(list, sceneCopy_.Get(),
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(list, depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (normalRoughness)
            Transition(list, normalRoughness,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

private:
    // t0 scene copy, t1 depth, t2 normal/roughness, t3 accumulation.
    static constexpr UINT kDescriptorsPerVariant = 4;

    struct Constants {
        XMFLOAT4X4 inverseViewProjection;
        XMFLOAT4X4 viewProjection;
        XMFLOAT4 cameraNear;
        XMFLOAT4 screenParams;
        XMFLOAT4 ssrParams;
        XMFLOAT4 ngParams;
        XMFLOAT4 ngTrace;
        XMFLOAT4 ngTemporal;
    };

    bool Compile(const std::string& source, const char* entry, const char* target,
                 UINT flags, ComPtr<ID3DBlob>& output,
                 ComPtr<ID3DBlob>& errors) {
        HRESULT hr = ShaderCacheDX12::CompileCached(
            source.data(), source.size(), "screen_space_reflections.hlsl",
            nullptr, nullptr, entry, target, flags, 0, &output, &errors);
        if (FAILED(hr) && errors)
            std::cerr << static_cast<const char*>(errors->GetBufferPointer());
        return SUCCEEDED(hr);
    }

    bool CreateRootSignature() {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        // t3 is the NGLighting accumulation history.
        range.NumDescriptors = 4;
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

    bool CreatePipeline(ID3DBlob* vs, ID3DBlob* ps, ID3DBlob* ngPs,
                        ID3DBlob* compositePs) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = rootSignature_.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        auto& blend = desc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_ONE;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED |
            D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&pipeline_)))) return false;

        // NG trace: writes the raw gather into the accumulation buffer. No
        // blending, and alpha is written too -- alpha carries the depth the
        // gather was traced against, which next frame reprojects against.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC ngDesc = desc;
        ngDesc.PS = { ngPs->GetBufferPointer(), ngPs->GetBufferSize() };
        ngDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
        ngDesc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &ngDesc, IID_PPV_ARGS(&ngPipeline_)))) return false;

        // NG composite: adds the accumulated result into the scene. Shares the
        // additive blend of the default path, so occlusion arrives as the
        // negative term the trace shader already folded in.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC compositeDesc = desc;
        compositeDesc.PS = { compositePs->GetBufferPointer(),
                             compositePs->GetBufferSize() };
        return SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
            &compositeDesc, IID_PPV_ARGS(&ngCompositePipeline_)));
    }

    bool CreateResources() {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        // Two 4-descriptor variants. They differ only in t3: variant 0 binds
        // accumulation buffer 0 there, variant 1 binds buffer 1. The trace
        // selects the variant holding last frame's buffer and the composite the
        // one holding the buffer just written, so neither has to have a
        // descriptor rewritten mid-frame -- which would race the GPU, since
        // recording a draw does not mean it has executed.
        heapDesc.NumDescriptors = kDescriptorsPerVariant * 2u;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&descriptorHeap_)))) return false;
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
        return CreateSceneCopy();
    }

    bool EnsureSceneCopy() {
        if (sceneCopy_ && copyWidth_ == g_dx12.screenWidth &&
            copyHeight_ == g_dx12.screenHeight) return true;
        sceneCopy_.Reset();
        return CreateSceneCopy();
    }

    bool CreateSceneCopy() {
        copyWidth_ = g_dx12.screenWidth;
        copyHeight_ = g_dx12.screenHeight;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = copyWidth_;
        desc.Height = copyHeight_;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&sceneCopy_)))) return false;

        // NGLighting accumulation: the gather in rgb and the depth it was
        // traced against in alpha, which is what the next frame reprojects
        // against. Two buffers so a frame can read one while writing the other.
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        // No clear value: the trace pass writes every pixel of the target, so
        // it is never cleared, and ngHistoryValid_ covers the one frame where
        // the contents are still undefined.
        for (UINT i = 0; i < 2; ++i) {
            ngAccum_[i].Reset();
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&ngAccum_[i])))) return false;
        }
        // Freshly created buffers hold garbage, so the first frame after a
        // resize must not blend against them.
        ngHistoryValid_ = false;
        ngWriteIndex_ = 0;

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = 2;
        if (!ngAccumRTV_ &&
            FAILED(g_dx12.device->CreateDescriptorHeap(
                &rtvDesc, IID_PPV_ARGS(&ngAccumRTV_)))) return false;
        rtvSize_ = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
            ngAccumRTV_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < 2; ++i) {
            g_dx12.device->CreateRenderTargetView(
                ngAccum_[i].Get(), nullptr, rtvHandle);
            rtvHandle.ptr += rtvSize_;
        }
        return true;
    }

    void Update(const Scene& scene, ID3D12Resource* depth,
                ID3D12Resource* normalRoughness) {
        Constants constants = {};
        XMMATRIX vp = scene.GetViewMatrix() * scene.GetProjectionMatrix();
        XMStoreFloat4x4(&constants.inverseViewProjection,
            XMMatrixTranspose(XMMatrixInverse(nullptr, vp)));
        XMStoreFloat4x4(&constants.viewProjection, XMMatrixTranspose(vp));
        constants.cameraNear = {
            scene.camera.Position.x, scene.camera.Position.y,
            scene.camera.Position.z, scene.cameraNear };
        constants.screenParams = {
            static_cast<float>(g_dx12.screenWidth),
            static_cast<float>(g_dx12.screenHeight),
            1.0f / g_dx12.screenWidth, 1.0f / g_dx12.screenHeight };
        constants.ssrParams = {
            scene.screenSpaceReflectionDistance,
            scene.screenSpaceReflectionThickness,
            scene.screenSpaceReflectionStrength,
            normalRoughness ? 1.0f : 0.0f };
        constants.ngParams = {
            scene.screenSpaceRTEnabled ? 1.0f : 0.0f,
            scene.screenSpaceRTGI ? 1.0f : 0.0f,
            scene.screenSpaceRTGIStrength,
            static_cast<float>(scene.screenSpaceRTRaySteps) };
        constants.ngTrace = {
            scene.screenSpaceRTRayGrowth,
            scene.screenSpaceRTAO ? scene.screenSpaceRTAOStrength : 0.0f,
            scene.screenSpaceRTAORadius,
            static_cast<float>(ngFrameIndex_) };
        constants.ngTemporal = {
            scene.screenSpaceRTAccumulation,
            ngHistoryValid_ ? 1.0f : 0.0f,
            scene.screenSpaceRTSpecular ? 1.0f : 0.0f,
            scene.screenSpaceRTAO ? 1.0f : 0.0f };
        std::memcpy(mappedConstants_ + g_dx12.frameIndex * 256u,
                    &constants, sizeof(constants));

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        for (UINT variant = 0; variant < 2; ++variant) {
            auto handle = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(descriptorSize_) *
                          kDescriptorsPerVariant * variant;
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            g_dx12.device->CreateShaderResourceView(
                sceneCopy_.Get(), &srv, handle);
            handle.ptr += descriptorSize_;
            srv.Format = DXGI_FORMAT_R32_FLOAT;
            g_dx12.device->CreateShaderResourceView(depth, &srv, handle);
            handle.ptr += descriptorSize_;
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            g_dx12.device->CreateShaderResourceView(
                normalRoughness, &srv, handle);
            handle.ptr += descriptorSize_;
            // t3 is this variant's accumulation buffer.
            g_dx12.device->CreateShaderResourceView(
                ngAccum_[variant].Get(), &srv, handle);
        }
    }

    D3D12_GPU_DESCRIPTOR_HANDLE VariantTable(UINT variant) const {
        D3D12_GPU_DESCRIPTOR_HANDLE table =
            descriptorHeap_->GetGPUDescriptorHandleForHeapStart();
        table.ptr += static_cast<UINT64>(descriptorSize_) *
                     kDescriptorsPerVariant * variant;
        return table;
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
    ComPtr<ID3D12PipelineState> pipeline_;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    ComPtr<ID3D12PipelineState> ngPipeline_, ngCompositePipeline_;
    ComPtr<ID3D12Resource> constantBuffer_, sceneCopy_;
    ComPtr<ID3D12Resource> ngAccum_[2];
    ComPtr<ID3D12DescriptorHeap> ngAccumRTV_;
    BYTE* mappedConstants_ = nullptr;
    UINT descriptorSize_ = 0;
    UINT rtvSize_ = 0;
    UINT copyWidth_ = 0;
    UINT copyHeight_ = 0;
    UINT ngFrameIndex_ = 0;
    UINT ngWriteIndex_ = 0;
    bool ngHistoryValid_ = false;
};

#endif
