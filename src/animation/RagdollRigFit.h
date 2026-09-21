#pragma once

// CPU-only conversion of an authored physics asset between bind poses.  The
// physics asset stores frames in bone-local space, so copying it to a rig with
// different bone axes makes capsules and limits visibly miss their limbs.
#include "SkinnedTypes.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

struct RagdollRigFitOptions {
    bool scaleSegments = true;
};

struct RagdollRigFit {
    using Matrix = DirectX::XMMATRIX;

    static RagdollSpec Fit(const Skeleton& source, const Skeleton& target,
                           const RagdollSpec& authored,
                           DirectX::FXMMATRIX sourceToTarget,
                           const RagdollRigFitOptions& options = {}) {
        using namespace DirectX;
        if (source.BoneCount() == 0 || target.BoneCount() == 0) return {};
        for (const auto& body : authored.bodies)
            if (source.Find(body.bone) < 0 || target.Find(body.bone) < 0) return {};
        for (const auto& link : authored.constraints)
            if (source.Find(link.boneA) < 0 || source.Find(link.boneB) < 0 ||
                target.Find(link.boneA) < 0 || target.Find(link.boneB) < 0) return {};
        RagdollSpec result = authored;
        std::vector<Matrix> sourceGlobal, targetGlobal;
        Globals(source, sourceGlobal);
        Globals(target, targetGlobal);

        Matrix orientation = sourceToTarget;
        XMVECTOR ignoredScale, sourceOrientation, ignoredTranslation;
        if (!XMMatrixDecompose(&ignoredScale, &sourceOrientation,
                               &ignoredTranslation, orientation))
            orientation = XMMatrixIdentity();
        else
            orientation = XMMatrixRotationQuaternion(
                XMQuaternionNormalize(sourceOrientation));

        std::vector<Matrix> correction(target.BoneCount(), XMMatrixIdentity());
        std::vector<float> segmentRatio(target.BoneCount(), 1.0f);
        for (size_t i = 0; i < target.BoneCount(); ++i) {
            const int sourceBone = Find(source, target.names[i]);
            if (sourceBone < 0 || i >= targetGlobal.size() ||
                static_cast<size_t>(sourceBone) >= sourceGlobal.size())
                continue;
            const Matrix sr = RotationOnly(sourceGlobal[sourceBone]);
            const Matrix tr = RotationOnly(targetGlobal[i]);
            // Row-vector convention: source local -> source model -> target
            // model -> target local.
            Matrix c = sr * orientation * XMMatrixTranspose(tr);

            const int sourceChild = SegmentChild(source, sourceBone);
            const int targetChild = SegmentChild(target, static_cast<int>(i));
            if (sourceChild >= 0 && targetChild >= 0) {
                const XMVECTOR sourceDir = XMVector3Normalize(
                    XMVector3TransformNormal(
                        XMVectorSubtract(sourceGlobal[sourceChild].r[3],
                                         sourceGlobal[sourceBone].r[3]),
                        orientation));
                const XMVECTOR targetDir = XMVector3Normalize(
                    XMVectorSubtract(targetGlobal[targetChild].r[3],
                                     targetGlobal[i].r[3]));
                if (FiniteDirection(sourceDir) && FiniteDirection(targetDir)) {
                    const XMVECTOR rawSourceDir = XMVector3Normalize(
                        XMVectorSubtract(sourceGlobal[sourceChild].r[3],
                                         sourceGlobal[sourceBone].r[3]));
                    const XMVECTOR sourceLocalDir = XMVector3Normalize(
                        XMVector3TransformNormal(rawSourceDir,
                                                 XMMatrixTranspose(sr)));
                    const XMVECTOR targetLocalDir = XMVector3Normalize(
                        XMVector3TransformNormal(targetDir,
                                                 XMMatrixTranspose(tr)));
                    // Keep the imported axis correction's roll, but force its
                    // primary anatomical direction onto the target segment.
                    const XMVECTOR mapped = XMVector3TransformNormal(
                        sourceLocalDir, c);
                    c = c * Swing(mapped, targetLocalDir);
                    const float sourceLength = XMVectorGetX(XMVector3Length(
                        XMVectorSubtract(sourceGlobal[sourceChild].r[3],
                                           sourceGlobal[sourceBone].r[3])));
                    const float targetLength = XMVectorGetX(XMVector3Length(
                        XMVectorSubtract(targetGlobal[targetChild].r[3],
                                         targetGlobal[i].r[3])));
                    if (sourceLength > 1e-5f && targetLength > 1e-5f)
                        segmentRatio[i] = targetLength / sourceLength;
                }
            }
            correction[i] = c;
        }

        for (RagdollBodySpec& body : result.bodies) {
            const int targetBone = Find(target, body.bone);
            const int sourceBone = Find(source, body.bone);
            if (targetBone < 0 || sourceBone < 0) continue;
            const Matrix c = correction[static_cast<size_t>(targetBone)];
            const float ratio = options.scaleSegments
                ? segmentRatio[static_cast<size_t>(targetBone)] : 1.0f;
            const Matrix sourceRotation = RotationOnly(sourceGlobal[sourceBone]);
            const XMVECTOR axis = SegmentAxisLocal(
                source, sourceBone, sourceGlobal, sourceRotation);
            for (RagdollShapeSpec& shape : body.shapes) {
                XMVECTOR center = XMLoadFloat3(&shape.center);
                if (options.scaleSegments && ratio != 1.0f &&
                    FiniteDirection(axis)) {
                    center = XMVectorAdd(center, XMVectorScale(axis,
                        XMVectorGetX(XMVector3Dot(center, axis)) * (ratio - 1.0f)));
                }
                XMStoreFloat3(&shape.center,
                    XMVector3TransformNormal(center, c));
                const Matrix shapeMatrix =
                    XMMatrixRotationQuaternion(XMLoadFloat4(&shape.rotation)) * c;
                XMStoreFloat4(&shape.rotation,
                    XMQuaternionNormalize(XMQuaternionRotationMatrix(shapeMatrix)));
                if (options.scaleSegments && shape.type == RagdollShapeType::Capsule)
                    shape.length = std::max(0.0001f, shape.length * ratio);
                if (options.scaleSegments && shape.type == RagdollShapeType::Capsule)
                    shape.halfExtent.y = shape.radius + shape.length * 0.5f;
            }
        }

        for (RagdollConstraintSpec& link : result.constraints) {
            const int a = Find(target, link.boneA);
            const int b = Find(target, link.boneB);
            const int sourceA = Find(source, link.boneA);
            const int sourceB = Find(source, link.boneB);
            if (a < 0 || b < 0 || sourceA < 0 || sourceB < 0) continue;
            const Matrix ca = correction[static_cast<size_t>(a)];
            const XMVECTOR anchorWorld = JointAnchor(target, a, b, targetGlobal,
                                                     link.frameA.position);
            XMStoreFloat3(&link.frameA.position,
                          ToLocal(anchorWorld, targetGlobal[a]));
            XMStoreFloat3(&link.frameB.position,
                          ToLocal(anchorWorld, targetGlobal[b]));
            // Imported FBX bind translations are centimetres; physics frames
            // are metres, while their rotations are unitless.
            link.frameA.position.x *= 0.01f;
            link.frameA.position.y *= 0.01f;
            link.frameA.position.z *= 0.01f;
            link.frameB.position.x *= 0.01f;
            link.frameB.position.y *= 0.01f;
            link.frameB.position.z *= 0.01f;
            TransformFrame(link.frameA, ca);
            // Independent per-bone fitting would give the solver a false
            // angular error before the body has even started moving.
            link.frameB.primary = link.frameA.primary;
            link.frameB.secondary = link.frameA.secondary;
            TransformFrame(link.frameB, RotationOnly(targetGlobal[a]) *
                XMMatrixTranspose(RotationOnly(targetGlobal[b])));
        }
        return result;
    }

private:
    static int Find(const Skeleton& s, const std::string& name) {
        const int found = s.Find(name);
        if (found >= 0) return found;
        for (size_t i = 0; i < s.names.size(); ++i)
            if (s.names[i] == name) return static_cast<int>(i);
        return -1;
    }

