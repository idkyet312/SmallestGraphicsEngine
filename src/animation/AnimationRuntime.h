#pragma once
// CPU animation sampling: given a clip and a time, produce the per-bone skinning
// palette (bone-space -> world-of-skeleton) uploaded to the GPU each frame.
// Row-vector convention throughout (v' = v * M, so child = local * parent),
// matching SkinnedFBXImporter's ToXM and the engine's XMMatrixMultiply usage.
#include "SkinnedTypes.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <vector>

class AnimationInstance {
public:
    const AnimationClip* clip = nullptr;
    float time = 0.0f;
    bool  loop = true;
    // Seconds blended on each side of the loop seam. Zero preserves exact clip
    // sampling. Non-zero crossfades the outgoing and incoming cycle so clips
    // whose last pose differs from their first do not snap at fmod().
    float loopBlendDuration = 0.0f;

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

    // Layers only the motion in `additive` over this base animation. The
    // additive clip's reference frame is subtracted per bone, so different
    // authored origins and rest poses never replace or displace the base pose.
    // Weight zero is exactly the base clip; weight one is base plus the full
    // additive motion.
    void ComputeAdditivePalette(
        const Skeleton& skel, const AnimationInstance& additive,
        float additiveReferenceTime, float weight,
        std::vector<DirectX::XMFLOAT4X4>& palette,
        std::vector<DirectX::XMFLOAT4X4>* modelMatrices = nullptr,
        float baseMotionWeight = 1.0f,
        float baseReferenceTime = 0.0f) const {
        using namespace DirectX;
        const size_t n = skel.BoneCount();
        EnsureCache(skel);
        additive.EnsureCache(skel);
        palette.resize(n);
        if (modelMatrices) modelMatrices->resize(n);
        weight = (std::max)(0.0f, (std::min)(1.0f, weight));
        baseMotionWeight =
            (std::max)(0.0f, (std::min)(1.0f, baseMotionWeight));

        for (size_t b = 0; b < n; ++b) {
            XMMATRIX baseLocal = XMLoadFloat4x4(&skel.localBind[b]);
            if (const BoneTrack* tr = trackByBone_[b]) {
                baseLocal = SampleTrack(*tr, b);
                if (baseMotionWeight < 0.9999f) {
                    const XMMATRIX reference =
                        SampleTrackAt(*tr, b, baseReferenceTime);
                    baseLocal = BlendLocalTransforms(
                        reference, baseLocal, baseMotionWeight);
                }
            }

            XMMATRIX additiveLocal = XMLoadFloat4x4(&skel.localBind[b]);
            XMMATRIX referenceLocal = additiveLocal;
            if (const BoneTrack* tr = additive.trackByBone_[b]) {
                additiveLocal = additive.SampleTrack(*tr, b);
                referenceLocal =
                    additive.SampleTrackAt(*tr, b, additiveReferenceTime);
            }

            XMVECTOR baseScale, baseRotation, baseTranslation;
            XMVECTOR addScale, addRotation, addTranslation;
            XMVECTOR refScale, refRotation, refTranslation;
            if (!XMMatrixDecompose(&baseScale, &baseRotation, &baseTranslation,
                                   baseLocal)) {
                baseScale = XMVectorSplatOne();
                baseRotation = XMQuaternionIdentity();
                baseTranslation = XMVectorZero();
            }
            if (!XMMatrixDecompose(&addScale, &addRotation, &addTranslation,
                                   additiveLocal) ||
                !XMMatrixDecompose(&refScale, &refRotation, &refTranslation,
                                   referenceLocal)) {
                addScale = refScale = XMVectorSplatOne();
                addRotation = refRotation = XMQuaternionIdentity();
                addTranslation = refTranslation = XMVectorZero();
            }

            // Translation and scale use value deltas. Rotation uses the matrix
            // delta reference^-1 * current, then blends that delta from identity
            // before composing it after the base rotation.
            const XMVECTOR translation = XMVectorAdd(
                baseTranslation,
                XMVectorScale(XMVectorSubtract(addTranslation, refTranslation),
                              weight));
            const XMVECTOR scale = XMVectorAdd(
                baseScale,
                XMVectorScale(XMVectorSubtract(addScale, refScale), weight));

            const XMMATRIX deltaRotationMatrix =
                XMMatrixTranspose(XMMatrixRotationQuaternion(refRotation)) *
                XMMatrixRotationQuaternion(addRotation);
            XMVECTOR ignoredScale, deltaRotation, ignoredTranslation;
            if (!XMMatrixDecompose(&ignoredScale, &deltaRotation,
                                   &ignoredTranslation, deltaRotationMatrix))
                deltaRotation = XMQuaternionIdentity();
            const XMVECTOR weightedDelta = XMQuaternionSlerp(
                XMQuaternionIdentity(), XMQuaternionNormalize(deltaRotation),
                weight);
            const XMMATRIX combinedRotationMatrix =
                XMMatrixRotationQuaternion(baseRotation) *
                XMMatrixRotationQuaternion(weightedDelta);
            XMVECTOR rotation;
            if (!XMMatrixDecompose(&ignoredScale, &rotation,
                                   &ignoredTranslation,
                                   combinedRotationMatrix))
                rotation = baseRotation;

            const XMMATRIX local = XMMatrixAffineTransformation(
                scale, XMVectorZero(), XMQuaternionNormalize(rotation),
                translation);
            const int parent = skel.parent[b];
            globalScratch_[b] = parent < 0
                ? local : XMMatrixMultiply(local, globalScratch_[parent]);
        }

        const XMMATRIX globalInverse = XMLoadFloat4x4(&skel.globalInverse);
        for (size_t b = 0; b < n; ++b) {
            const XMMATRIX modelSpace = globalScratch_[b] * globalInverse;
            if (modelMatrices)
                XMStoreFloat4x4(&(*modelMatrices)[b], modelSpace);
            const XMMATRIX skinMat =
                XMLoadFloat4x4(&skel.offset[b]) * modelSpace;
            XMStoreFloat4x4(&palette[b], XMMatrixTranspose(skinMat));
        }
    }

