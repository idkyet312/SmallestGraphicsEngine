#pragma once

#include "AnimationRuntime.h"
#include <array>

// Bake direction-specific foot trajectories once at asset load. The source
// cycle supplies weight transfer, foot lift and knee bend; IK redirects the
// stride without rotating the whole character away from its weapon target.
class DirectionalLocomotion {
public:
    static constexpr int Samples = 32;
    static constexpr const char* Names[8] = {
        "WalkForward", "WalkBackward", "WalkLeft", "WalkRight",
        "RunForward", "RunBackward", "RunLeft", "RunRight" };

    static bool Bake(const Skeleton& skel, std::vector<AnimationClip>& clips) {
        using namespace DirectX;
        const int thighs[2] = { skel.Find("thigh_l"), skel.Find("thigh_r") };
        const int calves[2] = { skel.Find("calf_l"), skel.Find("calf_r") };
        const int feet[2] = { skel.Find("foot_l"), skel.Find("foot_r") };
        for (int side = 0; side < 2; ++side)
            if (thighs[side] < 0 || calves[side] < 0 || feet[side] < 0) return false;
        const AnimationClip* sources[2] = {};
        for (const auto& clip : clips) {
            if (clip.name == "Walk" || clip.name == "ThirdPersonWalk") sources[0] = &clip;
            if (clip.name == "Run" || clip.name == "ThirdPersonRun") sources[1] = &clip;
            if (clip.name == Names[0]) return true;
        }
        if (!sources[0] || !sources[1]) return false;
        std::vector<AnimationClip> baked;
        // Bandit FBX is Z-up: after RotationX(-pi/2), native -Y is
        // engine forward and native +X is engine right.
        const XMVECTOR forward = XMVectorSet(0, -1, 0, 0);
        const XMVECTOR right = XMVectorSet(1, 0, 0, 0);
        const XMVECTOR directions[4] = { forward, -forward, -right, right };
        for (int gait = 0; gait < 2; ++gait) {
            AnimationInstance source;
            source.Play(sources[gait]);
            source.loopBlendDuration = 0.05f;
            std::array<std::vector<XMFLOAT4X4>, Samples> frames;
            XMVECTOR centers[2] = { XMVectorZero(), XMVectorZero() };
            for (int s = 0; s < Samples; ++s) {
                source.time = source.clip->duration * s / Samples;
                source.ComputeGlobalMatrices(skel, frames[s]);
                for (int side = 0; side < 2; ++side)
                    centers[side] += XMLoadFloat4x4(&frames[s][feet[side]]).r[3] / float(Samples);
            }
            // Align walk/run by the same foot's forward reach, rather than
            // trusting the unrelated FBX timeline origins to share a contact.
            int phaseOrigin = 0;
            for (int s = 1; s < Samples; ++s)
                if (XMVectorGetX(XMVector3Dot(XMLoadFloat4x4(&frames[s][feet[0]]).r[3], forward)) >
                    XMVectorGetX(XMVector3Dot(XMLoadFloat4x4(&frames[phaseOrigin][feet[0]]).r[3], forward)))
                    phaseOrigin = s;
            for (int direction = 0; direction < 4; ++direction) {
                AnimationClip clip;
                clip.name = Names[gait * 4 + direction];
                const bool strafe = direction >= 2;
                clip.duration = source.clip->duration * (strafe ? 0.65f : 1.0f);
                clip.tracks.resize(skel.BoneCount());
                for (size_t b = 0; b < skel.BoneCount(); ++b)
                    clip.tracks[b].bone = static_cast<int>(b);
                for (int s = 0; s <= Samples; ++s) {
                    auto globals = frames[(s + phaseOrigin) % Samples];
                    if (direction != 0) for (int side = 0; side < 2; ++side) {
                        const XMMATRIX foot = XMLoadFloat4x4(&globals[feet[side]]);
                        const float stride = XMVectorGetX(XMVector3Dot(foot.r[3] - centers[side], forward));
                        // Retain the natural lateral stance and toe orientation;
                        // only the fore/aft displacement becomes a side/back step.
                        // A forward stride turned sideways crosses the shins.
                        // Use a shorter, quicker shuffle with a wider stance.
                        const float lateralOffset = strafe
                            ? (XMVectorGetX(centers[side]) >= 0.0f ? 6.0f : -6.0f) : 0.0f;
                        const XMVECTOR target = foot.r[3] - forward * stride +
                            directions[direction] * stride * (strafe ? 0.33f : 1.0f) +
                            right * lateralOffset;
                        SolveLeg(skel, globals, thighs[side], calves[side], feet[side], target);
                        const XMMATRIX movedFoot = XMLoadFloat4x4(&globals[feet[side]]);
                        XMMATRIX planted = foot;
                        planted.r[3] = movedFoot.r[3];
                        TransformBranch(skel, globals, feet[side], XMMatrixInverse(nullptr, movedFoot) * planted);
                    }
                    for (size_t b = 0; b < skel.BoneCount(); ++b) {
                        const int parent = skel.parent[b];
                        const XMMATRIX local = XMLoadFloat4x4(&globals[b]) *
                            (parent < 0 ? XMMatrixInverse(nullptr, XMLoadFloat4x4(&skel.globalInverse))
                                        : XMMatrixInverse(nullptr, XMLoadFloat4x4(&globals[parent])));
                        XMVECTOR scale, rotation, translation;
                        XMMatrixDecompose(&scale, &rotation, &translation, local);
                        const float time = clip.duration * s / Samples;
                        BoneTrack::VecKey p{time, {}}, sc{time, {}};
                        BoneTrack::QuatKey r{time, {}};
                        XMStoreFloat3(&p.value, translation);
                        XMStoreFloat3(&sc.value, scale);
                        XMStoreFloat4(&r.value, XMQuaternionNormalize(rotation));
                        clip.tracks[b].positions.push_back(p);
                        clip.tracks[b].scales.push_back(sc);
                        clip.tracks[b].rotations.push_back(r);
                    }
                }
                baked.push_back(std::move(clip));
            }
        }
        for (auto& clip : baked) clips.push_back(std::move(clip));
        return true;
    }

private:
    static void TransformBranch(const Skeleton& skel,
        std::vector<DirectX::XMFLOAT4X4>& globals, int root, DirectX::FXMMATRIX delta) {
        using namespace DirectX;
        for (size_t b = 0; b < globals.size(); ++b) {
            int p = static_cast<int>(b);
            while (p >= 0 && p != root) p = skel.parent[p];
            if (p == root) XMStoreFloat4x4(&globals[b], XMLoadFloat4x4(&globals[b]) * delta);
        }
    }

