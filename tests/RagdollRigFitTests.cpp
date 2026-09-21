#include "RagdollRigFit.h"

#include <DirectXMath.h>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void Check(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

Skeleton Rig(float childDistance, bool zUp) {
    Skeleton s;
    s.names = { "pelvis", "calf_r" };
    s.parent = { -1, 0 };
    s.index["pelvis"] = 0;
    s.index["calf_r"] = 1;
    const DirectX::XMMATRIX root = DirectX::XMMatrixIdentity();
    const DirectX::XMMATRIX child = DirectX::XMMatrixTranslation(
        zUp ? 0.0f : 0.0f, zUp ? 0.0f : childDistance,
        zUp ? childDistance : 0.0f);
    DirectX::XMStoreFloat4x4(&s.localBind.emplace_back(), root);
    DirectX::XMStoreFloat4x4(&s.localBind.emplace_back(), child);
    s.offset.resize(2);
    DirectX::XMStoreFloat4x4(&s.globalInverse, DirectX::XMMatrixIdentity());
    return s;
}
}

int main() {
    const Skeleton source = Rig(200.0f, true);
    const Skeleton target = Rig(300.0f, false);
    RagdollSpec authored;
    RagdollBodySpec body;
    body.bone = "pelvis";
    RagdollShapeSpec capsule;
    capsule.type = RagdollShapeType::Capsule;
    capsule.center = { 0.0f, 0.0f, 1.0f };
    DirectX::XMStoreFloat4(&capsule.rotation,
        DirectX::XMQuaternionRotationRollPitchYaw(DirectX::XM_PIDIV2, 0, 0));
    capsule.radius = 0.1f;
    capsule.length = 1.0f;
    capsule.halfExtent = { 0.1f, 0.6f, 0.1f };
    body.shapes.push_back(capsule);
    authored.bodies.push_back(body);
    RagdollConstraintSpec joint;
    joint.boneA = "pelvis";
    joint.boneB = "calf_r";
    joint.frameA.position = { 0, 2, 0 };
    joint.frameB.position = { 0, 0, 0 };
    authored.constraints.push_back(joint);

    const RagdollSpec fitted = RagdollRigFit::Fit(
        source, target, authored,
        DirectX::XMMatrixRotationX(-DirectX::XM_PIDIV2));
    Check(fitted.bodies.size() == 1 && fitted.constraints.size() == 1,
          "fit preserves the authored asset topology");
    const RagdollShapeSpec& resultShape = fitted.bodies[0].shapes[0];
    Check(std::isfinite(resultShape.center.y) &&
          std::isfinite(resultShape.length), "fitted shape values stay finite");
    Check(std::abs(resultShape.center.y - 1.5f) < 0.01f,
          "shape center follows the target segment and length ratio");
    Check(std::abs(resultShape.length - 1.5f) < 0.01f,
          "capsule length follows the target segment ratio");
    DirectX::XMFLOAT3 axis;
    DirectX::XMStoreFloat3(&axis, DirectX::XMVector3Rotate(
        DirectX::XMVectorSet(0, 1, 0, 0),
        DirectX::XMLoadFloat4(&resultShape.rotation)));
    Check(std::abs(axis.y) > 0.99f && std::abs(axis.x) < 0.01f &&
          std::abs(axis.z) < 0.01f,
          "capsule axis follows the target anatomical segment");
    const auto& fittedJoint = fitted.constraints[0];
    Check(std::abs(fittedJoint.frameA.position.y - 3.0f) < 0.01f &&
          std::abs(fittedJoint.frameB.position.y) < 0.01f,
          "constraint frames meet at the target child joint");
    if (failures == 0) std::cout << "Ragdoll rig fit tests passed\n";
    return failures == 0 ? 0 : 1;
}
