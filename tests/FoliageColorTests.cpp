#include "FoliageColor.h"
#include <iostream>

namespace {
int failures = 0;
void Check(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
float Luma(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}
}

int main() {
    // Background colour must not influence a leaf's colour grade.
    const std::vector<unsigned char> cutout = {
        128, 128, 128, 255, 255, 0, 255, 0, 0, 255, 0, 20 };
    const auto mean = FoliageTextureMean(cutout, 0.20f);
    Check(std::abs(mean.x - 0.2195197f) < 1e-6f &&
          mean.x == mean.y && mean.y == mean.z,
          "Visible grey leaf must decode to linear grey despite coloured background");
    const auto fallback = FoliageTextureMean({255, 255, 255, 0}, 0.20f);
    Check(fallback.x == 1 && fallback.y == 1 && fallback.z == 1,
          "An empty cutout uses a finite neutral mean");

    const DirectX::XMFLOAT3 source(0.08f, 0.21f, 0.035f);
    const DirectX::XMFLOAT4 authored(0.08f, 0.09f, 0.07f, 0.65f);
    const DirectX::XMFLOAT3 ground(0.16f, 0.12f, 0.05f);
    const auto grade = FoliageColorMatchingGrass(authored, source, ground);
    const float r = grade.x * source.x, g = grade.y * source.y,
        b = grade.z * source.z;
    Check(std::abs(r / g - ground.x / ground.y) < 1e-5f &&
          std::abs(b / g - ground.z / ground.y) < 1e-5f,
          "Green leaf texture must take the ground hue after grading");
    Check(std::abs(Luma(r, g, b) - Luma(source.x * authored.x,
        source.y * authored.y, source.z * authored.z)) < 1e-6f,
          "Matching must preserve each species' authored brightness");
    Check(grade.w == authored.w, "Matching must preserve authored cutout opacity");
    const auto black = FoliageColorMatchingGrass(authored, source, {0, 0, 0});
    Check(black.x == 0 && black.y == 0 && black.z == 0,
          "A black ground tint must produce finite black foliage");
    if (!failures) std::cout << "FoliageColorTests passed\n";
    return failures ? 1 : 0;
}
