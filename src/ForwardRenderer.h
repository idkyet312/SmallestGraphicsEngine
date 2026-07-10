#ifndef FORWARD_RENDERER_H
#define FORWARD_RENDERER_H

// Forward clustered renderer � the original rendering path.
// Binds the clustered forward shader and draws each object with per-draw CBVs.

#include "DX12Core.h"
#include "ShaderDX12.h"
#include "Scene.h"
#include "SceneGraph.h"
#include "MeshShaderDX12.h"
#include "TerrainRendererDX12.h"
#include "DestructionDX12.h"

extern MeshShaderDX12 g_meshShader;
extern bool g_useMeshShader;
extern TerrainRendererDX12 g_terrain;

struct GeometryBuffers {
    ComPtr<ID3D12Resource>   cubeVertexBuffer;
    ComPtr<ID3D12Resource>   planeVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW cubeVBV  = {};
    D3D12_VERTEX_BUFFER_VIEW planeVBV = {};
};

// Vertex layout shared across renderers
struct VertexPosNormUV {
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT2 texCoord;
    XMFLOAT4 tangent = XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f);
};

inline bool CreateVertexBuffer(const std::vector<VertexPosNormUV>& verts,
                               ComPtr<ID3D12Resource>& buffer,
                               D3D12_VERTEX_BUFFER_VIEW& vbv) {
    UINT size = (UINT)(verts.size() * sizeof(VertexPosNormUV));
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = g_dx12.device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&buffer));
    if (FAILED(hr)) return false;

    void* mapped;
    D3D12_RANGE r = { 0, 0 };
    if (FAILED(buffer->Map(0, &r, &mapped))) return false;
    memcpy(mapped, verts.data(), size);
    buffer->Unmap(0, nullptr);

    vbv.BufferLocation = buffer->GetGPUVirtualAddress();
    vbv.SizeInBytes    = size;
    vbv.StrideInBytes  = sizeof(VertexPosNormUV);
    return true;
}

