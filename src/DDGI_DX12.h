#ifndef DDGI_DX12_H
#define DDGI_DX12_H

#include "DX12Core.h"
#include <DirectXMath.h>
#include <vector>
#include <iostream>

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

// Define struct for Main Light passed to Compute Shader
struct alignas(256) DDGIMainLightData {
    XMFLOAT3 lightPos;
    int lightType;             // 0=Point, 1=Directional
    
    XMFLOAT3 lightColor;
    float intensity;           // Reuse constant/linear/quad if needed, or just single intensity
    
    XMMATRIX lightSpaceMatrix; // For shadow mapping
    
    float shadowBias;
    int enableShadows;
    float padding[2];
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
    
    // External resources
    ID3D12Resource* shadowMapResource = nullptr;
    
    // Constant Buffers
    UploadBuffer<DDGIMainLightData> mainLightUploadBuffer;

    int frameCount = 0;
    
    // Compute pipeline
    ComPtr<ID3D12RootSignature> computeRootSignature;
    ComPtr<ID3D12PipelineState> computePSO;
    bool computeInitialized = false;

    bool init(ID3D12Device* device) {
        std::cout << "DDGI_DX12: Initializing..." << std::endl;
        if (!CreateTextures(device)) {
            std::cerr << "DDGI_DX12: Failed to create textures." << std::endl;
            return false;
        }
        if (!CreateDescriptors(device)) {
            std::cerr << "DDGI_DX12: Failed to create descriptors." << std::endl;
            return false;
        }
        if (!InitCompute(device)) {
            std::cerr << "DDGI_DX12: Failed to initialize compute shader." << std::endl;
            return false;
        }
        std::cout << "DDGI_DX12: Initialization complete." << std::endl;
        return true;
    }

