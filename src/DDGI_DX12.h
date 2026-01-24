#ifndef DDGI_DX12_H
#define DDGI_DX12_H

#include "DX12Core.h"
#include <DirectXMath.h>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

extern DX12Context g_dx12;

struct DDGIConfigDX12 {
    int probeCountX = 8;
    int probeCountY = 4;
    int probeCountZ = 8;
    float probeSpacing = 2.0f;
    XMFLOAT3 probeGridOrigin = XMFLOAT3(-7.0f, 0.5f, -7.0f);
    
    int irradianceTexWidth = 8;   // Per probe
    int irradianceTexHeight = 8;
    int visibilityTexWidth = 16;  // Per probe
    int visibilityTexHeight = 16;
    
    bool enabled = true;
    float giIntensity = 1.0f;
    float normalBias = 0.1f;
};

class DDGIRendererDX12 {
public:
    DDGIConfigDX12 config;
    
    // Resources
    ComPtr<ID3D12Resource> irradianceTexture;
    ComPtr<ID3D12Resource> visibilityTexture;
    
    // Descriptors
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    
    int frameCount = 0;
    
    bool init(ID3D12Device* device) {
        if (!CreateTextures(device)) return false;
        if (!CreateDescriptors(device)) return false;
        return true;
    }
    
    bool CreateTextures(ID3D12Device* device) {
        // Calculate atlas size
        int totalProbes = config.probeCountX * config.probeCountY * config.probeCountZ;
        int width = (int)sqrt(totalProbes);
        int height = (totalProbes + width - 1) / width;
        
        int texWidthIrradiance = width * (config.irradianceTexWidth + 2); // +2 for border
        int texHeightIrradiance = height * (config.irradianceTexHeight + 2);
        
        int texWidthVisibility = width * (config.visibilityTexWidth + 2);
        int texHeightVisibility = height * (config.visibilityTexHeight + 2);
        
        // Create Irradiance Texture
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = texWidthIrradiance;
        texDesc.Height = texHeightIrradiance;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texDesc.SampleDesc.Count = 1;
        // Allow UAV (for compute) and RTV (for clearing/raster)
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        
        // Use optimized clear value
        D3D12_CLEAR_VALUE clearVal = {};
        clearVal.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        clearVal.Color[0] = 0.0f; clearVal.Color[1] = 0.0f; clearVal.Color[2] = 0.0f; clearVal.Color[3] = 1.0f;
        
        HRESULT hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearVal,
            IID_PPV_ARGS(&irradianceTexture));
            
        if (FAILED(hr)) return false;
        
        // Create Visibility Texture
        texDesc.Width = texWidthVisibility;
        texDesc.Height = texHeightVisibility;
        texDesc.Format = DXGI_FORMAT_R16G16_FLOAT; // Depth + DistanceSq
        
        clearVal.Format = DXGI_FORMAT_R16G16_FLOAT;
        clearVal.Color[0] = 1000.0f; clearVal.Color[1] = 1000000.0f; clearVal.Color[2] = 0.0f; clearVal.Color[3] = 0.0f;
        
        hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearVal,
            IID_PPV_ARGS(&visibilityTexture));

        return SUCCEEDED(hr);
    }

    bool CreateDescriptors(ID3D12Device* device) {
        // Create RTV Heap (2 descriptors: Irradiance and Visibility)
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = 2;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)))) return false;

        // Create RTVs
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(irradianceTexture.Get(), nullptr, rtvHandle);

        rtvHandle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(visibilityTexture.Get(), nullptr, rtvHandle);

        // Create SRV Heap (2 descriptors)
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = 2;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; 
        if (FAILED(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)))) return false;

        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();

        // Irradiance SRV
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(irradianceTexture.Get(), &srvDesc, srvHandle);

        // Visibility SRV
        srvHandle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        device->CreateShaderResourceView(visibilityTexture.Get(), &srvDesc, srvHandle);

        return true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetIrradianceSRV() {
        return srvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetVisibilitySRV() {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return handle;
    }

    void UpdateProbes(ID3D12GraphicsCommandList* cmdList, XMFLOAT3 color) {
        frameCount++;
        if (frameCount % 30 != 0) return;

        // Transition to RTV
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = irradianceTexture.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = visibilityTexture.Get();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        cmdList->ResourceBarrier(2, barriers);

        // Clear
        float clearColor[4] = { color.x, color.y, color.z, 1.0f };
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        rtvHandle.ptr += g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        float clearVis[4] = { 1000.0f, 1000000.0f, 0.0f, 0.0f };
        cmdList->ClearRenderTargetView(rtvHandle, clearVis, 0, nullptr);

        // Transition back to SRV
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        cmdList->ResourceBarrier(2, barriers);
    }
};

#endif

