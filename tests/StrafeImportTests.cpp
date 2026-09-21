// The Mixamo-rigged bandit is the same character re-rigged by Mixamo's
// auto-rigger, carrying four authored run cycles on the rig they were made
// for. Everything the engine does with it -- gun IK, the ragdoll spec, the
// directional blend space -- finds bones by their UE4 names, so the whole
// variant rests on scripts/mixamo-to-ue.py having renamed every bone that
// matters. That is a build step outside the compiler's reach, which is what
// this test covers: the files on disk, as the game will read them.
#include "DirectionalLocomotion.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace DirectX;

static int g_failures = 0;
static void Check(bool ok, const std::string& message) {
    if (ok) return;
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
}

static XMFLOAT4X4 Matrix(const aiMatrix4x4& m) {
    return {m.a1,m.b1,m.c1,m.d1,m.a2,m.b2,m.c2,m.d2,
            m.a3,m.b3,m.c3,m.d3,m.a4,m.b4,m.c4,m.d4};
}

static void Bones(const aiNode* node, int parent, Skeleton& s) {
    const int id = static_cast<int>(s.names.size());
    s.names.push_back(node->mName.C_Str());
    s.index[s.names.back()] = id;
    s.parent.push_back(parent);
    s.localBind.push_back(Matrix(node->mTransformation));
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    s.offset.push_back(identity);
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        Bones(node->mChildren[i], id, s);
}

// Assimp splits a joint that carries a PreRotation into Translation,
// PreRotation and Rotation nodes around the named bone, so the animated
// channel is called "<bone>_$AssimpFbx$_Rotation" and resolves to no bone at
// all. The importer rebuilds the combined rotation as animated Rotation times
// the fixed PreRotation (SkinnedFBXImporter.cpp,
// AppendRetargetedSplitRotationTrack); reading the raw channel instead would
// drop the pre-rotation and bend every such joint the wrong way, so the test
// has to resolve these the same way the game does.
static bool ReadSplitRotation(const aiScene* scene, const aiNodeAnim& channel,
                              const Skeleton& skeleton, double rate,
                              AnimationClip& clip) {
    const std::string name = channel.mNodeName.C_Str();
    constexpr const char* suffix = "_$AssimpFbx$_Rotation";
    const size_t split = name.find(suffix);
    if (split == std::string::npos ||
        split + std::strlen(suffix) != name.size() ||
        channel.mNumRotationKeys == 0)
        return false;
    const int bone = skeleton.Find(name.substr(0, split));
    if (bone < 0) return false;

    const aiNode* rotation = scene->mRootNode->FindNode(channel.mNodeName);
    const aiNode* pre = rotation ? rotation->mParent : nullptr;
    if (!pre || std::string(pre->mName.C_Str()).find("_$AssimpFbx$_PreRotation")
                    == std::string::npos)
        return false;
    const XMFLOAT4X4 stored = Matrix(pre->mTransformation);
    XMVECTOR preScale, preRotation, preTranslation;
    if (!XMMatrixDecompose(&preScale, &preRotation, &preTranslation,
                           XMLoadFloat4x4(&stored)))
        return false;
    const XMMATRIX preMatrix =
        XMMatrixRotationQuaternion(XMQuaternionNormalize(preRotation));

    BoneTrack track;
    track.bone = bone;
    for (unsigned k = 0; k < channel.mNumRotationKeys; ++k) {
        const aiQuaternion& q = channel.mRotationKeys[k].mValue;
        XMVECTOR scale, combined, translation;
        if (!XMMatrixDecompose(&scale, &combined, &translation,
                XMMatrixRotationQuaternion(XMQuaternionNormalize(
                    XMVectorSet(q.x, q.y, q.z, q.w))) * preMatrix))
            continue;
        XMFLOAT4 value;
        XMStoreFloat4(&value, XMQuaternionNormalize(combined));
        track.rotations.push_back(
            {static_cast<float>(channel.mRotationKeys[k].mTime / rate), value});
    }
    if (track.rotations.empty()) return false;
    clip.tracks.push_back(std::move(track));
    return true;
}

