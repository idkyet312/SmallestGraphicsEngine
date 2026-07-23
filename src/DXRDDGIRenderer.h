#ifndef DXR_DDGI_RENDERER_H
#define DXR_DDGI_RENDERER_H

#include "DXRProbeLayout.h"
#include "DXRScene.h"
#include <cstring>
#include <filesystem>
#include <string>

// Sparse probe-field owner. DXRScene owns geometry; this class owns derived
// layout, lookup buffers, temporal atlases, update scheduling, and editor state.
class DXRDDGIRenderer {
public:
    struct Status {
        bool dxrSupported = false;
        uint32_t probeCount = 0;
        uint32_t raysPerFrame = 0;
        uint64_t gpuMemoryBytes = 0;
        std::string cacheStatus = "Not built";
    };

    bool Initialize(ID3D12Device* device) {
        device_ = device;
        status_.dxrSupported = scene_.Initialize(device);
        return status_.dxrSupported;
    }

    void ApplySettings(const LevelDXRDDGISettings& value) {
        const bool layoutChanged =
            value.surfaceSpacing != settings_.surfaceSpacing ||
            value.surfaceOffset != settings_.surfaceOffset ||
            value.maxProbes != settings_.maxProbes;
        const bool historyChanged = value.hysteresis != settings_.hysteresis ||
            value.multiBounceStrength != settings_.multiBounceStrength ||
            value.intensity != settings_.intensity ||
            value.normalBias != settings_.normalBias ||
            value.viewBias != settings_.viewBias;
        settings_ = value;
        if (settings_.probesPerFrame > settings_.maxProbes)
            settings_.probesPerFrame = settings_.maxProbes;
        layoutDirty_ |= layoutChanged;
        historyDirty_ |= historyChanged;
        status_.raysPerFrame = settings_.enabled && status_.dxrSupported
            ? settings_.raysPerProbe * settings_.probesPerFrame : 0;
    }

    bool BuildProbeLayout(const std::vector<DXRProbeTriangle>& triangles,
                          uint64_t geometryHash,
                          const std::filesystem::path& cachePath) {
        if (!settings_.enabled || !status_.dxrSupported) {
            layout_.Clear();
            status_.probeCount = 0;
            status_.cacheStatus = status_.dxrSupported ? "Disabled" :
                "DXR unsupported";
            return false;
        }
        const uint64_t settingsHash =
            DXRProbeLayout::SettingsHash(settings_);
        if (layout_.LoadCache(cachePath, geometryHash, settingsHash,
                              settings_.maxProbes)) {
            status_.cacheStatus = "Loaded";
        } else {
            if (!layout_.Build(triangles, settings_, geometryHash)) {
                status_.cacheStatus = "No static geometry";
                return false;
            }
            status_.cacheStatus =
                layout_.SaveCache(cachePath, settingsHash) ? "Generated" :
                                                            "Generated (uncached)";
        }
        layoutDirty_ = false;
        historyDirty_ = true;
        updateCursor_ = 0;
        status_.probeCount = static_cast<uint32_t>(layout_.probes.size());
        return true;
    }

    // Upload positions, metadata, hashed cells, and cell index lists. Buffers are
    // directly consumable as StructuredBuffer objects by both shading paths.
    bool UploadProbeBuffers(ID3D12GraphicsCommandList* commandList) {
        if (!device_ || !commandList || layout_.probes.empty()) return false;
        gpuMemoryBytes_ = 0;
        if (!UploadVector(commandList, layout_.probes, probeBuffer_) ||
            !UploadVector(commandList, layout_.cells, cellBuffer_) ||
            !UploadVector(commandList, layout_.cellProbeIndices, indexBuffer_))
            return false;
        if (!CreateAtlases()) return false;
        status_.gpuMemoryBytes = gpuMemoryBytes_;
        return true;
    }

    // Call only after the queue fence confirms all layout copies completed.
    void ReleaseCompletedUploads() { pendingUploads_.clear(); }

    bool UpdateTLAS(ID3D12GraphicsCommandList4* commandList,
                    const std::vector<DXRScene::Instance>& instances) {
        return scene_.UpdateTLAS(commandList, instances);
    }

    // Schedules a bounded probe batch. Dispatch integration consumes StartProbe
    // and ProbeCount; completed probes become green in the editor.
    void UpdateProbes(uint32_t frameIndex) {
        if (!settings_.enabled || !status_.dxrSupported ||
            layout_.probes.empty() || historyDirty_)
            return;
        const uint32_t count = (std::min)(settings_.probesPerFrame,
            static_cast<uint32_t>(layout_.probes.size()));
        lastDispatchStart_ = updateCursor_;
        lastDispatchCount_ = count;
        for (uint32_t i = 0; i < count; ++i) {
            DXRProbeRecord& probe =
                layout_.probes[(updateCursor_ + i) % layout_.probes.size()];
            if (probe.state != DXRProbeState::Rejected)
                probe.state = DXRProbeState::Valid;
            probe.lastUpdatedFrame = frameIndex;
        }
        updateCursor_ = (updateCursor_ + count) %
            static_cast<uint32_t>(layout_.probes.size());
        historyIndex_ ^= 1u;
    }

