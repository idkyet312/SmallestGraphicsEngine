#ifndef SKY_RENDERER_DX12_H
#define SKY_RENDERER_DX12_H

#include "ShaderCacheDX12.h"
#include "ShaderDX12.h"
#include "CameraDX12.h"
#include "GLBImporter.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Poly Haven "Kloppenheim 06 (Pure Sky)", CC0. The daylight default.
inline constexpr const char* kSkyEnvironmentPath =
    "Content/Models/Skyboxes/kloppenheim_06_puresky_2k.exr";
// Poly Haven "Qwantani Night (Pure Sky)", CC0. Swapped in for the Night
// time-of-day preset -- a night sun direction alone cannot produce a starfield,
// so the environment map has to change with it.
inline constexpr const char* kSkyNightEnvironmentPath =
    "Content/Textures/Sky/qwantani_night_puresky_4k.exr";
inline constexpr float kSkyEnvironmentRotationRadians = XM_PIDIV2;

struct alignas(256) SkyBufferDX12 {
    XMFLOAT3 cameraForward;
    float tanHalfFov;
    XMFLOAT3 cameraRight;
    float aspectRatio;
    XMFLOAT3 cameraUp;
    float environmentRotation;
    XMFLOAT3 sunDirection;
    float exposure;
    XMFLOAT3 cameraPosition;
    float time;
    XMFLOAT4 atmosphereParams;
    XMFLOAT4 cloudParams;
};

class SkyRendererDX12 {
public:
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12PipelineState> hdrPipelineState;
    ComPtr<ID3D12PipelineState> msaaPipelineState;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12Resource> skyTexture;
    std::vector<ComPtr<ID3D12Resource>> uploadHeaps;
    UploadBuffer<SkyBufferDX12> constants;
    bool initialized = false;
    // Which environment map is currently resident, so SetEnvironment can skip a
    // reload when nothing changed.
    std::string loadedEnvironmentPath;
    // Dedicated direct list + fence for runtime environment swaps, owned here
    // rather than borrowing the frame's.
    //
    // The swap runs between frames and has to reset whatever allocator it
    // records into. Using g_dx12.commandAllocators[frameIndex] meant resetting
    // the frame's own allocator and draining with WaitForGPUAllFrames, whose
    // fence bookkeeping left fenceValues[] holding values it never signalled --
    // the next MoveToNextFrame() then waited forever and picking Night hung.
    //
    // A private allocator/list/fence keeps the swap entirely off the frame
    // pacing path. Direct rather than compute because the IBL prefilter
    // transitions its output to PIXEL_SHADER_RESOURCE, which a COMPUTE queue
    // cannot express. Mirrors MipGenerator's graphicsHandoff* members.
    ComPtr<ID3D12CommandAllocator> swapAllocator;
    ComPtr<ID3D12GraphicsCommandList> swapCommandList;
    ComPtr<ID3D12Fence> swapFence;
    UINT64 swapFenceValue = 0;
    bool msaaSupported = false;
    bool msaaEnabled = false;
    bool hdrTargetEnabled = false;

