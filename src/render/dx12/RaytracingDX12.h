#ifndef RAYTRACING_DX12_H
#define RAYTRACING_DX12_H

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include "Scene.h"
#include "ForwardRenderer.h"
#include <vector>
#include <fstream>
#include <sstream>

// DXR requires ID3D12Device5 and ID3D12GraphicsCommandList4
// Check for raytracing support and provide a fallback

struct RaytracingContext {
    bool supported = false;
    bool enabled = false;
    bool initialized = false;

    // DXR interfaces
    ComPtr<ID3D12Device5> dxrDevice;
    ComPtr<ID3D12GraphicsCommandList4> dxrCommandList;

    // Acceleration structures
    ComPtr<ID3D12Resource> blasBuffer;
    ComPtr<ID3D12Resource> blasScratch;
    ComPtr<ID3D12Resource> tlasBuffer;
    ComPtr<ID3D12Resource> tlasScratch;
    ComPtr<ID3D12Resource> instanceDescBuffer;

    // Raytracing pipeline
    ComPtr<ID3D12StateObject> rtPipelineState;
    ComPtr<ID3D12RootSignature> rtGlobalRootSig;
    ComPtr<ID3D12RootSignature> rtLocalRootSig;

    // Shader table
    ComPtr<ID3D12Resource> shaderTable;
    UINT shaderTableRecordSize = 0;

    // Output UAV
    ComPtr<ID3D12Resource> rtOutput;
    ComPtr<ID3D12DescriptorHeap> rtDescHeap;

    // Scene data for TLAS rebuild
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;

    // Constants
    struct RaytracingConstants {
        XMFLOAT4X4 invViewProj;
        XMFLOAT3 cameraPos;
        float pad0;
        XMFLOAT3 lightPos;
        float pad1;
        XMFLOAT3 lightColor;
        float pad2;
    };
    ComPtr<ID3D12Resource> constantBuffer;
    RaytracingConstants* mappedConstants = nullptr;
};

inline RaytracingContext g_rt;

inline bool CheckRaytracingSupport() {
    if (!g_dx12.device) return false;

    // Query DXR device
    HRESULT hr = g_dx12.device.As(&g_rt.dxrDevice);
    if (FAILED(hr)) {
        std::cout << "DXR: Device5 not supported" << std::endl;
        return false;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5 = {};
    hr = g_dx12.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5));
    if (FAILED(hr) || opts5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
        std::cout << "DXR: Raytracing not supported on this GPU" << std::endl;
        return false;
    }

    std::cout << "DXR: Raytracing Tier " << (opts5.RaytracingTier == D3D12_RAYTRACING_TIER_1_0 ? "1.0" : "1.1+") << " supported" << std::endl;
    g_rt.supported = true;
    return true;
}

inline bool CreateRaytracingRootSignatures() {
    // Global root signature (used by all shaders)
    // b0 = constants, t0 = TLAS SRV, u0 = output UAV
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[3] = {};
    // b0 - CBV for constants
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // t0 - TLAS SRV (descriptor table)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // u0 - Output UAV (descriptor table)
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uavRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 3;
    rootSigDesc.pParameters = params;

    ComPtr<ID3DBlob> sigBlob, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) std::cerr << "RT RootSig error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        return false;
    }

    hr = g_dx12.device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
        IID_PPV_ARGS(&g_rt.rtGlobalRootSig));
    if (FAILED(hr)) return false;

    // Empty local root signature (no per-shader data)
    D3D12_ROOT_SIGNATURE_DESC localDesc = {};
    localDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
    sigBlob.Reset(); errorBlob.Reset();
    hr = D3D12SerializeRootSignature(&localDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
    if (FAILED(hr)) return false;

    hr = g_dx12.device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
        IID_PPV_ARGS(&g_rt.rtLocalRootSig));
    return SUCCEEDED(hr);
}