    static DirectX::XMMATRIX FromTo(DirectX::FXMVECTOR a, DirectX::FXMVECTOR b) {
        using namespace DirectX;
        const XMVECTOR from = XMVector3Normalize(a), to = XMVector3Normalize(b);
        const float dot = (std::clamp)(XMVectorGetX(XMVector3Dot(from, to)), -1.0f, 1.0f);
        if (dot > 0.99999f) return XMMatrixIdentity();
        XMVECTOR axis = XMVector3Cross(from, to);
        if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-8f) {
            axis = XMVector3Cross(from, XMVectorSet(1, 0, 0, 0));
            if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-8f)
                axis = XMVector3Cross(from, XMVectorSet(0, 1, 0, 0));
        }
        return XMMatrixRotationAxis(axis, std::acos(dot));
    }

    static void SolveLeg(const Skeleton& skel, std::vector<DirectX::XMFLOAT4X4>& globals,
        int thigh, int calf, int foot, DirectX::FXMVECTOR target) {
        using namespace DirectX;
        auto pos = [&](int b) { return XMLoadFloat4x4(&globals[b]).r[3]; };
        const XMVECTOR hip = pos(thigh), knee = pos(calf), ankle = pos(foot);
        const float a = XMVectorGetX(XMVector3Length(knee - hip));
        const float b = XMVectorGetX(XMVector3Length(ankle - knee));
        const float distance = XMVectorGetX(XMVector3Length(target - hip));
        if (a < 1e-4f || b < 1e-4f || distance < 1e-4f) return;
        const XMVECTOR direction = (target - hip) / distance;
        const float reach = (std::clamp)(distance, std::abs(a - b) + 0.001f, a + b - 0.001f);
        const float along = (a*a - b*b + reach*reach) / (2.0f * reach);
        XMVECTOR bend = knee - hip - direction * XMVector3Dot(knee - hip, direction);
        if (XMVectorGetX(XMVector3LengthSq(bend)) < 1e-6f) {
            const XMVECTOR forward = XMVectorSet(0, -1, 0, 0);
            bend = forward - direction * XMVector3Dot(forward, direction);
        }
        const XMVECTOR desiredKnee = hip + direction * along + XMVector3Normalize(bend) *
            std::sqrt((std::max)(0.0f, a*a - along*along));
        auto rotate = [&](int root, XMVECTOR pivot, XMMATRIX rotation) {
            TransformBranch(skel, globals, root,
                XMMatrixTranslationFromVector(-pivot) * rotation * XMMatrixTranslationFromVector(pivot));
        };
        rotate(thigh, hip, FromTo(knee - hip, desiredKnee - hip));
        const XMVECTOR newKnee = pos(calf);
        rotate(calf, newKnee, FromTo(pos(foot) - newKnee, hip + direction * reach - newKnee));
    }
};

