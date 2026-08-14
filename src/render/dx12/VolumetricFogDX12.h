#ifndef VOLUMETRIC_FOG_DX12_H
#define VOLUMETRIC_FOG_DX12_H

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include "Scene.h"
#include "WaterVolume.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

class VolumetricFogDX12 {
public:
    bool initialized = false;

    ~VolumetricFogDX12() {
        if (constantBuffer_ && constantsMapped_) constantBuffer_->Unmap(0, nullptr);
        if (clusterBuffer_ && clustersMapped_) clusterBuffer_->Unmap(0, nullptr);
        if (lightBuffer_ && lightsMapped_) lightBuffer_->Unmap(0, nullptr);
    }

    bool Init() {
        std::ifstream file("shaders/volumetric_fog.hlsl");
        if (!file) return false;
        std::stringstream text;
        text << file.rdbuf();
        const std::string source = text.str();
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ComPtr<ID3DBlob> cs, cloudCS, vs, ps, cloudPS, psMSAA, cloudPSMSAA,
            errors;
        const D3D_SHADER_MACRO cloudDefines[] = {
            { "SGE_WORLD_CLOUDS", "1" },
            { nullptr, nullptr }
        };
        if (!Compile(source, "CSMain", "cs_5_0", flags, cs, errors) ||
            !Compile(source, "CSMain", "cs_5_0", flags, cloudCS, errors,
                     cloudDefines) ||
            !Compile(source, "VSMain", "vs_5_0", flags, vs, errors) ||
            !Compile(source, "PSMain", "ps_5_0", flags, ps, errors) ||
            !Compile(source, "PSMain", "ps_5_0", flags, cloudPS, errors,
                     cloudDefines) ||
            !Compile(source, "PSMainMSAA", "ps_5_0", flags, psMSAA, errors) ||
            !Compile(source, "PSMainMSAA", "ps_5_0", flags, cloudPSMSAA,
                     errors, cloudDefines))
            return false;
        if (!CreateRootSignature() ||
            !CreatePipelines(cs.Get(), cloudCS.Get(), vs.Get(), ps.Get(),
                             cloudPS.Get(), psMSAA.Get(), cloudPSMSAA.Get()) ||
            !CreateHeapAndVolume() || !CreateUploadBuffers())
            return false;
        initialized = true;
        return true;
    }

    // CloudNoiseDX12 owns these immutable resources. This renderer only keeps
    // SRV descriptors, installed after the boot fence has completed generation.
    void SetCloudVolumes(ID3D12Resource* shape, ID3D12Resource* detail) {
        cloudVolumesReady_ = shape && detail && descriptorHeap_;
        CreateCloudVolumeSRV(shape, CpuHandle(5));
        CreateCloudVolumeSRV(detail, CpuHandle(6));
    }

