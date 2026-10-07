#pragma once

#include "VirtualShadowPages.h"
#include "DX12Core.h"
#include "ProfilerDX12.h"
#include "ShaderCacheDX12.h"
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>

// A standalone virtual shadow map, independent of the cascade shadow maps.
//
// The atlas is its own resource, not a copy of the cascades with pages bolted
// on: while this system is active ShadowMapDX12 swaps it into the shadow SRV
// slot wholesale and skips the cascade render entirely, so nothing downstream
// can fall back to a cascade sample.
//
// The page grid is anchored to the world, not the camera. All pages share one
// light rotation and a page's identity is its absolute lattice coordinate, so
// pages stay put as the camera moves and turns; the only thing that invalidates
// them is a change to the light basis itself. See VirtualShadowPages.h.
//
// Outputs are frame-local because the resolve can run on async compute; only
// the graphics queue touches the static cache.
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

    // World-space depth span the light basis covers, centred on the plane
    // through the world origin. Fixed rather than fitted to the view frustum: a
    // fitted range would change every frame and invalidate the whole clipmap,
    // which is the cost this system exists to avoid. It also lets one depth
    // value serve every level, since all pages share this range.
    static constexpr float DepthExtent = 4000.0f;

    bool Ensure() {
        if (cache) return true;
        if (attempted) return false;
        attempted = true;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc.Height = VirtualShadows::AtlasSize;
        desc.DepthOrArraySize = 1;
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
        if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
            D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clear, IID_PPV_ARGS(&cache)))) return AllocationFailed();
        cache->SetName(L"VSM static page cache");
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_D32_FLOAT;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.ArraySize = 1;
        for (UINT i = 0; i <= FRAME_COUNT; ++i)
            g_dx12.device->CreateDepthStencilView(
                i == FRAME_COUNT ? cache.Get() : output[i].Get(), &view, Dsv(i));
        return true;
    }

    // Renders the resident set. draw(matrix, live) records the shadow scene for
    // one page; live selects the dynamic half, matching the static/live split
    // the spot atlas uses.
    template<class Draw>
    void Render(const Scene& scene, Draw draw) {
        using namespace VirtualShadows;
        constants = {};
        active = false;
        resident = refreshed = reused = 0;
        slotStates.fill(SlotState::Unused);
        slotKeys.fill(VirtualShadows::Invalid);
        if (!Ensure()) return;
        ProfilerDX12::Scope timer(g_profiler, "Virtual Shadow Maps", g_dx12.commandList.Get());

        // The light basis. Built from the light direction ONLY -- never from the
        // camera -- which is what keeps the lattice world-anchored: turning or
        // moving the camera changes nothing here.
        XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&scene.lightPos));
        XMVECTOR lightUp = XMVectorSet(0, 1, 0, 0);
        if (std::fabs(XMVectorGetX(XMVector3Dot(lightDir, lightUp))) > 0.95f)
            lightUp = XMVectorSet(0, 0, 1, 0);
        const XMMATRIX rotation = XMMatrixLookAtLH(
            XMVectorZero(), -lightDir, lightUp);

        // A rotation change is the only event that invalidates pages: it
        // re-rasterizes every one of them through a different projection.
        XMFLOAT4X4 rotationStore;
        XMStoreFloat4x4(&rotationStore, rotation);
        if (std::memcmp(&rotationStore, &previousRotation, sizeof(rotationStore)) != 0) {
            pages.Invalidate();
            previousRotation = rotationStore;
        }

        // Where the viewer sits on the lattice. This selects which pages to
        // request; it never affects what a page is.
        XMFLOAT3 viewer;
        XMStoreFloat3(&viewer, XMVector3TransformCoord(
            XMLoadFloat3(&scene.camera.Position), rotation));

        std::array<uint32_t, Capacity> requests;
        const UINT budget = (std::clamp)(
            scene.virtualShadowPageBudget, 1, (int)Capacity);
        const UINT written = BuildRequests(viewer.x, viewer.y, budget, requests);
        pages.Request(requests, written);

        // Per-page projection: the shared light rotation, then an orthographic
        // box over exactly this page's lattice square. Because the lattice is
        // absolute, this depends only on the key -- never on the viewer.
        auto matrixFor = [&](UINT slot) {
            const uint32_t key = pages.keys[slot];
            const float extent = PageExtent(KeyLevel(key));
            const float left = KeyX(key) * extent;
            const float bottom = KeyY(key) * extent;
            return rotation * XMMatrixOrthographicOffCenterLH(
                left, left + extent, bottom, bottom + extent,
                -DepthExtent * 0.5f, DepthExtent * 0.5f);
        };

        const auto cacheDsv = Dsv(FRAME_COUNT);
        std::optional<ProfilerDX12::Scope> phase;
        phase.emplace(g_profiler, "VSM/Static Pages", g_dx12.commandList.Get());
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

        phase.reset();
        phase.emplace(g_profiler, "VSM/Atlas Copy", g_dx12.commandList.Get());
        auto* target = output[g_dx12.frameIndex].Get();
        if (EnsurePageCopy() && !WholeAtlasCopy()) {
            // Only the resident pages: the page table maps nothing else, and
            // the default budget keeps 3 of the 16 slots resident. Island
            // Atlas Copy measured 0.50 ms for the whole 4096^2 D32 copy.
            Transition(target, ReadState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Transition(cache.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ID3D12DescriptorHeap* heaps[] = { copyHeap.Get() };
            g_dx12.commandList->SetDescriptorHeaps(1, heaps);
            g_dx12.commandList->SetGraphicsRootSignature(copyRootSig.Get());
            g_dx12.commandList->SetPipelineState(copyPSO.Get());
            g_dx12.commandList->SetGraphicsRootDescriptorTable(
                0, copyHeap->GetGPUDescriptorHandleForHeapStart());
            g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (UINT slot = 0; slot < Capacity; ++slot) {
                if (!pages.requested[slot]) continue;
                Bind(slot, Dsv(g_dx12.frameIndex));
                g_dx12.commandList->DrawInstanced(3, 1, 0, 0);
            }
            Transition(cache.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
        } else {
            Transition(target, ReadState, D3D12_RESOURCE_STATE_COPY_DEST);
            Transition(cache.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            // Depth/stencil copies must cover a complete subresource (D3D12).
            Copy(target, 0, cache.Get(), 0);
            Transition(cache.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
        phase.reset();
        phase.emplace(g_profiler, "VSM/Live Casters", g_dx12.commandList.Get());
        bool complete = true;
        for (UINT slot = 0; slot < Capacity; ++slot) {
            if (!pages.requested[slot]) continue;
            Bind(slot, Dsv(g_dx12.frameIndex));
            if (!draw(matrixFor(slot), true)) { complete = false; break; }
            constants.Map(pages.keys[slot], slot);
        }
        Transition(target, D3D12_RESOURCE_STATE_DEPTH_WRITE, ReadState);
        phase.reset();
        if (!complete) { constants = {}; pages.Invalidate(); return; }

        constants.config = {1, resident, TableSize, PageSize};
        for (UINT level = 0; level < Levels; ++level)
            constants.levelScale[level] = 1.0f / PageExtent(level);
        // The matrix the shaders sample with: the light rotation with the depth
        // range folded in, so its z is directly comparable against what the page
        // projections above wrote. All pages share it, which is what lets one
        // sample serve whichever level turns out to be resident.
        XMFLOAT4X4 sampleTransform;
        // HLSL uses column-major storage, matching the other matrix uploads.
        XMStoreFloat4x4(&sampleTransform, XMMatrixTranspose(rotation *
            XMMatrixOrthographicOffCenterLH(-1, 1, -1, 1,
                -DepthExtent * 0.5f, DepthExtent * 0.5f)));
        for (UINT row = 0; row < 4; ++row)
            for (UINT column = 0; column < 4; ++column)
                constants.lightRotation[row][column] = sampleTransform.m[row][column];
        active = true;
    }

    // The light basis, for consumers that need to place a page in the world
    // (the debug overlay). Same construction as Render.
    static XMMATRIX LightRotation(const Scene& scene) {
        XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&scene.lightPos));
        XMVECTOR lightUp = XMVectorSet(0, 1, 0, 0);
        if (std::fabs(XMVectorGetX(XMVector3Dot(lightDir, lightUp))) > 0.95f)
            lightUp = XMVectorSet(0, 0, 1, 0);
        return XMMatrixLookAtLH(XMVectorZero(), -lightDir, lightUp);
    }

private:
    static constexpr auto ReadState = static_cast<D3D12_RESOURCE_STATES>(
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    std::array<ComPtr<ID3D12Resource>, FRAME_COUNT> output;
    ComPtr<ID3D12Resource> cache;
    ComPtr<ID3D12DescriptorHeap> dsvs;
    ComPtr<ID3D12DescriptorHeap> copyHeap;
    ComPtr<ID3D12RootSignature> copyRootSig;
    ComPtr<ID3D12PipelineState> copyPSO;
    XMFLOAT4X4 previousRotation{};
    bool attempted = false, active = false, copyTried = false;

    // SGE_VSM_WHOLE_ATLAS_COPY restores the full-subresource copy for A/B.
    static bool WholeAtlasCopy() {
        static const bool whole =
            GetEnvironmentVariableA("SGE_VSM_WHOLE_ATLAS_COPY", nullptr, 0) > 0;
        return whole;
    }

    // Depth-only draw that copies one page; see vsm_page_copy.hlsl. Falls back
    // to the whole-atlas copy if anything here fails.
    bool EnsurePageCopy() {
        if (copyPSO) return true;
        if (copyTried) return false;
        copyTried = true;
        std::ifstream file("shaders/vsm_page_copy.hlsl");
        if (!file.is_open()) return false;
        std::stringstream source;
        source << file.rdbuf();
        const std::string code = source.str();
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
        if (FAILED(ShaderCacheDX12::CompileCached(code.c_str(), code.length(),
                "shaders/vsm_page_copy.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                "VSMain", "vs_5_0", flags, 0, &vsBlob, &errorBlob)) ||
            FAILED(ShaderCacheDX12::CompileCached(code.c_str(), code.length(),
                "shaders/vsm_page_copy.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                "PSMain", "ps_5_0", flags, 0, &psBlob, &errorBlob))) {
            if (errorBlob)
                std::cerr << "VSM page copy shader error: "
                          << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 1;
        rootDesc.pParameters = &param;
        ComPtr<ID3DBlob> sigBlob;
        errorBlob.Reset();
        if (FAILED(D3D12SerializeRootSignature(&rootDesc,
                D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob)) ||
            FAILED(g_dx12.device->CreateRootSignature(0,
                sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                IID_PPV_ARGS(&copyRootSig))))
            return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = copyRootSig.Get();
        pso.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        pso.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 0;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pso.SampleDesc.Count = 1;
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&copyPSO))) ||
            FAILED(g_dx12.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&copyHeap)))) {
            copyPSO.Reset();
            copyHeap.Reset();
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(
            cache.Get(), &srv, copyHeap->GetCPUDescriptorHandleForHeapStart());
        return true;
    }
    bool AllocationFailed() {
        failed = true;
        cache.Reset(); dsvs.Reset();
        for (auto& target : output) target.Reset();
        std::cerr << "VSM allocation failed; virtual shadows disabled\n";
        return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT index) const {
        auto handle = dsvs->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += index * g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        return handle;
    }
    static D3D12_RECT Rect(UINT slot) {
        const LONG size = (LONG)VirtualShadows::PageSize;
        const LONG x = (slot % VirtualShadows::AtlasPages) * size;
        const LONG y = (slot / VirtualShadows::AtlasPages) * size;
        return {x, y, x + size, y + size};
    }
    static void Bind(UINT slot, D3D12_CPU_DESCRIPTOR_HANDLE dsv) {
        const auto rect = Rect(slot);
        const D3D12_VIEWPORT viewport = {(float)rect.left, (float)rect.top,
            (float)VirtualShadows::PageSize, (float)VirtualShadows::PageSize, 0, 1};
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
