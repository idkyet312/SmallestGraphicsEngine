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
    for (const RagdollBodySpec& body : ragdoll.bodies) {
        massFraction += body.massFraction;
        if (body.bone == "spine_02") torso = &body;
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

    int hingeCount = 0;
    bool foundKneeFrame = false;
    for (const RagdollConstraintSpec& joint : ragdoll.constraints) {
        if (joint.jointType == RagdollJointType::Hinge) ++hingeCount;
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
