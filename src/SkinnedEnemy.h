#pragma once
// A skinned character instance: owns the shared SkinnedModel, an animation
// player, and a per-frame bone-palette upload buffer, and knows how to draw
// itself through the mesh-shader path with skinning enabled.
#include "SkinnedFBXImporter.h"
#include "AnimationRuntime.h"
#include "MeshShaderDX12.h"
#include "DX12Core.h"
#include <DirectXMath.h>
#include <vector>

extern MeshShaderDX12 g_meshShader;

class SkinnedEnemy {
public:
    SkinnedModel      model;
    AnimationInstance anim;
    DirectX::XMFLOAT3 position{ 0, 0, 0 };
    float             yaw = 0.0f;     // radians, facing
    bool              visible = true;
    // UE mannequin is authored Z-up; the engine is Y-up, so the mesh imports
    // upside-down/rolled. rootPitch rotates it back onto its feet; footOffset
    // lifts the mesh so the soles sit on `position.y`. Tunable at runtime.
    float             rootPitch = 0.0f;
    float             rootRoll = 0.0f;
    float             modelScale = 0.01f; // UE cm -> engine metres (applied on world)
    float             footOffset = 3.0f;  // lift above grass for the debug view
    // Optional model-space correction for asset-specific authoring axes.
    float             meshPitch = 0.0f;
    float             meshRoll = 0.0f;
    float             meshYaw  = 0.0f;

