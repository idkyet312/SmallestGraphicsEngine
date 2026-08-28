#pragma once
// The one definition of how a LevelEntity's authored Euler angles become a
// rotation matrix.
//
// This has to agree with ImGuizmo exactly. The editor round-trips through it on
// every frame of a gizmo drag -- build a matrix from the stored angles, hand it
// to ImGuizmo::Manipulate, decompose back to angles with
// ImGuizmo::DecomposeMatrixToComponents, store -- and any disagreement makes
// that round trip lossy. ImGuizmo composes three separate axis rotations in the
// order X, then Y, then Z (see RecomposeMatrixFromComponents: rot[0] * rot[1] *
// rot[2] over the unary axes), so this does the same.
//
// It previously used XMMatrixRotationRollPitchYaw, which applies Z, then X,
// then Y. Measured consequence of that mismatch: an authored (20, 35, 10) came
// back as (26.0, 31.1, 11.0) after a single frame, and six frames of a drag bled
// 35 degrees of yaw down to 4.5 while pitch climbed from 20 to 39 -- the object
// rotating on its own while the mouse was held still.
//
// Both the editor and the runtime prefab batches use this, so a rotated prefab
// renders identically in the viewport and in the game.
#include "LevelDefinition.h"
#include <DirectXMath.h>

inline DirectX::XMMATRIX EulerDegreesToMatrix(const float rotationDegrees[3]) {
    return DirectX::XMMatrixRotationX(
               DirectX::XMConvertToRadians(rotationDegrees[0])) *
           DirectX::XMMatrixRotationY(
               DirectX::XMConvertToRadians(rotationDegrees[1])) *
           DirectX::XMMatrixRotationZ(
               DirectX::XMConvertToRadians(rotationDegrees[2]));
}

// Full world matrix for an authored transform: scale, then rotate, then
// translate.
inline DirectX::XMMATRIX EntityWorldMatrix(const Transform& t) {
    return DirectX::XMMatrixScaling(t.scale[0], t.scale[1], t.scale[2]) *
           EulerDegreesToMatrix(t.rotation) *
           DirectX::XMMatrixTranslation(
               t.position[0], t.position[1], t.position[2]);
}
