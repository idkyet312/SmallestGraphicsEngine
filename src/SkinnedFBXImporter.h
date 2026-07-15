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
    Skeleton                    skeleton;
    std::vector<AnimationClip>  clips;      // includes the clip baked into the mesh FBX
    bool                        valid = false;

    const AnimationClip* FindClip(const std::string& name) const {
        for (const auto& c : clips)
            if (c.name.find(name) != std::string::npos) return &c;
        return nullptr;
    }
};

class SkinnedFBXImporter {
public:
    // meshPath: the SK_*.FBX skinned mesh. animPaths: extra clip FBX files whose
    // AnimStacks are appended to the model's clip list. uniformScale bakes into
    // the mesh vertices AND the skeleton bind transforms so both stay consistent.
    static SkinnedModel Load(const std::string& meshPath,
                             const std::vector<std::string>& animPaths,
                             Microsoft::WRL::ComPtr<ID3D12Device> device,
                             Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
                             float uniformScale = 0.01f);
};
