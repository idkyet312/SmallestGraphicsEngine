#pragma once

#include "VirtualShadowPages.h"
#include "DX12Core.h"
#include "ProfilerDX12.h"
#include <cstring>

// The atlas shares a view with three fallback slices, keeping all existing
// shadow consumers on one SRV. Outputs are frame-local because the resolve can
// run on async compute; only the graphics queue touches the static cache.
class VirtualShadowMapDX12 {
public:
    VirtualShadows::PageCache pages;
    UINT resident = 0, refreshed = 0, reused = 0;
    bool failed = false;

    // Per-slot outcome for this frame, for the page debug overlay. Captured as
    // the refresh loop runs rather than read back from PageCache afterwards:
    // that loop sets valid[] true as it redraws, so by the end of the frame a
    // refreshed page is indistinguishable from one that was reused.
    enum class SlotState : uint8_t { Unused, Reused, Refreshed };
    std::array<SlotState, VirtualShadows::Capacity> slotStates{};
    std::array<uint32_t, VirtualShadows::Capacity> slotKeys{};
    ID3D12Resource* Output() const { return active ? output[g_dx12.frameIndex].Get() : nullptr; }
    void Disable() {
        active = false;
        constants = {};
        pages.Invalidate();
        slotStates.fill(SlotState::Unused);
        slotKeys.fill(VirtualShadows::Invalid);
    }
    VirtualShadows::Constants constants{};

