#pragma once
// Shared data types for GPU skeletal skinning: the per-vertex skin attributes,
// the skeleton, animation clips, and the authored physics-asset (T3D) ragdoll
// spec. Kept header-only so both the importer and the runtime can include it
// without pulling in assimp.
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Per-vertex skinning attributes, stored in a buffer parallel to the 12-float
// interleaved vertex stream (index i here corresponds to vertex i there). 32
// bytes, matches the HLSL StructuredBuffer<SkinVertex> at t13.
struct SkinVertex {
    uint32_t boneIndex[4] = { 0, 0, 0, 0 };
    float    boneWeight[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

// Flat skeleton. Every parallel array is indexed by bone id; parent[i] is the
// bone id of i's parent (-1 for the root). Bone ids are assigned in hierarchy
// order so a single forward pass computes global transforms.
struct Skeleton {
    std::vector<std::string>          names;
    std::vector<int>                  parent;    // parent bone id, -1 for root
    std::vector<DirectX::XMFLOAT4X4>  offset;    // inverse-bind (mesh->bone) matrix
    std::vector<DirectX::XMFLOAT4X4>  localBind; // node local transform at bind pose
    DirectX::XMFLOAT4X4               globalInverse = {}; // inverse imported scene root
    std::unordered_map<std::string, int> index; // bone name -> id

    int Find(const std::string& name) const {
        auto it = index.find(name);
        return it == index.end() ? -1 : it->second;
    }
    size_t BoneCount() const { return names.size(); }
};

// One animated bone track: separate keyframe streams for T/R/S. Times are in
// seconds. A track only exists for bones the clip actually animates; missing
// bones fall back to their bind-pose local transform.
struct BoneTrack {
    int bone = -1;
    struct VecKey { float time; DirectX::XMFLOAT3 value; };
    struct QuatKey { float time; DirectX::XMFLOAT4 value; };
    std::vector<VecKey>  positions;
    std::vector<QuatKey> rotations;
    std::vector<VecKey>  scales;
};

struct AnimationClip {
    std::string            name;
    float                  duration = 0.0f;      // seconds
    std::vector<BoneTrack> tracks;
};

// --- Authored ragdoll (Phy_Bandit_PhysicsAsset.T3D) --------------------------
enum class RagdollShapeType : uint8_t { Box = 0, Capsule = 1, Sphere = 2 };
enum class RagdollMotion : uint8_t { Locked = 0, Limited = 1, Free = 2 };
enum class RagdollJointType : uint8_t { Spherical = 0, Hinge = 1 };

// Shape frame is bone-local. All distances are converted to engine metres by
// the T3D importer. Keeping every primitive matters for compound torso bodies.
struct RagdollShapeSpec {
    RagdollShapeType type = RagdollShapeType::Capsule;
    DirectX::XMFLOAT3 center = {};
    DirectX::XMFLOAT4 rotation = { 0,0,0,1 };
    float radius = 0.1f;
    float length = 0.2f;
    DirectX::XMFLOAT3 halfExtent = {};
};

struct RagdollBodySpec {
    std::string bone;
    std::vector<RagdollShapeSpec> shapes;
    float massFraction = 0.0f;
};

// Unreal constraint frame. Primary is twist axis, secondary is swing-1 axis,
// and their cross product is swing-2. Position is bone-local metres.
struct RagdollJointFrame {
    DirectX::XMFLOAT3 position = {};
    DirectX::XMFLOAT3 primary = { 1,0,0 };
    DirectX::XMFLOAT3 secondary = { 0,1,0 };
};

struct RagdollConstraintSpec {
    std::string boneA;
    std::string boneB;
    RagdollJointFrame frameA;
    RagdollJointFrame frameB;
    RagdollMotion swing1Motion = RagdollMotion::Limited;
    RagdollMotion swing2Motion = RagdollMotion::Limited;
    RagdollMotion twistMotion = RagdollMotion::Limited;
    float swing1Angle = DirectX::XM_PIDIV4;
    float swing2Angle = DirectX::XM_PIDIV4;
    float lowerTwistAngle = -DirectX::XM_PIDIV4;
    float upperTwistAngle = DirectX::XM_PIDIV4;
    RagdollJointType jointType = RagdollJointType::Spherical;
    // Hinge limits are measured from authored reference pose.
    float lowerHingeAngle = -0.0872665f;
    float upperHingeAngle = 2.5307274f;
    // 1 = Unreal swing-1/secondary axis, 2 = swing-2/cross axis.
    uint8_t hingeAxis = 1;
};

struct RagdollSpec {
    std::vector<RagdollBodySpec>       bodies;
    std::vector<RagdollConstraintSpec> constraints;
};
