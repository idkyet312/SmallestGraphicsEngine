#ifndef PROFILER_DX12_H
#define PROFILER_DX12_H

#include "DX12Core.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

struct ProfilerSampleDX12 {
    std::string name;
    double milliseconds = 0.0;
};

class ProfilerDX12 {
public:
    using Clock = std::chrono::steady_clock;
    // Overflow is silent -- BeginGpuEvent returns InvalidQuery and the scope
    // vanishes from the report rather than erroring -- so keep headroom above
    // the number of live scopes.
    // Raised from 96 for the sniper scope: a second full camera pass records
    // its own Scope/* sibling for every pass the main view times, and an
    // editor-heavy frame was already at 58 live scopes. Overflow is silent
    // (BeginGpuEvent returns InvalidQuery and the scope vanishes from the
    // report), so the headroom has to cover both views plus the editor.
    static constexpr UINT MaxGpuEvents = 160;
    static constexpr UINT QueriesPerFrame = 2 + MaxGpuEvents * 2;

    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue) {
        Shutdown();
        if (!device || !queue) return false;

        D3D12_QUERY_HEAP_DESC queryDesc = {};
        queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryDesc.Count = QueriesPerFrame * FRAME_COUNT;
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queryHeap))))
            return false;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = UINT64(queryDesc.Count) * sizeof(UINT64);
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback)))) {
            queryHeap.Reset();
            return false;
        }

        if (FAILED(queue->GetTimestampFrequency(&timestampFrequency)) || timestampFrequency == 0) {
            Shutdown();
            return false;
        }
        initialized = true;
        return true;
    }

    void Shutdown() {
        initialized = false;
        queryHeap.Reset();
        readback.Reset();
        timestampFrequency = 0;
        cpuSamples.clear();
        currentCpuSamples.clear();
        gpuSamples.clear();
        for (auto& slot : slots) slot = {};
    }

    void BeginCpuFrame() {
        cpuFrameStart = Clock::now();
        currentCpuSamples.clear();
        cpuFrameActive = true;
    }

    void EndCpuFrame() {
        if (!cpuFrameActive) return;
        cpuFrameMs = Milliseconds(cpuFrameStart, Clock::now());
        cpuSamples = currentCpuSamples;
        cpuFrameActive = false;
    }

    void BeginGpuFrame(UINT frameIndex, ID3D12GraphicsCommandList* commandList) {
        if (!initialized || !commandList) return;
        currentSlot = frameIndex % FRAME_COUNT;
        ReadCompletedSlot(currentSlot);

        Slot& slot = slots[currentSlot];
        slot.events.clear();
        slot.usedQueries = 1;
        slot.frameBegin = SlotBase(currentSlot);
        commandList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot.frameBegin);
        gpuFrameActive = true;
    }

    UINT BeginGpuEvent(const char* name, ID3D12GraphicsCommandList* commandList) {
        if (!gpuFrameActive || !commandList) return InvalidQuery;
        Slot& slot = slots[currentSlot];
        if (slot.events.size() >= MaxGpuEvents || slot.usedQueries + 2 > QueriesPerFrame)
            return InvalidQuery;

        const UINT begin = SlotBase(currentSlot) + slot.usedQueries++;
        commandList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, begin);
        slot.events.push_back({ name ? name : "GPU", begin, InvalidQuery });
        return static_cast<UINT>(slot.events.size() - 1);
    }

    void EndGpuEvent(UINT eventIndex, ID3D12GraphicsCommandList* commandList) {
        if (!gpuFrameActive || !commandList || eventIndex == InvalidQuery) return;
        Slot& slot = slots[currentSlot];
        if (eventIndex >= slot.events.size() || slot.usedQueries >= QueriesPerFrame) return;
        const UINT end = SlotBase(currentSlot) + slot.usedQueries++;
        commandList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, end);
        slot.events[eventIndex].end = end;
    }

    void EndGpuFrame(ID3D12GraphicsCommandList* commandList) {
        if (!gpuFrameActive || !commandList) return;
        Slot& slot = slots[currentSlot];
        if (slot.usedQueries < QueriesPerFrame) {
            slot.frameEnd = SlotBase(currentSlot) + slot.usedQueries++;
            commandList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot.frameEnd);
        }
        const UINT base = SlotBase(currentSlot);
        commandList->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            base, slot.usedQueries, readback.Get(), UINT64(base) * sizeof(UINT64));
        slot.pending = true;
        gpuFrameActive = false;
    }

    void AddCpuSample(const char* name, Clock::time_point begin, Clock::time_point end) {
        if (cpuFrameActive)
            currentCpuSamples.push_back({ name ? name : "CPU", Milliseconds(begin, end) });
    }

    const std::vector<ProfilerSampleDX12>& CpuSamples() const { return cpuSamples; }
    const std::vector<ProfilerSampleDX12>& GpuSamples() const { return gpuSamples; }
    // GPU milliseconds for one named scope, or 0 when it did not run this
    // frame. Linear scan: the sample list is short and this is UI-rate.
    double GpuScopeMs(const char* name) const {
        if (!name) return 0.0;
        for (const ProfilerSampleDX12& sample : gpuSamples)
            if (sample.name == name) return sample.milliseconds;
        return 0.0;
    }
    double CpuFrameMs() const { return cpuFrameMs; }
    double GpuFrameMs() const { return gpuFrameMs; }
    double GpuFrameP95Ms() const {
        if (gpuFrameHistory.empty()) return 0.0;
        std::vector<double> sorted(gpuFrameHistory.begin(), gpuFrameHistory.end());
        std::sort(sorted.begin(), sorted.end());
        const size_t index = static_cast<size_t>((sorted.size() - 1) * 0.95);
        return sorted[index];
    }
    size_t GpuHistorySize() const { return gpuFrameHistory.size(); }
    bool IsInitialized() const { return initialized; }

    class CpuScope {
    public:
        CpuScope(ProfilerDX12& owner, const char* name)
            : profiler(owner), label(name), begin(Clock::now()) {}
        ~CpuScope() { profiler.AddCpuSample(label, begin, Clock::now()); }
    private:
        ProfilerDX12& profiler;
        const char* label;
        Clock::time_point begin;
    };

    class Scope {
    public:
        Scope(ProfilerDX12& owner, const char* name, ID3D12GraphicsCommandList* list)
            : profiler(owner), commandList(list), label(name), begin(Clock::now()),
              eventIndex(owner.BeginGpuEvent(name, list)) {
            // Metadata must be PIX_EVENT_ANSI_VERSION (1) for a char* label.
            // Passing 0 declares the string UTF-16, so RenderDoc decoded each
            // pair of ASCII bytes as one wide char and every marker showed up as
            // CJK mojibake ("Visibility Buffer" -> a run of Chinese glyphs).
            if (commandList && label)
                commandList->BeginEvent(1, label, static_cast<UINT>(std::strlen(label) + 1));
        }
        ~Scope() {
            if (commandList && label) commandList->EndEvent();
            profiler.EndGpuEvent(eventIndex, commandList);
            profiler.AddCpuSample(label, begin, Clock::now());
        }
    private:
        ProfilerDX12& profiler;
        ID3D12GraphicsCommandList* commandList;
        const char* label;
        Clock::time_point begin;
        UINT eventIndex;
    };

