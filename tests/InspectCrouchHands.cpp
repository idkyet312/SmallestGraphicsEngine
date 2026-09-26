// Throwaway diagnostic: sample a clip's own node hierarchy at t=0 and print
// hand_l/hand_r world position relative to spine_03, plus the distance
// between the two hands. Used to check whether a downloaded clip actually
// holds a rifle pose (hands together, near the chest) versus an unarmed or
// pistol pose (hands apart, away from the body) before wiring it in as a
// crouch pose for a rig that is always holding a rifle.
//
// Samples straight off the clip's own skeleton (whatever the FBX ships,
// mixamorig-prefixed or already renamed) rather than the game's bandit mesh,
// since a mismatch there is a separate, already-checked question
// (InspectClipBones) -- this tool only asks "what pose is authored here".
//
// Usage: InspectCrouchHands <clip.fbx> [clip.fbx ...]

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

struct Mat4 {
    float m[4][4];
    static Mat4 Identity() {
        Mat4 r{};
        for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
        return r;
    }
    static Mat4 FromAssimp(const aiMatrix4x4& a) {
        Mat4 r;
        // aiMatrix4x4 is row-major with translation in column 3 (a.a4 etc);
        // keep the same layout here and multiply row-vector * matrix so
        // world = local * parentWorld, matching the engine's own convention
        // (see AnimationRuntime.h's header comment).
        r.m[0][0] = a.a1; r.m[0][1] = a.b1; r.m[0][2] = a.c1; r.m[0][3] = a.d1;
        r.m[1][0] = a.a2; r.m[1][1] = a.b2; r.m[1][2] = a.c2; r.m[1][3] = a.d2;
        r.m[2][0] = a.a3; r.m[2][1] = a.b3; r.m[2][2] = a.c3; r.m[2][3] = a.d3;
        r.m[3][0] = a.a4; r.m[3][1] = a.b4; r.m[3][2] = a.c4; r.m[3][3] = a.d4;
        return r;
    }
    Mat4 Mul(const Mat4& b) const {
        Mat4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k) s += m[i][k] * b.m[k][j];
                r.m[i][j] = s;
            }
        return r;
    }
    void Position(float out[3]) const {
        out[0] = m[3][0]; out[1] = m[3][1]; out[2] = m[3][2];
    }
};

// Builds the local transform for `node` at time 0 of `anim`, or the node's
// own bind-pose transform if nothing animates it. When `outRotation` is
// given, also hands back the raw quaternion (identity if nothing animates
// the node) so a caller can compare rotations directly rather than only
// world positions.
static Mat4 LocalAtZero(const aiNode* node, const aiAnimation* anim,
                        aiQuaternion* outRotation = nullptr) {
    if (anim) {
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim* ch = anim->mChannels[c];
            if (std::strcmp(ch->mNodeName.C_Str(), node->mName.C_Str()) != 0)
                continue;
            aiVector3D pos = ch->mNumPositionKeys > 0
                ? ch->mPositionKeys[0].mValue : aiVector3D(0, 0, 0);
            aiQuaternion rot = ch->mNumRotationKeys > 0
                ? ch->mRotationKeys[0].mValue : aiQuaternion(1, 0, 0, 0);
            aiVector3D scl = ch->mNumScalingKeys > 0
                ? ch->mScalingKeys[0].mValue : aiVector3D(1, 1, 1);
            if (outRotation) *outRotation = rot;
            aiMatrix4x4 m;
            aiMatrix4x4::Scaling(scl, m);
            aiMatrix4x4 r(rot.GetMatrix());
            m = r * m;
            aiMatrix4x4 t;
            aiMatrix4x4::Translation(pos, t);
            m = t * m;
            return Mat4::FromAssimp(m);
        }
    }
    if (outRotation) *outRotation = aiQuaternion(1, 0, 0, 0);
    return Mat4::FromAssimp(node->mTransformation);
}

// Degrees, Euler XYZ, purely for a human-readable print -- not used for any
// composition.
static void QuatToEulerDegrees(const aiQuaternion& q, float out[3]) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float sinr = 2.0f * (w * x + y * z);
    const float cosr = 1.0f - 2.0f * (x * x + y * y);
    out[0] = std::atan2(sinr, cosr) * 57.29578f;
    float sinp = 2.0f * (w * y - z * x);
    sinp = sinp > 1.0f ? 1.0f : (sinp < -1.0f ? -1.0f : sinp);
    out[1] = std::asin(sinp) * 57.29578f;
    const float siny = 2.0f * (w * z + x * y);
    const float cosy = 1.0f - 2.0f * (y * y + z * z);
    out[2] = std::atan2(siny, cosy) * 57.29578f;
}

