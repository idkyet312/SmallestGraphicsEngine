#ifndef SHADOW_MAP_DX12_H
#define SHADOW_MAP_DX12_H

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include "ShaderDX12.h"
#include "Scene.h"
#include "ForwardRenderer.h"
#include "SceneGraph.h"
#include "SkinnedEnemy.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <fstream>
#include <sstream>

// 4096 rather than 2048: at 2048 a cascade-1/2 texel covered enough wall that
// building edges rasterised as visible stair-steps. Doubling the axis quarters
// the world size of a texel and removes most of that without touching the PCF
// kernel. Costs 3 cascades x 4096^2 x 4 bytes = 192 MB of depth (up from 48).
//
// Nothing else needs updating for this: the per-cascade texelWorld used for
// slope bias is derived from this value in ComputeCascadeMatrices, and the PCF
// tap spacing comes from the live resource via GetDesc().Width.
static const UINT SHADOW_MAP_SIZE = 4096;
static const UINT SHADOW_MAX_DRAWS = 12288;
static const UINT SHADOW_MAX_INSTANCES = 12288;

static constexpr D3D12_RESOURCE_STATES SHADOW_SHADER_READ_STATE =
    static_cast<D3D12_RESOURCE_STATES>(
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

class DepthOnlyShaderDX12 {
public:
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12PipelineState> grassPipelineState;
    ComPtr<ID3D12PipelineState> palmPipelineState;
    ComPtr<ID3D12PipelineState> palmAlphaPipelineState;
    ComPtr<ID3D12DescriptorHeap> palmTextureHeap;
    UploadBuffer<MatrixBufferDX12> matrixBuffer;
    UploadBuffer<MeshInstanceDataDX12> instanceBuffer;
    UINT currentDrawCall = 0;
    UINT currentInstance = 0;
    UINT batchesThisFrame = 0;
    UINT instancesThisFrame = 0;
    bool loaded = false;
    PalmWindFrameDX12 palmWindFrame{};
    ID3D12Resource* boundPalmTexture = nullptr;

    bool Load(const char* vertexPath) {
        std::ifstream vsFile(vertexPath);
        if (!vsFile.is_open()) {
            std::cerr << "Failed to open shadow VS: " << vertexPath << std::endl;
            return false;
        }

        std::stringstream vsStream;
        vsStream << vsFile.rdbuf();
        std::string vsCode = vsStream.str();

        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = ShaderCacheDX12::CompileCached(vsCode.c_str(), vsCode.length(), vertexPath,
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_0",
            compileFlags, 0, &vsBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "Shadow VS error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        D3D12_DESCRIPTOR_RANGE palmTextureRange = {};
        palmTextureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        palmTextureRange.NumDescriptors = 1;
        palmTextureRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER rootParams[9] = {};
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[0].Descriptor.ShaderRegister = 0;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams[1].Constants.ShaderRegister = 1;
        rootParams[1].Constants.Num32BitValues = 1;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[2].Descriptor.ShaderRegister = 12;
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[3].Descriptor.ShaderRegister = 13;
        rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams[4].Constants.ShaderRegister = 6;
        rootParams[4].Constants.Num32BitValues = 19;
        rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[5].Descriptor.ShaderRegister = 6;
        rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams[6].Constants.ShaderRegister = 7;
        rootParams[6].Constants.Num32BitValues = 1;
        rootParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParams[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[7].Descriptor.ShaderRegister = 14;
        rootParams[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParams[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[8].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[8].DescriptorTable.pDescriptorRanges = &palmTextureRange;
        rootParams[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC palmSampler = {};
        palmSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        palmSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        palmSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        palmSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        palmSampler.MaxAnisotropy = 1;
        palmSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        palmSampler.MaxLOD = D3D12_FLOAT32_MAX;
        palmSampler.ShaderRegister = 0;
        palmSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = _countof(rootParams);
        rootSigDesc.pParameters = rootParams;
        rootSigDesc.NumStaticSamplers = 1;
        rootSigDesc.pStaticSamplers = &palmSampler;
        rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sigBlob;
        errorBlob.Reset();
        hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "Shadow root sig error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        hr = g_dx12.device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
            sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
        if (FAILED(hr)) return false;

        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.pRootSignature = rootSignature.Get();
        psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
        psoDesc.RasterizerState.DepthBias = 1000;
        psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
        psoDesc.RasterizerState.SlopeScaledDepthBias = 1.0f;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;
        psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
        psoDesc.BlendState.IndependentBlendEnable = FALSE;
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 0;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count = 1;

        hr = g_dx12.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState));
        if (FAILED(hr)) {
            std::cerr << "Failed to create shadow PSO, HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }

        std::ifstream grassVsFile("shaders/grass_shadow_vs.hlsl");
        if (!grassVsFile.is_open()) return false;
        std::stringstream grassVsStream;
        grassVsStream << grassVsFile.rdbuf();
        const std::string grassVsCode = grassVsStream.str();
        ComPtr<ID3DBlob> grassVsBlob;
        errorBlob.Reset();
        hr = ShaderCacheDX12::CompileCached(grassVsCode.c_str(), grassVsCode.length(),
            "shaders/grass_shadow_vs.hlsl", nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_0",
            compileFlags, 0, &grassVsBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "Grass shadow VS error: "
                                     << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }
        D3D12_INPUT_ELEMENT_DESC grassInput[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        psoDesc.InputLayout = { grassInput, _countof(grassInput) };
        psoDesc.VS = { grassVsBlob->GetBufferPointer(), grassVsBlob->GetBufferSize() };
        hr = g_dx12.device->CreateGraphicsPipelineState(
            &psoDesc, IID_PPV_ARGS(&grassPipelineState));
        if (FAILED(hr)) return false;

        auto compileShader = [&](const char* path, const char* target,
                                 ComPtr<ID3DBlob>& blob) {
            std::ifstream file(path);
            if (!file.is_open()) return false;
            std::stringstream stream;
            stream << file.rdbuf();
            const std::string code = stream.str();
            errorBlob.Reset();
            const HRESULT result = ShaderCacheDX12::CompileCached(
                code.c_str(), code.length(), path, nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", target,
                compileFlags, 0, &blob, &errorBlob);
            if (FAILED(result) && errorBlob)
                std::cerr << "Palm shadow shader error: "
                          << (char*)errorBlob->GetBufferPointer() << std::endl;
            return SUCCEEDED(result);
        };
        ComPtr<ID3DBlob> palmVsBlob;
        ComPtr<ID3DBlob> palmPsBlob;
        if (!compileShader("shaders/palm_shadow_vs.hlsl", "vs_5_0", palmVsBlob) ||
            !compileShader("shaders/palm_shadow_ps.hlsl", "ps_5_0", palmPsBlob))
            return false;
        D3D12_INPUT_ELEMENT_DESC palmInput[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        psoDesc.InputLayout = { palmInput, _countof(palmInput) };
        psoDesc.VS = { palmVsBlob->GetBufferPointer(), palmVsBlob->GetBufferSize() };
        psoDesc.PS = {};
        hr = g_dx12.device->CreateGraphicsPipelineState(
            &psoDesc, IID_PPV_ARGS(&palmPipelineState));
        if (FAILED(hr)) return false;
        psoDesc.PS = { palmPsBlob->GetBufferPointer(), palmPsBlob->GetBufferSize() };
        hr = g_dx12.device->CreateGraphicsPipelineState(
            &psoDesc, IID_PPV_ARGS(&palmAlphaPipelineState));
        if (FAILED(hr)) return false;

        D3D12_DESCRIPTOR_HEAP_DESC palmHeapDesc = {};
        palmHeapDesc.NumDescriptors = 1;
        palmHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        palmHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &palmHeapDesc, IID_PPV_ARGS(&palmTextureHeap)))) return false;

        if (!matrixBuffer.Create(FRAME_COUNT * SHADOW_MAX_DRAWS)) return false;
        if (!instanceBuffer.Create(FRAME_COUNT * SHADOW_MAX_INSTANCES)) return false;
        loaded = true;
        return true;
    }

    void BeginFrame() {
        currentDrawCall = 0;
        currentInstance = 0;
        batchesThisFrame = 0;
        instancesThisFrame = 0;
    }

    void Use() {
        g_dx12.commandList->SetGraphicsRootSignature(rootSignature.Get());
        g_dx12.commandList->SetPipelineState(pipelineState.Get());
    }

    void SetMatrices(const XMMATRIX& model, const XMMATRIX& lightSpace,
                     const XMFLOAT4& palmRoot = {}) {
        UINT index = g_dx12.frameIndex * SHADOW_MAX_DRAWS + std::min(currentDrawCall, SHADOW_MAX_DRAWS - 1);
        MatrixBufferDX12 data = {};
        data.model = XMMatrixTranspose(model);
        data.view = XMMatrixIdentity();
        data.projection = XMMatrixIdentity();
        data.lightSpaceMatrix = XMMatrixTranspose(lightSpace);
        data.modelView = data.model;
        data.modelViewProjection = XMMatrixTranspose(model * lightSpace);
        data.palmWind = palmWindFrame.wind;
        data.palmPrimary = palmWindFrame.primary;
        data.palmSecondary = palmWindFrame.secondary;
        data.palmPreviousPrimary = palmWindFrame.previousPrimary;
        data.palmPreviousSecondary = palmWindFrame.previousSecondary;
        data.palmParams = palmWindFrame.params;
        data.palmRoot = palmRoot;
        matrixBuffer.CopyData(index, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(0, matrixBuffer.GetGPUAddress(index));
        const UINT disabled = 0;
        g_dx12.commandList->SetGraphicsRoot32BitConstants(1, 1, &disabled, 0);
        g_dx12.commandList->SetGraphicsRoot32BitConstants(6, 1, &disabled, 0);
        g_dx12.commandList->SetGraphicsRootShaderResourceView(7,
            instanceBuffer.GetGPUAddress(
                g_dx12.frameIndex * SHADOW_MAX_INSTANCES));
    }

    void SetSkinning(D3D12_GPU_VIRTUAL_ADDRESS palette, D3D12_GPU_VIRTUAL_ADDRESS skin) {
        const UINT instanceDisabled = 0;
        g_dx12.commandList->SetGraphicsRoot32BitConstants(
            6, 1, &instanceDisabled, 0);
        const UINT enabled = palette && skin ? 1u : 0u;
        g_dx12.commandList->SetGraphicsRoot32BitConstants(1, 1, &enabled, 0);
        if (enabled) {
            g_dx12.commandList->SetGraphicsRootShaderResourceView(2, palette);
            g_dx12.commandList->SetGraphicsRootShaderResourceView(3, skin);
        }
    }

    bool SetInstances(const std::vector<XMMATRIX>& models) {
        if (models.size() < 2 ||
            models.size() > SHADOW_MAX_INSTANCES - currentInstance) return false;
        const UINT frameBase = g_dx12.frameIndex * SHADOW_MAX_INSTANCES;
        const UINT first = currentInstance;
        for (const XMMATRIX& model : models) {
            MeshInstanceDataDX12 instance = {};
            XMStoreFloat4x4(&instance.model, XMMatrixTranspose(model));
            instanceBuffer.CopyData(frameBase + currentInstance, instance);
            ++currentInstance;
        }
        const UINT enabled = 1;
        g_dx12.commandList->SetGraphicsRoot32BitConstants(6, 1, &enabled, 0);
        g_dx12.commandList->SetGraphicsRootShaderResourceView(
            7, instanceBuffer.GetGPUAddress(frameBase + first));
        ++batchesThisFrame;
        instancesThisFrame += static_cast<UINT>(models.size());
        return true;
    }

    void UseGrass() {
        g_dx12.commandList->SetGraphicsRootSignature(rootSignature.Get());
        g_dx12.commandList->SetPipelineState(grassPipelineState.Get());
    }

    void SetPalmWindFrame(const PalmWindFrameDX12& frame) {
        palmWindFrame = frame;
    }

    void UsePalm(bool alphaCutout) {
        g_dx12.commandList->SetGraphicsRootSignature(rootSignature.Get());
        g_dx12.commandList->SetPipelineState(
            alphaCutout ? palmAlphaPipelineState.Get() : palmPipelineState.Get());
    }

    void SetPalmTexture(ID3D12Resource* texture) {
        if (!texture || !palmTextureHeap) return;
        if (boundPalmTexture != texture) {
            const D3D12_RESOURCE_DESC desc = texture->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = desc.Format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = desc.MipLevels;
            g_dx12.device->CreateShaderResourceView(
                texture, &srv, palmTextureHeap->GetCPUDescriptorHandleForHeapStart());
            boundPalmTexture = texture;
        }
        ID3D12DescriptorHeap* heaps[] = { palmTextureHeap.Get() };
        g_dx12.commandList->SetDescriptorHeaps(1, heaps);
        g_dx12.commandList->SetGraphicsRootDescriptorTable(
            8, palmTextureHeap->GetGPUDescriptorHandleForHeapStart());
    }

    void SetGrass(const GrassField::Params& params,
                  D3D12_GPU_VIRTUAL_ADDRESS instances) {
        g_dx12.commandList->SetGraphicsRoot32BitConstants(4, 19, &params, 0);
        g_dx12.commandList->SetGraphicsRootShaderResourceView(5, instances);
    }

    void NextDrawCall() {
        if (currentDrawCall + 1 < SHADOW_MAX_DRAWS) currentDrawCall++;
    }
};

inline void DrawSceneNodeShadow(const std::shared_ptr<SceneNode>& node,
                                DepthOnlyShaderDX12& shader,
                                const XMMATRIX& worldTransform,
                                const XMMATRIX& lightSpace,
                                D3D12_GPU_VIRTUAL_ADDRESS bonePalette = 0) {
    if (!node) return;

    if (node->mesh) {
        XMMATRIX model = XMLoadFloat4x4(&node->globalTransform) * worldTransform;
        shader.SetMatrices(model, lightSpace);

        for (const auto& prim : node->mesh->primitives) {
            if (prim.vbv.BufferLocation == 0) continue;
            if (prim.material && prim.material->baseColorFactor.w < 0.5f) continue;

            // Pose the shadow from the same palette as the colour pass, so a
            // spinning rotor casts a spinning shadow instead of a frozen one.
            shader.SetSkinning(
                (bonePalette && prim.skinBuffer) ? bonePalette : 0,
                (bonePalette && prim.skinBuffer)
                    ? prim.skinBuffer->GetGPUVirtualAddress() : 0);

            g_dx12.commandList->IASetVertexBuffers(0, 1, &prim.vbv);
            g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            if (prim.ibv.BufferLocation != 0) {
                g_dx12.commandList->IASetIndexBuffer(&prim.ibv);
                g_dx12.commandList->DrawIndexedInstanced(prim.indexCount, 1, 0, 0, 0);
            } else {
                g_dx12.commandList->DrawInstanced((UINT)(prim.vertices.size() / 12), 1, 0, 0);
            }
            shader.NextDrawCall();
        }
    }

    for (auto& child : node->children) {
        DrawSceneNodeShadow(child, shader, worldTransform, lightSpace,
                            bonePalette);
    }
}

inline bool SceneNodeSupportsShadowInstancing(const std::shared_ptr<SceneNode>& node) {
    if (!node) return false;
    if (node->mesh) for (const auto& prim : node->mesh->primitives) {
        if (!prim.vbv.BufferLocation || prim.skinBuffer ||
            (prim.material && prim.material->baseColorFactor.w < 0.5f)) return false;
    }
    for (const auto& child : node->children)
        if (!SceneNodeSupportsShadowInstancing(child)) return false;
    return true;
}

enum class ShadowScenePart {
    All,
    Static,
    Dynamic
};

inline void DrawSceneNodeShadowInstances(const std::shared_ptr<SceneNode>& node,
                                         DepthOnlyShaderDX12& shader,
                                         const std::vector<XMMATRIX>& worlds,
                                         const XMMATRIX& lightSpace) {
    if (!node || worlds.empty()) return;
    if (worlds.size() < 2 || !SceneNodeSupportsShadowInstancing(node)) {
        for (const XMMATRIX& world : worlds)
            DrawSceneNodeShadow(node, shader, world, lightSpace);
        return;
    }

    if (node->mesh) {
        std::vector<XMMATRIX> models;
        models.reserve(worlds.size());
        const XMMATRIX local = XMLoadFloat4x4(&node->globalTransform);
        for (const XMMATRIX& world : worlds) models.push_back(local * world);
        for (const auto& prim : node->mesh->primitives) {
            shader.SetMatrices(XMMatrixIdentity(), lightSpace);
            if (!shader.SetInstances(models)) {
                for (const XMMATRIX& model : models) {
                    shader.SetMatrices(model, lightSpace);
                    g_dx12.commandList->IASetVertexBuffers(0, 1, &prim.vbv);
                    g_dx12.commandList->IASetPrimitiveTopology(
                        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    if (prim.ibv.BufferLocation) {
                        g_dx12.commandList->IASetIndexBuffer(&prim.ibv);
                        g_dx12.commandList->DrawIndexedInstanced(
                            prim.indexCount, 1, 0, 0, 0);
                    } else {
                        g_dx12.commandList->DrawInstanced(
                            static_cast<UINT>(prim.vertices.size() / 12), 1, 0, 0);
                    }
                    shader.NextDrawCall();
                }
                continue;
            }
            g_dx12.commandList->IASetVertexBuffers(0, 1, &prim.vbv);
            g_dx12.commandList->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const UINT count = static_cast<UINT>(models.size());
            if (prim.ibv.BufferLocation) {
                g_dx12.commandList->IASetIndexBuffer(&prim.ibv);
                g_dx12.commandList->DrawIndexedInstanced(
                    prim.indexCount, count, 0, 0, 0);
            } else {
                g_dx12.commandList->DrawInstanced(
                    static_cast<UINT>(prim.vertices.size() / 12), count, 0, 0);
            }
            shader.NextDrawCall();
        }
    }
    for (const auto& child : node->children)
        DrawSceneNodeShadowInstances(child, shader, worlds, lightSpace);
}

class ShadowMapDX12 {
public:
    ComPtr<ID3D12Resource> shadowMap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12Resource> cachedFarShadowMap;
    ComPtr<ID3D12DescriptorHeap> cachedFarDsvHeap;
    // Spot atlas: its own texture and DSV heap so the sun cascades' resource,
    // views and barriers are untouched by this feature.
    ComPtr<ID3D12Resource> spotShadowMap;
    ComPtr<ID3D12DescriptorHeap> spotDsvHeap;
    DepthOnlyShaderDX12 depthShader;
    UINT size = SHADOW_MAP_SIZE;
    bool initialized = false;
    UINT cachedCascadesThisFrame = 0;
    UINT refreshedCascadesThisFrame = 0;

    void InvalidateCachedCascades() {
        farCacheValid.fill(false);
        cacheViewValid = false;
    }

    bool Init(UINT mapSize = SHADOW_MAP_SIZE) {
        size = mapSize;
        if (!CreateShadowMap()) return false;
        if (!CreateSpotShadowMap()) return false;
        if (!depthShader.Load("shaders/depth_vs.hlsl")) return false;
        initialized = true;
        std::cout << "Shadow map ready (" << size << "x" << size << ")" << std::endl;
        return true;
    }

    ID3D12Resource* GetResource() const {
        return shadowMap.Get();
    }

    ID3D12Resource* GetSpotResource() const {
        return spotShadowMap.Get();
    }

    UINT CachedCascadesThisFrame() const { return cachedCascadesThisFrame; }
    UINT RefreshedCascadesThisFrame() const {
        return refreshedCascadesThisFrame;
    }

    XMMATRIX ComputeLightSpace(const Scene& scene) const {
        XMVECTOR lightDir = XMLoadFloat3(&scene.lightPos);
        lightDir = XMVector3Normalize(lightDir);

        XMVECTOR target = XMLoadFloat3(&scene.shadowCenter);
        float distance = std::max(scene.shadowDistance, 1.0f);
        XMVECTOR lightPosition = target + lightDir * distance;

        XMVECTOR up = XMVectorSet(0, 1, 0, 0);
        if (std::fabs(XMVectorGetX(XMVector3Dot(lightDir, up))) > 0.95f) {
            up = XMVectorSet(0, 0, 1, 0);
        }

        float ortho = std::max(scene.shadowOrthoSize, 1.0f);
        float farPlane = std::max(scene.shadowFarPlane, distance + ortho);
        XMMATRIX lightView = XMMatrixLookAtLH(lightPosition, target, up);
        XMMATRIX lightProj = XMMatrixOrthographicLH(ortho * 2.0f, ortho * 2.0f, 0.1f, farPlane);
        return lightView * lightProj;
    }

    std::array<XMMATRIX, SHADOW_CASCADE_COUNT> ComputeCascadeMatrices(
        const Scene& scene) const {
        const float nearDepth = (std::max)(scene.cameraNear, 0.05f);
        const float farDepth = (std::min)(scene.EffectiveCameraFarPlane(),
            (std::max)(180.0f, scene.shadowFarPlane * 2.0f));
        constexpr float splitLambda = 0.65f;
        float splits[SHADOW_CASCADE_COUNT] = {};
        for (UINT i = 0; i < SHADOW_CASCADE_COUNT; ++i) {
            const float p = static_cast<float>(i + 1) / SHADOW_CASCADE_COUNT;
            const float logarithmic = nearDepth * std::pow(farDepth / nearDepth, p);
            const float uniform = nearDepth + (farDepth - nearDepth) * p;
            splits[i] = splitLambda * logarithmic + (1.0f - splitLambda) * uniform;
        }
        g_shadowCascadeSplits = { splits[0], splits[1], splits[2], 0.0f };
        const XMVECTOR cameraPos = XMLoadFloat3(&scene.camera.Position);
        const XMVECTOR front = XMVector3Normalize(XMLoadFloat3(&scene.camera.Front));
        const XMVECTOR up = XMVector3Normalize(XMLoadFloat3(&scene.camera.Up));
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, front));
        const float tanHalfFov = std::tan(
            XMConvertToRadians(scene.EffectiveCameraFOV()) * 0.5f);
        const float aspect = static_cast<float>(g_dx12.screenWidth) /
            (std::max)(1.0f, static_cast<float>(g_dx12.screenHeight));
        XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&scene.lightPos));
        XMVECTOR lightUp = XMVectorSet(0, 1, 0, 0);
        if (std::fabs(XMVectorGetX(XMVector3Dot(lightDir, lightUp))) > 0.95f)
            lightUp = XMVectorSet(0, 0, 1, 0);
        std::array<XMMATRIX, SHADOW_CASCADE_COUNT> result;
        float segmentNear = nearDepth;
        for (UINT cascade = 0; cascade < SHADOW_CASCADE_COUNT; ++cascade) {
            const float segmentFar = splits[cascade];
            std::array<XMVECTOR, 8> corners;
            UINT corner = 0;
            for (float depth : { segmentNear, segmentFar }) {
                const float halfY = tanHalfFov * depth;
                const float halfX = halfY * aspect;
                const XMVECTOR center = cameraPos + front * depth;
                for (float y : { -1.0f, 1.0f })
                    for (float x : { -1.0f, 1.0f })
                        corners[corner++] = center + right * (x * halfX) + up * (y * halfY);
            }
            XMVECTOR center = XMVectorZero();
            for (XMVECTOR p : corners) center += p;
            center /= 8.0f;
            const XMMATRIX lightView = XMMatrixLookAtLH(
                center + lightDir * 260.0f, center, lightUp);
            XMFLOAT3 minimum(FLT_MAX, FLT_MAX, FLT_MAX);
            XMFLOAT3 maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            for (XMVECTOR p : corners) {
                XMFLOAT3 q;
                XMStoreFloat3(&q, XMVector3TransformCoord(p, lightView));
                minimum.x = (std::min)(minimum.x, q.x);
                minimum.y = (std::min)(minimum.y, q.y);
                minimum.z = (std::min)(minimum.z, q.z);
                maximum.x = (std::max)(maximum.x, q.x);
                maximum.y = (std::max)(maximum.y, q.y);
                maximum.z = (std::max)(maximum.z, q.z);
            }
            const float extent = (std::max)(maximum.x - minimum.x,
                                             maximum.y - minimum.y) * 0.55f;
            float centerX = (minimum.x + maximum.x) * 0.5f;
            float centerY = (minimum.y + maximum.y) * 0.5f;
            const float texel = (extent * 2.0f) / static_cast<float>(size);
            centerX = std::floor(centerX / texel) * texel;
            centerY = std::floor(centerY / texel) * texel;
            const float nearPlane = (std::max)(0.1f, minimum.z - 80.0f);
            const float farPlane = maximum.z + 80.0f;
            result[cascade] = lightView * XMMatrixOrthographicOffCenterLH(
                centerX - extent, centerX + extent,
                centerY - extent, centerY + extent, nearPlane, farPlane);
            (&g_shadowCascadeTexelWorld.x)[cascade] = texel;
            (&g_shadowCascadeDepthRange.x)[cascade] = farPlane - nearPlane;
            segmentNear = segmentFar;
        }
        return result;
    }

    // The shadow scene, recorded once per light-space matrix.
    //
    // Shared by the sun cascades and the spot casters so a caster can never
    // drift out of step with what the cascades consider a shadow caster: one
    // list of draws, two projections. spotCull switches the frustum test
    // between the orthographic cascade box and a perspective cone.
    void DrawShadowScene(Scene& scene,
                         const GeometryBuffers& geo,
                         const std::vector<PrefabRenderBatch>& prefabRenderBatches,
                         const std::shared_ptr<SceneNode>& crateModel,
                         const std::vector<std::unique_ptr<SkinnedEnemy>>* bandits,
                         const XMMATRIX& lightSpace,
                         bool spotCull,
                         ShaderDX12* terrainShader = nullptr,
                         ShadowScenePart part = ShadowScenePart::All) {
        const bool drawStatic = part != ShadowScenePart::Dynamic;
        const bool drawDynamic = part != ShadowScenePart::Static;
        // Terrain first, while the main graphics root signature can still be
        // bound: it runs on the mesh-shader pipeline and needs slots that
        // DepthOnlyShaderDX12 does not declare. depthShader.Use() below then
        // takes the signature back for every other caster.
        if (drawStatic && terrainShader && g_terrain.supported &&
            !g_emptyLevelMode) {
            // The previous caster leaves DepthOnlyShaderDX12's root signature
            // bound. ShaderDX12 caches its binding state, so invalidate that
            // cache before switching back; otherwise cascade 1 writes main-root
            // parameters through the depth root signature inside the driver.
            terrainShader->InvalidateGraphicsRootBinding();
            terrainShader->Use(false);
            terrainShader->SetMatrices(XMMatrixIdentity(), XMMatrixIdentity(),
                                       lightSpace, lightSpace);
            // NOTE: do not call SetCamera here to re-centre the clipmap on the
            // lamp. cameraBuffer is sized FRAME_COUNT -- ONE slot per frame,
            // unlike matrixBuffer's per-draw-call slots -- so writing it mid
            // frame overwrites the value every other pass in the same frame
            // reads once the GPU runs the list, including the final shading.
            // Centring the shadow clipmap on the light needs a per-draw camera
            // slot (or the origin passed through TerrainParams) first.
            g_terrain.DrawShadow(*terrainShader, CurrentTerrainParams());
        }
        depthShader.Use();

        if (drawStatic && crateModel) {
            DrawSceneNodeShadow(crateModel, depthShader, XMMatrixIdentity(), lightSpace);
        }
        if (drawStatic && !crateModel && !g_destruction.IsInitialized()) {
            XMMATRIX model = scene.cube1.GetModelMatrix();
            depthShader.SetMatrices(model, lightSpace);
            DrawCube(geo);
            depthShader.NextDrawCall();
        }

        // Detached chunks keep casting shadows from their live physics poses.
        if (drawDynamic && scene.useDestruction &&
            g_destruction.IsInitialized()) {
            // Anything outside the light's frustum cannot land in the shadow
            // map; skip it before recording the draw. Handles both projections:
            // the sun cascades are orthographic (w stays 1, so the divide is a
            // no-op) while the spot casters are perspective, where w is the
            // view depth and the divide is what makes the test correct.
            const float ortho = (std::max)(scene.shadowOrthoSize, 1.0f);
            const float distance = (std::max)(scene.shadowDistance, 1.0f);
            const float depthRange =
                (std::max)(scene.shadowFarPlane, distance + ortho) - 0.1f;
            const auto inLightBox = [&](const XMFLOAT3& c, float r) {
                if (r <= 0.0f) return true;
                XMFLOAT4 ndc;
                XMStoreFloat4(&ndc, XMVector4Transform(
                    XMVectorSet(c.x, c.y, c.z, 1.0f), lightSpace));
                // Behind the light entirely: for a perspective frustum w <= 0
                // means the sphere centre is at or behind the eye, and the
                // radius is the only thing that can still reach into view.
                if (ndc.w <= 0.0f) return spotCull && r > -ndc.w;
                const float invW = spotCull ? 1.0f / ndc.w : 1.0f;
                const float rx = spotCull ? (r / ndc.w) : (r / ortho);
                const float rz = spotCull ? (r / ndc.w) : (r / depthRange);
                const float x = ndc.x * invW;
                const float y = ndc.y * invW;
                const float z = ndc.z * invW;
                return std::fabs(x) <= 1.0f + rx &&
                       std::fabs(y) <= 1.0f + rx &&
                       z + rz >= 0.0f && z - rz <= 1.0f;
            };
            const auto& batches = g_destruction.GetRenderBatches();
            if (!batches.empty()) {
                for (const DestructionRenderBatch& batch : batches) {
                    if (!inLightBox(batch.sphereCenter, batch.sphereRadius)) continue;
                    DrawSceneNodeShadow(batch.shadowNode, depthShader,
                        XMLoadFloat4x4(&batch.transform), lightSpace);
                }
            }
            for (const DestructionRenderItem& item : g_destruction.GetRenderItems()) {
                    if (!inLightBox(item.sphereCenter, item.sphereRadius)) continue;
                    DrawSceneNodeShadow(item.node, depthShader,
                        XMLoadFloat4x4(&item.transform), lightSpace);
            }
            for (const RagdollRenderItem& item : g_destruction.GetRagdollRenderItems()) {
                if (!inLightBox(item.sphereCenter, item.sphereRadius)) continue;
                depthShader.SetMatrices(XMLoadFloat4x4(&item.transform), lightSpace);
                DrawCube(geo);
                depthShader.NextDrawCall();
            }
        }

        if (drawDynamic && !g_emptyLevelMode && scene.cube2.visible) {
            XMMATRIX model = scene.cube2.GetModelMatrix();
            depthShader.SetMatrices(model, lightSpace);
            DrawCube(geo);
            depthShader.NextDrawCall();
        }

        if (drawDynamic && !g_emptyLevelMode && !g_trainingRangeMode &&
            g_humveeModel &&
            g_levelPlacesHumvee) {
            for (size_t index = 0; index < LevelHumveeCount(); ++index) {
                PrepareHumveeModelForRender(index);
                DrawSceneNodeShadow(
                    g_humveeShadowModel ? g_humveeShadowModel : g_humveeModel,
                    depthShader, HumveeWorldMatrix(index), lightSpace);
            }
            if (g_stressTestMode) {
                PrepareHumveeModelForRender(0);
                DrawSceneNodeShadow(
                    g_humveeShadowModel ? g_humveeShadowModel : g_humveeModel,
                    depthShader, SecondaryHumveeWorldMatrix(), lightSpace);
            }
        }

        if (drawDynamic && !g_emptyLevelMode && !g_trainingRangeMode &&
            g_boatModel) {
            std::vector<XMMATRIX> boatTransforms = { BoatWorldMatrix() };
            DrawSceneNodeShadowInstances(
                g_boatShadowModel ? g_boatShadowModel : g_boatModel,
                depthShader, boatTransforms, lightSpace);
        }

        if (drawDynamic && !g_emptyLevelMode && g_insertionBoatModel &&
            InsertionBoatVisible()) {
            std::vector<XMMATRIX> insertionBoatTransforms = {
                InsertionBoatWorldMatrix() };
            DrawSceneNodeShadowInstances(
                g_insertionBoatShadowModel ? g_insertionBoatShadowModel
                                           : g_insertionBoatModel,
                depthShader, insertionBoatTransforms, lightSpace);
        }

        if (drawDynamic && !g_emptyLevelMode && g_insertionBoatModel &&
            EscapeBoatVisible()) {
            std::vector<XMMATRIX> escapeBoatTransforms = {
                EscapeBoatWorldMatrix() };
            DrawSceneNodeShadowInstances(
                g_insertionBoatShadowModel ? g_insertionBoatShadowModel
                                           : g_insertionBoatModel,
                depthShader, escapeBoatTransforms, lightSpace);
        }

        if (drawDynamic && !g_emptyLevelMode && g_blackHawkModel &&
            BlackHawkVisible()) {
            // Skinned, so it takes the per-node path with the rotor palette
            // rather than the instanced fast path.
            DrawSceneNodeShadow(g_blackHawkModel, depthShader,
                BlackHawkWorldMatrix(), lightSpace, UploadBlackHawkPalette());
        }

        // Enemy gunships. Previously absent from this pass entirely, so an
        // aircraft cast nothing -- most obviously under its own searchlight,
        // which lit the ground straight through the fuselage carrying it.
        // Mirrors the draw gates in RenderForward.
        if (drawDynamic && !g_emptyLevelMode && g_helicopterModel &&
            scene.showHelicopter) {
            DrawSceneNodeShadow(g_helicopterModel, depthShader,
                HelicopterWorldMatrix(), lightSpace);
            if (SecondaryHelicopterVisible())
                DrawSceneNodeShadow(
                    g_secondaryHelicopterModel ? g_secondaryHelicopterModel
                                               : g_helicopterModel,
                    depthShader, SecondaryHelicopterWorldMatrix(), lightSpace);
        }

        if (drawStatic) for (const PrefabRenderBatch& batch : prefabRenderBatches) {
            if (batch.model && batch.castShadow && !batch.transforms.empty())
                DrawSceneNodeShadowInstances(batch.model, depthShader,
                    batch.transforms, lightSpace);
        }

        // Palm shadows use the same GPU wind as the visible mesh. Leaf cards run
        // an alpha-tested depth pass so fronds cast silhouettes, not solid quads.
        if (drawDynamic && !g_emptyLevelMode && g_trees.IsInitialized()) {
            for (const TreeItem& item : g_trees.GetItems()) {
                std::shared_ptr<SceneMesh> slice = item.meshOverride;
                if (!slice) {
                    if (item.crown) slice = PalmModel::Crown();
                    else if (item.segment >= 0 &&
                             item.segment < static_cast<int>(
                                 PalmModel::TrunkSlices().size()))
                        slice = PalmModel::TrunkSlices()[item.segment].mesh;
                }

                const XMMATRIX model = XMLoadFloat4x4(&item.transform);
                if (!slice) {
                    depthShader.Use();
                    depthShader.SetMatrices(model, lightSpace);
                    DrawCube(geo);
                    depthShader.NextDrawCall();
                    continue;
                }

                for (const MeshPrimitive& prim : slice->primitives) {
                    if (!prim.vbv.BufferLocation) continue;
                    const bool alphaCutout = prim.material &&
                        prim.material->alphaCutout &&
                        prim.material->baseColorTexture;
                    depthShader.UsePalm(alphaCutout);
                    if (alphaCutout)
                        depthShader.SetPalmTexture(
                            prim.material->baseColorTexture.Get());
                    depthShader.SetMatrices(model, lightSpace, item.palmWindRoot);
                    g_dx12.commandList->IASetVertexBuffers(0, 1, &prim.vbv);
                    g_dx12.commandList->IASetPrimitiveTopology(
                        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    if (prim.ibv.BufferLocation) {
                        g_dx12.commandList->IASetIndexBuffer(&prim.ibv);
                        g_dx12.commandList->DrawIndexedInstanced(
                            prim.indexCount, 1, 0, 0, 0);
                    } else {
                        g_dx12.commandList->DrawInstanced(
                            static_cast<UINT>(prim.vertices.size() / 12),
                            1, 0, 0);
                    }
                    depthShader.NextDrawCall();
                }
            }
            depthShader.Use();
        }

        // Sparse instanced blade silhouettes give grass a readable basic shadow
        // without repeating the full-density 400k-blade forward pass. The grass
        // shadow VS strides through each range so these are distributed tufts,
        // not a contiguous prefix that forms bands.
        if (drawDynamic && !g_emptyLevelMode && g_grass.IsInitialized() &&
            g_grass.CastShadows() &&
            g_grass.ShadowDensity() > 0.0f && depthShader.grassPipelineState) {
            g_grass.SetViewer(scene.camera.Position);
            static std::vector<GrassField::DrawRange> grassShadowRanges;
            g_grass.GetVisible(grassShadowRanges);
            const auto& grassVbv = g_grass.GetVBV();
            const auto& grassIbv = g_grass.GetIBV();
            const auto instances = g_grass.GetInstanceBufferAddress();
            if (!grassShadowRanges.empty() && grassVbv.BufferLocation && instances) {
                depthShader.UseGrass();
                // Pipeline/root signature must be bound before its b0 matrix;
                // binding it afterward invalidates the light-space transform.
                depthShader.SetMatrices(XMMatrixIdentity(), lightSpace);
                g_dx12.commandList->IASetVertexBuffers(0, 1, &grassVbv);
                g_dx12.commandList->IASetIndexBuffer(&grassIbv);
                g_dx12.commandList->IASetPrimitiveTopology(
                    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                GrassField::Params params = g_grass.GetParams(
                    scene.EffectiveCameraFOV(), static_cast<float>(g_dx12.screenHeight));
                for (const auto& range : grassShadowRanges) {
                    params.firstBlade = range.firstInstance;
                    // Shadow VS reuses raster-only fields to scatter the sparse
                    // caster budget across the complete cell instead of taking
                    // a stripe-forming contiguous prefix.
                    params.drawDistance = g_grass.ShadowDensity();
                    params.pixelWorldScale = static_cast<float>(range.instanceCount);
                    depthShader.SetGrass(params, instances);
                    const UINT sparseCount = static_cast<UINT>(
                        range.instanceCount * g_grass.ShadowDensity());
                    if (!sparseCount) continue;
                    g_dx12.commandList->DrawIndexedInstanced(
                        GrassField::IndexCount(), sparseCount, 0, 0, 0);
                    depthShader.NextDrawCall();
                }
                depthShader.Use();
            }
        }

        if (drawDynamic && !g_emptyLevelMode && g_explosiveBarrelModel) {
            std::vector<XMMATRIX> barrelTransforms;
            barrelTransforms.reserve(scene.explosiveBarrels.size());
            for (const ExplosiveBarrel& barrel : scene.explosiveBarrels) {
                if (!barrel.active) continue;
                barrelTransforms.push_back(XMMatrixTranslation(
                    barrel.position.x, barrel.position.y - 0.75f,
                    barrel.position.z));
            }
            DrawSceneNodeShadowInstances(
                g_explosiveBarrelShadowModel
                    ? g_explosiveBarrelShadowModel : g_explosiveBarrelModel,
                depthShader, barrelTransforms, lightSpace);
        } else if (drawDynamic && !g_emptyLevelMode) for (
            const ExplosiveBarrel& barrel : scene.explosiveBarrels) {
            if (barrel.active) {
                const XMMATRIX model = XMMatrixScaling(1.6f, 1.5f, 1.6f) *
                    XMMatrixTranslation(barrel.position.x, barrel.position.y,
                                        barrel.position.z);
                depthShader.SetMatrices(model, lightSpace);
                DrawCapsule(geo);
                depthShader.NextDrawCall();
            }
        }

        if (drawDynamic && bandits) for (const auto& banditOwner : *bandits) {
            SkinnedEnemy* bandit = banditOwner.get();
            if (!bandit || (!bandit->castsShadow && !bandit->Dead()) || !bandit->CanRender())
                continue;
            const D3D12_GPU_VIRTUAL_ADDRESS palette = bandit->UploadPalette();
            const XMMATRIX model = bandit->MeshWorldMatrix();
            for (const auto& prim : bandit->model.node->mesh->primitives) {
                if (!prim.vbv.BufferLocation || !prim.skinBuffer) continue;
                depthShader.SetMatrices(model, lightSpace);
                depthShader.SetSkinning(palette, prim.skinBuffer->GetGPUVirtualAddress());
                g_dx12.commandList->IASetVertexBuffers(0, 1, &prim.vbv);
                g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                if (prim.ibv.BufferLocation) {
                    g_dx12.commandList->IASetIndexBuffer(&prim.ibv);
                    g_dx12.commandList->DrawIndexedInstanced(prim.indexCount, 1, 0, 0, 0);
                } else {
                    g_dx12.commandList->DrawInstanced((UINT)(prim.vertices.size() / 12), 1, 0, 0);
                }
                depthShader.NextDrawCall();
            }
        }

        if (drawDynamic) for (auto& p : scene.projectiles) {
            if (!p.active) continue;
            // Bullets and tracers are far too small for a shadow to read as
            // anything but noise: at projectileScale they resolve to a pixel or
            // two of shadow-map coverage, which flickers across the map as they
            // fly. Only the objects big enough to cast a shadow a player can
            // recognise are drawn -- grenades, rockets, charges and spears.
            const bool castsShadow = p.grenade || p.molotov || p.vortex ||
                                     p.rocket || p.remoteCharge || p.harpoon;
            if (!castsShadow) continue;
            XMMATRIX model = XMMatrixScaling(scene.projectileScale, scene.projectileScale, scene.projectileScale);
            model = model * XMMatrixTranslation(p.position.x, p.position.y, p.position.z);
            depthShader.SetMatrices(model, lightSpace);
            DrawCube(geo);
            depthShader.NextDrawCall();
        }
    }

    // Depth for every registered spot caster, one atlas slice each.
    //
    // Runs after the cascades and reuses their depth-only shader and draw
    // list, so anything that casts a sun shadow casts a headlight shadow too.
    // The casters were registered earlier this frame by the light-add
    // functions in ForwardRenderer, which is what guarantees slice N here is
    // the same frustum the lights tagged with spotShadowIndex N.
    void RenderSpotShadows(
            Scene& scene,
            const GeometryBuffers& geo,
            const std::vector<PrefabRenderBatch>& prefabRenderBatches,
            const std::shared_ptr<SceneNode>& crateModel,
            const std::vector<std::unique_ptr<SkinnedEnemy>>* bandits,
            ShaderDX12* terrainShader) {
        const UINT casterCount = (std::min)(
            static_cast<UINT>(g_spotShadowCasters.size()), SPOT_SHADOW_COUNT);
        // Published before the early-out: with no casters this frame the
        // shaders must see zero, or they keep sampling last frame's depth for
        // a vehicle that has since been destroyed or driven out of the level.
        g_spotShadowActiveCount = casterCount;
        for (UINT slice = 0; slice < casterCount; ++slice)
            g_spotShadowMatrices[slice] = g_spotShadowCasters[slice].viewProjection;
        if (!casterCount || !spotShadowMap) return;

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = spotShadowMap.Get();
        barrier.Transition.StateBefore = SHADOW_SHADER_READ_STATE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_dx12.commandList->ResourceBarrier(1, &barrier);

        D3D12_VIEWPORT spotViewport = {};
        spotViewport.Width = static_cast<float>(SPOT_SHADOW_SIZE);
        spotViewport.Height = static_cast<float>(SPOT_SHADOW_SIZE);
        spotViewport.MinDepth = 0.0f;
        spotViewport.MaxDepth = 1.0f;
        D3D12_RECT spotScissor = {
            0, 0, (LONG)SPOT_SHADOW_SIZE, (LONG)SPOT_SHADOW_SIZE };
        g_dx12.commandList->RSSetViewports(1, &spotViewport);
        g_dx12.commandList->RSSetScissorRects(1, &spotScissor);

        const UINT dsvStride = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        for (UINT slice = 0; slice < casterCount; ++slice) {
            D3D12_CPU_DESCRIPTOR_HANDLE spotDsv =
                spotDsvHeap->GetCPUDescriptorHandleForHeapStart();
            spotDsv.ptr += static_cast<SIZE_T>(slice) * dsvStride;
            g_dx12.commandList->ClearDepthStencilView(
                spotDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            g_dx12.commandList->OMSetRenderTargets(0, nullptr, FALSE, &spotDsv);
            DrawShadowScene(scene, geo, prefabRenderBatches, crateModel,
                            bandits, g_spotShadowCasters[slice].viewProjection,
                            true, terrainShader);
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.StateAfter = SHADOW_SHADER_READ_STATE;
        g_dx12.commandList->ResourceBarrier(1, &barrier);
    }

    XMMATRIX Render(Scene& scene,
                    const GeometryBuffers& geo,
                    const std::vector<PrefabRenderBatch>& prefabRenderBatches,
                    const std::shared_ptr<SceneNode>& crateModel,
                    const std::vector<std::unique_ptr<SkinnedEnemy>>* bandits = nullptr,
                    ShaderDX12* terrainShader = nullptr) {
        const auto candidateMatrices = ComputeCascadeMatrices(scene);
        const XMFLOAT4 candidateTexelWorld = g_shadowCascadeTexelWorld;
        const XMFLOAT4 candidateDepthRange = g_shadowCascadeDepthRange;
        std::array<XMMATRIX, SHADOW_CASCADE_COUNT> cascadeMatrices =
            candidateMatrices;
        std::array<bool, SHADOW_CASCADE_COUNT> refreshCascade = {};
        cachedCascadesThisFrame = 0;
        refreshedCascadesThisFrame = 0;

        const bool useFarCache = scene.cacheFarShadowCascades &&
            EnsureFarCascadeCache();
        if (useFarCache) {
            const float aspect = static_cast<float>(g_dx12.screenWidth) /
                (std::max)(1.0f, static_cast<float>(g_dx12.screenHeight));
            const float cameraFar = scene.EffectiveCameraFarPlane();
            XMFLOAT3 lightDirection;
            XMStoreFloat3(&lightDirection, XMVector3Normalize(
                XMLoadFloat3(&scene.lightPos)));
            const bool projectionChanged = !cacheViewValid ||
                std::abs(cachedFov - scene.EffectiveCameraFOV()) > 0.001f ||
                std::abs(cachedAspect - aspect) > 0.0001f ||
                std::abs(cachedNear - scene.cameraNear) > 0.0001f ||
                std::abs(cachedFar - cameraFar) > 0.01f ||
                XMVectorGetX(XMVector3Dot(
                    XMLoadFloat3(&cachedLightDirection),
                    XMLoadFloat3(&lightDirection))) < 0.999999f;
            if (projectionChanged) farCacheValid.fill(false);
            cachedFov = scene.EffectiveCameraFOV();
            cachedAspect = aspect;
            cachedNear = scene.cameraNear;
            cachedFar = cameraFar;
            cachedLightDirection = lightDirection;
            cacheViewValid = true;

            const XMVECTOR cameraPosition =
                XMLoadFloat3(&scene.camera.Position);
            const XMVECTOR cameraFront = XMVector3Normalize(
                XMLoadFloat3(&scene.camera.Front));
            for (UINT cascade = 1; cascade < SHADOW_CASCADE_COUNT; ++cascade) {
                const UINT cacheIndex = cascade - 1;
                if (farCacheValid[cacheIndex]) {
                    const XMVECTOR positionDelta = cameraPosition -
                        XMLoadFloat3(&farCacheCameraPosition[cacheIndex]);
                    const float movement = XMVectorGetX(
                        XMVector3Length(positionDelta));
                    const float cascadeWidth =
                        (&candidateTexelWorld.x)[cascade] * size;
                    const float movementLimit = cascadeWidth *
                        (cascade == 1 ? 0.025f : 0.035f);
                    const float directionDot = XMVectorGetX(XMVector3Dot(
                        cameraFront,
                        XMLoadFloat3(&farCacheCameraFront[cacheIndex])));
                    const float rotationLimit = std::cos(XMConvertToRadians(
                        cascade == 1 ? 1.0f : 2.0f));
                    if (movement > movementLimit ||
                        directionDot < rotationLimit)
                        farCacheValid[cacheIndex] = false;
                }
                if (!farCacheValid[cacheIndex]) {
                    refreshCascade[cascade] = true;
                    farCacheMatrices[cacheIndex] = candidateMatrices[cascade];
                    farCacheCameraPosition[cacheIndex] = scene.camera.Position;
                    farCacheCameraFront[cacheIndex] = scene.camera.Front;
                    farCacheTexelWorld[cacheIndex] =
                        (&candidateTexelWorld.x)[cascade];
                    farCacheDepthRange[cacheIndex] =
                        (&candidateDepthRange.x)[cascade];
                } else {
                    cascadeMatrices[cascade] = farCacheMatrices[cacheIndex];
                    (&g_shadowCascadeTexelWorld.x)[cascade] =
                        farCacheTexelWorld[cacheIndex];
                    (&g_shadowCascadeDepthRange.x)[cascade] =
                        farCacheDepthRange[cacheIndex];
                    ++cachedCascadesThisFrame;
                }
            }
        } else {
            // Re-entering the opt-in path must never reuse depth produced while
            // caching was disabled and scene changes were not tracked.
            farCacheValid.fill(false);
            cacheViewValid = false;
        }
        g_shadowCascadeMatrices = cascadeMatrices;
        XMMATRIX lightSpace = cascadeMatrices[0];
        // Spot casters do not depend on the sun: a headlight or a searchlight
        // still needs its depth even when the level runs a non-directional
        // light type, which skips the cascade pass entirely below. Rendered
        // before that early-out so the atlas is filled either way.
        if (initialized) {
            depthShader.BeginFrame();
            depthShader.SetPalmWindFrame(g_trees.GetWindFrame());
            RenderSpotShadows(scene, geo, prefabRenderBatches, crateModel,
                              bandits, terrainShader);
        }
        if (!initialized || scene.lightType != 0) {
            // The spot pass above left its own atlas-sized viewport and a null
            // render target bound. The cascade path restores both on its way
            // out; this early return has to do the same, or the frame draws
            // into a 2048-square corner of the screen.
            if (initialized) {
                D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetCPUDescriptorHandle(
                    g_dx12.rtvHeap.Get(), g_dx12.rtvDescriptorSize,
                    g_dx12.frameIndex);
                D3D12_CPU_DESCRIPTOR_HANDLE dsv =
                    g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
                g_dx12.commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
                g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
            }
            if (terrainShader)
                terrainShader->InvalidateGraphicsRootBinding();
            return lightSpace;
        }

        depthShader.BeginFrame();
        depthShader.SetPalmWindFrame(g_trees.GetWindFrame());

        D3D12_VIEWPORT shadowViewport = {};
        shadowViewport.Width = (float)size;
        shadowViewport.Height = (float)size;
        shadowViewport.MinDepth = 0.0f;
        shadowViewport.MaxDepth = 1.0f;
        D3D12_RECT shadowScissor = { 0, 0, (LONG)size, (LONG)size };
        g_dx12.commandList->RSSetViewports(1, &shadowViewport);
        g_dx12.commandList->RSSetScissorRects(1, &shadowScissor);

        const UINT dsvStride = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        auto transition = [](ID3D12Resource* resource,
                             D3D12_RESOURCE_STATES before,
                             D3D12_RESOURCE_STATES after,
                             UINT subresource) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            barrier.Transition.Subresource = subresource;
            g_dx12.commandList->ResourceBarrier(1, &barrier);
        };

        if (!useFarCache) {
            transition(shadowMap.Get(), SHADOW_SHADER_READ_STATE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
            for (UINT cascade = 0; cascade < SHADOW_CASCADE_COUNT; ++cascade) {
                lightSpace = cascadeMatrices[cascade];
                D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv =
                    dsvHeap->GetCPUDescriptorHandleForHeapStart();
                shadowDsv.ptr += static_cast<SIZE_T>(cascade) * dsvStride;
                g_dx12.commandList->ClearDepthStencilView(
                    shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                g_dx12.commandList->OMSetRenderTargets(
                    0, nullptr, FALSE, &shadowDsv);
                DrawShadowScene(scene, geo, prefabRenderBatches, crateModel,
                    bandits, lightSpace, false, terrainShader);
            }
            transition(shadowMap.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                SHADOW_SHADER_READ_STATE,
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
        } else {
            const UINT cacheDsvStride =
                g_dx12.device->GetDescriptorHandleIncrementSize(
                    D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            for (UINT cascade = 1; cascade < SHADOW_CASCADE_COUNT; ++cascade) {
                if (!refreshCascade[cascade]) continue;
                const UINT cacheIndex = cascade - 1;
                transition(cachedFarShadowMap.Get(),
                    D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE, cacheIndex);
                D3D12_CPU_DESCRIPTOR_HANDLE cacheDsv =
                    cachedFarDsvHeap->GetCPUDescriptorHandleForHeapStart();
                cacheDsv.ptr +=
                    static_cast<SIZE_T>(cacheIndex) * cacheDsvStride;
                g_dx12.commandList->ClearDepthStencilView(
                    cacheDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                g_dx12.commandList->OMSetRenderTargets(
                    0, nullptr, FALSE, &cacheDsv);
                DrawShadowScene(scene, geo, prefabRenderBatches, crateModel,
                    bandits, farCacheMatrices[cacheIndex], false,
                    terrainShader, ShadowScenePart::Static);
                transition(cachedFarShadowMap.Get(),
                    D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, cacheIndex);
                farCacheValid[cacheIndex] = true;
                ++refreshedCascadesThisFrame;
            }

            transition(shadowMap.Get(), SHADOW_SHADER_READ_STATE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, 0);
            D3D12_CPU_DESCRIPTOR_HANDLE nearDsv =
                dsvHeap->GetCPUDescriptorHandleForHeapStart();
            g_dx12.commandList->ClearDepthStencilView(
                nearDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            g_dx12.commandList->OMSetRenderTargets(
                0, nullptr, FALSE, &nearDsv);
            DrawShadowScene(scene, geo, prefabRenderBatches, crateModel,
                bandits, cascadeMatrices[0], false, terrainShader);

            for (UINT cascade = 1; cascade < SHADOW_CASCADE_COUNT; ++cascade) {
                const UINT cacheIndex = cascade - 1;
                transition(shadowMap.Get(), SHADOW_SHADER_READ_STATE,
                    D3D12_RESOURCE_STATE_COPY_DEST, cascade);
                D3D12_TEXTURE_COPY_LOCATION destination = {};
                destination.pResource = shadowMap.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination.SubresourceIndex = cascade;
                D3D12_TEXTURE_COPY_LOCATION source = {};
                source.pResource = cachedFarShadowMap.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                source.SubresourceIndex = cacheIndex;
                g_dx12.commandList->CopyTextureRegion(
                    &destination, 0, 0, 0, &source, nullptr);
                transition(shadowMap.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE, cascade);
                D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv =
                    dsvHeap->GetCPUDescriptorHandleForHeapStart();
                shadowDsv.ptr += static_cast<SIZE_T>(cascade) * dsvStride;
                g_dx12.commandList->OMSetRenderTargets(
                    0, nullptr, FALSE, &shadowDsv);
                DrawShadowScene(scene, geo, prefabRenderBatches, crateModel,
                    bandits, cascadeMatrices[cascade], false, terrainShader,
                    ShadowScenePart::Dynamic);
            }
            for (UINT cascade = 0; cascade < SHADOW_CASCADE_COUNT; ++cascade)
                transition(shadowMap.Get(),
                    D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    SHADOW_SHADER_READ_STATE, cascade);
        }
        lightSpace = cascadeMatrices[0];

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = GetCPUDescriptorHandle(
            g_dx12.rtvHeap.Get(), g_dx12.rtvDescriptorSize, g_dx12.frameIndex);
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        g_dx12.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
        g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
        g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
        // DrawShadowScene finishes on the depth-only root signature. Make the
        // main shader rebind before the forward/visibility extension pass.
        if (terrainShader)
            terrainShader->InvalidateGraphicsRootBinding();

        return lightSpace;
    }

private:
    static constexpr UINT FAR_CASCADE_CACHE_COUNT =
        SHADOW_CASCADE_COUNT - 1;
    std::array<bool, FAR_CASCADE_CACHE_COUNT> farCacheValid = {};
    std::array<XMMATRIX, FAR_CASCADE_CACHE_COUNT> farCacheMatrices = {};
    std::array<XMFLOAT3, FAR_CASCADE_CACHE_COUNT> farCacheCameraPosition = {};
    std::array<XMFLOAT3, FAR_CASCADE_CACHE_COUNT> farCacheCameraFront = {};
    std::array<float, FAR_CASCADE_CACHE_COUNT> farCacheTexelWorld = {};
    std::array<float, FAR_CASCADE_CACHE_COUNT> farCacheDepthRange = {};
    XMFLOAT3 cachedLightDirection = {};
    float cachedFov = 0.0f;
    float cachedAspect = 0.0f;
    float cachedNear = 0.0f;
    float cachedFar = 0.0f;
    bool cacheViewValid = false;

    bool EnsureFarCascadeCache() {
        if (cachedFarShadowMap && cachedFarDsvHeap) return true;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = size;
        texDesc.Height = size;
        texDesc.DepthOrArraySize = FAR_CASCADE_CACHE_COUNT;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        texDesc.SampleDesc.Count = 1;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;

        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_COPY_SOURCE, &clearValue,
            IID_PPV_ARGS(&cachedFarShadowMap));
        if (FAILED(hr)) {
            std::cerr << "Failed to create far cascade shadow cache\n";
            return false;
        }
        cachedFarShadowMap->SetName(L"Cached Far Cascade Shadow Depth");

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = FAR_CASCADE_CACHE_COUNT;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = g_dx12.device->CreateDescriptorHeap(
            &heapDesc, IID_PPV_ARGS(&cachedFarDsvHeap));
        if (FAILED(hr)) {
            cachedFarShadowMap.Reset();
            return false;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.ArraySize = 1;
        const UINT stride = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            cachedFarDsvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT cacheIndex = 0; cacheIndex < FAR_CASCADE_CACHE_COUNT;
             ++cacheIndex) {
            dsvDesc.Texture2DArray.FirstArraySlice = cacheIndex;
            g_dx12.device->CreateDepthStencilView(
                cachedFarShadowMap.Get(), &dsvDesc, dsv);
            dsv.ptr += stride;
        }
        InvalidateCachedCascades();
        return true;
    }

    bool CreateShadowMap() {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = size;
        texDesc.Height = size;
        texDesc.DepthOrArraySize = SHADOW_CASCADE_COUNT;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        texDesc.SampleDesc.Count = 1;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;

        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            SHADOW_SHADER_READ_STATE, &clearValue,
            IID_PPV_ARGS(&shadowMap));
        if (FAILED(hr)) {
            std::cerr << "Failed to create shadow map texture" << std::endl;
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = SHADOW_CASCADE_COUNT;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = g_dx12.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap));
        if (FAILED(hr)) return false;

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.ArraySize = 1;
        const UINT dsvStride = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT cascade = 0; cascade < SHADOW_CASCADE_COUNT; ++cascade) {
            dsvDesc.Texture2DArray.FirstArraySlice = cascade;
            g_dx12.device->CreateDepthStencilView(shadowMap.Get(), &dsvDesc, dsv);
            dsv.ptr += dsvStride;
        }

        return true;
    }

    // Spot atlas. Same format and states as the cascades so both can share the
    // depth-only shader and the comparison sampler already bound at s0.
    bool CreateSpotShadowMap() {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = SPOT_SHADOW_SIZE;
        texDesc.Height = SPOT_SHADOW_SIZE;
        texDesc.DepthOrArraySize = static_cast<UINT16>(SPOT_SHADOW_COUNT);
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        texDesc.SampleDesc.Count = 1;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;

        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            SHADOW_SHADER_READ_STATE, &clearValue,
            IID_PPV_ARGS(&spotShadowMap));
        if (FAILED(hr)) {
            std::cerr << "Failed to create spot shadow atlas" << std::endl;
            return false;
        }
        spotShadowMap->SetName(L"Humvee Spotlight Shadow Depth");

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = SPOT_SHADOW_COUNT;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = g_dx12.device->CreateDescriptorHeap(
            &dsvHeapDesc, IID_PPV_ARGS(&spotDsvHeap));
        if (FAILED(hr)) return false;
        spotDsvHeap->SetName(L"Humvee Spotlight Shadow DSVs");

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.ArraySize = 1;
        const UINT dsvStride = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            spotDsvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT slice = 0; slice < SPOT_SHADOW_COUNT; ++slice) {
            dsvDesc.Texture2DArray.FirstArraySlice = slice;
            g_dx12.device->CreateDepthStencilView(
                spotShadowMap.Get(), &dsvDesc, dsv);
            dsv.ptr += dsvStride;
        }

        return true;
    }
};

#endif // SHADOW_MAP_DX12_H
