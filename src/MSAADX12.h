#ifndef MSAA_DX12_H
#define MSAA_DX12_H

#include "DX12Core.h"
#include <utility>

class MSAADX12 {
public:
    static constexpr UINT SampleCount = 4;
    bool initialized = false;

    bool Init(UINT width, UINT height) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS colorSupport = {};
        colorSupport.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        colorSupport.SampleCount = SampleCount;
        colorSupport.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS depthSupport = colorSupport;
        depthSupport.Format = DXGI_FORMAT_D32_FLOAT;
        if (FAILED(g_dx12.device->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                &colorSupport, sizeof(colorSupport))) ||
            FAILED(g_dx12.device->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                &depthSupport, sizeof(depthSupport))) ||
            colorSupport.NumQualityLevels == 0 ||
            depthSupport.NumQualityLevels == 0) {
            initialized = false;
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = 1;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &rtvHeapDesc, IID_PPV_ARGS(&rtvHeap_)))) {
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.NumDescriptors = 1;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &dsvHeapDesc, IID_PPV_ARGS(&dsvHeap_)))) {
            return false;
        }

        initialized = CreateTargets(width, height);
        return initialized;
    }

    bool Resize(UINT width, UINT height) {
        if (!rtvHeap_ || !dsvHeap_ || width == 0 || height == 0) return false;
        colorTarget_.Reset();
        depthTarget_.Reset();
        initialized = CreateTargets(width, height);
        return initialized;
    }

    void BindAndClear(const float* clearColor) {
        if (!initialized) return;
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        g_dx12.commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        g_dx12.commandList->ClearDepthStencilView(
            dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        g_dx12.commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    }

    void Bind() {
        if (!initialized) return;
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        g_dx12.commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
        g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
    }

    void ResolveToBackBuffer() {
        if (!initialized) return;
        ID3D12Resource* backBuffer =
            g_dx12.renderTargets[g_dx12.frameIndex].Get();

        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = colorTarget_.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        barriers[0].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = backBuffer;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        barriers[1].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_dx12.commandList->ResourceBarrier(2, barriers);

        g_dx12.commandList->ResolveSubresource(
            backBuffer, 0, colorTarget_.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);

        std::swap(barriers[0].Transition.StateBefore,
                  barriers[0].Transition.StateAfter);
        std::swap(barriers[1].Transition.StateBefore,
                  barriers[1].Transition.StateAfter);
        g_dx12.commandList->ResourceBarrier(2, barriers);
    }

    ID3D12Resource* GetDepthResource() const { return depthTarget_.Get(); }

private:
    bool CreateTargets(UINT width, UINT height) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC colorDesc = {};
        colorDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        colorDesc.Width = width;
        colorDesc.Height = height;
        colorDesc.DepthOrArraySize = 1;
        colorDesc.MipLevels = 1;
        colorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        colorDesc.SampleDesc.Count = SampleCount;
        colorDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE colorClear = {};
        colorClear.Format = colorDesc.Format;
        colorClear.Color[3] = 1.0f;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &colorDesc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &colorClear,
                IID_PPV_ARGS(&colorTarget_)))) {
            return false;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
        rtv.Format = colorDesc.Format;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        g_dx12.device->CreateRenderTargetView(
            colorTarget_.Get(), &rtv,
            rtvHeap_->GetCPUDescriptorHandleForHeapStart());

        D3D12_RESOURCE_DESC depthDesc = colorDesc;
        depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE depthClear = {};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                IID_PPV_ARGS(&depthTarget_)))) {
            colorTarget_.Reset();
            return false;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        g_dx12.device->CreateDepthStencilView(
            depthTarget_.Get(), &dsv,
            dsvHeap_->GetCPUDescriptorHandleForHeapStart());
        return true;
    }

    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    ComPtr<ID3D12Resource> colorTarget_;
    ComPtr<ID3D12Resource> depthTarget_;
};

#endif
