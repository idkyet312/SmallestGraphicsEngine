#pragma once

#include <DirectXMath.h>

// Shared per-frame palm wind state. Keep field order aligned with the HLSL
// constants in palm_wind.hlsli and visibility-buffer FrameConstants.
struct PalmWindFrameDX12 {
    DirectX::XMFLOAT4 wind{};              // current time, previous time, strength, speed
    DirectX::XMFLOAT4 primary{};           // current position.xyz, enabled
    DirectX::XMFLOAT4 secondary{};
    DirectX::XMFLOAT4 previousPrimary{};   // previous position.xyz, enabled
    DirectX::XMFLOAT4 previousSecondary{};
    DirectX::XMFLOAT4 params{};            // radius, rotor strength, model height, unused
};
