#pragma once

#include "DX12Core.h"
#include "ProfilerDX12.h"
#include "UltraWaterSimulation.h"
#include <d3dcompiler.h>
#include <array>
#include <cstring>
#include <string>
#include <vector>

class UltraWaterSimulationDX12 {
public:
    static constexpr UINT SpectrumResolution = 256;
    static constexpr UINT CascadeCount = 3;
    static constexpr UINT MaxInteractions = 16;

    bool Init() {
        Shutdown();
        ComPtr<ID3DBlob> spectrumShader;
        ComPtr<ID3DBlob> coastShader;
        ComPtr<ID3DBlob> heightQueryShader;
        if (FAILED(D3DReadFileToBlob(
                L"shaders/ultra_water_spectrum_cs.cso", &spectrumShader)) ||
            FAILED(D3DReadFileToBlob(
                L"shaders/ultra_water_coast_cs.cso", &coastShader)) ||
            FAILED(D3DReadFileToBlob(
                L"shaders/ultra_water_height_query_cs.cso",
                &heightQueryShader))) {
            failureReason_ = "SM 6.5 Ultra water compute shaders are unavailable";
            return false;
        }
        if (!CreateRootSignature() ||
            !CreatePipeline(spectrumShader.Get(), spectrumPSO_) ||
            !CreatePipeline(coastShader.Get(), coastPSO_) ||
            !CreatePipeline(heightQueryShader.Get(), heightQueryPSO_) ||
            !CreateDescriptorHeap() || !CreateConstantBuffer() ||
            !CreateSpectrumTextures() || !CreateHeightQueryResources()) {
            failureReason_ = "Ultra water GPU resources could not be created";
            Shutdown();
            return false;
        }
        waves_ = BuildUltraSpectralWaves();
        initialized_ = true;
        return true;
    }

    void Shutdown() {
        if (constantBuffer_ && mappedConstants_)
            constantBuffer_->Unmap(0, nullptr);
        mappedConstants_ = nullptr;
        initialized_ = false;
        ready_ = false;
        rootSignature_.Reset();
        spectrumPSO_.Reset();
        coastPSO_.Reset();
        heightQueryPSO_.Reset();
        heap_.Reset();
        constantBuffer_.Reset();
        for (auto& resource : spectrum_) resource.Reset();
        for (auto& resource : coast_) resource.Reset();
        bathymetry_.Reset();
        bathymetryUpload_.Reset();
        for (auto& retired : retiredBathymetry_) {
            retired.texture.Reset();
            retired.upload.Reset();
            for (auto& coastTexture : retired.coast)
                coastTexture.Reset();
        }
        for (auto& resource : heightQueryOutput_) resource.Reset();
        for (auto& resource : heightQueryReadback_) resource.Reset();
        pendingHeights_.clear();
        pendingDesc_ = {};
        frameParity_ = 0;
        coastReadIndex_ = 0;
        resetCoast_ = true;
        forceBathymetryRefresh_ = false;
        accumulatedTime_ = 0.0f;
        pendingQueryHead_ = 0;
        pendingQueryCount_ = 0;
        completedQueryCount_ = 0;
        querySlotValid_.fill(false);
    }

    bool Initialized() const { return initialized_; }
    bool Ready() const { return initialized_ && ready_; }
    const std::string& FailureReason() const { return failureReason_; }

    void RequestRefresh() {
        if (!initialized_) return;
        waves_ = BuildUltraSpectralWaves();
        accumulatedTime_ = 0.0f;
        resetCoast_ = true;
        forceBathymetryRefresh_ = true;
        interactions_.Clear();
        pendingQueryHead_ = 0;
        pendingQueryCount_ = 0;
        completedQueryCount_ = 0;
        querySlotValid_.fill(false);
    }

