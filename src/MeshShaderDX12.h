#ifndef MESH_SHADER_DX12_H
#define MESH_SHADER_DX12_H

#include "ShaderDX12.h"
#include <algorithm>

template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
struct alignas(8) MeshPSOSubobjectDX12 {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value{};
};

class MeshShaderDX12 {
public:
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12GraphicsCommandList6> commandList6;
    bool supported = false;

    bool CanDraw(UINT vertexCount, UINT indexCount) const {
        UINT cornerCount = indexCount ? indexCount : vertexCount;
        return supported && vertexCount > 0 && cornerCount >= 3;
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
        ComPtr<ID3DBlob> ps;
        if (FAILED(D3DReadFileToBlob(L"shaders/mesh_ms.cso", &ms))) {
            std::cerr << "Mesh shader DXIL missing: shaders/mesh_ms.cso\n";
            return false;
        }
        if (FAILED(D3DReadFileToBlob(L"shaders/mesh_ps.cso", &ps))) {
            std::cerr << "Mesh pixel shader DXIL missing: shaders/mesh_ps.cso\n";
            return false;
        }
        if (!shader.rootSignature) return false;

        using Root = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*>;
        using MS = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE>;
        using PS = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>;
        using Raster = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>;
        using Blend = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC>;
        using Depth = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC>;
        using Sample = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC>;
        using Mask = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT>;
        using RT = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>;
        using DS = MeshPSOSubobjectDX12<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>;
        struct alignas(8) Stream { Root root; MS ms; PS ps; Raster raster; Blend blend; Depth depth; Sample sample; Mask mask; RT rt; DS ds; } stream;
        stream.root.value = shader.rootSignature.Get();
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
        supported = true;
        return true;
    }

    void Draw(const D3D12_VERTEX_BUFFER_VIEW& vbv, const D3D12_INDEX_BUFFER_VIEW* ibv,
              UINT vertexCount, UINT indexCount) {
        if (!CanDraw(vertexCount, indexCount)) return;
        commandList6->SetPipelineState(pso.Get());
        commandList6->SetGraphicsRootShaderResourceView(9, vbv.BufferLocation);
        commandList6->SetGraphicsRootShaderResourceView(10, ibv ? ibv->BufferLocation : 0);
        const UINT cornerCount = ibv ? indexCount : vertexCount;
        const UINT totalGroups = (cornerCount + 95) / 96;
        UINT firstGroup = 0;
        while (firstGroup < totalGroups) {
            const UINT groupCount = std::min(65535u, totalGroups - firstGroup);
            MeshDrawBufferDX12 data = { vertexCount, indexCount, ibv ? 1u : 0u, firstGroup * 96u };
            commandList6->SetGraphicsRoot32BitConstants(8, 4, &data, 0);
            commandList6->DispatchMesh(groupCount, 1, 1);
            firstGroup += groupCount;
        }
    }
};

#endif
