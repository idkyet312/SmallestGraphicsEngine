#pragma once

#include <DirectXMath.h>
#include <d3d12.h>
#include <memory>
#include <vector>
#include "NvBlastTkEvent.h"
#include "SceneGraph.h"

struct DestructionRenderItem {
    std::shared_ptr<SceneNode> node;
    DirectX::XMFLOAT4X4 transform;
};

class DestructionDX12 final : public Nv::Blast::TkEventListener {
public:
    DestructionDX12();
    ~DestructionDX12();

    bool Initialize(const std::shared_ptr<SceneNode>& mergedModel,
                    ID3D12Device* device, int gridX = 4, int gridY = 3, int gridZ = 4);
    void Shutdown();
    void Reset();
    void Update(float dt);
    bool HitTest(const DirectX::XMFLOAT3& worldPosition, float radius,
                 DirectX::XMFLOAT3& hitPosition) const;
    bool HitTestSegment(const DirectX::XMFLOAT3& worldStart,
                        const DirectX::XMFLOAT3& worldEnd, float radius,
                        DirectX::XMFLOAT3& hitPosition) const;
    void ApplyRadialDamage(const DirectX::XMFLOAT3& worldPosition,
                           float radius, float damage = 2.0f);
    bool ApplyImpulse(const DirectX::XMFLOAT3& worldPosition,
                      const DirectX::XMFLOAT3& worldDirection,
                      float impulseStrength, float hitRadius = 0.5f);

    bool IsInitialized() const;
    uint32_t GetChunkCount() const;
    uint32_t GetActorCount() const;
    const std::vector<DestructionRenderItem>& GetRenderItems() const;

    void receive(const Nv::Blast::TkEvent* events, uint32_t eventCount) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

extern DestructionDX12 g_destruction;