    // Unreal-style layered blend per bone. `this` supplies locomotion, overlay
    // supplies the upper-body clip, and mask[b] selects the overlay per bone.
    // Optional local rotation offsets turn a neutral overlay into an authored
    // pose such as a rifle hold without changing lower-body animation.
    void ComputeLayeredPalette(
        const Skeleton& skel, const AnimationInstance& overlay,
        const std::vector<float>& mask,
        const std::vector<DirectX::XMFLOAT4>& rotationOffsets,
        std::vector<DirectX::XMFLOAT4X4>& palette,
        std::vector<DirectX::XMFLOAT4X4>* modelMatrices = nullptr) const {
        using namespace DirectX;
        const size_t n = skel.BoneCount();
        EnsureCache(skel);
        overlay.EnsureCache(skel);
        palette.resize(n);
        if (modelMatrices) modelMatrices->resize(n);

        for (size_t b = 0; b < n; ++b) {
            XMMATRIX baseLocal = XMLoadFloat4x4(&skel.localBind[b]);
            if (const BoneTrack* tr = trackByBone_[b]) baseLocal = SampleTrack(*tr, b);
            XMMATRIX overlayLocal = XMLoadFloat4x4(&skel.localBind[b]);
            if (const BoneTrack* tr = overlay.trackByBone_[b])
                overlayLocal = overlay.SampleTrack(*tr, b);

            const float weight = b < mask.size()
                ? (std::max)(0.0f, (std::min)(1.0f, mask[b])) : 0.0f;
            XMVECTOR bs, br, bt, os, orot, ot;
            if (!XMMatrixDecompose(&bs, &br, &bt, baseLocal)) {
                bs = XMVectorSplatOne(); br = XMQuaternionIdentity(); bt = XMVectorZero();
            }
            if (!XMMatrixDecompose(&os, &orot, &ot, overlayLocal)) {
                os = bs; orot = br; ot = bt;
            }
            XMVECTOR rotation = XMQuaternionSlerp(br, orot, weight);
            if (b < rotationOffsets.size()) {
                const XMVECTOR poseOffset = XMQuaternionSlerp(
                    XMQuaternionIdentity(), XMLoadFloat4(&rotationOffsets[b]), weight);
                rotation = XMQuaternionNormalize(XMQuaternionMultiply(rotation, poseOffset));
            }
            const XMMATRIX local = XMMatrixAffineTransformation(
                XMVectorLerp(bs, os, weight), XMVectorZero(), rotation,
                XMVectorLerp(bt, ot, weight));
            const int parent = skel.parent[b];
            globalScratch_[b] = parent < 0
                ? local : XMMatrixMultiply(local, globalScratch_[parent]);
        }

        const XMMATRIX globalInverse = XMLoadFloat4x4(&skel.globalInverse);
        for (size_t b = 0; b < n; ++b) {
            const XMMATRIX modelSpace = globalScratch_[b] * globalInverse;
            if (modelMatrices) XMStoreFloat4x4(&(*modelMatrices)[b], modelSpace);
            const XMMATRIX skinMat = XMLoadFloat4x4(&skel.offset[b]) * modelSpace;
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
        if (loop && clip && clip->duration > 1e-5f &&
            loopBlendDuration > 1e-5f) {
            const float blend = (std::min)(
                loopBlendDuration, clip->duration * 0.25f);
            const bool beforeSeam = time >= clip->duration - blend;
            const bool afterSeam = time < blend;
            if (beforeSeam || afterSeam) {
                // Treat [duration-blend, duration] and [0, blend] as one
                // continuous 2*blend interval. Both source cycles advance at
                // half speed through it; the blend is therefore identical on
                // both sides of fmod's wrap.
                const float elapsed = beforeSeam
                    ? time - (clip->duration - blend)
                    : blend + time;
                float alpha = elapsed / (2.0f * blend);
                alpha = alpha * alpha * (3.0f - 2.0f * alpha);
                const float outgoingTime =
                    clip->duration - blend + elapsed * 0.5f;
                const float incomingTime = elapsed * 0.5f;
                return BlendLocalTransforms(
                    SampleTrackAt(tr, bone, outgoingTime),
                    SampleTrackAt(tr, bone, incomingTime), alpha);
            }
        }
        return SampleTrackAt(tr, bone, time);
    }

    static DirectX::XMMATRIX BlendLocalTransforms(
        const DirectX::XMMATRIX& from, const DirectX::XMMATRIX& to,
        float weight) {
        using namespace DirectX;
        XMVECTOR fromScale, fromRotation, fromTranslation;
        XMVECTOR toScale, toRotation, toTranslation;
        if (!XMMatrixDecompose(
                &fromScale, &fromRotation, &fromTranslation, from))
            return to;
        if (!XMMatrixDecompose(&toScale, &toRotation, &toTranslation, to))
            return from;
        return XMMatrixAffineTransformation(
            XMVectorLerp(fromScale, toScale, weight), XMVectorZero(),
            XMQuaternionSlerp(fromRotation, toRotation, weight),
            XMVectorLerp(fromTranslation, toTranslation, weight));
    }

    DirectX::XMMATRIX SampleTrackAt(const BoneTrack& tr, size_t bone,
                                    float sampleTime) const {
        using namespace DirectX;
        const XMVECTOR t = SampleVec(
            tr.positions, XMLoadFloat3(&bindTranslation_[bone]), sampleTime);
        const XMVECTOR r = SampleQuat(
            tr.rotations, XMLoadFloat4(&bindRotation_[bone]), sampleTime);
        const XMVECTOR s = SampleVec(
            tr.scales, XMLoadFloat3(&bindScale_[bone]), sampleTime);
        return XMMatrixAffineTransformation(s, XMVectorZero(), r, t);
    }

    template <typename Keys>
    DirectX::XMVECTOR SampleVec(const Keys& keys, DirectX::XMVECTOR fallback,
                                float sampleTime) const {
        using namespace DirectX;
        if (keys.empty()) return fallback;
        if (keys.size() == 1) return XMLoadFloat3(&keys[0].value);
        if (sampleTime <= keys.front().time)
            return XMLoadFloat3(&keys.front().value);
        const auto upper = std::lower_bound(
            keys.begin() + 1, keys.end(), sampleTime,
            [](const auto& key, float sampleTime) { return key.time < sampleTime; });
        if (upper == keys.end()) return XMLoadFloat3(&keys.back().value);
        const auto& lower = *(upper - 1);
        const float span = upper->time - lower.time;
        const float a =
            span > 1e-6f ? (sampleTime - lower.time) / span : 0.0f;
        return XMVectorLerp(XMLoadFloat3(&lower.value), XMLoadFloat3(&upper->value), a);
    }

    DirectX::XMVECTOR SampleQuat(const std::vector<BoneTrack::QuatKey>& keys,
                                 DirectX::XMVECTOR fallback,
                                 float sampleTime) const {
        using namespace DirectX;
        if (keys.empty()) return fallback;
        if (keys.size() == 1) return XMLoadFloat4(&keys[0].value);
        if (sampleTime <= keys.front().time)
            return XMLoadFloat4(&keys.front().value);
        const auto upper = std::lower_bound(
            keys.begin() + 1, keys.end(), sampleTime,
            [](const BoneTrack::QuatKey& key, float sampleTime) { return key.time < sampleTime; });
        if (upper == keys.end()) return XMLoadFloat4(&keys.back().value);
        const auto& lower = *(upper - 1);
        const float span = upper->time - lower.time;
        const float a =
            span > 1e-6f ? (sampleTime - lower.time) / span : 0.0f;
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
