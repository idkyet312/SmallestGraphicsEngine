#include "T3DPhysicsAsset.h"
#include "PhysicsImpactPolicy.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
}

int main() {
    const std::string path = std::string(SGE_SOURCE_DIR) +
        "/Content/Models/MilitaryMercenaryBandit/Phy_Bandit_PhysicsAsset.T3D";
    const RagdollSpec ragdoll = T3DPhysicsAsset::Load(path);
    Check(ragdoll.bodies.size() == 16, "Bandit must load 16 bodies");
    Check(ragdoll.constraints.size() == 15, "Bandit must load 15 constraints");

    float massFraction = 0.0f;
    const RagdollBodySpec* torso = nullptr;
    const RagdollBodySpec* thigh = nullptr;
    const RagdollBodySpec* calf = nullptr;
    const RagdollBodySpec* rightFoot = nullptr;
    const RagdollBodySpec* leftFoot = nullptr;
    for (const RagdollBodySpec& body : ragdoll.bodies) {
        massFraction += body.massFraction;
        if (body.bone == "spine_02") torso = &body;
        if (body.bone == "thigh_r") thigh = &body;
        if (body.bone == "calf_r") calf = &body;
        if (body.bone == "foot_r") rightFoot = &body;
        if (body.bone == "foot_l") leftFoot = &body;
    }
    Check(std::abs(massFraction - 1.0f) < 0.0001f,
          "human body mass fractions must sum to one");
    Check(torso != nullptr, "spine_02 body missing");
    Check(torso && torso->shapes.size() == 4,
          "spine_02 must preserve all four authored capsules");
    if (torso) {
        for (const RagdollShapeSpec& shape : torso->shapes)
            Check(shape.type == RagdollShapeType::Capsule,
                  "spine_02 primitive must be capsule");
    }
    Check(thigh && thigh->shapes.size() == 1,
          "right thigh capsule missing");
    Check(thigh && thigh->shapes[0].radius < 0.075f,
          "thigh capsule radius must be reduced for self-collision");
    Check(thigh && thigh->shapes[0].radius > 0.073f,
          "thigh capsule radius reduction must preserve limb volume");
    Check(thigh && thigh->shapes[0].length > 0.23f,
          "thigh capsule must preserve authored bone coverage");
    if (thigh && !thigh->shapes.empty()) {
        DirectX::XMFLOAT3 axis;
        DirectX::XMStoreFloat3(&axis, DirectX::XMVector3Rotate(
            DirectX::XMVectorSet(0, 1, 0, 0),
            DirectX::XMLoadFloat4(&thigh->shapes[0].rotation)));
        Check(std::abs(axis.x) > 0.95f,
              "thigh capsule long axis must follow thigh bone");
        Check(std::abs(axis.y) < 0.10f && std::abs(axis.z) < 0.10f,
              "thigh capsule must not stand across thigh bone");
    }
    Check(calf && calf->shapes.size() == 1,
          "right calf capsule missing");
    Check(calf && calf->shapes[0].radius < 0.075f,
          "calf capsule radius must be reduced for self-collision");
    Check(calf && calf->shapes[0].length > 0.20f,
          "calf capsule must preserve authored bone coverage");
    if (calf && !calf->shapes.empty()) {
        DirectX::XMFLOAT3 axis;
        DirectX::XMStoreFloat3(&axis, DirectX::XMVector3Rotate(
            DirectX::XMVectorSet(0, 1, 0, 0),
            DirectX::XMLoadFloat4(&calf->shapes[0].rotation)));
        Check(std::abs(axis.x) > 0.95f,
              "calf capsule long axis must follow calf bone");
    }
    Check(rightFoot && rightFoot->shapes.size() == 1,
          "right foot collider missing");
    Check(leftFoot && leftFoot->shapes.size() == 1,
          "left foot collider missing");
    if (rightFoot && !rightFoot->shapes.empty()) {
        Check(rightFoot->shapes[0].type == RagdollShapeType::Box,
              "right foot collider must remain a box");
        Check(rightFoot->shapes[0].center.x < -0.095f,
              "right foot collider must be flipped 180 degrees around bone Y");
    }
    if (leftFoot && !leftFoot->shapes.empty()) {
        Check(leftFoot->shapes[0].type == RagdollShapeType::Box,
              "left foot collider must remain a box");
        Check(leftFoot->shapes[0].center.x > 0.095f,
              "left foot collider must be flipped 180 degrees around bone Y");
    }

    int hingeCount = 0;
    bool foundKneeFrame = false;
    int hipCount = 0;
    for (const RagdollConstraintSpec& joint : ragdoll.constraints) {
        if (joint.jointType == RagdollJointType::Hinge) ++hingeCount;
        if (joint.boneA.find("thigh") != std::string::npos) {
            ++hipCount;
            Check(joint.swing2Angle <= 18.01f * DirectX::XM_PI / 180.0f,
                  "hip abduction must prevent death-pose leg splay");
            Check(joint.upperTwistAngle <= 20.01f * DirectX::XM_PI / 180.0f,
                  "hip twist must stay anatomical");
        }
        if (joint.boneA == "calf_r" && joint.boneB == "thigh_r") {
            foundKneeFrame = true;
            Check(std::abs(joint.frameB.position.x - 0.43508488f) < 0.0001f,
                  "knee must preserve authored frame position");
            Check(joint.lowerHingeAngle >= -0.0874f,
                  "knee hyperextension must not exceed five degrees");
            Check(joint.upperHingeAngle > 2.5f,
                  "knee must retain human flexion range");
        }
    }
    Check(hingeCount == 4, "knees and elbows must use four hinge joints");
    Check(hipCount == 2, "both hip constraints must be calibrated");
    Check(foundKneeFrame, "right knee constraint missing");

    Check(!PhysicsImpactPolicy::CanFracture(
              PhysicsImpactPolicy::Ragdoll, PhysicsImpactPolicy::World),
          "ragdoll landing must not fracture world structures");
    Check(!PhysicsImpactPolicy::CanFracture(
              PhysicsImpactPolicy::Ragdoll, PhysicsImpactPolicy::Debris),
          "ragdoll collision must suppress fracture even against debris");
    Check(PhysicsImpactPolicy::CanFracture(
              PhysicsImpactPolicy::Debris, PhysicsImpactPolicy::World),
          "ordinary debris must retain impact fracture");
    Check(PhysicsImpactPolicy::CanFracture(
              PhysicsImpactPolicy::Vehicle, PhysicsImpactPolicy::World),
          "vehicle must retain impact fracture");
    if (failures == 0) std::cout << "Ragdoll asset tests passed\n";
    return failures == 0 ? 0 : 1;
}
