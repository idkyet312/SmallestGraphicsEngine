#ifndef IDTECH_RENDERER_H
#define IDTECH_RENDERER_H

// id Tech 8–style Visibility Buffer + Deferred hybrid.
//
// Pass 1  – Visibility rasterisation (ultra-thin VS/PS, writes drawID|triID to R32_UINT)
// Pass 2  – G-Buffer fill            (compute, reads VB + structured geometry ? writes compact G-Buffer)
// Pass 3  – Deferred lighting         (compute, reads G-Buffer + clustered lights ? writes final colour)
//
// The existing VisibilityBufferDX12 class handles Pass 1 + a single combined resolve.
// Here we orchestrate the full frame using that class and populate the light upload
// buffers correctly so the compute pass reads them via descriptor table.

#include "DX12Core.h"
#include "ShaderDX12.h"
#include "VisibilityBufferDX12.h"
#include "Scene.h"
#include "ForwardRenderer.h"
#include <array>
#include <algorithm>
#include <cmath>

// CPU-side flat vertex float arrays (pos3 norm3 uv2 = 8 floats per vert)
struct PackedGeometry {
    std::vector<float> cubeFloats;
    std::vector<float> planeFloats;
};

inline void BuildPackedGeometry(const std::vector<VertexPosNormUV>& cubeVerts,
                                const std::vector<VertexPosNormUV>& planeVerts,
                                PackedGeometry& out) {
    auto pack = [](const std::vector<VertexPosNormUV>& src, std::vector<float>& dst) {
        dst.resize(src.size() * 8);
        for (size_t i = 0; i < src.size(); i++) {
            dst[i * 8 + 0] = src[i].position.x; dst[i * 8 + 1] = src[i].position.y; dst[i * 8 + 2] = src[i].position.z;
            dst[i * 8 + 3] = src[i].normal.x;   dst[i * 8 + 4] = src[i].normal.y;   dst[i * 8 + 5] = src[i].normal.z;
            dst[i * 8 + 6] = src[i].texCoord.x; dst[i * 8 + 7] = src[i].texCoord.y;
        }
    };
    pack(cubeVerts, out.cubeFloats);
    pack(planeVerts, out.planeFloats);
}

enum IdTechMaterialId : UINT {
    MAT_FLOOR = 0,
    MAT_CUBE = 1,
    MAT_PROJECTILE = 2,
    MAT_GUN = 3,
    MAT_LIGHT_SPHERE = 4
};

struct IdTechDrawItem {
    XMMATRIX model;
    XMFLOAT3 color;
    bool isCube;
    UINT materialId;
    bool backfaceCullable;
};

struct FrustumPlanes {
    std::array<XMFLOAT4, 6> p;
};

inline XMFLOAT4 NormalizePlane(const XMFLOAT4& in) {
    XMVECTOR v = XMLoadFloat4(&in);
    XMVECTOR n = XMVector3Normalize(v);
    XMFLOAT4 out;
    XMStoreFloat4(&out, n);
    float len = sqrtf(in.x * in.x + in.y * in.y + in.z * in.z);
    if (len > 1e-6f) out.w = in.w / len;
    return out;
}

inline FrustumPlanes ExtractFrustumPlanes(const XMMATRIX& vp) {
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, vp);
    FrustumPlanes f;
    f.p[0] = NormalizePlane(XMFLOAT4(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41)); // left
    f.p[1] = NormalizePlane(XMFLOAT4(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41)); // right
    f.p[2] = NormalizePlane(XMFLOAT4(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42)); // top
    f.p[3] = NormalizePlane(XMFLOAT4(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42)); // bottom
    f.p[4] = NormalizePlane(XMFLOAT4(m._13, m._23, m._33, m._43));                                   // near
    f.p[5] = NormalizePlane(XMFLOAT4(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43)); // far
    return f;
}

