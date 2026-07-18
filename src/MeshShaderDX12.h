#ifndef MESH_SHADER_DX12_H
#define MESH_SHADER_DX12_H

#include "ShaderDX12.h"
#include <algorithm>

// Load generated DXIL beside the executable, independent of process working
// directory. Build scripts launch from the repository root while IDE/manual
// launches often use build/, so relative-only paths made mesh terrain fail on
// the first post-build run and work after relaunching from build/.
inline HRESULT ReadCompiledShaderDX12(const wchar_t* relativePath, ID3DBlob** blob) {
    wchar_t modulePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        std::wstring executablePath(modulePath, length);
        const size_t slash = executablePath.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            const std::wstring besideExecutable =
                executablePath.substr(0, slash + 1) + relativePath;
            const HRESULT hr = D3DReadFileToBlob(besideExecutable.c_str(), blob);
            if (SUCCEEDED(hr)) return hr;
        }
    }
    return D3DReadFileToBlob(relativePath, blob);
}

template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
struct alignas(8) MeshPSOSubobjectDX12 {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value{};
};

class MeshShaderDX12 {
public:
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12PipelineState> psoWireframe;
    ComPtr<ID3D12PipelineState> psoMSAA;
    ComPtr<ID3D12PipelineState> psoWireframeMSAA;
    ComPtr<ID3D12GraphicsCommandList6> commandList6;
    bool supported = false;
    bool msaaSupported = false;
    bool msaaEnabled = false;
    bool wireframe = false; // Z key: draw meshlets as wireframe
    bool occlusionEnabled = false;
    UINT occlusionMipCount = 1;
    D3D12_GPU_DESCRIPTOR_HANDLE occlusionDepthHandle = {};
    UINT dispatchesThisFrame = 0;
    UINT meshletsThisFrame = 0;
    UINT batchesThisFrame = 0;
    UINT instancesThisFrame = 0;
    UINT culledInstancesThisFrame = 0;
    static constexpr UINT MaxInstancesPerFrame = 4096;
    UploadBuffer<MeshInstanceDataDX12> instanceBuffer;
    UINT currentInstance = 0;

    void BeginFrame() {
        dispatchesThisFrame = 0;
        meshletsThisFrame = 0;
        batchesThisFrame = 0;
        instancesThisFrame = 0;
        culledInstancesThisFrame = 0;
        currentInstance = 0;
    }

    void SetOcclusionDepth(D3D12_GPU_DESCRIPTOR_HANDLE handle, bool enabled,
                           UINT mipCount = 1) {
        occlusionDepthHandle = handle;
        occlusionEnabled = enabled;
        occlusionMipCount = (std::max)(1u, mipCount);
    }

    bool CanDraw(UINT meshletCount,
                 D3D12_GPU_VIRTUAL_ADDRESS meshletDescAddress,
                 D3D12_GPU_VIRTUAL_ADDRESS meshletBoundsAddress,
                 D3D12_GPU_VIRTUAL_ADDRESS meshletVertexIndexAddress,
                 D3D12_GPU_VIRTUAL_ADDRESS meshletTriangleAddress) const {
        return supported && meshletCount > 0 && meshletDescAddress &&
               meshletBoundsAddress && meshletVertexIndexAddress && meshletTriangleAddress;
    }

    bool Init(ShaderDX12& shader) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
        HRESULT featureHr = g_dx12.device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
        if (FAILED(featureHr)) {
            std::cerr << "Mesh shader feature query failed: 0x" << std::hex << featureHr << std::dec << "\n";
            return false;
        }
        if (options7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
            std::cerr << "Mesh shaders unsupported by adapter (MeshShaderTier=0)\n";
            return false;
        }

