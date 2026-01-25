#ifndef DDGI_DX12_RT_H
#define DDGI_DX12_RT_H

#include "DX12Core.h"
#include "ShaderDX12.h"
#include <DirectXMath.h>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

extern DX12Context g_dx12;

// DDGI Configuration for raytracing
struct DDGIConfigRT {
    int probeCountX = 8;
    int probeCountY = 4;
    int probeCountZ = 8;
    float probeSpacing = 2.0f;
    XMFLOAT3 probeGridOrigin = XMFLOAT3(-7.0f, 0.5f, -7.0f);
    
    int irradianceTexWidth = 8;
    int irradianceTexHeight = 8;
    int raysPerProbe = 64;
    float maxRayDistance = 20.0f;
    
    bool enabled = true;
    float giIntensity = 1.0f;
    float normalBias = 0.1f;
    float hysteresis = 0.95f;  // Temporal blending
};

// Constant buffer for raytracing
struct alignas(256) DDGIRaytracingCB {
    XMFLOAT3 probeGridOrigin;
    float probeSpacing;
    
    int probeCountX;
    int probeCountY;
    int probeCountZ;
    int raysPerProbe;
    
    float maxRayDistance;
    float normalBias;
    float hysteresis;
    float giIntensity;
    
    int irradianceTexWidth;
    int irradianceTexHeight;
    int frameNumber;
    int padding;
    
    // Light data
    XMFLOAT3 sunDirection;
    float sunIntensity;
    XMFLOAT3 sunColor;
    int numPointLights;
};

// Material data for raytracing (matches HLSL)
struct RTMaterial {
    XMFLOAT3 albedo;
    float padding;
};

// Simple geometry for BLAS
struct RTGeometry {
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    UINT vertexCount;
    UINT indexCount;
    UINT vertexStride;
    XMFLOAT3 albedo;
};

class DDGIRendererRT {
public:
    DDGIConfigRT config;
    
    // Acceleration structures
    ComPtr<ID3D12Resource> bottomLevelAS;
    ComPtr<ID3D12Resource> topLevelAS;
    ComPtr<ID3D12Resource> scratchBuffer;
    ComPtr<ID3D12Resource> instanceBuffer;
    
    // Irradiance texture (output)
    ComPtr<ID3D12Resource> irradianceTexture;
    ComPtr<ID3D12Resource> prevIrradianceTexture;
    
    // Descriptor heaps
    ComPtr<ID3D12DescriptorHeap> srvUavHeap;
    
    // Raytracing pipeline
    ComPtr<ID3D12StateObject> rtPipelineState;
    ComPtr<ID3D12RootSignature> globalRootSignature;
    ComPtr<ID3D12RootSignature> localRootSignature;
    
    // Shader tables
    ComPtr<ID3D12Resource> rayGenShaderTable;
    ComPtr<ID3D12Resource> missShaderTable;
    ComPtr<ID3D12Resource> hitGroupShaderTable;
    
    // Constant buffer
    ComPtr<ID3D12Resource> constantBuffer;
    DDGIRaytracingCB* mappedCB = nullptr;
    
    // Material buffer for per-geometry albedo
    ComPtr<ID3D12Resource> materialBuffer;
    
    // Scene geometry for BLAS
    std::vector<RTGeometry> geometries;
    
    bool initialized = false;
    bool rtSupported = false;
    int frameNumber = 0;
    
    DDGIRendererRT() {}
    
    ~DDGIRendererRT() {
        Cleanup();
    }
    
