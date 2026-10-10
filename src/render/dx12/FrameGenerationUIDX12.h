#pragma once

#include "ShaderCacheDX12.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <fstream>
#include <sstream>

// Own buffers per swap-chain slot: SL consumes them on its presenting queue.
// Init/Release are restricted to drained resize/feature transitions.
class FrameGenerationUIDX12 {
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Slot { Resource hudless, ui; bool used = false; };
    std::vector<Slot> slots_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_, srv_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    UINT rtvStride_ = 0, srvStride_ = 0;
    static constexpr auto ReadState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        list->ResourceBarrier(1, &b);
    }

public:
    bool Ready() const { return !slots_.empty(); }
    void Release() {
        slots_.clear(); rtv_.Reset(); srv_.Reset(); root_.Reset(); pso_.Reset();
    }
    bool Init(ID3D12Device* device, UINT width, UINT height, UINT frames) {
        Release();
        if (!device || !width || !height) return false;
        std::ifstream file(ShaderCacheDX12::ExecutableDirectory() +
                           L"shaders/frame_generation_ui.hlsl");
        std::stringstream text; text << file.rdbuf();
        const std::string source = text.str();
        Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, errors, serialized;
        constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        if (FAILED(ShaderCacheDX12::CompileCached(source.data(), source.size(),
            "frame_generation_ui.hlsl", nullptr, nullptr, "VSMain", "vs_5_1",
            flags, 0, &vs, &errors)) ||
            FAILED(ShaderCacheDX12::CompileCached(source.data(), source.size(),
            "frame_generation_ui.hlsl", nullptr, nullptr, "PSMain", "ps_5_1",
            flags, 0, &ps, &errors))) return false;

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 2;
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &range;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 1;
        signature.pParameters = &parameter;
        signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized, &errors)) || FAILED(device->CreateRootSignature(0,
            serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_))))
            return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root_.Get();
        pipeline.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pipeline.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipeline.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pso_))))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = frames;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtv_)))) return false;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = frames * 2;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srv_)))) return false;
        rtvStride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srvStride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = width; texture.Height = height;
        texture.DepthOrArraySize = 1; texture.MipLevels = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.SampleDesc.Count = 1;
        slots_.resize(frames);
        auto srvHandle = srv_->GetCPUDescriptorHandleForHeapStart();
        auto rtvHandle = rtv_->GetCPUDescriptorHandleForHeapStart();
        for (auto& slot : slots_) {
            texture.Flags = D3D12_RESOURCE_FLAG_NONE;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &texture, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.hudless)))) {
                Release(); return false;
            }
            texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clear{}; clear.Format = texture.Format;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &texture, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&slot.ui)))) {
                Release(); return false;
            }
            slot.hudless->SetName(L"DLSS FG HUD-less");
            slot.ui->SetName(L"DLSS FG premultiplied UI");
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = texture.Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(slot.hudless.Get(), &view, srvHandle);
            srvHandle.ptr += srvStride_;
            device->CreateShaderResourceView(slot.ui.Get(), &view, srvHandle);
            srvHandle.ptr += srvStride_;
            device->CreateRenderTargetView(slot.ui.Get(), nullptr, rtvHandle);
            rtvHandle.ptr += rtvStride_;
        }
        return true;
    }
    ID3D12Resource* Hudless(UINT slot) const { return slots_[slot].hudless.Get(); }
    ID3D12Resource* UI(UINT slot) const { return slots_[slot].ui.Get(); }

    void BeginUI(ID3D12GraphicsCommandList* list, ID3D12Resource* backBuffer, UINT index) {
        Slot& slot = slots_[index];
        if (slot.used) {
            Barrier(list, slot.hudless.Get(), ReadState, D3D12_RESOURCE_STATE_COPY_DEST);
            Barrier(list, slot.ui.Get(), ReadState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        Barrier(list, backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(slot.hudless.Get(), backBuffer);
        Barrier(list, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Barrier(list, slot.hudless.Get(), D3D12_RESOURCE_STATE_COPY_DEST, ReadState);
        auto handle = rtv_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += index * rtvStride_;
        const float clear[4] = {};
        list->ClearRenderTargetView(handle, clear, 0, nullptr);
        list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
    }
    void Composite(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE backRTV,
                   UINT index, UINT width, UINT height) {
        Slot& slot = slots_[index];
        Barrier(list, slot.ui.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, ReadState);
        slot.used = true;
        ID3D12DescriptorHeap* heaps[] = { srv_.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(root_.Get());
        list->SetPipelineState(pso_.Get());
        auto handle = srv_->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += index * 2 * srvStride_;
        list->SetGraphicsRootDescriptorTable(0, handle);
        list->OMSetRenderTargets(1, &backRTV, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{ 0, 0, float(width), float(height), 0, 1 };
        const D3D12_RECT scissor{ 0, 0, LONG(width), LONG(height) };
        list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
    }
};
