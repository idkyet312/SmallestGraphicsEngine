#ifndef DXR_DDGI_RENDERER_H
#define DXR_DDGI_RENDERER_H

#include "DXRProbeLayout.h"
#include "DXRScene.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

// Sparse probe-field owner. DXRScene owns geometry; this class owns derived
// layout, lookup buffers, temporal atlases, update scheduling, and editor state.
class DXRDDGIRenderer {
public:
    struct Status {
        bool dxrSupported = false;
        // Tier 1.1 / inline RayQuery. Enhanced visuals require this; probe GI
        // does not.
        bool inlineRaytracingSupported = false;
        bool updatesActive = false;
        uint32_t probeCount = 0;
        uint32_t raysPerFrame = 0;
        uint64_t gpuMemoryBytes = 0;
        std::string cacheStatus = "Not built";
    };

    bool Initialize(ID3D12Device* device) {
        device_ = device;
        status_.dxrSupported = scene_.Initialize(device);
        status_.inlineRaytracingSupported = scene_.InlineSupported();
        if (status_.dxrSupported &&
            SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxrDevice_))))
            pipelineReady_ = CreateRaytracingPipeline();
        return status_.dxrSupported;
    }

    void ApplySettings(const LevelDXRDDGISettings& value) {
        const bool layoutChanged =
            value.surfaceSpacing != settings_.surfaceSpacing ||
            value.surfaceOffset != settings_.surfaceOffset ||
            value.maxProbes != settings_.maxProbes;
        const bool historyChanged = value.hysteresis != settings_.hysteresis ||
            value.multiBounceStrength != settings_.multiBounceStrength ||
            value.maxRayDistance != settings_.maxRayDistance ||
            value.intensity != settings_.intensity ||
            value.normalBias != settings_.normalBias ||
            value.viewBias != settings_.viewBias;
        settings_ = value;
        // Irradiance atlas has an 8x8 directional interior. More than 64 rays
        // would race while writing the same directional texels.
        settings_.raysPerProbe =
            (std::min)(64u, (std::max)(8u, settings_.raysPerProbe));
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
        ClearAtlases(commandList);
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
    void UpdateProbes(ID3D12GraphicsCommandList4* commandList,
                      uint32_t frameIndex,
                      const DirectX::XMFLOAT3& sunDirection,
                      const DirectX::XMFLOAT3& sunColor,
                      float sunIntensity = 1.0f,
                      float skyIntensity = 1.0f,
                      const DirectX::XMFLOAT3& pointLightPosition = {},
                      const DirectX::XMFLOAT3& pointLightColor = {},
                      float pointLightRadius = 0.0f,
                      float pointLightIntensity = 0.0f) {
        if (!settings_.enabled || !status_.dxrSupported ||
            !pipelineReady_ || !commandList || !scene_.TLAS() ||
            layout_.probes.empty() || historyDirty_)
            return;
        const uint32_t remaining =
            static_cast<uint32_t>(layout_.probes.size()) - updateCursor_;
        const uint32_t count = (std::min)(settings_.probesPerFrame, remaining);
        lastDispatchStart_ = updateCursor_;
        lastDispatchCount_ = count;

        const uint32_t previousIndex = historyIndex_;
        const uint32_t currentIndex = historyIndex_ ^ 1u;
        if (historyValid_) {
            Transition(commandList, irradiance_[previousIndex].Get(),
                irradianceStates_[previousIndex],
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(commandList, irradiance_[currentIndex].Get(),
                irradianceStates_[currentIndex],
                D3D12_RESOURCE_STATE_COPY_DEST);
            commandList->CopyResource(irradiance_[currentIndex].Get(),
                                      irradiance_[previousIndex].Get());
            Transition(commandList, visibility_[previousIndex].Get(),
                visibilityStates_[previousIndex],
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(commandList, visibility_[currentIndex].Get(),
                visibilityStates_[currentIndex],
                D3D12_RESOURCE_STATE_COPY_DEST);
            commandList->CopyResource(visibility_[currentIndex].Get(),
                                      visibility_[previousIndex].Get());
        }
        Transition(commandList, irradiance_[previousIndex].Get(),
            irradianceStates_[previousIndex],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, irradiance_[currentIndex].Get(),
            irradianceStates_[currentIndex],
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(commandList, visibility_[currentIndex].Get(),
            visibilityStates_[currentIndex],
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (historyValid_) {
            Transition(commandList, visibility_[previousIndex].Get(),
                visibilityStates_[previousIndex],
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        UpdateRaytracingDescriptors(previousIndex, currentIndex);

        ProbeConstants constants{};
        constants.startProbe = updateCursor_;
        constants.probeCount = count;
        constants.raysPerProbe = settings_.raysPerProbe;
        constants.atlasColumns = static_cast<uint32_t>(std::ceil(std::sqrt(
            static_cast<float>(layout_.probes.size()))));
        constants.irradianceTileSize = 10;
        constants.frameIndex = frameIndex;
        constants.hysteresis = historyValid_ ? settings_.hysteresis : 0.0f;
        constants.multiBounceStrength = historyValid_
            ? settings_.multiBounceStrength : 0.0f;
        constants.maxRayDistance = settings_.maxRayDistance;
        constants.surfaceBias = (std::max)(settings_.normalBias, 0.02f);
        constants.sunDirection = sunDirection;
        constants.sunIntensity = sunIntensity;
        constants.sunColor = sunColor;
        constants.skyIntensity = skyIntensity;
        constants.pointLightPosition = pointLightPosition;
        constants.pointLightRadius = pointLightRadius;
        constants.pointLightColor = pointLightColor;
        constants.pointLightIntensity = pointLightIntensity;
        *mappedConstants_ = constants;

        commandList->SetPipelineState1(stateObject_.Get());
        commandList->SetComputeRootSignature(globalRootSignature_.Get());
        commandList->SetComputeRootConstantBufferView(
            0, constantBuffer_->GetGPUVirtualAddress());
        commandList->SetComputeRootShaderResourceView(
            1, scene_.TLASAddress());
        commandList->SetComputeRootShaderResourceView(
            2, probeBuffer_->GetGPUVirtualAddress());
        ID3D12DescriptorHeap* heaps[] = { raytracingHeap_.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetComputeRootDescriptorTable(
            3, raytracingHeap_->GetGPUDescriptorHandleForHeapStart());

        D3D12_DISPATCH_RAYS_DESC dispatch{};
        const D3D12_GPU_VIRTUAL_ADDRESS table =
            shaderTable_->GetGPUVirtualAddress();
        dispatch.RayGenerationShaderRecord = { table, shaderRecordSize_ };
        dispatch.MissShaderTable.StartAddress = table + shaderRecordSize_;
        dispatch.MissShaderTable.SizeInBytes = shaderRecordSize_ * 2u;
        dispatch.MissShaderTable.StrideInBytes = shaderRecordSize_;
        dispatch.HitGroupTable.StartAddress = table + shaderRecordSize_ * 3u;
        dispatch.HitGroupTable.SizeInBytes = shaderRecordSize_;
        dispatch.HitGroupTable.StrideInBytes = shaderRecordSize_;
        dispatch.Width = settings_.raysPerProbe;
        dispatch.Height = count;
        dispatch.Depth = 1;
        commandList->DispatchRays(&dispatch);
        status_.updatesActive = true;
        if (!firstDispatchLogged_) {
            std::cout << "DXR DDGI: DispatchRays active ("
                      << dispatch.Width << "x" << dispatch.Height
                      << ", probes=" << layout_.probes.size() << ")\n";
            firstDispatchLogged_ = true;
        }

        D3D12_RESOURCE_BARRIER uavs[2] = {};
        for (D3D12_RESOURCE_BARRIER& barrier : uavs)
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavs[0].UAV.pResource = irradiance_[currentIndex].Get();
        uavs[1].UAV.pResource = visibility_[currentIndex].Get();
        commandList->ResourceBarrier(2, uavs);
        Transition(commandList, irradiance_[currentIndex].Get(),
            irradianceStates_[currentIndex],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(commandList, visibility_[currentIndex].Get(),
            visibilityStates_[currentIndex],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        for (uint32_t i = 0; i < count; ++i) {
            DXRProbeRecord& probe =
                layout_.probes[(updateCursor_ + i) % layout_.probes.size()];
            if (probe.state != DXRProbeState::Rejected)
                probe.state = DXRProbeState::Valid;
            probe.lastUpdatedFrame = frameIndex;
        }
        updateCursor_ += count;
        if (updateCursor_ >= layout_.probes.size()) updateCursor_ = 0;
        historyIndex_ = currentIndex;
        historyValid_ = true;
    }

    void ResetHistory() {
        historyDirty_ = false;
        historyIndex_ = 0;
        historyValid_ = false;
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
    D3D12_RESOURCE_STATES irradianceStates_[2] = {
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    };
    D3D12_RESOURCE_STATES visibilityStates_[2] = {
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    };
    std::vector<ComPtr<ID3D12Resource>> pendingUploads_;
    ComPtr<ID3D12Device5> dxrDevice_;
    ComPtr<ID3D12RootSignature> globalRootSignature_;
    ComPtr<ID3D12StateObject> stateObject_;
    ComPtr<ID3D12Resource> shaderTable_;
    ComPtr<ID3D12Resource> constantBuffer_;
    ComPtr<ID3D12DescriptorHeap> raytracingHeap_;
    uint32_t shaderRecordSize_ = 0;
    bool pipelineReady_ = false;
    bool firstDispatchLogged_ = false;
    bool historyValid_ = false;
    struct alignas(256) ProbeConstants {
        uint32_t startProbe = 0;
        uint32_t probeCount = 0;
        uint32_t raysPerProbe = 0;
        uint32_t atlasColumns = 0;
        uint32_t irradianceTileSize = 10;
        uint32_t frameIndex = 0;
        float hysteresis = 0.0f;
        float multiBounceStrength = 0.0f;
        float maxRayDistance = 24.0f;
        float surfaceBias = 0.1f;
        float padding0[2] = {};
        DirectX::XMFLOAT3 sunDirection = { 0.0f, -1.0f, 0.0f };
        float sunIntensity = 1.0f;
        DirectX::XMFLOAT3 sunColor = { 1.0f, 1.0f, 1.0f };
        float skyIntensity = 1.0f;
        DirectX::XMFLOAT3 pointLightPosition = {};
        float pointLightRadius = 0.0f;
        DirectX::XMFLOAT3 pointLightColor = {};
        float pointLightIntensity = 0.0f;
    };
    ProbeConstants* mappedConstants_ = nullptr;

    bool CreateRaytracingPipeline() {
        std::vector<char> library;
        const std::filesystem::path candidates[] = {
            "shaders/dxr_ddgi.cso",
            "dxr_ddgi.cso",
            "build/dxr_ddgi.cso",
            "build/RelWithDebInfo/shaders/dxr_ddgi.cso"
        };
        for (const auto& path : candidates) {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream) continue;
            const std::streamsize size = stream.tellg();
            if (size <= 0) continue;
            library.resize(static_cast<size_t>(size));
            stream.seekg(0);
            if (stream.read(library.data(), size)) break;
            library.clear();
        }
        if (library.empty()) {
            status_.cacheStatus = "DXR DDGI shader missing";
            return false;
        }

        D3D12_DESCRIPTOR_RANGE ranges[3] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 4;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[2].NumDescriptors = 1;
        ranges[2].BaseShaderRegister = 1;
        ranges[2].OffsetInDescriptorsFromTableStart = 2;
        D3D12_ROOT_PARAMETER parameters[4] = {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[1].Descriptor.ShaderRegister = 0;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[2].Descriptor.ShaderRegister = 1;
        parameters[3].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[3].DescriptorTable.NumDescriptorRanges = 3;
        parameters[3].DescriptorTable.pDescriptorRanges = ranges;
        for (auto& parameter : parameters)
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = 4;
        root.pParameters = parameters;
        ComPtr<ID3DBlob> rootBlob;
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3D12SerializeRootSignature(&root,
                D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errors)) ||
            FAILED(device_->CreateRootSignature(0, rootBlob->GetBufferPointer(),
                rootBlob->GetBufferSize(),
                IID_PPV_ARGS(&globalRootSignature_))))
            return false;

        D3D12_EXPORT_DESC exports[4] = {};
        exports[0].Name = L"ProbeRayGen";
        exports[1].Name = L"RadianceMiss";
        exports[2].Name = L"ShadowMiss";
        exports[3].Name = L"SurfaceClosestHit";
        D3D12_DXIL_LIBRARY_DESC dxil{};
        dxil.DXILLibrary = { library.data(), library.size() };
        dxil.NumExports = 4;
        dxil.pExports = exports;
        D3D12_HIT_GROUP_DESC hitGroup{};
        hitGroup.HitGroupExport = L"ProbeHitGroup";
        hitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitGroup.ClosestHitShaderImport = L"SurfaceClosestHit";
        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
        shaderConfig.MaxPayloadSizeInBytes = 16;
        shaderConfig.MaxAttributeSizeInBytes = 8;
        ID3D12RootSignature* global = globalRootSignature_.Get();
        D3D12_RAYTRACING_PIPELINE_CONFIG pipeline{};
        pipeline.MaxTraceRecursionDepth = 2;
        D3D12_STATE_SUBOBJECT subobjects[5] = {};
        subobjects[0] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &dxil };
        subobjects[1] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroup };
        subobjects[2] = {
            D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig
        };
        subobjects[3] = {
            D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global
        };
        subobjects[4] = {
            D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline
        };
        D3D12_STATE_OBJECT_DESC state{};
        state.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        state.NumSubobjects = _countof(subobjects);
        state.pSubobjects = subobjects;
        if (FAILED(dxrDevice_->CreateStateObject(
                &state, IID_PPV_ARGS(&stateObject_))))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.NumDescriptors = 7;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(
                &heap, IID_PPV_ARGS(&raytracingHeap_))) ||
            !CreateBuffer(sizeof(ProbeConstants), D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ, constantBuffer_))
            return false;
        if (FAILED(constantBuffer_->Map(
                0, nullptr, reinterpret_cast<void**>(&mappedConstants_))))
            return false;

        ComPtr<ID3D12StateObjectProperties> properties;
        if (FAILED(stateObject_.As(&properties))) return false;
        const wchar_t* identifiers[4] = {
            L"ProbeRayGen", L"RadianceMiss", L"ShadowMiss", L"ProbeHitGroup"
        };
        shaderRecordSize_ = (D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES +
            D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT - 1u) &
            ~(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT - 1u);
        if (!CreateBuffer(static_cast<uint64_t>(shaderRecordSize_) * 4u,
                D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                shaderTable_))
            return false;
        uint8_t* table = nullptr;
        if (FAILED(shaderTable_->Map(
                0, nullptr, reinterpret_cast<void**>(&table))))
            return false;
        for (uint32_t i = 0; i < 4; ++i) {
            const void* identifier =
                properties->GetShaderIdentifier(identifiers[i]);
            if (!identifier) {
                shaderTable_->Unmap(0, nullptr);
                return false;
            }
            memcpy(table + static_cast<size_t>(shaderRecordSize_) * i,
                identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        }
        shaderTable_->Unmap(0, nullptr);
        return true;
    }

    void UpdateRaytracingDescriptors(uint32_t previousIndex,
                                     uint32_t currentIndex) {
        const UINT descriptorSize = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            raytracingHeap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(
            irradiance_[previousIndex].Get(), &srv, handle);
        handle.ptr += descriptorSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device_->CreateUnorderedAccessView(
            irradiance_[currentIndex].Get(), nullptr, &uav, handle);
        handle.ptr += descriptorSize;
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        device_->CreateUnorderedAccessView(
            visibility_[currentIndex].Get(), nullptr, &uav, handle);
    }

    void ClearAtlases(ID3D12GraphicsCommandList* commandList) {
        if (!raytracingHeap_ || !commandList) return;
        ID3D12DescriptorHeap* heaps[] = { raytracingHeap_.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        const UINT descriptorSize = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            raytracingHeap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu =
            raytracingHeap_->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(descriptorSize) * 3u;
        gpu.ptr += static_cast<UINT64>(descriptorSize) * 3u;
        const float zeros[4] = {};
        for (uint32_t index = 0; index < 2; ++index) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            device_->CreateUnorderedAccessView(
                irradiance_[index].Get(), nullptr, &uav, cpu);
            commandList->ClearUnorderedAccessViewFloat(
                gpu, cpu, irradiance_[index].Get(), zeros, 0, nullptr);
            cpu.ptr += descriptorSize;
            gpu.ptr += descriptorSize;
            uav.Format = DXGI_FORMAT_R16G16_FLOAT;
            device_->CreateUnorderedAccessView(
                visibility_[index].Get(), nullptr, &uav, cpu);
            commandList->ClearUnorderedAccessViewFloat(
                gpu, cpu, visibility_[index].Get(), zeros, 0, nullptr);
            cpu.ptr += descriptorSize;
            gpu.ptr += descriptorSize;
        }
    }

    static void Transition(ID3D12GraphicsCommandList* commandList,
                           ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES& current,
                           D3D12_RESOURCE_STATES next) {
        if (!resource || current == next) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = current;
        barrier.Transition.StateAfter = next;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);
        current = next;
    }

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
            irradianceStates_[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            visibilityStates_[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
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