    void Render(const Scene& scene, const WaterVolume& ocean,
                const XMMATRIX& lightSpace,
                ID3D12Resource* shadowResource, ID3D12Resource* depthResource,
                bool multisampledDepth,
                D3D12_CPU_DESCRIPTOR_HANDLE targetRtv = {},
                bool hdrTarget = false,
                bool depthAlreadyReadable = false) {
        if (!initialized || !g_dx12.commandList || !depthResource) return;
        fogTime_ += 1.0f / 60.0f;
        UpdateFrameData(scene, ocean, lightSpace, shadowResource, depthResource,
                        multisampledDepth);
        ID3D12GraphicsCommandList* commandList = g_dx12.commandList.Get();
        ID3D12DescriptorHeap* heaps[] = { descriptorHeap_.Get() };
        commandList->SetDescriptorHeaps(1, heaps);

        Transition(commandList, volume_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->SetPipelineState(scene.enableFlyableClouds
            ? cloudComputePipeline_.Get() : computePipeline_.Get());
        commandList->SetComputeRootSignature(rootSignature_.Get());
        BindComputeRoots(commandList);
        commandList->Dispatch((gridX_ + 7) / 8, (gridY_ + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER uav = {};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = volume_.Get();
        commandList->ResourceBarrier(1, &uav);
        Transition(commandList, volume_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        if (!depthAlreadyReadable)
            Transition(commandList, depthResource,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = targetRtv.ptr
            ? targetRtv
            : GetCPUDescriptorHandle(g_dx12.rtvHeap.Get(),
                g_dx12.rtvDescriptorSize, g_dx12.frameIndex);
        commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        commandList->RSSetViewports(1, &g_dx12.viewport);
        commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
        ID3D12PipelineState* compositePipeline = nullptr;
        if (multisampledDepth)
            compositePipeline = scene.enableFlyableClouds
                ? cloudGraphicsPipelineMSAA_.Get()
                : graphicsPipelineMSAA_.Get();
        else if (hdrTarget)
            compositePipeline = scene.enableFlyableClouds
                ? cloudGraphicsPipelineHDR_.Get() : graphicsPipelineHDR_.Get();
        else
            compositePipeline = scene.enableFlyableClouds
                ? cloudGraphicsPipeline_.Get() : graphicsPipeline_.Get();
        commandList->SetPipelineState(compositePipeline);
        commandList->SetGraphicsRootSignature(rootSignature_.Get());
        BindGraphicsRoots(commandList);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->DrawInstanced(3, 1, 0, 0);
        if (!depthAlreadyReadable)
            Transition(commandList, depthResource,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

private:
    // Fog needs substantially finer spatial/depth sampling than light clusters.
    // Keeping it at 16x9x10 blurred palm silhouettes into uniform haze and
    // prevented visible shafts through gaps in the fronds.
    //
    // The high-res toggle doubles each axis (8x the froxels) to sharpen shafts
    // further. The volume texture is always allocated at the high-res size so
    // flipping the toggle never reallocates GPU resources mid-frame; only the
    // dispatch extent and the dimensions handed to the shader change, and the
    // shader derives its froxel UVs from those, so the unused tail of the
    // texture is simply never sampled.
    static constexpr UINT GridX = 64;
    static constexpr UINT GridY = 36;
    static constexpr UINT GridZ = 48;
    static constexpr UINT MaxGridX = GridX * 2;
    static constexpr UINT MaxGridY = GridY * 2;
    static constexpr UINT MaxGridZ = GridZ * 2;

    UINT gridX_ = GridX;
    UINT gridY_ = GridY;
    UINT gridZ_ = GridZ;
    UINT jitterFrame_ = 0;
    static constexpr UINT ClusterCount =
        ClusteredRendererDX12::CLUSTER_X *
        ClusteredRendererDX12::CLUSTER_Y *
        ClusteredRendererDX12::CLUSTER_Z;
    static constexpr UINT MaxLights = ClusteredRendererDX12::MAX_LIGHTS;
    static constexpr UINT ConstantsSize = 512;

    struct GPUCluster {
        UINT lightCount;
        UINT lightIndices[ClusteredRendererDX12::MAX_LIGHTS_PER_CLUSTER];
    };
    struct GPULight {
        XMFLOAT3 position;
        float radius;
        XMFLOAT3 color;
        float intensity;
    };
    struct FogConstants {
        XMFLOAT4X4 inverseViewProjection;
        XMFLOAT4X4 shadowCascadeMatrices[SHADOW_CASCADE_COUNT];
        XMFLOAT4 cameraPositionNear;
        XMFLOAT4 cameraForwardFar;
        XMFLOAT4 sunDirectionDensity;
        XMFLOAT4 sunColorAnisotropy;
        XMFLOAT4 fogParams;
        XMFLOAT4 ambientFogColor;
        XMFLOAT4 shadowCascadeSplits;
        XMUINT4 clusterDimsLightCount;
        XMUINT4 volumeDims;
        XMFLOAT4 atmosphereParams;
        XMFLOAT4 cloudParams;
        XMFLOAT4 oceanBounds0;
        XMFLOAT4 oceanBounds1;
        XMUINT4 maxVolumeDims;
        // base height, thickness, density, coverage. Density <= 0 disables the
        // layer, so the branch costs nothing when the feature is off.
        XMFLOAT4 flyableCloudParams;
    };
    static_assert(sizeof(FogConstants) <= ConstantsSize, "Fog constants exceed one CBV page");

    static UINT Align256(UINT size) { return (size + 255u) & ~255u; }

    bool Compile(const std::string& source, const char* entry, const char* target,
                 UINT flags, ComPtr<ID3DBlob>& blob, ComPtr<ID3DBlob>& errors,
                 const D3D_SHADER_MACRO* defines = nullptr) {
        errors.Reset();
        HRESULT hr = ShaderCacheDX12::CompileCached(source.data(), source.size(), "volumetric_fog.hlsl",
            defines, nullptr, entry, target, flags, 0, &blob, &errors);
        if (FAILED(hr)) {
            if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }
        return true;
    }

    bool CreateRootSignature() {
        D3D12_DESCRIPTOR_RANGE ranges[6] = {};
        ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, 0 };
        ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
        ranges[2] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, 0 };
        ranges[3] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0, 0 };
        ranges[4] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5, 0, 0 };
        // Append the immutable shape/detail pair. Existing root indices remain
        // unchanged; both SRVs are one contiguous table at t6-t7.
        ranges[5] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 6, 0, 0 };
        D3D12_ROOT_PARAMETER roots[9] = {};
        roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        roots[0].Descriptor.ShaderRegister = 0;
        roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        roots[1].Descriptor.ShaderRegister = 0;
        roots[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        roots[2].Descriptor.ShaderRegister = 1;
        for (UINT i = 0; i < 6; ++i) {
            roots[3 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            roots[3 + i].DescriptorTable.NumDescriptorRanges = 1;
            roots[3 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        }

        D3D12_STATIC_SAMPLER_DESC samplers[3] = {};
        samplers[0].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[0].ShaderRegister = 0;
        samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[1].ShaderRegister = 1;
        samplers[2] = samplers[1];
        samplers[2].AddressU = samplers[2].AddressV = samplers[2].AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[2].ShaderRegister = 2;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = _countof(roots);
        desc.pParameters = roots;
        desc.NumStaticSamplers = _countof(samplers);
        desc.pStaticSamplers = samplers;
        ComPtr<ID3DBlob> serialized, errors;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                &serialized, &errors))) {
            if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }
        return SUCCEEDED(g_dx12.device->CreateRootSignature(0,
            serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature_)));
    }

    bool CreatePipelines(ID3DBlob* cs, ID3DBlob* cloudCS, ID3DBlob* vs,
                         ID3DBlob* ps, ID3DBlob* cloudPS,
                         ID3DBlob* psMSAA, ID3DBlob* cloudPSMSAA) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC compute = {};
        compute.pRootSignature = rootSignature_.Get();
        compute.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateComputePipelineState(
                &compute, IID_PPV_ARGS(&computePipeline_)))) return false;
        compute.CS = { cloudCS->GetBufferPointer(), cloudCS->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateComputePipelineState(
                &compute, IID_PPV_ARGS(&cloudComputePipeline_)))) return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics = {};
        graphics.pRootSignature = rootSignature_.Get();
        graphics.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        graphics.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        graphics.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        graphics.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        graphics.RasterizerState.DepthClipEnable = TRUE;
        D3D12_RENDER_TARGET_BLEND_DESC& blend = graphics.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED |
            D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        graphics.DepthStencilState.DepthEnable = FALSE;
        graphics.DepthStencilState.StencilEnable = FALSE;
        graphics.SampleMask = UINT_MAX;
        graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        graphics.NumRenderTargets = 1;
        graphics.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        graphics.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &graphics, IID_PPV_ARGS(&graphicsPipeline_)))) return false;
        graphics.PS = { cloudPS->GetBufferPointer(), cloudPS->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &graphics, IID_PPV_ARGS(&cloudGraphicsPipeline_)))) return false;
        graphics.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &graphics, IID_PPV_ARGS(&cloudGraphicsPipelineHDR_)))) return false;
        graphics.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &graphics, IID_PPV_ARGS(&graphicsPipelineHDR_)))) return false;
        graphics.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        graphics.PS = { psMSAA->GetBufferPointer(), psMSAA->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &graphics, IID_PPV_ARGS(&graphicsPipelineMSAA_)))) return false;
        graphics.PS = {
            cloudPSMSAA->GetBufferPointer(), cloudPSMSAA->GetBufferSize()
        };
        return SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
            &graphics, IID_PPV_ARGS(&cloudGraphicsPipelineMSAA_)));
    }

    bool CreateHeapAndVolume() {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 7;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&descriptorHeap_)))) return false;
        descriptorSize_ = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC volume = {};
        volume.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        volume.Width = MaxGridX;
        volume.Height = MaxGridY;
        volume.DepthOrArraySize = MaxGridZ;
        volume.MipLevels = 1;
        volume.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        volume.SampleDesc.Count = 1;
        volume.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        volume.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_dx12.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &volume, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&volume_)))) return false;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = volume.Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uav.Texture3D.WSize = MaxGridZ;
        g_dx12.device->CreateUnorderedAccessView(volume_.Get(), nullptr, &uav, CpuHandle(1));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = volume.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(volume_.Get(), &srv, CpuHandle(3));
        CreateCloudVolumeSRV(nullptr, CpuHandle(5));
        CreateCloudVolumeSRV(nullptr, CpuHandle(6));
        return true;
    }

    void CreateCloudVolumeSRV(ID3D12Resource* resource,
                              D3D12_CPU_DESCRIPTOR_HANDLE target) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(resource, &srv, target);
    }

    bool CreateUploadBuffers() {
        clusterFrameSize_ = Align256(sizeof(GPUCluster) * ClusterCount);
        lightFrameSize_ = Align256(sizeof(GPULight) * MaxLights);
        if (!CreateUploadResource(ConstantsSize * FRAME_COUNT, constantBuffer_,
                                  reinterpret_cast<void**>(&constantsMapped_)) ||
            !CreateUploadResource(clusterFrameSize_ * FRAME_COUNT, clusterBuffer_,
                                  reinterpret_cast<void**>(&clustersMapped_)) ||
            !CreateUploadResource(lightFrameSize_ * FRAME_COUNT, lightBuffer_,
                                  reinterpret_cast<void**>(&lightsMapped_))) return false;
        return true;
    }

    bool CreateUploadResource(UINT64 size, ComPtr<ID3D12Resource>& resource, void** mapped) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = size;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dx12.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&resource)))) return false;
        return SUCCEEDED(resource->Map(0, nullptr, mapped));
    }

    void UpdateFrameData(const Scene& scene, const WaterVolume& ocean,
                         const XMMATRIX& lightSpace,
                         ID3D12Resource* shadowResource,
                         ID3D12Resource* depthResource, bool multisampledDepth) {
        frame_ = g_dx12.frameIndex;
        // Picked per frame so the toggle takes effect immediately. The volume is
        // allocated at the high-res size either way, so this only changes how
        // much of it the dispatch fills and the shader samples.
        const bool highResolution = scene.volumetricFogHighRes ||
            (scene.enableFlyableClouds && cloudVolumesReady_);
        gridX_ = highResolution ? MaxGridX : GridX;
        gridY_ = highResolution ? MaxGridY : GridY;
        gridZ_ = highResolution ? MaxGridZ : GridZ;
        const XMMATRIX viewProjection = scene.GetViewMatrix() * scene.GetProjectionMatrix();
        FogConstants constants = {};
        XMStoreFloat4x4(&constants.inverseViewProjection,
            XMMatrixTranspose(XMMatrixInverse(nullptr, viewProjection)));
        for (UINT i = 0; i < SHADOW_CASCADE_COUNT; ++i)
            XMStoreFloat4x4(&constants.shadowCascadeMatrices[i],
                XMMatrixTranspose(g_shadowCascadeMatrices[i]));
        constants.cameraPositionNear = { scene.camera.Position.x, scene.camera.Position.y,
            scene.camera.Position.z, scene.cameraNear };
        constants.cameraForwardFar = { scene.camera.Front.x, scene.camera.Front.y,
            scene.camera.Front.z, scene.cameraFar };
        XMVECTOR sun = XMVector3Normalize(XMLoadFloat3(&scene.lightPos));
        XMFLOAT3 sunDirection;
        XMStoreFloat3(&sunDirection, sun);
        constants.sunDirectionDensity = {
            sunDirection.x, sunDirection.y, sunDirection.z,
            scene.enableVolumetricFog
                ? scene.volumetricFogDensity : 0.0f
        };
        const XMFLOAT3 effectiveLightColor = scene.EffectiveLightColor();
        constants.sunColorAnisotropy = {
            effectiveLightColor.x, effectiveLightColor.y,
            effectiveLightColor.z, scene.volumetricFogAnisotropy
        };
        constants.fogParams = { scene.volumetricFogHeightFalloff,
            scene.volumetricFogBaseHeight, scene.volumetricFogDistance,
            shadowResource ? 1.0f : 0.0f };
        constants.ambientFogColor = {
            scene.volumetricFogTint.x,
            scene.volumetricFogTint.y,
            scene.volumetricFogTint.z,
            fogTime_ };
        constants.shadowCascadeSplits = g_shadowCascadeSplits;
        constants.clusterDimsLightCount = {
            ClusteredRendererDX12::CLUSTER_X,
            ClusteredRendererDX12::CLUSTER_Y,
            ClusteredRendererDX12::CLUSTER_Z,
            static_cast<UINT>((std::min)(scene.clusteredRenderer.lights.size(),
                                        static_cast<size_t>(MaxLights))) };
        // w carries a frame counter so the froxel sampling offset advances every
        // frame; without it the dither would be a fixed pattern instead of
        // averaging out over time.
        constants.volumeDims = { gridX_, gridY_, gridZ_, jitterFrame_++ };
        constants.maxVolumeDims = { MaxGridX, MaxGridY, MaxGridZ, 0u };
        constants.atmosphereParams = {
            scene.enablePhysicalAtmosphere ? scene.atmosphereRayleighStrength : 0.0f,
            scene.atmosphereMieStrength,
            scene.atmosphereMieAnisotropy,
            scene.atmosphereAerialDensity
        };
        constants.cloudParams = {
            scene.enableFlyableClouds ? 0.0f
                                       : scene.atmosphereCloudCoverage,
            scene.atmosphereCloudDensity,
            scene.atmosphereCloudBaseHeight,
            scene.atmosphereCloudThickness
        };
        constants.flyableCloudParams = {
            scene.flyableCloudBaseHeight,
            scene.flyableCloudThickness,
            scene.enableFlyableClouds && cloudVolumesReady_
                ? scene.flyableCloudDensity : 0.0f,
            scene.flyableCloudCoverage
        };
        const XMFLOAT3 oceanCenter = ocean.GetCenter();
        const XMFLOAT3 oceanExtents = ocean.GetExtents();
        const float oceanHalfX = oceanExtents.x * 0.5f;
        const float oceanHalfZ = oceanExtents.z * 0.5f;
        constants.oceanBounds0 = {
            oceanCenter.x, ocean.GetSurfaceY(), oceanCenter.z,
            ocean.IsInitialized() ? 1.0f : 0.0f
        };
        constants.oceanBounds1 = {
            oceanHalfX, oceanHalfZ,
            (std::min)(16.0f, (std::max)(2.0f,
                (std::min)(oceanHalfX, oceanHalfZ) * 0.04f)),
            0.0f
        };
        std::memcpy(constantsMapped_ + frame_ * ConstantsSize, &constants, sizeof(constants));

        GPUCluster* gpuClusters = reinterpret_cast<GPUCluster*>(
            clustersMapped_ + frame_ * clusterFrameSize_);
        for (UINT i = 0; i < ClusterCount; ++i) {
            const auto& source = scene.clusteredRenderer.clusters[i];
            gpuClusters[i].lightCount = static_cast<UINT>((std::max)(0,
                (std::min)(source.lightCount,
                    ClusteredRendererDX12::MAX_LIGHTS_PER_CLUSTER)));
            for (UINT j = 0; j < gpuClusters[i].lightCount; ++j)
                gpuClusters[i].lightIndices[j] = static_cast<UINT>(source.lightIndices[j]);
        }
        GPULight* gpuLights = reinterpret_cast<GPULight*>(
            lightsMapped_ + frame_ * lightFrameSize_);
        std::memset(gpuLights, 0, sizeof(GPULight) * MaxLights);
        const UINT lightCount = constants.clusterDimsLightCount.w;
        for (UINT i = 0; i < lightCount; ++i) {
            const auto& source = scene.clusteredRenderer.lights[i];
            gpuLights[i] = { source.position, source.radius, source.color,
                             source.active ? source.intensity : 0.0f };
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrv = {};
        shadowSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shadowSrv.Format = DXGI_FORMAT_R32_FLOAT;
        shadowSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        shadowSrv.Texture2DArray.MipLevels = 1;
        shadowSrv.Texture2DArray.ArraySize = SHADOW_CASCADE_COUNT;
        g_dx12.device->CreateShaderResourceView(shadowResource, &shadowSrv, CpuHandle(0));
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = {};
        depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrv.Texture2D.MipLevels = 1;
        if (!multisampledDepth) {
            g_dx12.device->CreateShaderResourceView(depthResource, &depthSrv, CpuHandle(2));
            D3D12_SHADER_RESOURCE_VIEW_DESC nullMSAA = {};
            nullMSAA.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullMSAA.Format = DXGI_FORMAT_R32_FLOAT;
            nullMSAA.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            g_dx12.device->CreateShaderResourceView(nullptr, &nullMSAA, CpuHandle(4));
        } else {
            g_dx12.device->CreateShaderResourceView(nullptr, &depthSrv, CpuHandle(2));
            D3D12_SHADER_RESOURCE_VIEW_DESC depthMSAA = {};
            depthMSAA.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            depthMSAA.Format = DXGI_FORMAT_R32_FLOAT;
            depthMSAA.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            g_dx12.device->CreateShaderResourceView(depthResource, &depthMSAA, CpuHandle(4));
        }
    }

    void BindComputeRoots(ID3D12GraphicsCommandList* list) {
        list->SetComputeRootConstantBufferView(0,
            constantBuffer_->GetGPUVirtualAddress() + frame_ * ConstantsSize);
        list->SetComputeRootShaderResourceView(1,
            clusterBuffer_->GetGPUVirtualAddress() + frame_ * clusterFrameSize_);
        list->SetComputeRootShaderResourceView(2,
            lightBuffer_->GetGPUVirtualAddress() + frame_ * lightFrameSize_);
        for (UINT i = 0; i < 5; ++i)
            list->SetComputeRootDescriptorTable(3 + i, GpuHandle(i));
        list->SetComputeRootDescriptorTable(8, GpuHandle(5));
    }
    void BindGraphicsRoots(ID3D12GraphicsCommandList* list) {
        list->SetGraphicsRootConstantBufferView(0,
            constantBuffer_->GetGPUVirtualAddress() + frame_ * ConstantsSize);
        list->SetGraphicsRootShaderResourceView(1,
            clusterBuffer_->GetGPUVirtualAddress() + frame_ * clusterFrameSize_);
        list->SetGraphicsRootShaderResourceView(2,
            lightBuffer_->GetGPUVirtualAddress() + frame_ * lightFrameSize_);
        for (UINT i = 0; i < 5; ++i)
            list->SetGraphicsRootDescriptorTable(3 + i, GpuHandle(i));
        list->SetGraphicsRootDescriptorTable(8, GpuHandle(5));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT index) const {
        auto handle = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * descriptorSize_;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(UINT index) const {
        auto handle = descriptorHeap_->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * descriptorSize_;
        return handle;
    }
    static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }

    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> computePipeline_;
    ComPtr<ID3D12PipelineState> cloudComputePipeline_;
    ComPtr<ID3D12PipelineState> graphicsPipeline_;
    ComPtr<ID3D12PipelineState> cloudGraphicsPipeline_;
    ComPtr<ID3D12PipelineState> graphicsPipelineHDR_;
    ComPtr<ID3D12PipelineState> cloudGraphicsPipelineHDR_;
    ComPtr<ID3D12PipelineState> graphicsPipelineMSAA_;
    ComPtr<ID3D12PipelineState> cloudGraphicsPipelineMSAA_;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    ComPtr<ID3D12Resource> volume_;
    ComPtr<ID3D12Resource> constantBuffer_;
    ComPtr<ID3D12Resource> clusterBuffer_;
    ComPtr<ID3D12Resource> lightBuffer_;
    BYTE* constantsMapped_ = nullptr;
    BYTE* clustersMapped_ = nullptr;
    BYTE* lightsMapped_ = nullptr;
    UINT descriptorSize_ = 0;
    UINT clusterFrameSize_ = 0;
    UINT lightFrameSize_ = 0;
    UINT frame_ = 0;
    float fogTime_ = 0.0f;
    bool cloudVolumesReady_ = false;
};

#endif