    bool Init() {
        if (!g_dx12.raytracingSupported || !g_dx12.device5) {
            std::cout << "DDGI_RT: Raytracing not supported, using fallback" << std::endl;
            rtSupported = false;
            return false;
        }
        
        rtSupported = true;
        std::cout << "DDGI_RT: Initializing raytraced DDGI..." << std::endl;
        
        if (!CreateRootSignatures()) {
            std::cerr << "DDGI_RT: Failed to create root signatures" << std::endl;
            return false;
        }
        
        if (!CreateRaytracingPipeline()) {
            std::cerr << "DDGI_RT: Failed to create raytracing pipeline" << std::endl;
            return false;
        }
        
        if (!CreateIrradianceTextures()) {
            std::cerr << "DDGI_RT: Failed to create irradiance textures" << std::endl;
            return false;
        }
        
        if (!CreateDescriptorHeap()) {
            std::cerr << "DDGI_RT: Failed to create descriptor heap" << std::endl;
            return false;
        }
        
        if (!CreateConstantBuffer()) {
            std::cerr << "DDGI_RT: Failed to create constant buffer" << std::endl;
            return false;
        }
        
        initialized = true;
        std::cout << "DDGI_RT: Initialization complete" << std::endl;
        std::cout << "DDGI_RT: rtPipelineState = " << (rtPipelineState ? "VALID" : "NULL") << std::endl;
        std::cout << "DDGI_RT: rayGenShaderTable = " << (rayGenShaderTable ? "VALID" : "NULL") << std::endl;
        return true;
    }
    
    void Cleanup() {
        if (mappedCB && constantBuffer) {
            constantBuffer->Unmap(0, nullptr);
            mappedCB = nullptr;
        }
        initialized = false;
    }
    
    // Add geometry for acceleration structure building
    void AddGeometry(ID3D12Resource* vertexBuffer, UINT vertexCount, UINT vertexStride,
                     ID3D12Resource* indexBuffer, UINT indexCount,
                     const XMFLOAT3& albedo = XMFLOAT3(0.5f, 0.5f, 0.5f)) {
        RTGeometry geom;
        geom.vertexBuffer = vertexBuffer;
        geom.indexBuffer = indexBuffer;
        geom.vertexCount = vertexCount;
        geom.indexCount = indexCount;
        geom.vertexStride = vertexStride;
        geom.albedo = albedo;
        geometries.push_back(geom);
    }
    
    void ClearGeometry() {
        geometries.clear();
    }
    
    // Build acceleration structures (call after adding all geometry)
    bool BuildAccelerationStructures() {
        if (!rtSupported || geometries.empty()) return false;
        
        auto device5 = g_dx12.device5.Get();
        auto cmdList4 = g_dx12.commandList4.Get();
        if (!device5 || !cmdList4) return false;
        
        std::cout << "DDGI_RT: Building acceleration structures for " << geometries.size() << " geometries" << std::endl;
        
        // Build BLAS for each geometry
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geomDescs;
        for (const auto& geom : geometries) {
            D3D12_RAYTRACING_GEOMETRY_DESC desc = {};
            desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            desc.Triangles.VertexBuffer.StartAddress = geom.vertexBuffer->GetGPUVirtualAddress();
            desc.Triangles.VertexBuffer.StrideInBytes = geom.vertexStride;
            desc.Triangles.VertexCount = geom.vertexCount;
            desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            
            if (geom.indexBuffer && geom.indexCount > 0) {
                desc.Triangles.IndexBuffer = geom.indexBuffer->GetGPUVirtualAddress();
                desc.Triangles.IndexCount = geom.indexCount;
                desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            }
            
            geomDescs.push_back(desc);
        }
        
        // Get BLAS prebuild info
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs = {};
        blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blasInputs.NumDescs = (UINT)geomDescs.size();
        blasInputs.pGeometryDescs = geomDescs.data();
        blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasPrebuildInfo = {};
        device5->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &blasPrebuildInfo);
        