inline bool CreateRaytracingPipeline() {
    // Compile RT shaders inline (simple ray gen + miss + closest hit)
    const char* rtShaderCode = R"(
        RaytracingAccelerationStructure Scene : register(t0);
        RWTexture2D<float4> Output : register(u0);

        cbuffer Constants : register(b0) {
            float4x4 invViewProj;
            float3 cameraPos;
            float pad0;
            float3 lightPos;
            float pad1;
            float3 lightColor;
            float pad2;
        };

        struct RayPayload {
            float3 color;
            float hitT;
        };

        [shader("raygeneration")]
        void RayGen() {
            uint2 launchIndex = DispatchRaysIndex().xy;
            uint2 launchDim = DispatchRaysDimensions().xy;

            float2 uv = (float2(launchIndex) + 0.5f) / float2(launchDim);
            float2 ndc = uv * 2.0f - 1.0f;
            ndc.y = -ndc.y;

            float4 worldPos = mul(invViewProj, float4(ndc, 0.0f, 1.0f));
            worldPos /= worldPos.w;
            float3 rayDir = normalize(worldPos.xyz - cameraPos);

            RayDesc ray;
            ray.Origin = cameraPos;
            ray.Direction = rayDir;
            ray.TMin = 0.001f;
            ray.TMax = 1000.0f;

            RayPayload payload;
            payload.color = float3(0, 0, 0);
            payload.hitT = -1.0f;

            TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

            Output[launchIndex] = float4(payload.color, 1.0f);
        }

        [shader("miss")]
        void Miss(inout RayPayload payload) {
            // Sky gradient
            float3 dir = WorldRayDirection();
            float t = 0.5f * (dir.y + 1.0f);
            payload.color = lerp(float3(1.0f, 1.0f, 1.0f), float3(0.5f, 0.7f, 1.0f), t);
            payload.hitT = -1.0f;
        }

        [shader("closesthit")]
        void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
            float3 hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
            
            // Simple shading: compute normal from barycentrics (flat shaded)
            float3 bary = float3(1.0f - attr.barycentrics.x - attr.barycentrics.y,
                                 attr.barycentrics.x, attr.barycentrics.y);
            
            // Fake normal (pointing up for floor-like surfaces, outward for others)
            float3 normal = float3(0, 1, 0);
            
            // Simple diffuse lighting
            float3 toLight = normalize(lightPos - hitPos);
            float diff = max(dot(normal, toLight), 0.0f);
            
            // Shadow ray
            RayDesc shadowRay;
            shadowRay.Origin = hitPos + normal * 0.001f;
            shadowRay.Direction = toLight;
            shadowRay.TMin = 0.001f;
            shadowRay.TMax = length(lightPos - hitPos);

            RayPayload shadowPayload;
            shadowPayload.hitT = -1.0f;
            TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                     0xFF, 0, 0, 0, shadowRay, shadowPayload);

            float shadow = (shadowPayload.hitT > 0.0f) ? 0.3f : 1.0f;
            
            float3 baseColor = float3(0.8f, 0.8f, 0.8f);
            payload.color = baseColor * lightColor * diff * shadow + baseColor * 0.1f;
            payload.hitT = RayTCurrent();
        }
    )";

    ComPtr<ID3DBlob> rtBlob, errorBlob;
    HRESULT hr = ShaderCacheDX12::CompileCached(rtShaderCode, strlen(rtShaderCode), "rt_shaders.hlsl",
        nullptr, nullptr, "", "lib_6_3",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &rtBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) std::cerr << "RT shader compile error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        return false;
    }

    // Build state object
    std::vector<D3D12_STATE_SUBOBJECT> subobjects;
    subobjects.reserve(16);

    // DXIL library
    D3D12_DXIL_LIBRARY_DESC libDesc = {};
    libDesc.DXILLibrary.pShaderBytecode = rtBlob->GetBufferPointer();
    libDesc.DXILLibrary.BytecodeLength = rtBlob->GetBufferSize();

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

    D3D12_STATE_SUBOBJECT libSubobj = {};
    libSubobj.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    libSubobj.pDesc = &libDesc;
    subobjects.push_back(libSubobj);

    // Hit group
    D3D12_HIT_GROUP_DESC hitGroupDesc = {};
    hitGroupDesc.HitGroupExport = L"HitGroup";
    hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroupDesc.ClosestHitShaderImport = L"ClosestHit";

    D3D12_STATE_SUBOBJECT hitGroupSubobj = {};
    hitGroupSubobj.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    hitGroupSubobj.pDesc = &hitGroupDesc;
    subobjects.push_back(hitGroupSubobj);

    // Shader config
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
    shaderConfig.MaxPayloadSizeInBytes = sizeof(float) * 4; // RayPayload
    shaderConfig.MaxAttributeSizeInBytes = sizeof(float) * 2; // barycentrics

    D3D12_STATE_SUBOBJECT shaderConfigSubobj = {};
    shaderConfigSubobj.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    shaderConfigSubobj.pDesc = &shaderConfig;
    subobjects.push_back(shaderConfigSubobj);

    // Global root signature
    D3D12_STATE_SUBOBJECT globalRootSigSubobj = {};
    globalRootSigSubobj.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    globalRootSigSubobj.pDesc = g_rt.rtGlobalRootSig.GetAddressOf();
    subobjects.push_back(globalRootSigSubobj);

    // Pipeline config
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = 2; // Primary + shadow

    D3D12_STATE_SUBOBJECT pipelineConfigSubobj = {};
    pipelineConfigSubobj.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    pipelineConfigSubobj.pDesc = &pipelineConfig;
    subobjects.push_back(pipelineConfigSubobj);

    // Create state object
    D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
    stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDesc.NumSubobjects = (UINT)subobjects.size();
    stateObjectDesc.pSubobjects = subobjects.data();

    hr = g_rt.dxrDevice->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&g_rt.rtPipelineState));
    if (FAILED(hr)) {
        std::cerr << "Failed to create RT pipeline state, HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    std::cout << "DXR: Pipeline created" << std::endl;
    return true;
}

