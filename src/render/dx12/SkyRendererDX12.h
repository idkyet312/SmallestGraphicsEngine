#ifndef SKY_RENDERER_DX12_H
#define SKY_RENDERER_DX12_H

#include "ShaderCacheDX12.h"
#include "ShaderDX12.h"
#include "CameraDX12.h"
#include "GLBImporter.h"
#include <fstream>
#include <sstream>

// Poly Haven "Kloppenheim 06 (Pure Sky)", CC0.
inline constexpr const char* kSkyEnvironmentPath =
    "Content/Models/Skyboxes/kloppenheim_06_puresky_2k.exr";
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
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap)))) return false;
        g_dx12.device->CreateShaderResourceView(
            skyTexture.Get(), nullptr, srvHeap->GetCPUDescriptorHandleForHeapStart());
        initialized = true;
        return true;
    }

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
        data.exposure = 1.32f;
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
