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
// One rigid body attached to a skeleton bone. Dimensions already scaled to
// engine metres. Center/rotation are in that bone's local space; the builder
// composes them with the bind-pose global transform of the bone to place the
// body in world space at spawn.
struct RagdollBodySpec {
    std::string bone;
    uint8_t     shape = 1;                  // 1 capsule (Sphyl), 0 box
    DirectX::XMFLOAT3 center = {};          // bone-local, native skeleton units (UE cm)
    DirectX::XMFLOAT4 rotation = { 0,0,0,1 }; // bone-local quaternion
    float       radius = 0.1f;              // capsule radius (metres)
    float       length = 0.2f;              // capsule segment length (metres)
    DirectX::XMFLOAT3 halfExtent = {};      // box half-extents (metres)
};

// One constraint joining two bodies (referenced by bone name).
struct RagdollConstraintSpec {
    std::string boneA;
    std::string boneB;
    float coneAngle = DirectX::XM_PIDIV4;
    float twistAngle = DirectX::XM_PIDIV4;
};

struct RagdollSpec {
    std::vector<RagdollBodySpec>       bodies;
    std::vector<RagdollConstraintSpec> constraints;
};
