#pragma once

#include "SceneGraph.h"
#include <DirectXMath.h>
#include <memory>

struct PalmMeshCut {
    std::shared_ptr<SceneMesh> lower;
    std::shared_ptr<SceneMesh> upper;
};

// Runtime half-space boolean used by breakable palm trunks. Mesh vertices stay
// in the palm model's original local space so a result can be cut repeatedly.
class PalmMeshCutter {
public:
    static std::shared_ptr<SceneMesh> BuildWholeTrunk();

    // cutY is in PalmModel local space. impactDirectionXZ tilts the cut slightly
    // toward the incoming damage so breaks do not look machine-sawn.
    static PalmMeshCut Cut(const std::shared_ptr<SceneMesh>& source,
                           float cutY,
                           const DirectX::XMFLOAT2& impactDirectionXZ);

    static bool Upload(const std::shared_ptr<SceneMesh>& mesh);
};
