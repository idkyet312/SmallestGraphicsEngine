#include "SkinnedFBXImporter.h"
#include "CookedAssetLoader.h"
#include "GLBImporter.h"
#include "StaticBufferDX12.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
// AnimationInstance: a rebased clip is replayed here to find its floor.
#include "AnimationRuntime.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>

using namespace DirectX;
namespace fs = std::filesystem;

namespace {

// assimp aiMatrix4x4 is row-major with row vectors (translation in a4/b4/c4).
// XMFLOAT4X4 is also row-major (m[row][col]); copying element-for-element yields
// a matrix the engine's row-vector math (XMMatrixMultiply left-to-right) uses
// directly. assimp exposes a[row][col] as m.aRC (m.a1 = row0col0). assimp uses
// column-vector math (result = M * v) while DirectXMath uses row-vector
// (result = v * M), so we TRANSPOSE here: assimp row R becomes XM column R,
// i.e. XM row r column c = assimp[c][r]. Then every product below is written in
// row-vector (child * parent) order and consumed directly by the shader's
// mul(pos, matrix). Keeping this one convention everywhere is what stops the
// skinned mesh from exploding.
XMFLOAT4X4 ToXM(const aiMatrix4x4& m) {
    return XMFLOAT4X4(
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4);
}

// Recursively assign every node in the hierarchy a bone id in parent-before-
// child order so a single forward pass computes globals. Fills names/parent/
// localBind/index; offset is filled later from the mesh's mBones.
void BuildSkeleton(const aiNode* node, int parentId, Skeleton& skel) {
    const int id = (int)skel.names.size();
    skel.names.push_back(node->mName.C_Str());
    skel.parent.push_back(parentId);
    skel.localBind.push_back(ToXM(node->mTransformation));
    skel.offset.push_back([] { XMFLOAT4X4 m; XMStoreFloat4x4(&m, XMMatrixIdentity()); return m; }());
    skel.index[node->mName.C_Str()] = id;
    for (unsigned c = 0; c < node->mNumChildren; ++c)
        BuildSkeleton(node->mChildren[c], id, skel);
}

XMFLOAT3 KeyVec(const aiVector3D& v, float s = 1.0f) { return XMFLOAT3(v.x * s, v.y * s, v.z * s); }

// Animation-only Mixamo FBXs often split a joint into Translation,
// PreRotation, Rotation, and the named bone while the mesh FBX has one combined
// node. Rebuild the combined rotation as animated Rotation * fixed PreRotation,
// while keeping translation and scale from the mesh skeleton. Mapping the raw
// Rotation channel directly loses both pre-rotation and joint translation,
// which is what previously exploded the arms across the screen.
bool AppendRetargetedSplitRotationTrack(const aiScene* scene,
                                        const aiNodeAnim* channel,
                                        const Skeleton& skel,
                                        double ticksPerSecond,
                                        AnimationClip& clip) {
    const std::string channelName = channel->mNodeName.C_Str();
    constexpr const char* rotationSuffix = "_$AssimpFbx$_Rotation";
    const size_t split = channelName.find(rotationSuffix);
    if (split == std::string::npos ||
        split + std::strlen(rotationSuffix) != channelName.size())
        return false;

    const std::string boneName = channelName.substr(0, split);
    if (channel->mNumRotationKeys == 0) return false;
    const int bone = skel.Find(boneName);
    if (bone < 0 || static_cast<size_t>(bone) >= skel.localBind.size())
        return false;

    const aiNode* rotationNode = scene && scene->mRootNode
        ? scene->mRootNode->FindNode(channel->mNodeName) : nullptr;
    const aiNode* preRotationNode =
        rotationNode ? rotationNode->mParent : nullptr;
    if (!preRotationNode ||
        std::string(preRotationNode->mName.C_Str()).find(
            "_$AssimpFbx$_PreRotation") == std::string::npos)
        return false;

    const XMFLOAT4X4 preTransform =
        ToXM(preRotationNode->mTransformation);
    XMVECTOR preScale, preRotation, preTranslation;
    if (!XMMatrixDecompose(
            &preScale, &preRotation, &preTranslation,
            XMLoadFloat4x4(&preTransform)))
        return false;
    const XMMATRIX preRotationMatrix =
        XMMatrixRotationQuaternion(XMQuaternionNormalize(preRotation));

    BoneTrack track;
    track.bone = bone;
    track.rotations.reserve(channel->mNumRotationKeys);
    for (unsigned key = 0; key < channel->mNumRotationKeys; ++key) {
        const aiQuaternion& value = channel->mRotationKeys[key].mValue;
        const XMVECTOR animatedRotation = XMQuaternionNormalize(
            XMVectorSet(value.x, value.y, value.z, value.w));
        const XMMATRIX retargeted =
            XMMatrixRotationQuaternion(animatedRotation) *
            preRotationMatrix;
        XMVECTOR scale, rotation, translation;
        if (!XMMatrixDecompose(&scale, &rotation, &translation, retargeted))
            continue;
        XMFLOAT4 result;
        XMStoreFloat4(&result, XMQuaternionNormalize(rotation));
        track.rotations.push_back({
            static_cast<float>(
                channel->mRotationKeys[key].mTime / ticksPerSecond),
            result
        });
    }
    if (track.rotations.empty()) return false;
    clip.tracks.push_back(std::move(track));
    return true;
}

// A clip converted from another rig can share this skeleton's bone names and
// still be a foreign rig underneath: these Mixamo strafes run their bones down
// +Y where the bandit's run down -X, carry their own bone lengths, and are not
// mirrored left-to-right where the bandit is. Copying the keys across therefore
// fails three ways at once -- the position keys resize the skeleton inside the
// mesh, and no single rotation delta can serve a mirrored pair, so both legs
// end up swinging the same way and the strafe collapses into a shuffle.
//
// A DIRECTION survives all of that, because it has no axis convention and no
// handedness. So drive each leg segment by aiming it along the direction its
// counterpart points in the clip, in model space, and let the skeleton keep its
// own bone lengths, stance and mirroring:
//
//     have = normalize(childGlobal - boneGlobal)      on this skeleton
//     want = normalize(childGlobal - boneGlobal)      in the clip
//     rotate the bone about its own origin by the swing taking have to want
//
// The body is then dropped so its lowest foot rests on the bind floor, which is
// what keeps a cycle authored for other legs from hovering or sinking.
struct ClipRig {
    std::vector<std::string> names;
    std::vector<int> parent;
    std::vector<aiMatrix4x4> bind;
    std::vector<const aiNodeAnim*> channel;
};

void BuildClipRig(const aiNode* node, int parent, ClipRig& rig) {
    const int id = static_cast<int>(rig.names.size());
    rig.names.push_back(node->mName.C_Str());
    rig.parent.push_back(parent);
    rig.bind.push_back(node->mTransformation);
    rig.channel.push_back(nullptr);
    for (unsigned c = 0; c < node->mNumChildren; ++c)
        BuildClipRig(node->mChildren[c], id, rig);
}

// The clip rig's parent-local matrix for one node at `time` ticks.
XMMATRIX ClipLocal(const ClipRig& rig, int node, double time) {
    const XMFLOAT4X4 store = ToXM(rig.bind[node]);
    const XMMATRIX bind = XMLoadFloat4x4(&store);
    const aiNodeAnim* channel = rig.channel[node];
    if (!channel || channel->mNumRotationKeys == 0) return bind;
    XMVECTOR scale, rotation, translation;
    if (!XMMatrixDecompose(&scale, &rotation, &translation, bind)) {
        scale = XMVectorSplatOne();
        rotation = XMQuaternionIdentity();
        translation = XMVectorZero();
    }
    unsigned key = 0;
    while (key + 1 < channel->mNumRotationKeys &&
           channel->mRotationKeys[key + 1].mTime <= time) ++key;
    const unsigned next = (key + 1 < channel->mNumRotationKeys) ? key + 1 : key;
    const double span =
        channel->mRotationKeys[next].mTime - channel->mRotationKeys[key].mTime;
    const float alpha = span > 1e-9
        ? static_cast<float>((time - channel->mRotationKeys[key].mTime) / span) : 0.0f;
    auto load = [&](unsigned k) {
        const aiQuaternion& q = channel->mRotationKeys[k].mValue;
        return XMQuaternionNormalize(XMVectorSet(q.x, q.y, q.z, q.w));
    };
    rotation = XMQuaternionSlerp(load(key), load(next), alpha);
    if (channel->mNumPositionKeys) {
        unsigned p = 0;
        while (p + 1 < channel->mNumPositionKeys &&
               channel->mPositionKeys[p + 1].mTime <= time) ++p;
        const aiVector3D& value = channel->mPositionKeys[p].mValue;
        translation = XMVectorSet(value.x, value.y, value.z, 0.0f);
    }
    return XMMatrixAffineTransformation(
        scale, XMVectorZero(), rotation, translation);
}

// The shortest rotation taking one direction onto another.
XMMATRIX SwingBetween(FXMVECTOR from, FXMVECTOR to) {
    const XMVECTOR a = XMVector3Normalize(from), b = XMVector3Normalize(to);
    const float dot = XMVectorGetX(XMVector3Dot(a, b));
    if (dot > 0.9999f) return XMMatrixIdentity();
    if (dot < -0.9999f) {
        XMVECTOR axis = XMVector3Cross(a, XMVectorSet(1, 0, 0, 0));
        if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-6f)
            axis = XMVector3Cross(a, XMVectorSet(0, 1, 0, 0));
        return XMMatrixRotationAxis(XMVector3Normalize(axis), 3.14159265f);
    }
    return XMMatrixRotationAxis(
        XMVector3Normalize(XMVector3Cross(a, b)), std::acos(dot));
}

