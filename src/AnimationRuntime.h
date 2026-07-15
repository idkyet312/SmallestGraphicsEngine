#pragma once
// CPU animation sampling: given a clip and a time, produce the per-bone skinning
// palette (bone-space -> world-of-skeleton) uploaded to the GPU each frame.
// Row-vector convention throughout (v' = v * M, so child = local * parent),
// matching SkinnedFBXImporter's ToXM and the engine's XMMatrixMultiply usage.
#include "SkinnedTypes.h"
#include <DirectXMath.h>
#include <vector>

class AnimationInstance {
public:
    const AnimationClip* clip = nullptr;
    float time = 0.0f;
    bool  loop = true;

    void Play(const AnimationClip* c) { clip = c; time = 0.0f; }
    void Advance(float dt) {
        if (!clip || clip->duration <= 0.0f) return;
        time += dt;
        if (loop) time = std::fmod(time, clip->duration);
        else if (time > clip->duration) time = clip->duration;
    }

    // Fills `palette` (size = skeleton.BoneCount()) with skinning matrices.
    // HLSL matrices use column-major buffer packing, so transpose before upload,
    // exactly like ShaderDX12::SetMatrices does for its constant-buffer matrices.
    // clip is set, produces the bind pose (identity-driven local transforms).
    // TEMP debug: when true, palette is all-identity so the mesh renders in its
    // raw bind-pose vertex positions (isolates skinning-matrix bugs from
    // position/scale/culling bugs).
    static inline bool s_forceIdentity = false;

    void ComputePalette(const Skeleton& skel, std::vector<DirectX::XMFLOAT4X4>& palette) const {
        using namespace DirectX;
        const size_t n = skel.BoneCount();
        palette.resize(n);
        if (s_forceIdentity) {
            for (size_t b = 0; b < n; ++b) XMStoreFloat4x4(&palette[b], XMMatrixIdentity());
            return;
        }
        std::vector<XMMATRIX> global(n);

        for (size_t b = 0; b < n; ++b) {
            XMMATRIX local = XMLoadFloat4x4(&skel.localBind[b]);
            if (clip) {
                const BoneTrack* tr = FindTrack((int)b);
                if (tr) local = SampleTrack(*tr, local);
            }
            const int parent = skel.parent[b];
            global[b] = (parent < 0) ? local : XMMatrixMultiply(local, global[parent]);
        }
        const XMMATRIX globalInverse = XMLoadFloat4x4(&skel.globalInverse);
        for (size_t b = 0; b < n; ++b) {
            // Transpose of Assimp's column-vector formula:
            // inverseRoot * globalBone * inverseBind.
            const XMMATRIX skinMat = XMLoadFloat4x4(&skel.offset[b]) * global[b] * globalInverse;
            XMStoreFloat4x4(&palette[b], XMMatrixTranspose(skinMat));
        }
    }

    // Debug: each bone's global (model-space) transform, for drawing the
    // skeleton as joints/bones without any mesh. Same forward pass as the
    // palette but WITHOUT the inverse-bind offset, so translation is the joint
    // position in the character's local space.
    void ComputeGlobals(const Skeleton& skel, std::vector<DirectX::XMFLOAT3>& jointPos) const {
        using namespace DirectX;
        const size_t n = skel.BoneCount();
        jointPos.resize(n);
        std::vector<XMMATRIX> global(n);
        for (size_t b = 0; b < n; ++b) {
            XMMATRIX local = XMLoadFloat4x4(&skel.localBind[b]);
            if (clip) { const BoneTrack* tr = FindTrack((int)b); if (tr) local = SampleTrack(*tr, local); }
            const int parent = skel.parent[b];
            global[b] = (parent < 0) ? local : XMMatrixMultiply(local, global[parent]);
            XMMATRIX modelSpace = global[b] * XMLoadFloat4x4(&skel.globalInverse);
            XMStoreFloat3(&jointPos[b], modelSpace.r[3]);
        }
    }

private:
    const BoneTrack* FindTrack(int bone) const {
        for (const auto& t : clip->tracks) if (t.bone == bone) return &t;
        return nullptr;
    }

    DirectX::XMMATRIX SampleTrack(const BoneTrack& tr, DirectX::FXMMATRIX bindLocal) const {
        using namespace DirectX;
        XMVECTOR bindScale, bindRotation, bindTranslation;
        if (!XMMatrixDecompose(&bindScale, &bindRotation, &bindTranslation, bindLocal)) {
            bindScale = XMVectorSplatOne();
            bindRotation = XMQuaternionIdentity();
            bindTranslation = XMVectorZero();
        }
        const XMVECTOR t = SampleVec(tr.positions, bindTranslation);
        const XMVECTOR r = SampleQuat(tr.rotations, bindRotation);
        const XMVECTOR s = SampleVec(tr.scales, bindScale);
        return XMMatrixAffineTransformation(s, XMVectorZero(), r, t);
    }

    template <typename Keys>
    DirectX::XMVECTOR SampleVec(const Keys& keys, DirectX::XMVECTOR fallback) const {
        using namespace DirectX;
        if (keys.empty()) return fallback;
        if (keys.size() == 1) return XMLoadFloat3(&keys[0].value);
        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            if (time <= keys[i + 1].time) {
                const float span = keys[i + 1].time - keys[i].time;
                const float a = span > 1e-6f ? (time - keys[i].time) / span : 0.0f;
                return XMVectorLerp(XMLoadFloat3(&keys[i].value), XMLoadFloat3(&keys[i + 1].value), a);
            }
        }
        return XMLoadFloat3(&keys.back().value);
    }

    DirectX::XMVECTOR SampleQuat(const std::vector<BoneTrack::QuatKey>& keys,
                                 DirectX::XMVECTOR fallback) const {
        using namespace DirectX;
        if (keys.empty()) return fallback;
        if (keys.size() == 1) return XMLoadFloat4(&keys[0].value);
        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            if (time <= keys[i + 1].time) {
                const float span = keys[i + 1].time - keys[i].time;
                const float a = span > 1e-6f ? (time - keys[i].time) / span : 0.0f;
                return XMQuaternionSlerp(XMLoadFloat4(&keys[i].value), XMLoadFloat4(&keys[i + 1].value), a);
            }
        }
        return XMLoadFloat4(&keys.back().value);
    }
};
