#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>
#include "DX12Core.h"

using Microsoft::WRL::ComPtr;

struct StaticBufferStatsDX12 {
    uint64_t bytes = 0;
    uint32_t resources = 0;
    uint32_t pendingUploads = 0;
};

namespace StaticBufferDetailDX12 {
struct PendingUpload {
    ComPtr<ID3D12Resource> destination;
    ComPtr<ID3D12Resource> staging;
    D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
};

inline std::mutex mutex;
inline std::vector<PendingUpload> pending;
inline std::array<std::vector<ComPtr<ID3D12Resource>>, FRAME_COUNT> retired;
inline StaticBufferStatsDX12 stats;
}

inline bool CreateStaticBufferDX12(
    ID3D12Device* device, const void* data, uint64_t size,
    D3D12_RESOURCE_STATES finalState, ComPtr<ID3D12Resource>& destination) {
    if (!device || !data || size == 0) return false;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&destination)))) return false;

    ComPtr<ID3D12Resource> staging;
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&staging)))) {
        destination.Reset();
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE noRead = { 0, 0 };
    if (FAILED(staging->Map(0, &noRead, &mapped))) {
        destination.Reset();
        return false;
    }
    std::memcpy(mapped, data, static_cast<size_t>(size));
    staging->Unmap(0, nullptr);

    std::lock_guard<std::mutex> lock(StaticBufferDetailDX12::mutex);
    StaticBufferDetailDX12::pending.push_back(
        { destination, std::move(staging), finalState });
    StaticBufferDetailDX12::stats.bytes += size;
    ++StaticBufferDetailDX12::stats.resources;
    StaticBufferDetailDX12::stats.pendingUploads =
        static_cast<uint32_t>(StaticBufferDetailDX12::pending.size());
    return true;
}

inline uint32_t FlushStaticBufferUploadsDX12(ID3D12GraphicsCommandList* commandList) {
    if (!commandList) return 0;
    std::vector<StaticBufferDetailDX12::PendingUpload> uploads;
    {
        std::lock_guard<std::mutex> lock(StaticBufferDetailDX12::mutex);
        uploads.swap(StaticBufferDetailDX12::pending);
        StaticBufferDetailDX12::stats.pendingUploads = 0;
    }
    auto& retired = StaticBufferDetailDX12::retired[g_dx12.frameIndex % FRAME_COUNT];
    retired.clear();
    if (uploads.empty()) return 0;
    retired.reserve(uploads.size());
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.reserve(uploads.size());
    for (auto& upload : uploads) {
        commandList->CopyBufferRegion(upload.destination.Get(), 0,
            upload.staging.Get(), 0, upload.destination->GetDesc().Width);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = upload.destination.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = upload.finalState;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers.push_back(barrier);
        retired.push_back(std::move(upload.staging));
        // Copies to resources superseded by a later merge are still present in
        // this command list. Keep those destinations alive until fence retirement.
        retired.push_back(std::move(upload.destination));
    }
    commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    return static_cast<uint32_t>(uploads.size());
}

inline StaticBufferStatsDX12 GetStaticBufferStatsDX12() {
    std::lock_guard<std::mutex> lock(StaticBufferDetailDX12::mutex);
    StaticBufferStatsDX12 result = StaticBufferDetailDX12::stats;
    result.pendingUploads =
        static_cast<uint32_t>(StaticBufferDetailDX12::pending.size());
    return result;
}