// The limb segments a strafe needs. Everything else keeps its bind pose, which
// is what the upper-body gun layer expects to sit on.
struct LimbSegment { const char* bone; const char* child; };
const LimbSegment kLegSegments[] = {
    { "thigh_l", "calf_l" }, { "calf_l", "foot_l" }, { "foot_l", "ball_l" },
    { "thigh_r", "calf_r" }, { "calf_r", "foot_r" }, { "foot_r", "ball_r" },
};

// Append every AnimStack in `scene`, direction-matched onto `skel` as above.
void AppendRebasedClips(const aiScene* scene, const Skeleton& skel,
                        std::vector<AnimationClip>& clips) {
    const size_t bones = skel.BoneCount();
    ClipRig rig;
    BuildClipRig(scene->mRootNode, -1, rig);
    const int pelvis = skel.Find("pelvis");
    const int feet[2] = { skel.Find("foot_l"), skel.Find("foot_r") };
    if (pelvis < 0 || feet[0] < 0 || feet[1] < 0) return;

    // This skeleton's own rest, and the floor its feet stand on. Measured the
    // way the runtime will measure it -- ComputeGlobalMatrices divides the
    // scene root back out, so a floor taken from the raw chain sits in a
    // different space and the drop below would aim at the wrong height.
    AnimationInstance rest;
    std::vector<XMFLOAT4X4> bindGlobal;
    rest.ComputeGlobalMatrices(skel, bindGlobal);
    const float floor = (std::min)(bindGlobal[feet[0]]._43,
                                   bindGlobal[feet[1]]._43);

    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation* anim = scene->mAnimations[a];
        if (anim->mDuration <= 0.0) continue;
        const double tps = anim->mTicksPerSecond != 0.0 ? anim->mTicksPerSecond : 30.0;
        std::fill(rig.channel.begin(), rig.channel.end(), nullptr);
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            const std::string name = anim->mChannels[c]->mNodeName.C_Str();
            for (size_t node = 0; node < rig.names.size(); ++node)
                if (rig.names[node] == name) {
                    rig.channel[node] = anim->mChannels[c];
                    break;
                }
        }

        AnimationClip clip;
        clip.name = anim->mName.length ? anim->mName.C_Str() : ("anim" + std::to_string(a));
        clip.duration = static_cast<float>(anim->mDuration / tps);
        clip.tracks.resize(bones);
        for (size_t b = 0; b < bones; ++b) clip.tracks[b].bone = static_cast<int>(b);

        const int frames = (std::max)(1, static_cast<int>(std::lround(anim->mDuration)));
        std::vector<XMMATRIX> clipGlobal(rig.names.size());
        std::vector<XMMATRIX> global(bones), local(bones);
        for (int f = 0; f <= frames; ++f) {
            const double ticks = anim->mDuration * f / frames;
            for (size_t node = 0; node < rig.names.size(); ++node) {
                const XMMATRIX m = ClipLocal(rig, static_cast<int>(node), ticks);
                clipGlobal[node] = rig.parent[node] < 0
                    ? m : m * clipGlobal[rig.parent[node]];
            }
            auto clipDirection = [&](const char* from, const char* to) {
                XMVECTOR head = XMVectorZero(), tail = XMVectorZero();
                for (size_t node = 0; node < rig.names.size(); ++node) {
                    if (rig.names[node] == from) head = clipGlobal[node].r[3];
                    if (rig.names[node] == to)   tail = clipGlobal[node].r[3];
                }
                return XMVector3Normalize(tail - head);
            };

            for (size_t b = 0; b < bones; ++b) {
                local[b] = XMLoadFloat4x4(&skel.localBind[b]);
                global[b] = skel.parent[b] < 0
                    ? local[b] : local[b] * global[skel.parent[b]];
            }
            for (const LimbSegment& segment : kLegSegments) {
                const int bone = skel.Find(segment.bone);
                const int child = skel.Find(segment.child);
                if (bone < 0 || child < 0) continue;
                const XMVECTOR have =
                    XMVector3Normalize(global[child].r[3] - global[bone].r[3]);
                const XMMATRIX swing = SwingBetween(
                    have, clipDirection(segment.bone, segment.child));
                const XMVECTOR origin = global[bone].r[3];
                XMMATRIX rotated = global[bone];
                rotated.r[3] = XMVectorSet(0, 0, 0, 1);
                rotated = rotated * swing;
                rotated.r[3] = origin;
                global[bone] = rotated;
                // Carry the rest of the limb with the bone that just moved.
                for (size_t k = 0; k < bones; ++k) {
                    if (skel.parent[k] < 0 || static_cast<int>(k) == bone) continue;
                    bool descends = false;
                    for (int p = static_cast<int>(k); p >= 0; p = skel.parent[p])
                        if (p == bone) { descends = true; break; }
                    if (descends) global[k] = local[k] * global[skel.parent[k]];
                }
            }

            const float time = static_cast<float>(ticks / tps);
            for (size_t b = 0; b < bones; ++b) {
                const int parent = skel.parent[b];
                const XMMATRIX parentGlobal =
                    parent < 0 ? XMMatrixIdentity() : global[parent];
                XMVECTOR scale, rotation, translation;
                XMMatrixDecompose(&scale, &rotation, &translation,
                    global[b] * XMMatrixInverse(nullptr, parentGlobal));
                XMFLOAT4 value;
                XMStoreFloat4(&value, XMQuaternionNormalize(rotation));
                clip.tracks[b].rotations.push_back({ time, value });
            }
        }

        // Legs authored for another body land at another height. Drop the whole
        // skeleton so its lowest foot over the cycle rests on the bind floor.
        AnimationInstance probe;
        probe.Play(&clip);
        std::vector<XMFLOAT4X4> posed;
        float lowest = FLT_MAX;
        for (int s = 0; s <= frames * 2; ++s) {
            probe.time = clip.duration * s / (frames * 2);
            probe.ComputeGlobalMatrices(skel, posed);
            for (int foot : feet)
                lowest = (std::min)(lowest, posed[foot]._43);
        }
        const XMFLOAT4X4& pelvisBind = skel.localBind[pelvis];
        const float drop = floor - lowest;
        for (int f = 0; f <= frames; ++f)
            clip.tracks[pelvis].positions.push_back(
                { clip.duration * f / frames,
                  { pelvisBind._41, pelvisBind._42, pelvisBind._43 + drop } });

        clips.push_back(std::move(clip));
    }
}


