#pragma once

#include <DirectXMath.h>
#include <array>
#include <algorithm>
#include <cmath>

struct OceanWave {
    DirectX::XMFLOAT2 direction = { 1.0f, 0.0f };
    float amplitude = 0.1f;
    float wavelength = 10.0f;
    float steepness = 0.2f;
};

struct OceanWaveSettings {
    static constexpr size_t WaveCount = 4;
    std::array<OceanWave, WaveCount> waves = {};
    DirectX::XMFLOAT3 absorption = { 0.45f, 0.16f, 0.09f };
    DirectX::XMFLOAT3 shallowScatter = { 0.015f, 0.14f, 0.16f };
    DirectX::XMFLOAT3 deepScatter = { 0.004f, 0.052f, 0.10f };
    float microNormalStrength = 0.055f;
    float foamDepth = 0.72f;
    float foamCrest = 0.18f;

    static OceanWaveSettings CalmTropical() {
        OceanWaveSettings result;
        result.waves = {{
            {{ 0.940f,  0.342f }, 0.18f, 22.0f, 0.24f},
            {{-0.423f,  0.906f }, 0.10f, 11.0f, 0.20f},
            {{ 0.259f, -0.966f }, 0.045f, 4.5f, 0.16f},
            {{-0.788f, -0.616f }, 0.018f, 1.8f, 0.10f}
        }};
        return result;
    }

    float EvaluateHeightAndSlope(float x, float z, float time,
                                 float& dhdx, float& dhdz) const {
        constexpr float gravity = 9.81f;
        dhdx = 0.0f;
        dhdz = 0.0f;
        float height = 0.0f;
        for (const OceanWave& wave : waves) {
            const float dirLength = std::sqrt(
                wave.direction.x * wave.direction.x +
                wave.direction.y * wave.direction.y);
            const float invLength = dirLength > 1e-5f ? 1.0f / dirLength : 0.0f;
            const float dx = wave.direction.x * invLength;
            const float dz = wave.direction.y * invLength;
            const float k =
                DirectX::XM_2PI / (std::max)(wave.wavelength, 0.1f);
            const float omega = std::sqrt(gravity * k);
            const float phase = k * (dx * x + dz * z) - omega * time;
            const float sine = std::sin(phase);
            const float cosine = std::cos(phase);
            height += wave.amplitude * sine;
            const float derivative = wave.amplitude * k * cosine;
            dhdx += derivative * dx;
            dhdz += derivative * dz;
        }
        return height;
    }
};