inline void DrawCube(const GeometryBuffers& geo) {
    g_dx12.commandList->IASetVertexBuffers(0, 1, &geo.cubeVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(36, 1, 0, 0);
}

inline void DrawPlane(const GeometryBuffers& geo) {
    g_dx12.commandList->IASetVertexBuffers(0, 1, &geo.planeVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(6, 1, 0, 0);
}

// Draw an imported GLB scene graph node (and its children) with an extra
// world transform applied on top of each node's own local/global transform.
inline void DrawSceneNode(const std::shared_ptr<SceneNode>& node, ShaderDX12& shader,
                           const XMMATRIX& worldTransform,
                           const XMMATRIX& view, const XMMATRIX& proj, const XMMATRIX& lightSpace) {
    if (!node) return;

    if (node->mesh) {
        XMMATRIX model = XMLoadFloat4x4(&node->globalTransform) * worldTransform;
        shader.SetMatrices(model, view, proj, lightSpace);

        for (const auto& prim : node->mesh->primitives) {
            if (prim.vbv.BufferLocation == 0) continue;

            if (prim.material) {
                XMFLOAT3 color(prim.material->baseColorFactor.x,
                                prim.material->baseColorFactor.y,
                                prim.material->baseColorFactor.z);
                shader.SetObjectMaterial(
                    color,
                    prim.material->baseColorTexture != nullptr,
                    prim.material->normalTexture != nullptr,
                    prim.material->metallicFactor,
                    prim.material->roughnessFactor,
                    prim.material->baseColorTexture.Get(),
                    prim.material->normalTexture.Get(),
                    prim.material->metallicRoughnessTexture.Get());
            } else {
                shader.SetObjectMaterial(XMFLOAT3(1, 1, 1), false, false, 0.0f, 0.5f, nullptr, nullptr, nullptr);
            }

            D3D12_GPU_VIRTUAL_ADDRESS meshletDescAddress = prim.meshletDescBuffer
                ? prim.meshletDescBuffer->GetGPUVirtualAddress() : 0;
            D3D12_GPU_VIRTUAL_ADDRESS boundsAddress = prim.meshletBoundsBuffer
                ? prim.meshletBoundsBuffer->GetGPUVirtualAddress() : 0;
            D3D12_GPU_VIRTUAL_ADDRESS vertexIndexAddress = prim.meshletVertexIndexBuffer
                ? prim.meshletVertexIndexBuffer->GetGPUVirtualAddress() : 0;
            D3D12_GPU_VIRTUAL_ADDRESS triangleAddress = prim.meshletTriangleBuffer
                ? prim.meshletTriangleBuffer->GetGPUVirtualAddress() : 0;
            if (g_useMeshShader && g_meshShader.CanDraw(
                    prim.meshletCount, meshletDescAddress, boundsAddress,
                    vertexIndexAddress, triangleAddress)) {
                g_meshShader.Draw(prim.vbv,
                    (UINT)(prim.vertices.size() / 12), prim.indexCount,
                    prim.meshletCount, meshletDescAddress, boundsAddress,
                    vertexIndexAddress, triangleAddress);
            } else {
                // A previous mesh draw leaves the mesh PSO bound. Restore the
                // conventional VS/PS pipeline before issuing IA draw calls.
                g_dx12.commandList->SetPipelineState(shader.pipelineState.Get());
                g_dx12.commandList->IASetVertexBuffers(0, 1, &prim.vbv);
                g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                if (prim.ibv.BufferLocation != 0) {
                g_dx12.commandList->IASetIndexBuffer(&prim.ibv);
                g_dx12.commandList->DrawIndexedInstanced(prim.indexCount, 1, 0, 0, 0);
                } else {
                g_dx12.commandList->DrawInstanced((UINT)(prim.vertices.size() / 12), 1, 0, 0);
                }
            }

            shader.NextDrawCall();
        }
    }

    for (auto& child : node->children) {
        DrawSceneNode(child, shader, worldTransform, view, proj, lightSpace);
    }
}

// Render the whole scene using the forward clustered path
inline void RenderForward(Scene& scene, ShaderDX12& shader, const GeometryBuffers& geo,
                           const std::shared_ptr<SceneNode>& crateModel = nullptr,
                           const std::shared_ptr<SceneMaterial>& floorMaterial = nullptr,
                           XMMATRIX lightSpace = XMMatrixIdentity(),
                           ID3D12Resource* shadowMap = nullptr) {
    XMMATRIX view = scene.GetViewMatrix();
    XMMATRIX proj = scene.GetProjectionMatrix();

    shader.Use(scene.wireframeMode);
    shader.BindGlobalResources(shadowMap);

    shader.SetLight(scene.lightPos, scene.lightType, scene.lightColor,
                    scene.lightConstant, scene.lightLinear, scene.lightQuadratic,
                    scene.ambientStrength, scene.specularStrength, scene.specularShininess,
                    scene.shadowBias, scene.enableShadows && shadowMap != nullptr);
    shader.SetCamera(scene.camera.Position);

    // Clustered light cull
    scene.clusteredRenderer.setScreenSize((float)g_dx12.screenWidth, (float)g_dx12.screenHeight);
    scene.clusteredRenderer.setCamera(scene.cameraFOV, scene.cameraNear, scene.cameraFar, view, proj);
    scene.clusteredRenderer.cullLights();

    auto lightData = scene.clusteredRenderer.getPointLightData();
    shader.SetPointLights((int)lightData.size(), lightData);
    shader.SetDDGI(scene.useDDGI, scene.giIntensity, scene.normalBias, scene.probeSpacing);
    shader.SetSH();

    g_meshShader.wireframe = scene.meshletWireframe;
    g_terrain.wireframe = scene.meshletWireframe;

    // Floor: mesh-shader tessellated terrain when available, flat plane otherwise.
    XMMATRIX model = XMMatrixIdentity();
    shader.SetMatrices(model, view, proj, lightSpace);
    if (floorMaterial && floorMaterial->baseColorTexture) {
        shader.SetObjectMaterial(scene.floor.color,
                                 true,
                                 floorMaterial->normalTexture != nullptr,
                                 floorMaterial->metallicFactor,
                                 floorMaterial->roughnessFactor,
                                 floorMaterial->baseColorTexture.Get(),
                                 floorMaterial->normalTexture.Get(),
                                 floorMaterial->metallicRoughnessTexture.Get());
    } else {
        shader.SetObjectColor(scene.floor.color);
    }
    if (scene.useMeshTerrain && g_terrain.supported) {
        TerrainRendererDX12::Params terrainParams;
        terrainParams.heightScale = scene.terrainHeightScale;
        g_terrain.Draw(terrainParams);
        // Terrain used the mesh pipeline; restore the IA pipeline for the
        // raster draws that follow (same pattern as imported-model draws).
        g_dx12.commandList->SetPipelineState(scene.wireframeMode
            ? shader.wireframePipelineState.Get() : shader.pipelineState.Get());
    } else {
        DrawPlane(geo);
    }
    shader.NextDrawCall();

    // Cube 1 - draw the imported model if loaded, else fall back to the procedural cube.
    // The model is its own multi-meter scene (not a unit cube), so place it directly
    // on the floor at the origin rather than reusing cube1's small transform.
    if (crateModel) {
        DrawSceneNode(crateModel, shader, XMMatrixIdentity(), view, proj, lightSpace);
        // Imported model used the mesh pipeline. Restore IA pipeline for
        // procedural objects that follow it.
        shader.Use(scene.wireframeMode);
    } else {
        model = scene.cube1.GetModelMatrix();
        shader.SetMatrices(model, view, proj, lightSpace);
        shader.SetObjectColor(scene.cube1.color);
        DrawCube(geo);
        shader.NextDrawCall();
    }

    // Separate destructible brick wall beside the house.
    if (scene.useDestruction && g_destruction.IsInitialized()) {
        for (const DestructionRenderItem& item : g_destruction.GetRenderItems()) {
            DrawSceneNode(item.node, shader, XMLoadFloat4x4(&item.transform), view, proj, lightSpace);
        }
        shader.Use(scene.wireframeMode);
    }

    // Cube 2
    if (scene.cube2.visible) {
        model = scene.cube2.GetModelMatrix();
        shader.SetMatrices(model, view, proj, lightSpace);
        shader.SetObjectColor(scene.cube2.color);
        DrawCube(geo);
        shader.NextDrawCall();
    }

    // Projectiles
    for (auto& p : scene.projectiles) {
        if (!p.active) continue;
        model = XMMatrixScaling(scene.projectileScale, scene.projectileScale, scene.projectileScale);
        model = model * XMMatrixTranslation(p.position.x, p.position.y, p.position.z);
        shader.SetMatrices(model, view, proj, lightSpace);
        shader.SetObjectColor(scene.projectileColor);
        DrawCube(geo);
        shader.NextDrawCall();
    }

    // Gun
    if (scene.gun.visible) {
        model = scene.GetGunModelMatrix();
        shader.SetMatrices(model, view, proj, lightSpace);
        shader.SetObjectColor(scene.gun.color);
        DrawCube(geo);
        shader.NextDrawCall();
    }

    // Light spheres
    for (int i = 0; i < scene.clusteredRenderer.getTotalLightCount(); i++) {
        PointLightDX12* light = scene.clusteredRenderer.getLight(i);
        if (!light || !light->active) continue;
        model = XMMatrixScaling(0.2f, 0.2f, 0.2f);
        model = model * XMMatrixTranslation(light->position.x, light->position.y, light->position.z);
        shader.SetMatrices(model, view, proj, lightSpace);
        XMFLOAT3 lc(light->color.x * light->intensity,
                     light->color.y * light->intensity,
                     light->color.z * light->intensity);
        shader.SetObjectColor(lc);
        DrawCube(geo);
        shader.NextDrawCall();
    }
}

#endif // FORWARD_RENDERER_H