// Append every AnimStack in `scene` to `clips`, resolving channels to skeleton
// bone ids by name. positionScale scales translation keys to match the baked
// mesh scale.
void AppendClips(const aiScene* scene, const Skeleton& skel, float positionScale,
                 std::vector<AnimationClip>& clips) {
    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation* anim = scene->mAnimations[a];
        const double tps = anim->mTicksPerSecond != 0.0 ? anim->mTicksPerSecond : 30.0;
        AnimationClip clip;
        clip.name = anim->mName.length ? anim->mName.C_Str() : ("anim" + std::to_string(a));
        clip.duration = (float)(anim->mDuration / tps);
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim* ch = anim->mChannels[c];
            const int bone = skel.Find(ch->mNodeName.C_Str());
            if (bone < 0) {
                AppendRetargetedSplitRotationTrack(
                    scene, ch, skel, tps, clip);
                continue;
            }
            BoneTrack track;
            track.bone = bone;
            for (unsigned k = 0; k < ch->mNumPositionKeys; ++k)
                track.positions.push_back({ (float)(ch->mPositionKeys[k].mTime / tps),
                                            KeyVec(ch->mPositionKeys[k].mValue, positionScale) });
            for (unsigned k = 0; k < ch->mNumRotationKeys; ++k) {
                const aiQuaternion& q = ch->mRotationKeys[k].mValue;
                track.rotations.push_back({ (float)(ch->mRotationKeys[k].mTime / tps),
                                            XMFLOAT4(q.x, q.y, q.z, q.w) });
            }
            for (unsigned k = 0; k < ch->mNumScalingKeys; ++k)
                track.scales.push_back({ (float)(ch->mScalingKeys[k].mTime / tps),
                                         KeyVec(ch->mScalingKeys[k].mValue) });
            clip.tracks.push_back(std::move(track));
        }
        if (!clip.tracks.empty()) clips.push_back(std::move(clip));
    }
}

