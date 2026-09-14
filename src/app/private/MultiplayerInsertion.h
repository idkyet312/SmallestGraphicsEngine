#pragma once

// Remote insertions share resident geometry, but not the local airframe's
// palette: players can choose different rigs on the same deployment screen.
struct RemoteInsertionRig {
    static constexpr size_t kPaletteCapacity = 256;
    ComPtr<ID3D12Resource> buffers[FRAME_COUNT];
    void* mapped[FRAME_COUNT] = {};
    std::vector<XMFLOAT4X4> palette;
    std::shared_ptr<SceneNode> rotorNode;
    int rotorBone = -1;
};
static RemoteInsertionRig g_remoteInsertionRigs[2];
static std::vector<RemoteInsertionHelicopterDraw> g_remoteInsertionDraws;
static std::vector<net::RemotePlayer> g_remoteInsertionPlayers;

static void InitializeRemoteInsertionVisuals() {
    for (size_t airframe = 0; airframe < 2; ++airframe) {
        auto& rig = g_remoteInsertionRigs[airframe];
        const auto& model = g_blackHawkAirframeModel[airframe];
        const auto& skeleton = g_blackHawkAirframeSkeleton[airframe];
        rig.palette.clear();
        rig.rotorNode.reset();
        rig.rotorBone = -1;
        if (!model) continue;
        if (!ModelHasSkinnedPrimitive(model)) {
            rig.rotorNode = FindNodeByName(model, "Bone");
            continue;
        }
        if (skeleton.BoneCount() == 0 ||
            skeleton.BoneCount() > RemoteInsertionRig::kPaletteCapacity) continue;
        rig.palette.resize(skeleton.BoneCount());
        rig.rotorBone = skeleton.Find("Bone");
        // Fixed capacity keeps model reloads from replacing a live buffer.
        // Allocation happens only in the existing model-loading stage.
        for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
            if (rig.buffers[frame]) continue;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = RemoteInsertionRig::kPaletteCapacity * sizeof(XMFLOAT4X4);
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ThrowIfFailed(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&rig.buffers[frame])));
            D3D12_RANGE none{0, 0};
            ThrowIfFailed(rig.buffers[frame]->Map(0, &none, &rig.mapped[frame]));
        }
    }
}

static void UpdateRemoteInsertionVisuals(float deltaTime) {
    g_remoteInsertionDraws.clear();
    if (!MultiplayerActive() || !IsGameplayScreen() ||
        g_game.loading.Active() || g_emptyLevelMode || g_baseMode) return;
    g_netSession.GetRemotePlayers(g_remoteInsertionPlayers);
    static float rotorAngle = 0.0f;
    rotorAngle = std::fmod(rotorAngle + deltaTime * 34.0f, XM_2PI);
    bool animated[2] = {};
    for (const auto& player : g_remoteInsertionPlayers) {
        const auto& helicopter = player.helicopter;
        if (!helicopter.visible || helicopter.airframe >= 2) continue;
        const size_t airframe = helicopter.airframe;
        const auto& model = g_blackHawkAirframeModel[airframe];
        if (!model) continue;
        auto& rig = g_remoteInsertionRigs[airframe];
        if (!animated[airframe]) {
            animated[airframe] = true;
            const bool usesLocalModel = model == g_blackHawkModel && BlackHawkVisible();
            const float angle = usesLocalModel ? g_blackHawkRotorSpin : rotorAngle;
            // Node animation is shared with the local aircraft. Leave its
            // phase alone; only drive the otherwise-unused alternate model.
            if (rig.rotorNode && !usesLocalModel) {
                XMStoreFloat4(&rig.rotorNode->rotation,
                    XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), angle));
                model->RefreshHierarchy();
            }
            const auto& skeleton = g_blackHawkAirframeSkeleton[airframe];
            for (size_t bone = 0; bone < rig.palette.size(); ++bone) {
                XMMATRIX posed = XMMatrixIdentity();
                if (static_cast<int>(bone) == rig.rotorBone) {
                    const XMMATRIX inverseBind = XMLoadFloat4x4(&skeleton.offset[bone]);
                    XMVECTOR determinant = XMMatrixDeterminant(inverseBind);
                    if (XMVectorGetX(XMVectorAbs(determinant)) >= 1e-12f)
                        posed = inverseBind * XMMatrixRotationY(angle) *
                            XMMatrixInverse(&determinant, inverseBind);
                }
                XMStoreFloat4x4(&rig.palette[bone], XMMatrixTranspose(posed));
            }
        }
        RemoteInsertionHelicopterDraw draw;
        draw.model = model;
        draw.airframe = helicopter.airframe;
        XMStoreFloat4x4(&draw.world,
            XMMatrixTranslation(-helicopter.centerX, -helicopter.minY,
                                -helicopter.centerZ) *
            XMMatrixScaling(helicopter.scale, helicopter.scale, helicopter.scale) *
            XMMatrixRotationRollPitchYaw(XMConvertToRadians(helicopter.pitch),
                XMConvertToRadians(helicopter.yaw), XMConvertToRadians(helicopter.roll)) *
            XMMatrixTranslation(helicopter.x, helicopter.y, helicopter.z));
        g_remoteInsertionDraws.push_back(std::move(draw));
    }
}

const std::vector<RemoteInsertionHelicopterDraw>& RemoteInsertionHelicopters() {
    return g_remoteInsertionDraws;
}

D3D12_GPU_VIRTUAL_ADDRESS UploadRemoteInsertionPalette(uint8_t airframe) {
    if (airframe >= 2) return 0;
    auto& rig = g_remoteInsertionRigs[airframe];
    const UINT frame = g_dx12.frameIndex % FRAME_COUNT;
    if (rig.palette.empty() || !rig.mapped[frame]) return 0;
    // Called only by drawing, after BeginFrame has acquired this frame slot.
    memcpy(rig.mapped[frame], rig.palette.data(), rig.palette.size() * sizeof(XMFLOAT4X4));
    return rig.buffers[frame]->GetGPUVirtualAddress();
}
