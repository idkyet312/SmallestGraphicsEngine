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
    ComPtr<ID3D12PipelineState> psoDoubleSided;
    ComPtr<ID3D12PipelineState> psoWireframe;
    ComPtr<ID3D12PipelineState> psoMSAA;
    ComPtr<ID3D12PipelineState> psoDoubleSidedMSAA;
    ComPtr<ID3D12PipelineState> psoWireframeMSAA;
    ComPtr<ID3D12PipelineState> psoHDR;
    ComPtr<ID3D12PipelineState> psoDoubleSidedHDR;
    ComPtr<ID3D12PipelineState> psoWireframeHDR;
    // Extension-motion PSOs: 2 RTVs (HDR colour + R16G16_FLOAT motion).
    ComPtr<ID3D12PipelineState> psoHDRMotion;
    ComPtr<ID3D12PipelineState> psoDoubleSidedHDRMotion;
    ComPtr<ID3D12PipelineState> psoBindless;
    ComPtr<ID3D12PipelineState> psoBindlessDoubleSided;
    ComPtr<ID3D12PipelineState> psoBindlessWireframe;
    ComPtr<ID3D12PipelineState> psoBindlessMSAA;
    ComPtr<ID3D12PipelineState> psoBindlessDoubleSidedMSAA;
    ComPtr<ID3D12PipelineState> psoBindlessWireframeMSAA;
    ComPtr<ID3D12PipelineState> psoBindlessHDR;
    ComPtr<ID3D12PipelineState> psoBindlessDoubleSidedHDR;
    ComPtr<ID3D12PipelineState> psoBindlessWireframeHDR;
    ComPtr<ID3D12PipelineState> psoBindlessHDRMotion;
    ComPtr<ID3D12PipelineState> psoBindlessDoubleSidedHDRMotion;
    ComPtr<ID3D12GraphicsCommandList6> commandList6;
    bool supported = false;
    bool msaaSupported = false;
    bool msaaEnabled = false;
    bool wireframe = false; // Z key: draw meshlets as wireframe
    bool hdrTargetEnabled = false;
    bool extensionMotionEnabled = false;
    bool bindlessReady = false;
    bool bindlessActive = false;
    bool occlusionEnabled = false;
    UINT occlusionMipCount = 1;
    D3D12_GPU_DESCRIPTOR_HANDLE occlusionDepthHandle = {};
    D3D12_GPU_DESCRIPTOR_HANDLE bindlessOcclusionDepthHandle = {};
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
                           UINT mipCount = 1,
                           D3D12_GPU_DESCRIPTOR_HANDLE bindlessHandle = {}) {
        occlusionDepthHandle = handle;
        bindlessOcclusionDepthHandle = bindlessHandle;
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
        ComPtr<ID3DBlob> hdrPs;
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
        if (FAILED(ReadCompiledShaderDX12(L"shaders/mesh_ps_hdr.cso", &hdrPs))) {
            std::cerr << "HDR mesh pixel shader DXIL missing: shaders/mesh_ps_hdr.cso\n";
            return false;
        }
        ComPtr<ID3DBlob> motionPs;
        const bool motionPsAvailable =
            SUCCEEDED(ReadCompiledShaderDX12(L"shaders/mesh_ps_motion.cso", &motionPs));
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
        stream.raster.value.CullMode = D3D12_CULL_MODE_BACK;
        // Engine geometry and imported glTF/FBX assets use CCW outward winding.
        stream.raster.value.FrontCounterClockwise = TRUE;
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
        stream.rt.value.RTFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        stream.ps.value = { hdrPs->GetBufferPointer(), hdrPs->GetBufferSize() };
        if (FAILED(device2->CreatePipelineState(
                &streamDesc, IID_PPV_ARGS(&psoHDR)))) return false;
        stream.ps.value = { ps->GetBufferPointer(), ps->GetBufferSize() };
        stream.rt.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        stream.sample.value.Count = MSAADX12::SampleCount;
        stream.raster.value.MultisampleEnable = TRUE;
        msaaSupported = SUCCEEDED(
            device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&psoMSAA)));
        stream.sample.value.Count = 1;
        stream.raster.value.MultisampleEnable = FALSE;

        // Thin sheets, foliage, glass, and explicitly double-sided imported
        // materials keep the compatibility path.
        stream.raster.value.CullMode = D3D12_CULL_MODE_NONE;
        if (FAILED(device2->CreatePipelineState(
                &streamDesc, IID_PPV_ARGS(&psoDoubleSided)))) {
            std::cerr << "Mesh double-sided PSO creation failed\n";
            return false;
        }
        stream.rt.value.RTFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        stream.ps.value = { hdrPs->GetBufferPointer(), hdrPs->GetBufferSize() };
        if (FAILED(device2->CreatePipelineState(
                &streamDesc, IID_PPV_ARGS(&psoDoubleSidedHDR)))) return false;
        stream.ps.value = { ps->GetBufferPointer(), ps->GetBufferSize() };
        stream.rt.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (msaaSupported) {
            stream.sample.value.Count = MSAADX12::SampleCount;
            stream.raster.value.MultisampleEnable = TRUE;
            if (FAILED(device2->CreatePipelineState(
                    &streamDesc, IID_PPV_ARGS(&psoDoubleSidedMSAA)))) {
                msaaSupported = false;
                psoDoubleSidedMSAA.Reset();
            }
            stream.sample.value.Count = 1;
            stream.raster.value.MultisampleEnable = FALSE;
        }

        stream.raster.value.FillMode = D3D12_FILL_MODE_WIREFRAME;
        ComPtr<ID3DBlob> wirePs;
        if (SUCCEEDED(ReadCompiledShaderDX12(L"shaders/wire_green_ps.cso", &wirePs))) {
            stream.ps.value = { wirePs->GetBufferPointer(), wirePs->GetBufferSize() };
        }
        if (FAILED(device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&psoWireframe)))) {
            std::cerr << "Mesh wireframe PSO creation failed (non-fatal)\n";
        }
        stream.rt.value.RTFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (FAILED(device2->CreatePipelineState(
                &streamDesc, IID_PPV_ARGS(&psoWireframeHDR))))
            psoWireframeHDR.Reset();
        stream.rt.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
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

        // Extension-motion HDR PSOs: 2 render targets (colour + motion).
        // Bandits are forward extensions drawn after the visibility resolve,
        // so these mirror the HDR solid and double-sided variants but with a
        // motion-target pixel shader and NumRenderTargets=2.
        if (motionPsAvailable) {
            stream.raster.value.CullMode = D3D12_CULL_MODE_BACK;
            stream.raster.value.FillMode = D3D12_FILL_MODE_SOLID;
            stream.rt.value.RTFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            stream.rt.value.RTFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
            stream.rt.value.NumRenderTargets = 2;
            stream.ps.value = { motionPs->GetBufferPointer(), motionPs->GetBufferSize() };
            if (FAILED(device2->CreatePipelineState(
                    &streamDesc, IID_PPV_ARGS(&psoHDRMotion))))
                psoHDRMotion.Reset();
            stream.raster.value.CullMode = D3D12_CULL_MODE_NONE;
            if (FAILED(device2->CreatePipelineState(
                    &streamDesc, IID_PPV_ARGS(&psoDoubleSidedHDRMotion))))
                psoDoubleSidedHDRMotion.Reset();
            // Restore to single-RT LDR defaults
            stream.ps.value = { ps->GetBufferPointer(), ps->GetBufferSize() };
            stream.rt.value.NumRenderTargets = 1;
            stream.rt.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            stream.raster.value.CullMode = D3D12_CULL_MODE_BACK;
        }

        if (shader.bindlessRootSignature && shader.bindlessPixelShaderBlob &&
            shader.bindlessHDRPixelShaderBlob &&
            shader.bindlessMotionPixelShaderBlob) {
            stream.root.value = shader.bindlessRootSignature.Get();
            auto createBindless = [&](ComPtr<ID3D12PipelineState>& target,
                    ID3DBlob* pixel, DXGI_FORMAT format, bool doubleSided,
                    bool wire, bool multisampled, bool motion) {
                stream.ps.value = {
                    pixel->GetBufferPointer(), pixel->GetBufferSize() };
                stream.raster.value.CullMode = doubleSided
                    ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
                stream.raster.value.FillMode = wire
                    ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
                stream.raster.value.MultisampleEnable = multisampled;
                stream.sample.value.Count = multisampled
                    ? MSAADX12::SampleCount : 1;
                stream.rt.value.NumRenderTargets = motion ? 2 : 1;
                stream.rt.value.RTFormats[0] = format;
                stream.rt.value.RTFormats[1] = motion
                    ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_UNKNOWN;
                return SUCCEEDED(device2->CreatePipelineState(
                    &streamDesc, IID_PPV_ARGS(&target)));
            };
            bool ok = true;
            ok = ok && createBindless(psoBindless,
                shader.bindlessPixelShaderBlob.Get(),
                DXGI_FORMAT_R8G8B8A8_UNORM, false, false, false, false);
            ok = ok && createBindless(psoBindlessDoubleSided,
                shader.bindlessPixelShaderBlob.Get(),
                DXGI_FORMAT_R8G8B8A8_UNORM, true, false, false, false);
            ok = ok && createBindless(psoBindlessWireframe,
                shader.bindlessPixelShaderBlob.Get(),
                DXGI_FORMAT_R8G8B8A8_UNORM, false, true, false, false);
            ok = ok && createBindless(psoBindlessHDR,
                shader.bindlessHDRPixelShaderBlob.Get(),
                DXGI_FORMAT_R16G16B16A16_FLOAT, false, false, false, false);
            ok = ok && createBindless(psoBindlessDoubleSidedHDR,
                shader.bindlessHDRPixelShaderBlob.Get(),
                DXGI_FORMAT_R16G16B16A16_FLOAT, true, false, false, false);
            ok = ok && createBindless(psoBindlessWireframeHDR,
                shader.bindlessHDRPixelShaderBlob.Get(),
                DXGI_FORMAT_R16G16B16A16_FLOAT, false, true, false, false);
            ok = ok && createBindless(psoBindlessMSAA,
                shader.bindlessPixelShaderBlob.Get(),
                DXGI_FORMAT_R8G8B8A8_UNORM, false, false, true, false);
            ok = ok && createBindless(psoBindlessDoubleSidedMSAA,
                shader.bindlessPixelShaderBlob.Get(),
                DXGI_FORMAT_R8G8B8A8_UNORM, true, false, true, false);
            ok = ok && createBindless(psoBindlessWireframeMSAA,
                shader.bindlessPixelShaderBlob.Get(),
                DXGI_FORMAT_R8G8B8A8_UNORM, false, true, true, false);
            ok = ok && createBindless(psoBindlessHDRMotion,
                shader.bindlessMotionPixelShaderBlob.Get(),
                DXGI_FORMAT_R16G16B16A16_FLOAT, false, false, false, true);
            ok = ok && createBindless(psoBindlessDoubleSidedHDRMotion,
                shader.bindlessMotionPixelShaderBlob.Get(),
                DXGI_FORMAT_R16G16B16A16_FLOAT, true, false, false, true);
            bindlessReady = ok;
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

    void SetHDRTargetEnabled(bool enabled) { hdrTargetEnabled = enabled; }

    void SetExtensionMotionEnabled(bool enabled) { extensionMotionEnabled = enabled; }
    void SetBindlessActive(bool enabled) {
        bindlessActive = enabled && bindlessReady;
    }

    void Draw(const D3D12_VERTEX_BUFFER_VIEW& vbv,
              UINT vertexCount, UINT indexCount, UINT totalMeshlets,
              D3D12_GPU_VIRTUAL_ADDRESS meshletDescAddress,
              D3D12_GPU_VIRTUAL_ADDRESS meshletBoundsAddress,
              D3D12_GPU_VIRTUAL_ADDRESS meshletVertexIndexAddress,
              D3D12_GPU_VIRTUAL_ADDRESS meshletTriangleAddress,
              D3D12_GPU_VIRTUAL_ADDRESS bonePaletteAddress = 0,
              D3D12_GPU_VIRTUAL_ADDRESS skinDataAddress = 0,
              bool doubleSided = false,
              bool allowOcclusion = true,
              // View model: bind-pose meshlet bounds say nothing about where
              // this ends up on screen, so exempt it from meshlet culling.
              bool disableCulling = false,
              D3D12_GPU_VIRTUAL_ADDRESS previousBonePaletteAddress = 0) {
        if (!CanDraw(totalMeshlets, meshletDescAddress, meshletBoundsAddress,
                     meshletVertexIndexAddress, meshletTriangleAddress)) return;
        ID3D12PipelineState* solid = bindlessActive
            ? (hdrTargetEnabled
                ? (extensionMotionEnabled && psoBindlessHDRMotion
                    ? (doubleSided ? psoBindlessDoubleSidedHDRMotion.Get()
                                   : psoBindlessHDRMotion.Get())
                    : (doubleSided ? psoBindlessDoubleSidedHDR.Get()
                                   : psoBindlessHDR.Get()))
                : (doubleSided
                    ? (msaaEnabled ? psoBindlessDoubleSidedMSAA.Get()
                                   : psoBindlessDoubleSided.Get())
                    : (msaaEnabled ? psoBindlessMSAA.Get()
                                   : psoBindless.Get())))
            : (hdrTargetEnabled
            ? (extensionMotionEnabled && psoHDRMotion
                ? (doubleSided
                    ? (psoDoubleSidedHDRMotion ? psoDoubleSidedHDRMotion.Get() : psoDoubleSidedHDR.Get())
                    : psoHDRMotion.Get())
                : (doubleSided ? psoDoubleSidedHDR.Get() : psoHDR.Get()))
            : (doubleSided
                ? (msaaEnabled ? psoDoubleSidedMSAA.Get() : psoDoubleSided.Get())
                : (msaaEnabled ? psoMSAA.Get() : pso.Get())));
        ID3D12PipelineState* wire = bindlessActive
            ? (hdrTargetEnabled ? psoBindlessWireframeHDR.Get()
                : (msaaEnabled ? psoBindlessWireframeMSAA.Get()
                               : psoBindlessWireframe.Get()))
            : (hdrTargetEnabled ? psoWireframeHDR.Get()
                : (msaaEnabled ? psoWireframeMSAA.Get() : psoWireframe.Get()));
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
        const D3D12_GPU_DESCRIPTOR_HANDLE activeOcclusionHandle =
            bindlessActive && bindlessOcclusionDepthHandle.ptr
                ? bindlessOcclusionDepthHandle : occlusionDepthHandle;
        if (activeOcclusionHandle.ptr) {
            commandList6->SetGraphicsRootDescriptorTable(12, activeOcclusionHandle);
        }
        // 2 selects the skinned-and-unculled path in mesh_as.hlsl; the mesh
        // shader treats any non-zero value as "skin this".
        const UINT skinning = (bonePaletteAddress && skinDataAddress)
            ? (disableCulling ? 2u : 1u) : 0u;
        if (skinning) {
            commandList6->SetGraphicsRootShaderResourceView(16, bonePaletteAddress);
            commandList6->SetGraphicsRootShaderResourceView(17, skinDataAddress);
            // Previous bone palette for motion-vector skinning (root param 19, t20).
            commandList6->SetGraphicsRootShaderResourceView(19,
                previousBonePaletteAddress ? previousBonePaletteAddress : bonePaletteAddress);
        }
        const UINT maxMeshletsPerDispatch = 65535u * 32u;
        UINT firstMeshlet = 0;
        while (firstMeshlet < totalMeshlets) {
            const UINT meshletCount = std::min(maxMeshletsPerDispatch, totalMeshlets - firstMeshlet);
            const UINT amplificationGroups = (meshletCount + 31) / 32;
            MeshDrawBufferDX12 data = {
                vertexCount, indexCount, indexCount ? 1u : 0u,
                firstMeshlet, totalMeshlets,
                (occlusionEnabled && allowOcclusion) ? 1u : 0u,
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
                       const std::vector<DirectX::XMMATRIX>& models,
                       bool doubleSided = false,
                       bool allowOcclusion = true) {
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

        ID3D12PipelineState* solid = bindlessActive
            ? (hdrTargetEnabled
                ? (doubleSided ? psoBindlessDoubleSidedHDR.Get()
                               : psoBindlessHDR.Get())
                : (doubleSided
                    ? (msaaEnabled ? psoBindlessDoubleSidedMSAA.Get()
                                   : psoBindlessDoubleSided.Get())
                    : (msaaEnabled ? psoBindlessMSAA.Get()
                                   : psoBindless.Get())))
            : (hdrTargetEnabled
            ? (doubleSided ? psoDoubleSidedHDR.Get() : psoHDR.Get())
            : (doubleSided
                ? (msaaEnabled ? psoDoubleSidedMSAA.Get() : psoDoubleSided.Get())
                : (msaaEnabled ? psoMSAA.Get() : pso.Get())));
        ID3D12PipelineState* wire = bindlessActive
            ? (hdrTargetEnabled ? psoBindlessWireframeHDR.Get()
                : (msaaEnabled ? psoBindlessWireframeMSAA.Get()
                               : psoBindlessWireframe.Get()))
            : (hdrTargetEnabled ? psoWireframeHDR.Get()
                : (msaaEnabled ? psoWireframeMSAA.Get() : psoWireframe.Get()));
        commandList6->SetPipelineState((wireframe && wire) ? wire : solid);
        commandList6->SetGraphicsRootShaderResourceView(9, vbv.BufferLocation);
        commandList6->SetGraphicsRootShaderResourceView(10, meshletDescAddress);
        commandList6->SetGraphicsRootShaderResourceView(11, meshletBoundsAddress);
        commandList6->SetGraphicsRootShaderResourceView(13, meshletVertexIndexAddress);
        commandList6->SetGraphicsRootShaderResourceView(14, meshletTriangleAddress);
        commandList6->SetGraphicsRootShaderResourceView(
            18, instanceBuffer.GetGPUAddress(frameBase + instanceBase));
        const D3D12_GPU_DESCRIPTOR_HANDLE activeOcclusionHandle =
            bindlessActive && bindlessOcclusionDepthHandle.ptr
                ? bindlessOcclusionDepthHandle : occlusionDepthHandle;
        if (activeOcclusionHandle.ptr)
            commandList6->SetGraphicsRootDescriptorTable(12, activeOcclusionHandle);

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
                (occlusionEnabled && allowOcclusion) ? 1u : 0u,
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