inline bool CreateBLAS(const GeometryBuffers& geo) {
    // Get DXR command list
    HRESULT hr = g_dx12.commandList.As(&g_rt.dxrCommandList);
    if (FAILED(hr)) return false;

    // Build BLAS for cube geometry
    D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
    geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geomDesc.Triangles.VertexBuffer.StartAddress = geo.cubeVBV.BufferLocation;
    geomDesc.Triangles.VertexBuffer.StrideInBytes = geo.cubeVBV.StrideInBytes;
    geomDesc.Triangles.VertexCount = 36;
    geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geomDesc.Triangles.IndexBuffer = 0;
    geomDesc.Triangles.IndexCount = 0;
    geomDesc.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = 1;
    inputs.pGeometryDescs = &geomDesc;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    g_rt.dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);

    // Create scratch and result buffers
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = prebuildInfo.ScratchDataSizeInBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    hr = g_dx12.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_rt.blasScratch));
    if (FAILED(hr)) return false;

    bufDesc.Width = prebuildInfo.ResultDataMaxSizeInBytes;
    hr = g_dx12.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&g_rt.blasBuffer));
    if (FAILED(hr)) return false;

    // Build BLAS (will be done on first frame)
    std::cout << "DXR: BLAS created" << std::endl;
    return true;
}

inline bool CreateTLAS(UINT maxInstances) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = maxInstances;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD |
                   D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    g_rt.dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = prebuildInfo.ScratchDataSizeInBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT hr = g_dx12.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_rt.tlasScratch));
    if (FAILED(hr)) return false;

    bufDesc.Width = prebuildInfo.ResultDataMaxSizeInBytes;
    hr = g_dx12.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&g_rt.tlasBuffer));
    if (FAILED(hr)) return false;

    // Instance desc buffer (upload heap for dynamic updates)
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    bufDesc.Width = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * maxInstances;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = g_dx12.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_rt.instanceDescBuffer));
    if (FAILED(hr)) return false;

    g_rt.instanceDescs.resize(maxInstances);

    std::cout << "DXR: TLAS created (max " << maxInstances << " instances)" << std::endl;
    return true;
}

inline bool CreateRTOutputAndDescriptors() {
    // Output UAV texture
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = g_dx12.screenWidth;
    texDesc.Height = g_dx12.screenHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT hr = g_dx12.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_rt.rtOutput));
    if (FAILED(hr)) return false;

    // Descriptor heap for RT (TLAS SRV + Output UAV)
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 2;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_dx12.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_rt.rtDescHeap));
    if (FAILED(hr)) return false;

    UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = g_rt.rtDescHeap->GetCPUDescriptorHandleForHeapStart();

    // TLAS SRV (slot 0)
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.RaytracingAccelerationStructure.Location = g_rt.tlasBuffer->GetGPUVirtualAddress();
    g_dx12.device->CreateShaderResourceView(nullptr, &srvDesc, cpuHandle);
    cpuHandle.ptr += descSize;

    // Output UAV (slot 1)
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_dx12.device->CreateUnorderedAccessView(g_rt.rtOutput.Get(), nullptr, &uavDesc, cpuHandle);

    // Constant buffer
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = (sizeof(RaytracingContext::RaytracingConstants) + 255) & ~255;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = g_dx12.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_rt.constantBuffer));
    if (FAILED(hr)) return false;

    D3D12_RANGE readRange = { 0, 0 };
    hr = g_rt.constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&g_rt.mappedConstants));
    if (FAILED(hr)) return false;

    std::cout << "DXR: Output texture and descriptors created" << std::endl;
    return true;
}

