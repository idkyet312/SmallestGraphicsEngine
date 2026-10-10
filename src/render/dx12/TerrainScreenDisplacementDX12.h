#ifndef TERRAIN_SCREEN_DISPLACEMENT_DX12_H
#define TERRAIN_SCREEN_DISPLACEMENT_DX12_H

#include "DX12Core.h"
#include "ShaderCacheDX12.h"
#include "ProfilerDX12.h"
#include <array>
#include <cstddef>
#include <fstream>
#include <sstream>

extern ProfilerDX12 g_profiler;

class TerrainScreenDisplacementDX12 {
public:
    // Root constants keep this separate from the hand-maintained resolve CB.
    struct Constants {
        XMFLOAT4X4 inverseViewProjection;
        XMFLOAT4X4 viewProjection;
        XMFLOAT3 cameraPosition;
        float strength = 1.0f;
        XMFLOAT2 screenSize;
        float fadeStart = 12.0f;
        float fadeEnd = 28.0f;
        XMFLOAT2 splatInvExtent;
        UINT splatEnabled = 0;
        UINT authoredPaths = 0;
        UINT neutralHeightMask = 0;
        UINT marchSteps = 24;
        float maxRayTravel = 0.75f;
        float edgeFadePixels = 16.0f;
    };
    static_assert(sizeof(Constants) == 192, "DisplacementConstants layout");
    static_assert(offsetof(Constants, screenSize) == 144, "Displacement screen layout");
    static_assert(offsetof(Constants, neutralHeightMask) == 176, "Displacement settings layout");

    // Called only for the requested main view. Allocations happen on first
    // activation (or after resize), never again while that size is in use.
    bool Prepare(ID3D12Resource* visibility, ID3D12Resource* depth) {
        if (failed || !visibility || !depth) return false;
        if (!rootSignature && !CreatePipelines()) return Fail("pipelines");
        const auto desc = visibility->GetDesc();
        const auto depthDesc = depth->GetDesc();
        if (desc.Format != DXGI_FORMAT_R32G32_UINT || desc.SampleDesc.Count != 1 ||
            depthDesc.Format != DXGI_FORMAT_R32_TYPELESS || depthDesc.SampleDesc.Count != 1 ||
            desc.Width != depthDesc.Width || desc.Height != depthDesc.Height)
            return Fail("target formats");
        if (slots[0].visibility) {
            const auto existing = slots[0].visibility->GetDesc();
            return existing.Width == desc.Width && existing.Height == desc.Height;
        }
        if (!CreateResources(visibility->GetDesc(), depth->GetDesc()))
            return Fail("screen resources");
        return true;
    }

    // Resize already runs under the renderer's idle-GPU contract. Keep all
    // frame-slot resources alive across toggles rather than retiring live SRVs.
    void ResetResources() {
        for (auto& slot : slots) slot = Slot{};
        failed = false;
    }

