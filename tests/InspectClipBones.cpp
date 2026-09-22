// Throwaway diagnostic: print the animation channels in a clip FBX and check
// them against the bone names of a skinned mesh FBX.
//
// SkinnedFBXImporter resolves each channel by exact name (Skeleton::Find) and
// drops any AnimStack that resolves no tracks at all. A downloaded clip that
// looks fine in a viewer can still import as nothing here if its channels are
// named even slightly differently from the mesh's bones -- and the failure is
// silent, so there is no way to tell that case apart from a missing file.
//
// Usage: InspectClipBones <mesh.fbx> <clip.fbx> [clip.fbx ...]

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cstdio>
#include <set>
#include <string>

// Mirrors how the importer collects bone names: the skin's bones, plus the
// node hierarchy, since a clip may animate nodes that carry no skin weights.
static void CollectNodes(const aiNode* node, std::set<std::string>& out) {
    if (!node) return;
    out.insert(node->mName.C_Str());
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        CollectNodes(node->mChildren[i], out);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: InspectClipBones <mesh.fbx> <clip.fbx> [...]\n");
        return 2;
    }

    Assimp::Importer meshImporter;
    const aiScene* mesh = meshImporter.ReadFile(argv[1], 0);
    if (!mesh) {
        std::printf("mesh failed: %s\n", meshImporter.GetErrorString());
        return 1;
    }

    std::set<std::string> boneNames;
    for (unsigned m = 0; m < mesh->mNumMeshes; ++m)
        for (unsigned b = 0; b < mesh->mMeshes[m]->mNumBones; ++b)
            boneNames.insert(mesh->mMeshes[m]->mBones[b]->mName.C_Str());
    std::set<std::string> nodeNames;
    CollectNodes(mesh->mRootNode, nodeNames);

    std::printf("MESH %s\n  skin bones=%zu  nodes=%zu\n",
                argv[1], boneNames.size(), nodeNames.size());
    int shown = 0;
    for (const std::string& n : boneNames) {
        if (shown++ >= 5) break;
        std::printf("    bone: '%s'\n", n.c_str());
    }

    for (int i = 2; i < argc; ++i) {
        Assimp::Importer clipImporter;
        const aiScene* clip = clipImporter.ReadFile(argv[i], 0);
        if (!clip) {
            std::printf("\nCLIP %s\n  FAILED: %s\n", argv[i],
                        clipImporter.GetErrorString());
            continue;
        }
        std::printf("\nCLIP %s\n  anims=%u\n", argv[i], clip->mNumAnimations);
        for (unsigned a = 0; a < clip->mNumAnimations; ++a) {
            const aiAnimation* an = clip->mAnimations[a];
            unsigned matchedBone = 0, matchedNode = 0;
            for (unsigned c = 0; c < an->mNumChannels; ++c) {
                const std::string name = an->mChannels[c]->mNodeName.C_Str();
                if (boneNames.count(name)) ++matchedBone;
                if (nodeNames.count(name)) ++matchedNode;
            }
            std::printf("  anim[%u] '%s' dur=%.2f tps=%.1f channels=%u\n"
                        "    match: skin-bone=%u  mesh-node=%u\n",
                        a, an->mName.C_Str(), an->mDuration,
                        an->mTicksPerSecond, an->mNumChannels,
                        matchedBone, matchedNode);
            unsigned printed = 0;
            for (unsigned c = 0; c < an->mNumChannels && printed < 6; ++c) {
                const std::string name = an->mChannels[c]->mNodeName.C_Str();
                std::printf("      ch '%s' %s\n", name.c_str(),
                            nodeNames.count(name) ? "OK" : "<-- NO MATCH");
                ++printed;
            }
        }
    }
    return 0;
}
