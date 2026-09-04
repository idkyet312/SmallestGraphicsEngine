#ifndef SNIPER_SCOPE_DX12_H
#define SNIPER_SCOPE_DX12_H

#include "DX12Core.h"
#include <d3d12.h>
#include <wrl.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

using Microsoft::WRL::ComPtr;

// Owns the sniper scope view's persistent render targets and history state.
//
// The scope is a second camera rendered into a square off-screen target in the
// same frame as the main view, then sampled by the R700 lens material. The
// direct queue writes the target and samples it later in the same frame, so
// one texture is sufficient: queue ordering protects it without a CPU wait.
//
// Everything here is allocated once at initialization and reused. No D3D
// resource is created or resized per frame.
class SniperScopeDX12 {
public:
    bool Init(ID3D12Device* device, UINT resolution) {
        if (!device || resolution == 0) return false;

        resolution_ = resolution;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        // Linear HDR, not display-encoded LDR. The lens is sampled BEFORE the
        // main view tone-map, so an LDR scope image would be tone-mapped a
        // second time along with the frame that contains it -- crushing the
        // scope highlights and shifting its colour away from the same content
        // viewed outside the lens. Keeping the lens linear lets the single
        // main-view tone-map do the whole job.
        D3D12_RESOURCE_DESC colourDesc{};
        colourDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        colourDesc.Width = resolution;
        colourDesc.Height = resolution;
        colourDesc.DepthOrArraySize = 1;
        colourDesc.MipLevels = 1;
        colourDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        colourDesc.SampleDesc.Count = 1;
        colourDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        colourDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE colourClear{};
        colourClear.Format = colourDesc.Format;
        colourClear.Color[0] = 0.012f;
        colourClear.Color[1] = 0.020f;
        colourClear.Color[2] = 0.028f;
        colourClear.Color[3] = 1.0f;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &colourDesc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &colourClear,
                IID_PPV_ARGS(&colour_)))) return false;

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(
                &rtvHeapDesc, IID_PPV_ARGS(&rtvHeap_)))) return false;
        device->CreateRenderTargetView(
            colour_.Get(), nullptr, rtvHeap_->GetCPUDescriptorHandleForHeapStart());

        // The scope owns its own depth buffer. Sharing the main view depth
        // would mean a 15-degree frustum writing values the main camera passes
        // then read, and the two projections agree on no pixel.
        D3D12_RESOURCE_DESC depthDesc{};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = resolution;
        depthDesc.Height = resolution;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = depthDesc.Format;
        depthClear.DepthStencil.Depth = 1.0f;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                IID_PPV_ARGS(&depth_)))) return false;

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(
                &dsvHeapDesc, IID_PPV_ARGS(&dsvHeap_)))) return false;
        device->CreateDepthStencilView(
            depth_.Get(), nullptr, dsvHeap_->GetCPUDescriptorHandleForHeapStart());

        colour_->SetName(L"Sniper scope colour (linear HDR)");
        depth_->SetName(L"Sniper scope depth");

        viewport_ = { 0.0f, 0.0f, static_cast<float>(resolution_),
                      static_cast<float>(resolution_), 0.0f, 1.0f };
        scissor_ = { 0, 0, static_cast<LONG>(resolution_),
                     static_cast<LONG>(resolution_) };
        initialized_ = true;
        return true;
    }

    // Rendered every active frame. The previous 30 Hz throttle made the lens
    // image lag the rifle during the fast micro-corrections aiming is made of,
    // which reads as the scope sticking to the world rather than to the gun.
    bool ShouldRender(bool active) {
        if (!initialized_ || !active) {
            wasActive_ = false;
            return false;
        }
        // Becoming active after being inactive means any temporal history the
        // scope kept describes a different aim direction entirely. The caller
        // uses this to invalidate it rather than reproject stale data.
        historyInvalidated_ = !wasActive_;
        wasActive_ = true;
        return true;
    }

    // True on the first frame of a new ADS entry, for one query.
    bool ConsumeHistoryInvalidated() {
        const bool value = historyInvalidated_;
        historyInvalidated_ = false;
        return value;
    }

    void Begin(ID3D12GraphicsCommandList* list) {
        if (!initialized_ || !list) return;
        Transition(list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        const float clear[] = { 0.012f, 0.020f, 0.028f, 1.0f };
        list->RSSetViewports(1, &viewport_);
        list->RSSetScissorRects(1, &scissor_);
        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->ClearDepthStencilView(
            dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }

    void End(ID3D12GraphicsCommandList* list) {
        if (!initialized_ || !list) return;
        Transition(list, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        hasRenderedOnce_ = true;
    }

    // Transitions the lens image to a shader-readable state without assuming
    // this class last wrote it -- the VB path copies its post output straight
    // into the target instead of rendering through Begin/End.
    void TransitionToShaderResource(ID3D12GraphicsCommandList* list,
                                    D3D12_RESOURCE_STATES from) {
        if (!initialized_ || !list) return;
        Transition(list, from, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        hasRenderedOnce_ = true;
    }

    bool Initialized() const { return initialized_; }
    // The lens material must not be bound to a target whose contents are still
    // undefined: a newly created default-heap texture holds garbage until its
    // first clear, and the rifle is visible at the hip long before that.
    bool HasRenderedOnce() const { return hasRenderedOnce_; }
    UINT Resolution() const { return resolution_; }
    ID3D12Resource* Texture() const { return colour_.Get(); }
    ID3D12Resource* Depth() const { return depth_.Get(); }
    const D3D12_VIEWPORT& Viewport() const { return viewport_; }
    const D3D12_RECT& Scissor() const { return scissor_; }
    D3D12_CPU_DESCRIPTOR_HANDLE RTV() const {
        return rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE DSV() const {
        return dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    }

private:
    void Transition(ID3D12GraphicsCommandList* list,
                    D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = colour_.Get();
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }

    ComPtr<ID3D12Resource> colour_;
    ComPtr<ID3D12Resource> depth_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissor_{};
    UINT resolution_ = 0;
    bool initialized_ = false;
    bool wasActive_ = false;
    bool hasRenderedOnce_ = false;
    bool historyInvalidated_ = false;
};

#endif