constexpr unsigned kImportFlags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
    aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights |
    // Blender FBX UVs use bottom-left image origin. Engine texture uploads and
    // D3D sampling use top-left, so flip V once during import.
    aiProcess_FlipUVs;

} // namespace

SkinnedModel SkinnedFBXImporter::Load(const std::string& meshPath,
    const std::vector<std::string>& animPaths,
    Microsoft::WRL::ComPtr<ID3D12Device> device,
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
    float uniformScale,
    bool useCookedClips,
    const std::vector<std::string>& rotationOnlyAnims) {

    SkinnedModel out;
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, 4);
    const aiScene* scene = importer.ReadFile(meshPath, kImportFlags);
    if (!scene || !scene->HasMeshes() || !scene->mRootNode) {
        std::cerr << "Skinned FBX load failed: " << importer.GetErrorString() << "\n";
        return out;
    }

    BuildSkeleton(scene->mRootNode, -1, out.skeleton);
    XMStoreFloat4x4(&out.skeleton.globalInverse,
        XMMatrixInverse(nullptr, XMLoadFloat4x4(&out.skeleton.localBind[0])));

    auto root = std::make_shared<SceneNode>("SkinnedRoot");
    root->mesh = std::make_shared<SceneMesh>();
    const fs::path base = fs::path(meshPath).parent_path();
    // Textures are searched for under this root, so it has to be the directory
    // that actually holds them. A variant of a character lives in a subfolder
    // beside its own animations and shares the parent's texture set rather
    // than duplicating it -- these maps run to hundreds of megabytes -- so a
    // folder with no Textures/ of its own defers to its parent, the same way
    // the "fbx" wrapper directory some exports come in does.
    const auto hasTextures = [](const fs::path& dir) {
        std::error_code ec;
        return fs::exists(dir / "Textures", ec);
    };
    fs::path textureRoot = base.filename() == "fbx" ? base.parent_path() : base;
    if (!hasTextures(textureRoot) && hasTextures(textureRoot.parent_path()))
        textureRoot = textureRoot.parent_path();

    auto lowerStr = [](std::string s) {
        for (char& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    int headBoneId = -1;
    for (size_t bone = 0; bone < out.skeleton.names.size(); ++bone) {
        const std::string name = lowerStr(out.skeleton.names[bone]);
        if (name.find("head") != std::string::npos &&
            name.find("end") == std::string::npos) {
            headBoneId = static_cast<int>(bone);
            break;
        }
    }

    // Resolve Blender/FBX material texture paths. Exported FBX files often keep
    // the author's absolute path, so fall back to the matching filename beside
    // the model. Load each authored map once; replacing it later can destroy a
    // resource still referenced by this load command list.
    auto loadReferencedTexture =
        [&](const aiString& texturePath,
            std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploads)
        -> Microsoft::WRL::ComPtr<ID3D12Resource> {
        std::string raw = texturePath.C_Str();
        std::replace(raw.begin(), raw.end(), '\\', '/');
        const fs::path authored(raw);
        if (authored.is_absolute() && fs::exists(authored))
            return GLBImporter::LoadTextureFromFile(
                authored.string(), device, commandList, uploads);

        const fs::path relative = (base / authored).lexically_normal();
        if (fs::exists(relative))
            return GLBImporter::LoadTextureFromFile(
                relative.string(), device, commandList, uploads);

        const std::string wanted = lowerStr(authored.filename().string());
        for (const auto& entry : fs::recursive_directory_iterator(textureRoot)) {
            if (entry.is_regular_file() &&
                lowerStr(entry.path().filename().string()) == wanted)
                return GLBImporter::LoadTextureFromFile(
                    entry.path().string(), device, commandList, uploads);
        }
        return nullptr;
    };

    // Load a texture by a bare filename stem, searching the model tree
    // case-insensitively for "<stem>.<ext>" (e.g. "T_Bandit_2_BaseColor").
    // Used as a per-material fallback because SK_Bandit.FBX only embeds the
    // texture path for material 1; the rest live on disk as T_Bandit_N_*.PNG.
    auto loadByStem = [&](const std::string& stem,
                          std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploads)
        -> Microsoft::WRL::ComPtr<ID3D12Resource> {
        const std::string want = lowerStr(stem);
        for (const auto& e : fs::recursive_directory_iterator(textureRoot)) {
            if (!e.is_regular_file()) continue;
            if (lowerStr(e.path().stem().string()) == want)
                return GLBImporter::LoadTextureFromFile(e.path().string(), device, commandList, uploads);
        }
        return nullptr;
    };

    std::vector<MeshPrimitive> sourcePrimitives;
    sourcePrimitives.reserve(scene->mNumMeshes);
    out.materialKeepAlive.reserve(scene->mNumMeshes);
    std::vector<std::shared_ptr<SceneMaterial>> materialCache(
        scene->mNumMaterials);
    size_t headMeshesFound = 0;
    size_t headTrianglesExamined = 0;
    size_t correctedHeadTriangles = 0;

    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* src = scene->mMeshes[mi];
        if (!src->HasPositions()) continue;
        MeshPrimitive p;
        std::shared_ptr<SceneMaterial> mat;
        const bool cachedMaterial = src->mMaterialIndex < materialCache.size() &&
            materialCache[src->mMaterialIndex] != nullptr;
        if (cachedMaterial) {
            mat = materialCache[src->mMaterialIndex];
        } else {
            mat = std::make_shared<SceneMaterial>();
            if (src->mMaterialIndex < scene->mNumMaterials) {
            const aiMaterial* am = scene->mMaterials[src->mMaterialIndex];
            aiString materialName;
            if (am->Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS)
                mat->name = materialName.C_Str();
            aiString texture;
            if ((am->GetTexture(aiTextureType_BASE_COLOR, 0, &texture) == AI_SUCCESS &&
                 texture.length) ||
                (am->GetTexture(aiTextureType_DIFFUSE, 0, &texture) == AI_SUCCESS &&
                 texture.length))
                mat->baseColorTexture =
                    loadReferencedTexture(texture, mat->uploadHeaps);
            if (am->GetTexture(aiTextureType_NORMALS, 0, &texture) == AI_SUCCESS &&
                texture.length)
                mat->normalTexture =
                    loadReferencedTexture(texture, mat->uploadHeaps);
            }
            // FBX parts are body, hair, and eyelashes. Mesh-index fallback used to
            // assign outfit sets 2 and 3 to hair cards, producing black/material
            // garbage. Resolve card materials by FBX material name instead.
            const std::string materialLower = lowerStr(mat->name);
            const bool hairCard = materialLower.find("hair") != std::string::npos ||
                                  materialLower.find("eyelash") != std::string::npos;
            std::string idx = "1";
            const size_t bandit = materialLower.find("bandit_");
            if (bandit != std::string::npos) {
                const size_t digit = bandit + 7;
                if (digit < materialLower.size() && std::isdigit((unsigned char)materialLower[digit]))
                    idx.assign(1, materialLower[digit]);
            }
            auto loadPart = [&](const char* suffix) {
                auto tex =
                    loadByStem("T_Bandit_" + idx + suffix, mat->uploadHeaps);
                if (!tex)
                    tex = loadByStem(
                        "T_Bandit_" + idx + "_1" + suffix, mat->uploadHeaps);
                return tex;
            };
            if (hairCard) {
                if (!mat->baseColorTexture)
                    mat->baseColorTexture =
                        loadByStem("T_Bandit_Hair_BaseColor", mat->uploadHeaps);
                const bool eyelashes = materialLower.find("eyelash") != std::string::npos;
                mat->baseColorFactor = eyelashes
                    ? XMFLOAT4(0.018f, 0.012f, 0.008f, 1.0f)
                    : XMFLOAT4(0.025f, 0.018f, 0.012f, 1.0f);
                mat->metallicFactor = 0.0f;
                mat->roughnessFactor = 0.88f;
                mat->doubleSided = true;
                mat->alphaCutout = true;
                mat->alphaFromLuminance = true;
                mat->ambientScale = 1.45f;
                mat->viewFillStrength = 0.08f;
            } else {
                // Honor Blender-authored FBX maps. Only infer a filename when
                // exporter omitted that slot (normally the packed ORM map).
                if (!mat->baseColorTexture)
                    mat->baseColorTexture = loadPart("_BaseColor");
                if (!mat->normalTexture)
                    mat->normalTexture = loadPart("_Normal");
                mat->metallicRoughnessTexture = loadPart("_ORM");
                mat->roughnessOnlyTexture = false;
                mat->baseColorFactor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
                mat->ambientScale = 1.15f;
                mat->occlusionStrength = 0.25f;
                // Outfit ORM maps are essentially non-metallic (~0 B) and
                // rough (~0.85 G). Keep defensive factors conservative and
                // use viewFillStrength as the skinned-character material tag.
                mat->viewFillStrength = 0.30f;
                mat->metallicFactor = 0.25f;
                mat->roughnessFactor = 1.10f;
            }
            if (src->mMaterialIndex < materialCache.size())
                materialCache[src->mMaterialIndex] = mat;
        }
        p.material = mat;
        p.materialIndex = (int)src->mMaterialIndex;
        out.materialKeepAlive.push_back(mat);

        // This FBX's separate hair-card meshes do not follow the converted head
        // bind pose. Both scalp and eyelash cards cut through the face as opaque
        // strips. The opaque head underneath already carries the authored cap,
        // mask, brows, and eye texture, so omit only these broken card batches.
        if (mat && (lowerStr(mat->name).find("hair") != std::string::npos ||
                    lowerStr(mat->name).find("eyelash") != std::string::npos))
            continue;

        // Geometry (12-float interleaved, scaled). Skin data is a parallel array.
        p.skin.assign(src->mNumVertices, SkinVertex{});
        for (unsigned v = 0; v < src->mNumVertices; ++v) {
            const aiVector3D n = src->HasNormals() ? src->mNormals[v] : aiVector3D(0, 1, 0);
            const aiVector3D uv = src->HasTextureCoords(0) ? src->mTextureCoords[0][v] : aiVector3D();
            const aiVector3D t = src->HasTangentsAndBitangents() ? src->mTangents[v] : aiVector3D(1, 0, 0);
            float handedness = 1.0f;
            if (src->HasTangentsAndBitangents()) {
                const aiVector3D& b = src->mBitangents[v];
                const aiVector3D cross(
                    n.y * t.z - n.z * t.y,
                    n.z * t.x - n.x * t.z,
                    n.x * t.y - n.y * t.x);
                handedness = (cross.x * b.x + cross.y * b.y + cross.z * b.z) < 0.0f
                    ? -1.0f : 1.0f;
            }
            p.vertices.insert(p.vertices.end(), {
                // Keep vertices in native (UE cm) space so they stay consistent
                // with the skeleton's offset/global matrices during GPU skinning.
                // The 0.01 metre scale is applied once on the world matrix.
                src->mVertices[v].x, src->mVertices[v].y, src->mVertices[v].z,
                n.x, n.y, n.z, uv.x, uv.y, t.x, t.y, t.z, handedness });
        }
        for (unsigned f = 0; f < src->mNumFaces; ++f)
            for (unsigned i = 0; i < src->mFaces[f].mNumIndices; ++i)
                p.indices.push_back(src->mFaces[f].mIndices[i]);
        if (p.indices.empty()) continue;

        // Skin weights: for each bone cluster, record its offset matrix in the
        // skeleton and accumulate up to 4 (index,weight) per affected vertex.
        std::vector<int> writeCount(src->mNumVertices, 0);
        for (unsigned b = 0; b < src->mNumBones; ++b) {
            const aiBone* bone = src->mBones[b];
            const int boneId = out.skeleton.Find(bone->mName.C_Str());
            if (boneId < 0) continue;
            out.skeleton.offset[boneId] = ToXM(bone->mOffsetMatrix);
            for (unsigned w = 0; w < bone->mNumWeights; ++w) {
                const aiVertexWeight& vw = bone->mWeights[w];
                if (vw.mVertexId >= src->mNumVertices) continue;
                int& c = writeCount[vw.mVertexId];
                if (c < 4) {
                    p.skin[vw.mVertexId].boneIndex[c] = (uint32_t)boneId;
                    p.skin[vw.mVertexId].boneWeight[c] = vw.mWeight;
                    ++c;
                }
            }
        }
        // Normalize weights; a vertex with no bone falls back to the root bone.
        for (unsigned v = 0; v < src->mNumVertices; ++v) {
            SkinVertex& s = p.skin[v];
            float sum = s.boneWeight[0] + s.boneWeight[1] + s.boneWeight[2] + s.boneWeight[3];
            if (sum > 1e-6f) { for (float& w : s.boneWeight) w /= sum; }
            else { s.boneIndex[0] = 0; s.boneWeight[0] = 1.0f; }
        }

        // Assimp collapses this FBX's named parts into shared material meshes,
        // so identify the face region by skin influence rather than mesh name.
        // The head bone's converted skin transform reverses handedness at draw
        // time, even though bind-pose winding agrees with bind-pose normals.
        // Pre-flip only head-driven triangles so raster culling sees their
        // animated outer surface. Body indices and global culling stay intact.
        bool meshContainsHead = false;
        if (headBoneId >= 0) {
            auto headWeight = [&](unsigned vertex) {
                float weight = 0.0f;
                for (int influence = 0; influence < 4; ++influence)
                    if (p.skin[vertex].boneIndex[influence] ==
                        static_cast<uint32_t>(headBoneId))
                        weight += p.skin[vertex].boneWeight[influence];
                return weight;
            };
            for (size_t triangle = 0; triangle + 2 < p.indices.size(); triangle += 3) {
                const unsigned i0 = p.indices[triangle + 0];
                const unsigned i1 = p.indices[triangle + 1];
                const unsigned i2 = p.indices[triangle + 2];
                if (i0 >= src->mNumVertices || i1 >= src->mNumVertices ||
                    i2 >= src->mNumVertices)
                    continue;
                const float influence =
                    (headWeight(i0) + headWeight(i1) + headWeight(i2)) / 3.0f;
                if (influence < 0.5f) continue;

                meshContainsHead = true;
                ++headTrianglesExamined;
                std::swap(p.indices[triangle + 1], p.indices[triangle + 2]);
                ++correctedHeadTriangles;
            }
        }
        if (meshContainsHead) ++headMeshesFound;

        sourcePrimitives.push_back(std::move(p));
    }

    std::cout << "Bandit head winding: meshes=" << headMeshesFound
              << " triangles=" << headTrianglesExamined
              << " corrected=" << correctedHeadTriangles << '\n';
    std::ofstream("bandit_winding.log", std::ios::trunc)
        << "meshes=" << headMeshesFound
        << " triangles=" << headTrianglesExamined
        << " corrected=" << correctedHeadTriangles << '\n';

    // Assimp commonly exposes one tiny aiMesh per authored FBX section even when
    // dozens of sections use the same material. Drawing those sections separately
    // produced ~72 mesh dispatches and ~40 shadow draws per bandit. Concatenate
    // compatible sections before creating GPU buffers; skin indices remain valid
    // because the skin array is parallel to the appended vertex stream.
    std::vector<MeshPrimitive> mergedPrimitives;
    std::unordered_map<int, size_t> bucketByMaterial;
    mergedPrimitives.reserve(sourcePrimitives.size());
    for (MeshPrimitive& source : sourcePrimitives) {
        auto [it, inserted] = bucketByMaterial.emplace(
            source.materialIndex, mergedPrimitives.size());
        if (inserted) {
            MeshPrimitive merged;
            merged.materialIndex = source.materialIndex;
            merged.material = source.material;
            mergedPrimitives.push_back(std::move(merged));
        }

        MeshPrimitive& merged = mergedPrimitives[it->second];
        const UINT baseVertex = static_cast<UINT>(merged.vertices.size() / 12);
        merged.vertices.insert(merged.vertices.end(),
            source.vertices.begin(), source.vertices.end());
        merged.skin.insert(merged.skin.end(), source.skin.begin(), source.skin.end());
        merged.indices.reserve(merged.indices.size() + source.indices.size());
        for (UINT index : source.indices) merged.indices.push_back(baseVertex + index);
    }

    for (MeshPrimitive& p : mergedPrimitives) {
        if (!GLBImporter::BuildMeshletData(p, device.Get())) continue;

        // Upload one parallel skin stream per merged material primitive.
        const UINT skinBytes = static_cast<UINT>(p.skin.size() * sizeof(SkinVertex));
        if (CreateStaticBufferDX12(device.Get(), p.skin.data(), skinBytes,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, p.skinBuffer,
                "SkinWeights"))
            p.skinVertexCount = static_cast<UINT>(p.skin.size());
        root->mesh->primitives.push_back(std::move(p));
    }

    // Clips keep native-space translation keys (matching the unscaled skeleton);
    // the 0.01 world scale is applied at draw. Prefer compressed clips from the
    // mesh's cooked blob, while retaining source parsing for stale/missing cooks.
    // Base clips stay first, followed by each extra animation-only FBX.
    // useCookedClips=false parses every clip from the source FBX instead.
    if (useCookedClips && CookedAssetLoader::LoadAnimationsForSource(
            meshPath, out.skeleton, out.clips)) {
        std::cout << "Loaded " << out.clips.size()
                  << " cooked embedded animation(s): "
                  << fs::path(meshPath).stem().string() << "\n";
    } else {
        AppendClips(scene, out.skeleton, 1.0f, out.clips);
    }
    for (const std::string& ap : animPaths) {
        const size_t before = out.clips.size();
        // Name the clip from its filename so callers can FindClip("Walk") etc.
        const std::string stem = fs::path(ap).stem().string();
        const bool rotationOnly =
            std::find(rotationOnlyAnims.begin(), rotationOnlyAnims.end(), stem) !=
            rotationOnlyAnims.end();
        // A cooked blob stores the keys as they were parsed, foreign bone
        // lengths included, so a rotation-only clip has to come from source.
        if (useCookedClips && !rotationOnly &&
            CookedAssetLoader::LoadAnimationsForSource(
                ap, out.skeleton, out.clips)) {
            for (size_t i = before; i < out.clips.size(); ++i)
                out.clips[i].name = stem;
            std::cout << "Loaded cooked animation: " << stem << "\n";
            continue;
        }
        Assimp::Importer animImporter;
        const aiScene* as = animImporter.ReadFile(ap, aiProcess_Triangulate);
        if (!as || as->mNumAnimations == 0) {
            std::cerr << "Anim FBX load failed: " << ap << " : " << animImporter.GetErrorString() << "\n";
            continue;
        }
        if (rotationOnly) {
            AppendRebasedClips(as, out.skeleton, out.clips);
            out.rebasedClips = true;
        } else {
            AppendClips(as, out.skeleton, 1.0f, out.clips);
        }
        for (size_t i = before; i < out.clips.size(); ++i) out.clips[i].name = stem;
    }

    root->UpdateGlobalTransform(root->localTransform);
    out.node = root;
    out.valid = !root->mesh->primitives.empty();
    std::cout << "Loaded skinned model: " << out.skeleton.BoneCount() << " bones, "
              << sourcePrimitives.size() << " source parts -> "
              << root->mesh->primitives.size() << " material batches, "
              << out.clips.size() << " clips\n";
    return out;
}