    static void Globals(const Skeleton& s, std::vector<Matrix>& out) {
        std::vector<Matrix> raw(s.BoneCount(), DirectX::XMMatrixIdentity());
        out.resize(s.BoneCount(), DirectX::XMMatrixIdentity());
        for (size_t i = 0; i < s.BoneCount(); ++i) {
            const Matrix local = DirectX::XMLoadFloat4x4(&s.localBind[i]);
            const int p = i < s.parent.size() ? s.parent[i] : -1;
            raw[i] = p < 0 ? local : local * raw[static_cast<size_t>(p)];
        }
        const Matrix inverseRoot = DirectX::XMLoadFloat4x4(&s.globalInverse);
        for (size_t i = 0; i < s.BoneCount(); ++i) out[i] = raw[i] * inverseRoot;
    }

    static Matrix RotationOnly(Matrix m) {
        using namespace DirectX;
        XMVECTOR scale, rotation, translation;
        if (!XMMatrixDecompose(&scale, &rotation, &translation, m))
            return XMMatrixIdentity();
        return XMMatrixRotationQuaternion(XMQuaternionNormalize(rotation));
    }

    static bool FiniteDirection(DirectX::FXMVECTOR v) {
        using namespace DirectX;
        const float l = XMVectorGetX(XMVector3LengthSq(v));
        return std::isfinite(l) && l > 1e-8f;
    }