    void QueueBathymetryRebuild(const WaterBathymetryDesc& desc) {
        if (!desc.Valid()) return;
        if (!forceBathymetryRefresh_ && ready_ &&
            desc.terrainRevision == terrainRevision_ &&
            desc.resolution == bathymetryResolution_ &&
            desc.minimumXZ.x == bounds_.x && desc.minimumXZ.y == bounds_.y &&
            desc.maximumXZ.x == bounds_.z && desc.maximumXZ.y == bounds_.w)
            return;
        pendingDesc_ = desc;
        pendingHeights_.resize(
            static_cast<size_t>(desc.resolution) * desc.resolution);
        const float spanX = desc.maximumXZ.x - desc.minimumXZ.x;
        const float spanZ = desc.maximumXZ.y - desc.minimumXZ.y;
        for (uint32_t z = 0; z < desc.resolution; ++z) {
            const float worldZ = desc.minimumXZ.y +
                (z + 0.5f) * spanZ / desc.resolution;
            for (uint32_t x = 0; x < desc.resolution; ++x) {
                const float worldX = desc.minimumXZ.x +
                    (x + 0.5f) * spanX / desc.resolution;
                pendingHeights_[static_cast<size_t>(z) * desc.resolution + x] =
                    desc.heightAt(worldX, worldZ);
            }
        }
        // Terrain height identifies the true waterline, including sculpted
        // bays and headlands. Convert that mask to a signed Euclidean distance
        // before deriving the solver bed, so every depth contour follows the
        // local shoreline rather than inheriting unrelated inland relief.
        pendingHeights_ = UltraWaterDetail::BuildShoreBathymetry(
            pendingHeights_, desc.resolution, desc.resolution,
            spanX / desc.resolution, spanZ / desc.resolution);
    }

    void QueueInteraction(const WaterInteraction& event) {
        interactions_.Push(event);
    }
    void QueueHeightQuery(const WaterHeightQuery& query) {
        if (pendingQueryCount_ < MaxHeightQueries) {
            pendingQueries_[(pendingQueryHead_ + pendingQueryCount_) %
                            MaxHeightQueries] = query;
            ++pendingQueryCount_;
        } else {
            pendingQueries_[pendingQueryHead_] = query;
            pendingQueryHead_ = (pendingQueryHead_ + 1) % MaxHeightQueries;
        }
    }
    bool TryGetHeightResult(uint64_t objectId,
                            WaterHeightResult& result) const {
        for (UINT i = 0; i < completedQueryCount_; ++i) {
            if (completedQueries_[i].objectId == objectId) {
                result = completedQueries_[i];
                return true;
            }
        }
        return false;
    }

    ID3D12Resource* CurrentSpectrum() const {
        return spectrum_[frameParity_].Get();
    }
    ID3D12Resource* PreviousSpectrum() const {
        return spectrum_[frameParity_ ^ 1u].Get();
    }
    ID3D12Resource* CurrentCoast() const {
        return coast_[coastReadIndex_].Get();
    }
    ID3D12Resource* PreviousCoast() const {
        return coast_[coastReadIndex_ ^ 1u].Get();
    }
    ID3D12Resource* Bathymetry() const { return bathymetry_.Get(); }
    XMFLOAT4 Bounds() const { return bounds_; }
    UINT CoastResolution() const { return coastResolution_; }
    UINT BathymetryResolution() const { return bathymetryResolution_; }

    bool PrepareBathymetry() {
        if (!initialized_) return false;
        if (!pendingHeights_.empty() && !UploadPendingBathymetry())
            return false;
        return ready_;
    }