// The clip as the importer builds it: channels resolved to bone ids by name.
static bool ReadClip(const aiScene* scene, const Skeleton& skeleton,
                     const std::string& name, AnimationClip& clip) {
    if (!scene || !scene->mNumAnimations) return false;
    const aiAnimation& a = *scene->mAnimations[0];
    const double rate = a.mTicksPerSecond ? a.mTicksPerSecond : 30;
    clip.name = name;
    clip.duration = static_cast<float>(a.mDuration / rate);
    for (unsigned c = 0; c < a.mNumChannels; ++c) {
        const auto& channel = *a.mChannels[c];
        const int bone = skeleton.Find(channel.mNodeName.C_Str());
        if (bone < 0) {
            ReadSplitRotation(scene, channel, skeleton, rate, clip);
            continue;
        }
        BoneTrack track;
        track.bone = bone;
        for (unsigned k = 0; k < channel.mNumPositionKeys; ++k) {
            const auto& key = channel.mPositionKeys[k];
            track.positions.push_back(
                {float(key.mTime / rate), {key.mValue.x, key.mValue.y, key.mValue.z}});
        }
        for (unsigned k = 0; k < channel.mNumRotationKeys; ++k) {
            const auto& key = channel.mRotationKeys[k];
            track.rotations.push_back(
                {float(key.mTime / rate),
                 {key.mValue.x, key.mValue.y, key.mValue.z, key.mValue.w}});
        }
        for (unsigned k = 0; k < channel.mNumScalingKeys; ++k) {
            const auto& key = channel.mScalingKeys[k];
            track.scales.push_back(
                {float(key.mTime / rate), {key.mValue.x, key.mValue.y, key.mValue.z}});
        }
        clip.tracks.push_back(std::move(track));
    }
    return !clip.tracks.empty();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: StrafeImportTests <repository root>\n";
        return 2;
    }
    const std::string root = argv[1];
    const std::string banditDir = root + "/Content/Models/MilitaryMercenaryBandit/";
    const std::string mixamoDir = banditDir + "Mixamo/";
    const std::string meshPath = mixamoDir + "SK_BanditMixamo.fbx";

    Assimp::Importer meshImporter;
    const aiScene* mesh = meshImporter.ReadFile(meshPath, 0);
    Check(mesh && mesh->mRootNode, "Load the Mixamo-rigged bandit mesh: " + meshPath);
    if (!mesh || !mesh->mRootNode) return 1;

    Skeleton skeleton;
    Bones(mesh->mRootNode, -1, skeleton);
    XMStoreFloat4x4(&skeleton.globalInverse,
        XMMatrixInverse(nullptr, XMLoadFloat4x4(&skeleton.localBind[0])));

    // 1. The rename reached every bone the engine asks for by name. These are
    // the names hardcoded across the gun layer, the locomotion bake and
    // Phy_Bandit_PhysicsAsset.T3D; a miss here is a silently limp enemy.
    for (const char* bone : {
            "pelvis", "spine_01", "spine_02", "spine_03", "neck_01", "head",
            "clavicle_l", "clavicle_r", "upperarm_l", "upperarm_r",
            "lowerarm_l", "lowerarm_r", "hand_l", "hand_r",
            "thigh_l", "thigh_r", "calf_l", "calf_r",
            "foot_l", "foot_r", "ball_l", "ball_r" })
        Check(skeleton.Find(bone) >= 0,
              std::string("Mesh skeleton resolves '") + bone + "'");

    // A half-converted file is worse than an unconverted one, because the
    // bones the engine does find make it look like it worked.
    for (const std::string& name : skeleton.names)
        Check(name.rfind("mixamorig:", 0) != 0,
              "Bone still carries the Mixamo prefix: " + name);

    // 2. Renaming the models must not have cost the mesh its skin binding.
    unsigned vertices = 0, skinned = 0;
    for (unsigned m = 0; m < mesh->mNumMeshes; ++m) {
        vertices += mesh->mMeshes[m]->mNumVertices;
        skinned += mesh->mMeshes[m]->mNumBones;
    }
    Check(vertices == 46428, "Mesh keeps its vertices (got " +
                             std::to_string(vertices) + ")");
    Check(skinned > 0, "Mesh keeps its skin clusters");
    Check(mesh->mNumMaterials == 3, "Mesh keeps its three material slots");

    // 3. Each cycle travels the way its slot expects. A planted foot does not
    // move with the body, the body moves over it, so the sweep of whichever
    // foot is lower points opposite to travel -- which is what
    // DirectionalLocomotion measures to place a cycle it was not told about.
    struct Cycle { const char* file; const char* name; int direction; };
    const Cycle cycles[] = {
        { "SK_BanditMixamo.fbx",        "SK_BanditMixamo", 0 },
        { "Animations/RunBackward.fbx", "RunBackward",     1 },
        { "Animations/RunLeft.fbx",     "RunLeft",         2 },
        { "Animations/RunRight.fbx",    "RunRight",        3 },
    };
    const char* kSlots[] = { "forward", "backward", "left", "right" };

    std::vector<AnimationClip> clips;
    std::vector<float> floors;
    for (const Cycle& cycle : cycles) {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(mixamoDir + cycle.file, 0);
        AnimationClip clip;
        if (!ReadClip(scene, skeleton, cycle.name, clip)) {
            Check(false, std::string("Read the ") + cycle.name + " cycle");
            continue;
        }
        Check(clip.duration > 0.0f, std::string(cycle.name) + " has a duration");

        const int measured = DirectionalLocomotion::MeasureDirection(skeleton, clip);
        Check(measured == cycle.direction,
              std::string(cycle.name) + " travels " + kSlots[cycle.direction] +
              " (measured " +
              (measured < 0 ? std::string("no travel") : kSlots[measured]) + ")");

        // The cycle is played back on the skeleton it was authored for, so its
        // bones are rigid. Only real joint pairs are measured: Assimp expands
        // a joint carrying a PreRotation into $AssimpFbx$ helper nodes that
        // sit between it and its parent, and the gap to one of those is not a
        // bone length at all.
        AnimationInstance instance;
        instance.Play(&clip);
        std::vector<XMFLOAT4X4> pose;
        auto span = [&](int a, int b) {
            return XMVectorGetX(XMVector3Length(
                XMLoadFloat4x4(&pose[b]).r[3] - XMLoadFloat4x4(&pose[a]).r[3]));
        };
        const char* limbs[][2] = {
            {"thigh_l", "calf_l"}, {"calf_l", "foot_l"},
            {"thigh_r", "calf_r"}, {"calf_r", "foot_r"},
            {"upperarm_l", "lowerarm_l"}, {"upperarm_r", "lowerarm_r"},
        };
        AnimationInstance rest;
        std::vector<XMFLOAT4X4> bind;
        rest.ComputeGlobalMatrices(skeleton, bind);
        pose = bind;
        float reference[6];
        for (int i = 0; i < 6; ++i)
            reference[i] = span(skeleton.Find(limbs[i][0]), skeleton.Find(limbs[i][1]));

        // Up is a property of the rig: this one stands along +Y, the UE4
        // bandit along +Z, so ask the rest pose which axis the pelvis rises
        // along rather than assuming one.
        XMFLOAT3 stance;
        XMStoreFloat3(&stance, XMVectorAbs(
            XMLoadFloat4x4(&bind[skeleton.Find("pelvis")]).r[3] -
            XMLoadFloat4x4(&bind[skeleton.Find("foot_l")]).r[3]));
        const int up = stance.z >= stance.x && stance.z >= stance.y ? 2
                     : stance.y >= stance.x ? 1 : 0;

        bool rigid = true, finite = true;
        float lowest = FLT_MAX;
        for (int t = 0; t <= 32; ++t) {
            instance.time = clip.duration * t / 32;
            instance.ComputeGlobalMatrices(skeleton, pose);
            for (size_t b = 0; b < skeleton.BoneCount(); ++b) {
                const float* m = &pose[b]._11;
                for (int e = 0; e < 16; ++e) finite = finite && std::isfinite(m[e]);
            }
            for (int i = 0; i < 6; ++i) {
                const float length =
                    span(skeleton.Find(limbs[i][0]), skeleton.Find(limbs[i][1]));
                rigid = rigid && std::abs(length - reference[i]) < 1e-2f;
            }
            for (const char* foot : {"foot_l", "foot_r"}) {
                XMFLOAT3 p;
                XMStoreFloat3(&p, XMLoadFloat4x4(&pose[skeleton.Find(foot)]).r[3]);
                lowest = (std::min)(lowest, (&p.x)[up]);
            }
        }
        Check(finite, std::string(cycle.name) + " poses are finite");
        Check(rigid, std::string(cycle.name) + " preserves bone lengths");

        // Every cycle has to stand on the same ground, or the body would pop
        // in height as the blend space crossfades between them.
        floors.push_back(lowest);

        clips.push_back(std::move(clip));
    }

    // A body mid-blend stands between two cycles, so they have to agree on
    // where the ground is; an outlier would show up as the enemy bobbing as
    // it changes direction.
    for (size_t i = 1; i < floors.size(); ++i)
        Check(std::abs(floors[i] - floors[0]) < 2.0f,
              std::string(cycles[i].name) + " plants its feet at the same height"
              " as " + cycles[0].name + " (" + std::to_string(floors[i]) +
              " vs " + std::to_string(floors[0]) + ")");

    if (g_failures == 0)
        std::cout << "StrafeImportTests: " << clips.size()
                  << " authored cycles verified on the Mixamo rig\n";
    return g_failures == 0 ? 0 : 1;
}
