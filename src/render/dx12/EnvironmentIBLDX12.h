#ifndef ENVIRONMENT_IBL_DX12_H
#define ENVIRONMENT_IBL_DX12_H

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include <d3dcompiler.h>
#include <fstream>
#include <iostream>
#include <sstream>

class EnvironmentIBLDX12 {
public:
    Microsoft::WRL::ComPtr<ID3D12Resource> prefilteredEnvironment;
    Microsoft::WRL::ComPtr<ID3D12Resource> brdfIntegrationLUT;

    bool Init(ID3D12Resource* sourceEnvironment,
              float environmentRotationRadians = 0.0f) {
        if (!sourceEnvironment || !g_dx12.device || !g_dx12.commandList)
            return false;
        const D3D12_RESOURCE_DESC sourceDesc = sourceEnvironment->GetDesc();
        mipLevels_ = sourceDesc.MipLevels;
        if (mipLevels_ == 0) return false;

        if (!CreateResources(sourceDesc) || !CreatePipelines() ||
            !CreateDescriptors(sourceEnvironment, sourceDesc))
            return false;

        ID3D12GraphicsCommandList* list = g_dx12.commandList.Get();
        D3D12_RESOURCE_BARRIER sourceBarrier = {};
        sourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        sourceBarrier.Transition.pResource = sourceEnvironment;
        sourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        sourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        sourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &sourceBarrier);

        ID3D12DescriptorHeap* heaps[] = { descriptorHeap_.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(prefilterRootSignature_.Get());
        list->SetPipelineState(prefilterPipeline_.Get());
        const UINT descriptorSize = g_dx12.cbvSrvUavDescriptorSize;
        D3D12_GPU_DESCRIPTOR_HANDLE base =
            descriptorHeap_->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(0, base);

        for (UINT mip = 0; mip < mipLevels_; ++mip) {
            const UINT width = (std::max)(1u, static_cast<UINT>(sourceDesc.Width) >> mip);
            const UINT height = (std::max)(1u, sourceDesc.Height >> mip);
            const float roughness = mipLevels_ > 1
                ? static_cast<float>(mip) / static_cast<float>(mipLevels_ - 1)
                : 0.0f;
            const UINT samples = mip == 0 ? 1u : (mip < 3 ? 64u : 128u);
            const UINT constants[5] = {
                width, height, FloatBits(roughness), samples,
                FloatBits(environmentRotationRadians)
            };
            D3D12_GPU_DESCRIPTOR_HANDLE output = base;
            output.ptr += static_cast<UINT64>(descriptorSize) * (1u + mip);
            list->SetComputeRootDescriptorTable(1, output);
            list->SetComputeRoot32BitConstants(2, 5, constants, 0);
            list->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);
            D3D12_RESOURCE_BARRIER uav = {};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav.UAV.pResource = prefilteredEnvironment.Get();
            list->ResourceBarrier(1, &uav);
        }

