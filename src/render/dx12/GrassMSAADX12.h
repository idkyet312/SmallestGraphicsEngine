#ifndef GRASS_MSAA_DX12_H
#define GRASS_MSAA_DX12_H

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include "MSAADX12.h"
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>

class GrassMSAADX12 {
public:
    bool initialized = false;

    bool Init(UINT width, UINT height) {
        if (!CreateDescriptorHeaps() || !CreateCompositePipeline()) return false;
        initialized = CreateTargets(width, height);
        return initialized;
    }

    bool Resize(UINT width, UINT height) {
        if (!initialized || width == 0 || height == 0) return false;
        colorTarget_.Reset();
        depthTarget_.Reset();
        combinedDepth_.Reset();
        width_ = width;
        height_ = height;
        readable_ = true;
        return CreateTargets(width, height);
    }

    void Begin(ID3D12GraphicsCommandList* commandList) {
        if (!initialized) return;
        if (readable_) {
            D3D12_RESOURCE_BARRIER barriers[2] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Transition.pResource = colorTarget_.Get();
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[1] = barriers[0];
            barriers[1].Transition.pResource = depthTarget_.Get();
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            commandList->ResourceBarrier(2, barriers);
            readable_ = false;
        }
        const float clear[4] = {};
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);
        commandList->ClearDepthStencilView(
            dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        commandList->RSSetViewports(1, &g_dx12.viewport);
        commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
    }

    void Composite(ID3D12GraphicsCommandList* commandList,
                   ID3D12Resource* sceneColor,
                   ID3D12Resource* sceneMotion,
                   ID3D12Resource* sceneDepth) {
        if (!initialized || !sceneColor || !sceneMotion || !sceneDepth) return;

        D3D12_RESOURCE_BARRIER barriers[5] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = colorTarget_.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = depthTarget_.Get();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[2] = barriers[0];
        barriers[2].Transition.pResource = sceneColor;
        barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[3] = barriers[2];
        barriers[3].Transition.pResource = sceneMotion;
        barriers[4] = barriers[2];
        barriers[4].Transition.pResource = combinedDepth_.Get();
        barriers[4].Transition.StateBefore =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commandList->ResourceBarrier(5, barriers);
        readable_ = true;

        UpdateDescriptors(sceneColor, sceneMotion, sceneDepth);
        ID3D12DescriptorHeap* heaps[] = { descriptorHeap_.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetComputeRootSignature(rootSignature_.Get());
        commandList->SetPipelineState(pipelineState_.Get());
        commandList->SetComputeRootDescriptorTable(
            0, descriptorHeap_->GetGPUDescriptorHandleForHeapStart());
        const UINT size[2] = { width_, height_ };
        commandList->SetComputeRoot32BitConstants(1, 2, size, 0);
        commandList->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);

        D3D12_RESOURCE_BARRIER uav[3] = {};
        for (UINT i = 0; i < 3; ++i) {
            uav[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav[i].UAV.pResource =
                i == 0 ? sceneColor :
                (i == 1 ? sceneMotion : combinedDepth_.Get());
        }
        commandList->ResourceBarrier(3, uav);
        std::swap(barriers[2].Transition.StateBefore,
                  barriers[2].Transition.StateAfter);
        std::swap(barriers[3].Transition.StateBefore,
                  barriers[3].Transition.StateAfter);
        std::swap(barriers[4].Transition.StateBefore,
                  barriers[4].Transition.StateAfter);
        commandList->ResourceBarrier(3, &barriers[2]);
    }

    ID3D12Resource* GetCombinedDepthResource() const {
        return combinedDepth_.Get();
    }

private:
    bool CreateDescriptorHeaps() {
        D3D12_DESCRIPTOR_HEAP_DESC rtv = {};
        rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv.NumDescriptors = 1;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &rtv, IID_PPV_ARGS(&rtvHeap_)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC dsv = rtv;
        dsv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &dsv, IID_PPV_ARGS(&dsvHeap_)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC descriptors = {};
        descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descriptors.NumDescriptors = 6;
        descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        return SUCCEEDED(g_dx12.device->CreateDescriptorHeap(
            &descriptors, IID_PPV_ARGS(&descriptorHeap_)));
    }

    bool CreateTargets(UINT width, UINT height) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS support = {};
        support.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        support.SampleCount = MSAADX12::SampleCount;
        if (FAILED(g_dx12.device->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                &support, sizeof(support))) || support.NumQualityLevels == 0)
            return false;

