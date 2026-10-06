#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

inline DirectX::XMFLOAT3 FoliageTextureMean(
        const std::vector<unsigned char>& pixels, float alphaCutoff) {
    std::array<float, 256> linear{};
    for (size_t i = 0; i < linear.size(); ++i)
        linear[i] = std::pow(static_cast<float>(i) / 255.0f, 2.2f);
    double sum[3] = {}, weight = 0.0;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const float alpha = static_cast<float>(pixels[i + 3]) / 255.0f;
        if (alpha < alphaCutoff || alpha <= 0.0f) continue;
        for (size_t channel = 0; channel < 3; ++channel)
            sum[channel] += linear[pixels[i + channel]] * alpha;
        weight += alpha;
    }
    return weight > 0.0 ? DirectX::XMFLOAT3(
        static_cast<float>(sum[0] / weight),
        static_cast<float>(sum[1] / weight),
        static_cast<float>(sum[2] / weight)) : DirectX::XMFLOAT3(1, 1, 1);
}

inline DirectX::XMFLOAT4 FoliageColorMatchingGrass(
        const DirectX::XMFLOAT4& authored,
        const DirectX::XMFLOAT3& textureMean,
        const DirectX::XMFLOAT3& grass) {
    // Remove the texture's average colour before applying the ground hue.
    // Keep each species' authored brightness and its leaf/flower detail.
    const float originalLuma = 0.2126f * textureMean.x * authored.x +
        0.7152f * textureMean.y * authored.y +
        0.0722f * textureMean.z * authored.z;
    const float grassLuma = 0.2126f * grass.x + 0.7152f * grass.y +
        0.0722f * grass.z;
    const float scale = originalLuma / (std::max)(grassLuma, 1e-4f);
    return DirectX::XMFLOAT4(
        grass.x * scale / (std::max)(textureMean.x, 1e-4f),
        grass.y * scale / (std::max)(textureMean.y, 1e-4f),
        grass.z * scale / (std::max)(textureMean.z, 1e-4f), authored.w);
}
