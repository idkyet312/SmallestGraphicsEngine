#pragma once
// Loads a skinned FBX (skeletal mesh + skeleton) plus a set of animation-only
// FBX clips that share the same skeleton (matched by bone name). Unlike
// FBXImporter, this does NOT pre-transform vertices -- it keeps the bone
// hierarchy and per-vertex weights needed for GPU skinning.
#include "SceneGraph.h"
#include "SkinnedTypes.h"
#include <memory>
#include <string>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

struct SkinnedModel {
    std::shared_ptr<SceneNode>  node;      // renderable mesh (primitives carry skin[])
    // Importers may collapse many source meshes into one primitive per material.
    // Keep every source material alive because its upload heaps can still be
    // referenced by the load command list when the merged primitives replace it.
    std::vector<std::shared_ptr<SceneMaterial>> materialKeepAlive;
    Skeleton                    skeleton;
    std::vector<AnimationClip>  clips;      // includes the clip baked into the mesh FBX
    RagdollSpec                 ragdoll;
    bool                        valid = false;
    // Whether any clip here was rebased off a foreign rig. Such a clip only
    // has its legs rebuilt and keeps bind pose above the hips, so consumers
    // that blend directional clips have to suppress their torso; a model whose
    // clips were all authored on its own skeleton carries a real one.
    bool                        rebasedClips = false;
    bool                        authoredDirectional = false;
    float                       rootPitch = -DirectX::XM_PIDIV2;
    float                       groundOffset = 0.16f;

    const AnimationClip* FindClip(const std::string& name) const {
        for (const auto& c : clips)
            if (c.name.find(name) != std::string::npos) return &c;
        return nullptr;
    }
};

class SkinnedFBXImporter {
public:
    // Read a reference rig for CPU-side physics fitting without uploading its mesh.
    static Skeleton LoadSkeleton(const std::string& path);
    // meshPath: the SK_*.FBX skinned mesh. animPaths: extra clip FBX files whose
    // AnimStacks are appended to the model's clip list. uniformScale bakes into
    // the mesh vertices AND the skeleton bind transforms so both stay consistent.
    //
    // useCookedClips=false parses all clips from the source FBX and ignores the
    // cooked blobs. The cooker's compression drops bone tracks (the player's
    // rifle idle came back with 4 of 66), which leaves most of the skeleton in
    // bind pose and visibly misaligns the mesh. Player assets pass false.
    //
    // rotationOnlyAnims: file stems (as animPaths name their clips) whose clips
    // are rebased onto this skeleton rather than copied onto it. A clip
    // converted from another rig can share these bone names while keeping its
    // own rest pose, bone axes and bone lengths, so its keys mean something
    // else here: copied straight across they resize the skeleton inside the
    // mesh and bend the limbs the wrong way. Rebasing carries the motion --
    // each bone's rotation relative to its own rest -- and leaves translation
    // and scale to this skeleton, so the proportions stay the mesh's.
    static SkinnedModel Load(const std::string& meshPath,
                             const std::vector<std::string>& animPaths,
                             Microsoft::WRL::ComPtr<ID3D12Device> device,
                             Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
                             float uniformScale = 0.01f,
                             bool useCookedClips = true,
                             const std::vector<std::string>& rotationOnlyAnims = {});
};