    bool Ensure() {
        if (cache) return true;
        if (attempted) return false;
        attempted = true;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc.Height = VirtualShadows::AtlasSize;
        desc.DepthOrArraySize = 4;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1;
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.NumDescriptors = FRAME_COUNT + 1;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvs))))
            return AllocationFailed();
        for (auto& target : output)
            if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
                D3D12_HEAP_FLAG_NONE, &desc, ReadState, &clear, IID_PPV_ARGS(&target))))
                return AllocationFailed();
        desc.DepthOrArraySize = 1;
        if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
            D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clear, IID_PPV_ARGS(&cache)))) return AllocationFailed();
        cache->SetName(L"VSM static page cache");
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_D32_FLOAT;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.ArraySize = 1;
        for (UINT i = 0; i <= FRAME_COUNT; ++i) {
            view.Texture2DArray.FirstArraySlice = i == FRAME_COUNT ? 0 : 3;
            g_dx12.device->CreateDepthStencilView(i == FRAME_COUNT ? cache.Get() : output[i].Get(),
                &view, Dsv(i));
        }
        return true;
    }

    template<class Draw>
    void Render(const Scene& scene, ID3D12Resource* fallback,
                const std::array<XMMATRIX, 3>& matrices, Draw draw) {
        using namespace VirtualShadows;
        constants = {};
        active = false;
        resident = refreshed = reused = 0;
        slotStates.fill(SlotState::Unused);
        slotKeys.fill(VirtualShadows::Invalid);
        if (!Ensure() || fallback->GetDesc().Width != AtlasSize) return;
        ProfilerDX12::Scope timer(g_profiler, "Virtual Shadow Maps", g_dx12.commandList.Get());
        for (UINT c = 0; c < 3; ++c) {
            XMFLOAT4X4 matrix;
            XMStoreFloat4x4(&matrix, matrices[c]);
            if (std::memcmp(&matrix, &previous[c], sizeof(matrix)) != 0) pages.InvalidateCascade(c);
            previous[c] = matrix;
        }
        std::array<uint32_t, Capacity> requests;
        requests.fill(Invalid);
        const UINT budget = (std::clamp)(scene.virtualShadowPageBudget, 1, (int)Capacity);
        const XMVECTOR position = XMLoadFloat3(&scene.camera.Position);
        const XMVECTOR forward = XMVector3Normalize(XMLoadFloat3(&scene.camera.Front));
        // Each cascade gets its own slice of the budget and its own spiral, so
        // the three do not all re-request the cell under the crosshair. The
        // remainder goes to the nearest cascade, where texels are smallest and
        // an extra page buys the most detail.
        //
        // A generated spiral replaces the old six-entry offset table: with a
        // budget of 16 that table could only name six cells per cascade, so the
        // pages piled into one clump instead of tiling outward from the focus.
        UINT written = 0;
        for (UINT c = 0; c < 3 && written < budget; ++c) {
            UINT share = budget / 3 + (c < budget % 3 ? 1 : 0);
            if (share == 0) continue;

            const float nearZ = c ? (&g_shadowCascadeSplits.x)[c - 1] : scene.cameraNear;
            const float farZ = (&g_shadowCascadeSplits.x)[c];
            XMFLOAT3 projected;
            XMStoreFloat3(&projected, XMVector3TransformCoord(
                position + forward * (nearZ + (farZ - nearZ) * 0.4f), matrices[c]));
            const int focusX = (int)std::floor((projected.x * 0.5f + 0.5f) * Grid);
            const int focusY = (int)std::floor((0.5f - projected.y * 0.5f) * Grid);

            // Square spiral out from the focus cell: right, down, left, up, with
            // the run length growing every second turn. Cells off the grid are
            // skipped without consuming the cascade's share, so a focus near an
            // edge still fills its quota from the cells that do exist. The step
            // cap bounds the walk when most of the neighbourhood is off-grid.
            int x = focusX, y = focusY, dx = 1, dy = 0, run = 1, stepsInRun = 0, turns = 0;
            const int maxSteps = (int)(Grid * Grid);
            for (int step = 0; step < maxSteps && share > 0 && written < budget; ++step) {
                if (x >= 0 && y >= 0 && x < (int)Grid && y < (int)Grid) {
                    requests[written++] = Key(c, (uint32_t)x, (uint32_t)y);
                    --share;
                }
                x += dx;
                y += dy;
                if (++stepsInRun == run) {          // corner: turn right
                    stepsInRun = 0;
                    const int swap = dx;
                    dx = -dy;
                    dy = swap;
                    if (++turns % 2 == 0) ++run;    // every second turn lengthens
                }
            }
        }
        pages.Request(requests, written);
        auto matrixFor = [&](UINT slot) {
            const UINT key = pages.keys[slot], c = key / 256;
            const UINT x = key % 16, y = (key / 16) % 16;
            return matrices[c] * XMMatrixScaling(16, 16, 1) *
                XMMatrixTranslation(15.0f - 2.0f * x, 2.0f * y - 15.0f, 0);
        };
        const auto cacheDsv = Dsv(FRAME_COUNT);
        for (UINT slot = 0; slot < Capacity; ++slot) {
            if (!pages.requested[slot]) continue;
            ++resident;
            slotKeys[slot] = pages.keys[slot];
            if (pages.valid[slot]) {
                ++reused;
                slotStates[slot] = SlotState::Reused;
                continue;
            }
            slotStates[slot] = SlotState::Refreshed;
            const D3D12_RECT rect = Rect(slot);
            g_dx12.commandList->ClearDepthStencilView(cacheDsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 1, &rect);
            Bind(slot, cacheDsv);
            if (!draw(matrixFor(slot), false)) { pages.Invalidate(); return; }
            pages.valid[slot] = true;
            ++refreshed;
        }
        auto* target = output[g_dx12.frameIndex].Get();
        Transition(target, ReadState, D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(fallback, ReadState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (UINT c = 0; c < 3; ++c) Copy(target, c, fallback, c);
        Transition(fallback, D3D12_RESOURCE_STATE_COPY_SOURCE, ReadState);
        Transition(cache.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        // Depth/stencil copies must cover a complete subresource (D3D12).
        Copy(target, 3, cache.Get(), 0);
        Transition(cache.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        bool complete = true;
        for (UINT slot = 0; slot < Capacity; ++slot) {
            if (!pages.requested[slot]) continue;
            Bind(slot, Dsv(g_dx12.frameIndex));
            if (!draw(matrixFor(slot), true)) { complete = false; break; }
            constants.Map(pages.keys[slot], slot);
        }
        Transition(target, D3D12_RESOURCE_STATE_DEPTH_WRITE, ReadState);
        if (!complete) { constants = {}; pages.Invalidate(); return; }
        constants.config = {1, resident, Grid, PageSize};
        active = true;
    }

private:
    static constexpr auto ReadState = static_cast<D3D12_RESOURCE_STATES>(
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    std::array<ComPtr<ID3D12Resource>, FRAME_COUNT> output;
    ComPtr<ID3D12Resource> cache;
    ComPtr<ID3D12DescriptorHeap> dsvs;
    std::array<XMFLOAT4X4, 3> previous{};
    bool attempted = false, active = false;
    bool AllocationFailed() {
        failed = true;
        cache.Reset(); dsvs.Reset();
        for (auto& target : output) target.Reset();
        std::cerr << "VSM allocation failed; using cascade shadows\n";
        return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT index) const {
        auto handle = dsvs->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += index * g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        return handle;
    }
    static D3D12_RECT Rect(UINT slot) {
        const LONG x = (slot % 4) * 1024, y = (slot / 4) * 1024;
        return {x, y, x + 1024, y + 1024};
    }
    static void Bind(UINT slot, D3D12_CPU_DESCRIPTOR_HANDLE dsv) {
        const auto rect = Rect(slot);
        const D3D12_VIEWPORT viewport = {(float)rect.left, (float)rect.top, 1024, 1024, 0, 1};
        g_dx12.commandList->RSSetViewports(1, &viewport);
        g_dx12.commandList->RSSetScissorRects(1, &rect);
        g_dx12.commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    }
    static void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        g_dx12.commandList->ResourceBarrier(1, &b);
    }
    static void Copy(ID3D12Resource* dst, UINT dstSlice, ID3D12Resource* src, UINT srcSlice) {
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = dst; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex = dstSlice;
        from.pResource = src; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = srcSlice;
        g_dx12.commandList->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
};