    void Render(ID3D12GraphicsCommandList* cmd, ID3D12Resource* visibility,
                ID3D12Resource* depth, D3D12_CPU_DESCRIPTOR_HANDLE visibilityRTV,
                D3D12_CPU_DESCRIPTOR_HANDLE depthDSV,
                ID3D12Resource* albedo, ID3D12Resource* normal,
                ID3D12Resource* packed, ID3D12Resource* splat,
                const Constants& constants) {
        Slot& slot = slots[g_dx12.frameIndex % FRAME_COUNT];
        cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        WriteMaterialDescriptors(slot, albedo, normal, packed, splat);
        {
            ProfilerDX12::Scope timer(g_profiler, "Terrain Displacement Snapshot", cmd);
            Transition(cmd, visibility, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(cmd, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(cmd, slot.visibility.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            Transition(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(slot.visibility.Get(), visibility);
            cmd->CopyResource(slot.depth.Get(), depth);
            Transition(cmd, slot.visibility.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(cmd, visibility, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Transition(cmd, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
        ID3D12DescriptorHeap* heaps[] = { slot.srvHeap.Get() };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootSignature(rootSignature.Get());
        cmd->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &constants, 0);
        cmd->SetGraphicsRootDescriptorTable(1, slot.srvHeap->GetGPUDescriptorHandleForHeapStart());
        D3D12_VIEWPORT viewport = { 0, 0, constants.screenSize.x, constants.screenSize.y, 0, 1 };
        D3D12_RECT scissor = { 0, 0, LONG(constants.screenSize.x), LONG(constants.screenSize.y) };
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto heightRTV = slot.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        {
            ProfilerDX12::Scope timer(g_profiler, "Terrain Displacement Height", cmd);
            Transition(cmd, slot.height.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmd->OMSetRenderTargets(1, &heightRTV, FALSE, nullptr);
            cmd->SetPipelineState(heightPSO.Get());
            cmd->DrawInstanced(3, 1, 0, 0);
            // Unbind before exposing the height target as a pixel-shader SRV.
            cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
            Transition(cmd, slot.height.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        {
            ProfilerDX12::Scope timer(g_profiler, "Terrain Displacement Raymarch", cmd);
            cmd->OMSetRenderTargets(1, &visibilityRTV, FALSE, &depthDSV);
            cmd->SetPipelineState(displacePSO.Get());
            cmd->DrawInstanced(3, 1, 0, 0);
            cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
            Transition(cmd, visibility, D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }

private:
    struct Slot {
        ComPtr<ID3D12Resource> visibility, depth, height;
        ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap;
    };
    std::array<Slot, FRAME_COUNT> slots;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> heightPSO, displacePSO;
    bool failed = false;

    bool Fail(const char* stage) {
        failed = true;
        std::cerr << "Terrain screen displacement unavailable: " << stage << " (using POM)\n";
        return false;
    }

    static void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &barrier);
    }

    static D3D12_CPU_DESCRIPTOR_HANDLE Handle(const Slot& slot, UINT index) {
        auto handle = slot.srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(index) * g_dx12.cbvSrvUavDescriptorSize;
        return handle;
    }

    bool CreateResources(D3D12_RESOURCE_DESC visDesc, D3D12_RESOURCE_DESC depthDesc) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        visDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_RESOURCE_DESC heightDesc = visDesc;
        heightDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        heightDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        // Publish only after every frame slot succeeds. A partial allocation
        // must not make the readiness gate switch the raster pass away from POM.
        std::array<Slot, FRAME_COUNT> created;
        for (auto& slot : created) {
            auto texture = [&](const D3D12_RESOURCE_DESC& desc, ComPtr<ID3D12Resource>& resource) {
                return SUCCEEDED(g_dx12.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                    &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&resource)));
            };
            if (!texture(visDesc, slot.visibility) || !texture(depthDesc, slot.depth) ||
                !texture(heightDesc, slot.height)) return false;
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = 7;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(g_dx12.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&slot.srvHeap)))) return false;
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = 1;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(g_dx12.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&slot.rtvHeap)))) return false;
            g_dx12.device->CreateRenderTargetView(slot.height.Get(), nullptr,
                slot.rtvHeap->GetCPUDescriptorHandleForHeapStart());
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            srv.Format = visDesc.Format;
            g_dx12.device->CreateShaderResourceView(slot.visibility.Get(), &srv, Handle(slot, 0));
            srv.Format = DXGI_FORMAT_R32_FLOAT;
            g_dx12.device->CreateShaderResourceView(slot.depth.Get(), &srv, Handle(slot, 1));
            srv.Format = heightDesc.Format;
            g_dx12.device->CreateShaderResourceView(slot.height.Get(), &srv, Handle(slot, 2));
        }
        slots = std::move(created);
        return true;
    }

    void WriteMaterialDescriptors(const Slot& slot, ID3D12Resource* albedo,
                                  ID3D12Resource* normal, ID3D12Resource* packed,
                                  ID3D12Resource* splat) {
        ID3D12Resource* arrays[] = { albedo, normal, packed };
        for (UINT i = 0; i < 3; ++i) {
            const auto desc = arrays[i]->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            // The engine's layer arrays are TYPELESS, which no view accepts.
            // Same views as VisibilityBufferDX12::WriteTerrainDescriptors.
            srv.Format = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS
                ? (i == 0 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM)
                : desc.Format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.ArraySize = desc.DepthOrArraySize;
            srv.Texture2DArray.MipLevels = desc.MipLevels;
            g_dx12.device->CreateShaderResourceView(arrays[i], &srv, Handle(slot, 3 + i));
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = splat ? splat->GetDesc().Format : DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(splat, &srv, Handle(slot, 6));
    }

    bool CreatePipelines() {
        std::ifstream file("shaders/terrain_screen_displacement.hlsl");
        if (!file) return false;
        std::stringstream stream; stream << file.rdbuf();
        const std::string source = stream.str();
        ComPtr<ID3DBlob> vs, height, displace, errors;
        auto compile = [&](const char* entry, const char* profile, ComPtr<ID3DBlob>& blob) {
            HRESULT hr = ShaderCacheDX12::CompileCached(source.data(), source.size(),
                "shaders/terrain_screen_displacement.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                entry, profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &blob, &errors);
            if (FAILED(hr) && errors) std::cerr << (const char*)errors->GetBufferPointer() << '\n';
            return SUCCEEDED(hr);
        };
        if (!compile("FullscreenVS", "vs_5_1", vs) || !compile("HeightPS", "ps_5_1", height) ||
            !compile("DisplacePS", "ps_5_1", displace)) return false;
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 7;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = sizeof(Constants) / 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        for (UINT i = 0; i < 2; ++i) {
            auto& sampler = samplers[i];
            sampler.ShaderRegister = i;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW =
                i == 0 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        }
        D3D12_ROOT_SIGNATURE_DESC root = {};
        root.NumParameters = 2; root.pParameters = params;
        root.NumStaticSamplers = 2; root.pStaticSamplers = samplers;
        ComPtr<ID3DBlob> signature;
        if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
                    &signature, &errors))) return false;
        ComPtr<ID3D12RootSignature> createdRoot;
        if (FAILED(g_dx12.device->CreateRootSignature(0, signature->GetBufferPointer(),
                    signature->GetBufferSize(), IID_PPV_ARGS(&createdRoot)))) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = createdRoot.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pso.PS = { height->GetBufferPointer(), height->GetBufferSize() };
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        auto& blend = pso.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DepthStencilState.StencilReadMask = pso.DepthStencilState.StencilWriteMask = 0xFF;
        pso.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        pso.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        pso.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
        pso.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&heightPSO)))) return false;
        pso.PS = { displace->GetBufferPointer(), displace->GetBufferSize() };
        pso.RTVFormats[0] = DXGI_FORMAT_R32G32_UINT;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&displacePSO)))) return false;
        rootSignature = std::move(createdRoot);
        return true;
    }
};

#endif
