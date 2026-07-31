#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
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

struct StaticBufferDiagnosticDX12 {
    uint64_t serial = 0;
    uint64_t bytes = 0;
    D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
    std::string label = "None";
};

namespace StaticBufferDetailDX12 {
struct PendingUpload {
    ComPtr<ID3D12Resource> destination;
    ComPtr<ID3D12Resource> staging;
    D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
};

struct RetiredUploadBatch {
    uint64_t fenceValue = 0;
    std::vector<ComPtr<ID3D12Resource>> resources;
};

inline std::mutex mutex;
inline std::vector<PendingUpload> pending;
inline std::array<std::vector<ComPtr<ID3D12Resource>>, FRAME_COUNT> retired;
inline std::array<ComPtr<ID3D12CommandAllocator>, FRAME_COUNT> copyAllocators;
inline ComPtr<ID3D12GraphicsCommandList> copyCommandList;
inline std::array<uint64_t, FRAME_COUNT> copyAllocatorFenceValues = {};
inline std::vector<RetiredUploadBatch> copyRetired;
inline std::atomic<uint64_t> nextSerial{1};
inline StaticBufferStatsDX12 stats;
inline StaticBufferDiagnosticDX12 latest;
}

inline bool CreateStaticBufferDX12(
    ID3D12Device* device, const void* data, uint64_t size,
    D3D12_RESOURCE_STATES finalState, ComPtr<ID3D12Resource>& destination,
    const char* debugLabel = "StaticBuffer") {
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

    const uint64_t serial = StaticBufferDetailDX12::nextSerial.fetch_add(
        1, std::memory_order_relaxed);
    const std::string label = debugLabel ? debugLabel : "StaticBuffer";
#ifdef _DEBUG
    const std::wstring wideLabel(label.begin(), label.end());
    const std::wstring destinationName = L"DX12 DEFAULT " + wideLabel +
        L" #" + std::to_wstring(serial);
    const std::wstring stagingName = L"DX12 UPLOAD " + wideLabel +
        L" #" + std::to_wstring(serial);
    destination->SetName(destinationName.c_str());
    staging->SetName(stagingName.c_str());
#endif
    std::lock_guard<std::mutex> lock(StaticBufferDetailDX12::mutex);
    StaticBufferDetailDX12::pending.push_back(
        { destination, std::move(staging), finalState });
    StaticBufferDetailDX12::stats.bytes += size;
    ++StaticBufferDetailDX12::stats.resources;
    StaticBufferDetailDX12::stats.pendingUploads =
        static_cast<uint32_t>(StaticBufferDetailDX12::pending.size());
    if (serial >= StaticBufferDetailDX12::latest.serial)
        StaticBufferDetailDX12::latest = { serial, size, finalState, label };
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
    if (uploads.empty()) return 0;

    // Static buffers use the COPY queue. Direct queue waits entirely on-GPU,
    // then performs final state transitions before any draw consumes them.
    // Dedicated allocators avoid collision with occlusion readback copy work.
    if (!StaticBufferDetailDX12::copyCommandList) {
        for (UINT i = 0; i < FRAME_COUNT; ++i)
            ThrowIfFailed(g_dx12.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_COPY,
                IID_PPV_ARGS(&StaticBufferDetailDX12::copyAllocators[i])));
        ThrowIfFailed(g_dx12.device->CreateCommandList(0,
            D3D12_COMMAND_LIST_TYPE_COPY,
            StaticBufferDetailDX12::copyAllocators[0].Get(), nullptr,
            IID_PPV_ARGS(&StaticBufferDetailDX12::copyCommandList)));
        ThrowIfFailed(StaticBufferDetailDX12::copyCommandList->Close());
    }
    const uint64_t completedCopy = g_dx12.copyFence->GetCompletedValue();
    StaticBufferDetailDX12::copyRetired.erase(
        std::remove_if(StaticBufferDetailDX12::copyRetired.begin(),
            StaticBufferDetailDX12::copyRetired.end(),
            [completedCopy](const StaticBufferDetailDX12::RetiredUploadBatch& batch) {
                return batch.fenceValue <= completedCopy;
            }),
        StaticBufferDetailDX12::copyRetired.end());
    const UINT slot = g_dx12.frameIndex % FRAME_COUNT;
    WaitForFenceCPU(g_dx12.copyFence.Get(),
        StaticBufferDetailDX12::copyAllocatorFenceValues[slot]);
    ThrowIfFailed(StaticBufferDetailDX12::copyAllocators[slot]->Reset());
    ThrowIfFailed(StaticBufferDetailDX12::copyCommandList->Reset(
        StaticBufferDetailDX12::copyAllocators[slot].Get(), nullptr));

    StaticBufferDetailDX12::RetiredUploadBatch retirement;
    retirement.resources.reserve(uploads.size() * 2);
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.reserve(uploads.size());
    for (auto& upload : uploads) {
        StaticBufferDetailDX12::copyCommandList->CopyBufferRegion(
            upload.destination.Get(), 0,
            upload.staging.Get(), 0, upload.destination->GetDesc().Width);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = upload.destination.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = upload.finalState;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers.push_back(barrier);
        retirement.resources.push_back(std::move(upload.staging));
        // Copies to resources superseded by a later merge are still present in
        // this command list. Keep those destinations alive until fence retirement.
        retirement.resources.push_back(std::move(upload.destination));
    }
    ThrowIfFailed(StaticBufferDetailDX12::copyCommandList->Close());
    ID3D12CommandList* copyLists[] = {
        StaticBufferDetailDX12::copyCommandList.Get() };
    g_dx12.copyQueue->ExecuteCommandLists(1, copyLists);
    const uint64_t copyFenceValue = ++g_dx12.copyFenceValue;
    ThrowIfFailed(g_dx12.copyQueue->Signal(
        g_dx12.copyFence.Get(), copyFenceValue));
    StaticBufferDetailDX12::copyAllocatorFenceValues[slot] = copyFenceValue;
    retirement.fenceValue = copyFenceValue;
    StaticBufferDetailDX12::copyRetired.push_back(std::move(retirement));
    ThrowIfFailed(g_dx12.commandQueue->Wait(
        g_dx12.copyFence.Get(), copyFenceValue));
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

inline StaticBufferDiagnosticDX12 GetStaticBufferDiagnosticDX12() {
    std::lock_guard<std::mutex> lock(StaticBufferDetailDX12::mutex);
    return StaticBufferDetailDX12::latest;
}
