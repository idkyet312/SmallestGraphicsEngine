#include "SkinnedFBXImporter.h"
#include "GLBImporter.h"
#include "StaticBufferDX12.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
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
            if (bone < 0) continue;
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
    float uniformScale) {

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
    const fs::path textureRoot =
        base.filename() == "fbx" ? base.parent_path() : base;

    auto lowerStr = [](std::string s) {
        for (char& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };

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
                mat->ambientScale = 1.35f;
                mat->occlusionStrength = 0.25f;
                mat->viewFillStrength = 0.65f;
                mat->metallicFactor = 1.0f;
                mat->roughnessFactor = 1.0f;
            }
            if (src->mMaterialIndex < materialCache.size())
                materialCache[src->mMaterialIndex] = mat;
        }
        p.material = mat;
        p.materialIndex = (int)src->mMaterialIndex;
        out.materialKeepAlive.push_back(mat);

        // Authored eyelash cards are displaced through the face by this FBX's
        // skeleton conversion and render as opaque black strips across the eyes.
        // Keep scalp hair cards, but omit this separate cosmetic batch.
        if (mat && lowerStr(mat->name).find("eyelash") != std::string::npos)
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

        sourcePrimitives.push_back(std::move(p));
    }

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
    // the 0.01 world scale is applied at draw. Clip baked into the mesh FBX
    // first, then each extra animation-only FBX.
    AppendClips(scene, out.skeleton, 1.0f, out.clips);
    for (const std::string& ap : animPaths) {
        Assimp::Importer animImporter;
        const aiScene* as = animImporter.ReadFile(ap, aiProcess_Triangulate);
        if (!as || as->mNumAnimations == 0) {
            std::cerr << "Anim FBX load failed: " << ap << " : " << animImporter.GetErrorString() << "\n";
            continue;
        }
        // Name the clip from its filename so callers can FindClip("Walk") etc.
        const std::string stem = fs::path(ap).stem().string();
        const size_t before = out.clips.size();
        AppendClips(as, out.skeleton, 1.0f, out.clips);
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
