#pragma once

#include "DX12Core.h"
#include "ProfilerDX12.h"
#include "ResolveEntryPipelineDX12.h"
#include <string>

extern ProfilerDX12 g_profiler;

// World-space radiance cache for the Lumen GI bounce. The table itself lives
// in shaders/gi_radiance_cache.hlsli; this owns its buffer and the refresh
// pass. Private to the visibility renderer, which binds the buffer as a root
// UAV (u16) on the enhanced and cascade resolve root signatures.
class GIRadianceCacheDX12 {
public:
    // Must match gi_radiance_cache.hlsli.
    static constexpr UINT Capacity = 1u << 19;
    static constexpr UINT EntryBytes = 3u * 16u;
    static constexpr UINT RefreshPerFrame = Capacity / 16u;

    D3D12_GPU_VIRTUAL_ADDRESS Address() const {
        return buffer ? buffer->GetGPUVirtualAddress() : 0;
    }
    const char* Status() const {
        return bufferFailed ? "Radiance cache allocation failed"
                            : refresh.Status();
    }

    // Allocated the first time the cache is switched on (24 MB). Committed
    // default-heap memory starts zeroed, which is the empty-cell state.
    bool EnsureBuffer() {
        if (buffer) return true;
        if (bufferFailed) return false;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = static_cast<UINT64>(Capacity) * EntryBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
                D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&buffer)))) {
            buffer.Reset();
            bufferFailed = true;
            return false;
        }
        buffer->SetName(L"Lumen radiance cache");
        return true;
    }

    bool EnsurePipeline(const std::string& source, bool bindless,
                        ID3D12RootSignature* root) {
        return refresh.Ensure(source, bindless, root);
    }

    // Requests a full clear on the next refresh (level change, toggle). The
    // caller writes giRadianceCache = 2 into the enhanced constants for the
    // frame whose refresh performs it.
    void RequestClear() { clearPending = true; }
    bool ClearThisFrame() const { return clearPending; }

    // Records the refresh with the resolve's root signature, table and root
    // UAV already bound; the caller restores its own PSO afterwards.
    void Refresh(ID3D12GraphicsCommandList* cmd, bool bindless) {
        ID3D12PipelineState* pso = refresh.PSO(bindless);
        if (!pso || !buffer) return;
        ProfilerDX12::Scope profile(g_profiler, "Lumen Radiance Cache", cmd);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = buffer.Get();
        // Earlier resolves this frame (the scope view) wrote the table.
        cmd->ResourceBarrier(1, &barrier);
        cmd->SetPipelineState(pso);
        const UINT threads = clearPending ? Capacity : RefreshPerFrame;
        cmd->Dispatch((threads + 63u) / 64u, 1, 1);
        cmd->ResourceBarrier(1, &barrier);
        clearPending = false;
    }

private:
    ResolveEntryPipelineDX12 refresh{ L"GICacheRefreshMain",
                                      "Radiance cache refresh" };
    ComPtr<ID3D12Resource> buffer;
    bool bufferFailed = false;
    bool clearPending = false;
};