inline bool SphereInsideFrustum(const FrustumPlanes& f, const XMFLOAT3& c, float r) {
    for (const auto& p : f.p) {
        float d = p.x * c.x + p.y * c.y + p.z * c.z + p.w;
        if (d < -r) return false;
    }
    return true;
}

inline float ApproximateProjectedPixelArea(float radius, const XMFLOAT3& center,
                                           const XMFLOAT3& camPos, float fovDeg, float screenH) {
    XMFLOAT3 d = XMFLOAT3(center.x - camPos.x, center.y - camPos.y, center.z - camPos.z);
    float dist = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    dist = (dist < 0.001f) ? 0.001f : dist;
    float tanHalf = tanf(XMConvertToRadians(fovDeg) * 0.5f);
    float pxRadius = (radius / (dist * tanHalf)) * (screenH * 0.5f);
    return 3.14159265f * pxRadius * pxRadius;
}

inline XMFLOAT3 TransformPoint(const XMMATRIX& m, const XMFLOAT3& p) {
    XMVECTOR v = XMVectorSet(p.x, p.y, p.z, 1.0f);
    XMVECTOR t = XMVector4Transform(v, m);
    XMFLOAT3 out;
    XMStoreFloat3(&out, t);
    return out;
}

inline XMFLOAT3 TransformDir(const XMMATRIX& m, const XMFLOAT3& d) {
    XMVECTOR v = XMVectorSet(d.x, d.y, d.z, 0.0f);
    XMVECTOR t = XMVector3Normalize(XMVector3TransformNormal(v, m));
    XMFLOAT3 out;
    XMStoreFloat3(&out, t);
    return out;
}

inline void BuildSceneDrawItems(Scene& scene, std::vector<IdTechDrawItem>& items) {
    items.clear();

    items.push_back({ XMMatrixIdentity(), scene.floor.color, false, MAT_FLOOR, true });
    items.push_back({ scene.cube1.GetModelMatrix(), scene.cube1.color, true, MAT_CUBE, false });

    if (scene.cube2.visible) {
        items.push_back({ scene.cube2.GetModelMatrix(), scene.cube2.color, true, MAT_CUBE, false });
    }

    for (auto& p : scene.projectiles) {
        if (!p.active) continue;
        XMMATRIX m = XMMatrixScaling(scene.projectileScale, scene.projectileScale, scene.projectileScale)
                   * XMMatrixTranslation(p.position.x, p.position.y, p.position.z);
        items.push_back({ m, scene.projectileColor, true, MAT_PROJECTILE, false });
    }

    if (scene.gun.visible) {
        items.push_back({ scene.GetGunModelMatrix(), scene.gun.color, true, MAT_GUN, false });
    }

    for (int i = 0; i < scene.clusteredRenderer.getTotalLightCount(); i++) {
        PointLightDX12* l = scene.clusteredRenderer.getLight(i);
        if (!l || !l->active) continue;
        XMMATRIX m = XMMatrixScaling(0.2f, 0.2f, 0.2f)
                   * XMMatrixTranslation(l->position.x, l->position.y, l->position.z);
        XMFLOAT3 lc(l->color.x * l->intensity, l->color.y * l->intensity, l->color.z * l->intensity);
        items.push_back({ m, lc, true, MAT_LIGHT_SPHERE, false });
    }
}