        if (FAILED(g_dx12.commandList.As(&commandList6))) {
            std::cerr << "ID3D12GraphicsCommandList6 unavailable\n";
            return false;
        }
        ComPtr<ID3DBlob> ms;
        ComPtr<ID3DBlob> as;
        ComPtr<ID3DBlob> ps;
        if (FAILED(ReadCompiledShaderDX12(L"shaders/mesh_as.cso", &as))) {
            std::cerr << "Amplification shader DXIL missing: shaders/mesh_as.cso\n";
            return false;
        }
        if (FAILED(ReadCompiledShaderDX12(L"shaders/mesh_ms.cso", &ms))) {
            std::cerr << "Mesh shader DXIL missing: shaders/mesh_ms.cso\n";
            return false;
        }
        if (FAILED(ReadCompiledShaderDX12(L"shaders/mesh_ps.cso", &ps))) {
            std::cerr << "Mesh pixel shader DXIL missing: shaders/mesh_ps.cso\n";
            return false;
        }
        if (!shader.rootSignature) return false;

        using Root = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*>;
        using AS = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE>;
        using MS = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE>;
        using PS = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>;
        using Raster = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>;
        using Blend = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC>;
        using Depth = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC>;
        using Sample = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC>;
        using Mask = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT>;
        using RT = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>;
        using DS = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>;
        struct alignas(8) Stream { Root root; AS as; MS ms; PS ps; Raster raster; Blend blend; Depth depth; Sample sample; Mask mask; RT rt; DS ds; } stream;
        stream.root.value = shader.rootSignature.Get();
        stream.as.value = { as->GetBufferPointer(), as->GetBufferSize() };
        stream.ms.value = { ms->GetBufferPointer(), ms->GetBufferSize() };
        stream.ps.value = { ps->GetBufferPointer(), ps->GetBufferSize() };
        stream.raster.value.FillMode = D3D12_FILL_MODE_SOLID;
        stream.raster.value.CullMode = D3D12_CULL_MODE_NONE;
        stream.raster.value.DepthClipEnable = TRUE;
        stream.blend.value.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        stream.depth.value.DepthEnable = TRUE;
        stream.depth.value.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        stream.depth.value.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        stream.sample.value.Count = 1;
        stream.mask.value = UINT_MAX;
        stream.rt.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        stream.rt.value.NumRenderTargets = 1;
        stream.ds.value = DXGI_FORMAT_D32_FLOAT;
        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = { sizeof(stream), &stream };
        ComPtr<ID3D12Device2> device2;
        HRESULT device2Hr = g_dx12.device.As(&device2);
        HRESULT psoHr = SUCCEEDED(device2Hr)
            ? device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pso))
            : device2Hr;
        if (FAILED(psoHr)) {
            std::cerr << "Mesh PSO creation failed: 0x" << std::hex << psoHr << std::dec << "\n";
            return false;
        }
        stream.sample.value.Count = MSAADX12::SampleCount;
        stream.raster.value.MultisampleEnable = TRUE;
        msaaSupported = SUCCEEDED(
            device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&psoMSAA)));
        stream.sample.value.Count = 1;
        stream.raster.value.MultisampleEnable = FALSE;

        stream.raster.value.FillMode = D3D12_FILL_MODE_WIREFRAME;
        ComPtr<ID3DBlob> wirePs;
        if (SUCCEEDED(ReadCompiledShaderDX12(L"shaders/wire_green_ps.cso", &wirePs))) {
            stream.ps.value = { wirePs->GetBufferPointer(), wirePs->GetBufferSize() };
        }
        if (FAILED(device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&psoWireframe)))) {
            std::cerr << "Mesh wireframe PSO creation failed (non-fatal)\n";
        }
        if (msaaSupported) {
            stream.sample.value.Count = MSAADX12::SampleCount;
            stream.raster.value.MultisampleEnable = TRUE;
            if (FAILED(device2->CreatePipelineState(
                    &streamDesc, IID_PPV_ARGS(&psoWireframeMSAA)))) {
                psoWireframeMSAA.Reset();
            }
            stream.sample.value.Count = 1;
            stream.raster.value.MultisampleEnable = FALSE;
        }

        if (!instanceBuffer.Create(FRAME_COUNT * MaxInstancesPerFrame)) {
            std::cerr << "Mesh instance upload buffer creation failed\n";
            return false;
        }

        supported = true;
        return true;
    }

    void SetMSAAEnabled(bool enabled) {
        msaaEnabled = enabled && msaaSupported;
    }

    void Draw(const D3D12_VERTEX_BUFFER_VIEW& vbv,
              UINT vertexCount, UINT indexCount, UINT totalMeshlets,
              D3D12_GPU_VIRTUAL_ADDRESS meshletDescAddress,
              D3D12_GPU_VIRTUAL_ADDRESS meshletBoundsAddress,
              D3D12_GPU_VIRTUAL_ADDRESS meshletVertexIndexAddress,
              D3D12_GPU_VIRTUAL_ADDRESS meshletTriangleAddress,
              D3D12_GPU_VIRTUAL_ADDRESS bonePaletteAddress = 0,
              D3D12_GPU_VIRTUAL_ADDRESS skinDataAddress = 0) {
        if (!CanDraw(totalMeshlets, meshletDescAddress, meshletBoundsAddress,
                     meshletVertexIndexAddress, meshletTriangleAddress)) return;
        ID3D12PipelineState* solid =
            msaaEnabled ? psoMSAA.Get() : pso.Get();
        ID3D12PipelineState* wire =
            msaaEnabled ? psoWireframeMSAA.Get() : psoWireframe.Get();
        commandList6->SetPipelineState((wireframe && wire) ? wire : solid);
        commandList6->SetGraphicsRootShaderResourceView(9, vbv.BufferLocation);
        commandList6->SetGraphicsRootShaderResourceView(10, meshletDescAddress);
        commandList6->SetGraphicsRootShaderResourceView(11, meshletBoundsAddress);
        commandList6->SetGraphicsRootShaderResourceView(13, meshletVertexIndexAddress);
        commandList6->SetGraphicsRootShaderResourceView(14, meshletTriangleAddress);
        // t14 is part of the root signature even for ordinary draws. Keep a
        // valid fallback VA bound: some drivers may speculatively evaluate the
        // instance-buffer branch despite instancingEnabled being zero.
        commandList6->SetGraphicsRootShaderResourceView(18,
            instanceBuffer.GetGPUAddress(
                g_dx12.frameIndex * MaxInstancesPerFrame));
        if (occlusionDepthHandle.ptr) {
            commandList6->SetGraphicsRootDescriptorTable(12, occlusionDepthHandle);
        }
        const UINT skinning = (bonePaletteAddress && skinDataAddress) ? 1u : 0u;
        if (skinning) {
            commandList6->SetGraphicsRootShaderResourceView(16, bonePaletteAddress);
            commandList6->SetGraphicsRootShaderResourceView(17, skinDataAddress);
        }
        const UINT maxMeshletsPerDispatch = 65535u * 32u;
        UINT firstMeshlet = 0;
        while (firstMeshlet < totalMeshlets) {
            const UINT meshletCount = std::min(maxMeshletsPerDispatch, totalMeshlets - firstMeshlet);
            const UINT amplificationGroups = (meshletCount + 31) / 32;
            MeshDrawBufferDX12 data = {
                vertexCount, indexCount, indexCount ? 1u : 0u,
                firstMeshlet, totalMeshlets,
                occlusionEnabled ? 1u : 0u,
                g_dx12.screenWidth, g_dx12.screenHeight,
                skinning, occlusionMipCount, g_currentModelMaxScale,
                1u, 0u
            };
            commandList6->SetGraphicsRoot32BitConstants(8, 13, &data, 0);
            commandList6->DispatchMesh(amplificationGroups, 1, 1);
            ++dispatchesThisFrame;
            meshletsThisFrame += meshletCount;
            firstMeshlet += meshletCount;
        }
    }

    // Draw repeated static geometry in one mesh dispatch. The caller binds one
    // material and supplies all model transforms. Skinned geometry deliberately
    // stays on Draw(), where each instance owns a different bone palette.
    bool DrawInstanced(const D3D12_VERTEX_BUFFER_VIEW& vbv,
                       UINT vertexCount, UINT indexCount, UINT totalMeshlets,
                       D3D12_GPU_VIRTUAL_ADDRESS meshletDescAddress,
                       D3D12_GPU_VIRTUAL_ADDRESS meshletBoundsAddress,
                       D3D12_GPU_VIRTUAL_ADDRESS meshletVertexIndexAddress,
                       D3D12_GPU_VIRTUAL_ADDRESS meshletTriangleAddress,
                       const std::vector<DirectX::XMMATRIX>& models) {
        if (models.size() < 2 || !CanDraw(totalMeshlets, meshletDescAddress,
                meshletBoundsAddress, meshletVertexIndexAddress,
                meshletTriangleAddress)) return false;
        if (models.size() > MaxInstancesPerFrame - currentInstance) return false;

        const UINT frameBase = g_dx12.frameIndex * MaxInstancesPerFrame;
        const UINT instanceBase = currentInstance;
        for (const DirectX::XMMATRIX& model : models) {
            MeshInstanceDataDX12 instance = {};
            DirectX::XMStoreFloat4x4(&instance.model, DirectX::XMMatrixTranspose(model));
            instance.modelMaxScale = (std::max)({
                DirectX::XMVectorGetX(DirectX::XMVector3Length(model.r[0])),
                DirectX::XMVectorGetX(DirectX::XMVector3Length(model.r[1])),
                DirectX::XMVectorGetX(DirectX::XMVector3Length(model.r[2])) });
            instanceBuffer.CopyData(frameBase + currentInstance, instance);
            ++currentInstance;
        }

        ID3D12PipelineState* solid = msaaEnabled ? psoMSAA.Get() : pso.Get();
        ID3D12PipelineState* wire = msaaEnabled ? psoWireframeMSAA.Get() : psoWireframe.Get();
        commandList6->SetPipelineState((wireframe && wire) ? wire : solid);
        commandList6->SetGraphicsRootShaderResourceView(9, vbv.BufferLocation);
        commandList6->SetGraphicsRootShaderResourceView(10, meshletDescAddress);
        commandList6->SetGraphicsRootShaderResourceView(11, meshletBoundsAddress);
        commandList6->SetGraphicsRootShaderResourceView(13, meshletVertexIndexAddress);
        commandList6->SetGraphicsRootShaderResourceView(14, meshletTriangleAddress);
        commandList6->SetGraphicsRootShaderResourceView(
            18, instanceBuffer.GetGPUAddress(frameBase + instanceBase));
        if (occlusionDepthHandle.ptr)
            commandList6->SetGraphicsRootDescriptorTable(12, occlusionDepthHandle);

        const UINT instanceCount = static_cast<UINT>(models.size());
        const UINT totalWorkItems = totalMeshlets * instanceCount;
        const UINT maxWorkItems = 65535u * 32u;
        UINT firstWorkItem = 0;
        while (firstWorkItem < totalWorkItems) {
            const UINT workCount = (std::min)(maxWorkItems,
                totalWorkItems - firstWorkItem);
            const UINT amplificationGroups = (workCount + 31) / 32;
            MeshDrawBufferDX12 data = {
                vertexCount, indexCount, indexCount ? 1u : 0u,
                firstWorkItem, totalMeshlets,
                occlusionEnabled ? 1u : 0u,
                g_dx12.screenWidth, g_dx12.screenHeight,
                0u, occlusionMipCount, 1.0f,
                instanceCount, 1u
            };
            commandList6->SetGraphicsRoot32BitConstants(8, 13, &data, 0);
            commandList6->DispatchMesh(amplificationGroups, 1, 1);
            ++dispatchesThisFrame;
            firstWorkItem += workCount;
        }
        ++batchesThisFrame;
        instancesThisFrame += instanceCount;
        meshletsThisFrame += totalMeshlets * instanceCount;
        return true;
    }
};

#endif
