#ifndef NIGHT_VISION_DX12_H
#define NIGHT_VISION_DX12_H

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>

// Night vision goggles post-process. Structured exactly like FXAADX12: copy the
// backbuffer aside, then draw a fullscreen triangle that samples the copy and
// writes the intensified image back over the top. Sharing that shape keeps the
// two passes interchangeable in the frame's post chain. NVG replaces FXAA while
// raised and runs before ImGui, so source detail and HUD text both stay sharp.
class NightVisionDX12 {
public:
    bool initialized = false;

    bool Init(UINT width, UINT height) {
        std::ifstream shaderFile("shaders/night_vision.hlsl");
        if (!shaderFile) return false;
        std::stringstream shaderText;
        shaderText << shaderFile.rdbuf();
        const std::string source = shaderText.str();

        ComPtr<ID3DBlob> vs, ps, errors;
        const UINT flags =
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        HRESULT hr = ShaderCacheDX12::CompileCached(
            source.data(), source.size(), "night_vision.hlsl",
            nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &vs, &errors);
        if (FAILED(hr)) {
            if (errors)
                std::cerr << static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }
        errors.Reset();
        hr = ShaderCacheDX12::CompileCached(
            source.data(), source.size(), "night_vision.hlsl",
            nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &ps, &errors);
        if (FAILED(hr)) {
            if (errors)
                std::cerr << static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }

        // t0 scene copy, t1 scene depth. Depth is what lets the IR illuminator
        // be a real spotlight: without a distance per pixel the lamp can only be
        // a screen-space vignette, lighting a far wall exactly as hard as a near
        // one.
        D3D12_DESCRIPTOR_RANGE textureRange = {};
        textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRange.NumDescriptors = 2;
        textureRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER roots[2] = {};
        roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[0].DescriptorTable.NumDescriptorRanges = 1;
        roots[0].DescriptorTable.pDescriptorRanges = &textureRange;
        roots[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        // inverseScreenSize, time, strength, gain, overload, then the depth
        // linearisation pair (near, far) the illuminator needs.
        roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        roots[1].Constants.ShaderRegister = 0;
        roots[1].Constants.Num32BitValues = 8;
        roots[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler = {};
        // This is a same-resolution scene copy. Point sampling preserves the
        // exact source pixel; linear filtering made already-dark silhouettes
        // look optically defocused before the intensifier curve even ran.
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = roots;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers = &sampler;
        rootDesc.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized;
        hr = D3D12SerializeRootSignature(
            &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
        if (FAILED(hr)) return false;
        hr = g_dx12.device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature_));
        if (FAILED(hr)) return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {};
        pipeline.pRootSignature = rootSignature_.Get();
        pipeline.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pipeline.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.DepthStencilState.DepthEnable = FALSE;
        pipeline.DepthStencilState.StencilEnable = FALSE;
        pipeline.SampleMask = UINT_MAX;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipeline.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &pipeline, IID_PPV_ARGS(&pipelineState_))))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 2;   // scene copy + depth
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&srvHeap_))))
            return false;

        initialized = CreateSceneCopy(width, height);
        return initialized;
    }

    bool Resize(UINT width, UINT height) {
        if (!rootSignature_ || width == 0 || height == 0) return false;
        sceneCopy_.Reset();
        initialized = CreateSceneCopy(width, height);
        return initialized;
    }

    // strength is the 0..1 ramp; time drives the grain and is wrapped by the
    // caller so it never grows large enough to lose float precision in sin().
    //
    // gain is the automatic gain control level the caller has settled on for the
    // current scene brightness, and overload is how far the tube is being driven
    // past what it can handle -- 0 in proper darkness, rising toward 1 in
    // daylight, where it washes the image out the way real goggles do.
    void Apply(ID3D12GraphicsCommandList* commandList, float time,
               float strength, float gain, float overload,
               float nearPlane, float farPlane) {
        if (!initialized || !commandList || !sceneCopy_) return;
        if (strength <= 0.0f) return;
        ID3D12Resource* backBuffer =
            g_dx12.renderTargets[g_dx12.frameIndex].Get();

        Transition(commandList, backBuffer,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->CopyResource(sceneCopy_.Get(), backBuffer);
        Transition(commandList, sceneCopy_.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(commandList, backBuffer,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        // Depth was written earlier this frame and is still in DEPTH_WRITE.
        // Read it as a texture for the illuminator, then hand it back.
        ID3D12Resource* depthBuffer = g_dx12.depthStencilBuffer.Get();
        if (depthBuffer)
            Transition(commandList, depthBuffer,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetCPUDescriptorHandle(
            g_dx12.rtvHeap.Get(), g_dx12.rtvDescriptorSize, g_dx12.frameIndex);
        commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        commandList->RSSetViewports(1, &g_dx12.viewport);
        commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
        commandList->SetPipelineState(pipelineState_.Get());
        commandList->SetGraphicsRootSignature(rootSignature_.Get());
        ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetGraphicsRootDescriptorTable(
            0, srvHeap_->GetGPUDescriptorHandleForHeapStart());
        const float constants[8] = {
            1.0f / static_cast<float>(width_),
            1.0f / static_cast<float>(height_),
            time,
            strength,
            gain,
            overload,
            nearPlane,
            farPlane
        };
        commandList->SetGraphicsRoot32BitConstants(1, 8, constants, 0);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->DrawInstanced(3, 1, 0, 0);

        Transition(commandList, sceneCopy_.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
        if (depthBuffer)
            Transition(commandList, depthBuffer,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

private:
    static void Transition(ID3D12GraphicsCommandList* commandList,
                           ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);
    }

    bool CreateSceneCopy(UINT width, UINT height) {
        width_ = width;
        height_ = height;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture = {};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = width;
        texture.Height = height;
        texture.DepthOrArraySize = 1;
        texture.MipLevels = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &texture,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&sceneCopy_))))
            return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = texture.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(
            sceneCopy_.Get(), &srv,
            srvHeap_->GetCPUDescriptorHandleForHeapStart());

        // Depth in slot 1, read as R32_FLOAT (the buffer is D32_FLOAT). Created
        // here rather than per-frame so Apply only has to transition it.
        D3D12_CPU_DESCRIPTOR_HANDLE depthHandle =
            srvHeap_->GetCPUDescriptorHandleForHeapStart();
        depthHandle.ptr += g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSRV = {};
        depthSRV.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSRV.Format = DXGI_FORMAT_R32_FLOAT;
        depthSRV.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSRV.Texture2D.MipLevels = 1;
        if (g_dx12.depthStencilBuffer)
            g_dx12.device->CreateShaderResourceView(
                g_dx12.depthStencilBuffer.Get(), &depthSRV, depthHandle);
        return true;
    }

    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipelineState_;
    ComPtr<ID3D12DescriptorHeap> srvHeap_;
    ComPtr<ID3D12Resource> sceneCopy_;
    UINT width_ = 0;
    UINT height_ = 0;
};

#endif