    bool Update(float deltaTime, float absoluteTime, float surfaceY,
                const UltraWaterTuning& tuning,
                ProfilerDX12* profiler = nullptr) {
        if (!initialized_) return false;
        if (!PrepareBathymetry()) return false;

        if (tuning.waveHeight != tuning_.waveHeight ||
            tuning.waveScale != tuning_.waveScale ||
            tuning.waveSpeed != tuning_.waveSpeed ||
            tuning.directionRadians != tuning_.directionRadians ||
            tuning.choppiness != tuning_.choppiness ||
            tuning.surfStrength != tuning_.surfStrength ||
            tuning.foamStrength != tuning_.foamStrength ||
            tuning.coastDamping != tuning_.coastDamping) {
            tuning_ = tuning;
        }

        const UINT frameSlot = g_dx12.frameIndex % FRAME_COUNT;
        ConsumeHeightQueries(frameSlot);

        frameParity_ ^= 1u;
        ID3D12GraphicsCommandList* list = g_dx12.commandList.Get();
        ID3D12DescriptorHeap* heaps[] = { heap_.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(rootSignature_.Get());
        WriteDescriptorSets();

        const UINT spectrumEvent = profiler
            ? profiler->BeginGpuEvent("Water Spectrum", list) : UINT_MAX;
        Transition(list, spectrum_[frameParity_].Get(), ShaderReadState(),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        BindSet(list, coastReadIndex_, frameParity_);
        list->SetPipelineState(spectrumPSO_.Get());
        const ComputeConstants spectrumConstants = BuildConstants(
            0.0f, absoluteTime, false, false, nullptr, 0);
        BindConstants(list, spectrumConstants, 0);
        list->Dispatch((SpectrumResolution + 7) / 8,
                       (SpectrumResolution + 7) / 8, CascadeCount);
        UAVBarrier(list, spectrum_[frameParity_].Get());
        Transition(list, spectrum_[frameParity_].Get(),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, ShaderReadState());
        if (profiler) profiler->EndGpuEvent(spectrumEvent, list);

        std::array<WaterInteraction, MaxInteractions> interactionBatch{};
        const size_t interactionCount = interactions_.Drain(
            interactionBatch.data(), interactionBatch.size());
        accumulatedTime_ += (std::min)(deltaTime, 1.0f / 30.0f);
        UINT substeps = (std::min)(4u, static_cast<UINT>(
            accumulatedTime_ / (1.0f / 120.0f)));
        if (resetCoast_) substeps = (std::max)(1u, substeps);

        const UINT coastEvent = profiler
            ? profiler->BeginGpuEvent("Water Coastal Simulation", list)
            : UINT_MAX;
        list->SetPipelineState(coastPSO_.Get());
        for (UINT step = 0; step < substeps; ++step) {
            const UINT writeIndex = coastReadIndex_ ^ 1u;
            Transition(list, coast_[writeIndex].Get(), ShaderReadState(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            BindSet(list, coastReadIndex_, frameParity_);
            const bool applyInteractions = step == 0 && interactionCount > 0;
            const ComputeConstants constants = BuildConstants(
                1.0f / 120.0f, absoluteTime, resetCoast_, applyInteractions,
                interactionBatch.data(), static_cast<UINT>(interactionCount));
            BindConstants(list, constants, step + 1);
            list->Dispatch((coastResolution_ + 7) / 8,
                           (coastResolution_ + 7) / 8, 1);
            UAVBarrier(list, coast_[writeIndex].Get());
            Transition(list, coast_[writeIndex].Get(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, ShaderReadState());
            coastReadIndex_ = writeIndex;
            resetCoast_ = false;
            accumulatedTime_ = (std::max)(
                0.0f, accumulatedTime_ - 1.0f / 120.0f);
        }
        if (profiler) profiler->EndGpuEvent(coastEvent, list);
        DispatchHeightQueries(list, frameSlot, absoluteTime, surfaceY,
                              profiler);
        return true;
    }

private:
    static constexpr UINT DispatchesPerFrame = 6;
    static constexpr UINT DescriptorsPerSet = 5;
    static constexpr UINT DescriptorSetCount = 4;
    static constexpr UINT MaxHeightQueries = 16;

    struct InteractionGPU {
        XMFLOAT4 positionRadius;
        XMFLOAT4 velocityType;
    };
    struct ComputeConstants {
        XMFLOAT4 bounds;
        XMFLOAT4 simulation;
        XMFLOAT4 dispatch;
        XMFLOAT4 tuning0;
        XMFLOAT4 tuning1;
        XMFLOAT4 spectralWaves[16];
        XMFLOAT4 spectralExtra[16];
        InteractionGPU interactions[MaxInteractions];
    };
    static_assert(sizeof(ComputeConstants) == 1104,
                  "UltraWaterConstants must match ultra_water_cs.hlsl");
    struct RetiredBathymetryResources {
        ComPtr<ID3D12Resource> texture;
        ComPtr<ID3D12Resource> upload;
        std::array<ComPtr<ID3D12Resource>, 2> coast;
    };

    static D3D12_RESOURCE_STATES ShaderReadState() {
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    static UINT AlignConstantSize(UINT size) {
        return (size + 255u) & ~255u;
    }

    bool CreateRootSignature() {
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 3;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        D3D12_ROOT_PARAMETER roots[3] = {};
        roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        roots[0].Descriptor.ShaderRegister = 0;
        roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[1].DescriptorTable = {1, &ranges[0]};
        roots[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[2].DescriptorTable = {1, &ranges[1]};
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = _countof(roots);
        desc.pParameters = roots;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers = &sampler;
        ComPtr<ID3DBlob> blob;
        if (FAILED(D3D12SerializeRootSignature(
                &desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)))
            return false;
        return SUCCEEDED(g_dx12.device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature_)));
    }

    bool CreatePipeline(ID3DBlob* shader,
                        ComPtr<ID3D12PipelineState>& pipeline) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = rootSignature_.Get();
        desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        return SUCCEEDED(g_dx12.device->CreateComputePipelineState(
            &desc, IID_PPV_ARGS(&pipeline)));
    }

    bool CreateDescriptorHeap() {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors =
            (DescriptorSetCount + FRAME_COUNT) * DescriptorsPerSet;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &desc, IID_PPV_ARGS(&heap_)))) return false;
        descriptorSize_ = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }

    bool CreateConstantBuffer() {
        constantStride_ = AlignConstantSize(sizeof(ComputeConstants));
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = static_cast<UINT64>(constantStride_) *
            FRAME_COUNT * DispatchesPerFrame;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&constantBuffer_)))) return false;
        return SUCCEEDED(constantBuffer_->Map(
            0, nullptr, reinterpret_cast<void**>(&mappedConstants_)));
    }

    bool CreateSpectrumTextures() {
        for (auto& texture : spectrum_)
            if (!CreateTexture(SpectrumResolution, SpectrumResolution,
                    CascadeCount, DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                    texture)) return false;
        return true;
    }

    bool CreateHeightQueryResources() {
        const UINT64 byteSize = MaxHeightQueries * sizeof(XMFLOAT4);
        for (UINT slot = 0; slot < FRAME_COUNT; ++slot) {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = byteSize;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES defaultHeap = {};
            defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                    IID_PPV_ARGS(&heightQueryOutput_[slot])))) return false;

            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES readbackHeap = {};
            readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &readbackHeap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&heightQueryReadback_[slot])))) return false;
        }
        return true;
    }

    bool CreateCoastTextures(UINT resolution) {
        for (auto& texture : coast_) texture.Reset();
        for (auto& texture : coast_)
            if (!CreateTexture(resolution, resolution, 1,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, true, texture))
                return false;
        coastResolution_ = resolution;
        coastReadIndex_ = 0;
        resetCoast_ = true;
        return true;
    }

    bool CreateTexture(UINT width, UINT height, UINT16 arraySize,
                       DXGI_FORMAT format, bool uav,
                       ComPtr<ID3D12Resource>& output) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = arraySize;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                         : D3D12_RESOURCE_FLAG_NONE;
        return SUCCEEDED(g_dx12.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            uav ? ShaderReadState() : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&output)));
    }

    bool UploadPendingBathymetry() {
        // This slot was waited by frame pacing before recording began. Keep
        // the previous bathymetry generation alive in the other slot until
        // its direct-queue work has retired; sculpt commits never need a wait.
        RetiredBathymetryResources& retired =
            retiredBathymetry_[g_dx12.frameIndex % FRAME_COUNT];
        retired.texture.Reset();
        retired.upload.Reset();
        for (auto& texture : retired.coast) texture.Reset();
        retired.texture = bathymetry_;
        retired.upload = bathymetryUpload_;
        retired.coast = coast_;

        const UINT resolution = pendingDesc_.resolution;
        ComPtr<ID3D12Resource> texture;
        if (!CreateTexture(resolution, resolution, 1, DXGI_FORMAT_R32_FLOAT,
                           false, texture)) return false;
        const D3D12_RESOURCE_DESC textureDesc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 rowBytes = 0;
        UINT64 uploadBytes = 0;
        g_dx12.device->GetCopyableFootprints(
            &textureDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &uploadBytes);
        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadBytes;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&upload)))) return false;
        uint8_t* mapped = nullptr;
        if (FAILED(upload->Map(
                0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
        for (UINT row = 0; row < rows; ++row)
            std::memcpy(mapped + footprint.Offset +
                            static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                        pendingHeights_.data() +
                            static_cast<size_t>(row) * resolution,
                        static_cast<size_t>(resolution) * sizeof(float));
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        g_dx12.commandList->CopyTextureRegion(
            &destination, 0, 0, 0, &source, nullptr);
        Transition(g_dx12.commandList.Get(), texture.Get(),
                   D3D12_RESOURCE_STATE_COPY_DEST, ShaderReadState());
        bathymetry_ = texture;
        bathymetryUpload_ = upload;
        bounds_ = {pendingDesc_.minimumXZ.x, pendingDesc_.minimumXZ.y,
                   pendingDesc_.maximumXZ.x, pendingDesc_.maximumXZ.y};
        bathymetryResolution_ = resolution;
        terrainRevision_ = pendingDesc_.terrainRevision;
        forceBathymetryRefresh_ = false;
        if (!CreateCoastTextures(SelectCoastalResolution(
                bounds_.z - bounds_.x, bounds_.w - bounds_.y))) return false;
        pendingHeights_.clear();
        pendingDesc_ = {};
        ready_ = true;
        WriteDescriptorSets();
        return true;
    }

    void WriteDescriptorSets() {
        if (!bathymetry_ || !coast_[0] || !coast_[1]) return;
        for (UINT coastRead = 0; coastRead < 2; ++coastRead) {
            for (UINT spectrumWrite = 0; spectrumWrite < 2; ++spectrumWrite) {
                const UINT set = coastRead * 2 + spectrumWrite;
                D3D12_CPU_DESCRIPTOR_HANDLE handle =
                    Cpu(set * DescriptorsPerSet);
                CreateSRV(bathymetry_.Get(), DXGI_FORMAT_R32_FLOAT,
                          false, handle);
                handle.ptr += descriptorSize_;
                CreateSRV(coast_[coastRead].Get(),
                          DXGI_FORMAT_R16G16B16A16_FLOAT, false, handle);
                handle.ptr += descriptorSize_;
                CreateSRV(spectrum_[spectrumWrite].Get(),
                          DXGI_FORMAT_R16G16B16A16_FLOAT, true, handle);
                handle.ptr += descriptorSize_;
                CreateUAV(coast_[coastRead ^ 1u].Get(),
                          DXGI_FORMAT_R16G16B16A16_FLOAT, false, handle);
                handle.ptr += descriptorSize_;
                CreateUAV(spectrum_[spectrumWrite].Get(),
                          DXGI_FORMAT_R16G16B16A16_FLOAT, true, handle);
            }
        }
    }

    void WriteHeightQuerySet(UINT slot) {
        const UINT base =
            (DescriptorSetCount + slot) * DescriptorsPerSet;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = Cpu(base);
        CreateSRV(bathymetry_.Get(), DXGI_FORMAT_R32_FLOAT, false, handle);
        handle.ptr += descriptorSize_;
        CreateSRV(coast_[coastReadIndex_].Get(),
                  DXGI_FORMAT_R16G16B16A16_FLOAT, false, handle);
        handle.ptr += descriptorSize_;
        CreateSRV(spectrum_[frameParity_].Get(),
                  DXGI_FORMAT_R16G16B16A16_FLOAT, true, handle);
        handle.ptr += descriptorSize_;
        CreateBufferUAV(heightQueryOutput_[slot].Get(), handle);
        handle.ptr += descriptorSize_;
        CreateUAV(nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT, false, handle);
    }

    void CreateSRV(ID3D12Resource* resource, DXGI_FORMAT format, bool array,
                   D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = format;
        srv.ViewDimension = array ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                  : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (array) {
            srv.Texture2DArray.MipLevels = 1;
            srv.Texture2DArray.ArraySize = CascadeCount;
        } else srv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(resource, &srv, handle);
    }
    void CreateUAV(ID3D12Resource* resource, DXGI_FORMAT format, bool array,
                   D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = format;
        uav.ViewDimension = array ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY
                                  : D3D12_UAV_DIMENSION_TEXTURE2D;
        if (array) uav.Texture2DArray.ArraySize = CascadeCount;
        g_dx12.device->CreateUnorderedAccessView(resource, nullptr, &uav, handle);
    }
    void CreateBufferUAV(ID3D12Resource* resource,
                         D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = MaxHeightQueries;
        uav.Buffer.StructureByteStride = sizeof(XMFLOAT4);
        g_dx12.device->CreateUnorderedAccessView(
            resource, nullptr, &uav, handle);
    }

    void ConsumeHeightQueries(UINT slot) {
        completedQueryCount_ = 0;
        if (!querySlotValid_[slot]) return;
        const UINT count = querySlotCounts_[slot];
        const SIZE_T bytes = static_cast<SIZE_T>(count) * sizeof(XMFLOAT4);
        const D3D12_RANGE readRange = {0, bytes};
        XMFLOAT4* samples = nullptr;
        if (FAILED(heightQueryReadback_[slot]->Map(
                0, &readRange, reinterpret_cast<void**>(&samples)))) return;
        completedQueryCount_ = count;
        for (UINT i = 0; i < count; ++i) {
            completedQueries_[i].objectId = querySlotIds_[slot][i];
            completedQueries_[i].height = samples[i].x;
            completedQueries_[i].slope = {samples[i].y, samples[i].z};
        }
        const D3D12_RANGE writtenRange = {0, 0};
        heightQueryReadback_[slot]->Unmap(0, &writtenRange);
        querySlotValid_[slot] = false;
    }

    void DispatchHeightQueries(ID3D12GraphicsCommandList* list, UINT slot,
                               float time, float surfaceY,
                               ProfilerDX12* profiler) {
        const UINT count = (std::min)(pendingQueryCount_, MaxHeightQueries);
        if (count == 0) return;
        std::array<WaterInteraction, MaxHeightQueries> inputs{};
        for (UINT i = 0; i < count; ++i) {
            const WaterHeightQuery& query =
                pendingQueries_[(pendingQueryHead_ + i) % MaxHeightQueries];
            querySlotIds_[slot][i] = query.objectId;
            inputs[i].worldXZ = query.worldXZ;
        }
        pendingQueryHead_ = (pendingQueryHead_ + count) % MaxHeightQueries;
        pendingQueryCount_ -= count;
        querySlotCounts_[slot] = count;
        querySlotValid_[slot] = true;

        WriteHeightQuerySet(slot);
        const UINT event = profiler
            ? profiler->BeginGpuEvent("Water Height Queries", list) : UINT_MAX;
        list->SetPipelineState(heightQueryPSO_.Get());
        ComputeConstants constants = BuildConstants(
            0.0f, time, false, false, inputs.data(), count);
        constants.dispatch.w = surfaceY;
        BindConstants(list, constants, 5);
        const UINT base =
            (DescriptorSetCount + slot) * DescriptorsPerSet;
        list->SetComputeRootDescriptorTable(1, Gpu(base));
        list->SetComputeRootDescriptorTable(2, Gpu(base + 3));
        list->Dispatch((count + 15) / 16, 1, 1);
        UAVBarrier(list, heightQueryOutput_[slot].Get());
        Transition(list, heightQueryOutput_[slot].Get(),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(heightQueryReadback_[slot].Get(), 0,
                               heightQueryOutput_[slot].Get(), 0,
                               count * sizeof(XMFLOAT4));
        Transition(list, heightQueryOutput_[slot].Get(),
                   D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (profiler) profiler->EndGpuEvent(event, list);
    }

    ComputeConstants BuildConstants(
        float dt, float time, bool reset, bool applyInteractions,
        const WaterInteraction* events, UINT eventCount) const {
        ComputeConstants result = {};
        result.bounds = bounds_;
        result.simulation = {static_cast<float>(coastResolution_),
            static_cast<float>(bathymetryResolution_), dt, time};
        result.dispatch = {reset ? 1.0f : 0.0f,
            applyInteractions ? 1.0f : 0.0f,
            static_cast<float>((std::min)(eventCount, MaxInteractions)),
            static_cast<float>(frameParity_)};
        result.tuning0 = {tuning_.waveHeight, tuning_.waveScale,
            tuning_.waveSpeed, tuning_.directionRadians};
        result.tuning1 = {tuning_.choppiness, tuning_.surfStrength,
            tuning_.foamStrength, tuning_.coastDamping};
        for (size_t i = 0; i < waves_.size(); ++i) {
            result.spectralWaves[i] = {waves_[i].direction.x,
                waves_[i].direction.y, waves_[i].amplitude,
                waves_[i].wavelength};
            result.spectralExtra[i] = {
                waves_[i].phase, waves_[i].steepness, 0.0f, 0.0f};
        }
        for (UINT i = 0; i < eventCount && i < MaxInteractions; ++i) {
            result.interactions[i].positionRadius = {events[i].worldXZ.x,
                events[i].worldXZ.y, events[i].radius,
                events[i].heightImpulse};
            result.interactions[i].velocityType = {
                events[i].velocityImpulse.x, events[i].velocityImpulse.y,
                static_cast<float>(events[i].type), 0.0f};
        }
        return result;
    }

    void BindConstants(ID3D12GraphicsCommandList* list,
                       const ComputeConstants& constants, UINT dispatchIndex) {
        const UINT slot = g_dx12.frameIndex % FRAME_COUNT;
        const UINT64 offset = static_cast<UINT64>(constantStride_) *
            (slot * DispatchesPerFrame + dispatchIndex);
        std::memcpy(mappedConstants_ + offset, &constants, sizeof(constants));
        list->SetComputeRootConstantBufferView(
            0, constantBuffer_->GetGPUVirtualAddress() + offset);
    }
    void BindSet(ID3D12GraphicsCommandList* list, UINT coastRead,
                 UINT spectrumWrite) const {
        const UINT set = coastRead * 2 + spectrumWrite;
        list->SetComputeRootDescriptorTable(1, Gpu(set * DescriptorsPerSet));
        list->SetComputeRootDescriptorTable(
            2, Gpu(set * DescriptorsPerSet + 3));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index) const {
        auto handle = heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * descriptorSize_;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index) const {
        auto handle = heap_->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * descriptorSize_;
        return handle;
    }
    static void Transition(ID3D12GraphicsCommandList* list,
                           ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after) {
        if (!resource || before == after) return;
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }
    static void UAVBarrier(ID3D12GraphicsCommandList* list,
                           ID3D12Resource* resource) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        list->ResourceBarrier(1, &barrier);
    }

    bool initialized_ = false;
    bool ready_ = false;
    bool resetCoast_ = true;
    bool forceBathymetryRefresh_ = false;
    std::string failureReason_;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> spectrumPSO_;
    ComPtr<ID3D12PipelineState> coastPSO_;
    ComPtr<ID3D12PipelineState> heightQueryPSO_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    ComPtr<ID3D12Resource> constantBuffer_;
    uint8_t* mappedConstants_ = nullptr;
    UINT constantStride_ = 0;
    UINT descriptorSize_ = 0;
    std::array<ComPtr<ID3D12Resource>, 2> spectrum_;
    std::array<ComPtr<ID3D12Resource>, 2> coast_;
    ComPtr<ID3D12Resource> bathymetry_;
    ComPtr<ID3D12Resource> bathymetryUpload_;
    std::array<RetiredBathymetryResources, FRAME_COUNT> retiredBathymetry_;
    std::array<ComPtr<ID3D12Resource>, FRAME_COUNT> heightQueryOutput_;
    std::array<ComPtr<ID3D12Resource>, FRAME_COUNT> heightQueryReadback_;
    std::array<UltraSpectralWave, 16> waves_{};
    WaterInteractionRing<128> interactions_;
    std::array<WaterHeightQuery, MaxHeightQueries> pendingQueries_{};
    UINT pendingQueryHead_ = 0;
    UINT pendingQueryCount_ = 0;
    std::array<std::array<uint64_t, MaxHeightQueries>, FRAME_COUNT>
        querySlotIds_{};
    std::array<UINT, FRAME_COUNT> querySlotCounts_{};
    std::array<bool, FRAME_COUNT> querySlotValid_{};
    std::array<WaterHeightResult, MaxHeightQueries> completedQueries_{};
    UINT completedQueryCount_ = 0;
    WaterBathymetryDesc pendingDesc_;
    std::vector<float> pendingHeights_;
    XMFLOAT4 bounds_ = {};
    UINT bathymetryResolution_ = 0;
    UINT coastResolution_ = 0;
    UINT frameParity_ = 0;
    UINT coastReadIndex_ = 0;
    uint64_t terrainRevision_ = 0;
    float accumulatedTime_ = 0.0f;
    UltraWaterTuning tuning_{};
};
