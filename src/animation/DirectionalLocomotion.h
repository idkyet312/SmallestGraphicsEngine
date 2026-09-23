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

    // An authored cycle to fit into the blend space instead of baking one, and
    // the gait it belongs to. Which slot it fills is measured from the clip
    // where that is possible, because a converted rig need not agree with the
    // engine about which side of the body +X is; a caller that knows can say
    // so instead.
    struct AuthoredCycle {
        std::string clip;
        bool running = true;
        // An authored cycle can stand in for the other gait as well. The IK
        // bake is the only thing that fills a slot otherwise, and it drags the
        // feet to targets rather than replaying real footwork, so a real clip
        // retimed to the walk's cycle beats a synthesised one.
        bool bothGaits = false;
        // Which slot to fill: 0 = forward, 1 = backward, 2 = left, 3 = right,
        // or -1 to measure it from the clip's own footfall. A running cycle
        // crosses its trailing leg over, so the planted foot sweeps both ways
        // within one cycle and the measurement cannot always separate the
        // four -- state it instead when the answer is already known.
        int direction = -1;
    };

    static bool Bake(const Skeleton& skel, std::vector<AnimationClip>& clips,
                     const std::vector<AuthoredCycle>& authored = {}) {
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
        // A rig may ship only the four directional Mixamo cycles.  Keep this
        // path separate from the IK fallback: it does not need a generic Walk
        // or Run clip to establish a cadence.
        if (!sources[0] || !sources[1])
            return authored.empty() ? false : BakeAuthored(skel, clips, authored);
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
            // The gait's own footfall, as a phase of the baked cycle. Authored
            // clips are shifted onto it so a strafe and a forward run planted
            // at the same phase stay planted together while they blend.
            const int upAxis = UpAxis(skel);
            int plant = 0;
            for (int s = 1; s < Samples; ++s)
                if (XMVectorGetByIndex(XMLoadFloat4x4(&frames[s][feet[0]]).r[3], upAxis) <
                    XMVectorGetByIndex(XMLoadFloat4x4(&frames[plant][feet[0]]).r[3], upAxis))
                    plant = s;
            const float plantPhase =
                float((plant - phaseOrigin + Samples) % Samples) / Samples;

            bool adopted[4] = {};
            for (const AuthoredCycle& cycle : authored) {
                const bool ownGait = cycle.running == (gait == 1);
                if (!ownGait && !cycle.bothGaits) continue;
                const int index = FindClip(clips, cycle.clip);
                if (index < 0) continue;
                const int direction = cycle.direction >= 0
                    ? cycle.direction : MeasureDirection(skel, clips[index]);
                if (direction < 0 || adopted[direction]) continue;
                // The clip belongs to one gait; the other borrows it at that
                // gait's cycle length, so a borrowed run strafe reads as a
                // walk instead of running on the spot.
                AnimationClip fitted = Resample(skel, clips[index],
                    Names[gait * 4 + direction], plantPhase,
                    ownGait ? 0.0f : source.clip->duration);
                // Source clips are also used by upper-body animation and are
                // part of the asset's public inventory. Never rename or
                // replace them while filling a directional slot.
                baked.push_back(std::move(fitted));
                adopted[direction] = true;
            }

            for (int direction = 0; direction < 4; ++direction) {
                if (adopted[direction]) continue;
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
                    WriteFrame(skel, globals, clip.duration * s / Samples, clip);
                }
                baked.push_back(std::move(clip));
            }
        }
        for (auto& clip : baked) clips.push_back(std::move(clip));
        return true;
    }

    // Bake a blend space from four authored cardinal cycles. This is the
    // normal path for Mixamo exports, which commonly contain no generic Walk
    // or Run clips. Each authored run is retained and sampled twice: its run
    // duration is kept for the run slots and stretched by 1.5x for walking.
    // Every source cycle is rolled so its lowest foot sample is at phase zero,
    // making independently exported clips line up when blended.
    static bool BakeAuthored(const Skeleton& skel,
                             std::vector<AnimationClip>& clips,
                             const std::vector<AuthoredCycle>& authored) {
        if (skel.Find("foot_l") < 0 || skel.Find("foot_r") < 0 ||
            skel.Find("thigh_l") < 0 || skel.Find("thigh_r") < 0)
            return false;
        std::array<const AnimationClip*, 4> source{};
        for (const AuthoredCycle& cycle : authored) {
            if (cycle.direction < 0 || cycle.direction >= 4) continue;
            const int index = FindClip(clips, cycle.clip);
            if (index >= 0 && !source[cycle.direction]) source[cycle.direction] = &clips[index];
        }
        for (const AnimationClip* clip : source)
            if (!clip || clip->duration <= 0.0f) return false;
        // Keeping source clips intact means their names cannot also identify
        // generated slots: otherwise the blend space would select an
        // unretimed source and the result would depend on vector order.
        for (int direction = 0; direction < 4; ++direction)
            if (FindClip(clips, Names[direction]) >= 0 ||
                FindClip(clips, Names[4 + direction]) >= 0)
                return false;

        std::vector<AnimationClip> baked;
        baked.reserve(8);
        for (int direction = 0; direction < 4; ++direction) {
            const AnimationClip& run = *source[direction];
            baked.push_back(Resample(skel, run, Names[direction], 0.0f,
                                     run.duration * 1.5f));
            baked.push_back(Resample(skel, run, Names[4 + direction], 0.0f,
                                     run.duration));
        }
        for (auto& clip : baked) clips.push_back(std::move(clip));
        return true;
    }

    // Which way an in-place cycle travels, as a blend-space direction index
    // (0 = forward, 1 = backward, 2 = left, 3 = right, -1 when the clip does
    // not travel at all). A planted foot does not move with the body, the body
    // moves over it, so the ground sweep of whichever foot is lower points
    // opposite to travel. Whichever axis the sweep favours decides the pair,
    // and its sign decides which of the two.
    static int MeasureDirection(const Skeleton& skel, const AnimationClip& clip) {
        using namespace DirectX;
        const int feet[2] = { skel.Find("foot_l"), skel.Find("foot_r") };
        const int pelvis = skel.Find("pelvis");
        if (feet[0] < 0 || feet[1] < 0 || pelvis < 0 || clip.duration <= 0.0f)
            return -1;
        // Which axis is which is a property of the rig, not of the engine: the
        // UE4 bandit stands along native +Z while a Mixamo rig stands along
        // +Y, and the two disagree about the remaining pair as well. Read the
        // frame off the REST pose -- mid-stride the feet are split along the
        // direction of travel, so a posed frame would answer for the clip
        // rather than for the skeleton.
        AnimationInstance rest;
        std::vector<XMFLOAT4X4> bind;
        rest.ComputeGlobalMatrices(skel, bind);

        // The pelvis sits directly above the feet, so the axis they differ
        // along most is up.
        XMFLOAT3 stance;
        XMStoreFloat3(&stance, XMVectorAbs(XMLoadFloat4x4(&bind[pelvis]).r[3] -
                                           XMLoadFloat4x4(&bind[feet[0]]).r[3]));
        const int up = stance.z >= stance.x && stance.z >= stance.y ? 2
                     : stance.y >= stance.x ? 1 : 0;

        // Left and right come from the axis the two feet straddle at rest, so
        // a rig mirrored the other way cannot transpose the two slots. The
        // remaining axis carries fore and aft.
        XMFLOAT3 straddle;
        XMStoreFloat3(&straddle, XMLoadFloat4x4(&bind[feet[0]]).r[3] -
                                 XMLoadFloat4x4(&bind[feet[1]]).r[3]);
        int lateral = up == 0 ? 1 : 0;
        for (int axis = 0; axis < 3; ++axis)
            if (axis != up && std::abs((&straddle.x)[axis]) >
                              std::abs((&straddle.x)[lateral]))
                lateral = axis;
        const int travel = 3 - up - lateral;
        // Which way along that axis the left foot lies, so the reading below
        // means the same thing on a rig mirrored the other way.
        const float handedness = (&straddle.x)[lateral] >= 0.0f ? 1.0f : -1.0f;

        AnimationInstance instance;
        instance.Play(&clip);
        std::vector<XMFLOAT4X4> previous, current;
        instance.ComputeGlobalMatrices(skel, previous);

        XMFLOAT3 total{};
        for (int s = 1; s <= Samples * 2; ++s) {
            instance.time = clip.duration * s / (Samples * 2);
            instance.ComputeGlobalMatrices(skel, current);
            for (int side = 0; side < 2; ++side) {
                XMFLOAT3 foot, other, was;
                XMStoreFloat3(&foot, XMLoadFloat4x4(&current[feet[side]]).r[3]);
                XMStoreFloat3(&other, XMLoadFloat4x4(&current[feet[1 - side]]).r[3]);
                XMStoreFloat3(&was, XMLoadFloat4x4(&previous[feet[side]]).r[3]);
                // Only the planted foot -- the lower of the two -- sweeps.
                if ((&foot.x)[up] > (&other.x)[up]) continue;
                for (int axis = 0; axis < 3; ++axis)
                    (&total.x)[axis] += (&foot.x)[axis] - (&was.x)[axis];
            }
            previous.swap(current);
        }
        // Measured on both rigs: a cycle travelling forward sweeps its planted
        // foot along native +Y on the Z-up bandit and native -Z on the Y-up
        // Mixamo rig, and one travelling left sweeps away from the side the
        // left foot rests on. Both readings are normalised to "positive means
        // forward" and "positive means right" here.
        const float sideways = (&total.x)[lateral] * handedness;
        const float forward = (up == 2 ? 1.0f : -1.0f) * (&total.x)[travel];
        if (std::abs(sideways) >= std::abs(forward))
            return std::abs(sideways) < 1e-3f ? -1 : (sideways > 0.0f ? 3 : 2);
        return std::abs(forward) < 1e-3f ? -1 : (forward > 0.0f ? 0 : 1);
    }