    bool Init() {
        std::ifstream vsFile("shaders/sky_vs.hlsl");
        std::ifstream psFile("shaders/sky_ps.hlsl");
        if (!vsFile || !psFile) return false;
        std::stringstream vsText, psText;
        vsText << vsFile.rdbuf(); psText << psFile.rdbuf();
        std::string vsSource = vsText.str();
        std::string psSource = psText.str();

        ComPtr<ID3DBlob> vs, ps, hdrPs, errors;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        HRESULT hr = ShaderCacheDX12::CompileCached(vsSource.data(), vsSource.size(), "sky_vs.hlsl",
            nullptr, nullptr, "main", "vs_5_0", flags, 0, &vs, &errors);
        if (FAILED(hr)) { if (errors) std::cerr << (char*)errors->GetBufferPointer(); return false; }
        errors.Reset();
        hr = ShaderCacheDX12::CompileCached(psSource.data(), psSource.size(), "shaders/sky_ps.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "main", "ps_5_0", flags, 0, &ps, &errors);
        if (FAILED(hr)) { if (errors) std::cerr << (char*)errors->GetBufferPointer(); return false; }
        const D3D_SHADER_MACRO hdrDefines[] = {
            { "SGE_HDR_TARGET", "1" }, { nullptr, nullptr }
        };
        errors.Reset();
        hr = ShaderCacheDX12::CompileCached(psSource.data(), psSource.size(), "shaders/sky_ps.hlsl",
            hdrDefines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "main", "ps_5_0", flags, 0, &hdrPs, &errors);
        if (FAILED(hr)) { if (errors) std::cerr << (char*)errors->GetBufferPointer(); return false; }

        D3D12_ROOT_PARAMETER roots[2] = {};
        roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        roots[0].Descriptor.ShaderRegister = 0;
        roots[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_DESCRIPTOR_RANGE textureRange = {};
        textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRange.NumDescriptors = 1;
        textureRange.BaseShaderRegister = 0;
        textureRange.OffsetInDescriptorsFromTableStart = 0;
        roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[1].DescriptorTable.NumDescriptorRanges = 1;
        roots[1].DescriptorTable.pDescriptorRanges = &textureRange;
        roots[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 2; rs.pParameters = roots;
        rs.NumStaticSamplers = 1; rs.pStaticSamplers = &sampler;
        ComPtr<ID3DBlob> signature;
        hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors);
        if (FAILED(hr)) return false;
        hr = g_dx12.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
        if (FAILED(hr)) return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = rootSignature.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        hr = g_dx12.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelineState));
        if (FAILED(hr) || !constants.Create(FRAME_COUNT)) return false;
        desc.PS = { hdrPs->GetBufferPointer(), hdrPs->GetBufferSize() };
        desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&hdrPipelineState)))) return false;
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = MSAADX12::SampleCount;
        desc.RasterizerState.MultisampleEnable = TRUE;
        msaaSupported = SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
            &desc, IID_PPV_ARGS(&msaaPipelineState)));

        skyTexture = GLBImporter::LoadEXRTextureFromFile(
            kSkyEnvironmentPath, g_dx12.device,
            g_dx12.commandList, uploadHeaps);
        if (!skyTexture) return false;
        loadedEnvironmentPath = kSkyEnvironmentPath;
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap)))) return false;
        g_dx12.device->CreateShaderResourceView(
            skyTexture.Get(), nullptr, srvHeap->GetCPUDescriptorHandleForHeapStart());

        // Private submission path for runtime environment swaps. Non-fatal: a
        // failure here only costs the ability to change sky at runtime, so the
        // level still boots on whatever map loaded above.
        if (SUCCEEDED(g_dx12.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&swapAllocator))) &&
            SUCCEEDED(g_dx12.device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, swapAllocator.Get(), nullptr,
                IID_PPV_ARGS(&swapCommandList)))) {
            if (FAILED(swapCommandList->Close())) swapCommandList.Reset();
            g_dx12.device->CreateFence(
                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&swapFence));
        }

        initialized = true;
        return true;
    }

    // Swaps the environment map. Handles both .exr and .hdr; no-ops when the
    // requested map is already resident, so callers can set it unconditionally.
    //
    // Returns false and keeps the current sky on a failed load rather than
    // leaving the renderer with no environment -- a missing night map should
    // cost the player the starfield, not the sky.
    // Opens the private swap command list for recording. Returns null when the
    // swap path is unavailable, in which case the environment cannot change.
    //
    // Split from SetEnvironment so the caller can record its own work (the IBL
    // prefilter) onto the same list and pay for one submit rather than two.
    ID3D12GraphicsCommandList* BeginEnvironmentSwap() {
        if (!initialized || !swapCommandList || !swapFence) return nullptr;
        // Wait for the previous swap only -- this fence is private, so this
        // cannot interact with frame pacing.
        WaitForFenceCPU(swapFence.Get(), swapFenceValue);
        if (FAILED(swapAllocator->Reset())) return nullptr;
        if (FAILED(swapCommandList->Reset(swapAllocator.Get(), nullptr)))
            return nullptr;
        return swapCommandList.Get();
    }

    // Closes, submits and blocks until the swap's GPU work has completed. The
    // texture copy and the prefilter must both be done before the next frame
    // samples them, and a swap is a rare, already-stalling event.
    void EndEnvironmentSwap() {
        if (!swapCommandList || !swapFence) return;
        if (FAILED(swapCommandList->Close())) return;
        ID3D12CommandList* lists[] = { swapCommandList.Get() };
        g_dx12.commandQueue->ExecuteCommandLists(1, lists);
        const UINT64 value = ++swapFenceValue;
        if (FAILED(g_dx12.commandQueue->Signal(swapFence.Get(), value))) return;
        WaitForFenceCPU(swapFence.Get(), value);
    }

    // Loads a new environment map, recording the upload onto `commandList` --
    // which must come from BeginEnvironmentSwap and be submitted with
    // EndEnvironmentSwap once the caller has finished recording.
    //
    // Takes the list explicitly rather than using g_dx12.commandList: the swap
    // runs between frames, when the frame's list is closed, and recording onto a
    // closed list is invalid.
    bool SetEnvironment(const std::string& path,
                        ID3D12GraphicsCommandList* commandList) {
        if (!initialized || path.empty() || !commandList) return false;
        if (path == loadedEnvironmentPath) return true;

        std::vector<ComPtr<ID3D12Resource>> pendingUploads;
        ComPtr<ID3D12Resource> loaded = GLBImporter::LoadEXRTextureFromFile(
            path, g_dx12.device, commandList, pendingUploads);
        if (!loaded) return false;

        skyTexture = loaded;
        // The staging buffers must outlive the copy just queued, so they replace
        // the previous set rather than being dropped here.
        uploadHeaps = std::move(pendingUploads);
        g_dx12.device->CreateShaderResourceView(
            skyTexture.Get(), nullptr,
            srvHeap->GetCPUDescriptorHandleForHeapStart());
        loadedEnvironmentPath = path;
        return true;
    }

    const std::string& EnvironmentPath() const { return loadedEnvironmentPath; }

    void SetMSAAEnabled(bool enabled) {
        msaaEnabled = enabled && msaaSupported;
    }

    void SetHDRTargetEnabled(bool enabled) { hdrTargetEnabled = enabled; }

    void Render(const Camera& camera, float fovDegrees,
                const XMFLOAT3& lightDirection, float time,
                bool physicalAtmosphere, const XMFLOAT4& atmosphereParams,
                const XMFLOAT4& cloudParams) {
        if (!initialized) return;
        // camera.Up is always world-up, not the camera's actual up. Building the
        // ray basis straight from it collapses the image plane as pitch nears
        // +/-90 (front ~ parallel to up), warping the sky while geometry - which
        // goes through LookAt's internal orthonormalization - stays correct.
        // Re-orthonormalize here instead.
        XMVECTOR front = XMVector3Normalize(XMLoadFloat3(&camera.Front));
        XMVECTOR worldUp = XMVector3Normalize(XMLoadFloat3(&camera.Up));
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, front));
        XMVECTOR up = XMVector3Normalize(XMVector3Cross(front, right));
        SkyBufferDX12 data = {};
        XMStoreFloat3(&data.cameraForward, front);
        XMStoreFloat3(&data.cameraRight, right);
        XMStoreFloat3(&data.cameraUp, up);
        data.tanHalfFov = tanf(XMConvertToRadians(fovDegrees) * 0.5f);
        data.aspectRatio = (float)g_dx12.screenWidth / (float)g_dx12.screenHeight;
        data.environmentRotation = kSkyEnvironmentRotationRadians;
        XMVECTOR sun = XMVector3Normalize(XMLoadFloat3(&lightDirection));
        XMStoreFloat3(&data.sunDirection, sun);
        // The night HDRI has a bright photographic horizon. Feeding it through
        // the daylight exposure makes AgX lift that band almost to white even
        // after atmospheric attenuation. Fade only once the sun is well below
        // the horizon so Noon/Afternoon/Dusk keep their historical exposure.
        const float nightBlend = (std::max)(0.0f, (std::min)(1.0f,
            (-XMVectorGetY(sun) - 0.10f) / 0.18f));
        data.exposure = 1.32f + (0.10f - 1.32f) * nightBlend;
        data.cameraPosition = camera.Position;
        data.time = time;
        data.atmosphereParams = atmosphereParams;
        if (!physicalAtmosphere)
            data.atmosphereParams.x = 0.0f;
        data.cloudParams = cloudParams;
        constants.CopyData(g_dx12.frameIndex, data);

        g_dx12.commandList->SetPipelineState(hdrTargetEnabled
            ? hdrPipelineState.Get()
            : (msaaEnabled ? msaaPipelineState.Get() : pipelineState.Get()));
        g_dx12.commandList->SetGraphicsRootSignature(rootSignature.Get());
        ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
        g_dx12.commandList->SetDescriptorHeaps(1, heaps);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(0, constants.GetGPUAddress(g_dx12.frameIndex));
        g_dx12.commandList->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());
        g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_dx12.commandList->DrawInstanced(3, 1, 0, 0);
    }
};

#endif