private:
    static constexpr UINT InvalidQuery = UINT_MAX;

    struct GpuEvent {
        std::string name;
        UINT begin = InvalidQuery;
        UINT end = InvalidQuery;
    };
    struct Slot {
        std::vector<GpuEvent> events;
        UINT usedQueries = 0;
        UINT frameBegin = InvalidQuery;
        UINT frameEnd = InvalidQuery;
        bool pending = false;
    };

    static double Milliseconds(Clock::time_point begin, Clock::time_point end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    }
    static UINT SlotBase(UINT slot) { return slot * QueriesPerFrame; }

    void ReadCompletedSlot(UINT slotIndex) {
        Slot& slot = slots[slotIndex];
        if (!slot.pending || !readback) return;

        const UINT base = SlotBase(slotIndex);
        const SIZE_T beginByte = SIZE_T(base) * sizeof(UINT64);
        const SIZE_T endByte = SIZE_T(base + slot.usedQueries) * sizeof(UINT64);
        D3D12_RANGE readRange = { beginByte, endByte };
        UINT64* timestamps = nullptr;
        if (SUCCEEDED(readback->Map(0, &readRange, reinterpret_cast<void**>(&timestamps)))) {
            gpuSamples.clear();
            if (slot.frameBegin != InvalidQuery && slot.frameEnd != InvalidQuery) {
                gpuFrameMs = double(timestamps[slot.frameEnd] - timestamps[slot.frameBegin])
                           * 1000.0 / double(timestampFrequency);
                gpuFrameHistory.push_back(gpuFrameMs);
                if (gpuFrameHistory.size() > 300) gpuFrameHistory.pop_front();
            }
            for (const GpuEvent& event : slot.events) {
                if (event.begin == InvalidQuery || event.end == InvalidQuery) continue;
                const double ms = double(timestamps[event.end] - timestamps[event.begin])
                                * 1000.0 / double(timestampFrequency);
                gpuSamples.push_back({ event.name, ms });
            }
            D3D12_RANGE writtenRange = { 0, 0 };
            readback->Unmap(0, &writtenRange);
        }
        slot.pending = false;
    }

    ComPtr<ID3D12QueryHeap> queryHeap;
    ComPtr<ID3D12Resource> readback;
    UINT64 timestampFrequency = 0;
    Slot slots[FRAME_COUNT];
    UINT currentSlot = 0;
    bool initialized = false;
    bool gpuFrameActive = false;
    bool cpuFrameActive = false;
    Clock::time_point cpuFrameStart = {};
    double cpuFrameMs = 0.0;
    double gpuFrameMs = 0.0;
    std::vector<ProfilerSampleDX12> currentCpuSamples;
    std::vector<ProfilerSampleDX12> cpuSamples;
    std::vector<ProfilerSampleDX12> gpuSamples;
    std::deque<double> gpuFrameHistory;
};

#endif
