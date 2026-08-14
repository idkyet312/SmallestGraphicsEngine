#ifndef CLOUD_NOISE_DX12_H
#define CLOUD_NOISE_DX12_H

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>

// Bakes the two 3D noise volumes the cloud raymarch samples, once at startup.
//
// Generated on the GPU rather than shipped as assets: the volumes are ~8 MB
// uncompressed together, they are pure functions of the generator shader, and
// baking them takes a few milliseconds of compute at boot. Shipping them as
// files would add to a package that is already large for no benefit.
//
// Following Schneider & Vos (SIGGRAPH 2015):
//   Shape 128^3 RGBA16F -- R Perlin-Worley, GBA Worley at rising frequencies.
//   Detail 32^3 RGBA16F -- RGB Worley at rising frequencies.
class CloudNoiseDX12 {
public:
    static constexpr UINT ShapeResolution = 128;
    static constexpr UINT DetailResolution = 32;
    bool initialized = false;

    // Baked once. The raymarch samples these every frame but nothing ever
    // writes them again, so after Generate they live in PIXEL_SHADER_RESOURCE
    // for the life of the process.
    ID3D12Resource* ShapeVolume() const { return shape_.Get(); }
    ID3D12Resource* DetailVolume() const { return detail_.Get(); }

    bool Init() {
        std::ifstream shaderFile("shaders/cloud_noise_gen.hlsl");
        if (!shaderFile) return false;
        std::stringstream shaderText;
        shaderText << shaderFile.rdbuf();
        const std::string source = shaderText.str();

        ComPtr<ID3DBlob> cs, errors;
        const UINT flags =
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        if (FAILED(ShaderCacheDX12::CompileCached(
                source.data(), source.size(), "cloud_noise_gen.hlsl",
                nullptr, nullptr, "CSMain", "cs_5_0", flags, 0, &cs, &errors))) {
            if (errors)
                std::cerr << static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }

        // u0 for the volume being written, plus the resolution/mode constants.
        D3D12_DESCRIPTOR_RANGE uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER roots[2] = {};
        roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[0].DescriptorTable.NumDescriptorRanges = 1;
        roots[0].DescriptorTable.pDescriptorRanges = &uavRange;
        roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        roots[1].Constants.ShaderRegister = 0;
        roots[1].Constants.Num32BitValues = 4;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = roots;
        ComPtr<ID3DBlob> serialized;
        if (FAILED(D3D12SerializeRootSignature(
                &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
            return false;
        if (FAILED(g_dx12.device->CreateRootSignature(
                0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(&rootSignature_))))
            return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC compute = {};
        compute.pRootSignature = rootSignature_.Get();
        compute.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateComputePipelineState(
                &compute, IID_PPV_ARGS(&pipeline_))))
            return false;

        // Two UAVs to write with, two SRVs for the raymarch to read.
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 4;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&heap_))))
            return false;
        descriptorSize_ = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        if (!CreateVolume(ShapeResolution, shape_, 0, 2)) return false;
        if (!CreateVolume(DetailResolution, detail_, 1, 3)) return false;
        initialized = true;
        return true;
    }

    // Runs the two dispatches. Called once, on a command list the caller
    // executes and waits on before the first frame samples the volumes.
    void Generate(ID3D12GraphicsCommandList* commandList) {
        if (!initialized || generated_ || !commandList) return;
        ID3D12DescriptorHeap* heaps[] = { heap_.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetComputeRootSignature(rootSignature_.Get());
        commandList->SetPipelineState(pipeline_.Get());

        Dispatch(commandList, shape_.Get(), ShapeResolution, 0, 0);
        Dispatch(commandList, detail_.Get(), DetailResolution, 1, 1);
        generated_ = true;
    }

    bool Generated() const { return generated_; }

    D3D12_GPU_DESCRIPTOR_HANDLE ShapeSRV() const { return GpuHandle(2); }
    D3D12_GPU_DESCRIPTOR_HANDLE DetailSRV() const { return GpuHandle(3); }
    ID3D12DescriptorHeap* Heap() const { return heap_.Get(); }

private:
    void Dispatch(ID3D12GraphicsCommandList* commandList,
                  ID3D12Resource* volume, UINT resolution, UINT uavIndex,
                  UINT isDetail) {
        D3D12_RESOURCE_BARRIER toUAV = {};
        toUAV.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUAV.Transition.pResource = volume;
        toUAV.Transition.StateBefore =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toUAV.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toUAV.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &toUAV);

        commandList->SetComputeRootDescriptorTable(0, GpuHandle(uavIndex));
        const UINT constants[4] = { resolution, isDetail, 0, 0 };
        commandList->SetComputeRoot32BitConstants(1, 4, constants, 0);
        const UINT groups = (resolution + 7) / 8;
        commandList->Dispatch(groups, groups, groups);

        D3D12_RESOURCE_BARRIER toSRV = toUAV;
        toSRV.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toSRV.Transition.StateAfter =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commandList->ResourceBarrier(1, &toSRV);
    }

    bool CreateVolume(UINT resolution, ComPtr<ID3D12Resource>& target,
                      UINT uavIndex, UINT srvIndex) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC volume = {};
        volume.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        volume.Width = resolution;
        volume.Height = resolution;
        volume.DepthOrArraySize = static_cast<UINT16>(resolution);
        volume.MipLevels = 1;
        volume.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        volume.SampleDesc.Count = 1;
        volume.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        volume.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &volume,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&target))))
            return false;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = volume.Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uav.Texture3D.WSize = resolution;
        g_dx12.device->CreateUnorderedAccessView(
            target.Get(), nullptr, &uav, CpuHandle(uavIndex));

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = volume.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(
            target.Get(), &srv, CpuHandle(srvIndex));
        return true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * descriptorSize_;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(UINT index) const {
        D3D12_GPU_DESCRIPTOR_HANDLE handle =
            heap_->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * descriptorSize_;
        return handle;
    }

    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipeline_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    ComPtr<ID3D12Resource> shape_;
    ComPtr<ID3D12Resource> detail_;
    UINT descriptorSize_ = 0;
    bool generated_ = false;
};

#endif