static const aiNode* FindNode(const aiNode* node, const std::string& suffix) {
    if (!node) return nullptr;
    const std::string name = node->mName.C_Str();
    // Case-insensitive suffix match so both "hand_l" (UE-renamed) and
    // "mixamorig12:LeftHand" (raw Mixamo) are found by asking for "hand_l"/
    // "hand_r" or "lefthand"/"righthand" respectively.
    if (name.size() >= suffix.size()) {
        std::string tail = name.substr(name.size() - suffix.size());
        std::string tailLower = tail, suffixLower = suffix;
        for (auto& c : tailLower) c = (char)std::tolower((unsigned char)c);
        for (auto& c : suffixLower) c = (char)std::tolower((unsigned char)c);
        if (tailLower == suffixLower) return node;
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        if (const aiNode* found = FindNode(node->mChildren[i], suffix)) return found;
    return nullptr;
}

// World transform of `target`, sampling every ancestor (and target itself) at
// time 0 of `anim`.
static Mat4 WorldAtZero(const aiNode* target, const aiAnimation* anim) {
    if (!target) return Mat4::Identity();
    if (!target->mParent) return LocalAtZero(target, anim);
    return WorldAtZero(target->mParent, anim).Mul(LocalAtZero(target, anim));
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: InspectCrouchHands <clip.fbx> [clip.fbx ...]\n");
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(argv[i], 0);
        if (!scene) {
            std::printf("%s FAILED: %s\n", argv[i], importer.GetErrorString());
            continue;
        }
        const aiAnimation* anim = scene->mNumAnimations > 0 ? scene->mAnimations[0] : nullptr;
        const aiNode* handL = FindNode(scene->mRootNode, "hand_l");
        if (!handL) handL = FindNode(scene->mRootNode, "lefthand");
        const aiNode* handR = FindNode(scene->mRootNode, "hand_r");
        if (!handR) handR = FindNode(scene->mRootNode, "righthand");
        const aiNode* spine3 = FindNode(scene->mRootNode, "spine_03");
        if (!spine3) spine3 = FindNode(scene->mRootNode, "spine2");
        const aiNode* pelvis = FindNode(scene->mRootNode, "pelvis");
        if (!pelvis) pelvis = FindNode(scene->mRootNode, "hips");

        std::printf("\n%s\n", argv[i]);
        if (!handL || !handR || !spine3) {
            std::printf("  MISSING bone(s): handL=%p handR=%p spine3=%p\n",
                        (const void*)handL, (const void*)handR, (const void*)spine3);
            continue;
        }
        const Mat4 handLWorld = WorldAtZero(handL, anim);
        const Mat4 handRWorld = WorldAtZero(handR, anim);
        const Mat4 spineWorld = WorldAtZero(spine3, anim);
        float hl[3], hr[3], sp[3];
        handLWorld.Position(hl);
        handRWorld.Position(hr);
        spineWorld.Position(sp);
        const float relL[3] = { hl[0] - sp[0], hl[1] - sp[1], hl[2] - sp[2] };
        const float relR[3] = { hr[0] - sp[0], hr[1] - sp[1], hr[2] - sp[2] };
        const float dx = hl[0] - hr[0], dy = hl[1] - hr[1], dz = hl[2] - hr[2];
        const float handDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
        std::printf("  hand_l rel spine_03 = (%.2f, %.2f, %.2f)\n",
                    relL[0], relL[1], relL[2]);
        std::printf("  hand_r rel spine_03 = (%.2f, %.2f, %.2f)\n",
                    relR[0], relR[1], relR[2]);
        std::printf("  hand_l-to-hand_r distance = %.2f\n", handDistance);
        if (pelvis) {
            aiQuaternion pelvisRot;
            LocalAtZero(pelvis, anim, &pelvisRot);
            float euler[3];
            QuatToEulerDegrees(pelvisRot, euler);
            std::printf("  pelvis local rotation (deg XYZ) = (%.2f, %.2f, %.2f)\n",
                        euler[0], euler[1], euler[2]);
        }
    }
    return 0;
}