        list->SetComputeRootSignature(brdfRootSignature_.Get());
        list->SetPipelineState(brdfPipeline_.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE brdf = base;
        brdf.ptr += static_cast<UINT64>(descriptorSize) * (1u + mipLevels_);
        list->SetComputeRootDescriptorTable(0, brdf);
        const UINT brdfConstants[5] = {
            BrdfSize, BrdfSize, FloatBits(0.0f), 256u, FloatBits(0.0f)
        };
        list->SetComputeRoot32BitConstants(1, 5, brdfConstants, 0);
        list->Dispatch((BrdfSize + 7u) / 8u, (BrdfSize + 7u) / 8u, 1u);

        D3D12_RESOURCE_BARRIER finished[2] = {};
        for (UINT i = 0; i < 2; ++i) {
            finished[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            finished[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            finished[i].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            finished[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        finished[0].Transition.pResource = prefilteredEnvironment.Get();
        finished[1].Transition.pResource = brdfIntegrationLUT.Get();
        list->ResourceBarrier(2, finished);
        initialized_ = true;
        return true;
    }

    bool IsInitialized() const { return initialized_; }

private:
    static constexpr UINT BrdfSize = 256;
    UINT mipLevels_ = 0;
    bool initialized_ = false;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> prefilterRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> brdfRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> prefilterPipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> brdfPipeline_;

    static UINT FloatBits(float value) {
        UINT bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    bool CreateTexture(UINT64 width, UINT height, UINT16 mips, DXGI_FORMAT format,
                       Microsoft::WRL::ComPtr<ID3D12Resource>& resource) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = mips;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(g_dx12.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&resource)));
    }

    bool CreateResources(const D3D12_RESOURCE_DESC& sourceDesc) {
        return CreateTexture(sourceDesc.Width, sourceDesc.Height, mipLevels_,
                             DXGI_FORMAT_R32G32B32A32_FLOAT,
                             prefilteredEnvironment) &&
               CreateTexture(BrdfSize, BrdfSize, 1, DXGI_FORMAT_R32G32_FLOAT,
                             brdfIntegrationLUT);
    }

    bool Compile(const std::string& source, const char* entry,
                 Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = ShaderCacheDX12::CompileCached(
            source.data(), source.size(), "environment_ibl_cs.hlsl", nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &blob, &errors);
        if (FAILED(result) && errors)
            std::cerr << static_cast<const char*>(errors->GetBufferPointer());
        return SUCCEEDED(result);
    }

    bool CreateRootSignature(bool prefilter) {
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        D3D12_ROOT_PARAMETER params[3] = {};
        UINT paramCount = 0;
        if (prefilter) {
            ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[0].NumDescriptors = 1;
            ranges[0].BaseShaderRegister = 0;
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[0].DescriptorTable.NumDescriptorRanges = 1;
            params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
            ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            ranges[1].NumDescriptors = 1;
            ranges[1].BaseShaderRegister = 0;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1].DescriptorTable.NumDescriptorRanges = 1;
            params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
            params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[2].Constants.ShaderRegister = 0;
            params[2].Constants.Num32BitValues = 5;
            paramCount = 3;
        } else {
            ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            ranges[0].NumDescriptors = 1;
            ranges[0].BaseShaderRegister = 1;
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[0].DescriptorTable.NumDescriptorRanges = 1;
            params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[1].Constants.ShaderRegister = 0;
            params[1].Constants.Num32BitValues = 5;
            paramCount = 2;
        }

        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = paramCount;
        desc.pParameters = params;
        if (prefilter) {
            desc.NumStaticSamplers = 1;
            desc.pStaticSamplers = &sampler;
        }
        Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
        if (FAILED(D3D12SerializeRootSignature(
                &desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
            return false;
        auto& target = prefilter ? prefilterRootSignature_ : brdfRootSignature_;
        return SUCCEEDED(g_dx12.device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&target)));
    }

    bool CreatePipelines() {
        std::ifstream file("shaders/environment_ibl_cs.hlsl");
        if (!file.is_open()) return false;
        std::stringstream stream;
        stream << file.rdbuf();
        const std::string source = stream.str();
        Microsoft::WRL::ComPtr<ID3DBlob> prefilterShader, brdfShader;
        if (!Compile(source, "PrefilterEnvironmentCS", prefilterShader) ||
            !Compile(source, "IntegrateBRDFCS", brdfShader) ||
            !CreateRootSignature(true) || !CreateRootSignature(false))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = prefilterRootSignature_.Get();
        desc.CS = { prefilterShader->GetBufferPointer(), prefilterShader->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateComputePipelineState(
                &desc, IID_PPV_ARGS(&prefilterPipeline_)))) return false;
        desc.pRootSignature = brdfRootSignature_.Get();
        desc.CS = { brdfShader->GetBufferPointer(), brdfShader->GetBufferSize() };
        return SUCCEEDED(g_dx12.device->CreateComputePipelineState(
            &desc, IID_PPV_ARGS(&brdfPipeline_)));
    }

    bool CreateDescriptors(ID3D12Resource* source,
                           const D3D12_RESOURCE_DESC& sourceDesc) {
        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.NumDescriptors = 2u + mipLevels_;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &heap, IID_PPV_ARGS(&descriptorHeap_)))) return false;
        const UINT size = g_dx12.cbvSrvUavDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = sourceDesc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = sourceDesc.MipLevels;
        g_dx12.device->CreateShaderResourceView(source, &srv, handle);
        handle.ptr += size;
        for (UINT mip = 0; mip < mipLevels_; ++mip) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Texture2D.MipSlice = mip;
            g_dx12.device->CreateUnorderedAccessView(
                prefilteredEnvironment.Get(), nullptr, &uav, handle);
            handle.ptr += size;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC brdf = {};
        brdf.Format = DXGI_FORMAT_R32G32_FLOAT;
        brdf.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_dx12.device->CreateUnorderedAccessView(
            brdfIntegrationLUT.Get(), nullptr, &brdf, handle);
        return true;
    }
};

#endif