// A phase-synchronised polar 2D blend space: facing-local velocity selects
// adjacent cardinal directions, speed blends idle/walk/run. The evaluated
// one-frame clip plugs into the existing upper-body layer and rifle IK.
class LocomotionBlendSpace {
public:
    bool Initialize(const Skeleton& skel, const std::vector<AnimationClip>& clips) {
        ready_ = false;
        clips_.fill(nullptr);
        right_ = forward_ = phase_ = 0.0f;
        for (const auto& clip : clips) {
            if (clip.name == "Idle" || clip.name == "ThirdPersonIdle") clips_[0] = &clip;
            for (int i = 0; i < 8; ++i)
                if (clip.name == DirectionalLocomotion::Names[i]) clips_[i + 1] = &clip;
        }
        for (auto clip : clips_) if (!clip) return false;
        output_.name = "Directional blend space";
        output_.duration = 1.0f;
        output_.tracks.resize(skel.BoneCount());
        // Idle needs the same dense, phase-sampled layout as the baked clips.
        AnimationInstance idle;
        idle.Play(clips_[0]);
        std::vector<DirectX::XMFLOAT4X4> globals;
        idle.ComputeGlobalMatrices(skel, globals);
        for (size_t b = 0; b < skel.BoneCount(); ++b) {
            using namespace DirectX;
            auto& track = output_.tracks[b];
            track.bone = static_cast<int>(b);
            XMVECTOR s, r, t;
            const int p = skel.parent[b];
            XMMatrixDecompose(&s, &r, &t, XMLoadFloat4x4(&globals[b]) *
                (p < 0 ? XMMatrixInverse(nullptr, XMLoadFloat4x4(&skel.globalInverse))
                       : XMMatrixInverse(nullptr, XMLoadFloat4x4(&globals[p]))));
            track.positions.resize(1); track.scales.resize(1); track.rotations.resize(1);
            XMStoreFloat3(&track.positions[0].value, t);
            XMStoreFloat3(&track.scales[0].value, s);
            XMStoreFloat4(&track.rotations[0].value, r);
        }
        idle_ = output_;
        ready_ = true;
        return true;
    }

    static std::array<float, 9> Weights(float right, float forward, float walkSpeed) {
        std::array<float, 9> w{};
        const float speed = std::sqrt(right*right + forward*forward);
        if (speed < 1e-5f || walkSpeed <= 0.0f) { w[0] = 1.0f; return w; }
        const float moving = (std::min)(1.0f, speed / walkSpeed);
        const float run = (std::clamp)((speed / walkSpeed - 1.0f) / 0.65f, 0.0f, 1.0f);
        const float sum = std::abs(right) + std::abs(forward);
        const int fb = forward >= 0 ? 1 : 2, lr = right >= 0 ? 4 : 3;
        w[0] = 1.0f - moving;
        w[fb] = moving * (1.0f - run) * std::abs(forward) / sum;
        w[lr] = moving * (1.0f - run) * std::abs(right) / sum;
        w[fb + 4] = moving * run * std::abs(forward) / sum;
        w[lr + 4] = moving * run * std::abs(right) / sum;
        return w;
    }

    const AnimationClip* Update(float dt, float right, float forward, float walkSpeed) {
        using namespace DirectX;
        if (!ready_) return nullptr;
        dt = (std::max)(0.0f, dt);
        const float response = 1.0f - std::exp(-12.0f * dt);
        right_ += (right - right_) * response;
        forward_ += (forward - forward_) * response;
        const auto weights = Weights(right_, forward_, walkSpeed);
        float cadence = 0.0f;
        for (int i = 1; i < 9; ++i)
            cadence += weights[i] / (std::max)(0.01f, clips_[i]->duration);
        phase_ = std::fmod(phase_ + dt * cadence, 1.0f);
        const float sample = phase_ * DirectionalLocomotion::Samples;
        const int frame = static_cast<int>(sample);
        const float alpha = sample - frame;
        for (size_t b = 0; b < output_.tracks.size(); ++b) {
            XMVECTOR translation = XMVectorZero(), scale = XMVectorZero(), rotation = XMVectorZero();
            const XMVECTOR reference = XMLoadFloat4(&idle_.tracks[b].rotations[0].value);
            for (int i = 0; i < 9; ++i) {
                if (weights[i] <= 0.0f) continue;
                const auto& track = i == 0 ? idle_.tracks[b] : clips_[i]->tracks[b];
                const int lo = i == 0 ? 0 : frame, hi = i == 0 ? 0 : frame + 1;
                translation += XMVectorLerp(XMLoadFloat3(&track.positions[lo].value), XMLoadFloat3(&track.positions[hi].value), alpha) * weights[i];
                scale += XMVectorLerp(XMLoadFloat3(&track.scales[lo].value), XMLoadFloat3(&track.scales[hi].value), alpha) * weights[i];
                XMVECTOR q = XMQuaternionSlerp(XMLoadFloat4(&track.rotations[lo].value), XMLoadFloat4(&track.rotations[hi].value), alpha);
                if (XMVectorGetX(XMVector4Dot(reference, q)) < 0.0f) q = -q;
                rotation += q * weights[i];
            }
            auto& track = output_.tracks[b];
            XMStoreFloat3(&track.positions[0].value, translation);
            XMStoreFloat3(&track.scales[0].value, scale);
            XMStoreFloat4(&track.rotations[0].value, XMQuaternionNormalize(rotation));
        }
        return &output_;
    }

private:
    std::array<const AnimationClip*, 9> clips_{};
    AnimationClip output_, idle_;
    float right_ = 0.0f, forward_ = 0.0f, phase_ = 0.0f;
    bool ready_ = false;
};