    bool InitCompute(ID3D12Device* device) {
        std::cout << "DDGI_DX12: InitCompute started." << std::endl;
        
        // Create upload buffer for main light
        if (!mainLightUploadBuffer.Create(3)) { // 3 frames in flight buffer
            std::cerr << "DDGI_DX12: Failed to create main light upload buffer." << std::endl;
            return false;
        }

        // Root signature
        D3D12_ROOT_PARAMETER rootParams[4] = {}; // Increased to 4

        // b0: DDGI Constants
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[0].Descriptor.ShaderRegister = 0;
        rootParams[0].Descriptor.RegisterSpace = 0;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // b1: Light Buffer
        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[1].Descriptor.ShaderRegister = 1;
        rootParams[1].Descriptor.RegisterSpace = 0;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // b2: Main Light Buffer
        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[2].Descriptor.ShaderRegister = 2;
        rootParams[2].Descriptor.RegisterSpace = 0;
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // u0: Output Texture (Descriptor Table)
        // t0: Shadow Map (Descriptor Table)
        // We will combine these into one range or use separate ranges? 
        // For simplicity, let's use two tables for flexibility with heap slots.
        // Or actually, we serve from one heap.
        // Heap Layout:
        // 0: Irradiance SRV
        // 1: Visibility SRV
        // 2: Irradiance UAV (u0)
        // 3: Shadow Map SRV (t0)
        
        // Let's make param 3 be a TABLE for u0 and t0.
        // But u0 and t0 might need to be in different ranges.
        
        D3D12_DESCRIPTOR_RANGE ranges[2];
        
        // Range 0: UAV (u0)
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 2; // Offset in heap 
        // Wait, OffsetInDescriptorsFromTableStart behaves differently depending on table binding.
        // If we bind the start of the heap, we can use offsets.
        
        // Range 1: SRV (t0) -> Shadow Map
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 3; // Offset in heap

        rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[3].DescriptorTable.NumDescriptorRanges = 2;
        rootParams[3].DescriptorTable.pDescriptorRanges = ranges;
        rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = 4;
        rootSigDesc.pParameters = rootParams;
        
        // Add static sampler for shadow map
        D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; // Simple point sampling or LINEAR
        samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplerDesc.MipLODBias = 0;
        samplerDesc.MaxAnisotropy = 0;
        samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE; // Far depth ideally
        samplerDesc.MinLOD = 0.0f;
        samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
        samplerDesc.ShaderRegister = 0;
        samplerDesc.RegisterSpace = 0;
        samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        
        rootSigDesc.NumStaticSamplers = 1;
        rootSigDesc.pStaticSamplers = &samplerDesc;
        
        rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> signature, error;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
        if (FAILED(hr)) {
            std::cerr << "DDGI_DX12: Failed to serialize root signature." << std::endl;
            if (error) std::cerr << (char*)error->GetBufferPointer() << std::endl;
            return false;
        }

        hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&computeRootSignature));
        if (FAILED(hr)) {
            std::cerr << "DDGI_DX12: CreateRootSignature failed." << std::endl;
            return false;
        }

        // Load shader
        ComPtr<ID3DBlob> csBlob, csError;
        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        // Assume shaders are in "shaders/"
        std::cout << "DDGI_DX12: Compiling compute shader: shaders/ddgi_update_cs.hlsl" << std::endl;
        hr = D3DCompileFromFile(L"shaders/ddgi_update_cs.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0", compileFlags, 0, &csBlob, &csError);
        if (FAILED(hr)) {
             std::cerr << "DDGI_DX12: Failed to compile compute shader." << std::endl;
             if (csError) {
                 std::cerr << "Shader Error: " << (char*)csError->GetBufferPointer() << std::endl;
                 OutputDebugStringA((char*)csError->GetBufferPointer());
             }
             return false;
        }
        std::cout << "DDGI_DX12: Compute shader compiled successfully." << std::endl;

        // PSO
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = computeRootSignature.Get();
        psoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };

        hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&computePSO));
        if (FAILED(hr)) {
            std::cerr << "DDGI_DX12: CreateComputePipelineState failed." << std::endl;
            return false;
        }
        
        // Create UAV descriptor in the heap
        // Note: srvHeap is CBV_SRV_UAV. We used slots 0 and 1 for SRVs (Irradiance, Visibility)
        // Let's use slot 2 for UAV Irradiance
        // But GetIrradianceSRV() uses heap start.
        // We need a free slot.
        // Wait, srvHeap was size 2 in CreateDescriptors.
        // We need to increase heap size.

        computeInitialized = true;
        std::cout << "DDGI_DX12: InitCompute successful." << std::endl;
        return true;
    }
    
    bool CreateTextures(ID3D12Device* device) {
        std::cout << "DDGI_DX12: Creating textures..." << std::endl;
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
            
        if (FAILED(hr)) {
            std::cerr << "DDGI_DX12: Failed to create irradiance texture." << std::endl;
            return false;
        }
        
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

        if (FAILED(hr)) {
            std::cerr << "DDGI_DX12: Failed to create visibility texture." << std::endl;
            return false;
        }

        std::cout << "DDGI_DX12: Textures created successfully." << std::endl;
        return true;
    }

    bool CreateDescriptors(ID3D12Device* device) {
        std::cout << "DDGI_DX12: Creating descriptors..." << std::endl;
        // Create RTV Heap (2 descriptors: Irradiance and Visibility)
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = 2;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)))) {
            std::cerr << "DDGI_DX12: Failed to create RTV heap." << std::endl;
            return false;
        }

        // Create RTVs
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(irradianceTexture.Get(), nullptr, rtvHandle);

        rtvHandle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(visibilityTexture.Get(), nullptr, rtvHandle);

        // Create SRV Heap (Increase to 4 descriptors: Irradiance SRV, Visibility SRV, Irradiance UAV, ShadowMap SRV)
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = 4;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; 
        if (FAILED(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)))) {
            std::cerr << "DDGI_DX12: Failed to create SRV heap." << std::endl;
            return false;
        }

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

        // Irradiance UAV
        srvHandle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;
        device->CreateUnorderedAccessView(irradianceTexture.Get(), nullptr, &uavDesc, srvHandle);

        // Shadow Map SRV (t0) - will be filled in by RegisterShadowMap
        // Create a null descriptor as placeholder
        srvHandle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
        nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(nullptr, &nullSrvDesc, srvHandle);

        std::cout << "DDGI_DX12: Descriptors created successfully." << std::endl;
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

    D3D12_GPU_DESCRIPTOR_HANDLE GetIrradianceUAV_GPU() {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = srvHeap->GetGPUDescriptorHandleForHeapStart();
        UINT inc = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        handle.ptr += 2 * inc;
        return handle;
    }

    void RegisterShadowMap(ID3D12Resource* resource) {
        shadowMapResource = resource;
        if (srvHeap && resource) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += 3 * g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT; // Assuming Depth texture
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(resource, &srvDesc, handle);
        }
    }

    void UpdateProbes(ID3D12GraphicsCommandList* cmdList, D3D12_GPU_VIRTUAL_ADDRESS lightBufferAddr, int numLights, D3D12_GPU_VIRTUAL_ADDRESS ddgiCBAddr, const DDGIMainLightData& mainLightData) {
        if (!computeInitialized) {
            static bool printedOnce = false;
            if (!printedOnce) {
                std::cout << "DDGI_DX12: UpdateProbes called but computeInitialized=false!" << std::endl;
                printedOnce = true;
            }
            return;
        }
        frameCount++;
        
        // Print debug info once
        static bool printedDebug = false;
        if (!printedDebug) {
            D3D12_RESOURCE_DESC desc = irradianceTexture->GetDesc();
            std::cout << "DDGI_DX12: UpdateProbes running!" << std::endl;
            std::cout << "  - Irradiance texture size: " << desc.Width << "x" << desc.Height << std::endl;
            std::cout << "  - Dispatch groups: " << ((desc.Width + 7) / 8) << "x" << ((desc.Height + 7) / 8) << std::endl;
            std::cout << "  - Num lights: " << numLights << std::endl;
            std::cout << "  - Main light color: " << mainLightData.lightColor.x << ", " << mainLightData.lightColor.y << ", " << mainLightData.lightColor.z << std::endl;
            printedDebug = true;
        }
        
        // Update Main Light Buffer
        mainLightUploadBuffer.CopyData(g_dx12.frameIndex, mainLightData);
        D3D12_GPU_VIRTUAL_ADDRESS mainLightCBAddr = mainLightUploadBuffer.GetGPUAddress(g_dx12.frameIndex);

        // Transition to UAV
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = irradianceTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);

        cmdList->SetPipelineState(computePSO.Get());
        cmdList->SetComputeRootSignature(computeRootSignature.Get());

        // Bind resources
        ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        cmdList->SetComputeRootConstantBufferView(0, ddgiCBAddr);
        cmdList->SetComputeRootConstantBufferView(1, lightBufferAddr);
        cmdList->SetComputeRootConstantBufferView(2, mainLightCBAddr);
        cmdList->SetComputeRootDescriptorTable(3, srvHeap->GetGPUDescriptorHandleForHeapStart());

        // Dispatch
        // Calculate groups
        D3D12_RESOURCE_DESC desc = irradianceTexture->GetDesc();
        UINT width = (UINT)desc.Width;
        UINT height = (UINT)desc.Height;
        
        cmdList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

        // Transition back
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmdList->ResourceBarrier(1, &barrier);
    }
    
    // Overload for compatibility/transition (can be removed later)
    void UpdateProbes(ID3D12GraphicsCommandList* cmdList, XMFLOAT3 color) {
         // Do nothing or call legacy clear
    }
};

#endif