private:
    // The imported scene may be Y-up (Mixamo) or Z-up (the UE4 bandit).
    // Infer up from the bind-pose pelvis-to-foot separation instead of making
    // the resampler depend on one exporter convention.
    static int UpAxis(const Skeleton& skel) {
        const int pelvis = skel.Find("pelvis");
        const int foot = skel.Find("foot_l");
        if (pelvis < 0 || foot < 0) return 2;
        using namespace DirectX;
        AnimationInstance rest;
        std::vector<XMFLOAT4X4> bind;
        rest.ComputeGlobalMatrices(skel, bind);
        const XMVECTOR delta = XMVectorAbs(
            XMLoadFloat4x4(&bind[pelvis]).r[3] -
            XMLoadFloat4x4(&bind[foot]).r[3]);
        XMFLOAT3 d; XMStoreFloat3(&d, delta);
        return d.y >= d.x && d.y >= d.z ? 1 : (d.z >= d.x ? 2 : 0);
    }

    // One sampled pose becomes one key per bone, converted back out of model
    // space into the parent-local transforms a clip stores.
    static void WriteFrame(const Skeleton& skel,
                           const std::vector<DirectX::XMFLOAT4X4>& globals,
                           float time, AnimationClip& clip) {
        using namespace DirectX;
        for (size_t b = 0; b < skel.BoneCount(); ++b) {
            const int parent = skel.parent[b];
            const XMMATRIX local = XMLoadFloat4x4(&globals[b]) *
                (parent < 0 ? XMMatrixInverse(nullptr, XMLoadFloat4x4(&skel.globalInverse))
                            : XMMatrixInverse(nullptr, XMLoadFloat4x4(&globals[parent])));
            XMVECTOR scale, rotation, translation;
            XMMatrixDecompose(&scale, &rotation, &translation, local);
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

    static int FindClip(const std::vector<AnimationClip>& clips, const std::string& name) {
        for (size_t i = 0; i < clips.size(); ++i)
            if (clips[i].name == name) return static_cast<int>(i);
        return -1;
    }


    // Refit an authored cycle into the layout the blend space indexes: a key
    // per bone per phase sample, with the clip rolled so its own footfall lands
    // on the gait's. The seam repeats the first sample rather than resampling
    // the wrap, so a clip that does not quite close still loops cleanly.
    // `duration` retimes the result: the poses are the source's, spread over
    // whichever cycle length the gait being filled runs at. Zero keeps the
    // source's own length.
    static AnimationClip Resample(const Skeleton& skel, const AnimationClip& source,
                                  const std::string& name, float plantPhase,
                                  float duration = 0.0f) {
        using namespace DirectX;
        AnimationInstance instance;
        instance.Play(&source);
        const int foot = skel.Find("foot_l");
        std::vector<XMFLOAT4X4> globals;
        const int upAxis = UpAxis(skel);
        float plantTime = 0.0f, lowest = 0.0f;
        for (int s = 0; s < Samples; ++s) {
            instance.time = source.duration * s / Samples;
            instance.ComputeGlobalMatrices(skel, globals);
            const XMVECTOR position = XMLoadFloat4x4(&globals[foot]).r[3];
            const float height = XMVectorGetByIndex(position, upAxis);
            if (s == 0 || height < lowest) { lowest = height; plantTime = instance.time; }
        }
        const float start = std::fmod(
            source.duration * 2.0f + plantTime - plantPhase * source.duration,
            source.duration);

        AnimationClip clip;
        clip.name = name;
        clip.duration = duration > 0.0f ? duration : source.duration;
        clip.tracks.resize(skel.BoneCount());
        for (size_t b = 0; b < skel.BoneCount(); ++b)
            clip.tracks[b].bone = static_cast<int>(b);
        for (int s = 0; s <= Samples; ++s) {
            instance.time = s == Samples
                ? start : std::fmod(start + source.duration * s / Samples, source.duration);
            instance.ComputeGlobalMatrices(skel, globals);
            WriteFrame(skel, globals, clip.duration * s / Samples, clip);
        }
        return clip;
    }

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
    // legsOnlyClips: whether the directional clips can be trusted above the
    // hips. A cycle retargeted onto a foreign skeleton only has its legs
    // rebuilt and keeps bind pose everywhere else, so its torso has to be
    // suppressed; a cycle authored on this rig carries a real torso and
    // suppressing it would throw that away.
    bool Initialize(const Skeleton& skel, const std::vector<AnimationClip>& clips,
                    bool legsOnlyClips = true) {
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
        if (legsOnlyClips) BuildLowerBodyMask(skel);
        else lowerBody_.assign(skel.BoneCount(), true);
        legsOnly_ = legsOnlyClips;
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

    // Whether Initialize found every clip the blend space needs. Callers that
    // have no other source of motion have to know: Update returns null forever
    // once this is false, and a body that never gets a clip assigned renders
    // its bind pose -- a T-pose -- rather than standing still.
    bool Ready() const { return ready_; }

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
        // Above the hips a leg-only slot hands its weight to the gait it
        // belongs to, so the torso plays a plain forward cycle at the same
        // cadence while the legs go sideways. Retargeted arms and spine never
        // reach the mesh, and the gun layer keeps a stable pose to sit on.
        // Clips authored on this rig need none of that -- lowerBody_ is all
        // true for them, so `upper` goes unread.
        auto upper = weights;
        if (legsOnly_) for (int gait = 0; gait < 2; ++gait) {
            const int forwardSlot = 1 + gait * 4, backwardSlot = forwardSlot + 1;
            for (int side = 2; side < 4; ++side) {
                const int slot = forwardSlot + side;
                upper[forward_ >= 0.0f ? forwardSlot : backwardSlot] += upper[slot];
                upper[slot] = 0.0f;
            }
        }
        for (size_t b = 0; b < output_.tracks.size(); ++b) {
            XMVECTOR translation = XMVectorZero(), scale = XMVectorZero(), rotation = XMVectorZero();
            const XMVECTOR reference = XMLoadFloat4(&idle_.tracks[b].rotations[0].value);
            const auto& active = (b < lowerBody_.size() && lowerBody_[b]) ? weights : upper;
            for (int i = 0; i < 9; ++i) {
                if (active[i] <= 0.0f) continue;
                const auto& track = i == 0 ? idle_.tracks[b] : clips_[i]->tracks[b];
                const int lo = i == 0 ? 0 : frame, hi = i == 0 ? 0 : frame + 1;
                translation += XMVectorLerp(XMLoadFloat3(&track.positions[lo].value), XMLoadFloat3(&track.positions[hi].value), alpha) * active[i];
                scale += XMVectorLerp(XMLoadFloat3(&track.scales[lo].value), XMLoadFloat3(&track.scales[hi].value), alpha) * active[i];
                XMVECTOR q = XMQuaternionSlerp(XMLoadFloat4(&track.rotations[lo].value), XMLoadFloat4(&track.rotations[hi].value), alpha);
                if (XMVectorGetX(XMVector4Dot(reference, q)) < 0.0f) q = -q;
                rotation += q * active[i];
            }
            auto& track = output_.tracks[b];
            XMStoreFloat3(&track.positions[0].value, translation);
            XMStoreFloat3(&track.scales[0].value, scale);
            XMStoreFloat4(&track.rotations[0].value, XMQuaternionNormalize(rotation));
        }
        return &output_;
    }

private:
    // Which bones the blend space is allowed to write. The strafe cycles are
    // retargeted from another rig and only their legs are trustworthy, so the
    // pelvis and everything above it keeps the forward gait's own pose and the
    // upper-body gun layer still lands on an undisturbed spine.
    void BuildLowerBodyMask(const Skeleton& skel) {
        const int pelvis = skel.Find("pelvis");
        lowerBody_.assign(skel.BoneCount(), false);
        for (size_t b = 0; b < skel.BoneCount(); ++b) {
            // A leg bone is one that reaches the pelvis without passing through
            // the spine, so the arms and head are excluded by construction.
            for (int p = static_cast<int>(b); p >= 0; p = skel.parent[p]) {
                if (p == pelvis) { lowerBody_[b] = true; break; }
                const std::string& name = skel.names[p];
                if (name.rfind("spine", 0) == 0) break;
            }
        }
        // The pelvis itself carries the whole body, so a strafe's hip swing
        // would drag the torso sideways with it.
        if (pelvis >= 0) lowerBody_[pelvis] = false;
    }

    std::array<const AnimationClip*, 9> clips_{};
    AnimationClip output_, idle_;
    std::vector<bool> lowerBody_;
    bool legsOnly_ = true;
    float right_ = 0.0f, forward_ = 0.0f, phase_ = 0.0f;
    bool ready_ = false;
};
