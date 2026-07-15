#pragma once
// CPU animation sampling: given a clip and a time, produce the per-bone skinning
// palette (bone-space -> world-of-skeleton) uploaded to the GPU each frame.
// Row-vector convention throughout (v' = v * M, so child = local * parent),
// matching SkinnedFBXImporter's ToXM and the engine's XMMatrixMultiply usage.
#include "SkinnedTypes.h"
#include <DirectXMath.h>
#include <algorithm>
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
    void ComputePalette(const Skeleton& skel, std::vector<DirectX::XMFLOAT4X4>& palette) const {
        using namespace DirectX;
        const size_t n = skel.BoneCount();
        EnsureCache(skel);
        palette.resize(n);

        for (size_t b = 0; b < n; ++b) {
            XMMATRIX local = XMLoadFloat4x4(&skel.localBind[b]);
            const BoneTrack* tr = trackByBone_[b];
            if (tr) local = SampleTrack(*tr, b);
            const int parent = skel.parent[b];
            globalScratch_[b] = (parent < 0) ? local : XMMatrixMultiply(local, globalScratch_[parent]);
        }
        const XMMATRIX globalInverse = XMLoadFloat4x4(&skel.globalInverse);
        for (size_t b = 0; b < n; ++b) {
            // Transpose of Assimp's column-vector formula:
            // inverseRoot * globalBone * inverseBind.
            const XMMATRIX skinMat = XMLoadFloat4x4(&skel.offset[b]) * globalScratch_[b] * globalInverse;
            XMStoreFloat4x4(&palette[b], XMMatrixTranspose(skinMat));
        }
    }

    // Debug: each bone's global (model-space) transform, for drawing the
    // skeleton as joints/bones without any mesh. Same forward pass as the
    // palette but WITHOUT the inverse-bind offset, so translation is the joint
    // position in the character's local space.
    void ComputeGlobals(const Skeleton& skel, std::vector<DirectX::XMFLOAT3>& jointPos) const {
        using namespace DirectX;
        std::vector<XMFLOAT4X4> matrices;
        ComputeGlobalMatrices(skel, matrices);
        const size_t n = matrices.size();
        jointPos.resize(n);
        for (size_t b = 0; b < n; ++b)
            XMStoreFloat3(&jointPos[b], XMLoadFloat4x4(&matrices[b]).r[3]);
    }

    void ComputeGlobalMatrices(const Skeleton& skel,
                               std::vector<DirectX::XMFLOAT4X4>& matrices) const {
        using namespace DirectX;
        const size_t n = skel.BoneCount();
        EnsureCache(skel);
        matrices.resize(n);
        for (size_t b = 0; b < n; ++b) {
            XMMATRIX local = XMLoadFloat4x4(&skel.localBind[b]);
            const BoneTrack* tr = trackByBone_[b];
            if (tr) local = SampleTrack(*tr, b);
            const int parent = skel.parent[b];
            globalScratch_[b] = (parent < 0) ? local : XMMatrixMultiply(local, globalScratch_[parent]);
            XMMATRIX modelSpace = globalScratch_[b] * XMLoadFloat4x4(&skel.globalInverse);
            XMStoreFloat4x4(&matrices[b], modelSpace);
        }
    }

private:
    void EnsureCache(const Skeleton& skel) const {
        using namespace DirectX;
        const size_t n = skel.BoneCount();
        if (cachedSkeleton_ != &skel || bindScale_.size() != n) {
            bindScale_.resize(n);
            bindRotation_.resize(n);
            bindTranslation_.resize(n);
            globalScratch_.resize(n);
            for (size_t b = 0; b < n; ++b) {
                XMVECTOR scale, rotation, translation;
                if (!XMMatrixDecompose(&scale, &rotation, &translation,
                                       XMLoadFloat4x4(&skel.localBind[b]))) {
                    scale = XMVectorSplatOne();
                    rotation = XMQuaternionIdentity();
                    translation = XMVectorZero();
                }
                XMStoreFloat3(&bindScale_[b], scale);
                XMStoreFloat4(&bindRotation_[b], rotation);
                XMStoreFloat3(&bindTranslation_[b], translation);
            }
            cachedSkeleton_ = &skel;
            cachedClip_ = nullptr;
        }

        if (cachedClip_ != clip || trackByBone_.size() != n) {
            trackByBone_.assign(n, nullptr);
            if (clip) {
                for (const BoneTrack& track : clip->tracks) {
                    if (track.bone >= 0 && static_cast<size_t>(track.bone) < n)
                        trackByBone_[track.bone] = &track;
                }
            }
            cachedClip_ = clip;
        }
    }

    DirectX::XMMATRIX SampleTrack(const BoneTrack& tr, size_t bone) const {
        using namespace DirectX;
        const XMVECTOR t = SampleVec(tr.positions, XMLoadFloat3(&bindTranslation_[bone]));
        const XMVECTOR r = SampleQuat(tr.rotations, XMLoadFloat4(&bindRotation_[bone]));
        const XMVECTOR s = SampleVec(tr.scales, XMLoadFloat3(&bindScale_[bone]));
        return XMMatrixAffineTransformation(s, XMVectorZero(), r, t);
    }

    template <typename Keys>
    DirectX::XMVECTOR SampleVec(const Keys& keys, DirectX::XMVECTOR fallback) const {
        using namespace DirectX;
        if (keys.empty()) return fallback;
        if (keys.size() == 1) return XMLoadFloat3(&keys[0].value);
        if (time <= keys.front().time) return XMLoadFloat3(&keys.front().value);
        const auto upper = std::lower_bound(keys.begin() + 1, keys.end(), time,
            [](const auto& key, float sampleTime) { return key.time < sampleTime; });
        if (upper == keys.end()) return XMLoadFloat3(&keys.back().value);
        const auto& lower = *(upper - 1);
        const float span = upper->time - lower.time;
        const float a = span > 1e-6f ? (time - lower.time) / span : 0.0f;
        return XMVectorLerp(XMLoadFloat3(&lower.value), XMLoadFloat3(&upper->value), a);
    }

    DirectX::XMVECTOR SampleQuat(const std::vector<BoneTrack::QuatKey>& keys,
                                 DirectX::XMVECTOR fallback) const {
        using namespace DirectX;
        if (keys.empty()) return fallback;
        if (keys.size() == 1) return XMLoadFloat4(&keys[0].value);
        if (time <= keys.front().time) return XMLoadFloat4(&keys.front().value);
        const auto upper = std::lower_bound(keys.begin() + 1, keys.end(), time,
            [](const BoneTrack::QuatKey& key, float sampleTime) { return key.time < sampleTime; });
        if (upper == keys.end()) return XMLoadFloat4(&keys.back().value);
        const auto& lower = *(upper - 1);
        const float span = upper->time - lower.time;
        const float a = span > 1e-6f ? (time - lower.time) / span : 0.0f;
        return XMQuaternionSlerp(XMLoadFloat4(&lower.value), XMLoadFloat4(&upper->value), a);
    }

    mutable const Skeleton* cachedSkeleton_ = nullptr;
    mutable const AnimationClip* cachedClip_ = nullptr;
    mutable std::vector<const BoneTrack*> trackByBone_;
    mutable std::vector<DirectX::XMFLOAT3> bindScale_;
    mutable std::vector<DirectX::XMFLOAT4> bindRotation_;
    mutable std::vector<DirectX::XMFLOAT3> bindTranslation_;
    mutable std::vector<DirectX::XMMATRIX> globalScratch_;
};