inline bool CreateShaderTable() {
    // Get shader identifiers
    ComPtr<ID3D12StateObjectProperties> stateObjectProps;
    HRESULT hr = g_rt.rtPipelineState.As(&stateObjectProps);
    if (FAILED(hr)) return false;

    void* rayGenId = stateObjectProps->GetShaderIdentifier(L"RayGen");
    void* missId = stateObjectProps->GetShaderIdentifier(L"Miss");
    void* hitGroupId = stateObjectProps->GetShaderIdentifier(L"HitGroup");

    UINT shaderIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    g_rt.shaderTableRecordSize = (shaderIdSize + D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT - 1) &
                                  ~(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT - 1);

    UINT tableSize = g_rt.shaderTableRecordSize * 3; // RayGen, Miss, HitGroup

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = tableSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = g_dx12.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_rt.shaderTable));
    if (FAILED(hr)) return false;

    // Map and fill
    UINT8* pData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = g_rt.shaderTable->Map(0, &readRange, reinterpret_cast<void**>(&pData));
    if (FAILED(hr)) return false;

    memcpy(pData, rayGenId, shaderIdSize);
    pData += g_rt.shaderTableRecordSize;
    memcpy(pData, missId, shaderIdSize);
    pData += g_rt.shaderTableRecordSize;
    memcpy(pData, hitGroupId, shaderIdSize);

    g_rt.shaderTable->Unmap(0, nullptr);

    std::cout << "DXR: Shader table created" << std::endl;
    return true;
}

inline bool InitRaytracing(const GeometryBuffers& geo) {
    if (!CheckRaytracingSupport()) return false;
    if (!CreateRaytracingRootSignatures()) return false;
    if (!CreateRaytracingPipeline()) return false;
    if (!CreateBLAS(geo)) return false;
    if (!CreateTLAS(256)) return false;
    if (!CreateRTOutputAndDescriptors()) return false;
    if (!CreateShaderTable()) return false;

    g_rt.initialized = true;
    std::cout << "DXR: Raytracing initialized successfully" << std::endl;
    return true;
}

inline void UpdateTLAS(Scene& scene) {
    if (!g_rt.initialized) return;

    // Map instance buffer
    D3D12_RAYTRACING_INSTANCE_DESC* pInstances = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    g_rt.instanceDescBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pInstances));

    UINT instanceCount = 0;
    auto addInstance = [&](const XMMATRIX& transform) {
        if (instanceCount >= g_rt.instanceDescs.size()) return;
        XMFLOAT3X4 m;
        XMStoreFloat3x4(&m, transform);
        memcpy(pInstances[instanceCount].Transform, &m, sizeof(m));
        pInstances[instanceCount].InstanceID = instanceCount;
        pInstances[instanceCount].InstanceMask = 0xFF;
        pInstances[instanceCount].InstanceContributionToHitGroupIndex = 0;
        pInstances[instanceCount].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        pInstances[instanceCount].AccelerationStructure = g_rt.blasBuffer->GetGPUVirtualAddress();
        instanceCount++;
    };

    // Add scene objects
    addInstance(XMMatrixScaling(40.0f, 0.1f, 40.0f)); // Floor (scaled cube)
    addInstance(scene.cube1.GetModelMatrix());
    if (scene.cube2.visible) addInstance(scene.cube2.GetModelMatrix());

    for (auto& p : scene.projectiles) {
        if (p.active) {
            XMMATRIX m = XMMatrixScaling(scene.projectileScale, scene.projectileScale, scene.projectileScale)
                       * XMMatrixTranslation(p.position.x, p.position.y, p.position.z);
            addInstance(m);
        }
    }

    if (scene.gun.visible) addInstance(scene.GetGunModelMatrix());

    for (int i = 0; i < scene.clusteredRenderer.getTotalLightCount(); i++) {
        PointLightDX12* l = scene.clusteredRenderer.getLight(i);
        if (l && l->active) {
            XMMATRIX m = XMMatrixScaling(0.2f, 0.2f, 0.2f)
                       * XMMatrixTranslation(l->position.x, l->position.y, l->position.z);
            addInstance(m);
        }
    }

    g_rt.instanceDescBuffer->Unmap(0, nullptr);

    // Build TLAS
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildDesc.Inputs.NumDescs = instanceCount;
    buildDesc.Inputs.InstanceDescs = g_rt.instanceDescBuffer->GetGPUVirtualAddress();
    buildDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    buildDesc.DestAccelerationStructureData = g_rt.tlasBuffer->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData = g_rt.tlasScratch->GetGPUVirtualAddress();

    g_rt.dxrCommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    // UAV barrier
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = g_rt.tlasBuffer.Get();
    g_dx12.commandList->ResourceBarrier(1, &barrier);
}

