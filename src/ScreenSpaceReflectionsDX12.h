#ifndef SCREEN_SPACE_REFLECTIONS_DX12_H
#define SCREEN_SPACE_REFLECTIONS_DX12_H

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
        ComPtr<ID3DBlob> vs, ps, errors;
        if (!Compile(source, "VSMain", "vs_5_0", flags, vs, errors) ||
            !Compile(source, "PSMain", "ps_5_0", flags, ps, errors) ||
            !CreateRootSignature() || !CreatePipeline(vs.Get(), ps.Get()) ||
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

        list->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);
        list->RSSetViewports(1, &g_dx12.viewport);
        list->RSSetScissorRects(1, &g_dx12.scissorRect);
        list->SetPipelineState(pipeline_.Get());
        list->SetGraphicsRootSignature(rootSignature_.Get());
        ID3D12DescriptorHeap* heaps[] = { descriptorHeap_.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootConstantBufferView(
            0, constantBuffer_->GetGPUVirtualAddress() +
               g_dx12.frameIndex * 256u);
        list->SetGraphicsRootDescriptorTable(
            1, descriptorHeap_->GetGPUDescriptorHandleForHeapStart());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);

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
    struct Constants {
        XMFLOAT4X4 inverseViewProjection;
        XMFLOAT4X4 viewProjection;
        XMFLOAT4 cameraNear;
        XMFLOAT4 screenParams;
        XMFLOAT4 ssrParams;
    };

    bool Compile(const std::string& source, const char* entry, const char* target,
                 UINT flags, ComPtr<ID3DBlob>& output,
                 ComPtr<ID3DBlob>& errors) {
        HRESULT hr = D3DCompile(
            source.data(), source.size(), "screen_space_reflections.hlsl",
            nullptr, nullptr, entry, target, flags, 0, &output, &errors);
        if (FAILED(hr) && errors)
            std::cerr << static_cast<const char*>(errors->GetBufferPointer());
        return SUCCEEDED(hr);
    }

    bool CreateRootSignature() {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 3;
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

    bool CreatePipeline(ID3DBlob* vs, ID3DBlob* ps) {
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
        return SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
            &desc, IID_PPV_ARGS(&pipeline_)));
    }

    bool CreateResources() {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 3;
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
        std::memcpy(mappedConstants_ + g_dx12.frameIndex * 256u,
                    &constants, sizeof(constants));

        auto handle = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
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
    ComPtr<ID3D12Resource> constantBuffer_, sceneCopy_;
    BYTE* mappedConstants_ = nullptr;
    UINT descriptorSize_ = 0;
    UINT copyWidth_ = 0;
    UINT copyHeight_ = 0;
};

#endif