    static int SegmentChild(const Skeleton& s, int bone) {
        if (bone < 0 || static_cast<size_t>(bone) >= s.BoneCount()) return -1;
        const std::string& name = s.names[static_cast<size_t>(bone)];
        const char* torso = name == "pelvis" ? "spine_01" :
            name == "spine_01" ? "spine_02" :
            name == "spine_02" ? "spine_03" :
            name == "spine_03" ? "neck_01" :
            name == "neck_01" ? "head" : nullptr;
        if (torso) {
            const int endpoint = Find(s, torso);
            if (endpoint >= 0 && Descends(s, endpoint, bone)) return endpoint;
        }
        const char* wanted = nullptr;
        if (name.find("thigh") != std::string::npos) wanted = "calf";
        else if (name.find("calf") != std::string::npos) wanted = "foot";
        else if (name.find("upperarm") != std::string::npos) wanted = "lowerarm";
        else if (name.find("lowerarm") != std::string::npos) wanted = "hand";
        else if (name.find("foot") != std::string::npos) wanted = "ball";
        else if (name.find("spine") != std::string::npos) wanted = "spine";
        // FBX helper nodes can sit between anatomical joints.
        if (wanted && name.size() > 2 && name[name.size() - 2] == '_') {
            const int endpoint = Find(s, std::string(wanted) + name.substr(name.size() - 2));
            if (endpoint >= 0 && endpoint != bone && Descends(s, endpoint, bone)) return endpoint;
        }
        for (size_t i = 0; i < s.BoneCount(); ++i)
            if (s.parent[i] == bone && (!wanted ||
                s.names[i].find(wanted) != std::string::npos)) return static_cast<int>(i);
        for (size_t i = 0; i < s.BoneCount(); ++i)
            if (s.parent[i] == bone) return static_cast<int>(i);
        return -1;
    }

    static DirectX::XMVECTOR SegmentAxisLocal(const Skeleton& s, int bone,
        const std::vector<Matrix>& global, Matrix rotation) {
        const int child = SegmentChild(s, bone);
        if (child < 0) return DirectX::XMVectorZero();
        const auto world = DirectX::XMVectorSubtract(global[child].r[3], global[bone].r[3]);
        return DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(
            world, DirectX::XMMatrixTranspose(rotation)));
    }

    static Matrix Swing(DirectX::FXMVECTOR from, DirectX::FXMVECTOR to) {
        using namespace DirectX;
        const XMVECTOR a = XMVector3Normalize(from), b = XMVector3Normalize(to);
        const float dot = std::clamp(XMVectorGetX(XMVector3Dot(a, b)), -1.0f, 1.0f);
        if (dot > 0.9999f) return XMMatrixIdentity();
        XMVECTOR axis = XMVector3Cross(a, b);
        if (dot < -0.9999f) {
            axis = XMVector3Cross(a, XMVectorSet(1, 0, 0, 0));
            if (!FiniteDirection(axis)) axis = XMVector3Cross(a, XMVectorSet(0, 1, 0, 0));
            return XMMatrixRotationAxis(XMVector3Normalize(axis), XM_PI);
        }
        return XMMatrixRotationAxis(XMVector3Normalize(axis), std::acos(dot));
    }

    static DirectX::XMVECTOR ToLocal(DirectX::FXMVECTOR world, Matrix bone) {
        return DirectX::XMVector3TransformCoord(world,
            DirectX::XMMatrixInverse(nullptr, bone));
    }

    static DirectX::XMVECTOR JointAnchor(const Skeleton& target, int a, int b,
        const std::vector<Matrix>& globals, const DirectX::XMFLOAT3& fallback) {
        if (Descends(target, b, a))
            return globals[static_cast<size_t>(b)].r[3];
        if (Descends(target, a, b))
            return globals[static_cast<size_t>(a)].r[3];
        return DirectX::XMVector3TransformCoord(
            DirectX::XMVectorScale(DirectX::XMLoadFloat3(&fallback), 100.0f),
            globals[static_cast<size_t>(a)]);
    }

    static bool Descends(const Skeleton& s, int child, int ancestor) {
        for (int p = child; p >= 0; p = s.parent[p])
            if (p == ancestor) return true;
        return false;
    }

    static void TransformFrame(RagdollJointFrame& frame, Matrix correction) {
        using namespace DirectX;
        frame.primary = StoreDirection(XMVector3TransformNormal(
            XMLoadFloat3(&frame.primary), correction));
        frame.secondary = StoreDirection(XMVector3TransformNormal(
            XMLoadFloat3(&frame.secondary), correction));
    }

    static DirectX::XMFLOAT3 StoreDirection(DirectX::FXMVECTOR v) {
        DirectX::XMFLOAT3 out{ 1,0,0 };
        if (FiniteDirection(v)) DirectX::XMStoreFloat3(&out, DirectX::XMVector3Normalize(v));
        return out;
    }
};
