// Throwaway diagnostic: dump the raw structure of the FPS arms FBX.
//
// The arms load (arms_load.log confirms two textured primitives) but do not
// appear on screen, and the normalised bounds come out suspiciously symmetric
// for a two-arm asset. That is the signature of a bind pose where the mesh sits
// at the origin rather than posed forward, so this prints the per-mesh bounds
// straight from Assimp -- before any of ArmsModel's reorientation -- to show
// what the asset actually contains.

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cfloat>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1]
        : "Content/Models/fps_arms/SK_FPSHands_Male_01b_ThickerForeArms.FBX";

    // PreTransformVertices bakes the bind pose and THROWS AWAY the skeleton and
    // animations, so a posed/animated FBX inspected with it looks identical to
    // the base mesh. Pass "raw" to keep bones and AnimStacks visible.
    const bool raw = argc > 2 && std::string(argv[2]) == "raw";

    Assimp::Importer importer;
    unsigned flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
        aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
        aiProcess_ImproveCacheLocality | aiProcess_FlipWindingOrder;
    if (!raw) flags |= aiProcess_PreTransformVertices;
    const aiScene* scene = importer.ReadFile(path, flags);

    if (!scene) {
        std::printf("FAILED: %s\n", importer.GetErrorString());
        return 1;
    }

    std::printf("meshes=%u materials=%u animations=%u textures=%u%s\n",
                scene->mNumMeshes, scene->mNumMaterials, scene->mNumAnimations,
                scene->mNumTextures,
                raw ? "  [raw: skeleton preserved]" : "  [bind pose baked]");

    // Embedded textures live inside the FBX and are referenced as "*0", "*1".
    for (unsigned t = 0; t < scene->mNumTextures; ++t) {
        const aiTexture* tex = scene->mTextures[t];
        std::printf("embedded[%u] '%s' %ux%u fmt='%s' bytes=%u\n", t,
                    tex->mFilename.C_Str(), tex->mWidth, tex->mHeight,
                    tex->achFormatHint, tex->mHeight == 0 ? tex->mWidth : 0);
    }

    // What each material asks for, per texture slot.
    for (unsigned m = 0; m < scene->mNumMaterials; ++m) {
        const aiMaterial* mat = scene->mMaterials[m];
        aiString name;
        mat->Get(AI_MATKEY_NAME, name);
        std::printf("material[%u] '%s'\n", m, name.C_Str());
        const std::pair<aiTextureType, const char*> slots[] = {
            { aiTextureType_DIFFUSE, "DIFFUSE" },
            { aiTextureType_BASE_COLOR, "BASE_COLOR" },
            { aiTextureType_NORMALS, "NORMALS" },
            { aiTextureType_HEIGHT, "HEIGHT" },
            { aiTextureType_SPECULAR, "SPECULAR" },
            { aiTextureType_METALNESS, "METALNESS" },
            { aiTextureType_DIFFUSE_ROUGHNESS, "ROUGHNESS" },
            { aiTextureType_OPACITY, "OPACITY" },
        };
        for (const auto& slot : slots) {
            const unsigned count = mat->GetTextureCount(slot.first);
            for (unsigned i = 0; i < count; ++i) {
                aiString path;
                mat->GetTexture(slot.first, i, &path);
                std::printf("    %-12s -> '%s'\n", slot.second, path.C_Str());
            }
        }
    }

    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation* clip = scene->mAnimations[a];
        std::printf("anim[%u] '%s' duration=%.2f ticks @ %.2f tps, channels=%u\n",
                    a, clip->mName.C_Str(), clip->mDuration,
                    clip->mTicksPerSecond, clip->mNumChannels);
        // The channel names must match the bone names the mesh is skinned to,
        // or the clip drives nothing and every vertex keeps its bind transform.
        for (unsigned c = 0; c < clip->mNumChannels && c < 8; ++c) {
            std::printf("    channel[%u] '%s' pos=%u rot=%u scale=%u\n", c,
                        clip->mChannels[c]->mNodeName.C_Str(),
                        clip->mChannels[c]->mNumPositionKeys,
                        clip->mChannels[c]->mNumRotationKeys,
                        clip->mChannels[c]->mNumScalingKeys);
            if (clip->mChannels[c]->mNumPositionKeys > 0) {
                const aiVector3D& first =
                    clip->mChannels[c]->mPositionKeys[0].mValue;
                const aiVector3D& last = clip->mChannels[c]->mPositionKeys[
                    clip->mChannels[c]->mNumPositionKeys - 1].mValue;
                std::printf("        position first=(%.3f, %.3f, %.3f)"
                            " last=(%.3f, %.3f, %.3f)\n",
                            first.x, first.y, first.z,
                            last.x, last.y, last.z);
            }
        }
    }

    if (raw && scene->mNumMeshes > 0 && scene->mNumAnimations > 0) {
        // The clip only poses a bone if a channel NAMES that bone. Any weighted
        // bone without a channel keeps its bind transform while its neighbours
        // move, which tears the mesh -- so list exactly which weighted bones the
        // animation actually reaches.
        const aiMesh* mesh = scene->mMeshes[0];
        const aiAnimation* clip = scene->mAnimations[0];
        std::printf("\n-- weighted bones vs animation coverage --\n");
        unsigned weighted = 0, covered = 0;
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            if (bone->mNumWeights == 0) continue;
            ++weighted;
            // Assimp splits an FBX node's transform into synthetic
            // "<bone>_$AssimpFbx$_Rotation/Translation/Scaling" nodes. Those
            // still animate the bone, so a bare name comparison under-reports
            // coverage badly -- count any channel whose name starts with the
            // bone name as driving it.
            const std::string boneName = bone->mName.C_Str();
            bool hasChannel = false;
            for (unsigned c = 0; c < clip->mNumChannels; ++c) {
                const std::string channelName = clip->mChannels[c]->mNodeName.C_Str();
                if (channelName == boneName ||
                    (channelName.size() > boneName.size() &&
                     channelName.compare(0, boneName.size(), boneName) == 0 &&
                     channelName.compare(boneName.size(), 11, "_$AssimpFbx") == 0)) {
                    hasChannel = true;
                    break;
                }
            }
            if (hasChannel) ++covered;
            else std::printf("    NO ANIMATION: '%s' (%u weights)\n",
                             bone->mName.C_Str(), bone->mNumWeights);
        }
        std::printf("  weighted bones=%u, animated=%u, MISSING=%u\n",
                    weighted, covered, weighted - covered);
    }

    float glo[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float ghi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
            const aiVector3D& p = mesh->mVertices[v];
            const float c[3] = { p.x, p.y, p.z };
            for (int i = 0; i < 3; ++i) {
                if (c[i] < lo[i]) lo[i] = c[i];
                if (c[i] > hi[i]) hi[i] = c[i];
                if (c[i] < glo[i]) glo[i] = c[i];
                if (c[i] > ghi[i]) ghi[i] = c[i];
            }
        }

        aiString materialName;
        if (mesh->mMaterialIndex < scene->mNumMaterials)
            scene->mMaterials[mesh->mMaterialIndex]->Get(AI_MATKEY_NAME, materialName);

        // A mesh with zero bones carries no skin weights, so nothing can pose it
        // -- it will render frozen in whatever space it was authored in, no
        // matter what the skeleton does.
        unsigned weightedBones = 0;
        for (unsigned b = 0; b < mesh->mNumBones; ++b)
            if (mesh->mBones[b]->mNumWeights > 0) ++weightedBones;
        std::printf("mesh[%u] name='%s' mat='%s' verts=%u tris=%u bones=%u weightedBones=%u%s\n",
                    m, mesh->mName.C_Str(), materialName.C_Str(),
                    mesh->mNumVertices, mesh->mNumFaces, mesh->mNumBones,
                    weightedBones,
                    mesh->mNumBones == 0 ? "  [STATIC - cannot animate]" : "");
        std::printf("    x[%.3f..%.3f] y[%.3f..%.3f] z[%.3f..%.3f]  (extent %.3f x %.3f x %.3f)\n",
                    lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                    hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
    }

    std::printf("\nCOMBINED x[%.3f..%.3f] y[%.3f..%.3f] z[%.3f..%.3f]\n",
                glo[0], ghi[0], glo[1], ghi[1], glo[2], ghi[2]);
    std::printf("extent %.3f x %.3f x %.3f\n",
                ghi[0] - glo[0], ghi[1] - glo[1], ghi[2] - glo[2]);

    // Winding check. The two hands are mirrored copies of each other, and
    // mirroring reverses triangle winding -- which makes one of them vanish
    // under backface culling while the other renders correctly. Compare each
    // mesh's signed volume: consistently wound closed meshes give the same sign,
    // so opposite signs prove one side is inverted.
    std::printf("\n-- winding (signed volume; opposite signs => one side mirrored) --\n");
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        double volume = 0.0;
        for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices != 3) continue;
            const aiVector3D& a = mesh->mVertices[face.mIndices[0]];
            const aiVector3D& b = mesh->mVertices[face.mIndices[1]];
            const aiVector3D& c = mesh->mVertices[face.mIndices[2]];
            volume += (double)a.x * ((double)b.y * c.z - (double)b.z * c.y)
                    - (double)a.y * ((double)b.x * c.z - (double)b.z * c.x)
                    + (double)a.z * ((double)b.x * c.y - (double)b.y * c.x);
        }
        aiString materialName;
        if (mesh->mMaterialIndex < scene->mNumMaterials)
            scene->mMaterials[mesh->mMaterialIndex]->Get(AI_MATKEY_NAME, materialName);
        std::printf("mesh[%u] mat='%s' signedVolume=%+.1f -> %s\n", m,
                    materialName.C_Str(), volume / 6.0,
                    volume >= 0.0 ? "CCW/outward" : "CW/inverted");
    }
    return 0;
}
