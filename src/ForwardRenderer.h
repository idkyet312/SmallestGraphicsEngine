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
#include "RoofModel.h"
#include "WaterVolume.h"
#include "RopeSwing.h"
#include "PalmTrees.h"
#include "PalmModel.h"
#include "GunModel.h"
#include "GrassField.h"

extern MeshShaderDX12 g_meshShader;
extern bool g_useMeshShader;
extern TerrainRendererDX12 g_terrain;
extern bool g_showH2Model;
extern WaterVolume g_water;
extern WaterVolume g_ocean;   // sea ringing the island
extern ComPtr<ID3D12Resource> g_smokeTexture;   // soft smoke sprite for billboards

struct GeometryBuffers {
    ComPtr<ID3D12Resource>   cubeVertexBuffer;
    ComPtr<ID3D12Resource>   planeVertexBuffer;
    ComPtr<ID3D12Resource>   sphereVertexBuffer;
    ComPtr<ID3D12Resource>   quadVertexBuffer;      // unit XY billboard quad
    D3D12_VERTEX_BUFFER_VIEW cubeVBV  = {};
    D3D12_VERTEX_BUFFER_VIEW planeVBV = {};
    D3D12_VERTEX_BUFFER_VIEW sphereVBV = {};
    D3D12_VERTEX_BUFFER_VIEW quadVBV = {};
    UINT                     sphereVertexCount = 0;
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

// A unit quad in the XY plane (-0.5..0.5, UV 0..1), for camera-facing billboards.
inline void DrawQuad(const GeometryBuffers& geo) {
    g_dx12.commandList->IASetVertexBuffers(0, 1, &geo.quadVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(6, 1, 0, 0);
}

inline void DrawSphere(const GeometryBuffers& geo) {
    if (!geo.sphereVertexCount) { DrawCube(geo); return; }
    g_dx12.commandList->IASetVertexBuffers(0, 1, &geo.sphereVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(geo.sphereVertexCount, 1, 0, 0);
}

// UV sphere as a non-indexed triangle list of VertexPosNormUV.
inline std::vector<VertexPosNormUV> BuildSphereVertices(int stacks = 12, int slices = 16) {
    std::vector<VertexPosNormUV> verts;
    auto at = [&](int st, int sl) {
        const float v = (float)st / stacks, u = (float)sl / slices;
        const float phi = v * 3.14159265f, theta = u * 2.0f * 3.14159265f;
        const float sp = sinf(phi), cp = cosf(phi), stc = sinf(theta), ctc = cosf(theta);
        XMFLOAT3 n(sp * ctc, cp, sp * stc);
        return VertexPosNormUV{ { n.x * 0.5f, n.y * 0.5f, n.z * 0.5f }, n, { u, v } };
    };
    for (int st = 0; st < stacks; ++st) for (int sl = 0; sl < slices; ++sl) {
        VertexPosNormUV a = at(st, sl), b = at(st + 1, sl), c = at(st + 1, sl + 1), d = at(st, sl + 1);
        verts.push_back(a); verts.push_back(b); verts.push_back(c);
        verts.push_back(a); verts.push_back(c); verts.push_back(d);
    }
    return verts;
}

// Draw a standalone mesh's primitives at `model`, honouring their materials. This
// is the per-primitive body of DrawSceneNode, pulled out so meshes that are not
// part of a scene graph (e.g. the sliced palm) can be drawn the same way.
inline void DrawMeshAt(const std::shared_ptr<SceneMesh>& mesh, ShaderDX12& shader,
                       const XMMATRIX& model,
                       const XMMATRIX& view, const XMMATRIX& proj, const XMMATRIX& lightSpace) {
    if (!mesh) return;
    shader.SetMatrices(model, view, proj, lightSpace);

    for (const auto& prim : mesh->primitives) {
        if (prim.vbv.BufferLocation == 0) continue;

        const bool transparent = prim.material && prim.material->baseColorFactor.w < 0.999f;
        if (prim.material) {
            if (transparent) shader.UseTransparent(); else shader.Use(false);
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
                prim.material->metallicRoughnessTexture.Get(),
                prim.material->roughnessOnlyTexture,
                prim.material->baseColorFactor.w,
                prim.material->alphaCutout);
        } else {
            shader.SetObjectMaterial(XMFLOAT3(1, 1, 1), false, false, 0.0f, 0.5f, nullptr, nullptr, nullptr);
        }

        // Palm slices are IA meshes (no meshlet data), so always take the VS/PS path.
        g_dx12.commandList->SetPipelineState(transparent
            ? shader.transparentPipelineState.Get() : shader.pipelineState.Get());
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

            const bool transparent = prim.material && prim.material->baseColorFactor.w < 0.999f;
            if (prim.material) {
                if (transparent) shader.UseTransparent(); else shader.Use(false);
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
                    prim.material->metallicRoughnessTexture.Get(),
                    prim.material->roughnessOnlyTexture,
                    prim.material->baseColorFactor.w);
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
            if (!transparent && g_useMeshShader && g_meshShader.CanDraw(
                    prim.meshletCount, meshletDescAddress, boundsAddress,
                    vertexIndexAddress, triangleAddress)) {
                g_meshShader.Draw(prim.vbv,
                    (UINT)(prim.vertices.size() / 12), prim.indexCount,
                    prim.meshletCount, meshletDescAddress, boundsAddress,
                    vertexIndexAddress, triangleAddress);
            } else {
                // A previous mesh draw leaves the mesh PSO bound. Restore the
                // conventional VS/PS pipeline before issuing IA draw calls.
                g_dx12.commandList->SetPipelineState(transparent
                    ? shader.transparentPipelineState.Get() : shader.pipelineState.Get());
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
                                 floorMaterial->metallicRoughnessTexture.Get(),
                                 floorMaterial->roughnessOnlyTexture);
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
    if (crateModel && g_showH2Model) {
        DrawSceneNode(crateModel, shader, XMMatrixIdentity(), view, proj, lightSpace);
        // Imported model used the mesh pipeline. Restore IA pipeline for
        // procedural objects that follow it.
        shader.Use(scene.wireframeMode);
    }

    // Separate destructible brick wall beside the house.
    if (scene.useDestruction && g_destruction.IsInitialized()) {
        for (const DestructionRenderItem& item : g_destruction.GetRenderItems()) {
            DrawSceneNode(item.node, shader, XMLoadFloat4x4(&item.transform), view, proj, lightSpace);
        }
        shader.Use(scene.wireframeMode);
        for (const RagdollRenderItem& item : g_destruction.GetRagdollRenderItems()) {
            shader.SetMatrices(XMLoadFloat4x4(&item.transform), view, proj, lightSpace);
            shader.SetObjectColor(item.color);
            DrawCube(geo);
            shader.NextDrawCall();
        }
    }

    // Palm trees: trunk sections and fronds are boxes driven by physics bodies,
    // standing or toppling, so they all draw through the ordinary cube path.
    if (g_trees.IsInitialized()) {
        shader.Use(scene.wireframeMode);
        for (const TreeItem& item : g_trees.GetItems()) {
            const XMMATRIX xf = XMLoadFloat4x4(&item.transform);
            // If the real palm model loaded, each physics box carries the identity
            // of a model slice (a trunk segment or the crown); draw that slice's
            // geometry at the box's transform. Otherwise the item is a plain box.
            std::shared_ptr<SceneMesh> slice;
            if (item.crown) slice = PalmModel::Crown();
            else if (item.segment >= 0 && item.segment < (int)PalmModel::TrunkSlices().size())
                slice = PalmModel::TrunkSlices()[item.segment].mesh;

            if (slice) {
                DrawMeshAt(slice, shader, xf, view, proj, lightSpace);
            } else {
                shader.Use(scene.wireframeMode);
                shader.SetMatrices(xf, view, proj, lightSpace);
                shader.SetObjectColor(item.color);
                DrawCube(geo);
                shader.NextDrawCall();
            }
        }
        // A palm slice draw may have left a non-cube pipeline bound.
        shader.Use(scene.wireframeMode);
    }

    // Grass: an INSTANCED draw of one 8-vertex blade, bent in the wind by its own
    // vertex shader. This is how a real engine draws foliage, and it is the third
    // version -- the first re-simulated every blade on the CPU each frame (~11 ms,
    // more than the whole rest of the scene), and the second made the geometry
    // static but still pushed 1.4M unique vertices through the vertex fetch.
    //
    // Two things keep it cheap. The mesh is a single blade template drawn once per
    // blade, so the per-blade data is read once from a structured buffer instead of
    // being duplicated across eight vertices. And the field is submitted a patch at
    // a time, so distant grass is not drawn at all -- shrinking blades away in the
    // shader is not enough, because a zero-height blade still costs vertex shading
    // and triangle setup.
    //
    // Blades are built in world space, hence the identity model matrix. Opaque, so
    // this lands before the transparent water below or it would sort wrong.
    if (g_grass.IsInitialized() && shader.grassPipelineState) {
        // The shader fades blades with distance, and the cull below needs it too.
        g_grass.SetViewer(scene.camera.Position);

        static std::vector<GrassField::DrawRange> grassRanges;
        g_grass.GetVisible(grassRanges);

        const D3D12_VERTEX_BUFFER_VIEW& gvbv = g_grass.GetVBV();
        const D3D12_GPU_VIRTUAL_ADDRESS ginst = g_grass.GetInstanceBufferAddress();
        if (!grassRanges.empty() && gvbv.BufferLocation && ginst) {
            const D3D12_INDEX_BUFFER_VIEW& gibv = g_grass.GetIBV();

            shader.SetMatrices(XMMatrixIdentity(), view, proj, lightSpace);
            // Untextured: a flat green with a matte response.
            shader.SetObjectMaterial(XMFLOAT3(0.20f, 0.42f, 0.13f), false, false,
                                     0.0f, 0.85f, nullptr, nullptr, nullptr);

            // Wind parameters as root constants (b6), and the per-blade data as a
            // root SRV (t6). Both slots exist for the terrain mesh-shader path and
            // are unused by the raster path, so the grass borrows them and needs no
            // root-signature change of its own. (Terrain and grass never draw in the
            // same call, so sharing the slots is safe.)
            GrassField::Params gp = g_grass.GetParams();
            static_assert(sizeof(GrassField::Params) == 8 * sizeof(UINT),
                          "GrassParams must match the 8 root constants at b6");
            g_dx12.commandList->SetGraphicsRoot32BitConstants(8, 8, &gp, 0);
            g_dx12.commandList->SetGraphicsRootShaderResourceView(9, ginst);

            g_dx12.commandList->SetPipelineState(shader.grassPipelineState.Get());
            g_dx12.commandList->IASetVertexBuffers(0, 1, &gvbv);
            g_dx12.commandList->IASetIndexBuffer(&gibv);
            g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            // Every patch draws the same template with the same material. The only
            // thing that changes is which run of blades it reads -- and that has to
            // go in as a root constant, because SV_InstanceID ignores
            // StartInstanceLocation for a structured buffer (see grass_vs.hlsl).
            for (const GrassField::DrawRange& r : grassRanges) {
                g_dx12.commandList->SetGraphicsRoot32BitConstants(8, 1, &r.firstInstance, 7);
                g_dx12.commandList->DrawIndexedInstanced(
                    GrassField::IndexCount(), r.instanceCount, 0, 0, 0);
            }
            shader.NextDrawCall();

            // Hand the pipeline back to the ordinary object shader for what follows.
            shader.Use(scene.wireframeMode);
        }
    }

    // Rope-hung block: each rope link and the block are just boxes driven by the
    // physics bodies, so they draw with the ordinary cube path.
    if (g_rope.IsInitialized()) {
        shader.Use(scene.wireframeMode);
        for (const RopeItem& item : g_rope.GetItems()) {
            shader.SetMatrices(XMLoadFloat4x4(&item.transform), view, proj, lightSpace);
            shader.SetObjectColor(item.color);
            DrawCube(geo);
            shader.NextDrawCall();
        }
    }

    // Second rope rig: the strung-up ragdoll. Same boxes, same path.
    if (g_gibbet.IsInitialized()) {
        shader.Use(scene.wireframeMode);
        for (const RopeItem& item : g_gibbet.GetItems()) {
            shader.SetMatrices(XMLoadFloat4x4(&item.transform), view, proj, lightSpace);
            shader.SetObjectColor(item.color);
            DrawCube(geo);
            shader.NextDrawCall();
        }
    }

    // The sea around the island. Drawn before the pool because it is the farther
    // transparent surface, and blended surfaces have to go back-to-front.
    if (g_ocean.IsInitialized()) {
        const D3D12_VERTEX_BUFFER_VIEW& ovbv = g_ocean.UpdateAndGetVBV(g_dx12.frameIndex);
        const D3D12_INDEX_BUFFER_VIEW& oibv = g_ocean.GetIBV();
        const UINT oceanIndices = g_ocean.GetIndexCount();
        if (oceanIndices && ovbv.BufferLocation) {
            shader.UseTransparent();
            shader.SetMatrices(XMMatrixIdentity(), view, proj, lightSpace);
            // Deeper and less transparent than the pool: open water reads darker.
            shader.SetObjectMaterial(XMFLOAT3(0.03f, 0.18f, 0.38f), false, false,
                                     0.04f, 0.05f, nullptr, nullptr, nullptr, false, 0.80f);
            g_dx12.commandList->IASetVertexBuffers(0, 1, &ovbv);
            g_dx12.commandList->IASetIndexBuffer(&oibv);
            g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_dx12.commandList->DrawIndexedInstanced(oceanIndices, 1, 0, 0, 0);
            shader.NextDrawCall();
        }
        shader.Use(scene.wireframeMode);
    }

    // Water pool: floating crates + any debris (opaque) drawn first, then the
    // animated, undulating water surface on top so the floaters show through it.
    if (g_water.IsInitialized()) {
        shader.Use(scene.wireframeMode);
        for (const WaterFloaterItem& item : g_water.GetFloaterItems()) {
            shader.SetMatrices(XMLoadFloat4x4(&item.transform), view, proj, lightSpace);
            shader.SetObjectColor(item.color);
            DrawCube(geo);
            shader.NextDrawCall();
        }
        // Wave surface: positions/normals recomputed on the CPU each frame into a
        // per-frame upload buffer, drawn as a translucent glossy sheet so the
        // moving swell catches the light.
        const D3D12_VERTEX_BUFFER_VIEW& wvbv = g_water.UpdateAndGetVBV(g_dx12.frameIndex);
        const D3D12_INDEX_BUFFER_VIEW& wibv = g_water.GetIBV();
        const UINT waterIndices = g_water.GetIndexCount();
        if (waterIndices && wvbv.BufferLocation) {
            shader.UseTransparent();
            shader.SetMatrices(XMMatrixIdentity(), view, proj, lightSpace);  // verts are world-space
            shader.SetObjectMaterial(XMFLOAT3(0.06f, 0.30f, 0.55f), false, false,
                                     0.05f, 0.06f, nullptr, nullptr, nullptr, false, 0.62f);
            g_dx12.commandList->IASetVertexBuffers(0, 1, &wvbv);
            g_dx12.commandList->IASetIndexBuffer(&wibv);
            g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_dx12.commandList->DrawIndexedInstanced(waterIndices, 1, 0, 0, 0);
            shader.NextDrawCall();
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

    // Projectiles. Grenades: dark spheres. Bullets: bright tracer rounds -- a
    // thin streak stretched along the flight direction, glowing hot so it reads
    // like a real tracer whipping downrange.
    for (auto& p : scene.projectiles) {
        if (!p.active) continue;
        if (p.grenade) {
            model = XMMatrixScaling(scene.projectileScale * 1.6f, scene.projectileScale * 1.6f,
                                    scene.projectileScale * 1.6f) *
                    XMMatrixTranslation(p.position.x, p.position.y, p.position.z);
            shader.SetMatrices(model, view, proj, lightSpace);
            shader.SetObjectMaterial(XMFLOAT3(0.10f, 0.12f, 0.08f), false, false,
                                     0.6f, 0.5f, nullptr, nullptr, nullptr);
            DrawSphere(geo);
            shader.NextDrawCall();
            continue;
        }

        // Orthonormal basis with local +Z along the bullet's travel direction.
        XMVECTOR fwd = XMLoadFloat3(&p.direction);
        if (XMVectorGetX(XMVector3LengthSq(fwd)) < 1e-6f) fwd = XMVectorSet(0, 0, 1, 0);
        fwd = XMVector3Normalize(fwd);
        XMVECTOR up0 = fabsf(XMVectorGetY(fwd)) > 0.95f ? XMVectorSet(1, 0, 0, 0)
                                                        : XMVectorSet(0, 1, 0, 0);
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(up0, fwd));
        XMVECTOR up    = XMVector3Cross(fwd, right);
        XMMATRIX basis = XMMatrixIdentity();
        basis.r[0] = XMVectorSetW(right, 0.0f);
        basis.r[1] = XMVectorSetW(up, 0.0f);
        basis.r[2] = XMVectorSetW(fwd, 0.0f);
        basis.r[3] = XMVectorSet(p.position.x, p.position.y, p.position.z, 1.0f);

        const float r = scene.projectileScale * 0.28f;   // slim tracer
        const float len = scene.projectileScale * 6.0f;  // long streak
        model = XMMatrixScaling(r * 2.0f, r * 2.0f, len) * basis;
        shader.SetMatrices(model, view, proj, lightSpace);
        // Hot tracer glow (bright, low-roughness so it reads as emissive-ish).
        shader.SetObjectMaterial(XMFLOAT3(3.0f, 1.4f, 0.35f), false, false,
                                 0.0f, 0.9f, nullptr, nullptr, nullptr);
        DrawCube(geo);
        shader.NextDrawCall();
    }

    // Impact particles. Sparks: opaque bright cubes (hot debris shards). Smoke:
    // soft camera-facing billboards using the smoke sprite, alpha-blended so they
    // read as real translucent, light-scattering puffs.
    //
    // Camera right/up come from the view matrix's rotation (its transpose maps
    // view axes back to world), so every quad faces the camera.
    XMMATRIX invRot = XMMatrixTranspose(view);
    const XMVECTOR camRight = XMVectorSetW(invRot.r[0], 0.0f);
    const XMVECTOR camUp    = XMVectorSetW(invRot.r[1], 0.0f);
    const XMVECTOR camFwd   = XMVectorSetW(invRot.r[2], 0.0f);

    // Opaque sparks first.
    shader.Use(scene.wireframeMode);
    for (auto& sp : scene.impactParticles) {
        if (!sp.spark) continue;
        const float fade = sp.life / sp.maxLife;
        model = XMMatrixScaling(sp.size, sp.size, sp.size) *
                XMMatrixTranslation(sp.position.x, sp.position.y, sp.position.z);
        shader.SetMatrices(model, view, proj, lightSpace);
        const float b = 0.6f + 0.4f * fade;
        shader.SetObjectColor(XMFLOAT3(sp.color.x * b, sp.color.y * b, sp.color.z * b));
        DrawCube(geo);
        shader.NextDrawCall();
    }

    // Translucent smoke billboards.
    shader.UseTransparent();
    for (auto& sp : scene.impactParticles) {
        if (sp.spark) continue;
        const float fade = sp.life / sp.maxLife;      // 1 fresh -> 0 dead
        const float age = 1.0f - fade;
        // Opacity ramps up as the puff first blooms, then fades out at the end.
        const float fadeIn = age < 0.15f ? age / 0.15f : 1.0f;
        const float fadeOut = fade < 0.4f ? fade / 0.4f : 1.0f;
        const float opacity = (std::min)(0.85f, fadeIn * fadeOut * 0.85f);
        if (opacity <= 0.01f) { shader.NextDrawCall(); continue; }

        // Billboard: build a world basis from the camera axes, scaled by size.
        const XMVECTOR pos = XMVectorSet(sp.position.x, sp.position.y, sp.position.z, 1.0f);
        model = XMMATRIX(camRight * sp.size, camUp * sp.size, camFwd * sp.size,
                         XMVectorSetW(pos, 1.0f));
        shader.SetMatrices(model, view, proj, lightSpace);
        // Smoke lightens from sooty core to grey as it disperses.
        const float g = 1.0f + 2.0f * age;
        const XMFLOAT3 tint((std::min)(1.0f, sp.color.x * g),
                            (std::min)(1.0f, sp.color.y * g),
                            (std::min)(1.0f, sp.color.z * g));
        shader.SetSmokeMaterial(tint, opacity, g_smokeTexture.Get());
        DrawQuad(geo);
        shader.NextDrawCall();
    }
    shader.Use(scene.wireframeMode);

    // Gun: the AK47 model, drawn in the gun's local space (+Z down the barrel)
    // and placed in front of the camera. If the FBX did not load, fall back to
    // the boxed carbine below so the player still sees a weapon.
    if (scene.gun.visible) {
        const XMMATRIX gunBase = scene.GetGunBaseMatrix();
        const float S = scene.GunModelScale();

        if (GunModel::Loaded()) {
            // The model is normalised with its origin at the rear of the weapon,
            // so shift it back and down into the same pocket of screen space the
            // boxed M4 occupied, then scale to the gun's on-screen size.
            const XMMATRIX xf =
                XMMatrixScaling(S, S, S) *
                XMMatrixTranslation(0.0f, -0.10f * S, -0.44f * S) *
                gunBase;
            shader.Use(scene.wireframeMode);
            DrawMeshAt(GunModel::Mesh(), shader, xf, view, proj, lightSpace);
            shader.Use(scene.wireframeMode);
        } else {
            // Fallback: the old M4-style carbine, built from boxed parts.
            // Gunmetal / polymer palette.
            const XMFLOAT3 metal(0.14f, 0.14f, 0.16f);   // receiver, barrel
            const XMFLOAT3 poly (0.09f, 0.10f, 0.11f);   // handguard, stock, grip
            const XMFLOAT3 mag  (0.11f, 0.12f, 0.13f);   // magazine
            const XMFLOAT3 iron (0.05f, 0.05f, 0.06f);   // sights, muzzle

            struct Part { XMFLOAT3 c, h, col; };
            // center (x,y,z), half-extents (x,y,z), colour -- local units.
            const Part parts[] = {
                // Upper + lower receiver (main body).
                {{0.00f,  0.00f,  0.05f}, {0.055f, 0.075f, 0.26f}, metal},
                // Handguard around the barrel (forward, slightly fatter).
                {{0.00f, -0.01f,  0.42f}, {0.05f,  0.055f, 0.20f}, poly},
                // Barrel poking out of the handguard.
                {{0.00f,  0.01f,  0.66f}, {0.018f, 0.018f, 0.10f}, metal},
                // Flash hider / muzzle tip.
                {{0.00f,  0.01f,  0.78f}, {0.026f, 0.026f, 0.03f}, iron},
                // Magazine, canted slightly forward under the receiver.
                {{0.00f, -0.20f,  0.02f}, {0.04f,  0.13f,  0.055f}, mag},
                // Pistol grip, behind the mag.
                {{0.00f, -0.15f, -0.16f}, {0.035f, 0.10f,  0.04f}, poly},
                // Buffer tube + stock, to the rear.
                {{0.00f,  0.00f, -0.24f}, {0.03f,  0.05f,  0.10f}, metal},
                {{0.00f, -0.02f, -0.38f}, {0.05f,  0.085f, 0.06f}, poly},
                // Optic/carry-handle rail on top.
                {{0.00f,  0.10f,  0.02f}, {0.03f,  0.03f,  0.20f}, iron},
                // Front sight post.
                {{0.00f,  0.11f,  0.52f}, {0.014f, 0.05f,  0.02f}, iron},
                // Charging-handle bump at the back top.
                {{0.00f,  0.09f, -0.14f}, {0.028f, 0.024f, 0.05f}, iron},
            };

            shader.Use(scene.wireframeMode);
            for (const Part& p : parts) {
                model = XMMatrixScaling(p.h.x * 2.0f * S, p.h.y * 2.0f * S, p.h.z * 2.0f * S) *
                        XMMatrixTranslation(p.c.x * S, p.c.y * S, p.c.z * S) *
                        gunBase;
                shader.SetMatrices(model, view, proj, lightSpace);
                shader.SetObjectMaterial(p.col, false, false, 0.85f, 0.35f,
                                         nullptr, nullptr, nullptr);
                DrawCube(geo);
                shader.NextDrawCall();
            }
        }
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