    void ResetHistory() {
        historyDirty_ = false;
        historyIndex_ = 0;
        updateCursor_ = 0;
        for (DXRProbeRecord& probe : layout_.probes) {
            if (probe.state != DXRProbeState::Rejected)
                probe.state = DXRProbeState::Pending;
            probe.lastUpdatedFrame = 0;
        }
    }

    void MarkLayoutDirty() { layoutDirty_ = true; }
    bool LayoutDirty() const { return layoutDirty_; }
    bool HistoryDirty() const { return historyDirty_; }
    const std::vector<DXRProbeRecord>& GetDebugProbes() const {
        return layout_.probes;
    }
    const DXRProbeLayout& Layout() const { return layout_; }
    DXRScene& Scene() { return scene_; }
    const Status& GetStatus() const { return status_; }
    ID3D12Resource* ProbeBuffer() const { return probeBuffer_.Get(); }
    ID3D12Resource* CellBuffer() const { return cellBuffer_.Get(); }
    ID3D12Resource* IndexBuffer() const { return indexBuffer_.Get(); }
    ID3D12Resource* IrradianceAtlas() const {
        return irradiance_[historyIndex_].Get();
    }
    ID3D12Resource* VisibilityAtlas() const {
        return visibility_[historyIndex_].Get();
    }
    uint32_t LastDispatchStart() const { return lastDispatchStart_; }
    uint32_t LastDispatchCount() const { return lastDispatchCount_; }

private:
    ID3D12Device* device_ = nullptr;
    LevelDXRDDGISettings settings_;
    DXRScene scene_;
    DXRProbeLayout layout_;
    Status status_;
    bool layoutDirty_ = true;
    bool historyDirty_ = true;
    uint32_t updateCursor_ = 0;
    uint32_t historyIndex_ = 0;
    uint32_t lastDispatchStart_ = 0;
    uint32_t lastDispatchCount_ = 0;
    uint64_t gpuMemoryBytes_ = 0;
    ComPtr<ID3D12Resource> probeBuffer_;
    ComPtr<ID3D12Resource> cellBuffer_;
    ComPtr<ID3D12Resource> indexBuffer_;
    ComPtr<ID3D12Resource> irradiance_[2];
    ComPtr<ID3D12Resource> visibility_[2];
    std::vector<ComPtr<ID3D12Resource>> pendingUploads_;

    template<class T>
    bool UploadVector(ID3D12GraphicsCommandList* commandList,
                      const std::vector<T>& source,
                      ComPtr<ID3D12Resource>& destination) {
        if (source.empty()) return false;
        const uint64_t bytes = source.size() * sizeof(T);
        ComPtr<ID3D12Resource> upload;
        if (!CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COPY_DEST, destination) ||
            !CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ, upload))
            return false;
        void* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, &mapped))) return false;
        memcpy(mapped, source.data(), static_cast<size_t>(bytes));
        upload->Unmap(0, nullptr);
        commandList->CopyBufferRegion(destination.Get(), 0, upload.Get(), 0,
                                      bytes);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);
        pendingUploads_.push_back(std::move(upload));
        gpuMemoryBytes_ += (bytes + 255u) & ~255ull;
        return true;
    }

    bool CreateBuffer(uint64_t bytes, D3D12_HEAP_TYPE heapType,
                      D3D12_RESOURCE_STATES state,
                      ComPtr<ID3D12Resource>& output) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = heapType;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = (bytes + 255u) & ~255ull;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device_->CreateCommittedResource(&heap,
            D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
            IID_PPV_ARGS(&output)));
    }

    bool CreateAtlases() {
        const uint32_t probeCount =
            static_cast<uint32_t>(layout_.probes.size());
        const uint32_t columns = static_cast<uint32_t>(
            std::ceil(std::sqrt(static_cast<float>(probeCount))));
        const uint32_t rows = (probeCount + columns - 1u) / columns;
        for (uint32_t i = 0; i < 2; ++i) {
            if (!CreateTexture(columns * 10u, rows * 10u,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, irradiance_[i]) ||
                !CreateTexture(columns * 18u, rows * 18u,
                    DXGI_FORMAT_R16G16_FLOAT, visibility_[i]))
                return false;
        }
        return true;
    }

    bool CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format,
                       ComPtr<ID3D12Resource>& output) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device_->CreateCommittedResource(&heap,
                D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&output))))
            return false;
        const uint32_t bytesPerPixel =
            format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
        gpuMemoryBytes_ += static_cast<uint64_t>(width) * height * bytesPerPixel;
        return true;
    }
};

#endif
