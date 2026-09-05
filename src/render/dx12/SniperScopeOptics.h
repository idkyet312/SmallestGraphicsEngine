#pragma once
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace SniperScopeOptics {
inline constexpr float Magnification = 6.0f;
inline constexpr unsigned Resolution = 1024;

inline float FieldOfViewDegrees(float apertureHalfTangent) {
    return DirectX::XMConvertToDegrees(2.0f * std::atan(
        (std::max)(apertureHalfTangent, 1e-4f) / Magnification));
}

inline DirectX::XMFLOAT2 ProjectTangent(DirectX::FXMVECTOR position) {
    const float z = (std::max)(DirectX::XMVectorGetZ(position), 1e-4f);
    return { DirectX::XMVectorGetX(position) / z,
             DirectX::XMVectorGetY(position) / z };
}

inline DirectX::XMMATRIX AlignWeapon(
    DirectX::FXMMATRIX weapon, DirectX::CXMMATRIX view,
    const DirectX::XMFLOAT3& lensCenter, const DirectX::XMFLOAT2& aimTangent,
    float blend) {
    using namespace DirectX;
    const XMVECTOR lens = XMVector3TransformCoord(XMLoadFloat3(&lensCenter), weapon * view);
    const float amount = std::clamp(blend, 0.0f, 1.0f);
    const XMVECTOR offset = XMVectorSet(
        (aimTangent.x * XMVectorGetZ(lens) - XMVectorGetX(lens)) * amount,
        (aimTangent.y * XMVectorGetZ(lens) - XMVectorGetY(lens)) * amount, 0, 0);
    XMMATRIX result = weapon;
    result.r[3] += XMVectorSetW(XMVector3TransformNormal(
        offset, XMMatrixInverse(nullptr, view)), 0.0f);
    return result;
}
}
