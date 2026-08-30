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

    // Live High-path sliders, applied on top of the authored spectrum above.
    // They live here rather than only in Scene so the CPU buoyancy query and
    // the GPU surface scale by the same numbers: the renderer folds these into
    // the uploaded wave constants, and EvaluateHeightAndSlope applies them
    // directly. A slider that moved only one of the two would float boats above
    // or below the water being drawn.
    float heightScale = 1.0f;
    float lengthScale = 1.0f;
    float speedScale = 1.0f;

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

    // Linear directional waves, as the High path has always used: each
    // component sweeps across the world on its authored bearing. Bathymetric
    // refraction and shoaling live in the Ultra path, which has real bed data
    // to drive them; approximating either here made the surface worse, not
    // better. The slider scales are applied so buoyancy tracks the drawn
    // surface exactly.
    float EvaluateHeightAndSlope(float x, float z, float time,
                                 float& dhdx, float& dhdz) const {
        constexpr float gravity = 9.81f;
        dhdx = 0.0f;
        dhdz = 0.0f;
        float height = 0.0f;
        const float safeHeight = (std::max)(0.0f, heightScale);
        const float safeLength = (std::max)(0.05f, lengthScale);
        const float scaledTime = time * speedScale;
        for (const OceanWave& wave : waves) {
            const float dirLength = std::sqrt(
                wave.direction.x * wave.direction.x +
                wave.direction.y * wave.direction.y);
            const float invLength = dirLength > 1e-5f ? 1.0f / dirLength : 0.0f;
            const float dx = wave.direction.x * invLength;
            const float dz = wave.direction.y * invLength;
            const float wavelength =
                (std::max)(wave.wavelength * safeLength, 0.1f);
            const float amplitude = wave.amplitude * safeHeight;
            const float k = DirectX::XM_2PI / wavelength;
            const float omega = std::sqrt(gravity * k);
            const float phase = k * (dx * x + dz * z) - omega * scaledTime;
            const float sine = std::sin(phase);
            const float cosine = std::cos(phase);
            height += amplitude * sine;
            const float derivative = amplitude * k * cosine;
            dhdx += derivative * dx;
            dhdz += derivative * dz;
        }
        return height;
    }
};
