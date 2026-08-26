#ifndef COLLISION_DEBUG_RENDERER_DX12_H
#define COLLISION_DEBUG_RENDERER_DX12_H

#include "ShaderDX12.h"
#include "CollisionMesh.h"
#include "PrefabColliders.h"

#include <fstream>
#include <sstream>
#include <vector>

// Wireframe overlay for the volumes the player actually collides against.
//
// Line-list rather than the existing wireframe fill mode: a triangulated box
// rasterized as wireframe shows each face's diagonal, so a scene full of boxes
// reads as a mess of triangles rather than as boxes. Twelve explicit edges per
// volume is both cheaper and clearer.
//
// This renderer owns a minimal root signature (one CBV) instead of borrowing the
// forward path's 23-parameter one. It costs a root-signature switch per frame,
// which the impact-particle renderer already establishes as acceptable, and it
// keeps a debug view from constraining the shape of the real rendering path.
struct CollisionDebugVertexDX12 {
    XMFLOAT3 position;
    XMFLOAT4 color;
};
static_assert(sizeof(CollisionDebugVertexDX12) == 28);

struct alignas(256) CollisionDebugFrameDX12 {
    XMMATRIX viewProjection;
};

class CollisionDebugRendererDX12 {
public:
    // 12 edges * 2 vertices = 24 vertices per box. This ceiling covers roughly
    // 5,400 volumes per frame, far past any level's prefab count; overflow stops
    // adding rather than growing the buffer mid-frame.
    static constexpr UINT MaxVertices = 131072;
    bool initialized = false;

    bool Init() {
        std::ifstream shaderFile("shaders/collision_debug.hlsl");
        if (!shaderFile) return false;
        std::stringstream shaderText;
        shaderText << shaderFile.rdbuf();
        const std::string source = shaderText.str();

        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        ComPtr<ID3DBlob> vs, ps, hdrPs, errors;
        if (FAILED(ShaderCacheDX12::CompileCached(source.data(), source.size(),
                "shaders/collision_debug.hlsl", nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0", flags,
                0, &vs, &errors))) {
            if (errors) std::cerr << (char*)errors->GetBufferPointer();
            return false;
        }
        errors.Reset();
        if (FAILED(ShaderCacheDX12::CompileCached(source.data(), source.size(),
                "shaders/collision_debug.hlsl", nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", flags,
                0, &ps, &errors))) {
            if (errors) std::cerr << (char*)errors->GetBufferPointer();
            return false;
        }
        const D3D_SHADER_MACRO hdrDefines[] = {
            { "SGE_HDR_TARGET", "1" }, { nullptr, nullptr }
        };
        errors.Reset();
        if (FAILED(ShaderCacheDX12::CompileCached(source.data(), source.size(),
                "shaders/collision_debug.hlsl", hdrDefines,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", flags,
                0, &hdrPs, &errors))) {
            if (errors) std::cerr << (char*)errors->GetBufferPointer();
            return false;
        }

        D3D12_ROOT_PARAMETER roots[1] = {};
        roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        roots[0].Descriptor.ShaderRegister = 0;
        roots[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = _countof(roots);
        rootDesc.pParameters = roots;
        rootDesc.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> signature;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc,
                D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors)) ||
            FAILED(g_dx12.device->CreateRootSignature(0,
                signature->GetBufferPointer(), signature->GetBufferSize(),
                IID_PPV_ARGS(&rootSignature_)))) return false;

        if (!CreatePipelines(vs.Get(), ps.Get(), hdrPs.Get())) return false;
        if (!vertices_.Create(MaxVertices * FRAME_COUNT) ||
            !frames_.Create(FRAME_COUNT)) return false;
        initialized = true;
        return true;
    }

    // Queues one oriented box. `yawRadians` matches PrefabCollider's single-angle
    // orientation, which is all the box colliders themselves store.
    void AddBox(const XMFLOAT3& center, const XMFLOAT3& halfExtents,
                float yawRadians, const XMFLOAT4& color) {
        const float cosine = std::cos(yawRadians);
        const float sine = std::sin(yawRadians);
        XMFLOAT3 corner[8];
        for (int i = 0; i < 8; ++i) {
            const float x = (i & 1) ? halfExtents.x : -halfExtents.x;
            const float y = (i & 2) ? halfExtents.y : -halfExtents.y;
            const float z = (i & 4) ? halfExtents.z : -halfExtents.z;
            // Same yaw convention as ResolvePlayerPrefabCollisions writes back,
            // so the drawn box is the box the player is pushed out of.
            corner[i] = XMFLOAT3(center.x + x * cosine - z * sine,
                                 center.y + y,
                                 center.z + x * sine + z * cosine);
        }
        AddBoxEdges(corner, color);
    }

    // Queues an axis-aligned box given by its two extreme corners.
    void AddBounds(const XMFLOAT3& minimum, const XMFLOAT3& maximum,
                   const XMFLOAT4& color) {
        XMFLOAT3 corner[8];
        for (int i = 0; i < 8; ++i) {
            corner[i] = XMFLOAT3((i & 1) ? maximum.x : minimum.x,
                                 (i & 2) ? maximum.y : minimum.y,
                                 (i & 4) ? maximum.z : minimum.z);
        }
        AddBoxEdges(corner, color);
    }

    void Clear() { pending_.clear(); }
    bool Empty() const { return pending_.empty(); }
    size_t LineCount() const { return pending_.size() / 2; }

    // Submits everything queued since the last Clear. Returns the vertex count
    // drawn, which is zero when the overlay is off or nothing was queued.
    UINT Render(const Scene& scene, bool hdrTarget, bool msaaTarget) {
        if (!initialized || pending_.empty()) return 0;

        const UINT frame = g_dx12.frameIndex % FRAME_COUNT;
        const UINT frameBase = frame * MaxVertices;
        const UINT count = static_cast<UINT>(
            (std::min)(static_cast<size_t>(MaxVertices), pending_.size()));
        std::memcpy(vertices_.mappedData + frameBase, pending_.data(),
                    static_cast<size_t>(count) * sizeof(CollisionDebugVertexDX12));

        CollisionDebugFrameDX12 frameData = {};
        frameData.viewProjection = XMMatrixTranspose(
            scene.GetViewMatrix() * scene.GetProjectionMatrix());
        frames_.CopyData(frame, frameData);

        ID3D12GraphicsCommandList* commandList = g_dx12.commandList.Get();
        commandList->SetGraphicsRootSignature(rootSignature_.Get());
        commandList->SetGraphicsRootConstantBufferView(
            0, frames_.GetGPUAddress(frame));
        commandList->SetPipelineState(
            pipelines_[hdrTarget ? 1u : (msaaTarget ? 2u : 0u)].Get());
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        D3D12_VERTEX_BUFFER_VIEW view = {};
        view.BufferLocation = vertices_.GetGPUAddress(frameBase);
        view.SizeInBytes = count * sizeof(CollisionDebugVertexDX12);
        view.StrideInBytes = sizeof(CollisionDebugVertexDX12);
        commandList->IASetVertexBuffers(0, 1, &view);
        commandList->DrawInstanced(count, 1, 0, 0);
        return count;
    }