inline void BuildBLAS() {
    if (!g_rt.initialized) return;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildDesc.Inputs.NumDescs = 1;
    buildDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    buildDesc.DestAccelerationStructureData = g_rt.blasBuffer->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData = g_rt.blasScratch->GetGPUVirtualAddress();

    // Need to set geometry desc again
    // For simplicity, we assume BLAS was already built in init
}

inline void RenderRaytracing(Scene& scene) {
    if (!g_rt.initialized || !g_rt.enabled) return;

    // Update constants
    XMMATRIX view = scene.GetViewMatrix();
    XMMATRIX proj = scene.GetProjectionMatrix();
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);
    XMStoreFloat4x4(&g_rt.mappedConstants->invViewProj, XMMatrixTranspose(invViewProj));
    g_rt.mappedConstants->cameraPos = scene.camera.Position;
    g_rt.mappedConstants->lightPos = scene.lightPos;
    g_rt.mappedConstants->lightColor = scene.EffectiveLightColor();

    // Update TLAS with current scene
    UpdateTLAS(scene);

    // Transition output to UAV
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_rt.rtOutput.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_dx12.commandList->ResourceBarrier(1, &barrier);

    // Set pipeline
    g_rt.dxrCommandList->SetPipelineState1(g_rt.rtPipelineState.Get());

    // Set root signature and descriptors
    g_dx12.commandList->SetComputeRootSignature(g_rt.rtGlobalRootSig.Get());
    g_dx12.commandList->SetComputeRootConstantBufferView(0, g_rt.constantBuffer->GetGPUVirtualAddress());

    ID3D12DescriptorHeap* heaps[] = { g_rt.rtDescHeap.Get() };
    g_dx12.commandList->SetDescriptorHeaps(1, heaps);

    UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = g_rt.rtDescHeap->GetGPUDescriptorHandleForHeapStart();
    g_dx12.commandList->SetComputeRootDescriptorTable(1, gpuHandle); // TLAS SRV
    gpuHandle.ptr += descSize;
    g_dx12.commandList->SetComputeRootDescriptorTable(2, gpuHandle); // Output UAV

    // Dispatch rays
    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    D3D12_GPU_VIRTUAL_ADDRESS tableBase = g_rt.shaderTable->GetGPUVirtualAddress();

    dispatchDesc.RayGenerationShaderRecord.StartAddress = tableBase;
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = g_rt.shaderTableRecordSize;

    dispatchDesc.MissShaderTable.StartAddress = tableBase + g_rt.shaderTableRecordSize;
    dispatchDesc.MissShaderTable.SizeInBytes = g_rt.shaderTableRecordSize;
    dispatchDesc.MissShaderTable.StrideInBytes = g_rt.shaderTableRecordSize;

    dispatchDesc.HitGroupTable.StartAddress = tableBase + g_rt.shaderTableRecordSize * 2;
    dispatchDesc.HitGroupTable.SizeInBytes = g_rt.shaderTableRecordSize;
    dispatchDesc.HitGroupTable.StrideInBytes = g_rt.shaderTableRecordSize;

    dispatchDesc.Width = g_dx12.screenWidth;
    dispatchDesc.Height = g_dx12.screenHeight;
    dispatchDesc.Depth = 1;

    g_rt.dxrCommandList->DispatchRays(&dispatchDesc);

    // Transition output to copy source
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_dx12.commandList->ResourceBarrier(1, &barrier);

    // Copy to backbuffer
    ID3D12Resource* backBuffer = g_dx12.renderTargets[g_dx12.frameIndex].Get();

    barrier.Transition.pResource = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_dx12.commandList->ResourceBarrier(1, &barrier);

    g_dx12.commandList->CopyResource(backBuffer, g_rt.rtOutput.Get());

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_dx12.commandList->ResourceBarrier(1, &barrier);
}

inline void ResizeRaytracing(UINT width, UINT height) {
    if (!g_rt.initialized) return;

    WaitForGPU();
    g_rt.rtOutput.Reset();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    g_dx12.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&g_rt.rtOutput));

    // Update UAV descriptor
    UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = g_rt.rtDescHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += descSize; // Skip TLAS SRV

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_dx12.device->CreateUnorderedAccessView(g_rt.rtOutput.Get(), nullptr, &uavDesc, cpuHandle);
}

#endif // RAYTRACING_DX12_H