inline void CullAndBatchDrawItems(Scene& scene, const XMMATRIX& view, const XMMATRIX& proj,
                                  std::vector<IdTechDrawItem>& items) {
    FrustumPlanes frustum = ExtractFrustumPlanes(view * proj);
    const float smallTriPixelAreaThreshold = 2.0f;

    std::vector<IdTechDrawItem> visible;
    visible.reserve(items.size());

    for (const auto& item : items) {
        XMFLOAT3 center = TransformPoint(item.model, XMFLOAT3(0, 0, 0));

        float radius;
        if (item.isCube) {
            XMFLOAT4X4 m;
            XMStoreFloat4x4(&m, item.model);
            float sx = sqrtf(m._11 * m._11 + m._12 * m._12 + m._13 * m._13);
            float sy = sqrtf(m._21 * m._21 + m._22 * m._22 + m._23 * m._23);
            float sz = sqrtf(m._31 * m._31 + m._32 * m._32 + m._33 * m._33);
            float maxScale = std::max(sx, std::max(sy, sz));
            radius = 0.8660254f * maxScale;
        } else {
            radius = 28.5f; // Plane approx radius for side ~40
        }

        // Frustum culling
        if (!SphereInsideFrustum(frustum, center, radius)) continue;

        // Backface culling (object-level) where safe
        if (item.backfaceCullable) {
            XMFLOAT3 worldNormal = TransformDir(item.model, XMFLOAT3(0, 1, 0));
            XMFLOAT3 toCamera(scene.camera.Position.x - center.x,
                              scene.camera.Position.y - center.y,
                              scene.camera.Position.z - center.z);
            float facing = worldNormal.x * toCamera.x + worldNormal.y * toCamera.y + worldNormal.z * toCamera.z;
            if (facing <= 0.0f) continue;
        }

        // Small-triangle culling via projected-area estimate
        float pxArea = ApproximateProjectedPixelArea(radius, center, scene.camera.Position,
                                                     scene.cameraFOV, (float)g_dx12.screenHeight);
        if (pxArea < smallTriPixelAreaThreshold) continue;

        visible.push_back(item);
    }

    // Batch by material ID for cache/coherence
    std::sort(visible.begin(), visible.end(),
        [](const IdTechDrawItem& a, const IdTechDrawItem& b) {
            return a.materialId < b.materialId;
        });

    items.swap(visible);
}

#pragma pack(push, 1)
struct VisIndirectCommand {
    D3D12_VERTEX_BUFFER_VIEW vbv;
    D3D12_GPU_VIRTUAL_ADDRESS matrixCBV;
    UINT drawCallID;
    D3D12_DRAW_ARGUMENTS drawArgs;
};
#pragma pack(pop)

static_assert(sizeof(VisIndirectCommand) == sizeof(D3D12_VERTEX_BUFFER_VIEW) + sizeof(D3D12_GPU_VIRTUAL_ADDRESS) + sizeof(UINT) + sizeof(D3D12_DRAW_ARGUMENTS),
    "VisIndirectCommand must be tightly packed for ExecuteIndirect.");

struct GPUDrivenVisibilityContext {
    bool initialized = false;
    UINT maxCommands = 2048;
    ComPtr<ID3D12CommandSignature> commandSignature;
    ComPtr<ID3D12Resource> indirectBuffer;
    VisIndirectCommand* mappedCommands = nullptr;

    bool Init(ID3D12RootSignature* visRootSig) {
        if (initialized) return true;

        D3D12_INDIRECT_ARGUMENT_DESC args[4] = {};
        args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
        args[0].VertexBuffer.Slot = 0;
        args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
        args[1].ConstantBufferView.RootParameterIndex = 0; // matrix CBV
        args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        args[2].Constant.RootParameterIndex = 1; // drawCallID root constants
        args[2].Constant.DestOffsetIn32BitValues = 0;
        args[2].Constant.Num32BitValuesToSet = 1;
        args[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

        D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
        sigDesc.ByteStride = sizeof(VisIndirectCommand);
        sigDesc.NumArgumentDescs = _countof(args);
        sigDesc.pArgumentDescs = args;

        HRESULT hr = g_dx12.device->CreateCommandSignature(&sigDesc, visRootSig, IID_PPV_ARGS(&commandSignature));
        if (FAILED(hr)) return false;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = sizeof(VisIndirectCommand) * maxCommands;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&indirectBuffer));
        if (FAILED(hr)) return false;

        D3D12_RANGE rr = { 0, 0 };
        hr = indirectBuffer->Map(0, &rr, reinterpret_cast<void**>(&mappedCommands));
        if (FAILED(hr)) return false;

