#pragma once

#include "SkinnedTypes.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <utility>

// Builds a small, perfectly looping additive run cycle for first-person arms.
// The authored rifle idle stays as the base pose; this contributes cadence only.
class ProceduralRunAnimation {
public:
    struct Settings {
        float duration = 1.04f;
        float rootSway = 0.45f;    // native skeleton units
        float rootBob = 0.65f;
        float rootSurge = 0.20f;
        float torsoPitch = 0.70f;  // degrees
        float torsoYaw = 0.50f;
        float torsoRoll = 0.90f;
        float armSwing = 1.40f;
    };

    static AnimationClip Build(const Skeleton& skeleton,
                               const Settings& settings = {}) {
        AnimationClip clip;
        clip.name = "Procedural Run";
        clip.duration = (std::max)(0.1f, settings.duration);

        AddTrack(clip, skeleton, "hips", settings, Motion::Root, 1.0f);
        AddTrack(clip, skeleton, "spine", settings, Motion::Torso, 0.35f);
        AddTrack(clip, skeleton, "spine1", settings, Motion::Torso, 0.65f);
        AddTrack(clip, skeleton, "spine2", settings, Motion::Torso, 1.0f);
        AddTrack(clip, skeleton, "leftshoulder", settings,
                 Motion::Shoulder, 1.0f);
        AddTrack(clip, skeleton, "rightshoulder", settings,
                 Motion::Shoulder, -1.0f);
        AddTrack(clip, skeleton, "leftarm", settings, Motion::Arm, 1.0f);
        AddTrack(clip, skeleton, "rightarm", settings, Motion::Arm, -1.0f);
        AddTrack(clip, skeleton, "leftforearm", settings,
                 Motion::Forearm, 1.0f);
        AddTrack(clip, skeleton, "rightforearm", settings,
                 Motion::Forearm, -1.0f);
        AddTrack(clip, skeleton, "lefthand", settings, Motion::Hand, 1.0f);
        AddTrack(clip, skeleton, "righthand", settings, Motion::Hand, -1.0f);
        return clip;
    }

private:
    enum class Motion { Root, Torso, Shoulder, Arm, Forearm, Hand };
    static constexpr int kSamples = 16;

    static std::string LeafName(std::string name) {
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const size_t colon = name.find_last_of(':');
        if (colon != std::string::npos) name.erase(0, colon + 1);
        return name;
    }

    static int FindBone(const Skeleton& skeleton, const char* leafName) {
        for (size_t i = 0; i < skeleton.names.size(); ++i)
            if (LeafName(skeleton.names[i]) == leafName)
                return static_cast<int>(i);
        return -1;
    }

    static void AddTrack(AnimationClip& clip, const Skeleton& skeleton,
                         const char* boneName, const Settings& settings,
                         Motion motion, float sideOrStrength) {
        using namespace DirectX;
        const int bone = FindBone(skeleton, boneName);
        if (bone < 0 || static_cast<size_t>(bone) >= skeleton.localBind.size())
            return;

        XMVECTOR bindScale, bindRotation, bindTranslation;
        if (!XMMatrixDecompose(
                &bindScale, &bindRotation, &bindTranslation,
                XMLoadFloat4x4(&skeleton.localBind[bone])))
            return;

        BoneTrack track;
        track.bone = bone;
        for (int sample = 0; sample <= kSamples; ++sample) {
            const float phase =
                static_cast<float>(sample) / static_cast<float>(kSamples);
            const float stride = std::sin(XM_2PI * phase);
            const float doubleStride = std::sin(2.0f * XM_2PI * phase);
            const float bob =
                0.5f * (1.0f - std::cos(2.0f * XM_2PI * phase));

            XMVECTOR translation = bindTranslation;
            float pitch = 0.0f;
            float yaw = 0.0f;
            float roll = 0.0f;
            switch (motion) {
            case Motion::Root:
                translation = XMVectorAdd(bindTranslation, XMVectorSet(
                    settings.rootSway * stride,
                    settings.rootBob * bob,
                    settings.rootSurge * doubleStride, 0.0f));
                pitch = settings.torsoPitch * doubleStride * 0.45f;
                yaw = settings.torsoYaw * stride * 0.35f;
                roll = settings.torsoRoll * stride * 0.45f;
                break;
            case Motion::Torso:
                pitch = settings.torsoPitch * doubleStride * sideOrStrength;
                yaw = settings.torsoYaw * stride * sideOrStrength;
                roll = settings.torsoRoll * stride * sideOrStrength;
                break;
            case Motion::Shoulder:
                pitch = settings.armSwing * stride * sideOrStrength * 0.25f;
                roll = settings.armSwing * bob * sideOrStrength * 0.18f;
                break;
            case Motion::Arm:
                pitch = settings.armSwing * stride * sideOrStrength;
                yaw = settings.armSwing * doubleStride * sideOrStrength * 0.18f;
                break;
            case Motion::Forearm:
                pitch = settings.armSwing * stride * sideOrStrength * 0.35f;
                roll = settings.armSwing * doubleStride * sideOrStrength * 0.12f;
                break;
            case Motion::Hand:
                yaw = settings.armSwing * stride * sideOrStrength * 0.12f;
                roll = settings.armSwing * doubleStride * sideOrStrength * 0.16f;
                break;
            }

            const XMMATRIX rotationMatrix =
                XMMatrixRotationQuaternion(bindRotation) *
                XMMatrixRotationRollPitchYaw(
                    XMConvertToRadians(pitch), XMConvertToRadians(yaw),
                    XMConvertToRadians(roll));
            XMVECTOR ignoredScale, rotation, ignoredTranslation;
            if (!XMMatrixDecompose(&ignoredScale, &rotation,
                                   &ignoredTranslation, rotationMatrix))
                rotation = bindRotation;

            BoneTrack::VecKey positionKey;
            positionKey.time = phase * clip.duration;
            XMStoreFloat3(&positionKey.value, translation);
            track.positions.push_back(positionKey);

            BoneTrack::QuatKey rotationKey;
            rotationKey.time = positionKey.time;
            XMStoreFloat4(
                &rotationKey.value, XMQuaternionNormalize(rotation));
            track.rotations.push_back(rotationKey);
        }
        clip.tracks.push_back(std::move(track));
    }
};
