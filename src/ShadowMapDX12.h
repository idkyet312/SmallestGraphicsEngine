#ifndef SHADOW_MAP_DX12_H
#define SHADOW_MAP_DX12_H

#include "DX12Core.h"
#include "ShaderDX12.h"
#include "Scene.h"
#include "ForwardRenderer.h"
#include "SceneGraph.h"
#include "SkinnedEnemy.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

static const UINT SHADOW_MAP_SIZE = 2048;
static const UINT SHADOW_MAX_DRAWS = 4096;

class DepthOnlyShaderDX12 {
public:
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12PipelineState> grassPipelineState;
    UploadBuffer<MatrixBufferDX12> matrixBuffer;
    UINT currentDrawCall = 0;
    bool loaded = false;

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
        HRESULT hr = D3DCompile(vsCode.c_str(), vsCode.length(), vertexPath,
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_0",
            compileFlags, 0, &vsBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "Shadow VS error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        D3D12_ROOT_PARAMETER rootParams[6] = {};
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
        rootParams[4].Constants.Num32BitValues = 12;
        rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[5].Descriptor.ShaderRegister = 6;
        rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = _countof(rootParams);
        rootSigDesc.pParameters = rootParams;
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
        hr = D3DCompile(grassVsCode.c_str(), grassVsCode.length(),
            "shaders/grass_shadow_vs.hlsl", nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_0",
            compileFlags, 0, &grassVsBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "Grass shadow VS error: "
                                     << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }
        D3D12_INPUT_ELEMENT_DESC grassInput[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        psoDesc.InputLayout = { grassInput, _countof(grassInput) };
        psoDesc.VS = { grassVsBlob->GetBufferPointer(), grassVsBlob->GetBufferSize() };
        hr = g_dx12.device->CreateGraphicsPipelineState(
            &psoDesc, IID_PPV_ARGS(&grassPipelineState));
        if (FAILED(hr)) return false;

        if (!matrixBuffer.Create(FRAME_COUNT * SHADOW_MAX_DRAWS)) return false;
        loaded = true;
        return true;
    }

    void BeginFrame() {
        currentDrawCall = 0;
    }

    void Use() {
        g_dx12.commandList->SetGraphicsRootSignature(rootSignature.Get());
        g_dx12.commandList->SetPipelineState(pipelineState.Get());
    }

    void SetMatrices(const XMMATRIX& model, const XMMATRIX& lightSpace) {
        UINT index = g_dx12.frameIndex * SHADOW_MAX_DRAWS + std::min(currentDrawCall, SHADOW_MAX_DRAWS - 1);
        MatrixBufferDX12 data = {};
        data.model = XMMatrixTranspose(model);
        data.view = XMMatrixIdentity();
        data.projection = XMMatrixIdentity();
        data.lightSpaceMatrix = XMMatrixTranspose(lightSpace);
        matrixBuffer.CopyData(index, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(0, matrixBuffer.GetGPUAddress(index));
        const UINT disabled = 0;
        g_dx12.commandList->SetGraphicsRoot32BitConstants(1, 1, &disabled, 0);
    }

    void SetSkinning(D3D12_GPU_VIRTUAL_ADDRESS palette, D3D12_GPU_VIRTUAL_ADDRESS skin) {
        const UINT enabled = palette && skin ? 1u : 0u;
        g_dx12.commandList->SetGraphicsRoot32BitConstants(1, 1, &enabled, 0);
        if (enabled) {
            g_dx12.commandList->SetGraphicsRootShaderResourceView(2, palette);
            g_dx12.commandList->SetGraphicsRootShaderResourceView(3, skin);
        }
    }

    void UseGrass() {
        g_dx12.commandList->SetGraphicsRootSignature(rootSignature.Get());
        g_dx12.commandList->SetPipelineState(grassPipelineState.Get());
    }

    void SetGrass(const GrassField::Params& params,
                  D3D12_GPU_VIRTUAL_ADDRESS instances) {
        g_dx12.commandList->SetGraphicsRoot32BitConstants(4, 12, &params, 0);
        g_dx12.commandList->SetGraphicsRootShaderResourceView(5, instances);
    }

    void NextDrawCall() {
        if (currentDrawCall + 1 < SHADOW_MAX_DRAWS) currentDrawCall++;
    }
};

inline void DrawSceneNodeShadow(const std::shared_ptr<SceneNode>& node,
                                DepthOnlyShaderDX12& shader,
                                const XMMATRIX& worldTransform,
                                const XMMATRIX& lightSpace) {
    if (!node) return;

    if (node->mesh) {
        XMMATRIX model = XMLoadFloat4x4(&node->globalTransform) * worldTransform;
        shader.SetMatrices(model, lightSpace);

        for (const auto& prim : node->mesh->primitives) {
            if (prim.vbv.BufferLocation == 0) continue;
            if (prim.material && prim.material->baseColorFactor.w < 0.5f) continue;

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
        DrawSceneNodeShadow(child, shader, worldTransform, lightSpace);
    }
}

class ShadowMapDX12 {
public:
    ComPtr<ID3D12Resource> shadowMap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    DepthOnlyShaderDX12 depthShader;
    UINT size = SHADOW_MAP_SIZE;
    bool initialized = false;

    bool Init(UINT mapSize = SHADOW_MAP_SIZE) {
        size = mapSize;
        if (!CreateShadowMap()) return false;
        if (!depthShader.Load("shaders/depth_vs.hlsl")) return false;
        initialized = true;
        std::cout << "Shadow map ready (" << size << "x" << size << ")" << std::endl;
        return true;
    }

    ID3D12Resource* GetResource() const {
        return shadowMap.Get();
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

    XMMATRIX Render(Scene& scene,
                    const GeometryBuffers& geo,
                    const std::shared_ptr<SceneNode>& crateModel,
                    const std::vector<std::unique_ptr<SkinnedEnemy>>* bandits = nullptr) {
        XMMATRIX lightSpace = ComputeLightSpace(scene);
        if (!initialized || scene.lightType != 0) return lightSpace;

        depthShader.BeginFrame();

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = shadowMap.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_dx12.commandList->ResourceBarrier(1, &barrier);

        D3D12_VIEWPORT shadowViewport = {};
        shadowViewport.Width = (float)size;
        shadowViewport.Height = (float)size;
        shadowViewport.MinDepth = 0.0f;
        shadowViewport.MaxDepth = 1.0f;
        D3D12_RECT shadowScissor = { 0, 0, (LONG)size, (LONG)size };
        g_dx12.commandList->RSSetViewports(1, &shadowViewport);
        g_dx12.commandList->RSSetScissorRects(1, &shadowScissor);

        D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        g_dx12.commandList->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        g_dx12.commandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);

        depthShader.Use();

        if (crateModel) {
            DrawSceneNodeShadow(crateModel, depthShader, XMMatrixIdentity(), lightSpace);
        }
        if (!crateModel && !g_destruction.IsInitialized()) {
            XMMATRIX model = scene.cube1.GetModelMatrix();
            depthShader.SetMatrices(model, lightSpace);
            DrawCube(geo);
            depthShader.NextDrawCall();
        }

        // Detached chunks keep casting shadows from their live physics poses.
        if (scene.useDestruction && g_destruction.IsInitialized()) {
            // Anything outside the light's ortho box cannot land in the shadow
            // map; skip it before recording the draw. lightSpace is orthographic
            // so w stays 1 and the NDC test needs no divide.
            const float ortho = (std::max)(scene.shadowOrthoSize, 1.0f);
            const float distance = (std::max)(scene.shadowDistance, 1.0f);
            const float depthRange =
                (std::max)(scene.shadowFarPlane, distance + ortho) - 0.1f;
            const auto inLightBox = [&](const XMFLOAT3& c, float r) {
                if (r <= 0.0f) return true;
                XMFLOAT4 ndc;
                XMStoreFloat4(&ndc, XMVector4Transform(
                    XMVectorSet(c.x, c.y, c.z, 1.0f), lightSpace));
                const float rx = r / ortho;
                const float rz = r / depthRange;
                return std::fabs(ndc.x) <= 1.0f + rx &&
                       std::fabs(ndc.y) <= 1.0f + rx &&
                       ndc.z + rz >= 0.0f && ndc.z - rz <= 1.0f;
            };
            const auto& batches = g_destruction.GetRenderBatches();
            if (!batches.empty()) {
                for (const DestructionRenderBatch& batch : batches) {
                    if (!inLightBox(batch.sphereCenter, batch.sphereRadius)) continue;
                    DrawSceneNodeShadow(batch.shadowNode, depthShader,
                        XMLoadFloat4x4(&batch.transform), lightSpace);
                }
            } else for (const DestructionRenderItem& item : g_destruction.GetRenderItems()) {
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

        if (scene.cube2.visible) {
            XMMATRIX model = scene.cube2.GetModelMatrix();
            depthShader.SetMatrices(model, lightSpace);
            DrawCube(geo);
            depthShader.NextDrawCall();
        }

        if (g_humveeModel) {
            DrawSceneNodeShadow(g_humveeModel, depthShader,
                                HumveeWorldMatrix(), lightSpace);
        }

        // Sparse instanced blade silhouettes give grass a readable basic shadow
        // without repeating the full-density 400k-blade forward pass.
        if (g_grass.IsInitialized() && g_grass.CastShadows() &&
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
                GrassField::Params params = g_grass.GetParams();
                for (const auto& range : grassShadowRanges) {
                    params.firstBlade = range.firstInstance;
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

        for (const ExplosiveBarrel& barrel : scene.explosiveBarrels) {
            if (!barrel.active) continue;
            if (g_explosiveBarrelModel) {
                const XMMATRIX model = XMMatrixTranslation(
                    barrel.position.x, barrel.position.y - 0.75f,
                    barrel.position.z);
                DrawSceneNodeShadow(g_explosiveBarrelModel, depthShader,
                                    model, lightSpace);
            } else {
                const XMMATRIX model = XMMatrixScaling(1.6f, 1.5f, 1.6f) *
                    XMMatrixTranslation(barrel.position.x, barrel.position.y,
                                        barrel.position.z);
                depthShader.SetMatrices(model, lightSpace);
                DrawCapsule(geo);
                depthShader.NextDrawCall();
            }
        }

        if (bandits) for (const auto& banditOwner : *bandits) {
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

        for (auto& p : scene.projectiles) {
            if (!p.active) continue;
            XMMATRIX model = XMMatrixScaling(scene.projectileScale, scene.projectileScale, scene.projectileScale);
            model = model * XMMatrixTranslation(p.position.x, p.position.y, p.position.z);
            depthShader.SetMatrices(model, lightSpace);
            DrawCube(geo);
            depthShader.NextDrawCall();
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_dx12.commandList->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = GetCPUDescriptorHandle(
            g_dx12.rtvHeap.Get(), g_dx12.rtvDescriptorSize, g_dx12.frameIndex);
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        g_dx12.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
        g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
        g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);

        return lightSpace;
    }

private:
    bool CreateShadowMap() {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = size;
        texDesc.Height = size;
        texDesc.DepthOrArraySize = 1;
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
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
            IID_PPV_ARGS(&shadowMap));
        if (FAILED(hr)) {
            std::cerr << "Failed to create shadow map texture" << std::endl;
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = g_dx12.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap));
        if (FAILED(hr)) return false;

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        g_dx12.device->CreateDepthStencilView(shadowMap.Get(), &dsvDesc,
            dsvHeap->GetCPUDescriptorHandleForHeapStart());

        return true;
    }
};

#endif // SHADOW_MAP_DX12_H