        initialized = true;
        return true;
    }
};

inline void FillMatrixBufferForIndirect(ShaderDX12& matrixShader, UINT drawIndex,
                                        const XMMATRIX& model, const XMMATRIX& view,
                                        const XMMATRIX& proj, const XMMATRIX& lightSpace,
                                        D3D12_GPU_VIRTUAL_ADDRESS& outCBV) {
    UINT bufferIndex = g_dx12.frameIndex * MAX_DRAW_CALLS_PER_FRAME + drawIndex;

    MatrixBufferDX12 data;
    data.model = XMMatrixTranspose(model);
    data.view = XMMatrixTranspose(view);
    data.projection = XMMatrixTranspose(proj);
    data.lightSpaceMatrix = XMMatrixTranspose(lightSpace);
    matrixShader.matrixBuffer.CopyData(bufferIndex, data);

    outCBV = matrixShader.matrixBuffer.GetGPUAddress(bufferIndex);
}

// Full id Tech–style frame.
// Pass 1: Visibility rasterisation
// Pass 2+3 (combined in visbuf_resolve_cs): G-Buffer fill + deferred lighting
inline void RenderIdTech(Scene& scene, ShaderDX12& shader,
                         VisibilityBufferDX12& vb,
                         const GeometryBuffers& geo,
                         const PackedGeometry& packed,
                         const XMMATRIX& lightSpace,
                         ID3D12Resource* shadowResource) {
    XMMATRIX view = scene.GetViewMatrix();
    XMMATRIX proj = scene.GetProjectionMatrix();

    // Build draw item list
    std::vector<IdTechDrawItem> drawItems;
    BuildSceneDrawItems(scene, drawItems);

    // Culling before visibility pass (frustum + backface + small triangle)
    CullAndBatchDrawItems(scene, view, proj, drawItems);

    scene.clusteredRenderer.setScreenSize((float)g_dx12.screenWidth, (float)g_dx12.screenHeight);
    scene.clusteredRenderer.setCamera(scene.cameraFOV, scene.cameraNear,
        scene.cameraFar, view, proj);
    scene.clusteredRenderer.cullLights();

    // Register immutable meshes once. Frames upload instance/material records only.
    static UINT cubeMesh = VB_INVALID_MESH;
    static UINT planeMesh = VB_INVALID_MESH;
    if (cubeMesh == VB_INVALID_MESH)
        cubeMesh = vb.RegisterMesh(packed.cubeFloats.data(),
            (UINT)(packed.cubeFloats.size() / 8), nullptr, 0);
    if (planeMesh == VB_INVALID_MESH)
        planeMesh = vb.RegisterMesh(packed.planeFloats.data(),
            (UINT)(packed.planeFloats.size() / 8), nullptr, 0);

    vb.BeginFrame();
    for (UINT clusterIndex = 0;
         clusterIndex < (UINT)scene.clusteredRenderer.clusters.size();
         ++clusterIndex) {
        const auto& cluster = scene.clusteredRenderer.clusters[clusterIndex];
        vb.SetCluster(clusterIndex, (UINT)cluster.lightCount, cluster.lightIndices);
    }
    std::vector<UINT> dcIDs;
    dcIDs.reserve(drawItems.size());

    for (const auto& item : drawItems) {
        UINT dc = vb.RegisterInstance(item.isCube ? cubeMesh : planeMesh,
            item.model, item.color, 0.0f, 0.5f, item.materialId);
        dcIDs.push_back(dc);
    }

    vb.UploadBuffers(g_dx12.commandList.Get());

    // Pass 1: visibility rasterisation (GPU-driven ExecuteIndirect)
    vb.BeginVisibilityPass(g_dx12.commandList.Get());

    static GPUDrivenVisibilityContext gpuDriven;
    if (!gpuDriven.initialized) {
        gpuDriven.Init(vb.visPassRootSig.Get());
    }

    UINT drawCount = (UINT)drawItems.size();
    if (drawCount > gpuDriven.maxCommands) drawCount = gpuDriven.maxCommands;
    if (drawCount > MAX_DRAW_CALLS_PER_FRAME) drawCount = MAX_DRAW_CALLS_PER_FRAME;

    for (UINT i = 0; i < drawCount; i++) {
        D3D12_GPU_VIRTUAL_ADDRESS matrixCBV = 0;
        FillMatrixBufferForIndirect(shader, i, drawItems[i].model, view, proj, lightSpace, matrixCBV);

        gpuDriven.mappedCommands[i].vbv = drawItems[i].isCube ? geo.cubeVBV : geo.planeVBV;
        gpuDriven.mappedCommands[i].matrixCBV = matrixCBV;
        gpuDriven.mappedCommands[i].drawCallID = dcIDs[i];
        gpuDriven.mappedCommands[i].drawArgs.VertexCountPerInstance = drawItems[i].isCube ? 36 : 6;
        gpuDriven.mappedCommands[i].drawArgs.InstanceCount = 1;
        gpuDriven.mappedCommands[i].drawArgs.StartVertexLocation = 0;
        gpuDriven.mappedCommands[i].drawArgs.StartInstanceLocation = 0;
    }

    if (drawCount > 0 && gpuDriven.commandSignature) {
        g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_dx12.commandList->ExecuteIndirect(
            gpuDriven.commandSignature.Get(),
            drawCount,
            gpuDriven.indirectBuffer.Get(),
            0,
            nullptr,
            0);
    }

    vb.EndVisibilityPass(g_dx12.commandList.Get());

    // Lighting setup
    {
        LightBufferDX12 lb = {};
        lb.lightPos = scene.lightPos;
        lb.lightType = scene.lightType;
        lb.lightColor = scene.lightColor;
        lb.constant = scene.lightConstant;
        lb.linear = scene.lightLinear;
        lb.quadratic = scene.lightQuadratic;
        lb.ambientStrength = scene.ambientStrength;
        lb.specularStrength = scene.specularStrength;
        lb.shininess = scene.specularShininess;
        lb.shadowBias = scene.shadowBias;
        lb.enableShadows = scene.enableShadows ? 1 : 0;
        shader.lightBuffer.CopyData(g_dx12.frameIndex, lb);

        PointLightsBufferDX12 plb = {};
        const auto& sourceLights = scene.clusteredRenderer.lights;
        int cnt = ((int)sourceLights.size() < 64) ? (int)sourceLights.size() : 64;
        plb.numPointLights = cnt;
        for (int i = 0; i < cnt; i++) {
            plb.lights[i].position = sourceLights[i].position;
            plb.lights[i].radius = sourceLights[i].radius;
            plb.lights[i].color = sourceLights[i].color;
            plb.lights[i].intensity = sourceLights[i].active
                ? sourceLights[i].intensity : 0.0f;
        }
        shader.pointLightsBuffer.CopyData(g_dx12.frameIndex, plb);
    }

    vb.UpdateLightDescriptors(
        shader.lightBuffer.GetGPUAddress(g_dx12.frameIndex),
        shader.pointLightsBuffer.GetGPUAddress(g_dx12.frameIndex));
    vb.UpdateShadowMapDescriptor(shadowResource);

    LightBufferDX12 dummyLB = {};
    PointLightsBufferDX12 dummyPL = {};
    vb.Resolve(g_dx12.commandList.Get(), view, proj, lightSpace,
        scene.camera.Position, scene.cameraNear, scene.cameraFar,
        dummyLB, dummyPL);

    vb.PostProcess(g_dx12.commandList.Get());
    vb.CopyToBackBuffer(g_dx12.commandList.Get());
    vb.TransitionBuffersForUpload(g_dx12.commandList.Get());
}

#endif // IDTECH_RENDERER_H