    bool Init(const SkinnedModel& m) {
        model = m;
        if (!model.valid) return false;
        // One palette upload buffer per in-flight frame so we never overwrite a
        // palette the GPU is still reading.
        const UINT bytes = (UINT)(model.skeleton.BoneCount() * sizeof(DirectX::XMFLOAT4X4));
        paletteBytes_ = bytes;
        D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (UINT i = 0; i < FRAME_COUNT; ++i) {
            if (FAILED(g_dx12.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&palette_[i]))))
                return false;
            D3D12_RANGE none{ 0, 0 };
            if (FAILED(palette_[i]->Map(0, &none, &mapped_[i]))) return false;
        }
        // Prime with the bind pose so the very first frame renders upright even
        // before any clip is assigned.
        anim.ComputePalette(model.skeleton, paletteCPU_);
        return true;
    }

    void PlayClip(const std::string& name) {
        if (const AnimationClip* c = model.FindClip(name)) anim.Play(c);
    }

    // Shared world matrix for both the skinned mesh and the skeleton overlay:
    // native (cm) space -> scaled to metres -> oriented (roll/pitch/yaw) ->
    // placed on the ground. Mesh and joints use the SAME matrix so they align.
    DirectX::XMMATRIX WorldMatrix() const {
        using namespace DirectX;
        return XMMatrixScaling(modelScale, modelScale, modelScale) *
               XMMatrixRotationZ(rootRoll) *
               XMMatrixRotationX(rootPitch) *
               XMMatrixRotationY(yaw) *
               XMMatrixTranslation(position.x, position.y + footOffset, position.z);
    }

    void Update(float dt) {
        anim.Advance(dt);
        anim.ComputePalette(model.skeleton, paletteCPU_);
    }

    // Uploads this frame's palette and draws every skinned primitive. Mirrors
    // DrawSceneNode's material setup but routes through g_meshShader.Draw with
    // the palette + skin SRV addresses so the mesh shader skins on the GPU.
    void Draw(ShaderDX12& shader, const DirectX::XMMATRIX& view,
              const DirectX::XMMATRIX& proj, const DirectX::XMMATRIX& lightSpace) {
        using namespace DirectX;
        if (!visible || !model.valid || paletteCPU_.empty()) return;

        const UINT frame = g_dx12.frameIndex % FRAME_COUNT;
        memcpy(mapped_[frame], paletteCPU_.data(), paletteBytes_);
        const D3D12_GPU_VIRTUAL_ADDRESS paletteAddr = palette_[frame]->GetGPUVirtualAddress();

        // Mesh gets an extra independent rotation (debug) pre-applied in its own
        // local space so it can be aligned against the skeleton overlay.
        const XMMATRIX world =
            XMMatrixRotationZ(meshRoll) * XMMatrixRotationX(meshPitch) *
            XMMatrixRotationY(meshYaw) * WorldMatrix();
        shader.SetMatrices(world, view, proj, lightSpace);

        for (const auto& prim : model.node->mesh->primitives) {
            if (prim.vbv.BufferLocation == 0 || !prim.skinBuffer) continue;
            shader.Use(false);
            if (prim.material) {
                XMFLOAT3 color(prim.material->baseColorFactor.x,
                               prim.material->baseColorFactor.y,
                               prim.material->baseColorFactor.z);
                shader.SetObjectMaterial(color,
                    prim.material->baseColorTexture != nullptr,
                    prim.material->normalTexture != nullptr,
                    prim.material->metallicFactor, prim.material->roughnessFactor,
                    prim.material->baseColorTexture.Get(),
                    prim.material->normalTexture.Get(),
                    prim.material->metallicRoughnessTexture.Get(),
                    prim.material->roughnessOnlyTexture, 1.0f, false,
                    prim.material.get());
            } else {
                shader.SetObjectColor(XMFLOAT3(0.7f, 0.7f, 0.72f));
            }

            const D3D12_GPU_VIRTUAL_ADDRESS descA = prim.meshletDescBuffer ? prim.meshletDescBuffer->GetGPUVirtualAddress() : 0;
            const D3D12_GPU_VIRTUAL_ADDRESS boundsA = prim.meshletBoundsBuffer ? prim.meshletBoundsBuffer->GetGPUVirtualAddress() : 0;
            const D3D12_GPU_VIRTUAL_ADDRESS vidxA = prim.meshletVertexIndexBuffer ? prim.meshletVertexIndexBuffer->GetGPUVirtualAddress() : 0;
            const D3D12_GPU_VIRTUAL_ADDRESS triA = prim.meshletTriangleBuffer ? prim.meshletTriangleBuffer->GetGPUVirtualAddress() : 0;
            if (g_meshShader.CanDraw(prim.meshletCount, descA, boundsA, vidxA, triA)) {
                g_meshShader.Draw(prim.vbv, (UINT)(prim.vertices.size() / 12), prim.indexCount,
                    prim.meshletCount, descA, boundsA, vidxA, triA,
                    paletteAddr, prim.skinBuffer->GetGPUVirtualAddress());
            }
            shader.NextDrawCall();
        }
    }

    const std::vector<DirectX::XMFLOAT4X4>& Palette() const { return paletteCPU_; }

    // Debug: draw the skeleton as a small sphere per joint (no mesh, no
    // skinning), so orientation and animation are readable directly. `drawOne`
    // is a callback that binds a unit-sphere draw given a world matrix + colour.
    template <typename DrawOne>
    void DrawSkeleton(ShaderDX12& shader, const DirectX::XMMATRIX& view,
                      const DirectX::XMMATRIX& proj, const DirectX::XMMATRIX& lightSpace,
                      DrawOne&& drawOne) {
        using namespace DirectX;
        if (!model.valid) return;
        anim.ComputeGlobals(model.skeleton, joints_);
        const XMMATRIX world = WorldMatrix();
        for (size_t b = 0; b < joints_.size(); ++b) {
            // Joint spheres share the model's world matrix so they overlay the
            // skinned mesh exactly. Sphere size is in native (cm) space, undone
            // by the model scale, so it stays a constant on-screen size.
            const XMMATRIX m = XMMatrixScaling(3.0f, 3.0f, 3.0f) *
                XMMatrixTranslation(joints_[b].x, joints_[b].y, joints_[b].z) * world;
            shader.SetMatrices(m, view, proj, lightSpace);
            // Colour-code by height so up/down is obvious: root red, higher greener.
            const float t = std::min(1.0f, std::max(0.0f, joints_[b].y * 0.5f + 0.5f));
            shader.SetObjectColor(XMFLOAT3(1.0f - t, t, 0.15f));
            drawOne();
            shader.NextDrawCall();
        }
    }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> palette_[FRAME_COUNT];
    void* mapped_[FRAME_COUNT] = {};
    UINT  paletteBytes_ = 0;
    std::vector<DirectX::XMFLOAT4X4> paletteCPU_;
    std::vector<DirectX::XMFLOAT3>   joints_;
};
