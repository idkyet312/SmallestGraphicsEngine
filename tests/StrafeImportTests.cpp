// The Mixamo-rigged bandit is the same character re-rigged by Mixamo's
// auto-rigger, carrying four authored run cycles on the rig they were made
// for. Everything the engine does with it -- gun IK, the ragdoll spec, the
// directional blend space -- finds bones by their UE4 names, so the whole
// variant rests on scripts/mixamo-to-ue.py having renamed every bone that
// matters. That is a build step outside the compiler's reach, which is what
// this test covers: the files on disk, as the game will read them.
#include "DirectionalLocomotion.h"
#include "RagdollRigFit.h"
#include "T3DPhysicsAsset.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <algorithm>
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

// How far the planted foot travels over a whole clip, summed the way
// DirectionalLocomotion::MeasureDirection accumulates it. A gait sweeps a
// stride; a clip animated on the spot sweeps only its own jitter.
static float FootSweep(const Skeleton& skeleton, const AnimationClip& clip,
                       int up) {
    if (clip.duration <= 0.0f) return 0.0f;
    const int feet[2] = { skeleton.Find("foot_l"), skeleton.Find("foot_r") };
    if (feet[0] < 0 || feet[1] < 0) return 0.0f;
    AnimationInstance instance;
    instance.Play(&clip);
    std::vector<XMFLOAT4X4> previous, current;
    instance.ComputeGlobalMatrices(skeleton, previous);
    XMFLOAT3 total{};
    for (int t = 1; t <= 64; ++t) {
        instance.time = clip.duration * t / 64.0f;
        instance.ComputeGlobalMatrices(skeleton, current);
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
    total.x = std::abs(total.x);
    total.y = std::abs(total.y);
    total.z = std::abs(total.z);
    return (std::max)({ total.x, total.y, total.z });
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

    // The rifle idle ships from the same Mixamo download path as the cycles
    // and is played on this rig too, so it has to survive the bone rename and
    // stand on the cycles' ground. It travels nowhere by design, so it is
    // checked here rather than in the direction loop above.
    {
        Assimp::Importer importer;
        const aiScene* scene =
            importer.ReadFile(mixamoDir + "Animations/RifleAimingIdle.fbx", 0);
        AnimationClip idle;
        if (!ReadClip(scene, skeleton, "Idle", idle)) {
            Check(false, "Read the rifle idle");
        } else {
            Check(idle.duration > 0.0f, "Rifle idle has a duration");
            AnimationInstance instance;
            instance.Play(&idle);
            std::vector<XMFLOAT4X4> pose;
            AnimationInstance rest;
            std::vector<XMFLOAT4X4> bind;
            rest.ComputeGlobalMatrices(skeleton, bind);
            XMFLOAT3 stance;
            XMStoreFloat3(&stance, XMVectorAbs(
                XMLoadFloat4x4(&bind[skeleton.Find("pelvis")]).r[3] -
                XMLoadFloat4x4(&bind[skeleton.Find("foot_l")]).r[3]));
            const int up = stance.z >= stance.x && stance.z >= stance.y ? 2
                         : stance.y >= stance.x ? 1 : 0;
            bool finite = true;
            float lowest = FLT_MAX;
            for (int t = 0; t <= 32; ++t) {
                instance.time = idle.duration * t / 32;
                instance.ComputeGlobalMatrices(skeleton, pose);
                for (size_t b = 0; b < skeleton.BoneCount(); ++b) {
                    const float* m = &pose[b]._11;
                    for (int e = 0; e < 16; ++e)
                        finite = finite && std::isfinite(m[e]);
                }
                for (const char* foot : {"foot_l", "foot_r"}) {
                    XMFLOAT3 p;
                    XMStoreFloat3(&p,
                        XMLoadFloat4x4(&pose[skeleton.Find(foot)]).r[3]);
                    lowest = (std::min)(lowest, (&p.x)[up]);
                }
            }
            Check(finite, "Rifle idle poses are finite");

            // The idle holds its ground while a cycle carries the body a
            // stride. MeasureDirection's own "no travel" epsilon is a fixed
            // 1e-3 in rig units and a 3.1s idle's foot jitter exceeds it, so
            // compare against what a real cycle sweeps instead: measured,
            // this idle sweeps 0.009 against 108-138 for the four runs.
            float cycleSweep = 0.0f;
            for (const AnimationClip& cycle : clips)
                cycleSweep = (std::max)(cycleSweep, FootSweep(skeleton, cycle, up));
            const float idleSweep = FootSweep(skeleton, idle, up);
            Check(cycleSweep > 0.0f && idleSweep < cycleSweep * 0.05f,
                  "Rifle idle stands in place (sweeps " +
                  std::to_string(idleSweep) + " against " +
                  std::to_string(cycleSweep) + " for a run cycle)");
            // Stopping crossfades the idle against a run slot, so a
            // disagreeing floor would drop the enemy as it comes to rest.
            Check(!floors.empty() && std::abs(lowest - floors[0]) < 2.0f,
                  "Rifle idle plants its feet at the cycles' height (" +
                  std::to_string(lowest) + " vs " +
                  std::to_string(floors.empty() ? 0.0f : floors[0]) + ")");
        }
    }

    // Verify the authored-only production path with the same split-rotation
    // importer used above. Sources remain intact while eight slots are made.
    std::vector<AnimationClip> authored;
    for (int i = 0; i < 4; ++i) {
        authored.push_back(clips[i]);
        authored.back().name = std::string("RunSource") + std::to_string(i);
    }
    const std::vector<DirectionalLocomotion::AuthoredCycle> spec = {
        {"RunSource0", true, false, 0}, {"RunSource1", true, false, 1},
        {"RunSource2", true, false, 2}, {"RunSource3", true, false, 3}};
    Check(DirectionalLocomotion::BakeAuthored(skeleton, authored, spec),
          "Bake authored-only Mixamo cycles");
    Check(authored.size() == 12, "Retain four sources and add eight slots");
    for (int d = 0; d < 4; ++d) {
        Check(authored[d].name == std::string("RunSource") + std::to_string(d),
              "Preserve authored source name");
        const AnimationClip* walk = nullptr; const AnimationClip* run = nullptr;
        for (const auto& clip : authored) {
            if (clip.name == DirectionalLocomotion::Names[d]) walk = &clip;
            if (clip.name == DirectionalLocomotion::Names[4 + d]) run = &clip;
        }
        Check(walk && run, "Generate every authored walk/run slot");
        if (!walk || !run) continue;
        Check(std::abs(walk->duration / run->duration - 1.5f) < 1e-4f,
              "Walk is retimed from authored run");
        Check(DirectionalLocomotion::MeasureDirection(skeleton, *walk) == d &&
              DirectionalLocomotion::MeasureDirection(skeleton, *run) == d,
              "Authored directions survive resampling");
        AnimationInstance instance; instance.Play(run);
        std::vector<XMFLOAT4X4> pose; float firstY = 0.0f, lowestY = FLT_MAX;
        for (int sample = 0; sample <= 32; ++sample) {
            instance.time = run->duration * sample / 32.0f;
            instance.ComputeGlobalMatrices(skeleton, pose);
            const float y = XMVectorGetY(XMLoadFloat4x4(
                &pose[skeleton.Find("foot_l")]).r[3]);
            if (sample == 0) firstY = y;
            lowestY = (std::min)(lowestY, y);
        }
        Check(std::abs(firstY - lowestY) < 1e-3f,
              "Phase starts at the Y-up foot plant");
    }

    AnimationClip idle; idle.name = "Idle"; idle.duration = 1.0f;
    authored.push_back(idle);
    LocomotionBlendSpace blend;
    Check(blend.Initialize(skeleton, authored, false),
          "Initialize authored-only blend space");
    AnimationInstance blended;
    const char* rigidPairs[][2] = {{"thigh_l", "calf_l"}, {"calf_l", "foot_l"},
        {"thigh_r", "calf_r"}, {"calf_r", "foot_r"}};
    AnimationInstance rest; std::vector<XMFLOAT4X4> bind;
    rest.ComputeGlobalMatrices(skeleton, bind);
    float rigidReference[4] = {};
    for (int i = 0; i < 4; ++i)
        rigidReference[i] = XMVectorGetX(XMVector3Length(
            XMLoadFloat4x4(&bind[skeleton.Find(rigidPairs[i][1])]).r[3] -
            XMLoadFloat4x4(&bind[skeleton.Find(rigidPairs[i][0])]).r[3]));
    for (int sample = 0; sample < 32; ++sample) {
        const float a = sample * XM_2PI / 32.0f;
        blended.Play(blend.Update(1.0f / 60.0f, std::sin(a) * 2.97f,
                                  std::cos(a) * 2.97f, 1.8f));
        std::vector<XMFLOAT4X4> pose; blended.ComputeGlobalMatrices(skeleton, pose);
        for (const auto& matrix : pose)
            for (const auto& row : matrix.m)
                for (float value : row) Check(std::isfinite(value), "Finite blended pose");
        for (int i = 0; i < 4; ++i) {
            const float length = XMVectorGetX(XMVector3Length(
                XMLoadFloat4x4(&pose[skeleton.Find(rigidPairs[i][1])]).r[3] -
                XMLoadFloat4x4(&pose[skeleton.Find(rigidPairs[i][0])]).r[3]));
            Check(std::abs(length - rigidReference[i]) < 1e-2f,
                  "Blended authored bone lengths remain rigid");
        }
    }
    auto invalid = authored; invalid.resize(4); const auto before = invalid;
    Check(!DirectionalLocomotion::BakeAuthored(skeleton, invalid,
        {{"RunSource0", true, false, 0}, {"RunSource1", true, false, 1}}),
          "Reject missing authored directions");
    Check(invalid.size() == before.size() && invalid[0].name == before[0].name,
          "Invalid bake leaves sources unchanged");

    Assimp::Importer referenceImporter;
    const aiScene* referenceMesh = referenceImporter.ReadFile(banditDir + "SK_Bandit.FBX", 0);
    Check(referenceMesh && referenceMesh->mRootNode, "Load physics reference rig");
    if (referenceMesh && referenceMesh->mRootNode) {
        Skeleton reference;
        Bones(referenceMesh->mRootNode, -1, reference);
        XMStoreFloat4x4(&reference.globalInverse,
            XMMatrixInverse(nullptr, XMLoadFloat4x4(&reference.localBind[0])));
        const auto physics = T3DPhysicsAsset::Load(banditDir + "Phy_Bandit_PhysicsAsset.T3D");
        const auto fitted = RagdollRigFit::Fit(reference, skeleton, physics,
                                              XMMatrixRotationX(-XM_PIDIV2));
        Check(!fitted.bodies.empty() && fitted.bodies.size() == physics.bodies.size(),
              "Fit every authored physics body to the new rig");
        AnimationInstance rest;
        std::vector<XMFLOAT4X4> bind;
        rest.ComputeGlobalMatrices(skeleton, bind);
        for (const auto& joint : fitted.constraints) {
            const auto a = XMLoadFloat4x4(&bind[skeleton.Find(joint.boneA)]);
            const auto b = XMLoadFloat4x4(&bind[skeleton.Find(joint.boneB)]);
            const auto pa = XMVector3TransformCoord(XMLoadFloat3(&joint.frameA.position) * 100, a);
            const auto pb = XMVector3TransformCoord(XMLoadFloat3(&joint.frameB.position) * 100, b);
            Check(XMVectorGetX(XMVector3Length(pa - pb)) < 0.01f,
                  "Fitted constraint anchors coincide in centimetre bind space");
            const auto axisA = XMVector3Normalize(XMVector3TransformNormal(
                XMLoadFloat3(&joint.frameA.primary), a));
            const auto axisB = XMVector3Normalize(XMVector3TransformNormal(
                XMLoadFloat3(&joint.frameB.primary), b));
            Check(XMVectorGetX(XMVector3Dot(axisA, axisB)) > 0.999f,
                  "Fitted joint frames agree at rest");
        }
        for (const auto& body : fitted.bodies) for (const auto& shape : body.shapes) {
            const float distance = XMVectorGetX(XMVector3Length(XMLoadFloat3(&shape.center)));
            Check(std::isfinite(distance) && distance < 0.8f && shape.length < 1.0f,
                  "Fitted shapes retain human-scale metre dimensions");
        }
        Check(RagdollRigFit::Fit({}, skeleton, physics, XMMatrixIdentity()).bodies.empty(),
              "Missing reference rig cannot silently reuse incompatible physics");
    }

    if (g_failures == 0)
        std::cout << "StrafeImportTests: " << clips.size()
                  << " authored cycles verified on the Mixamo rig\n";
    return g_failures == 0 ? 0 : 1;
}
