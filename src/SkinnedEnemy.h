#pragma once
// A skinned character instance: owns the shared SkinnedModel, an animation
// player, and a per-frame bone-palette upload buffer, and knows how to draw
// itself through the mesh-shader path with skinning enabled.
#include "SkinnedFBXImporter.h"
#include "AnimationRuntime.h"
#include "MeshShaderDX12.h"
#include "DX12Core.h"
#include "DestructionDX12.h"
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
    float             health = 100.0f;
    float             moveSpeed = 1.8f;
    // Asset-space orientation and ground offset.
    float             rootPitch = 0.0f;
    float             rootRoll = 0.0f;
    float             modelScale = 0.01f; // UE cm -> engine metres (applied on world)
    float             footOffset = 0.0f;
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
        if (const AnimationClip* c = model.FindClip(name); c && anim.clip != c) anim.Play(c);
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

    DirectX::XMMATRIX MeshWorldMatrix() const {
        using namespace DirectX;
        return XMMatrixRotationZ(meshRoll) * XMMatrixRotationX(meshPitch) *
               XMMatrixRotationY(meshYaw) * WorldMatrix();
    }

    bool CanRender() const { return visible && !dead_ && model.valid && !paletteCPU_.empty(); }

    D3D12_GPU_VIRTUAL_ADDRESS UploadPalette() {
        if (!CanRender()) return 0;
        const UINT frame = g_dx12.frameIndex % FRAME_COUNT;
        memcpy(mapped_[frame], paletteCPU_.data(), paletteBytes_);
        return palette_[frame]->GetGPUVirtualAddress();
    }

    void Update(float dt, const DirectX::XMFLOAT3& target, float groundY) {
        if (dead_) return;
        position.y = groundY;
        const float dx = target.x - position.x, dz = target.z - position.z;
        const float distance = std::sqrt(dx*dx + dz*dz);
        float speed = 0.0f;
        if (distance > 4.0f) {
            speed = distance > 11.0f ? moveSpeed * 1.65f : moveSpeed;
            const float inv = 1.0f / distance;
            position.x += dx * inv * speed * dt;
            position.z += dz * inv * speed * dt;
            yaw = std::atan2(dx, dz);
        }
        PlayClip(speed > moveSpeed * 1.2f ? "Run" : speed > 0.01f ? "Walk" : "Idle");
        anim.Advance(dt);
        anim.ComputePalette(model.skeleton, paletteCPU_);
    }

    bool Shoot(const DirectX::XMFLOAT3& start, const DirectX::XMFLOAT3& end,
               const DirectX::XMFLOAT3& direction, float radius) {
        using namespace DirectX;
        if (dead_ || !visible) return false;
        const XMVECTOR a = XMLoadFloat3(&start), b = XMLoadFloat3(&end);
        const XMVECTOR center = XMVectorSet(position.x, position.y + footOffset + 1.0f,
                                            position.z, 0.0f);
        const XMVECTOR ab = b - a;
        const float lengthSq = XMVectorGetX(XMVector3LengthSq(ab));
        float t = lengthSq > 1e-6f
            ? XMVectorGetX(XMVector3Dot(center - a, ab)) / lengthSq : 0.0f;
        t = (std::max)(0.0f, (std::min)(1.0f, t));
        const float hitRadius = 0.72f + radius;
        if (XMVectorGetX(XMVector3LengthSq(a + ab*t - center)) > hitRadius*hitRadius) return false;
        health -= 34.0f;
        if (health <= 0.0f) Kill(direction);
        return true;
    }

    bool Dead() const { return dead_; }

    // Uploads this frame's palette and draws every skinned primitive. Mirrors
    // DrawSceneNode's material setup but routes through g_meshShader.Draw with
    // the palette + skin SRV addresses so the mesh shader skins on the GPU.
    void Draw(ShaderDX12& shader, const DirectX::XMMATRIX& view,
              const DirectX::XMMATRIX& proj, const DirectX::XMMATRIX& lightSpace) {
        using namespace DirectX;
        if (!CanRender()) return;
        const D3D12_GPU_VIRTUAL_ADDRESS paletteAddr = UploadPalette();

        // Mesh gets an extra independent rotation (debug) pre-applied in its own
        // local space so it can be aligned against the skeleton overlay.
        const XMMATRIX world = MeshWorldMatrix();
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

private:
    void Kill(const DirectX::XMFLOAT3& impulseDirection) {
        using namespace DirectX;
        dead_ = true;
        std::vector<XMFLOAT4X4> globals;
        anim.ComputeGlobalMatrices(model.skeleton, globals);
        std::vector<AuthoredRagdollBody> bodies;
        bodies.reserve(model.ragdoll.bodies.size());
        for (const RagdollBodySpec& spec : model.ragdoll.bodies) {
            const int bone = model.skeleton.Find(spec.bone);
            if (bone < 0 || (size_t)bone >= globals.size()) continue;
            const XMMATRIX local = XMMatrixRotationQuaternion(XMLoadFloat4(&spec.rotation)) *
                                   XMMatrixTranslation(spec.center.x, spec.center.y, spec.center.z);
            const XMMATRIX bodyWorld = local * XMLoadFloat4x4(&globals[bone]) * WorldMatrix();
            XMVECTOR scale, rotation, translation;
            if (!XMMatrixDecompose(&scale, &rotation, &translation, bodyWorld)) continue;
            AuthoredRagdollBody body;
            body.name = spec.bone; body.halfExtent = spec.halfExtent;
            body.radius = spec.radius; body.length = spec.length; body.shape = spec.shape;
            XMStoreFloat3(&body.position, translation);
            XMStoreFloat4(&body.rotation, XMQuaternionNormalize(rotation));
            bodies.push_back(body);
        }
        g_destruction.SpawnAuthoredRagdoll(bodies, model.ragdoll.constraints, impulseDirection);
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> palette_[FRAME_COUNT];
    void* mapped_[FRAME_COUNT] = {};
    UINT  paletteBytes_ = 0;
    std::vector<DirectX::XMFLOAT4X4> paletteCPU_;
    bool dead_ = false;
};