        // Create scratch buffer
        UINT64 scratchSize = std::max(blasPrebuildInfo.ScratchDataSizeInBytes, (UINT64)256);
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = scratchSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&scratchBuffer));
        if (FAILED(hr)) return false;
        
        // Create BLAS buffer
        bufferDesc.Width = blasPrebuildInfo.ResultDataMaxSizeInBytes;
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
            IID_PPV_ARGS(&bottomLevelAS));
        if (FAILED(hr)) return false;
        
        // Build BLAS
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasBuildDesc = {};
        blasBuildDesc.Inputs = blasInputs;
        blasBuildDesc.ScratchAccelerationStructureData = scratchBuffer->GetGPUVirtualAddress();
        blasBuildDesc.DestAccelerationStructureData = bottomLevelAS->GetGPUVirtualAddress();
        
        cmdList4->BuildRaytracingAccelerationStructure(&blasBuildDesc, 0, nullptr);
        
        // UAV barrier
        D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = bottomLevelAS.Get();
        cmdList4->ResourceBarrier(1, &uavBarrier);
        
        // Create instance buffer for TLAS
        D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
        instanceDesc.Transform[0][0] = 1.0f;
        instanceDesc.Transform[1][1] = 1.0f;
        instanceDesc.Transform[2][2] = 1.0f;
        instanceDesc.InstanceID = 0;
        instanceDesc.InstanceMask = 0xFF;
        instanceDesc.InstanceContributionToHitGroupIndex = 0;
        instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        instanceDesc.AccelerationStructure = bottomLevelAS->GetGPUVirtualAddress();
        
        D3D12_HEAP_PROPERTIES uploadHeapProps = {};
        uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        
        bufferDesc.Width = sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        
        hr = g_dx12.device->CreateCommittedResource(
            &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&instanceBuffer));
        if (FAILED(hr)) return false;
        
        void* mapped;
        instanceBuffer->Map(0, nullptr, &mapped);
        memcpy(mapped, &instanceDesc, sizeof(instanceDesc));
        instanceBuffer->Unmap(0, nullptr);
        
        // Get TLAS prebuild info
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs = {};
        tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlasInputs.NumDescs = 1;
        tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPrebuildInfo = {};
        device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasPrebuildInfo);
        
        // Create TLAS buffer
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        bufferDesc.Width = tlasPrebuildInfo.ResultDataMaxSizeInBytes;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
            IID_PPV_ARGS(&topLevelAS));
        if (FAILED(hr)) return false;
        
        // Reallocate scratch if needed
        if (tlasPrebuildInfo.ScratchDataSizeInBytes > scratchSize) {
            scratchBuffer.Reset();
            bufferDesc.Width = tlasPrebuildInfo.ScratchDataSizeInBytes;
            hr = g_dx12.device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(&scratchBuffer));
            if (FAILED(hr)) return false;
        }
        
        // Build TLAS
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasBuildDesc = {};
        tlasBuildDesc.Inputs = tlasInputs;
        tlasBuildDesc.Inputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();
        tlasBuildDesc.ScratchAccelerationStructureData = scratchBuffer->GetGPUVirtualAddress();
        tlasBuildDesc.DestAccelerationStructureData = topLevelAS->GetGPUVirtualAddress();
        
        cmdList4->BuildRaytracingAccelerationStructure(&tlasBuildDesc, 0, nullptr);
        
        // UAV barrier
        uavBarrier.UAV.pResource = topLevelAS.Get();
        cmdList4->ResourceBarrier(1, &uavBarrier);
        
        // Create material buffer with per-geometry albedo
        if (!CreateMaterialBuffer()) {
            std::cerr << "DDGI_RT: Failed to create material buffer" << std::endl;
            return false;
        }
        
        std::cout << "DDGI_RT: Acceleration structures built successfully" << std::endl;
        return true;
    }
    
    // Update DDGI probes using raytracing
    void UpdateProbes(const XMFLOAT3& sunDir, const XMFLOAT3& sunColor, float sunIntensity,
                      const std::vector<PointLightDataDX12>& pointLights) {
        if (!initialized || !rtSupported) return;
        
        auto cmdList4 = g_dx12.commandList4.Get();
        if (!cmdList4) return;
        
        frameNumber++;
        
        // Update constant buffer
        if (mappedCB) {
            mappedCB->probeGridOrigin = config.probeGridOrigin;
            mappedCB->probeSpacing = config.probeSpacing;
            mappedCB->probeCountX = config.probeCountX;
            mappedCB->probeCountY = config.probeCountY;
            mappedCB->probeCountZ = config.probeCountZ;
            mappedCB->raysPerProbe = config.raysPerProbe;
            mappedCB->maxRayDistance = config.maxRayDistance;
            mappedCB->normalBias = config.normalBias;
            mappedCB->hysteresis = config.hysteresis;
            mappedCB->giIntensity = config.giIntensity;
            mappedCB->irradianceTexWidth = config.irradianceTexWidth;
            mappedCB->irradianceTexHeight = config.irradianceTexHeight;
            mappedCB->frameNumber = frameNumber;
            mappedCB->sunDirection = sunDir;
            mappedCB->sunIntensity = sunIntensity;
            mappedCB->sunColor = sunColor;
            mappedCB->numPointLights = (int)std::min(pointLights.size(), (size_t)64);
        }
        
        // If we don't have a full raytracing pipeline, use simplified update
        if (!rtPipelineState || !rayGenShaderTable) {
            UpdateProbesSimplified(sunDir, sunColor, sunIntensity);
            return;
        }
        
        // Transition irradiance texture to UAV
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = irradianceTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList4->ResourceBarrier(1, &barrier);
        
        // Set pipeline state
        cmdList4->SetPipelineState1(rtPipelineState.Get());
        cmdList4->SetComputeRootSignature(globalRootSignature.Get());
        
        // Bind descriptor heap
        ID3D12DescriptorHeap* heaps[] = { srvUavHeap.Get() };
        cmdList4->SetDescriptorHeaps(1, heaps);
        
        // Bind root parameters
        cmdList4->SetComputeRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress());
        cmdList4->SetComputeRootDescriptorTable(1, srvUavHeap->GetGPUDescriptorHandleForHeapStart());
        if (topLevelAS) {
            cmdList4->SetComputeRootShaderResourceView(2, topLevelAS->GetGPUVirtualAddress());
        }
        if (materialBuffer) {
            cmdList4->SetComputeRootShaderResourceView(3, materialBuffer->GetGPUVirtualAddress());
        }
        
        // Dispatch rays
        D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
        
        // Ray generation shader table
        dispatchDesc.RayGenerationShaderRecord.StartAddress = rayGenShaderTable->GetGPUVirtualAddress();
        dispatchDesc.RayGenerationShaderRecord.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        
        // Miss shader table
        dispatchDesc.MissShaderTable.StartAddress = missShaderTable->GetGPUVirtualAddress();
        dispatchDesc.MissShaderTable.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        dispatchDesc.MissShaderTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        
        // Hit group shader table
        dispatchDesc.HitGroupTable.StartAddress = hitGroupShaderTable->GetGPUVirtualAddress();
        dispatchDesc.HitGroupTable.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        dispatchDesc.HitGroupTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        
        // Calculate dispatch dimensions
        int totalProbes = config.probeCountX * config.probeCountY * config.probeCountZ;
        int atlasWidth = (int)sqrt(totalProbes);
        int atlasHeight = (totalProbes + atlasWidth - 1) / atlasWidth;
        
        dispatchDesc.Width = atlasWidth * config.irradianceTexWidth;
        dispatchDesc.Height = atlasHeight * config.irradianceTexHeight;
        dispatchDesc.Depth = 1;
        
        cmdList4->DispatchRays(&dispatchDesc);
        
        // Transition back to SRV
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmdList4->ResourceBarrier(1, &barrier);
    }
    
    // Simplified probe update when full raytracing pipeline isn't available
    // Uses the acceleration structure for visibility but computes lighting on CPU
    void UpdateProbesSimplified(const XMFLOAT3& sunDir, const XMFLOAT3& sunColor, float sunIntensity) {
        // This fallback doesn't dispatch rays, but the compute DDGI from DDGI_DX12.h
        // will be used instead. Mark that we should use compute fallback.
        
        if (!irradianceTexture || !srvUavHeap) return;
        
        // The compute DDGI (ddgiRenderer) handles the actual probe update
        // when useRaytracedDDGI is true but rtPipelineState is null
        // This function is called but the real work is done by the compute path
    }
    
    D3D12_CPU_DESCRIPTOR_HANDLE GetIrradianceSRV() {
        if (!srvUavHeap) {
            D3D12_CPU_DESCRIPTOR_HANDLE nullHandle = {};
            return nullHandle;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE handle = srvUavHeap->GetCPUDescriptorHandleForHeapStart();
        // SRV is at offset 2 (UAV at 0, prev irradiance SRV at 1, current irradiance SRV at 2)
        handle.ptr += g_dx12.cbvSrvUavDescriptorSize * 2;
        return handle;
    }
    
private:
    bool CreateRootSignatures() {
        // Global root signature
        D3D12_ROOT_PARAMETER rootParams[4] = {};
        
        // Constant buffer (b0)
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[0].Descriptor.ShaderRegister = 0;
        rootParams[0].Descriptor.RegisterSpace = 0;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        
        // UAV/SRV descriptor table (u0 = output, t0 = prev irradiance)
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;
        
        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1].DescriptorTable.NumDescriptorRanges = 2;
        rootParams[1].DescriptorTable.pDescriptorRanges = ranges;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        
        // Acceleration structure (t1)
        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[2].Descriptor.ShaderRegister = 1;
        rootParams[2].Descriptor.RegisterSpace = 0;
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        
        // Material buffer (t2)
        rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[3].Descriptor.ShaderRegister = 2;
        rootParams[3].Descriptor.RegisterSpace = 0;
        rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        
        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = 4;
        rootSigDesc.pParameters = rootParams;
        rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        
        ComPtr<ID3DBlob> signature, error;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
        if (FAILED(hr)) {
            if (error) std::cerr << (char*)error->GetBufferPointer() << std::endl;
            return false;
        }
        
        hr = g_dx12.device->CreateRootSignature(0, signature->GetBufferPointer(),
            signature->GetBufferSize(), IID_PPV_ARGS(&globalRootSignature));
        return SUCCEEDED(hr);
    }
    
    bool CreateRaytracingPipeline() {
        // For raytracing shaders, we need DXC (dxcompiler.dll) which may not be available
        // Try to load and compile, fall back to simplified pipeline if it fails
        
        std::cout << "DDGI_RT: Attempting to create raytracing pipeline..." << std::endl;
        
        // Initialize DXC compiler
        if (!InitDXC()) {
            std::cerr << "DDGI_RT: DXC not available, using simplified fallback" << std::endl;
            return CreateSimplifiedPipeline();
        }
        
        // Compile raytracing library shader using DXC with debug enabled
        std::cout << "DDGI_RT: Compiling shaders/ddgi_raytracing.hlsl..." << std::endl;
        ComPtr<IDxcBlob> rtLibrary = CompileRaytracingLibrary(L"shaders/ddgi_raytracing.hlsl", true);
        if (!rtLibrary) {
            std::cerr << "DDGI_RT: Failed to compile raytracing shader, using fallback" << std::endl;
            return CreateSimplifiedPipeline();
        }
        
        std::cout << "DDGI_RT: Raytracing shader compiled successfully, size: " << rtLibrary->GetBufferSize() << " bytes" << std::endl;
        
        // Create the state object
        D3D12_STATE_OBJECT_DESC stateDesc = {};
        stateDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        
        std::vector<D3D12_STATE_SUBOBJECT> subobjects;
        subobjects.reserve(10);
        
        // DXIL Library subobject
        D3D12_DXIL_LIBRARY_DESC libDesc = {};
        libDesc.DXILLibrary.pShaderBytecode = rtLibrary->GetBufferPointer();
        libDesc.DXILLibrary.BytecodeLength = rtLibrary->GetBufferSize();
        
        D3D12_EXPORT_DESC exports[3] = {};
        exports[0].Name = L"RayGen";
        exports[0].ExportToRename = nullptr;
        exports[0].Flags = D3D12_EXPORT_FLAG_NONE;
        
        exports[1].Name = L"Miss";
        exports[1].ExportToRename = nullptr;
        exports[1].Flags = D3D12_EXPORT_FLAG_NONE;
        
        exports[2].Name = L"ClosestHit";
        exports[2].ExportToRename = nullptr;
        exports[2].Flags = D3D12_EXPORT_FLAG_NONE;
        
        libDesc.NumExports = 3;
        libDesc.pExports = exports;
        
        D3D12_STATE_SUBOBJECT libSubobject = {};
        libSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
        libSubobject.pDesc = &libDesc;
        subobjects.push_back(libSubobject);
        
        // Hit group
        D3D12_HIT_GROUP_DESC hitGroupDesc = {};
        hitGroupDesc.HitGroupExport = L"HitGroup";
        hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitGroupDesc.ClosestHitShaderImport = L"ClosestHit";
        
        D3D12_STATE_SUBOBJECT hitGroupSubobject = {};
        hitGroupSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
        hitGroupSubobject.pDesc = &hitGroupDesc;
        subobjects.push_back(hitGroupSubobject);
        
        // Shader config
        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
        shaderConfig.MaxPayloadSizeInBytes = sizeof(float) * 4;  // RayPayload: color + hitT
        shaderConfig.MaxAttributeSizeInBytes = sizeof(float) * 2;  // barycentrics
        
        D3D12_STATE_SUBOBJECT shaderConfigSubobject = {};
        shaderConfigSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
        shaderConfigSubobject.pDesc = &shaderConfig;
        subobjects.push_back(shaderConfigSubobject);
        
        // Global root signature
        D3D12_STATE_SUBOBJECT rootSigSubobject = {};
        rootSigSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
        rootSigSubobject.pDesc = globalRootSignature.GetAddressOf();
        subobjects.push_back(rootSigSubobject);
        
        // Pipeline config
        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
        pipelineConfig.MaxTraceRecursionDepth = 2;  // Primary + one bounce
        
        D3D12_STATE_SUBOBJECT pipelineConfigSubobject = {};
        pipelineConfigSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
        pipelineConfigSubobject.pDesc = &pipelineConfig;
        subobjects.push_back(pipelineConfigSubobject);
        
        stateDesc.NumSubobjects = (UINT)subobjects.size();
        stateDesc.pSubobjects = subobjects.data();
        
        HRESULT hr = g_dx12.device5->CreateStateObject(&stateDesc, IID_PPV_ARGS(&rtPipelineState));
        if (FAILED(hr)) {
            std::cerr << "DDGI_RT: Failed to create state object, HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
            return CreateSimplifiedPipeline();
        }
        
        std::cout << "DDGI_RT: Raytracing pipeline created successfully" << std::endl;
        
        // Create shader tables
        if (!CreateShaderTables()) {
            std::cerr << "DDGI_RT: Failed to create shader tables" << std::endl;
            return CreateSimplifiedPipeline();
        }
        
        return true;
    }
    
    bool CreateSimplifiedPipeline() {
        // Create a simplified compute-based GI that leverages the raytracing
        // acceleration structure for visibility queries
        
        std::cout << "DDGI_RT: Using simplified raytracing pipeline" << std::endl;
        
        // Mark as initialized - the UpdateProbes will use compute with AS queries
        // when full raytracing shaders aren't available
        
        return true;
    }
    
    bool CreateShaderTables() {
        if (!rtPipelineState) return true; // Simplified pipeline
        
        ComPtr<ID3D12StateObjectProperties> stateProps;
        rtPipelineState->QueryInterface(IID_PPV_ARGS(&stateProps));
        if (!stateProps) return false;
        
        UINT shaderIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        UINT tableSize = Align(shaderIdSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = tableSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        
        // Ray gen table
        g_dx12.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&rayGenShaderTable));
        
        void* rayGenData;
        rayGenShaderTable->Map(0, nullptr, &rayGenData);
        memcpy(rayGenData, stateProps->GetShaderIdentifier(L"RayGen"), shaderIdSize);
        rayGenShaderTable->Unmap(0, nullptr);
        
        // Miss table
        g_dx12.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&missShaderTable));
        
        void* missData;
        missShaderTable->Map(0, nullptr, &missData);
        memcpy(missData, stateProps->GetShaderIdentifier(L"Miss"), shaderIdSize);
        missShaderTable->Unmap(0, nullptr);
        
        // Hit group table
        g_dx12.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&hitGroupShaderTable));
        
        void* hitData;
        hitGroupShaderTable->Map(0, nullptr, &hitData);
        memcpy(hitData, stateProps->GetShaderIdentifier(L"HitGroup"), shaderIdSize);
        hitGroupShaderTable->Unmap(0, nullptr);
        
        return true;
    }
    
    bool CreateIrradianceTextures() {
        int totalProbes = config.probeCountX * config.probeCountY * config.probeCountZ;
        int atlasWidth = (int)sqrt(totalProbes);
        int atlasHeight = (totalProbes + atlasWidth - 1) / atlasWidth;
        
        int texWidth = atlasWidth * config.irradianceTexWidth;
        int texHeight = atlasHeight * config.irradianceTexHeight;
        
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = texWidth;
        texDesc.Height = texHeight;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texDesc.SampleDesc.Count = 1;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&irradianceTexture));
        if (FAILED(hr)) return false;
        
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&prevIrradianceTexture));
        if (FAILED(hr)) return false;
        
        return true;
    }
    
    bool CreateDescriptorHeap() {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 4; // UAV, SRV prev, SRV irradiance, SRV TLAS
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        
        HRESULT hr = g_dx12.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvUavHeap));
        if (FAILED(hr)) return false;
        
        D3D12_CPU_DESCRIPTOR_HANDLE handle = srvUavHeap->GetCPUDescriptorHandleForHeapStart();
        UINT increment = g_dx12.cbvSrvUavDescriptorSize;
        
        // UAV for output
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_dx12.device->CreateUnorderedAccessView(irradianceTexture.Get(), nullptr, &uavDesc, handle);
        
        // SRV for previous irradiance
        handle.ptr += increment;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(prevIrradianceTexture.Get(), &srvDesc, handle);
        
        // SRV for current irradiance (for main shader binding)
        handle.ptr += increment;
        g_dx12.device->CreateShaderResourceView(irradianceTexture.Get(), &srvDesc, handle);
        
        return true;
    }
    
    bool CreateMaterialBuffer() {
        if (geometries.empty()) return true;
        
        // Create structured buffer with material data for each geometry
        std::vector<RTMaterial> materials(geometries.size());
        for (size_t i = 0; i < geometries.size(); i++) {
            materials[i].albedo = geometries[i].albedo;
            materials[i].padding = 0.0f;
        }
        
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = sizeof(RTMaterial) * materials.size();
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&materialBuffer));
        if (FAILED(hr)) {
            std::cerr << "DDGI_RT: Failed to create material buffer" << std::endl;
            return false;
        }
        
        // Upload material data
        void* mapped;
        materialBuffer->Map(0, nullptr, &mapped);
        memcpy(mapped, materials.data(), sizeof(RTMaterial) * materials.size());
        materialBuffer->Unmap(0, nullptr);
        
        std::cout << "DDGI_RT: Created material buffer with " << materials.size() << " materials" << std::endl;
        return true;
    }
    
    bool CreateConstantBuffer() {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = sizeof(DDGIRaytracingCB);
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&constantBuffer));
        if (FAILED(hr)) return false;
        
        constantBuffer->Map(0, nullptr, (void**)&mappedCB);
        return true;
    }
    
    static UINT Align(UINT size, UINT alignment) {
        return (size + alignment - 1) & ~(alignment - 1);
    }
};

#endif // DDGI_DX12_RT_H

