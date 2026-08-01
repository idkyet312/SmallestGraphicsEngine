#ifndef LIGHT_SHAFTS_DX12_H
#define LIGHT_SHAFTS_DX12_H

// Screen-space radial light shafts, offered as an alternative to the froxel
// volumetric path.
//
// The volumetric renderer is more capable -- it occludes correctly against any
// geometry and works with the sun off screen -- but its grid is only 64 (or 128
// with the high-res toggle) froxels wide. A froxel column that straddles a
// silhouette averages the shadowed ray with the unoccluded one beside it, so
// brightness bleeds across the edge; the classic symptom is a glow that appears
// to shine through a hillside standing in front of the sun. No amount of
// sampling care fixes that, because the information was lost when the volume
// was rasterised at that resolution.
//
// This pass has no volume at all. It blurs the framebuffer radially away from
// the sun's screen position, depth-testing every tap, so it cannot quantise and
// cannot bleed across an occluder. The trade is that the sun must be on screen.

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include "Scene.h"
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>

class LightShaftsDX12 {
public:
    bool initialized = false;

    bool Init() {
        std::ifstream shaderFile("shaders/light_shafts.hlsl");
        if (!shaderFile) return false;
        std::stringstream shaderText;
        shaderText << shaderFile.rdbuf();
        const std::string source = shaderText.str();

        ComPtr<ID3DBlob> vs, ps, errors;
        const UINT flags =
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        HRESULT hr = ShaderCacheDX12::CompileCached(
            source.data(), source.size(), "light_shafts.hlsl", nullptr, nullptr,
            "VSMain", "vs_5_0", flags, 0, &vs, &errors);
        if (FAILED(hr)) {
            if (errors)
                std::cerr << static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }
        errors.Reset();
        hr = ShaderCacheDX12::CompileCached(
            source.data(), source.size(), "light_shafts.hlsl", nullptr, nullptr,
            "PSMain", "ps_5_0", flags, 0, &ps, &errors);
        if (FAILED(hr)) {
            if (errors)
                std::cerr << static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }

        // t0 scene depth only -- see the shader for why colour is not sampled.
        D3D12_DESCRIPTOR_RANGE textureRange = {};
        textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRange.NumDescriptors = 1;
        textureRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER roots[2] = {};
        roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[0].DescriptorTable.NumDescriptorRanges = 1;
        roots[0].DescriptorTable.pDescriptorRanges = &textureRange;
        roots[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        roots[1].Constants.ShaderRegister = 0;
        roots[1].Constants.Num32BitValues = 16;  // four float4s
        roots[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = roots;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers = &sampler;
        rootDesc.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized;
        if (FAILED(D3D12SerializeRootSignature(
                &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
            return false;
        if (FAILED(g_dx12.device->CreateRootSignature(
                0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(&rootSignature_))))
            return false;

        // Additive: shafts add light to the frame, they never darken it.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {};
        pipeline.pRootSignature = rootSignature_.Get();
        pipeline.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pipeline.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        D3D12_RENDER_TARGET_BLEND_DESC& blend =
            pipeline.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_ONE;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.DepthStencilState.DepthEnable = FALSE;
        pipeline.DepthStencilState.StencilEnable = FALSE;
        pipeline.SampleMask = UINT_MAX;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pipeline.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &pipeline, IID_PPV_ARGS(&pipelineState_))))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&srvHeap_))))
            return false;

        descriptorSize_ = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        initialized = true;
        return true;
    }

    // Depth is transitioned here unless the caller says it is already readable.
    void Render(const Scene& scene, ID3D12GraphicsCommandList* commandList,
                ID3D12Resource* depthResource,
                D3D12_CPU_DESCRIPTOR_HANDLE targetRtv,
                bool depthAlreadyReadable) {
        if (!initialized || !commandList || !depthResource)
            return;

        // Project the sun into screen space. lightPos is a direction, so place a
        // proxy far along it from the camera.
        const XMMATRIX viewProjection =
            scene.GetViewMatrix() * scene.GetProjectionMatrix();
        XMVECTOR direction = XMVector3Normalize(XMLoadFloat3(&scene.lightPos));
        XMVECTOR sunWorld = XMVectorAdd(
            XMLoadFloat3(&scene.camera.Position),
            XMVectorScale(direction, 4000.0f));
        XMVECTOR clip = XMVector3Transform(sunWorld, viewProjection);
        XMFLOAT4 projected;
        XMStoreFloat4(&projected, clip);

        float sunU = 0.5f, sunV = 0.5f;
        bool onScreen = false;
        if (projected.w > 0.0f) {
            const float ndcX = projected.x / projected.w;
            const float ndcY = projected.y / projected.w;
            sunU = ndcX * 0.5f + 0.5f;
            sunV = -ndcY * 0.5f + 0.5f;
            // Allow a margin so shafts fade rather than pop at the frame edge.
            onScreen = sunU > -0.35f && sunU < 1.35f &&
                       sunV > -0.35f && sunV < 1.35f;
        }

        if (!depthAlreadyReadable)
            Transition(commandList, depthResource,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        UpdateDescriptors(depthResource);

        commandList->OMSetRenderTargets(1, &targetRtv, FALSE, nullptr);
        commandList->RSSetViewports(1, &g_dx12.viewport);
        commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
        commandList->SetPipelineState(pipelineState_.Get());
        commandList->SetGraphicsRootSignature(rootSignature_.Get());
        ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetGraphicsRootDescriptorTable(
            0, srvHeap_->GetGPUDescriptorHandleForHeapStart());

        const float constants[16] = {
            sunU, sunV, scene.lightShaftIntensity, onScreen ? 1.0f : 0.0f,
            scene.lightShaftDensity, scene.lightShaftDecay,
            scene.lightShaftWeight, scene.lightShaftExposure,
            scene.cameraNear, scene.cameraFar, 0.0f, 0.0f,
            scene.lightColor.x, scene.lightColor.y, scene.lightColor.z, 0.0f
        };
        commandList->SetGraphicsRoot32BitConstants(1, 16, constants, 0);
        commandList->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->DrawInstanced(3, 1, 0, 0);

        if (!depthAlreadyReadable)
            Transition(commandList, depthResource,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

private:
    static void Transition(ID3D12GraphicsCommandList* commandList,
                           ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);
    }

    void UpdateDescriptors(ID3D12Resource* depthResource) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            srvHeap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = {};
        depthSrv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(depthResource, &depthSrv, handle);
    }

    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipelineState_;
    ComPtr<ID3D12DescriptorHeap> srvHeap_;
    UINT descriptorSize_ = 0;
};

#endif // LIGHT_SHAFTS_DX12_H