        width_ = width;
        height_ = height;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = MSAADX12::SampleCount;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE colorClear = {};
        colorClear.Format = desc.Format;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &colorClear,
                IID_PPV_ARGS(&colorTarget_)))) return false;
        D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
        rtv.Format = desc.Format;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        g_dx12.device->CreateRenderTargetView(
            colorTarget_.Get(), &rtv,
            rtvHeap_->GetCPUDescriptorHandleForHeapStart());

        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE depthClear = {};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &depthClear,
                IID_PPV_ARGS(&depthTarget_)))) return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        g_dx12.device->CreateDepthStencilView(
            depthTarget_.Get(), &dsv,
            dsvHeap_->GetCPUDescriptorHandleForHeapStart());

        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&combinedDepth_)))) return false;
        readable_ = true;
        return true;
    }

    bool CreateCompositePipeline() {
        std::ifstream file("shaders/grass_msaa_composite_cs.hlsl");
        if (!file.is_open()) return false;
        std::stringstream stream;
        stream << file.rdbuf();
        const std::string source = stream.str();
        ComPtr<ID3DBlob> shader, errors;
        if (FAILED(ShaderCacheDX12::CompileCached(source.data(), source.size(),
                "grass_msaa_composite_cs.hlsl", nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &shader, &errors))) {
            if (errors) std::cerr << "Grass MSAA composite: "
                                  << (char*)errors->GetBufferPointer() << '\n';
            return false;
        }

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 3;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 3;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 3;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;
        params[1].Constants.Num32BitValues = 2;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root = {};
        root.NumParameters = 2;
        root.pParameters = params;
        ComPtr<ID3DBlob> signature;
        if (FAILED(D3D12SerializeRootSignature(
                &root, D3D_ROOT_SIGNATURE_VERSION_1,
                &signature, &errors))) return false;
        if (FAILED(g_dx12.device->CreateRootSignature(
                0, signature->GetBufferPointer(), signature->GetBufferSize(),
                IID_PPV_ARGS(&rootSignature_)))) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = rootSignature_.Get();
        pso.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        return SUCCEEDED(g_dx12.device->CreateComputePipelineState(
            &pso, IID_PPV_ARGS(&pipelineState_)));
    }

    void UpdateDescriptors(ID3D12Resource* sceneColor,
                           ID3D12Resource* sceneMotion,
                           ID3D12Resource* sceneDepth) {
        const UINT stride = g_dx12.cbvSrvUavDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        g_dx12.device->CreateShaderResourceView(colorTarget_.Get(), &srv, handle);
        handle.ptr += stride;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        g_dx12.device->CreateShaderResourceView(depthTarget_.Get(), &srv, handle);
        handle.ptr += stride;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(sceneDepth, &srv, handle);
        handle.ptr += stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        g_dx12.device->CreateUnorderedAccessView(sceneColor, nullptr, &uav, handle);
        handle.ptr += stride;
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        g_dx12.device->CreateUnorderedAccessView(sceneMotion, nullptr, &uav, handle);
        handle.ptr += stride;
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        g_dx12.device->CreateUnorderedAccessView(
            combinedDepth_.Get(), nullptr, &uav, handle);
    }

    UINT width_ = 0;
    UINT height_ = 0;
    bool readable_ = true;
    ComPtr<ID3D12Resource> colorTarget_;
    ComPtr<ID3D12Resource> depthTarget_;
    ComPtr<ID3D12Resource> combinedDepth_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipelineState_;
};

#endif