private:
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipelines_[3];
    UploadBuffer<CollisionDebugVertexDX12> vertices_;
    UploadBuffer<CollisionDebugFrameDX12> frames_;
    std::vector<CollisionDebugVertexDX12> pending_;

    void AddLine(const XMFLOAT3& from, const XMFLOAT3& to,
                 const XMFLOAT4& color) {
        if (pending_.size() + 2 > MaxVertices) return;
        pending_.push_back({ from, color });
        pending_.push_back({ to, color });
    }

    // Corner indexing is a bit field: bit 0 = +X, bit 1 = +Y, bit 2 = +Z. Two
    // corners share an edge exactly when their indices differ in one bit, which
    // is what each group below walks.
    void AddBoxEdges(const XMFLOAT3 corner[8], const XMFLOAT4& color) {
        for (int i = 0; i < 8; ++i) {
            if (!(i & 1)) AddLine(corner[i], corner[i | 1], color);
            if (!(i & 2)) AddLine(corner[i], corner[i | 2], color);
            if (!(i & 4)) AddLine(corner[i], corner[i | 4], color);
        }
    }

    bool CreatePipelines(ID3DBlob* vs, ID3DBlob* ps, ID3DBlob* hdrPs) {
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.InputLayout = { layout, _countof(layout) };
        desc.pRootSignature = rootSignature_.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        // Alpha blending so a volume can be drawn semi-transparent where it
        // would otherwise bury the geometry it wraps.
        desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        // Depth-tested so volumes are occluded by the world and read as being
        // in the scene, but no depth write: overlapping debug lines must not
        // hide each other, and nothing afterwards should sort against them.
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&pipelines_[0])))) return false;

        desc.PS = { hdrPs->GetBufferPointer(), hdrPs->GetBufferSize() };
        desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&pipelines_[1])))) return false;

        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = MSAADX12::SampleCount;
        desc.RasterizerState.MultisampleEnable = TRUE;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&pipelines_[2])))) return false;
        return true;
    }
};

#endif
